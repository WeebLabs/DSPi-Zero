// DSPi's soft VCXO (soft_vcxo.h) on the Pi. On the RP2350 it trims the system PLL that every output
// clock comes from; here PLLA is that clock (PiZero_PLLA_plan.md), so the command becomes pll_out_trim().
// DSPi works the command out against its RP2350 output divider, whose rounding is added back here
// because the Pi's output clocks are exact. A command of 0 parks PLLA at nominal, as on the RP2350.

#include <math.h>

#include "soft_vcxo.h"
#include "audio_clock_div.h"
#include "usb_audio.h"
#include "pll_out.h"

#define MAX_PPM 300.0f

static float current_ppm;

void soft_vcxo_init(void) {}

void soft_vcxo_set_ppm(float ppm) {
    if (ppm > MAX_PPM) {
        ppm = MAX_PPM;
    } else if (ppm < -MAX_PPM) {
        ppm = -MAX_PPM;
    }
    current_ppm = ppm;
    float trim = 0.0f;
    uint32_t fs = audio_state.freq;
    if (ppm != 0.0f && fs) {
        float rp = (float)clock_get_hz(clk_sys) / (float)audio_base_divider_16_8(fs) / (float)fs;
        trim = ppm + (rp - 1.0f) * 1e6f;
    }
    pll_out_trim((int32_t)lrintf(trim * 1000.0f));
}

float soft_vcxo_current_ppm(void) {
    return current_ppm;
}
