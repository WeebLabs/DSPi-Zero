// Stand-ins for DSPi modules that drive RP2350-only hardware: flash_clkdiv.c (QSPI flash access), and the
// i2s_input.c and adat_input.c PIO receivers. Each keeps the module's public API. The receivers start
// and stop but never see a signal, as on an RP2350 DSPi with nothing plugged in. spdif_input.c is not
// stood in for: it is compiled, on spdif_rx_pi.c.

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "hardware/flash.h"

#include "config.h"
#include "adat_input.h"
#include "audio_input.h"
#include "flash_clkdiv.h"
#include "i2s_input.h"
#include "spdif_input.h"
#include "usb_audio.h"

// ---------------------------------------------------------------------------------------------------
// flash_clkdiv.c: the RAM-backed flash in pico_shim.c has no clock divider to keep

void dspi_flash_apply_clkdiv(void) {}

void dspi_flash_range_erase(uint32_t flash_offs, size_t count) {
    flash_range_erase(flash_offs, count);
}

void dspi_flash_range_program(uint32_t flash_offs, const uint8_t *data, size_t count) {
    flash_range_program(flash_offs, data, count);
}

// ---------------------------------------------------------------------------------------------------
// i2s_input.c: the input never runs, so it reports inactive in either clock role

void i2s_input_init(void) {}

void i2s_input_start(bool clock_master) {
    (void)clock_master;
}

void i2s_input_stop(void) {}

void i2s_input_resync(void) {}

uint32_t i2s_input_poll(void) {
    return 0;
}

void i2s_input_discard_queued(void) {}

void i2s_input_prefill_silence(uint32_t frames) {
    (void)frames;
}

I2sInputState i2s_input_get_state(void) {
    return I2S_INPUT_INACTIVE;
}

bool i2s_input_is_clock_master(void) {
    return false;
}

uint8_t i2s_input_active_bck_pin(void) {
    return i2s_effective_bck_pin();
}

void i2s_slave_poll(void) {}

void i2s_slave_update_clock_servo(void) {}

bool i2s_slave_check_rate_change(void) {
    return false;
}

I2sSlaveState i2s_slave_get_state(void) {
    return I2S_SLAVE_INACTIVE;
}

uint32_t i2s_slave_get_detected_rate(void) {
    return 0;
}

void i2s_slave_get_status(I2sSlaveStatusPacket *out) {
    memset(out, 0, sizeof(*out));
    out->state = I2S_SLAVE_INACTIVE;
    out->clock_mode = i2s_clock_mode;
}

// ---------------------------------------------------------------------------------------------------
// adat_input.c: started means acquiring, as with no light on the receiver

static AdatInputState adat_state = ADAT_INPUT_INACTIVE;

void adat_input_init(void) {
    adat_state = ADAT_INPUT_INACTIVE;
}

void adat_input_start(void) {
    if (adat_input_pin != 0xFF) {
        adat_state = ADAT_INPUT_ACQUIRING;
    }
}

void adat_input_stop(void) {
    adat_state = ADAT_INPUT_INACTIVE;
}

uint32_t adat_input_poll(void) {
    return 0;
}

void adat_input_discard_queued(void) {}

void adat_input_update_clock_servo(void) {}

bool adat_input_check_rate_change(void) {
    return false;
}

void adat_input_on_rate_change(uint32_t freq) {
    (void)freq;
}

AdatInputState adat_input_get_state(void) {
    return adat_state;
}

bool adat_input_is_running(void) {
    return adat_state != ADAT_INPUT_INACTIVE;
}

uint32_t adat_input_get_detected_rate(void) {
    return 0;
}

void adat_input_get_status(AdatInputStatusPacket *out) {
    memset(out, 0, sizeof(*out));
    out->state = (uint8_t)adat_state;
    out->clock_mode = adat_clock_mode;
    out->enabled = adat_input_enabled;
    out->pin = adat_input_pin;
    out->rate_ok = (adat_clock_mode == ADAT_CLOCK_MODE_SLAVE || audio_state.freq <= 48000u) ? 1 : 0;
}
