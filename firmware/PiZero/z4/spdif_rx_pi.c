// pico_spdif_rx's API (spdif_rx.h) on the Pi, for DSPi's spdif_input.c, which is compiled unchanged.
// The PCM block samples GPIO 20 at 125 MHz (pcm_capture_pi.c). Each chunk's interrupt decodes it
// (spdif_decode.c) and builds 384-subframe blocks into the library's FIFO, with the library's lock
// states, callbacks and block timing.

#include <string.h>

#include "spdif_rx.h"
#include "input_capture_arena.h"

#include "irq.h"
#include "pcm_capture_pi.h"
#include "spdif_decode.h"
#include "spdif_rx_pi.h"
#include "pmu.h"

#define SAMPLE_HZ PCM_CAPTURE_SAMPLE_HZ
#define STABLE_BLOCKS 4u            // aligned blocks at one rate before STABLE
#define DECODE_TIMEOUT_US 10000u    // no block for this long loses the signal
#define CAPTURE_CHUNK_SAMPLES 131072u   // pcm_capture_pi.c's chunk

#define fifo_buff (input_capture_arena.spdif_fifo)
_Static_assert(PCM_CAPTURE_TAIL >= SPDIF_RING_TAIL, "the capture's tail covers the decoder's look-ahead");
_Static_assert(sizeof(input_capture_arena.spdif_fifo) == SPDIF_RX_FIFO_SIZE * 4, "arena S/PDIF FIFO size");

static spdif_rx_config_t gcfg;
static volatile bool running;   // the capture is this receiver's
static uint64_t cap_end;        // samples handed over so far
static spdif_decoder_t dec;

static volatile spdif_rx_state_t state;
static void (*on_stable_func)(spdif_rx_samp_freq_t samp_freq);
static void (*on_lost_stable_func)(void);
static spdif_rx_samp_freq_t samp_freq;
static float samp_freq_actual;
static uint32_t stable_run;
static uint32_t parity_err_count;
static uint8_t c_bits[SPDIF_BLOCK_SIZE / 16];

// What every subframe touches, together so one base address reaches it all. The block being assembled
// starts at a B preamble: 384 subframes alternating B or M, then W. The FIFO's write and read pointers
// count modulo twice the FIFO, as pico_spdif_rx's do, so full and empty differ.
#define PTR_SPAN (SPDIF_RX_FIFO_SIZE * 2u)
static struct {
    uint32_t blk_pos, blk_parity;
    volatile uint32_t wr_ptr, rd_ptr;
    uint64_t blk_at;
    bool blk_ok;
    uint8_t blk_c[SPDIF_BLOCK_SIZE / 16];
} rx = {.blk_pos = SPDIF_BLOCK_SIZE};
static uint64_t prev_blk_at;
static uint32_t block_count;
static uint32_t last_block_us;

static inline uint32_t fifo_used(void) {
    return (rx.wr_ptr + PTR_SPAN - rx.rd_ptr) % PTR_SPAN;
}

static volatile uint32_t block_ref_seq, block_ref_count;
static volatile uint64_t block_ref_time_us;

static uint32_t decode_us;
static uint32_t dec_cycles, dec_ev[PMU_N_EVENTS];
static bool monitor;

static uint32_t micros(void) {
    return *(volatile uint32_t *)0x3F003004u;
}

static spdif_rx_samp_freq_t classify(float hz) {
    static const spdif_rx_samp_freq_t rates[] = {SAMP_FREQ_44100, SAMP_FREQ_48000, SAMP_FREQ_88200,
                                                 SAMP_FREQ_96000, SAMP_FREQ_176400, SAMP_FREQ_192000};
    for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        float r = (float)rates[i];
        if (hz > r * 0.98f && hz < r * 1.02f) {
            return rates[i];
        }
    }
    return SAMP_FREQ_NONE;
}

static void lose_signal(void) {
    bool was_stable = state == SPDIF_RX_STATE_STABLE;
    state = SPDIF_RX_STATE_NO_SIGNAL;
    stable_run = 0;
    rx.blk_pos = SPDIF_BLOCK_SIZE;
    prev_blk_at = 0;
    if (was_stable && on_lost_stable_func) {
        on_lost_stable_func();
    }
}

static __attribute__((noinline)) void block_done(void) {
    float hz = prev_blk_at ? (float)(SPDIF_BLOCK_SIZE / 2) * (float)SAMPLE_HZ / (float)(rx.blk_at - prev_blk_at) : 0.0f;
    prev_blk_at = rx.blk_at;
    spdif_rx_samp_freq_t sf = classify(hz);
    samp_freq_actual = hz;
    parity_err_count += rx.blk_parity;
    block_count++;
    last_block_us = (uint32_t)pcm_capture_irq_us();
    block_ref_seq++;
    __asm__ volatile("dmb" ::: "memory");
    block_ref_count = block_count;
    block_ref_time_us = pcm_capture_time_of(rx.blk_at);
    __asm__ volatile("dmb" ::: "memory");
    block_ref_seq++;
    if (!rx.blk_ok || sf == SAMP_FREQ_NONE || (state == SPDIF_RX_STATE_STABLE && sf != samp_freq)) {
        if (state == SPDIF_RX_STATE_STABLE) {
            lose_signal();
        }
        samp_freq = sf;
        stable_run = 0;
        return;
    }
    memcpy(c_bits, rx.blk_c, sizeof(c_bits));
    stable_run = sf == samp_freq ? stable_run + 1u : 1u;
    samp_freq = sf;
    if (state == SPDIF_RX_STATE_NO_SIGNAL) {
        state = SPDIF_RX_STATE_WAITING_STABLE;
    }
    if (state == SPDIF_RX_STATE_WAITING_STABLE && stable_run >= STABLE_BLOCKS) {
        state = SPDIF_RX_STATE_STABLE;
        if (on_stable_func) {
            on_stable_func(sf);
        }
    }
}

// A B preamble starts a block. Kept out of on_subframe so the other 383 subframes of a block run
// without a stack frame.
static __attribute__((noinline)) void block_start(uint64_t at) {
    if (rx.blk_pos != SPDIF_BLOCK_SIZE) {
        stable_run = 0;     // a B inside a block: the stream slipped
        if (state == SPDIF_RX_STATE_STABLE) {
            lose_signal();
        }
    }
    rx.blk_ok = true;
    rx.blk_at = at;
    rx.blk_parity = 0;
    memset(rx.blk_c, 0, sizeof(rx.blk_c));
}

static void on_subframe(uint32_t w, uint64_t at, void *ctx) {
    (void)ctx;
    uint32_t sync = w & 0xFu, pos = rx.blk_pos;
    if (sync == SPDIF_SYNC_B) {
        block_start(at);
        pos = 0;
    } else if (pos == SPDIF_BLOCK_SIZE) {
        return;     // waiting for the next block's B
    } else if ((pos & 1u) ? sync != SPDIF_SYNC_W : sync == SPDIF_SYNC_W) {
        rx.blk_ok = false;     // an M where a W belongs, or the reverse
    }
    uint32_t v = w & 0xFFFFFFF0u;     // even parity over time slots 4 to 31
    v ^= v >> 16;
    v ^= v >> 8;
    v ^= v >> 4;
    rx.blk_parity += (0x6996u >> (v & 0xFu)) & 1u;
    if ((w & 0x40000000u) && !(pos & 1u)) {     // the C bit, from the frame's first subframe
        rx.blk_c[pos / 16u] |= (uint8_t)(1u << ((pos / 2u) % 8u));
    }
    // A reader that has fallen a whole FIFO behind loses its oldest pair. Both pointers stay below
    // PTR_SPAN, so each wrap is one subtraction.
    uint32_t wr = rx.wr_ptr, rd = rx.rd_ptr;
    if ((wr >= rd ? wr - rd : wr + PTR_SPAN - rd) >= SPDIF_RX_FIFO_SIZE) {
        rd += 2u;
        rx.rd_ptr = rd >= PTR_SPAN ? rd - PTR_SPAN : rd;
    }
    fifo_buff[wr >= SPDIF_RX_FIFO_SIZE ? wr - SPDIF_RX_FIFO_SIZE : wr] = w;
    rx.wr_ptr = wr + 1u == PTR_SPAN ? 0u : wr + 1u;
    rx.blk_pos = ++pos;
    if (pos == SPDIF_BLOCK_SIZE) {
        block_done();
    }
}

// Each capture chunk, in its interrupt.
static void on_capture(const uint32_t *ring, uint32_t ring_words, uint64_t end) {
    uint32_t irq_us = (uint32_t)pcm_capture_irq_us();
    cap_end = end;
    // A decoder that fell most of a ring behind gives up the old samples rather than read overwritten ones.
    if (end - spdif_decode_needs(&dec, end) > (uint64_t)ring_words * 32u - 2u * CAPTURE_CHUNK_SAMPLES) {
        spdif_decode_skip(&dec, end - CAPTURE_CHUNK_SAMPLES);
    }
    pmu_snap_t a, b;
    pmu_read(&a);
    spdif_decode_ring(&dec, ring, ring_words, end, on_subframe, NULL);
    pmu_read(&b);
    dec_cycles += b.cycles - a.cycles;
    for (uint32_t i = 0; i < PMU_N_EVENTS; i++) {
        dec_ev[i] += b.ev[i] - a.ev[i];
    }
    decode_us += micros() - irq_us;
    if (state != SPDIF_RX_STATE_NO_SIGNAL && irq_us - last_block_us > DECODE_TIMEOUT_US) {
        lose_signal();
    }
}

void spdif_rx_start(const spdif_rx_config_t *config) {
    if (monitor) {
        spdif_rx_pi_monitor(false);     // DSPi's input takes the receiver from the bench
    }
    uint32_t save = irq_global_disable();
    gcfg = *config;
    state = SPDIF_RX_STATE_NO_SIGNAL;
    samp_freq = SAMP_FREQ_NONE;
    samp_freq_actual = 0.0f;
    stable_run = 0;
    parity_err_count = 0;
    rx.blk_pos = SPDIF_BLOCK_SIZE;
    prev_blk_at = 0;
    rx.wr_ptr = rx.rd_ptr = 0;
    block_count = 0;
    last_block_us = micros();
    memset(c_bits, 0, sizeof(c_bits));
    spdif_decode_reset(&dec);
    cap_end = 0;
    running = true;
    irq_global_restore(save);
    pcm_capture_start(on_capture);
}

void spdif_rx_end(void) {
    uint32_t save = irq_global_disable();
    pcm_capture_stop(on_capture);
    running = false;
    state = SPDIF_RX_STATE_NO_SIGNAL;
    irq_global_restore(save);
}

void spdif_rx_set_callback_on_stable(void (*func)(spdif_rx_samp_freq_t samp_freq)) {
    on_stable_func = func;
}

void spdif_rx_set_callback_on_lost_stable(void (*func)()) {
    on_lost_stable_func = func;
}

spdif_rx_state_t spdif_rx_get_state(void) {
    return state;
}

float spdif_rx_get_samp_freq_actual(void) {
    return samp_freq_actual;
}

void spdif_rx_get_block_ref(uint32_t *block_cnt, uint64_t *time_us) {
    uint32_t s1, s2;
    do {
        s1 = block_ref_seq;
        __asm__ volatile("dmb" ::: "memory");
        *block_cnt = block_ref_count;
        *time_us = block_ref_time_us;
        __asm__ volatile("dmb" ::: "memory");
        s2 = block_ref_seq;
    } while ((s1 & 1u) || s1 != s2);
}

spdif_rx_samp_freq_t spdif_rx_get_samp_freq(void) {
    return samp_freq;
}

void spdif_rx_get_c_bits(void *ptr, size_t size, uint32_t offset) {
    uint32_t save = irq_global_disable();
    for (size_t i = 0; i < size && offset + i < sizeof(c_bits); i++) {
        ((uint8_t *)ptr)[i] = c_bits[offset + i];
    }
    irq_global_restore(save);
}

uint32_t spdif_rx_get_parity_err_count(void) {
    return parity_err_count;
}

// Every word in the FIFO is decoded and final, so the live count and the block count are the same.
uint32_t spdif_rx_get_fifo_count(void) {
    return fifo_used();
}

uint32_t spdif_rx_get_fifo_count_live(void) {
    return state == SPDIF_RX_STATE_STABLE ? fifo_used() : 0u;
}

static uint32_t read_fifo(uint32_t **buff, uint32_t req_count, bool live) {
    uint32_t save = irq_global_disable();
    uint32_t avail = (live ? spdif_rx_get_fifo_count_live() : spdif_rx_get_fifo_count()) & ~1u;
    uint32_t n = req_count < avail ? req_count : avail;
    uint32_t at = rx.rd_ptr % SPDIF_RX_FIFO_SIZE;
    if (at + n > SPDIF_RX_FIFO_SIZE) {
        n = SPDIF_RX_FIFO_SIZE - at;
    }
    *buff = &fifo_buff[at];
    rx.rd_ptr = (rx.rd_ptr + n) % PTR_SPAN;
    irq_global_restore(save);
    return n;
}

uint32_t spdif_rx_read_fifo_live(uint32_t **buff, uint32_t req_count) {
    return read_fifo(buff, req_count, true);
}

uint32_t spdif_rx_read_fifo(uint32_t **buff, uint32_t req_count) {
    return read_fifo(buff, req_count, false);
}

void spdif_rx_pi_status(spdif_rx_pi_status_t *out) {
    pcm_capture_status_t cs;
    pcm_capture_status(&cs);
    uint32_t save = irq_global_disable();
    *out = (spdif_rx_pi_status_t){
        .magic = SPDIF_RX_PI_MAGIC, .running = running, .state = state, .dma_channel = cs.dma_channel,
        .samples_lo = (uint32_t)cap_end, .samples_hi = (uint32_t)(cap_end >> 32), .subframes = dec.subframes,
        .decode_errors = dec.errors, .cell_q8 = dec.cell_q8, .blocks = block_count, .samp_freq = samp_freq,
        .samp_freq_actual_mhz = (uint32_t)(samp_freq_actual * 1000.0f), .parity_errors = parity_err_count,
        .fifo_used = fifo_used(), .irqs = cs.irqs, .decode_us = decode_us, .pcm_cs = cs.pcm_cs,
        .dma_debug = cs.dma_debug, .monitor = monitor,
    };
    out->inv_cycles = cs.inv_cycles;
    out->dec_cycles = dec_cycles;
    out->dec_inst = dec_ev[0];
    out->dec_l1d_refill = dec_ev[2];
    out->dec_l2d_refill = dec_ev[3];
    out->dec_br_mispred = dec_ev[4];
    for (uint32_t i = 0; i < 8u; i++) {
        out->last[i] = fifo_buff[(rx.wr_ptr + PTR_SPAN - 8u + i) % SPDIF_RX_FIFO_SIZE];
    }
    irq_global_restore(save);
}

bool spdif_rx_pi_monitor(bool on) {
    if (on == monitor) {
        return true;
    }
    if (on) {
        if (running || pcm_capture_running()) {
            return false;   // DSPi's S/PDIF or ADAT input is using the capture
        }
        input_arena_claim(INPUT_ARENA_SPDIF);
        spdif_rx_config_t cfg = {.data_pin = 20, .flags = SPDIF_RX_FLAGS_ALL};
        spdif_rx_start(&cfg);
    } else {
        spdif_rx_end();
        input_arena_release(INPUT_ARENA_SPDIF);
    }
    monitor = on;
    return true;
}
