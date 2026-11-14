// The Zero 2 W's pin rules for DSPi's DSPI_PLATFORM_PINS hooks (DSPi's platform_pins.h), from the
// hardware table in Documentation/Porting/PiZero_GPIO_plan.md. Each rule is what the hardware can do;
// until the port moves a function to the pin it is given, that function stays on its default.

#include "platform_pins.h"
#include "control_surfaces.h"

#define HEADER_FIRST    2u      // GPIO 0 and 1 are kept for a HAT's ID EEPROM
#define HEADER_LAST     27u
#define SMI_FIRST       8u      // SD0 to SD15 at 16 bits
#define SMI_LAST        23u
#define WINDOW_FIRST    12u     // the I2S input's 16-pin snapshot window
#define WINDOW_LAST     27u
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
        case DSPI_PIN_FN_I2S_CLK_SLAVE:
        case DSPI_PIN_FN_I2S_RX:
            return in(pin, WINDOW_FIRST, WINDOW_LAST);
        case DSPI_PIN_FN_MCK:
            return pin == 4u;   // GPCLK0; GPCLK1 (GPIO 5) and GPCLK2 (GPIO 6) once proven free
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
