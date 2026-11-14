// DSPi's soft VCXO (soft_vcxo.h) on the Pi: PLLA, which every output clock comes from (PiZero_PLLA_plan.md),
// trimmed with pll_out_trim(). The RP2350 divider's rounding that DSPi's command allows for is added back,
// since the Pi's clocks are exact, and 0 parks PLLA at nominal. With an external I2S master the trim is
// the clock follower's (i2s_input_pi.c), and commands only change what is reported.

#include <math.h>

#include "soft_vcxo.h"
#include "audio_input.h"
#include "audio_clock_div.h"
#include "usb_audio.h"
#include "pll_out.h"
#include "i2s_input_pi.h"

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
    if (i2s_input_pi_owns_pll()) {
        return;
    }
    float trim = 0.0f;
    uint32_t fs = audio_state.freq;
    if (ppm != 0.0f && fs) {
        float rp = (float)clock_get_hz(clk_sys) / (float)audio_base_divider_16_8(fs) / (float)fs;
        trim = ppm + (rp - 1.0f) * 1e6f;
    }
    pll_out_trim((int32_t)lrintf(trim * 1000.0f));
}

// True while an input owns the trim: the S/PDIF servo, or ADAT or I2S in slave mode. PLLA then keeps
// off its integer row (pll_out_set_tracked()), whatever the trim is at the moment.
bool soft_vcxo_pi_tracking(void) {
    return input_source_is_spdif(active_input_source) || adat_slave_mode_active() || i2s_slave_mode_active();
}

float soft_vcxo_current_ppm(void) {
    return current_ppm;
}
