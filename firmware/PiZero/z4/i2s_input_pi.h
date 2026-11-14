#pragma once

// The Pi's I2S receiver under DSPi's i2s_input.c, which is compiled unchanged: the shim (pico_shim.c)
// passes DSPi's calls for its PIO receiver on pio2 here. Four data lines (DSPi's I2S input pins, GPIO
// 22-25 by default) are decoded from the same snapshots, so the eight channels stay sample-aligned. BCK
// and LRCLK are the SMI's own (DSPi's BCK pin, GPIO 12 and 13 by default) in DSPi's master and slave roles.

#include <stdbool.h>
#include <stdint.h>

// With an external clock master, BCK and LRCLK come in on DSPi's slave BCK pin and the next (GPIO 26
// and 27 by default), in either clock-pin mode, since the SMI always drives its own. Those are then
// phase-locked to the master by trimming PLLA, so every output, S/PDIF and ADAT included, runs on the
// master's clock and a DAC may take either pair. Every pin the capture reads must lie within 16 GPIOs,
// and the data lines within 8, or the pins above are used instead.

// From the shim, for DSPi's state machines on pio2: pio_enable_sm_mask_in_sync starts the receiver or,
// while it runs, makes it frame afresh (i2s_input_resync); the last pio_sm_unclaim stops it.
void i2s_input_pi_enable(void);
void i2s_input_pi_release(void);
// PIO IRQ 7, the checked program's framing slip: raised on lasting loss of framing or of the clock lock.
bool i2s_input_pi_slipped(void);
void i2s_input_pi_clear_slip(void);

void i2s_input_pi_poll(void);               // main loop: the clock follower and bench requests
bool i2s_input_pi_running(void);            // the capture is on (VideoCore mailbox calls must wait)
bool soft_vcxo_pi_tracking(void);           // soft_vcxo_pi.c: an input's servo or the follower owns the trim
bool i2s_input_pi_owns_pll(void);           // external clock: PLLA's trim belongs to the follower
bool i2s_input_pi_follower_locked(void);    // external clock: the SMI's clocks follow the master's

// Bench (request 0x51). Overrides take effect at the next start; zeros keep the defaults.
#define I2S_INPUT_PI_MAGIC 0x50493249u      // "I2IP"
enum {
    I2S_INPUT_PI_PCM_MASTER = 1u << 0,      // the PCM block plays a sine as a master on GPIO 18, 19, 21
    I2S_INPUT_PI_HIST = 1u << 1,            // count the snapshots between BCK rises (status hist[])
    I2S_INPUT_PI_KEEP_LINES = 1u << 2,      // leave the decoded snapshots in the cache, as before the fix
};
typedef struct {
    uint8_t shift, data, ext_bck;           // snapshot window base, data line 0 (the rest follow), external BCK
    uint8_t flags;
    uint8_t check_lines;                    // bit k: data line k must carry a sine on both channels
    uint8_t pad[3];
    uint32_t pcm_hz;                        // I2S_INPUT_PI_PCM_MASTER: its sample rate
    int32_t sine_c_q30;                     // 2 cos(w) for the sine check
    uint32_t sine_tol;                      // largest residual that passes
} i2s_input_pi_bench_t;

typedef struct {
    uint32_t magic, running, role, waits;   // role: 1 master, 2 slave, 3 external clock
    uint32_t dma_channel, warm_channel, pairs, ext_bck_hz;
    uint32_t sample_ps;                     // one snapshot
    uint32_t ticks, skipped, overruns, wraps, warms;
    uint32_t frames, bad_halves, concealed, repaired, gaps, rescued, wr;
    uint32_t samples_lo, samples_hi, dec_cycles_lo, dec_cycles_hi, tick_cycles_max, elapsed_us;
    uint32_t slips, starts, reframes;
    // The clock follower.
    uint32_t lock_state, lock_updates, locked_us, unlocks, rate_mismatch;
    int32_t trim_ppb, trim_min_ppb, trim_max_ppb;
    int32_t phase_ns_x100, phase_min_x100, phase_max_x100;
    // The bench's sine check.
    uint32_t sine_checked, sine_errors, sine_worst, pcm_master;
    uint32_t dma_cs, dma_conblk, dma_dest, dma_debug;   // the capture channel's registers
    int32_t bck_ratio_ppm;                  // the master's BCK period against the SMI's, last measured
    // The decoder's bad halves by cause (i2s_rx_t), and how many dumps are held.
    uint32_t rescue_fail[5], bad_short, bad_one_short, bad_one_long, bad_long, bad_at_gap, dumps;
    uint32_t hist[10];                      // I2S_INPUT_PI_HIST: BCK rises k + 1 snapshots apart (the last: more)
    uint32_t when[16];                      // the stalls among them, by sixteenths of the tick's snapshots
    uint32_t rescued_when[16];              // the stalls put right, by the same sixteenths
    uint32_t rescued_by_step, rescued_dist[4];  // how each was found (i2s_rx_t)
    uint32_t prefill_drains, prefill_starts, dac_muted;     // the main loop's prefill handshake (main_z4.c fills these)
} i2s_input_pi_status_t;

// Bench, the snapshots around the k-th bad half since the last bench request. Four words come first, which
// are their count, the index of the rise that ended the half, its bit clocks and their place in the ring.
#define I2S_INPUT_PI_DUMPS 6u
#define I2S_INPUT_PI_DUMP_WORDS 1024u
const uint32_t *i2s_input_pi_dump(uint32_t k);

void i2s_input_pi_bench(const i2s_input_pi_bench_t *b);    // NULL puts the defaults back
void i2s_input_pi_status(i2s_input_pi_status_t *out);
