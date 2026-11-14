// Z4: DSPi on the UAC2 High Speed front end. Every millisecond the rate's worth of frames is taken from
// TinyUSB's FIFO and run through DSPi's process_input_block(); the output rings are drained by the system
// timer (pico_shim.c). Vendor requests go to DSPi's vendor_commands.c, notifications go out on EP 0x83,
// and z4_mainloop.c carries DSPi's main-loop work. Bench counters are read with vendor request 0x4F.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "board.h"
#include "mmu.h"
#include "irq.h"
#include "mbox.h"
#include "uart.h"

#include "tusb.h"
#include "device/usbd_pvt.h"

#include "broadcom/defines.h"
#include "broadcom/gen/vcmailbox.h"

#include "loader_proto.h"
#include "broadcom/caches.h"
#include "usb_descriptors.h"

#include "pico/audio_i2s_multi.h"
#include "pico/audio_spdif.h"
#include "hardware/sync.h"
#include "pico/time.h"

#include "config.h"
#include "audio_input.h"
#include "audio_pipeline.h"
#include "notify.h"
#include "output_latency.h"
#include "pdm_generator.h"
#include "siggen.h"
#include "spdif_input.h"
#include "dac_hw_mute.h"
#include "i2s_input.h"
#include "adat_input.h"
#include "usb_audio.h"

#include "z4.h"
#include "z4_globals.h"
#include "pmu.h"
#include "pcm_out.h"
#include "smi_out.h"
#include "pcm_rx.h"
#include "i2s_input_pi.h"
#include "adat_input_pi.h"
#include "spdif_rx_pi.h"
#include "cpu_meter.h"
#include "clk_dump.h"
#include "pll_out.h"
#include "device/dcd.h"
#include "z4_prof.h"

uint32_t tusb_time_millis_api(void) {
    return board_micros() / 1000u;
}

static void feedback_measure(void);

// USB event accounting: events the DWC2 driver posted against events TinyUSB queued. TinyUSB drops
// an event when its queue is full, and a lost SETUP or EP0 completion leaves control transfers stuck.
// Kinds are 0 SETUP, 1 transfer complete, 2 other bus events, 3 SOF and deferred calls.
#define UH_SETUPS 8u
typedef struct {
    uint32_t magic, posted[4], queued[4];
    uint32_t gap_max_us, gap_max_rate, gap_max_alt, gaps_over_4ms, tud_calls;
    // Hang watch: a snapshot at the moment control transfers were found stuck, before reconnecting.
    uint32_t usb_irqs, ep0_done, recoveries, trigger, trigger_ms, setups_since_done;
    uint32_t armed;         // trigger 3 is armed by a 0x45 read with wValue 1, and fires once
    uint32_t snap[32];      // see usb_watch_snapshot()
    struct uh_setup {
        uint32_t ms, done;
        uint8_t pkt[8];
    } setup[UH_SETUPS];     // the last SETUPs before the trigger, oldest first, done if EP0 completed after
    uint32_t wait_timeouts;     // DWC2 endpoint-disable waits that gave up (dcd_dwc2.c patch 3)
} usb_health_t;
static usb_health_t uh = {.magic = 0x48425355u};   // "USBH"

static unsigned uh_kind(uint32_t id) {
    return id == DCD_EVENT_SETUP_RECEIVED ? 0u : id == DCD_EVENT_XFER_COMPLETE ? 1u
           : (id == DCD_EVENT_SOF || id == USBD_EVENT_FUNC_CALL) ? 3u : 2u;
}

static void usb_irq(void) {
    uh.usb_irqs++;
    tud_int_handler(0);
    feedback_measure();
    // Read back from the controller so its interrupt-clear writes have landed before we return.
    // Otherwise the line is still high and the IRQ fires again with nothing pending.
    (void)*(volatile uint32_t *)(USB_OTG_GLOBAL_BASE + 0x014);    // GINTSTS
}

// usb_sound_card_init() in DSPi's usb_audio.c calls tud_init(), whose tud_rhport_init() is renamed to
// this for that file only. USB is brought up by main() below instead.
bool dspi_z4_tud_rhport_init(uint8_t rhport, const tusb_rhport_init_t *rh_init) {
    (void)rhport;
    (void)rh_init;
    return true;
}

// ---------------------------------------------------------------------------------------------------
// State and counters

static const uint32_t sample_rates[] = {44100, 48000, 96000};
#define N_SAMPLE_RATES (sizeof(sample_rates) / sizeof(sample_rates[0]))

static volatile uint32_t current_rate = 48000;
static volatile uint8_t current_alt;
static int8_t mute_master;
static int16_t volume_master;   // 1/256 dB

typedef struct {
    uint32_t class_requests;
    uint32_t set_alt_count;
    uint32_t rate_sets;
    uint32_t reserved0;
    uint32_t fb_sent;
    uint32_t reserved1;
    uint32_t out_packets;
    uint32_t out_bytes;
    uint32_t consumed_bytes;
    uint32_t underruns;     // consumer ticks that found less than a tick's worth in the FIFO
    uint32_t fifo_level;    // bytes in the FIFO at the last consumer tick
    uint32_t suspends;
    uint32_t resumes;
    uint32_t mounts;
    uint32_t irqs;
    uint32_t irq_spurious;
    // Diagnostics for the feedback endpoint (EP 2 IN) and the data endpoint (EP 1 OUT).
    uint32_t fb_diepctl;
    uint32_t fb_dieptsiz;
    uint32_t fb_diepint;
    uint32_t data_doepctl;
    uint32_t gintmsk;
    uint32_t dsts;
    uint32_t fb_busy;
    int32_t fb_parity;
    int32_t data_parity;
} u2_stats_t;

// DSPi side, appended after u2_stats_t in the 0x4F reply (version 2).
#define Z4_PEAK_SLOTS 18
_Static_assert(NUM_CHANNELS <= Z4_PEAK_SLOTS, "peaks[] holds every DSPi channel");

typedef struct {
    uint8_t cpu0_load;              // global_status.cpu0_load: whole-core load from cpu_meter.c, percent
    uint8_t cpu1_load;              // always 0, Core 1 is never started
    uint8_t core1_mode;
    uint8_t n_channels;             // valid entries in peaks[] (NUM_CHANNELS)
    uint32_t blocks;                // process_input_block() calls since the stream started
    uint32_t frames;                // frames passed to it
    uint32_t block_us_last;         // duration of the last call
    uint32_t block_us_max;
    uint32_t block_us_avg_x100;     // mean duration, hundredths of a microsecond
    uint32_t clip_flags;            // global_status.clip_flags
    uint16_t peaks[Z4_PEAK_SLOTS];  // global_status.peaks: inputs 0-7, outputs 1-8, PDM; 32767 = full scale
    int32_t slot0_fill;             // output_ring_fill_signed(0), frames
    uint32_t ring_target;           // output_ring_target_frames
    uint32_t ring_drops;            // output_ring_drops: blocks refused by group admission
    uint32_t slot0_underrun_frames; // silence frames skipped into slot 0 by recovery
    uint32_t slot0_drop_frames;     // frames slot 0 refused (should stay 0)
    uint32_t pipeline_rate;         // audio_state.freq
    uint32_t vendor_requests;       // vendor requests DSPi accepted (SETUP stage), version 3
    uint32_t vendor_stalls;         // vendor requests DSPi stalled
    uint32_t notify_packets;        // event packets sent on EP 0x83
    uint32_t notify_keepalives;     // 1-byte idle packets sent on EP 0x83
    // Version 4: performance counters over process_input_block(), all wrapping totals.
    uint32_t pmu_counters;          // event counters the core has, 0 if the counters are unused
    uint32_t perf_blocks;
    uint32_t perf_cycles;           // inside the block, interrupts included
    uint32_t perf_irq_cycles;       // of those, cycles in interrupt handlers (USB and the SMI composer)
    uint32_t perf_irqs;             // interrupts taken inside the block
    uint32_t perf_ev[PMU_N_EVENTS]; // instructions, L1I, L1D and L2 refills, branch mispredicts
    uint32_t irq_cycles_all;        // all interrupt handler cycles
    pcm_out_status_t pcm;           // version 5: the PCM block (Z5 I2S out)
    uint32_t output_clock;          // shim_output_clock(), frames
    smi_out_status_t smi;           // version 7: the SMI output stream
    uint32_t fb_sent_q16;           // version 8: the feedback value sent, 16.16 samples per 1 ms
    uint32_t fb_rate_iir_q16;       // DSPi's controller: short-window rate
    uint32_t fb_rate_long_q16;      // long-window rate, 0 until valid
    int32_t fb_servo_q16;           // fill servo term
    int32_t fb_fill_err_q16;        // filtered slot 0 fill error, in 48-frame buckets
    uint32_t fb_updates;
    uint32_t smi_source;            // version 9: 1 when the SMI runs from PLLA, 0 on PLLD
    uint32_t pll_wd_events;         // PLLA watchdog: mismatches seen, restores, fallbacks to PLLD
    uint32_t pll_wd_restores;
    uint32_t pll_wd_fallbacks;
} z4_stats_t;

int8_t dcd_dwc2_iso_parity(uint8_t dir, uint8_t epnum);
#define DWC2_R(off) (*(volatile uint32_t *)(USB_OTG_GLOBAL_BASE + (off)))

static volatile u2_stats_t stats;
static uint32_t fb_override;    // feedback_send(): bench-fixed feedback value, 0 for automatic
static uint32_t fb_value;

static volatile bool reboot_requested;

static uint32_t dspi_blocks, dspi_frames, dspi_us_last, dspi_us_max;
static uint64_t dspi_us_total;
static uint32_t vendor_requests, vendor_stalls, notify_packets, notify_keepalives;
static uint32_t pmu_counters, perf_blocks, perf_cycles, perf_irq_cycles, perf_irqs, perf_ev[PMU_N_EVENTS];
static volatile bool consumer_running;

void tud_mount_cb(void) {
    stats.mounts++;
    usb_audio_mounted = 1;
}

void tud_umount_cb(void) {
    usb_audio_mounted = 0;
}

void tud_suspend_cb(bool remote_wakeup_en) {
    (void)remote_wakeup_en;
    stats.suspends++;
}

void tud_resume_cb(void) {
    stats.resumes++;
}

// ---------------------------------------------------------------------------------------------------
// UAC2 requests: clock source (rate) and feature unit (master mute and volume)

static bool clock_get(uint8_t rhport, audio_control_request_t const *req) {
    if (req->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ) {
        if (req->bRequest == AUDIO_CS_REQ_CUR) {
            audio_control_cur_4_t cur = {.bCur = (int32_t)tu_htole32(current_rate)};
            return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)req, &cur, sizeof(cur));
        }
        if (req->bRequest == AUDIO_CS_REQ_RANGE) {
            audio_control_range_4_n_t(N_SAMPLE_RATES) range = {.wNumSubRanges = tu_htole16(N_SAMPLE_RATES)};
            for (uint8_t i = 0; i < N_SAMPLE_RATES; i++) {
                range.subrange[i].bMin = (int32_t)sample_rates[i];
                range.subrange[i].bMax = (int32_t)sample_rates[i];
                range.subrange[i].bRes = 0;
            }
            return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)req, &range, sizeof(range));
        }
    } else if (req->bControlSelector == AUDIO_CS_CTRL_CLK_VALID && req->bRequest == AUDIO_CS_REQ_CUR) {
        audio_control_cur_1_t valid = {.bCur = 1};
        return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)req, &valid, sizeof(valid));
    }
    return false;
}

static bool clock_set(audio_control_request_t const *req, uint8_t const *buf) {
    if (req->bControlSelector != AUDIO_CS_CTRL_SAM_FREQ || req->bRequest != AUDIO_CS_REQ_CUR ||
        req->wLength != sizeof(audio_control_cur_4_t)) {
        return false;
    }
    uint32_t f = (uint32_t)((audio_control_cur_4_t const *)buf)->bCur;
    for (uint8_t i = 0; i < N_SAMPLE_RATES; i++) {
        if (f == sample_rates[i]) {
            current_rate = f;
            stats.rate_sets++;
            return true;
        }
    }
    return false;   // unsupported values are stalled, not clamped
}

static bool feature_get(uint8_t rhport, audio_control_request_t const *req) {
    if (req->bChannelNumber != 0) {
        return false;   // master controls only
    }
    if (req->bControlSelector == AUDIO_FU_CTRL_MUTE && req->bRequest == AUDIO_CS_REQ_CUR) {
        audio_control_cur_1_t m = {.bCur = mute_master};
        return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)req, &m, sizeof(m));
    }
    if (req->bControlSelector == AUDIO_FU_CTRL_VOLUME) {
        if (req->bRequest == AUDIO_CS_REQ_RANGE) {
            // -60 to 0 dB in 1 dB steps, as on DSPi's UAC1 boards.
            audio_control_range_2_n_t(1) range = {
                .wNumSubRanges = tu_htole16(1),
                .subrange[0] = {.bMin = tu_htole16(-60 * 256), .bMax = tu_htole16(0), .bRes = tu_htole16(256)},
            };
            return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)req, &range, sizeof(range));
        }
        if (req->bRequest == AUDIO_CS_REQ_CUR) {
            audio_control_cur_2_t v = {.bCur = tu_htole16(volume_master)};
            return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)req, &v, sizeof(v));
        }
    }
    return false;
}

// Host mute and volume drive DSPi's audio_state the way its UAC1 driver does. These callbacks run
// from tud_task() in the main loop, which audio_set_volume() requires.
static bool feature_set(audio_control_request_t const *req, uint8_t const *buf) {
    if (req->bChannelNumber != 0 || req->bRequest != AUDIO_CS_REQ_CUR) {
        return false;
    }
    if (req->bControlSelector == AUDIO_FU_CTRL_MUTE && req->wLength == sizeof(audio_control_cur_1_t)) {
        mute_master = ((audio_control_cur_1_t const *)buf)->bCur;
        audio_state.mute = mute_master != 0;
        return true;
    }
    if (req->bControlSelector == AUDIO_FU_CTRL_VOLUME && req->wLength == sizeof(audio_control_cur_2_t)) {
        volume_master = ((audio_control_cur_2_t const *)buf)->bCur;
        audio_set_volume(volume_master);
        return true;
    }
    return false;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    audio_control_request_t const *req = (audio_control_request_t const *)p_request;
    stats.class_requests++;
    if (req->bEntityID == UAC2_ENTITY_CLOCK) {
        return clock_get(rhport, req);
    }
    if (req->bEntityID == UAC2_ENTITY_FEATURE_UNIT) {
        return feature_get(rhport, req);
    }
    return false;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *buf) {
    (void)rhport;
    audio_control_request_t const *req = (audio_control_request_t const *)p_request;
    stats.class_requests++;
    if (req->bEntityID == UAC2_ENTITY_CLOCK) {
        return clock_set(req, buf);
    }
    if (req->bEntityID == UAC2_ENTITY_FEATURE_UNIT) {
        return feature_set(req, buf);
    }
    return false;
}

bool tud_audio_set_itf_close_EP_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void)rhport;
    if (tu_u16_low(tu_le16toh(p_request->wIndex)) == ITF_NUM_AUDIO_STREAMING) {
        current_alt = 0;
        usb_audio_alt_set = 0;
    }
    return true;
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    if (tu_u16_low(tu_le16toh(p_request->wIndex)) == ITF_NUM_AUDIO_STREAMING) {
        current_alt = tu_u16_low(tu_le16toh(p_request->wValue));
        usb_audio_alt_set = current_alt;
        stats.set_alt_count++;
        // A feedback packet still queued when the last stream stopped leaves the endpoint claimed, and
        // the class's re-activation clears busy but not claimed, so feedback would never be sent again.
        if (current_alt != 0) {
            usbd_edpt_release(rhport, EP_AUDIO_FB);
        }
    }
    return true;
}

void tud_audio_feedback_params_cb(uint8_t func_id, uint8_t alt_itf, audio_feedback_params_t *feedback_param) {
    (void)func_id;
    (void)alt_itf;
    // feedback_send() below sets the feedback from DSPi's own controller.
    feedback_param->method = AUDIO_FEEDBACK_METHOD_DISABLED;
    feedback_param->sample_freq = current_rate;
}

bool tud_audio_rx_done_post_read_cb(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id, uint8_t ep_out,
                                    uint8_t cur_alt_setting) {
    (void)rhport;
    (void)func_id;
    (void)ep_out;
    (void)cur_alt_setting;
    stats.out_packets++;
    stats.out_bytes += n_bytes_received;
    usb_audio_packets++;
    return true;
}

void tud_audio_fb_done_cb(uint8_t func_id) {
    (void)func_id;
    stats.fb_sent++;
}

// ---------------------------------------------------------------------------------------------------
// Vendor interface (DSPi Console's channel) and bench requests

// ---------------------------------------------------------------------------------------------------
// Notifications: usb_notify_drain() from DSPi's usb_audio.c, on this driver's EP 0x83. Events go out
// as soon as they are queued; with none, a 1-byte idle packet after 100 ms of quiet. Each packet is
// at most 64 bytes, a short packet on this 512-byte High Speed bulk endpoint, so each ends a transfer.

#define NOTIFY_IDLE_KEEPALIVE_US 100000u
#define Z4_NOTIFY_MAX_PKT 64u

static uint8_t notify_buf[Z4_NOTIFY_MAX_PKT] __attribute__((aligned(4)));
static bool notify_ep_open;
static uint32_t notify_last_arm_us;

static void notify_drain(uint8_t rhport) {
    if (!notify_ep_open) {
        return;
    }
    if (!notify_has_pending_for(NOTIFY_CONSUMER_USB) &&
        (uint32_t)(time_us_32() - notify_last_arm_us) < NOTIFY_IDLE_KEEPALIVE_US) {
        return;
    }
    if (!usbd_edpt_claim(rhport, EP_NOTIFY_IN)) {
        return;     // busy: the completion re-arms
    }
    uint16_t len = 0;
    if (notify_has_pending_for(NOTIFY_CONSUMER_USB)) {
        len = notify_peek_next_for(NOTIFY_CONSUMER_USB, notify_buf, Z4_NOTIFY_MAX_PKT);
    }
    bool event = len > 0;
    if (!event) {
        notify_buf[0] = 0x00;
        len = 1;
    }
    if (!usbd_edpt_xfer(rhport, EP_NOTIFY_IN, notify_buf, len)) {
        usbd_edpt_release(rhport, EP_NOTIFY_IN);
        return;     // the same event is retried next pass
    }
    notify_last_arm_us = time_us_32();
    if (event) {
        notify_commit_pop_for(NOTIFY_CONSUMER_USB);
        notify_packets++;
    } else {
        notify_keepalives++;
    }
}

static uint16_t vendor_open(uint8_t rhport, tusb_desc_interface_t const *itf, uint16_t max_len) {
    if (itf->bInterfaceClass != TUSB_CLASS_VENDOR_SPECIFIC) {
        return 0;
    }
    uint16_t len = tu_desc_len(itf);
    uint8_t const *p = (uint8_t const *)itf + len;
    for (uint8_t i = 0; i < itf->bNumEndpoints && len < max_len; i++) {
        if (tu_desc_type(p) == TUSB_DESC_ENDPOINT) {
            tusb_desc_endpoint_t const *ep = (tusb_desc_endpoint_t const *)p;
            TU_ASSERT(usbd_edpt_open(rhport, ep), 0);
            if (ep->bEndpointAddress == EP_NOTIFY_IN) {
                notify_ep_open = true;
            }
        }
        len += tu_desc_len(p);
        p += tu_desc_len(p);
    }
    return len;
}

static bool vendor_control(uint8_t rhport, uint8_t stage, tusb_control_request_t const *req) {
    // Only standard interface requests reach a class driver. The vendor interface has one alt.
    if (stage == CONTROL_STAGE_SETUP && req->bmRequestType_bit.type == TUSB_REQ_TYPE_STANDARD &&
        req->bRequest == TUSB_REQ_SET_INTERFACE && req->wValue == 0) {
        return tud_control_status(rhport, req);
    }
    return stage != CONTROL_STAGE_SETUP;
}

static bool vendor_xfer(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes) {
    (void)result;
    (void)xferred_bytes;
    if (ep_addr != EP_NOTIFY_IN) {
        return false;
    }
    notify_drain(rhport);   // delivered: send the next one, if any
    return true;
}

static void vendor_noop(void) {}

static bool vendor_deinit(void) {
    return true;
}

// Bus reset, as DSPi's UAC1 driver handles it: queued notifications are stale.
static void vendor_reset(uint8_t rhport) {
    (void)rhport;
    notify_ep_open = false;
    notify_reset_queue();
    notify_master_vol_host_initiated = false;
}

static const usbd_class_driver_t vendor_driver = {
    .name = "DSPi_vendor",
    .init = vendor_noop,
    .deinit = vendor_deinit,
    .reset = vendor_reset,
    .open = vendor_open,
    .control_xfer_cb = vendor_control,
    .xfer_cb = vendor_xfer,
    .sof = NULL,
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count) {
    *driver_count = 1;
    return &vendor_driver;
}

#define REQ_Z_GET_STATS 0x4F
#define BOARD_ID_PI_ZERO_2W 1   // GET_PLATFORM byte 7, matched by DSPi Console's Constants.swift
#define U2_STATS_MAGIC  0x5532u     // "U2"

typedef struct {
    uint16_t magic;
    uint16_t version;
    uint32_t uptime_ms;
    uint32_t rate;
    uint8_t alt;
    uint8_t mounted;
    uint8_t speed;      // tusb_speed_t: 0 full, 2 high
    uint8_t reserved;
    u2_stats_t stats;
    z4_stats_t dspi;    // version 2, grown in version 3
} u2_reply_t;

// Wire layout read by the bench tools: version 1 fields unchanged, the DSPi block appended.
_Static_assert(offsetof(u2_reply_t, dspi) == 116, "0x4F reply: DSPi block offset");
_Static_assert(offsetof(z4_stats_t, peaks) == 28 && offsetof(z4_stats_t, slot0_fill) == 64, "0x4F reply layout");
_Static_assert(sizeof(u2_reply_t) == 388, "0x4F reply size");

static void fill_dspi_stats(z4_stats_t *z) {
    memset(z, 0, sizeof(*z));
    z->cpu0_load = global_status.cpu0_load;
    z->cpu1_load = global_status.cpu1_load;
    z->core1_mode = (uint8_t)core1_mode;
    z->n_channels = NUM_CHANNELS;
    z->blocks = dspi_blocks;
    z->frames = dspi_frames;
    z->block_us_last = dspi_us_last;
    z->block_us_max = dspi_us_max;
    z->block_us_avg_x100 = dspi_blocks ? (uint32_t)(dspi_us_total * 100u / dspi_blocks) : 0;
    z->clip_flags = global_status.clip_flags;
    for (int i = 0; i < NUM_CHANNELS; i++) {
        z->peaks[i] = global_status.peaks[i];
    }
    z->slot0_fill = output_ring_fill_signed(0);
    z->ring_target = output_ring_target_frames;
    z->ring_drops = output_ring_drops;
    if (output_types[0] == OUTPUT_TYPE_I2S) {
        audio_i2s_instance_t *inst = i2s_instance_ptrs[0];
        z->slot0_underrun_frames = inst ? inst->underrun_frames : 0;
        z->slot0_drop_frames = inst ? inst->drop_frames : 0;
    } else {
        audio_spdif_instance_t *inst = spdif_instance_ptrs[0];
        z->slot0_underrun_frames = inst ? inst->underrun_frames : 0;
        z->slot0_drop_frames = inst ? inst->drop_frames : 0;
    }
    z->pipeline_rate = audio_state.freq;
    z->vendor_requests = vendor_requests;
    z->vendor_stalls = vendor_stalls;
    z->notify_packets = notify_packets;
    z->notify_keepalives = notify_keepalives;
    z->pmu_counters = pmu_counters;
    z->perf_blocks = perf_blocks;
    z->perf_cycles = perf_cycles;
    z->perf_irq_cycles = perf_irq_cycles;
    z->perf_irqs = perf_irqs;
    memcpy(z->perf_ev, perf_ev, sizeof(perf_ev));
    z->irq_cycles_all = irq_cycles();
    pcm_out_status(&z->pcm);
    z->output_clock = shim_output_clock();
    smi_out_status(&z->smi);
    z->fb_sent_q16 = fb_value;
    z->fb_rate_iir_q16 = fb_ctrl.rate_estimate_q16;
    z->fb_rate_long_q16 = fb_ctrl.long_valid ? fb_ctrl.rate_long_q16 : 0;
    z->fb_servo_q16 = fb_ctrl.servo_q16;
    z->fb_fill_err_q16 = fb_ctrl.fill_error_filtered;
    z->fb_updates = fb_ctrl.diag_updates;
    static pll_out_status_t ps;
    pll_out_status(&ps);
    z->smi_source = smi_out_source();
    z->pll_wd_events = ps.wd_events;
    z->pll_wd_restores = ps.wd_restores;
    z->pll_wd_fallbacks = ps.wd_fallbacks;
}

bool dspi_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *req);

// VideoCore sensor readings for DSPi Console's System Information, refreshed once a second from the
// main loop because a mailbox round trip is too slow for a control request.
static volatile uint32_t sensor_arm_hz, sensor_core_mv;
static volatile int32_t sensor_temp_cc;

static void sensors_refresh(void) {
    static uint32_t last_us;
    static bool primed;
    uint32_t now = board_micros();
    if ((primed && now - last_us < 1000000u) || i2s_input_pi_running()) {
        return;
    }
    primed = true;
    last_us = now;
    if (!pll_out_counter_claimed()) {   // the firmware may measure with the same counter
        sensor_arm_hz = mbox_clock_rate_measured(VCMAILBOX_CLOCK_ARM);
    }
    sensor_core_mv = (mbox_voltage_uv(1) + 500u) / 1000u;
    sensor_temp_cc = (int32_t)(mbox_temperature_mc() + 5u) / 10;
}

static volatile uint32_t spdif_monitor_request = 0xFFFFFFFFu;    // bench 0x44, run from the main loop
static volatile uint32_t smi_source_request = 0xFFFFFFFFu;     // bench 0x46 op 4, run from the main loop

// On a card that carries the USB loader beside DSPi, the firmware placed the loader at LOADER_BASE; its first
// instruction is startup.S's, the same as ours. The marker then sends the reboot to it. With no loader
// there (the loader card starts it as the kernel anyway), the reboot is a plain one.
static void arm_loader_handoff(void) {
    extern const uint32_t _start[];
    volatile uint32_t *loader = (volatile uint32_t *)LOADER_BASE;
    if (loader[0] != _start[0]) {
        return;
    }
    volatile uint32_t *m = (volatile uint32_t *)LOADER_HANDOFF_ADDR;
    m[0] = LOADER_HANDOFF_MAGIC;
    m[1] = ~LOADER_HANDOFF_MAGIC;
    data_clean((void *)m, 8);
    __asm__ volatile("dsb" ::: "memory");
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *req) {
    static u2_reply_t reply;
    bool in = req->bmRequestType_bit.direction == TUSB_DIR_IN;
    // Bench requests are answered only when sent to the device. DSPi Console and its test harness
    // address the vendor interface, so for them these codes stay unknown and stall, as on an RP2350.
    bool bench = req->bmRequestType_bit.recipient == TUSB_REQ_RCPT_DEVICE;
    if (bench && req->bRequest == APP_REQ_REBOOT_TO_LOADER && !in) {
        if (stage == CONTROL_STAGE_SETUP) {
            return tud_control_status(rhport, req);
        }
        if (stage == CONTROL_STAGE_ACK) {
            reboot_requested = true;
        }
        return true;
    }
    // Profiler: 0x4D starts sampling every wValue us (0 stops). 0x4E reads the sample count with
    // wValue 0xFFFF, otherwise 4 KB of samples at chunk wValue.
    if (bench && req->bRequest == 0x4D && !in) {
        if (stage == CONTROL_STAGE_SETUP) {
            z4_prof_start(req->wValue);
            return tud_control_status(rhport, req);
        }
        return true;
    }
    // 0x4C: clock register dump, boot snapshot and live values; wValue bit 0 adds the firmware's clock
    // rates, which takes mailbox calls long enough to disturb streaming.
    if (bench && req->bRequest == 0x4C && in) {
        static clk_dump_t dump;
        if (stage == CONTROL_STAGE_SETUP) {
            uint32_t len = clk_dump_fill(&dump, req->wValue & 1u);
            return tud_control_xfer(rhport, req, &dump, req->wLength < len ? req->wLength : (uint16_t)len);
        }
        return true;
    }
    // 0x45: USB event accounting (see usb_health_t); wValue 1 also clears the longest main loop gap.
    if (bench && req->bRequest == 0x45 && in) {
        static usb_health_t copy;
        if (stage == CONTROL_STAGE_SETUP) {
            extern volatile uint32_t dwc2_pizero_wait_timeouts;
            uh.wait_timeouts = dwc2_pizero_wait_timeouts;
            copy = uh;
            if (req->wValue == 1) {
                uh.gap_max_us = 0;
                uh.armed = 1;
            }
            return tud_control_xfer(rhport, req, &copy, req->wLength < sizeof(copy) ? req->wLength : sizeof(copy));
        }
        return true;
    }
    // 0x43: the whole-core CPU load DSPi Console shows, with its parts (cpu_meter.c).
    if (bench && req->bRequest == 0x43 && in) {
        static cpu_meter_status_t ms;
        if (stage == CONTROL_STAGE_SETUP) {
            cpu_meter_status(&ms);
            return tud_control_xfer(rhport, req, &ms, req->wLength < sizeof(ms) ? req->wLength : sizeof(ms));
        }
        return true;
    }
    // 0x44: the S/PDIF receiver's capture and decoder (spdif_rx_pi.c). IN returns its status; OUT wValue 1
    // runs it as a bench monitor while DSPi's input is something else, and 0 stops the monitor.
    if (bench && req->bRequest == 0x44) {
        static spdif_rx_pi_status_t rs;
        if (stage != CONTROL_STAGE_SETUP) {
            return true;
        }
        if (in) {
            spdif_rx_pi_status(&rs);
            return tud_control_xfer(rhport, req, &rs, req->wLength < sizeof(rs) ? req->wLength : sizeof(rs));
        }
        spdif_monitor_request = req->wValue ? 1u : 0u;
        return tud_control_status(rhport, req);
    }
    // 0x47: PCM receive rate (port plan section 6). OUT runs a capture of wValue ms from the main loop, at
    // the PCM clock bench request 0x46 op 2 set; IN returns the result.
    if (bench && req->bRequest == 0x47) {
        if (stage != CONTROL_STAGE_SETUP) {
            return true;
        }
        if (in) {
            const pcm_rx_result_t *r = pcm_rx_result();
            return tud_control_xfer(rhport, req, (void *)r, req->wLength < sizeof(*r) ? req->wLength : sizeof(*r));
        }
        pcm_rx_request(req->wValue);
        return tud_control_status(rhport, req);
    }
    // 0x51: the I2S receiver (i2s_input_pi.h). IN returns its status, or with wValue k + 1 the snapshots
    // around its k-th bad half; OUT sets the bench overrides (i2s_input_pi_bench_t), or with no data
    // puts the defaults back.
    if (bench && req->bRequest == 0x51) {
        static i2s_input_pi_bench_t pb;
        static i2s_input_pi_status_t ps;
        if (in) {
            if (stage != CONTROL_STAGE_SETUP) {
                return true;
            }
            if (req->wValue) {
                return tud_control_xfer(rhport, req, (void *)(uintptr_t)i2s_input_pi_dump(req->wValue - 1u),
                                        req->wLength < 4096u ? req->wLength : 4096u);
            }
            i2s_input_pi_status(&ps);
            z4_i2s_prefill_counts(&ps.prefill_drains, &ps.prefill_starts);
            ps.dac_muted = dac_hw_mute_is_asserted();
            return tud_control_xfer(rhport, req, &ps, req->wLength < sizeof(ps) ? req->wLength : sizeof(ps));
        }
        if (stage == CONTROL_STAGE_SETUP) {
            if (req->wLength == 0u) {
                i2s_input_pi_bench(NULL);
                return tud_control_status(rhport, req);
            }
            memset(&pb, 0, sizeof(pb));
            return tud_control_xfer(rhport, req, &pb, req->wLength < sizeof(pb) ? req->wLength : sizeof(pb));
        }
        if (stage == CONTROL_STAGE_DATA) {
            i2s_input_pi_bench(&pb);
        }
        return true;
    }
    // 0x52: the ADAT receiver (adat_input_pi.h). IN returns its status; OUT starts its frame reader afresh
    // with the bench settings (adat_input_pi_bench_t), or with no data stops it.
    if (bench && req->bRequest == 0x52) {
        static adat_input_pi_bench_t ab;
        static adat_input_pi_status_t as;
        if (in) {
            if (stage != CONTROL_STAGE_SETUP) {
                return true;
            }
            adat_input_pi_status(&as);
            return tud_control_xfer(rhport, req, &as, req->wLength < sizeof(as) ? req->wLength : sizeof(as));
        }
        if (stage == CONTROL_STAGE_SETUP) {
            if (req->wLength == 0u) {
                adat_input_pi_bench(NULL);
                return tud_control_status(rhport, req);
            }
            memset(&ab, 0, sizeof(ab));
            return tud_control_xfer(rhport, req, &ab, req->wLength < sizeof(ab) ? req->wLength : sizeof(ab));
        }
        if (stage == CONTROL_STAGE_DATA) {
            adat_input_pi_bench(&ab);
        }
        return true;
    }
    // 0x46: PLLA (PiZero_PLLA_plan.md phase 1). OUT wValue 1 retunes to row wIndex, 2 drives the unused
    // PCM clock from source wIndex >> 12 with integer divider wIndex & 0xFFF, 3 starts the frequency
    // counter on mux wIndex & 0xFF for (wIndex >> 8) x 192000 oscillator cycles, 4 puts the SMI on PLLA
    // (wIndex 1) or PLLD (0), 5 captures settling into row wIndex & 0xFF over wIndex >> 8 windows, 6 trims PLLA by
    // (int16)wIndex x 0.01 ppm, 7 rewrites PLLA behind the watchdog's back (once, or for wIndex ms). IN returns
    // the status, or with wValue 1 the settling capture.
    if (bench && req->bRequest == 0x46) {
        static pll_out_status_t pst;
        if (stage != CONTROL_STAGE_SETUP) {
            return true;
        }
        if (in && req->wValue == 1) {
            const pll_out_capture_t *c = pll_out_capture();
            return tud_control_xfer(rhport, req, (void *)c, req->wLength < sizeof(*c) ? req->wLength : sizeof(*c));
        }
        if (in) {
            pll_out_status(&pst);
            return tud_control_xfer(rhport, req, &pst, req->wLength < sizeof(pst) ? req->wLength : sizeof(pst));
        }
        if (req->wValue == 1) {
            pll_out_request(req->wIndex);
        } else if (req->wValue == 2) {
            pll_out_pcm_clock(req->wIndex >> 12, req->wIndex & 0xFFFu);
        } else if (req->wValue == 3) {
            uint32_t gates = req->wIndex >> 8;
            pll_out_count_request(req->wIndex & 0xFFu, (gates ? gates : 1u) * 192000u);
        } else if (req->wValue == 5) {
            pll_out_capture_request(req->wIndex & 0xFFu, req->wIndex >> 8);
        } else if (req->wValue == 6) {
            pll_out_trim((int32_t)(int16_t)req->wIndex * 10);     // wIndex in units of 0.01 ppm
        } else if (req->wValue == 7) {
            pll_out_sabotage(req->wIndex);    // bench: rewrite PLLA as the firmware would, for wIndex ms
        } else if (req->wValue == 4) {
            smi_source_request = req->wIndex;
        } else {
            return false;
        }
        return tud_control_status(rhport, req);
    }
    // 0x4A: firmware clock probes for the PLLA plan (phase 0). wValue 1 sets the V3D clock to wIndex MHz,
    // 2 sets the V3D clock state to wIndex, 3 sets the ARM clock to wIndex MHz (0 its minimum, 0xFFFF its
    // maximum). Mailbox calls stall the main loop, so these are for bench use only.
    if (bench && req->bRequest == 0x4A && !in) {
        if (stage == CONTROL_STAGE_SETUP) {
            if (req->wValue == 1) {
                mbox_set_clock_rate(VCMAILBOX_CLOCK_V3D, (uint32_t)req->wIndex * 1000000u);
            } else if (req->wValue == 2) {
                mbox_set_clock_state(VCMAILBOX_CLOCK_V3D, req->wIndex);
            } else if (req->wValue == 3) {
                uint32_t hz = req->wIndex == 0 ? mbox_clock_rate_min(VCMAILBOX_CLOCK_ARM)
                              : req->wIndex == 0xFFFF ? mbox_clock_rate_max(VCMAILBOX_CLOCK_ARM)
                                                      : (uint32_t)req->wIndex * 1000000u;
                mbox_set_clock_rate(VCMAILBOX_CLOCK_ARM, hz);
            } else {
                return false;
            }
            return tud_control_status(rhport, req);
        }
        return true;
    }
    // 0x48: fixes the feedback value at wIndex << 16 | wValue (16.16), or 0 for automatic.
    if (bench && req->bRequest == 0x48 && !in) {
        if (stage == CONTROL_STAGE_SETUP) {
            fb_override = (uint32_t)req->wIndex << 16 | req->wValue;
            return tud_control_status(rhport, req);
        }
        return true;
    }
    // 0x49: up to 16 composed SMI frames not yet sent: the first frame's number, then 512 bytes a frame.
    if (bench && req->bRequest == 0x49 && in) {
        static uint32_t snap[1 + 16 * SMI_FRAME_WORDS / 2];
        if (stage == CONTROL_STAGE_SETUP) {
            uint32_t n = req->wValue < 1 ? 1 : (req->wValue > 16 ? 16 : req->wValue);
            snap[0] = shim_smi_snapshot((uint16_t *)&snap[1], n);
            uint32_t len = 4u + n * SMI_FRAME_WORDS * 2u;
            return tud_control_xfer(rhport, req, snap, req->wLength < len ? req->wLength : (uint16_t)len);
        }
        return true;
    }
    // 0x4B: count level changes on GPIO wValue over wIndex ms; replies toggles and samples.
    if (bench && req->bRequest == 0x4B && in) {
        static uint32_t probe_reply[2];
        if (stage == CONTROL_STAGE_SETUP) {
            probe_reply[0] = smi_out_probe_toggles(req->wValue, req->wIndex > 100 ? 100 : req->wIndex,
                                                   &probe_reply[1]);
            return tud_control_xfer(rhport, req, probe_reply, req->wLength < 8 ? req->wLength : 8);
        }
        return true;
    }
    if (bench && req->bRequest == 0x4E && in) {
        static uint32_t count_reply;
        if (stage != CONTROL_STAGE_SETUP) {
            return true;
        }
        if (req->wValue == 0xFFFF) {
            count_reply = z4_prof_count;
            return tud_control_xfer(rhport, req, &count_reply, req->wLength < 4 ? req->wLength : 4);
        }
        uint32_t first = (uint32_t)req->wValue * 1024u;
        if (first >= Z4_PROF_MAX) {
            return false;
        }
        uint16_t len = req->wLength < 4096 ? req->wLength : 4096;
        return tud_control_xfer(rhport, req, &z4_prof_samples[first], len);
    }
    if (bench && req->bRequest == REQ_Z_GET_STATS && in) {
        if (stage != CONTROL_STAGE_SETUP) {
            return true;
        }
        reply.magic = U2_STATS_MAGIC;
        reply.version = 9;
        reply.uptime_ms = board_micros() / 1000u;
        reply.rate = current_rate;
        reply.alt = current_alt;
        reply.mounted = tud_mounted() ? 1u : 0u;
        reply.speed = (uint8_t)tud_speed_get();
        stats.irqs = irq_count();
        stats.irq_spurious = irq_spurious();
        stats.fb_diepctl = DWC2_R(0x900 + 0x20 * 2);
        stats.fb_dieptsiz = DWC2_R(0x910 + 0x20 * 2);
        stats.fb_diepint = DWC2_R(0x908 + 0x20 * 2);
        stats.data_doepctl = DWC2_R(0xB00 + 0x20 * 1);
        stats.gintmsk = DWC2_R(0x018);
        stats.dsts = DWC2_R(0x808);
        stats.fb_busy = usbd_edpt_busy(rhport, EP_AUDIO_FB);
        stats.fb_parity = dcd_dwc2_iso_parity(TUSB_DIR_IN, 2);
        stats.data_parity = dcd_dwc2_iso_parity(TUSB_DIR_OUT, 1);
        memcpy(&reply.stats, (const void *)&stats, sizeof(reply.stats));
        fill_dspi_stats(&reply.dspi);
        uint16_t len = req->wLength < sizeof(reply) ? req->wLength : (uint16_t)sizeof(reply);
        return tud_control_xfer(rhport, req, &reply, len);
    }
    // GET_PLATFORM for 8 or more bytes: DSPi's 7-byte reply plus a board byte that DSPi Console shows
    // in Device Information. Byte 0 stays RP2350, the code base, which every feature gate uses, so a
    // Console that asks for 7 bytes (or predates the board byte) sees an RP2350 as before.
    if (!bench && req->bRequest == REQ_GET_PLATFORM && in && req->wLength >= 8) {
        static uint8_t platform_reply[8];
        if (stage == CONTROL_STAGE_SETUP) {
            platform_reply[0] = PLATFORM_RP2350;
            platform_reply[1] = (uint8_t)(FW_VERSION_PACKED >> 8);
            platform_reply[2] = (uint8_t)(FW_VERSION_PACKED & 0xFF);
            platform_reply[3] = NUM_OUTPUT_CHANNELS;
            platform_reply[4] = FW_VERSION_MINOR;
            platform_reply[5] = FW_VERSION_PATCH;
            platform_reply[6] = FW_VERSION_BETA;
            platform_reply[7] = BOARD_ID_PI_ZERO_2W;
            vendor_requests++;
            return tud_control_xfer(rhport, req, platform_reply, sizeof(platform_reply));
        }
        return true;
    }
    // GET_STATUS 13, 14 and 16 (clock, core voltage, temperature) come from the VideoCore, not DSPi's
    // RP2350 clock tree, regulator table and ADC formula. Values are cached by sensors_refresh().
    if (!bench && req->bRequest == REQ_GET_STATUS && in &&
        (req->wValue == 13 || req->wValue == 14 || req->wValue == 16)) {
        static uint32_t sensor_reply;
        if (stage == CONTROL_STAGE_SETUP) {
            sensor_reply = req->wValue == 13 ? sensor_arm_hz : req->wValue == 14 ? sensor_core_mv : (uint32_t)sensor_temp_cc;
            vendor_requests++;
            uint16_t len = req->wLength < 4 ? req->wLength : 4;
            return tud_control_xfer(rhport, req, &sensor_reply, len);
        }
        return true;
    }
    // Everything else is DSPi's, every stage.
    bool ok = dspi_vendor_control_xfer_cb(rhport, stage, req);
    if (stage == CONTROL_STAGE_SETUP) {
        if (ok) {
            vendor_requests++;
        } else {
            vendor_stalls++;
        }
    }
    return ok;
}

// ---------------------------------------------------------------------------------------------------
// Stream consumer: every millisecond, the sample rate's worth of frames from the FIFO into DSPi.
// It starts once the FIFO is half full, the level FIFO-count feedback aims for.

static uint8_t rx_buf[AUDIO_BUFFER_SAMPLES * 6] __attribute__((aligned(4)));

// DSPi's own USB decode (process_audio_packet in usb_audio.c, RP2350 stereo path): full-scale integer
// to +-1.0, times the per-channel preamp.
static void decode_into_dspi(uint32_t frames, uint8_t alt) {
    const float gain_l = global_preamp_linear[0];
    const float gain_r = global_preamp_linear[1];
    if (alt == 1) {
        const int16_t *in = (const int16_t *)rx_buf;
        const float g_l = (1.0f / 32768.0f) * gain_l;
        const float g_r = (1.0f / 32768.0f) * gain_r;
        for (uint32_t i = 0; i < frames; i++) {
            buf_l[i] = (float)in[2 * i] * g_l;
            buf_r[i] = (float)in[2 * i + 1] * g_r;
        }
    } else {
        const uint8_t *p = rx_buf;
        const float g_l = (1.0f / 8388608.0f) * gain_l;
        const float g_r = (1.0f / 8388608.0f) * gain_r;
        for (uint32_t i = 0; i < frames; i++, p += 6) {
            int32_t l = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 8;
            int32_t r = (int32_t)((uint32_t)p[3] << 8 | (uint32_t)p[4] << 16 | (uint32_t)p[5] << 24) >> 8;
            buf_l[i] = (float)l * g_l;
            buf_r[i] = (float)r * g_r;
        }
    }
}

static void run_block(uint32_t frames, uint8_t alt) {
    decode_into_dspi(frames, alt);
    if (!sync_started) {
        start_time_us = time_us_64();
        sync_started = true;
    }
    total_samples_produced += frames;

    pmu_snap_t a, b;
    uint32_t irq_c0 = irq_cycles(), irq_n0 = irq_count();
    uint32_t t0 = board_micros();
    pmu_read(&a);
    z4_prof_in_block = true;
    process_input_block(frames);
    z4_prof_in_block = false;
    pmu_read(&b);
    uint32_t us = board_micros() - t0;
    perf_blocks++;
    perf_cycles += b.cycles - a.cycles;
    perf_irq_cycles += irq_cycles() - irq_c0;
    perf_irqs += irq_count() - irq_n0;
    for (int i = 0; i < PMU_N_EVENTS; i++) {
        perf_ev[i] += b.ev[i] - a.ev[i];
    }

    dspi_blocks++;
    dspi_frames += frames;
    dspi_us_last = us;
    dspi_us_total += us;
    if (us > dspi_us_max) {
        dspi_us_max = us;
    }
}

// A starting stream only restarts the pipeline while USB is the selected input.
static void stream_start(void) {
    sync_started = false;
    total_samples_produced = 0;
    if (active_input_source == INPUT_SOURCE_USB) {
        z4_stream_restart();
    }
    dspi_blocks = dspi_frames = dspi_us_last = dspi_us_max = 0;
    dspi_us_total = 0;
}

// USB feedback: DSPi's own controller (usb_feedback_controller.c), unchanged. It measures the output rate
// against the host's 1 ms frames over an 8-16 s window and servos slot 0's fill to DSPi's target, all in
// 16.16 samples per 1 ms. That is also the unit macOS reads at High Speed with a 1 ms data interval
// (bring-up section 19), so its value is sent as is, with no 10.14 rounding.

extern audio_spdif_instance_t *spdif_instance_ptrs[];
extern volatile bool output_type_switch_in_progress;

// From the USB interrupt, once per new 1 ms frame: the frame number and the SMI's consumption, latched
// together, and slot 0's fill from its ring's snapshot pair. The DMA count is word-precise, unlike the
// composer's 16-frame steps. This replaces DSPi's SOF hook without an 8 kHz microframe interrupt.
static void feedback_measure(void) {
    static uint32_t last_frame = 0xFFFFu;
    if (current_alt == 0 || output_type_switch_in_progress) {
        return;
    }
    uint32_t frame = (DWC2_R(0x808) >> 11) & 0x7FFu;    // DSTS SOFFN: frame << 3 | microframe at HS
    if (frame == last_frame) {
        return;
    }
    last_frame = frame;
    uint32_t words = smi_out_words2();
    uint32_t fill_frames = 0;
    if (output_types[0] == OUTPUT_TYPE_I2S) {
        audio_i2s_instance_t *inst = i2s_instance_ptrs[0];
        if (inst && inst->ring) {
            fill_frames = inst->snap_produced_frames - inst->snap_consumed_frames;
        }
    } else {
        audio_spdif_instance_t *inst = spdif_instance_ptrs[0];
        if (inst && inst->ring) {
            fill_frames = inst->snap_produced_frames - inst->snap_consumed_frames;
        }
    }
    if ((int32_t)fill_frames < 0) {
        fill_frames = 0;
    }
    fb_ctrl_sof_update(&fb_ctrl, frame, words, 13, (fill_frames << 16) / SLOT_FILL_FRAMES_PER_BUCKET,
                       (output_ring_target_frames << 16) / SLOT_FILL_FRAMES_PER_BUCKET);
}

// From the main loop: sends the controller's value, nominal while another input is selected (as DSPi
// does), or the bench override. DSPi's 10.14 copy is kept for its own status reports.
static void feedback_send(void) {
    uint32_t nominal = nominal_feedback_10_14 << 2;
    uint32_t fb = fb_ctrl.feedback_out_q16 ? fb_ctrl.feedback_out_q16 : nominal;
    if (active_input_source != INPUT_SOURCE_USB) {
        fb = nominal;
    }
    if (fb_override) {
        fb = fb_override;
    }
    feedback_10_14 = (fb + 2u) >> 2;
    if (fb != fb_value) {
        fb_value = fb;
        tud_audio_fb_set(fb);
    }
}

// Everything the host has delivered goes into the pipeline as soon as it lands, so the TinyUSB FIFO stays
// near empty and the slot rings are the only buffer, which the feedback holds at DSPi's target. While
// another input is selected the host's stream is drained and dropped, as DSPi ignores it.
static void consume_task(void) {
    static bool running;
    static bool busy;
    static uint32_t last_data_us;
    static bool in_gap;

    if (busy) {
        return;     // a stream restart is settling the pipeline from in here
    }
    uint8_t alt = current_alt;
    uint16_t level = tud_audio_available();
    stats.fifo_level = level;
    bool usb_source = active_input_source == INPUT_SOURCE_USB;
    if (alt == 0) {
        if (running) {
            sync_started = false;
        }
        running = false;
        consumer_running = false;
        return;
    }
    feedback_send();
    uint32_t bytes_per_frame = 2u * (alt == 1 ? 2u : 3u);
    uint32_t now = board_micros();
    if (!running) {
        // Collect DSPi's fill target (scaled if the rate is about to change) before starting, so the
        // first drain puts the slot rings at target and the feedback only has to hold them there.
        uint32_t target = output_ring_target_frames;
        if (audio_state.freq && audio_state.freq != current_rate) {
            target = (uint32_t)((uint64_t)target * current_rate / audio_state.freq);
        }
        if (level < target * bytes_per_frame) {
            return;
        }
        running = true;
        busy = true;
        stream_start();
        busy = false;
        last_data_us = board_micros();
    } else if (usb_source && audio_state.freq != current_rate) {
        busy = true;
        stream_start();     // the host changed rate without closing the stream
        busy = false;
        last_data_us = board_micros();
    }
    consumer_running = usb_source;

    uint32_t frames = level / bytes_per_frame;
    if (frames) {
        last_data_us = now;
        in_gap = false;
    } else if (!in_gap && now - last_data_us >= 3000u) {
        stats.underruns++;  // the host delivered nothing for 3 ms while streaming
        in_gap = true;
    }
    // At most one producer block per pipeline call (output_latency.c's cap, 192 at most).
    while (frames) {
        uint32_t n = frames < output_ring_block_cap ? frames : output_ring_block_cap;
        if (n > AUDIO_BUFFER_SAMPLES) {
            n = AUDIO_BUFFER_SAMPLES;
        }
        uint16_t got = tud_audio_read(rx_buf, (uint16_t)(n * bytes_per_frame));
        stats.consumed_bytes += got;
        n = got / bytes_per_frame;
        if (n == 0) {
            break;
        }
        if (usb_source) {
            run_block(n, alt);
        }
        frames -= n;
    }
}

// ---------------------------------------------------------------------------------------------------
// Hooks for DSPi and z4_mainloop.c

void __real_dcd_event_handler(dcd_event_t const *event, bool in_isr);
static uint32_t ep0_done_us;
static struct uh_setup setup_live[UH_SETUPS];

void __wrap_dcd_event_handler(dcd_event_t const *event, bool in_isr) {
    uh.posted[uh_kind(event->event_id)]++;
    if (event->event_id == DCD_EVENT_SETUP_RECEIVED) {
        for (unsigned i = 0; i + 1 < UH_SETUPS; i++) {
            setup_live[i] = setup_live[i + 1];
        }
        setup_live[UH_SETUPS - 1].ms = board_micros() / 1000u;
        setup_live[UH_SETUPS - 1].done = 0;
        memcpy(setup_live[UH_SETUPS - 1].pkt, &event->setup_received, 8);
        uh.setups_since_done++;
    } else if (event->event_id == DCD_EVENT_XFER_COMPLETE && (event->xfer_complete.ep_addr & 0x7Fu) == 0) {
        uh.ep0_done++;
        setup_live[UH_SETUPS - 1].done = 1;
        uh.setups_since_done = 0;
        ep0_done_us = board_micros();
    }
    __real_dcd_event_handler(event, in_isr);
}

static void usb_watch_snapshot(uint32_t trigger) {
    static const uint16_t regs[] = {0x008, 0x010, 0x014, 0x018, 0x01C, 0x804, 0x808, 0x818, 0x81C, 0x900, 0x908,
                                    0x910, 0xB00, 0xB08, 0xB10, 0x024, 0xB20, 0xB28, 0xB30, 0x920, 0x928, 0x940,
                                    0x948, 0x960, 0x02C, 0x000, 0xE00, 0x028};
    for (unsigned i = 0; i < 28; i++) {
        uh.snap[i] = DWC2_R(regs[i]);
    }
    uh.snap[28] = *(volatile uint32_t *)0x3F00B204u;    // ARM IRQ pending 1
    uh.snap[29] = *(volatile uint32_t *)0x3F00B210u;    // IRQ enable 1
    uint32_t cpsr;
    __asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
    uh.snap[30] = cpsr;
    uh.snap[31] = current_alt | current_rate << 8;
    memcpy(uh.setup, setup_live, sizeof(setup_live));
    uh.trigger = trigger;
    uh.trigger_ms = board_micros() / 1000u;
}

// Main loop, every 50 ms. Trigger 1: SETUPs keep arriving but EP0 completes nothing for 3 s. Trigger 2:
// the controller has an enabled interrupt (or RX FIFO data, even masked) pending and the IRQ handler
// has not run for 150 ms. Trigger 3, once armed: EP0 completes nothing for 8 s, whether or not SETUPs arrive.
// Either way it snapshots, then disconnects and reconnects so the host re-enumerates (bench diagnosis).
static void usb_watch(void) {
    static uint32_t last_us, last_irqs, pending_runs;
    uint32_t now = board_micros();
    if (now - last_us < 50000u) {
        return;
    }
    last_us = now;
    uint32_t sts = DWC2_R(0x014);
    uint32_t pend = (sts & DWC2_R(0x018)) | (sts & (1u << 4));    // RXFLVL counts even while masked
    pending_runs = (pend && uh.usb_irqs == last_irqs) ? pending_runs + 1u : 0;
    last_irqs = uh.usb_irqs;
    uint32_t trigger = 0;
    int32_t idle_us = (int32_t)(now - ep0_done_us);     // signed: an IRQ may have updated it since `now`
    if (uh.setups_since_done >= 3u && idle_us > 3000000) {
        trigger = 1;
    } else if (pending_runs >= 3u) {
        trigger = 2;
    } else if (uh.armed && idle_us > 8000000) {
        trigger = 3;
        uh.armed = 0;
    }
    if (!trigger) {
        return;
    }
    usb_watch_snapshot(trigger);
    uh.recoveries++;
    uh.setups_since_done = 0;
    ep0_done_us = now;
    pending_runs = 0;
    tud_disconnect();
    board_delay_us(200000);
    tud_connect();
}

void tud_event_hook_cb(uint8_t rhport, uint32_t eventid, bool in_isr) {
    (void)rhport;
    (void)in_isr;
    uh.queued[uh_kind(eventid)]++;
}

// main.c's pipeline_service_producer(): in the main loop (lock_gated) an external-clock I2S input delivers
// only while LOCKED, since before that it runs at a foreign rate; the settle loops take whatever comes.
static void service_producer(bool lock_gated) {
    static uint32_t last;
    uint32_t now = board_micros();
    uint32_t gap = now - last;
    if (last && gap > uh.gap_max_us) {
        uh.gap_max_us = gap;
        uh.gap_max_rate = current_rate;
        uh.gap_max_alt = current_alt;
    }
    uh.gaps_over_4ms += last && gap > 4000u;
    uh.tud_calls++;
    last = now;
    cpu_meter_enter();
    tud_task();
    z4_sync_core1_mode();   // before any block: a request just handled may have asked for Core 1
    consume_task();
    input_delivery_mark();
    if (input_source_is_spdif(active_input_source)) {
        spdif_input_poll();
    } else if (active_input_source == INPUT_SOURCE_I2S) {
        // The follower's lock as well: blocks delivered before it would run preset_loading out, and
        // the prefill handshake that starts the outputs and releases the DAC mute waits on both.
        if (!lock_gated || i2s_clock_mode != I2S_CLOCK_MODE_SLAVE ||
            (i2s_slave_get_state() == I2S_SLAVE_LOCKED && i2s_input_pi_follower_locked())) {
            i2s_input_poll();
        }
    } else if (active_input_source == INPUT_SOURCE_ADAT) {
        adat_input_poll();
    }
    siggen_pump();
    output_rings_idle_pump();
    notify_drain(0);
    cpu_meter_leave(CPU_METER_PRODUCER);
    cpu_meter_publish();    // after any block, whose own load figure DSPi has just written
}

void z4_service_producer(void) {
    service_producer(false);
}

bool z4_stream_running(void) {
    return consumer_running;
}

uint32_t z4_host_rate(void) {
    return current_rate;
}

// Replaces usb_audio.c's (renamed there): DSPi's version times UAC1 packets, which never arrive here.
// The test-signal pump asks it whether USB is already producing blocks.
bool usb_audio_stream_active(void) {
    return consumer_running && sync_started;
}

// ---------------------------------------------------------------------------------------------------

static void set_serial_string(void) {
    uint64_t serial = mbox_board_serial();
    if (serial == 0) {
        return;
    }
    for (int i = 0; i < 16; i++) {
        uint32_t nib = (uint32_t)(serial >> (60 - 4 * i)) & 0xFu;
        usb_serial_string[i] = (char)(nib < 10 ? '0' + nib : 'A' + nib - 10);
    }
    // DSPi's own copy, which its vendor requests report (RP2350: the chip ID).
    memcpy(usb_descriptor_str_serial, usb_serial_string, sizeof(usb_descriptor_str_serial));
}

int main(void) {
    clk_dump_boot_snapshot();   // clocks exactly as the firmware left them (PLLA plan phase 0)
#if BCM_VERSION == 2837
    mmu_enable();   // D-cache on: DSP data no longer comes from SDRAM on every access
    pmu_counters = pmu_init();
#endif
    // The VideoCore keeps the ARM at arm_freq_min (600 MHz on a Zero 2 W) until the OS asks for more,
    // as Linux's cpufreq does. Ask for the maximum; the firmware still throttles if it gets too hot.
    {
        uint32_t max_hz = mbox_clock_rate_max(VCMAILBOX_CLOCK_ARM);
        if (max_hz) {
            mbox_set_clock_rate(VCMAILBOX_CLOCK_ARM, max_hz);
        }
    }
    board_led_init();
    uart_init(BOARD_UART_CLOCK_HZ, BOARD_UART_BAUD);
    uart_puts("\nDSPi PiZero Z4 pipeline (" __DATE__ " " __TIME__ ")\n");
    set_serial_string();
    mbox_set_power_state(VCMAILBOX_DEVICE_USB_HCD, 1);
    uart_set_buffered(1);

    // Outputs first, with interrupts on for its composer, so DSPi's slots start on the SMI's frame clock.
    shim_smi_start();
    irq_global_enable();
    z4_dspi_init();

    irq_set_handler(IRQ_USB, usb_irq);
    tusb_rhport_init_t dev_init = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_HIGH};
    tusb_init(0, &dev_init);    // enables the USB IRQ in the interrupt controller
    irq_global_enable();

    uint32_t last_toggle = board_micros();
    int led_on = 1;
    for (;;) {
        sensors_refresh();
        pll_out_poll();
        pcm_rx_poll();
        i2s_input_pi_poll();
        pll_out_set_tracked(soft_vcxo_pi_tracking());
        if (pll_out_reselect()) {
            smi_out_reclock();      // 48 kHz moves between the integer row and C48 as trimming starts or stops
        }
        if (pll_out_watchdog() == PLL_WD_FALLBACK) {
            smi_out_set_source(SMI_SRC_PLLD);   // PLLA keeps being rewritten: stay on PLLD until reboot
        }
        usb_watch();
        if (spdif_monitor_request != 0xFFFFFFFFu) {
            if (!input_source_is_spdif(active_input_source)) {
                spdif_rx_pi_monitor(spdif_monitor_request != 0u);
            }
            spdif_monitor_request = 0xFFFFFFFFu;
        }
        if (smi_source_request != 0xFFFFFFFFu) {
            smi_out_set_source(smi_source_request);
            smi_source_request = 0xFFFFFFFFu;
        }
        service_producer(true);
        cpu_meter_enter();
        z4_dspi_service();
        cpu_meter_leave(CPU_METER_SERVICES);
        cpu_meter_tick();
        uart_poll();

        if (reboot_requested) {
            board_delay_us(20000);
            irq_global_disable();
            tud_disconnect();
            board_delay_us(50000);
            arm_loader_handoff();
            board_reboot();
        }

        uint32_t now = board_micros();
        if (now - last_toggle >= (current_alt ? 125000u : 500000u)) {
            last_toggle = now;
            led_on = !led_on;
            board_led_write(led_on);
        }
    }
}
