#pragma once

#include "pico.h"

// RP2350 clock tree. clock_get_hz() reports the frequencies DSPi configures (clk_sys 307.2 MHz),
// so divider arithmetic gives the same numbers as on the RP2350. Nothing here touches Pi clocks.
enum clock_num {
    clk_gpout0 = 0,
    clk_gpout1 = 1,
    clk_gpout2 = 2,
    clk_gpout3 = 3,
    clk_ref = 4,
    clk_sys = 5,
    clk_peri = 6,
    clk_hstx = 7,
    clk_usb = 8,
    clk_adc = 9,
    CLK_COUNT
};
typedef enum clock_num clock_num_t;
typedef enum clock_num clock_handle_t;

#define SHIM_CLK_SYS_HZ 307200000u

uint32_t clock_get_hz(clock_handle_t clock);
uint32_t frequency_count_khz(uint src);
bool set_sys_clock_hz(uint32_t freq_hz, bool required);
bool set_sys_clock_khz(uint32_t freq_khz, bool required);
void set_sys_clock_pll(uint32_t vco_freq, uint post_div1, uint post_div2);
bool check_sys_clock_hz(uint32_t freq_hz, uint *vco_freq_out, uint *post_div1_out, uint *post_div2_out);
void clock_gpio_init_int_frac8(uint gpio, uint src, uint32_t div_int, uint8_t div_frac);
void clock_gpio_init_int_frac16(uint gpio, uint src, uint32_t div_int, uint16_t div_frac);
void clock_gpio_init(uint gpio, uint src, float div);

// The Pi's clock outputs on the header: GPCLK0 on GPIO 4, GPCLK1 on 5 and GPCLK2 on 6 (20 and 21 are
// the PCM block's). DSPi's MCK request rejects any other pin.
#define GPIO_TO_GPOUT_CLOCK_HANDLE(gpio, default_clk_handle) \
    ((gpio) == 4 ? clk_gpout0 : (gpio) == 5 ? clk_gpout1 : (gpio) == 6 ? clk_gpout2 : (default_clk_handle))
