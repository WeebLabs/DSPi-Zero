#pragma once

// PLLA as the audio clock source (PiZero_PLLA_plan.md). PLLA is retuned in place: only NDIV and FRAC
// change, the firmware's pre-divider, loop settings and CORE channel stay as they are.

#include <stdbool.h>
#include <stdint.h>

enum {
    PLL_ROW_FIRMWARE = 0,   // the values found at the first retune, PER channel off
    PLL_ROW_A48,            // 2359.296 MHz, PER /8 = 294.912 MHz
    PLL_ROW_A44,            // 2167.6032 MHz, PER /8 = 270.9504 MHz
    PLL_ROW_B48,            // 1843.2 MHz integer-N, PER /5 = 368.64 MHz
    PLL_ROW_B44,            // 1693.44 MHz, PER /5 = 338.688 MHz
    PLL_ROW_COUNT,
};

enum { PLL_IDLE, PLL_WAITING, PLL_LOCKED, PLL_TIMEOUT };
enum { COUNT_IDLE, COUNT_RUNNING, COUNT_DONE, COUNT_TIMEOUT };

#define PLL_OUT_MAGIC 0x4F4C4C50u   // "PLLO"

typedef struct {
    uint32_t magic;
    uint32_t row, state, retunes;
    uint32_t lock_us;           // from the last write to FLOCKA set
    uint32_t unlock_first_us;   // first FLOCKA clear after the write, 0xFFFFFFFF if never seen
    uint32_t unlock_reads;      // reads that found FLOCKA clear during the lock wait
    uint32_t lock_reads;        // reads during the lock wait
    uint32_t lock_lost;         // FLOCKA clear while locked, counted by pll_out_poll()
    uint32_t count, count_mux, count_osc, count_seq, count_state, count_us;
    // Live registers, then the firmware's values as found at the first retune.
    uint32_t ctrl, frac, ana1, per, core, cm_plla, cm_lock, v3d_ctl, v3d_div, pcm_ctl, pcm_div;
    uint32_t fw_ctrl, fw_frac, fw_per, fw_cm_plla;
    uint32_t smi_ctl, smi_div, in_use, refused;     // phase 2: the SMI clock and who owns PLLA
    int32_t trim_ppb;                               // phase 5
    uint32_t trims;
    uint32_t wd_events, wd_restores, wd_fallbacks;  // phase 6 watchdog
    uint32_t wd_seen[4];    // CTRL, FRAC, PER, CM_PLLA (bit 31: FLOCKA) at the last mismatch
    uint32_t gp0_ctl, gp0_div;  // MCK on GPCLK0
    uint32_t gpfsel0, gpfsel1;  // GPIO 0-19 functions: MCK on GPIO 4, ADAT on GPIO 16
} pll_out_status_t;

// For the SMI: selects a row now and returns true once plla_per runs at it. A move within the same PER
// divider is live and returns at once; a first start waits for lock and must not be done with the
// SMI on PLLA. pll_out_smi_row() picks the row and CM_SMI divider for a rate, or returns PLL_ROW_COUNT.
bool pll_out_select(uint32_t row);
uint32_t pll_out_smi_row(uint32_t freq, uint32_t *divi);
void pll_out_set_in_use(bool on);
void pll_out_trim(int32_t ppb);
enum { PLL_WD_OK, PLL_WD_RESTORED, PLL_WD_FALLBACK };
uint32_t pll_out_watchdog(void);    // main loop; on PLL_WD_FALLBACK the caller moves the SMI to PLLD
void pll_out_sabotage(uint32_t ms); // bench: rewrite PLLA as the firmware would, once or for ms     // live trim of every row's multiplier, kept across family moves                   // the SMI runs from plla_per: bench moves that hold it are refused

// Settling capture: the SMI clock counted in back-to-back 1 ms windows, with PLLA moved to a row at the
// start of window PLL_CAPTURE_PRE. Bench only: it blocks the main loop for the whole capture.
#define PLL_CAPTURE_MAX 64u
#define PLL_CAPTURE_PRE 2u
typedef struct {
    uint32_t seq, row, from_row, mux, gate, n;
    uint32_t count[PLL_CAPTURE_MAX];
    int32_t start_us[PLL_CAPTURE_MAX];      // window start from the write, negative before it
} pll_out_capture_t;
void pll_out_capture_request(uint32_t row, uint32_t n);
const pll_out_capture_t *pll_out_capture(void);

void pll_out_request(uint32_t row);                 // carried out by pll_out_poll()
void pll_out_count_request(uint32_t mux, uint32_t osc_cycles);
void pll_out_pcm_clock(uint32_t src, uint32_t divi);   // integer divider, no MASH; divi 0 stops it
bool pll_out_counter_claimed(void);                 // true once the counter has been used
void pll_out_poll(void);                            // main loop
void pll_out_status(pll_out_status_t *out);
