// Stand-in for flash_clkdiv.c, which drives the RP2350's QSPI flash, keeping its public API. The input
// modules are not stood in for: spdif_input.c, i2s_input.c and adat_input.c are compiled, on
// spdif_rx_pi.c, i2s_input_pi.c and adat_input_pi.c.

#include <stdbool.h>
#include <stdint.h>

#include "hardware/flash.h"

#include "flash_clkdiv.h"

// ---------------------------------------------------------------------------------------------------
// flash_clkdiv.c: the RAM-backed flash in pico_shim.c has no clock divider to keep

void dspi_flash_apply_clkdiv(void) {}

void dspi_flash_range_erase(uint32_t flash_offs, size_t count) {
    flash_range_erase(flash_offs, count);
}

void dspi_flash_range_program(uint32_t flash_offs, const uint8_t *data, size_t count) {
    flash_range_program(flash_offs, data, count);
}
