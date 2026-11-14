#ifndef DSPI_PLATFORM_PINS_PI_H
#define DSPI_PLATFORM_PINS_PI_H

// The Zero 2 W's default pins for DSPi's DSPI_PLATFORM_PINS hooks (DSPi's platform_pins.h). They are
// the pins the port drives today; the rules are in z4/platform_pins_pi.c.

#undef PICO_AUDIO_SPDIF_PIN
#define PICO_AUDIO_SPDIF_PIN        8       // output slot 1, SD0
#define PICO_SPDIF_PIN_2            9
#define PICO_SPDIF_PIN_3            10
#define PICO_SPDIF_PIN_4            11
#define PICO_PDM_PIN                21      // no PDM output on the Pi yet; an SMI line no default uses
#define PICO_ADAT_PIN               16      // SD8
#define PICO_I2S_BCK_PIN            12      // LRCLK 13
#define PICO_I2S_BCK_PIN_SLAVE      26      // an external master's LRCLK on 27
#define PICO_I2S_MCK_PIN            4       // GPCLK0

// S/PDIF and ADAT input share the PCM block's one input, so S/PDIF inputs 2 to 4 sit on input 1's pin,
// which is always claimed: they can never be enabled.
#define PICO_SPDIF_RX_PIN_DEFAULT   20
#define PICO_SPDIF_RX_PIN2_DEFAULT  20
#define PICO_SPDIF_RX_PIN3_DEFAULT  20
#define PICO_SPDIF_RX_PIN4_DEFAULT  20
#define PICO_I2S_RX_PIN_DEFAULT     22
#define PICO_I2S_RX_PIN2_DEFAULT    23
#define PICO_I2S_RX_PIN3_DEFAULT    24
#define PICO_I2S_RX_PIN4_DEFAULT    25

#define DAC_HW_MUTE_DEFAULT_PIN     7u
#define UART_CTRL_DEFAULT_TX_PIN    14      // the header's only UART pins
#define UART_CTRL_DEFAULT_RX_PIN    15
#define I2C_CTRL_DEFAULT_SDA_PIN    2       // BSC1, the header's only I2C pins
#define I2C_CTRL_DEFAULT_SCL_PIN    3

#endif
