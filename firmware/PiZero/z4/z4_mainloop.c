// Port of the parts of DSPi's main.c that keep the pipeline and its configuration running: start-up,
// the muted pipeline reset, rate changes and the deferred work behind vendor SET requests. main.c
// itself is not compiled (it owns main() and the RP2350 hardware), so its handlers are carried over
// here calling the same DSPi functions, minus the input receivers, Core 1 and PIO output hardware.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "pico/audio_i2s_multi.h"
#include "pico/audio_spdif.h"
#include "pico/time.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

#include "config.h"
#include "adat_input.h"
#include "adat_output.h"
#include "audio_input.h"
#include "audio_pipeline.h"
#include "bg_job.h"
#include "bulk_params.h"
#include "control_surfaces.h"
#include "crossfeed.h"
#include "crossover.h"
#include "dac_hw_mute.h"
#include "dsp_pipeline.h"
#include "flash_storage.h"
#include "i2c_control.h"
#include "i2s_input.h"
#include "input_source_names.h"
#include "leveller.h"
#include "lg_sound_sync.h"
#include "limiter.h"
#include "loudness.h"
#include "notify.h"
#include "output_latency.h"
#include "pdm_generator.h"
#include "psybass.h"
#include "rta.h"
#include "siggen.h"
#include "spdif_input.h"
#include "subharm.h"
#include "tube.h"
#include "uart_control.h"
#include "upmix.h"
#include "usb_audio.h"
#include "usb_feedback_controller.h"

#include "z4.h"
#include "z4_globals.h"

// Defined in usb_audio.c and declared only where main.c uses them.
extern DacHwMuteConfig flash_set_dac_hw_mute_val;
extern volatile bool spdif_rx_pin_change_pending;

// ---------------------------------------------------------------------------------------------------
// Pipeline reset, from main.c (fade to silence, prepare, synchronized restart)

#define PIPELINE_FADE_REQUEST_MS  40u
#define PIPELINE_FADE_CAP_US      200000u
#define PIPELINE_FADE_STALE_US    50000u
#define PIPELINE_OP_MUTE_FLOOR_MS 24u
#define FLASH_WRITE_PREMUTE_MS     120u
#define FLASH_WRITE_FADE_SETTLE_US 30000u
#define LIMITER_ENGAGE_IDLE_US     50000u

static uint64_t fade_last_poll_us;
static uint64_t fade_start_us;
static bool fade_drain_armed;
static bool pipeline_wire_silent;

static uint32_t samples_for_duration_ms(uint32_t sample_rate_hz, uint32_t duration_ms) {
    uint64_t samples = ((uint64_t)sample_rate_hz * (uint64_t)duration_ms + 999u) / 1000u;
    if (samples < PRESET_MUTE_SAMPLES) {
        samples = PRESET_MUTE_SAMPLES;
    }
    if (samples > UINT32_MAX) {
        samples = UINT32_MAX;
    }
    return (uint32_t)samples;
}

static void reset_usb_feedback_loop(void) {
    fb_ctrl_reset(&fb_ctrl, nominal_feedback_10_14 << 2);
    feedback_10_14 = nominal_feedback_10_14;
}

static void pipeline_fade_reset(void) {
    fade_drain_armed = false;
    fade_last_poll_us = 0;
    fade_start_us = 0;
}

// Public in main.c (Control Surfaces' input level noun reads it). The receivers never lock here, so
// only USB and the test-signal generator can be live producers.
bool pipeline_producer_is_streaming(void) {
    if (active_input_source == INPUT_SOURCE_USB && sync_started) {
        return true;
    }
    if (input_source_is_spdif(active_input_source) && spdif_input_get_state() == SPDIF_INPUT_LOCKED) {
        return true;
    }
    if (active_input_source == INPUT_SOURCE_I2S && i2s_input_get_state() == I2S_INPUT_RUNNING) {
        return true;
    }
    if (active_input_source == INPUT_SOURCE_ADAT && adat_input_get_state() == ADAT_INPUT_LOCKED) {
        return true;
    }
    if (siggen_running && !input_source_change_pending && !output_type_switch_in_progress &&
        !(preset_loading && active_input_source != INPUT_SOURCE_USB)) {
        return true;
    }
    return false;
}

static bool pipeline_fade_to_silence_poll(void) {
    const uint64_t now = time_us_64();
    if (fade_last_poll_us != 0 && (now - fade_last_poll_us) > PIPELINE_FADE_STALE_US) {
        pipeline_fade_reset();
    }
    if (fade_start_us == 0) {
        fade_start_us = now;
    }
    fade_last_poll_us = now;
    if (!pipeline_mute_is_silent()) {
        pipeline_wire_silent = false;
    }
    uint32_t fs = audio_state.freq ? audio_state.freq : 48000u;
    pipeline_request_soft_mute(samples_for_duration_ms(fs, PIPELINE_FADE_REQUEST_MS));
    if (pipeline_wire_silent) {
        return true;
    }
    if (!pipeline_producer_is_streaming() || (now - fade_start_us) > PIPELINE_FADE_CAP_US) {
        pipeline_latch_mute_silence();
        pipeline_wire_silent = true;
        return true;
    }
    if (!fade_drain_armed) {
        if (!pipeline_mute_is_silent()) {
            return false;
        }
        output_rings_silence_mark_arm(pipeline_max_active_delay_samples());
        fade_drain_armed = true;
    }
    if (!output_rings_silence_mark_reached()) {
        return false;
    }
    pipeline_latch_mute_silence();
    pipeline_wire_silent = true;
    return true;
}

// Spins on the fade while the producer keeps delivering, as main.c does. Bounded by the fade cap.
static void pipeline_settle_to_silence(void) {
    while (!pipeline_fade_to_silence_poll()) {
        z4_service_producer();
    }
}

static void prepare_pipeline_reset(uint32_t mute_samples) {
    pipeline_settle_to_silence();
    uint32_t fs = audio_state.freq ? audio_state.freq : 48000u;
    uint32_t min_samples = samples_for_duration_ms(fs, PIPELINE_OP_MUTE_FLOOR_MS);
    if (mute_samples < min_samples) {
        mute_samples = min_samples;
    }
    preset_mute_counter = mute_samples;
    preset_loading = true;
    __dmb();
    pipeline_clear_soft_mute_request();
    pipeline_fade_reset();
    dac_hw_mute_assert();
    if (!dac_hw_mute_hold_elapsed()) {
        uint32_t floor_samples = samples_for_duration_ms(
            audio_state.freq, (uint32_t)dac_hw_mute_hold_ms() + PRESET_MUTE_HOLD_MARGIN_MS);
        if (preset_mute_counter < floor_samples) {
            preset_mute_counter = floor_samples;
        }
    }
}

static bool pipeline_reset_ready(void) {
    if (!pipeline_fade_to_silence_poll()) {
        return false;
    }
    dac_hw_mute_assert();
    return dac_hw_mute_hold_elapsed();
}

static void teardown_output_slot(int slot) {
    if (output_types[slot] == OUTPUT_TYPE_I2S) {
        audio_i2s_instance_t *inst = i2s_instance_ptrs[slot];
        if (inst && inst->ring) {
            audio_i2s_set_enabled(inst, false);
            audio_i2s_ring_reset(inst);
        }
    } else {
        audio_spdif_instance_t *inst = spdif_instance_ptrs[slot];
        if (inst && inst->ring) {
            audio_spdif_set_enabled(inst, false);
            audio_spdif_ring_reset(inst);
        }
    }
}

// main.c's complete_pipeline_reset(). The ADAT output restarts last, against slot 0, as in main.c; the
// SMI composer carries its stream.
static void complete_pipeline_reset(void) {
    audio_spdif_instance_t *spdif_sync[NUM_SPDIF_INSTANCES];
    audio_i2s_instance_t *i2s_sync[NUM_SPDIF_INSTANCES];
    uint spdif_count = 0, i2s_count = 0;
    for (int i = 0; i < NUM_SPDIF_INSTANCES; i++) {
        teardown_output_slot(i);
        if (output_types[i] == OUTPUT_TYPE_I2S) {
            if (i2s_instance_ptrs[i] && i2s_instance_ptrs[i]->ring) {
                i2s_sync[i2s_count++] = i2s_instance_ptrs[i];
            }
        } else if (spdif_instance_ptrs[i] && spdif_instance_ptrs[i]->ring) {
            spdif_sync[spdif_count++] = spdif_instance_ptrs[i];
        }
    }
    uint32_t flags = save_and_disable_interrupts();
    if (spdif_count) {
        audio_spdif_enable_sync(spdif_sync, spdif_count);
    }
    if (i2s_count) {
        audio_i2s_enable_sync(i2s_sync, i2s_count);
    }
    restore_interrupts(flags);
    reset_usb_feedback_loop();

    uint32_t fs = audio_state.freq ? audio_state.freq : 48000u;
    uint32_t dwell = output_ring_target_frames + samples_for_duration_ms(fs, (uint32_t)dac_hw_mute_release_ms());
    if (preset_loading) {
        if (preset_mute_counter < dwell) {
            preset_mute_counter = dwell;
        }
    } else {
        pipeline_request_soft_mute(dwell);
    }
    if (!(preset_loading && (active_input_source == INPUT_SOURCE_I2S || input_source_is_spdif(active_input_source) ||
                             active_input_source == INPUT_SOURCE_ADAT))) {
        dac_hw_mute_release();
    }
    i2s_input_resync();
    adat_output_resync();
}

// main.c's drain_and_disable_outputs() and enable_outputs_in_sync(), for the S/PDIF prefill: outputs stop
// and their rings empty, then all start together once slot 0 holds real audio at its target fill.
static void drain_and_disable_outputs(void) {
    for (int i = 0; i < NUM_SPDIF_INSTANCES; i++) {
        teardown_output_slot(i);
    }
    adat_output_stream_stop();
}

static void enable_outputs_in_sync(void) {
    audio_spdif_instance_t *spdif_sync[NUM_SPDIF_INSTANCES];
    audio_i2s_instance_t *i2s_sync[NUM_SPDIF_INSTANCES];
    uint spdif_count = 0, i2s_count = 0;
    uint32_t flags = save_and_disable_interrupts();
    for (int i = 0; i < NUM_SPDIF_INSTANCES; i++) {
        if (output_types[i] == OUTPUT_TYPE_I2S) {
            if (i2s_instance_ptrs[i] && i2s_instance_ptrs[i]->ring) {
                i2s_sync[i2s_count++] = i2s_instance_ptrs[i];
            }
        } else if (spdif_instance_ptrs[i] && spdif_instance_ptrs[i]->ring) {
            spdif_sync[spdif_count++] = spdif_instance_ptrs[i];
        }
    }
    if (spdif_count) {
        audio_spdif_enable_sync(spdif_sync, spdif_count);
    }
    if (i2s_count) {
        audio_i2s_enable_sync(i2s_sync, i2s_count);
    }
    restore_interrupts(flags);
    i2s_input_resync();
    adat_output_resync();
}

// main.c's S/PDIF part of input_service_handshakes(): on lock, drain the outputs and prefill them from
// the input; on loss, mute. The clock servo then runs every pass, gating its own tick.
static bool spdif_prefilling;

static void service_spdif_input(void) {
    if (!input_source_is_spdif(active_input_source)) {
        spdif_prefilling = false;
        return;
    }
    SpdifInputState rx_state = spdif_input_get_state();
    if (rx_state == SPDIF_INPUT_LOCKED && preset_loading && !spdif_prefilling && dac_hw_mute_hold_elapsed()) {
        spdif_input_check_rate_change();
        drain_and_disable_outputs();
        preset_loading = false;
        preset_mute_counter = 0;
        spdif_prefilling = true;
        spdif_input_discard_queued();
    }
    if (spdif_prefilling && slot_fill_at_target(0)) {
        enable_outputs_in_sync();
        spdif_prefilling = false;
        dac_hw_mute_release();
    }
    if (rx_state == SPDIF_INPUT_RELOCKING && !preset_loading && !spdif_prefilling) {
        prepare_pipeline_reset(PRESET_MUTE_SAMPLES);
        pipeline_reset_cpu_metering();
    }
    spdif_input_update_clock_servo();
}

// main.c's flash write bracket. The receivers are stand-ins, so there is nothing to suspend.
static void prepare_flash_write_operation(void) {
    z4_service_producer();
    prepare_pipeline_reset(samples_for_duration_ms(audio_state.freq, FLASH_WRITE_PREMUTE_MS));
    if (pipeline_producer_is_streaming()) {
        uint64_t start_us = time_us_64();
        while ((time_us_64() - start_us) < FLASH_WRITE_FADE_SETTLE_US || !dac_hw_mute_hold_elapsed()) {
            z4_service_producer();
        }
    }
    output_rings_stamp_silence_ahead();
}

static void complete_flash_write_operation_full(void) {
    if (input_source_is_spdif(active_input_source)) {
        reset_usb_feedback_loop();
        return;
    }
    complete_pipeline_reset();
}

// ---------------------------------------------------------------------------------------------------
// Output slot types and pins, from main.c's process_type_switches() and process_pin_changes()

// MCK runs only when locally clocked and something uses I2S (an output slot or the input), from main.c.
static void apply_mck_policy(void) {
    bool any_i2s = active_input_source == INPUT_SOURCE_I2S;
    for (int i = 0; i < NUM_SPDIF_INSTANCES; i++) {
        if (output_types[i] == OUTPUT_TYPE_I2S) {
            any_i2s = true;
        }
    }
    audio_i2s_mck_apply_state(i2s_mck_pin, !i2s_slave_mode_active() && any_i2s && i2s_mck_enabled,
                              audio_state.freq, i2s_mck_multiplier);
}

// Slot rebuild for a type change or a changed I2S clock role: the lowest-index I2S slot is clock master,
// unless I2S runs as clock slave. Pins and PDM only move as bookkeeping here.
static bool process_type_switches(uint8_t change_mask, const uint8_t new_types[]) {
    if (change_mask == 0) {
        return false;
    }
    uint8_t current_types[NUM_SPDIF_INSTANCES];
    uint8_t target_types[NUM_SPDIF_INSTANCES];
    memcpy(current_types, output_types, NUM_SPDIF_INSTANCES);
    memcpy(target_types, current_types, NUM_SPDIF_INSTANCES);
    for (int i = 0; i < NUM_SPDIF_INSTANCES; i++) {
        if ((change_mask & (1u << i)) && new_types[i] <= OUTPUT_TYPE_I2S) {
            target_types[i] = new_types[i];
        }
    }
    const bool want_extclk = i2s_slave_mode_active();
    int target_master_slot = -1;
    for (int i = 0; i < NUM_SPDIF_INSTANCES && !want_extclk; i++) {
        if (target_types[i] == OUTPUT_TYPE_I2S) {
            target_master_slot = i;
            break;
        }
    }
    bool any_change = false;
    bool i2s_rebuild[NUM_SPDIF_INSTANCES] = {0};
    for (int i = 0; i < NUM_SPDIF_INSTANCES; i++) {
        if (target_types[i] != current_types[i]) {
            any_change = true;
        } else if (target_types[i] == OUTPUT_TYPE_I2S) {
            audio_i2s_instance_t *inst = i2s_instance_ptrs[i];
            if (inst && inst->ring &&
                (inst->external_clock != want_extclk || inst->clock_master != (i == target_master_slot) ||
                 inst->clock_pin_base != i2s_effective_bck_pin())) {
                i2s_rebuild[i] = true;
                any_change = true;
            }
        }
    }
    if (!any_change) {
        return false;
    }

    pipeline_settle_to_silence();
    output_type_switch_in_progress = true;
    __dmb();
    prepare_pipeline_reset(PRESET_MUTE_SAMPLES);
    for (int i = 0; i < NUM_SPDIF_INSTANCES; i++) {
        bool had_i2s = current_types[i] == OUTPUT_TYPE_I2S;
        bool want_i2s = target_types[i] == OUTPUT_TYPE_I2S;
        if (had_i2s && (!want_i2s || i2s_rebuild[i])) {
            audio_i2s_teardown(i2s_instance_ptrs[i]);
        } else if (!had_i2s && want_i2s) {
            audio_spdif_teardown(spdif_instance_ptrs[i]);
        }
    }
    for (int i = 0; i < NUM_SPDIF_INSTANCES; i++) {
        bool had_i2s = current_types[i] == OUTPUT_TYPE_I2S;
        bool want_i2s = target_types[i] == OUTPUT_TYPE_I2S;
        if (want_i2s && (!had_i2s || i2s_rebuild[i])) {
            audio_i2s_config_t cfg = {
                .data_pin = output_pins[i],
                .clock_pin_base = i2s_effective_bck_pin(),
                .dma_channel = (uint8_t)i,
                .pio_sm = (uint8_t)i,
                .pio = PICO_AUDIO_SPDIF_PIO,
                .clock_master = i == target_master_slot,
                .external_clock = want_extclk,
                .ring_base = slot_ring_store[i],
            };
            audio_i2s_setup(i2s_instance_ptrs[i], output_rate_hz, &cfg);
        } else if (!want_i2s && had_i2s) {
            audio_spdif_config_t cfg = {
                .pin = output_pins[i],
                .dma_channel = (uint8_t)i,
                .pio_sm = (uint8_t)i,
                .pio = PICO_AUDIO_SPDIF_PIO,
                .ring_base = slot_ring_store[i],
            };
            audio_spdif_setup(spdif_instance_ptrs[i], output_rate_hz, &cfg);
            memset(i2s_instance_ptrs[i], 0, sizeof(audio_i2s_instance_t));
        }
    }
    // Default channel names follow the type, unless the user renamed the channel.
    for (int slot = 0; slot < NUM_SPDIF_INSTANCES; slot++) {
        if (current_types[slot] == target_types[slot]) {
            continue;
        }
        for (int side = 0; side < 2; side++) {
            int ch = CH_OUT_1 + slot * 2 + side;
            char old_default[PRESET_NAME_LEN];
            char new_default[PRESET_NAME_LEN];
            get_default_channel_name(ch, active_input_source, current_types, old_default);
            get_default_channel_name(ch, active_input_source, target_types, new_default);
            if (strcmp(old_default, new_default) == 0 || strcmp(channel_names[ch], old_default) != 0) {
                continue;
            }
            memcpy(channel_names[ch], new_default, PRESET_NAME_LEN);
            notify_param_write((uint16_t)(offsetof(WireBulkParams, channel_names.names) + ch * WIRE_NAME_LEN),
                               WIRE_NAME_LEN, channel_names[ch]);
        }
    }
    memcpy(output_types, target_types, NUM_SPDIF_INSTANCES);
    apply_mck_policy();
    complete_pipeline_reset();
    output_type_switch_in_progress = false;
    __dmb();
    return true;
}

static void rebuild_i2s_output_clocking(void) {
    uint8_t mask = 0;
    for (int i = 0; i < NUM_SPDIF_INSTANCES; i++) {
        if (output_types[i] == OUTPUT_TYPE_I2S) {
            mask |= (uint8_t)(1u << i);
        }
    }
    if (mask) {
        process_type_switches(mask, output_types);
    }
}

static bool apply_restored_outputs(const uint8_t old_types[]) {
    uint8_t new_types[NUM_SPDIF_INSTANCES];
    memcpy(new_types, output_types, NUM_SPDIF_INSTANCES);
    memcpy(output_types, old_types, NUM_SPDIF_INSTANCES);
    if (process_type_switches((uint8_t)((1u << NUM_SPDIF_INSTANCES) - 1u), new_types)) {
        return true;
    }
    memcpy(output_types, new_types, NUM_SPDIF_INSTANCES);
    apply_mck_policy();
    return false;
}

static void process_pin_changes(uint8_t mask) {
    if (mask == 0) {
        return;
    }
    prepare_pipeline_reset(PRESET_MUTE_SAMPLES);
    for (int i = 0; i < NUM_SPDIF_INSTANCES; i++) {
        teardown_output_slot(i);
    }
    adat_output_release_moving_pin();
    for (int i = 0; i < NUM_SPDIF_INSTANCES; i++) {
        if (!(mask & (1u << i))) {
            continue;
        }
        if (output_types[i] == OUTPUT_TYPE_I2S) {
            audio_i2s_instance_t *inst = i2s_instance_ptrs[i];
            if (inst && inst->ring && inst->data_pin != output_pins[i]) {
                audio_i2s_change_data_pin(inst, output_pins[i]);
            }
        } else {
            audio_spdif_instance_t *inst = spdif_instance_ptrs[i];
            if (inst && inst->ring && inst->pin != output_pins[i]) {
                audio_spdif_change_pin(inst, output_pins[i]);
            }
        }
    }
    complete_pipeline_reset();
}

// ---------------------------------------------------------------------------------------------------
// Rate changes, from main.c's perform_rate_change()

static void apply_nominal_output_rate(uint32_t rate) {
    for (int i = 0; i < NUM_SPDIF_INSTANCES; i++) {
        audio_spdif_instance_t *inst = spdif_instance_ptrs[i];
        if (output_types[i] != OUTPUT_TYPE_I2S && inst && inst->ring) {
            audio_spdif_apply_pio_frequency(inst, rate);
        }
    }
    if (i2s_mck_enabled) {
        audio_i2s_mck_update_frequency(rate, i2s_mck_multiplier);
    }
}

static void perform_rate_change(uint32_t new_freq) {
    switch (new_freq) {
    case 44100:
    case 48000:
    case 96000:
        break;
    default:
        new_freq = 44100;
    }
    prepare_pipeline_reset(PRESET_MUTE_SAMPLES);
    output_rate_hz = new_freq;
    output_latency_apply(new_freq, active_input_source);
    audio_state.freq = new_freq;
    rta_restart();
    sync_started = false;
    total_samples_produced = 0;
    nominal_feedback_10_14 = (uint32_t)(((uint64_t)new_freq << 14) / 1000u);
    feedback_10_14 = nominal_feedback_10_14;
    reset_usb_feedback_loop();

    uint32_t flags = save_and_disable_interrupts();
    dsp_recalculate_all_filters((float)new_freq);
    restore_interrupts(flags);
    loudness_recompute_pending = true;
    crossfeed_update_pending = true;
    leveller_update_pending = true;
    psybass_update_pending = true;
    subharm_update_pending = true;
    tube_update_pending = true;
    limiter_update_pending = true;
    upmix_update_pending = true;
    pdm_update_clock(new_freq);
    adat_output_on_rate_change(new_freq);
    adat_input_on_rate_change(new_freq);
    audio_i2s_update_all_frequencies(new_freq);
    apply_nominal_output_rate(new_freq);
    complete_pipeline_reset();
}

void z4_stream_restart(void) {
    uint32_t rate = z4_host_rate();
    if (active_input_source == INPUT_SOURCE_USB && audio_state.freq != rate) {
        perform_rate_change(rate);
    } else {
        prepare_pipeline_reset(PRESET_MUTE_SAMPLES);
        complete_pipeline_reset();
    }
    pipeline_reset_cpu_metering();
}

// ---------------------------------------------------------------------------------------------------
// Coefficient updates and the limiter's delay switch

static void limiter_engage_service(void) {
    static uint32_t last_count;
    static uint64_t last_packet_us, last_call_us;
    bool want = limiter_wants_engaged();
    uint64_t now = time_us_64();
    uint32_t count = limiter_packet_count();
    bool stalled_here = (now - last_call_us) > 10000u;
    last_call_us = now;
    if (count != last_count || want == limiter_is_engaged() || stalled_here) {
        last_count = count;
        last_packet_us = now;
    }
    if (want == limiter_is_engaged()) {
        return;
    }
    if (!pipeline_producer_is_streaming() || (now - last_packet_us) > LIMITER_ENGAGE_IDLE_US) {
        limiter_force_engage(want);
        return;
    }
    if (limiter_switch_ready()) {
        return;
    }
    uint32_t fs = audio_state.freq ? audio_state.freq : 48000u;
    pipeline_hold_soft_mute(samples_for_duration_ms(fs, PIPELINE_FADE_REQUEST_MS));
}

static void service_eq_update(void) {
    if (!eq_update_pending) {
        return;
    }
    EqParamPacket p = pending_packet;
    uint16_t qpx = pending_eq_qp_x512;
    eq_update_pending = false;

    bool is_xover = p.band >= XOVER_BAND_BASE && p.band < (XOVER_BAND_BASE + MAX_XOVER_BANDS);
    uint8_t local = is_xover ? (uint8_t)(p.band - XOVER_BAND_BASE) : p.band;
    WireBandParams wbp;
    memset(&wbp, 0, sizeof(wbp));
    wbp.type = (uint8_t)p.type;
    wbp.bypass = (p.bypass == 1) ? 1 : 0;
    wbp.freq = p.freq;
    wbp.q = p.Q;
    wbp.gain_db = p.gain_db;
    if (is_xover) {
        xover_recipes[p.channel][local] = p;
        uint16_t off = (uint16_t)(offsetof(WireBulkParams, crossovers) + offsetof(WireCrossoverConfig, bands) +
                                  ((uint16_t)p.channel * WIRE_MAX_XOVER_BANDS + local) * sizeof(WireBandParams));
        notify_param_write(off, sizeof(WireBandParams), &wbp);
    } else {
        peq_qp_x512[p.channel][p.band] = qpx;
        filter_recipes[p.channel][p.band] = p;
        if (p.type == FILTER_LINKWITZ_TRANSFORM) {
            wbp.reserved[0] = (uint8_t)(qpx & 0xFF);
            wbp.reserved[1] = (uint8_t)((qpx >> 8) & 0xFF);
        }
        uint16_t off = (uint16_t)(offsetof(WireBulkParams, eq) +
                                  ((uint16_t)p.channel * WIRE_MAX_BANDS + p.band) * sizeof(WireBandParams));
        notify_param_write(off, sizeof(WireBandParams), &wbp);
    }

    uint32_t flags = save_and_disable_interrupts();
    if (is_xover) {
        if (p.channel >= CH_OUT_1) {
            uint8_t out = (uint8_t)(p.channel - CH_OUT_1);
            xover_design_filter(&p, &xover_filters[out][local], (float)audio_state.freq);
            xover_update_output_bypass(out);
        }
    } else {
        dsp_compute_coefficients(&p, &filters[p.channel][p.band], (float)audio_state.freq);
        bool all_bypassed = true;
        for (int b = 0; b < channel_band_counts[p.channel]; b++) {
            if (!filters[p.channel][b].bypass) {
                all_bypassed = false;
                break;
            }
        }
        channel_bypassed[p.channel] = all_bypassed;
    }
    restore_interrupts(flags);
}

static void service_coefficients(void) {
    if (loudness_recompute_pending) {
        loudness_recompute_pending = false;
        loudness_recompute_begin(loudness_ref_spl, loudness_intensity_pct, (float)audio_state.freq);
    }
    if (loudness_recompute_take_done() && loudness_enabled && loudness_active_table) {
        uint8_t idx = effective_vol_index;
        if (idx > CENTER_VOLUME_INDEX) {
            idx = CENTER_VOLUME_INDEX;
        }
        current_loudness_coeffs = loudness_active_table[idx];
    }
    if (crossfeed_update_pending) {
        crossfeed_update_pending = false;
        crossfeed_apply_config((const CrossfeedConfig *)&crossfeed_config, (float)audio_state.freq);
    }
    if (psybass_update_pending) {
        psybass_update_pending = false;
        psybass_apply_config((const PsybassConfig *)&psybass_config, (float)audio_state.freq);
    }
    if (subharm_update_pending) {
        subharm_update_pending = false;
        subharm_apply_config((const SubharmConfig *)&subharm_config, (float)audio_state.freq);
    }
    if (tube_update_pending) {
        tube_update_pending = false;
        tube_apply_config((const TubeConfig *)&tube_config, (float)audio_state.freq);
    }
    if (limiter_update_pending) {
        limiter_update_pending = false;
        limiter_apply_config((float)audio_state.freq);
    }
    limiter_engage_service();
    if (upmix_update_pending) {
        upmix_update_pending = false;
        upmix_apply_config((const UpmixConfig *)&upmix_config, (float)audio_state.freq);
    }
    if (leveller_update_pending) {
        leveller_update_pending = false;
        leveller_compute_coefficients(&leveller_coeffs, (const LevellerConfig *)&leveller_config,
                                      (float)audio_state.freq);
        if (leveller_reset_pending) {
            leveller_reset_pending = false;
            leveller_reset_state(&leveller_state);
        }
        leveller_bypassed = !leveller_config.enabled;
    }
}

// Core 1 does not exist here: every output runs in the pipeline's single-core path, PDM included
// (its samples queue for a Core 1 that never drains them).
static void sync_core1_mode(void) {
    if (core1_mode != CORE1_MODE_IDLE) {
        core1_mode = CORE1_MODE_IDLE;
    }
}

// ---------------------------------------------------------------------------------------------------
// Deferred directory writes, control interfaces, Control Surfaces and DAC mute (main.c order)

static void service_directory_writes(void) {
    if (flash_set_name_pending) {
        char name[PRESET_NAME_LEN];
        uint32_t f = save_and_disable_interrupts();
        uint8_t slot = flash_set_name_slot;
        memcpy(name, flash_set_name_buf, PRESET_NAME_LEN);
        flash_set_name_pending = false;
        restore_interrupts(f);
        prepare_flash_write_operation();
        (void)preset_set_name(slot, name);
        complete_flash_write_operation_full();
    }
    if (flash_set_startup_pending) {
        uint32_t f = save_and_disable_interrupts();
        uint8_t mode = flash_set_startup_mode;
        uint8_t slot = flash_set_startup_slot;
        flash_set_startup_pending = false;
        restore_interrupts(f);
        prepare_flash_write_operation();
        (void)preset_set_startup(mode, slot);
        complete_flash_write_operation_full();
    }
    if (input_source_names_pending()) {
        prepare_flash_write_operation();
        input_source_names_apply_pending();
        complete_flash_write_operation_full();
    }
    if (flash_set_output_latency_pending) {
        uint8_t id = flash_set_output_latency_id;
        prepare_flash_write_operation();
        preset_set_output_latency(id);
        output_latency_request(id);
        output_latency_apply(audio_state.freq, active_input_source);
        dsp_update_delay_samples((float)audio_state.freq);
        complete_flash_write_operation_full();
        uint32_t f = save_and_disable_interrupts();
        if (flash_set_output_latency_id == id) {
            flash_set_output_latency_pending = false;
        }
        restore_interrupts(f);
    }
    if (flash_set_output_config_mode_pending) {
        uint32_t f = save_and_disable_interrupts();
        uint8_t val = flash_set_output_config_mode_val;
        flash_set_output_config_mode_pending = false;
        restore_interrupts(f);
        prepare_flash_write_operation();
        preset_set_output_config_mode(val);
        complete_flash_write_operation_full();
    }
    if (flash_save_output_config_pending) {
        uint32_t f = save_and_disable_interrupts();
        flash_save_output_config_pending = false;
        restore_interrupts(f);
        prepare_flash_write_operation();
        preset_save_output_config();
        complete_flash_write_operation_full();
    }
    if (flash_set_master_volume_mode_pending) {
        uint32_t f = save_and_disable_interrupts();
        uint8_t val = flash_set_master_volume_mode_val;
        flash_set_master_volume_mode_pending = false;
        restore_interrupts(f);
        prepare_flash_write_operation();
        preset_set_master_volume_mode(val);
        complete_flash_write_operation_full();
    }
    if (flash_save_master_volume_pending) {
        uint32_t f = save_and_disable_interrupts();
        flash_save_master_volume_pending = false;
        restore_interrupts(f);
        prepare_flash_write_operation();
        preset_save_master_volume();
        complete_flash_write_operation_full();
    }
}

// UART and I2C control run DSPi's own apply and persist. The peripherals are stand-ins, so a config
// that validates is reported live but no byte ever moves (uart_ctrl_poll and i2c_ctrl_poll are not run).
static void service_ctrl_interfaces(void) {
    if (ctrl_set_uart_pending) {
        UartCtrlConfig cfg;
        uint32_t f = save_and_disable_interrupts();
        memcpy(&cfg, (const void *)&ctrl_set_uart_val, sizeof(cfg));
        ctrl_set_uart_pending = false;
        restore_interrupts(f);
        uint8_t status = uart_ctrl_apply(&cfg);
        ctrl_uart_last_status = status;
        if (status == PIN_CONFIG_SUCCESS) {
            prepare_flash_write_operation();
            preset_set_ctrl_iface(&cfg, NULL);
            complete_flash_write_operation_full();
        }
    }
    if (ctrl_set_i2c_pending) {
        I2cCtrlConfig cfg;
        uint32_t f = save_and_disable_interrupts();
        memcpy(&cfg, (const void *)&ctrl_set_i2c_val, sizeof(cfg));
        ctrl_set_i2c_pending = false;
        restore_interrupts(f);
        uint8_t status = i2c_ctrl_apply(&cfg);
        ctrl_i2c_last_status = status;
        if (status == PIN_CONFIG_SUCCESS) {
            prepare_flash_write_operation();
            preset_set_ctrl_iface(NULL, &cfg);
            complete_flash_write_operation_full();
        }
    }
}

// Control Surfaces configuration is applied, saved and reverted by DSPi's own code, so Console sees
// its edits round-trip. control_surfaces_tick() is not run: the GPIO, ADC and display are stand-ins
// whose readings would fire bindings.
static void service_control_surfaces(void) {
    if (control_surfaces_busy()) {
        return;
    }
    if (cs_set_binding_pending) {
        CsBinding b;
        uint32_t f = save_and_disable_interrupts();
        memcpy(&b, (const void *)&cs_set_binding_val, sizeof(b));
        uint8_t slot = cs_set_binding_slot;
        cs_set_binding_pending = false;
        restore_interrupts(f);
        uint8_t status = control_surfaces_apply_binding(slot, &b);
        cs_last_status = status;
        cs_last_slot = slot;
        if (status == PIN_CONFIG_SUCCESS) {
            control_surfaces_set_dirty(true);
        }
    }
    if (cs_set_name_pending) {
        char name[CS_NAME_LEN];
        uint32_t f = save_and_disable_interrupts();
        memcpy(name, cs_set_name_val, sizeof(name));
        uint8_t slot = cs_set_name_slot;
        cs_set_name_pending = false;
        restore_interrupts(f);
        uint8_t status = control_surfaces_apply_name(slot, name);
        cs_last_status = status;
        cs_last_slot = slot;
        if (status == PIN_CONFIG_SUCCESS) {
            control_surfaces_set_dirty(true);
        }
    }
    if (cs_set_ir_cmd_pending) {
        IrCommand c;
        uint32_t f = save_and_disable_interrupts();
        memcpy(&c, (const void *)&cs_set_ir_cmd_val, sizeof(c));
        uint8_t slot = cs_set_ir_cmd_slot;
        cs_set_ir_cmd_pending = false;
        restore_interrupts(f);
        uint8_t status = control_surfaces_apply_ir_cmd(slot, &c);
        cs_last_status = status;
        cs_last_slot = 0x80 | slot;
        if (status == PIN_CONFIG_SUCCESS) {
            control_surfaces_set_dirty(true);
        }
    }
    if (cs_set_group_pending) {
        CsGroup g;
        uint32_t f = save_and_disable_interrupts();
        memcpy(&g, (const void *)&cs_set_group_val, sizeof(g));
        uint8_t slot = cs_set_group_slot;
        cs_set_group_pending = false;
        restore_interrupts(f);
        uint8_t status = control_surfaces_apply_group(slot, &g);
        cs_last_status = status;
        cs_last_slot = 0x40 | slot;
        if (status == PIN_CONFIG_SUCCESS) {
            control_surfaces_set_dirty(true);
        }
    }
    if (cs_set_macro_hdr_pending) {
        CsMacroHeaderWire h;
        uint32_t f = save_and_disable_interrupts();
        memcpy(&h, (const void *)&cs_set_macro_hdr_val, sizeof(h));
        uint8_t slot = cs_set_macro_hdr_slot;
        cs_set_macro_hdr_pending = false;
        restore_interrupts(f);
        uint8_t status = control_surfaces_apply_macro_header(slot, &h);
        cs_last_status = status;
        cs_last_slot = 0x60 | slot;
        if (status == PIN_CONFIG_SUCCESS) {
            control_surfaces_set_dirty(true);
        }
    }
    if (cs_set_macro_step_pending) {
        CsMacroStep s;
        uint32_t f = save_and_disable_interrupts();
        memcpy(&s, (const void *)&cs_set_macro_step_val, sizeof(s));
        uint8_t slot = cs_set_macro_step_slot;
        uint8_t step = cs_set_macro_step_idx;
        cs_set_macro_step_pending = false;
        restore_interrupts(f);
        uint8_t status = control_surfaces_apply_macro_step(slot, step, &s);
        cs_last_status = status;
        cs_last_slot = 0x60 | slot;
        if (status == PIN_CONFIG_SUCCESS) {
            control_surfaces_set_dirty(true);
        }
    }
    if (cs_set_disp_cfg_pending) {
        CsDisplayCfg c;
        uint32_t f = save_and_disable_interrupts();
        memcpy(&c, (const void *)&cs_set_disp_cfg_val, sizeof(c));
        cs_set_disp_cfg_pending = false;
        restore_interrupts(f);
        uint8_t status = control_surfaces_apply_display_cfg(&c);
        cs_last_status = status;
        cs_last_slot = 0x50;
        if (status == PIN_CONFIG_SUCCESS) {
            control_surfaces_set_dirty(true);
        }
    }
    if (cs_set_disp_page_pending) {
        CsDisplayPage p;
        uint32_t f = save_and_disable_interrupts();
        memcpy(&p, (const void *)&cs_set_disp_page_val, sizeof(p));
        uint8_t slot = cs_set_disp_page_slot;
        cs_set_disp_page_pending = false;
        restore_interrupts(f);
        uint8_t status = control_surfaces_apply_display_page(slot, &p);
        cs_last_status = status;
        cs_last_slot = 0x50 | slot;
        if (status == PIN_CONFIG_SUCCESS) {
            control_surfaces_set_dirty(true);
        }
    }
    if (cs_save_pending) {
        uint32_t f = save_and_disable_interrupts();
        cs_save_pending = false;
        restore_interrupts(f);
        control_surfaces_aux_prepare_save();
        prepare_flash_write_operation();
        uint8_t rc = preset_set_cs_all(control_surfaces_config(), control_surfaces_ir_config(),
                                       control_surfaces_names(), control_surfaces_group_config(),
                                       control_surfaces_macro_config(), control_surfaces_display_flash());
        complete_flash_write_operation_full();
        cs_last_status = (rc == PRESET_OK) ? PIN_CONFIG_SUCCESS : CS_STATUS_FLASH_ERROR;
        cs_last_slot = 0xFF;
        if (rc == PRESET_OK) {
            control_surfaces_set_dirty(false);
        }
    }
    if (cs_revert_pending) {
        uint32_t f = save_and_disable_interrupts();
        cs_revert_pending = false;
        restore_interrupts(f);
        control_surfaces_revert_begin();
    }
}

// DAC mute config and test pulse: DSPi's own code, driving a stand-in GPIO.
static void service_dac_hw_mute(void) {
    if (flash_set_dac_hw_mute_pending) {
        DacHwMuteConfig hw;
        uint32_t f = save_and_disable_interrupts();
        memcpy(&hw, (const void *)&flash_set_dac_hw_mute_val, sizeof(hw));
        flash_set_dac_hw_mute_pending = false;
        restore_interrupts(f);
        prepare_flash_write_operation();
        (void)dac_hw_mute_set_config(&hw);
        complete_flash_write_operation_full();
    }
    if (dac_hw_mute_test_pending) {
        uint32_t f = save_and_disable_interrupts();
        dac_hw_mute_test_pending = false;
        restore_interrupts(f);
        (void)dac_hw_mute_test_start();
    }
}

// ---------------------------------------------------------------------------------------------------
// Presets, factory reset, output config and bulk parameters (main.c order)

static void service_presets(void) {
    if (preset_load_pending && pipeline_reset_ready()) {
        preset_load_pending = false;
        __dmb();
        siggen_stop_immediate(SIGGEN_STOP_PRESET);
        rta_restart();
        uint8_t old_types[NUM_SPDIF_INSTANCES];
        memcpy(old_types, output_types, NUM_SPDIF_INSTANCES);
        prepare_pipeline_reset(PRESET_MUTE_SAMPLES);
        preset_load(pending_preset_load_slot);
        if (!apply_restored_outputs(old_types)) {
            if (input_source_is_spdif(active_input_source)) {
                reset_usb_feedback_loop();
            } else {
                complete_pipeline_reset();
            }
        }
        i2s_rx_pin_change_pending = false;
        i2s_input_restart_pending = false;
    }
    if (save_params_pending) {
        save_params_pending = false;
        __dmb();
        prepare_flash_write_operation();
        (void)flash_save_params();
        complete_flash_write_operation_full();
    }
    if (preset_save_pending) {
        preset_save_pending = false;
        __dmb();
        prepare_flash_write_operation();
        (void)preset_save(pending_preset_save_slot);
        complete_flash_write_operation_full();
    }
    if (preset_delete_mask) {
        uint32_t flags = save_and_disable_interrupts();
        uint16_t mask = preset_delete_mask;
        preset_delete_mask = 0;
        restore_interrupts(flags);
        uint8_t old_types[NUM_SPDIF_INSTANCES];
        memcpy(old_types, output_types, NUM_SPDIF_INSTANCES);
        prepare_flash_write_operation();
        for (int slot = 0; slot < PRESET_SLOTS; slot++) {
            if (mask & (1u << slot)) {
                preset_delete(slot);
            }
        }
        if (!apply_restored_outputs(old_types)) {
            complete_flash_write_operation_full();
        }
    }
    if (factory_reset_pending && pipeline_reset_ready()) {
        factory_reset_pending = false;
        __dmb();
        siggen_stop_immediate(SIGGEN_STOP_PRESET);
        rta_restart();
        uint8_t old_types[NUM_SPDIF_INSTANCES];
        memcpy(old_types, output_types, NUM_SPDIF_INSTANCES);
        prepare_pipeline_reset(PRESET_MUTE_SAMPLES);
        flash_factory_reset();
        dsp_recalculate_all_filters((float)audio_state.freq);
        dsp_update_delay_samples((float)audio_state.freq);
        loudness_recompute_pending = true;
        crossfeed_update_pending = true;
        memset(delay_lines, 0, sizeof(delay_lines));
        limiter_reset_all();
        sync_core1_mode();
        if (!apply_restored_outputs(old_types)) {
            complete_pipeline_reset();
        }
        i2s_rx_pin_change_pending = false;
        i2s_input_restart_pending = false;
    }
}

static void service_outputs_and_bulk(void) {
    if (output_type_change_mask && pipeline_reset_ready()) {
        uint8_t mask = output_type_change_mask;
        output_type_change_mask = 0;
        __dmb();
        process_type_switches(mask, (const uint8_t *)pending_output_types);
    }
    // ADAT: silence top-up every pass, and a configuration change through a muted restart, as in main.c.
    adat_output_task();
    if (adat_output_config_dirty && pipeline_reset_ready()) {
        prepare_pipeline_reset(PRESET_MUTE_SAMPLES);
        complete_pipeline_reset();
    }
    if (output_pin_change_mask && pipeline_reset_ready()) {
        uint8_t mask = output_pin_change_mask;
        output_pin_change_mask = 0;
        __dmb();
        process_pin_changes(mask);
    }
    if (bulk_params_pending && pipeline_reset_ready()) {
        bulk_params_pending = false;
        uint8_t old_types[NUM_SPDIF_INSTANCES];
        memcpy(old_types, output_types, NUM_SPDIF_INSTANCES);
        prepare_pipeline_reset(PRESET_MUTE_SAMPLES);
        uint16_t occ;
        uint8_t startup_mode, default_slot, last_active, oc_mode, mv_mode;
        preset_get_directory(&occ, &startup_mode, &default_slot, &last_active, &oc_mode, &mv_mode);
        int err = bulk_params_apply((const WireBulkParams *)bulk_param_buf,
                                    oc_mode == OUTPUT_CONFIG_MODE_WITH_PRESET);
        if (err == 0) {
            float rate = (float)audio_state.freq;
            dsp_recalculate_all_filters(rate);
            dsp_update_delay_samples(rate);
            sync_core1_mode();
            if (!apply_restored_outputs(old_types)) {
                if (input_source_is_spdif(active_input_source)) {
                    reset_usb_feedback_loop();
                } else {
                    complete_pipeline_reset();
                }
            }
        }
        i2s_rx_pin_change_pending = false;
        i2s_input_restart_pending = false;
    }
}

// ---------------------------------------------------------------------------------------------------
// Input configuration. The receivers are stand-ins (dspi_standins.c): a selected S/PDIF or ADAT
// input waits for lock forever and I2S stays inactive, so only USB and the generator produce audio.

static void service_input_config(void) {
    if (i2s_clock_mode_change_pending) {
        i2s_clock_mode_change_pending = false;
        __dmb();
        if (i2s_clock_mode != pending_i2s_clock_mode) {
            bool live = active_input_source == INPUT_SOURCE_I2S;
            i2s_clock_mode = pending_i2s_clock_mode;
            if (live) {
                rebuild_i2s_output_clocking();
            }
            apply_mck_policy();
            uint8_t wire_mode = i2s_clock_mode;
            notify_param_write(offsetof(WireBulkParams, input_config.i2s_clock_mode), 1, &wire_mode);
        }
    }
    if (adat_clock_mode_change_pending) {
        adat_clock_mode_change_pending = false;
        __dmb();
        if (pending_adat_clock_mode != adat_clock_mode) {
            adat_clock_mode = pending_adat_clock_mode;
            uint8_t wire_mode_p1 = (uint8_t)(adat_clock_mode + 1);
            notify_param_write(offsetof(WireBulkParams, input_config.adat_clock_mode_p1), 1, &wire_mode_p1);
        }
    }

    if (input_source_change_pending) {
        uint8_t new_source = pending_input_source;
        uint8_t old_source = active_input_source;
        bool real_switch = (new_source != old_source) && input_source_selectable(new_source);
        if (!real_switch) {
            input_source_change_pending = false;
            __dmb();
        } else if (pipeline_reset_ready()) {
            input_source_change_pending = false;
            __dmb();
            prepare_pipeline_reset(PRESET_MUTE_SAMPLES);
            if (input_source_is_spdif(old_source)) {
                spdif_input_stop();
            } else if (old_source == INPUT_SOURCE_I2S) {
                i2s_input_stop();
            } else if (old_source == INPUT_SOURCE_ADAT) {
                adat_input_stop();
            }
            if (old_source != INPUT_SOURCE_USB) {
                leveller_reset_pending = true;
                pipeline_reset_cpu_metering();
            }
            // Default input channel names follow the source, unless the user renamed the channel.
            for (int ch = 0; ch < NUM_INPUT_CHANNELS; ch++) {
                char old_default[PRESET_NAME_LEN];
                char new_default[PRESET_NAME_LEN];
                get_default_channel_name(ch, old_source, output_types, old_default);
                get_default_channel_name(ch, new_source, output_types, new_default);
                if (strcmp(old_default, new_default) == 0 || strcmp(channel_names[ch], old_default) != 0) {
                    continue;
                }
                memcpy(channel_names[ch], new_default, PRESET_NAME_LEN);
                notify_param_write((uint16_t)(offsetof(WireBulkParams, channel_names.names) + ch * WIRE_NAME_LEN),
                                   WIRE_NAME_LEN, channel_names[ch]);
            }
            active_input_source = new_source;
            rta_restart();
            output_latency_apply(audio_state.freq, new_source);
            dsp_update_delay_samples((float)audio_state.freq);
            rate_change_pending = false;
            lg_sound_sync_on_input_source_change(active_input_source);
            if (i2s_clock_mode == I2S_CLOCK_MODE_SLAVE &&
                (old_source == INPUT_SOURCE_I2S || new_source == INPUT_SOURCE_I2S)) {
                rebuild_i2s_output_clocking();
            }
            if (input_source_is_spdif(new_source)) {
                spdif_input_start();
            } else if (new_source == INPUT_SOURCE_I2S) {
                if (i2s_clock_mode != I2S_CLOCK_MODE_SLAVE && i2s_input_rate != audio_state.freq) {
                    perform_rate_change(i2s_input_rate);
                }
                apply_mck_policy();     // main.c's i2s_input_bringup_prefill() starts MCK for a master input
                i2s_input_start(true);
            } else if (new_source == INPUT_SOURCE_ADAT) {
                if (adat_clock_mode == ADAT_CLOCK_MODE_MASTER && i2s_input_rate != audio_state.freq) {
                    perform_rate_change(i2s_input_rate);
                }
                adat_input_start();
            } else {
                complete_pipeline_reset();
                audio_thaw_host_volume();
                if (z4_host_rate() != audio_state.freq) {
                    pending_rate = z4_host_rate();
                    __dmb();
                    rate_change_pending = true;
                }
            }
            uint8_t wire_src = (uint8_t)active_input_source;
            notify_param_write(offsetof(WireBulkParams, input_config.input_source), 1, &wire_src);
            notify_push_input_format(active_input_channel_count());
        }
    }

    // Receiver pin moves and restarts: nothing is running to move.
    spdif_rx_pin_change_pending = false;
    adat_input_restart_pending = false;
    i2s_rx_pin_change_pending = false;
    i2s_input_restart_pending = false;
}

// ---------------------------------------------------------------------------------------------------

void z4_dspi_service(void) {
    static bool busy;
    if (busy) {
        return;     // a settle loop is running the producer from inside a handler
    }
    busy = true;

    dac_hw_mute_tick();
    service_spdif_input();
    lg_sound_sync_tick();
    siggen_service();
    rta_service();
    bg_run_one();

    service_directory_writes();
    service_ctrl_interfaces();
    service_control_surfaces();
    service_dac_hw_mute();
    service_eq_update();

    // Rate changes from DSPi itself (I2S or ADAT rate, a switch back to USB). The USB host's own rate
    // is applied when its stream starts (z4_stream_restart).
    if (rate_change_pending && pipeline_reset_ready()) {
        uint32_t r = pending_rate;
        rate_change_pending = false;
        if (active_input_source == INPUT_SOURCE_USB) {
            r = z4_host_rate();
        }
        if (r != audio_state.freq) {
            perform_rate_change(r);
        }
    }

    service_coefficients();
    service_presets();
    service_outputs_and_bulk();
    service_input_config();
    sync_core1_mode();

    busy = false;
}

// main.c's core0_init() in the same order, without the RP2350 clocks, regulator, soft VCXO, flash
// clock divider, stall alarm, clock diagnostics, PDM hardware and Core 1. The preset store starts
// blank each boot (RAM-backed flash), so preset_boot_load() writes a fresh directory and applies
// factory defaults, as on a new RP2350 board.
void z4_dspi_init(void) {
    pico_shim_flash_init();
    output_latency_apply(48000u, INPUT_SOURCE_USB);
    usb_sound_card_init();
    adat_output_init();
    adat_input_init();
    fb_ctrl_init(&fb_ctrl);
    nominal_feedback_10_14 = (uint32_t)(((uint64_t)audio_state.freq << 14) / 1000u);
    feedback_10_14 = nominal_feedback_10_14;

    preset_boot_load();
    output_latency_request(preset_get_output_latency());
    output_latency_apply(48000u, active_input_source);
    uint32_t flags = save_and_disable_interrupts();
    dsp_recalculate_all_filters(48000.0f);
    dsp_update_delay_samples(48000.0f);
    restore_interrupts(flags);
    {
        uint8_t boot_live_types[NUM_SPDIF_INSTANCES];
        for (int i = 0; i < NUM_SPDIF_INSTANCES; i++) {
            boot_live_types[i] = OUTPUT_TYPE_SPDIF;
        }
        apply_restored_outputs(boot_live_types);
    }
    {
        DacHwMuteConfig hw;
        preset_get_dac_hw_mute(&hw);
        dac_hw_mute_init(&hw);
    }
    adat_output_resync();    // if the loaded configuration enables it

    loudness_recompute_table(loudness_ref_spl, loudness_intensity_pct, 48000.0f);
    if (loudness_enabled && loudness_active_table) {
        current_loudness_coeffs = loudness_active_table[effective_vol_index];
    }
    leveller_compute_coefficients(&leveller_coeffs, (const LevellerConfig *)&leveller_config, 48000.0f);
    leveller_reset_state(&leveller_state);
    leveller_bypassed = !leveller_config.enabled;
    psybass_apply_config((const PsybassConfig *)&psybass_config, 48000.0f);
    subharm_apply_config((const SubharmConfig *)&subharm_config, 48000.0f);
    tube_apply_config((const TubeConfig *)&tube_config, 48000.0f);
    limiter_apply_config(48000.0f);
    upmix_apply_config((const UpmixConfig *)&upmix_config, 48000.0f);

    core1_mode = CORE1_MODE_IDLE;
    pdm_set_enabled(false);
    spdif_input_init();
    i2s_input_init();
    lg_sound_sync_init();
    if (input_source_is_spdif(active_input_source)) {
        prepare_pipeline_reset(PRESET_MUTE_SAMPLES);
        spdif_input_start();
    } else if (active_input_source == INPUT_SOURCE_ADAT) {
        prepare_pipeline_reset(PRESET_MUTE_SAMPLES);
        adat_input_start();
    }

    notify_init();
    {
        UartCtrlConfig ucfg;
        I2cCtrlConfig icfg;
        preset_get_ctrl_iface(&ucfg, &icfg);
        ctrl_uart_last_status = uart_ctrl_init(&ucfg);
        ctrl_i2c_last_status = i2c_ctrl_init(&icfg);
    }
    control_surfaces_init();
    rta_init();

    // usb_sound_card_init() queues a rate change for main.c's loop. The pipeline starts at the UAC2
    // clock's rate instead (48 kHz until the host sets it).
    rate_change_pending = false;
    if (active_input_source == INPUT_SOURCE_USB && audio_state.freq != z4_host_rate()) {
        perform_rate_change(z4_host_rate());
    }
}
