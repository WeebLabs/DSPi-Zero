// The Zero 2 W's pin rules for DSPi's DSPI_PLATFORM_PINS hooks (DSPi's platform_pins.h), from the
// hardware table in Documentation/Porting/PiZero_GPIO_plan.md. Each rule is what the hardware can do
// for one pin. The I2S input's pins must also fit its snapshot window together (i2s_input_pi.h), which a
// rule for one pin cannot check, so a set that does not fit falls back to the default pins at start.

#include "platform_pins.h"
#include "control_surfaces.h"

#define HEADER_FIRST    2u      // GPIO 0 and 1 are kept for a HAT's ID EEPROM
#define HEADER_LAST     27u
#define SMI_FIRST       8u      // SD0 to SD15 at 16 bits
#define SMI_LAST        23u
#define PCM_DIN         20u

static bool in(uint8_t pin, uint32_t first, uint32_t last) {
    return pin >= first && pin <= last;
}

bool dspi_platform_pin_valid(uint8_t fn, uint8_t pin) {
    switch (fn) {
        case DSPI_PIN_FN_OUTPUT:
        case DSPI_PIN_FN_PDM:
        case DSPI_PIN_FN_ADAT_OUT:
        case DSPI_PIN_FN_I2S_CLK:
            return in(pin, SMI_FIRST, SMI_LAST);
        case DSPI_PIN_FN_MCK:
            return pin == 4u || pin == 5u;  // GPCLK0, GPCLK1; GPCLK2 is the wireless chip's clock
        case DSPI_PIN_FN_SPDIF_RX:
        case DSPI_PIN_FN_ADAT_IN:
            return pin == PCM_DIN;
        default:
            return in(pin, HEADER_FIRST, HEADER_LAST);
    }
}

bool dspi_platform_pin_pair_valid(uint8_t fn, uint8_t a, uint8_t b) {
    switch (fn) {
        case DSPI_PIN_FN_UART_CTRL:
            return a == 14u && b == 15u;
        case DSPI_PIN_FN_I2C_CTRL:
        case DSPI_PIN_FN_CS_DISPLAY:
            return a == 2u && b == 3u;
        default:
            return false;
    }
}

// The Pi has no ADC, and its PWM pins are SMI or PCM lines.
bool dspi_platform_cs_pin_valid(uint8_t cs_type, uint8_t pin) {
    (void)pin;
    return cs_type != CS_TYPE_POT && cs_type != CS_TYPE_LED_PWM && cs_type != CS_TYPE_AUX_PWM;
}
