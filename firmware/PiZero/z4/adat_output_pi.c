// DSPi's adat_output.h API on the Pi, in place of adat_output.c, which this file follows function for
// function. The one difference is what the ring holds. DSPi encodes each frame into 256 line levels as
// it is pushed. Here a ring frame is the 8 samples and the line level the frame starts from
// (smi_frame.h), and the SMI composer encodes it (bring-up section 29).

// Everything DSPi sees is as in adat_output.c. That covers the configuration and its dirty flag, the
// notify events, the status packet, the rate policy, and the lead, keep-alive, slot-skip and resync
// accounting. The PIO and DMA calls are kept, because the shim's composer starts and stops on pio1 SM 1
// and reports its progress in DMA channel 13's read address (pico_shim.c).

#include "adat_output.h"

#if PICO_RP2350

#include <string.h>
#include "config.h"
#include "smi_frame.h"
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"
#include "pico/audio_spdif.h"
#include "pico/audio_i2s_multi.h"
#include "usb_audio.h"
#include "audio_pipeline.h"
#include "audio_clock_div.h"
#include "notify.h"

#define ADAT_PIO                pio1   // PDM owns SM 0 on this block
#define ADAT_SM                 1
#define ADAT_DMA_DATA_CH        13     // 13/14 are unused in every config:
#define ADAT_DMA_CTRL_CH        14     // outputs 0-3, PDM 4, input 5-12
#define ADAT_FRAME_WORDS        8      // 256 bits
#define ADAT_RING_FRAMES        576    // 18 KB; must exceed cushion + slot ring fill cap
#define ADAT_ALIGN_LEAD_FRAMES  96     // constant ADAT-vs-slot lead; underrun cushion
// Largest lead the slot admission cap can produce; any larger value means
// the slot accounting is broken and must not be written into the ring.
#define ADAT_MAX_LEAD_FRAMES    (ADAT_ALIGN_LEAD_FRAMES + \
                                 PICO_AUDIO_SPDIF_RING_FRAMES - PICO_AUDIO_SPDIF_ERASE_AHEAD)

_Static_assert(ADAT_RING_FRAMES > ADAT_MAX_LEAD_FRAMES,
               "ADAT ring must hold the cushion plus a full slot ring");
_Static_assert(PICO_AUDIO_I2S_RING_FRAMES - PICO_AUDIO_I2S_ERASE_AHEAD <=
               PICO_AUDIO_SPDIF_RING_FRAMES - PICO_AUDIO_SPDIF_ERASE_AHEAD,
               "ADAT lead bound assumes the I2S slot fill cap is no larger");

// DSPi's program, one bit a cycle. Nothing runs it here: the shim only
// watches the state machine it is loaded for.
static const uint16_t adat_pio_instr[] = { 0x6001 };
static const struct pio_program adat_pio_program = {
    .instructions = adat_pio_instr, .length = 1, .origin = -1,
};

// A frame is half a cache line, and aligned so that none lies across two.
static uint32_t adat_ring[ADAT_RING_FRAMES * ADAT_FRAME_WORDS] __attribute__((aligned(64)));
// Read by the control DMA channel to re-point the data channel at wrap.
static uint32_t adat_ring_base_holder;

static uint32_t adat_line_level;   // line level after the last ring write

static uint32_t adat_wr_frame;
// Mutated only from Core 0 main-loop context (resync/stop paths).  The DSP
// pipeline's per-packet finalize_s24 snapshot (audio_pipeline.c) relies on
// this staying constant across one process_input_block() call; never flip it
// from an ISR or Core 1.
static bool     adat_running;
static bool     adat_hw_init_done;
static uint8_t  adat_hw_pin = 0xFF;       // GPIO currently routed to the PIO
static uint     adat_pio_offset;
static uint32_t adat_cur_freq = 48000;
static bool     adat_rate_ok = true;
// Silence written ahead of the slot timeline while slot 0 ran dry, not yet
// matched by the slot skip that re-anchors it.
static uint32_t adat_excess_frames;
static bool     adat_need_local_resync;

static volatile uint8_t adat_cfg_enabled;
static volatile uint8_t adat_cfg_pin = PICO_ADAT_PIN;
volatile bool adat_output_config_dirty;

static uint16_t adat_resync_count;
static uint16_t adat_slip_count;
static uint32_t adat_last_notified_state = 0xFFFFFFFF;


// ---------------------------------------------------------------------------
// Ring bookkeeping
// ---------------------------------------------------------------------------

static inline uint32_t adat_rd_frame(void) {
    uint32_t off = (uint32_t)(dma_hw->ch[ADAT_DMA_DATA_CH].read_addr
                              - (uintptr_t)adat_ring);
    // At the wrap instant read_addr can equal ring end for one control-channel
    // transfer; the modulo folds it to 0.
    return (off >> 5) % ADAT_RING_FRAMES;   // 32 bytes per frame
}

static inline uint32_t adat_lead_frames(void) {
    return (adat_wr_frame + ADAT_RING_FRAMES - adat_rd_frame()) % ADAT_RING_FRAMES;
}

// Silence frames carry an even number of ones, so they leave the line level
// unchanged and an all-silence ring replays seamlessly.
DSP_TIME_CRITICAL
static void adat_write_silence(uint32_t frames) {
    uint32_t wf = adat_wr_frame;
    if (frames > ADAT_RING_FRAMES) {
        // Only the last ring's worth can survive; skip straight to it.
        wf = (wf + (frames - ADAT_RING_FRAMES)) % ADAT_RING_FRAMES;
        frames = ADAT_RING_FRAMES;
    }
    while (frames) {
        uint32_t run = MIN(frames, ADAT_RING_FRAMES - wf);
        smi_adat_silence(&adat_ring[wf * ADAT_FRAME_WORDS], run, adat_line_level);
        frames -= run;
        wf = wf + run == ADAT_RING_FRAMES ? 0 : wf + run;
    }
    adat_wr_frame = wf;
}

// ---------------------------------------------------------------------------
// Hardware bring-up / teardown
// ---------------------------------------------------------------------------

// Nominal 16.8 clkdiv for PIO clock = 256*Fs: the shared base the S/PDIF TX
// library also runs (audio_clock_div.h), so ADAT consumes at the identical
// rate.  Computed at resync; adat_update_divider must stay flash-free.
static uint32_t adat_nom_div;

DSP_TIME_CRITICAL
static void adat_update_divider(void) {
    // Apply the nominal divider; pio_sm_init resets it, so this re-writes it
    // at every resync.
    uint32_t div = adat_nom_div;
    pio_sm_set_clkdiv_int_frac(ADAT_PIO, ADAT_SM,
                               (uint16_t)(div >> 8), (uint8_t)(div & 0xFF));
}

static void adat_hw_init_once(void) {
    if (adat_hw_init_done) return;
    pio_sm_claim(ADAT_PIO, ADAT_SM);
    adat_pio_offset = pio_add_program(ADAT_PIO, &adat_pio_program);
    dma_channel_claim(ADAT_DMA_DATA_CH);
    dma_channel_claim(ADAT_DMA_CTRL_CH);
    adat_ring_base_holder = (uint32_t)(uintptr_t)adat_ring;
    adat_hw_init_done = true;
}

static void adat_sm_configure(void) {
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, adat_pio_offset, adat_pio_offset);
    sm_config_set_out_pins(&c, adat_hw_pin, 1);
    sm_config_set_set_pins(&c, adat_hw_pin, 1);      // lets stop force the pin low
    sm_config_set_out_shift(&c, false, true, 32);    // shift left: MSB first
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    pio_sm_init(ADAT_PIO, ADAT_SM, adat_pio_offset, &c);
    adat_update_divider();                            // pio_sm_init resets the divider
}

static void adat_stop_hw(void) {
    if (!adat_hw_init_done) return;
    // Break the retrigger chain first: a data completion racing the aborts
    // would otherwise fire the control channel and re-arm the data channel.
    dma_channel_config dc = dma_get_channel_config(ADAT_DMA_DATA_CH);
    channel_config_set_chain_to(&dc, ADAT_DMA_DATA_CH);
    dma_channel_set_config(ADAT_DMA_DATA_CH, &dc, false);
    dma_channel_abort(ADAT_DMA_CTRL_CH);
    dma_channel_abort(ADAT_DMA_DATA_CH);
    pio_sm_set_enabled(ADAT_PIO, ADAT_SM, false);
    pio_sm_clear_fifos(ADAT_PIO, ADAT_SM);
    // Force only OUR pin low via the SM's SET mapping; pio_sm_set_pins()
    // would transiently drive every GPIO muxed to this PIO block (PDM).
    if (adat_hw_pin != 0xFF)
        pio_sm_exec(ADAT_PIO, ADAT_SM, pio_encode_set(pio_pins, 0));
    adat_running = false;
}

static void adat_release_pin(void) {
    if (adat_hw_pin == 0xFF) return;
    // Only release if still ours: another peripheral may have re-claimed the
    // pin between a deferred disable and this service (its mux is not PIO1).
    if (gpio_get_function(adat_hw_pin) == GPIO_FUNC_PIO1) {
        gpio_set_function(adat_hw_pin, GPIO_FUNC_NULL);
        gpio_set_dir(adat_hw_pin, GPIO_IN);
    }
    adat_hw_pin = 0xFF;
}

static void adat_arm_dma(void) {
    // Control channel: on data completion, rewrite the data channel's read
    // address (trigger alias) back to the ring base.  TRANS_COUNT reloads
    // from its shadow on every trigger, so this runs forever without an IRQ.
    dma_channel_config cc = dma_channel_get_default_config(ADAT_DMA_CTRL_CH);
    channel_config_set_transfer_data_size(&cc, DMA_SIZE_32);
    channel_config_set_read_increment(&cc, false);
    channel_config_set_write_increment(&cc, false);
    dma_channel_configure(ADAT_DMA_CTRL_CH, &cc,
                          &dma_hw->ch[ADAT_DMA_DATA_CH].al3_read_addr_trig,
                          &adat_ring_base_holder, 1, false);

    dma_channel_config dc = dma_channel_get_default_config(ADAT_DMA_DATA_CH);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_read_increment(&dc, true);
    channel_config_set_write_increment(&dc, false);
    channel_config_set_dreq(&dc, pio_get_dreq(ADAT_PIO, ADAT_SM, true));
    channel_config_set_chain_to(&dc, ADAT_DMA_CTRL_CH);
    channel_config_set_high_priority(&dc, true);
    dma_channel_configure(ADAT_DMA_DATA_CH, &dc, &ADAT_PIO->txf[ADAT_SM],
                          adat_ring, ADAT_RING_FRAMES * ADAT_FRAME_WORDS, true);
}

static void adat_notify_if_changed(void) {
    // Whole tuple, so enable/pin changes made while rate-suspended (active
    // unchanged) still produce an event.
    uint32_t state = ((uint32_t)adat_cfg_pin << 16)
                   | ((uint32_t)adat_cfg_enabled << 8)
                   | (adat_running ? 1u : 0u);
    if (state == adat_last_notified_state) return;
    adat_last_notified_state = state;
    notify_push_adat_state(adat_cfg_enabled, adat_running ? 1 : 0, adat_cfg_pin);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void adat_output_init(void) {
    // Nothing to build: the composer encodes with no tables.
}

void adat_output_set_config(bool enabled, uint8_t pin) {
    if (adat_cfg_enabled == (uint8_t)enabled && adat_cfg_pin == pin) return;
    adat_cfg_enabled = (uint8_t)enabled;
    adat_cfg_pin = pin;
    adat_output_config_dirty = true;
}

bool    adat_output_config_enabled(void) { return adat_cfg_enabled != 0; }
uint8_t adat_output_pin(void)            { return adat_cfg_pin; }
uint8_t adat_output_pi_pin(void)         { return adat_hw_pin; }    // pico_shim.c: the pin in use

// Sampled per packet by the DSP pipeline to pick the slot finalization mode
// (see output_s24.h); must stay RAM-resident like its caller.
DSP_TIME_CRITICAL
bool    adat_output_is_active(void)      { return adat_running; }

void adat_output_get_status(AdatStatus *out) {
    out->enabled      = adat_cfg_enabled;
    out->active       = adat_running ? 1 : 0;
    out->pin          = adat_cfg_pin;
    out->rate_ok      = adat_rate_ok ? 1 : 0;
    out->resync_count = adat_resync_count;
    out->slip_count   = adat_slip_count;
}

void adat_output_on_rate_change(uint32_t freq) {
    adat_cur_freq = freq;
    adat_rate_ok = (freq <= 48000);
    // Caller (perform_rate_change) holds the mute; the following
    // complete_pipeline_reset() restarts the stream via resync if valid.
    if (!adat_rate_ok && adat_running) adat_stop_hw();
}

void adat_output_stream_stop(void) {
    adat_stop_hw();
}

void adat_output_release_moving_pin(void) {
    if (adat_hw_pin == 0xFF) return;
    if (adat_cfg_enabled && adat_hw_pin == adat_cfg_pin) return;
    adat_stop_hw();
    adat_release_pin();
}

void adat_output_resync(void) {
    adat_stop_hw();
    adat_output_config_dirty = false;
    adat_need_local_resync = false;

    // Re-derive from the live rate: a boot preset can put the system at
    // 44.1 kHz without perform_rate_change() ever running.
    adat_cur_freq = audio_state.freq;
    adat_rate_ok = (adat_cur_freq <= 48000);

    if (!adat_cfg_enabled || !adat_rate_ok) {
        adat_release_pin();
        adat_notify_if_changed();
        return;
    }

    adat_hw_init_once();
    if (adat_hw_pin != adat_cfg_pin) {
        adat_release_pin();
        adat_hw_pin = adat_cfg_pin;
        pio_gpio_init(ADAT_PIO, adat_hw_pin);
        pio_sm_set_consecutive_pindirs(ADAT_PIO, ADAT_SM, adat_hw_pin, 1, true);
    }
    {
        adat_nom_div = audio_base_divider_16_8(adat_cur_freq);
    }
    adat_sm_configure();

    // Line starts low: force the pin and fill the whole ring with silence at
    // that level, so the anchor below only has to place the write pointer.
    pio_sm_exec(ADAT_PIO, ADAT_SM, pio_encode_set(pio_pins, 0));
    adat_line_level = 0;
    adat_wr_frame = 0;
    adat_write_silence(ADAT_RING_FRAMES);

    // Anchor against slot 0 as it stands at the SM start, within one frame
    // (ADAT is on another PIO block and cannot join the synchronized start).
    uint32_t save = save_and_disable_interrupts();
    int32_t lead = (int32_t)ADAT_ALIGN_LEAD_FRAMES + output_ring_fill_signed(0);
    adat_excess_frames = 0;
    if (lead < (int32_t)ADAT_ALIGN_LEAD_FRAMES) {
        adat_excess_frames = (uint32_t)((int32_t)ADAT_ALIGN_LEAD_FRAMES - lead);
        lead = ADAT_ALIGN_LEAD_FRAMES;
    } else if (lead > (int32_t)ADAT_MAX_LEAD_FRAMES) {
        // Slot 0 reports more fill than admission allows: never index past
        // the ring on a broken reading; count it so the bench can see it.
        lead = ADAT_MAX_LEAD_FRAMES;
        adat_slip_count++;
    }
    adat_wr_frame = (uint32_t)lead;
    adat_arm_dma();
    pio_sm_set_enabled(ADAT_PIO, ADAT_SM, true);
    restore_interrupts(save);
    adat_running = true;
    adat_resync_count++;
    adat_notify_if_changed();
}

DSP_TIME_CRITICAL
void adat_output_keepalive(void) {
    if (!adat_running) return;
    // Exact from slot 0's counters even after the reader overtook the writer,
    // where the ring position alone is ambiguous.
    int32_t lead = (int32_t)ADAT_ALIGN_LEAD_FRAMES + output_ring_fill_signed(0)
                 + (int32_t)adat_excess_frames;
    if (lead >= (int32_t)ADAT_ALIGN_LEAD_FRAMES) return;
    uint32_t n = (uint32_t)((int32_t)ADAT_ALIGN_LEAD_FRAMES - lead);
    adat_write_silence(n);
    adat_excess_frames += n;
}

DSP_TIME_CRITICAL
void adat_output_on_slot_skip(uint32_t frames) {
    if (!adat_running) return;
    // Post-skip lead, exact from slot 0's counters. Excess above the skip is
    // unreachable (excess only accrues while slot 0 is dry, and the skip that
    // ends a dry spell covers its whole deficit); a lead the ring cannot hold
    // means broken slot accounting. Re-anchor rather than lap the reader.
    int32_t lead = (int32_t)ADAT_ALIGN_LEAD_FRAMES + output_ring_fill_signed(0);
    if (adat_excess_frames > frames || lead > (int32_t)ADAT_MAX_LEAD_FRAMES) {
        adat_need_local_resync = true;
        return;
    }
    adat_write_silence(frames - adat_excess_frames);
    adat_excess_frames = 0;
}

void adat_output_task(void) {
    if (adat_need_local_resync) {
        adat_slip_count++;
        adat_output_resync();
        return;
    }
    adat_output_keepalive();
}

DSP_TIME_CRITICAL
void adat_output_push_block(const out_s24_t (*bufs)[192], uint32_t sample_count) {
    if (!adat_running) return;

    // Structurally unreachable while the blocking give caps the slot fill;
    // reaching it means the DMA stalled.  Skip and restart rather than
    // overwrite unplayed frames.
    if (adat_lead_frames() + sample_count >= ADAT_RING_FRAMES) {
        adat_need_local_resync = true;
        return;
    }

    // The rows of buf_out as ring frames, with the line level each starts from.
    uint32_t wf = adat_wr_frame;
    uint32_t lvl = adat_line_level;
    for (uint32_t i = 0; i < sample_count;) {
        uint32_t run = MIN(sample_count - i, ADAT_RING_FRAMES - wf);
        lvl = smi_adat_write(&adat_ring[wf * ADAT_FRAME_WORDS], &bufs[0][i], 192, run, lvl);
        i += run;
        wf = wf + run == ADAT_RING_FRAMES ? 0 : wf + run;
    }
    adat_line_level = lvl;
    adat_wr_frame = wf;
}

#endif // PICO_RP2350
