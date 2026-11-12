// PLLA retune in place and the clock manager's frequency counter (PiZero_PLLA_plan.md phases 1 to 3).
// Register layout and the divider load sequence follow Linux clk-bcm2835.c. Every write carries the
// clock manager password. Work requested over USB runs from the main loop, never from the USB IRQ.

#include "pll_out.h"

#define REG(a) (*(volatile uint32_t *)(a))

#define CPRMAN 0x3F101000u
#define PASSWD (0x5Au << 24)
#define CM_PCMCTL (CPRMAN + 0x098u)
#define CM_PCMDIV (CPRMAN + 0x09cu)
#define CM_SMICTL (CPRMAN + 0x0b0u)
#define CM_SMIDIV (CPRMAN + 0x0b4u)
#define CM_GP0CTL (CPRMAN + 0x070u)
#define CM_GP0DIV (CPRMAN + 0x074u)
#define GPFSEL0 0x3F200000u
#define GPFSEL1 0x3F200004u
#define CM_V3DCTL (CPRMAN + 0x038u)
#define CM_V3DDIV (CPRMAN + 0x03cu)
#define CM_TCNTCTL (CPRMAN + 0x0c0u)
#define CM_TCNTCNT (CPRMAN + 0x0c4u)
#define CM_OSCCOUNT (CPRMAN + 0x100u)
#define CM_PLLA (CPRMAN + 0x104u)
#define CM_LOCK (CPRMAN + 0x114u)
#define A2W_PLLA_ANA1 (CPRMAN + 0x1014u)
#define A2W_PLLA_CTRL (CPRMAN + 0x1100u)
#define A2W_PLLA_FRAC (CPRMAN + 0x1200u)
#define A2W_PLLA_CORE (CPRMAN + 0x1400u)
#define A2W_PLLA_PER (CPRMAN + 0x1500u)

#define CM_ENAB (1u << 4)
#define CM_KILL (1u << 5)
#define CM_BUSY (1u << 7)
#define FLOCKA (1u << 8)
#define HOLDPER (1u << 7)
#define LOADPER (1u << 6)
#define LOAD_BITS 0x55u             // LOADPER, LOADCORE, LOADCCP2, LOADDSI0
#define NDIV_MASK 0x3FFu
#define FRAC_MASK 0xFFFFFu
#define CHANNEL_DISABLE (1u << 8)

#define LOCK_SPIN_US 300u           // tight polling after the write, then once per main loop pass
#define LOCK_TIMEOUT_US 100000u     // Linux's LOCK_TIMEOUT_NS

typedef struct {
    uint16_t ndiv;
    uint32_t frac;
    uint8_t per_div;
} row_t;

// NDIV and FRAC are for a reference of 2 x 19.2 MHz: the firmware leaves the feedback pre-divider on.
static const row_t rows[PLL_ROW_COUNT] = {
    [PLL_ROW_A48] = {61, 0x70A3D, 8},
    [PLL_ROW_A44] = {56, 0x72B02, 8},
    [PLL_ROW_B48] = {48, 0x00000, 5},
    [PLL_ROW_B44] = {44, 0x1999A, 5},
};

static pll_out_status_t st = {.magic = PLL_OUT_MAGIC, .unlock_first_us = 0xFFFFFFFFu};
static bool fw_saved, counter_claimed, prev_set, per_held, in_use;
static volatile uint32_t pending_row = 0xFFFFFFFFu;
static volatile uint32_t pending_mux = 0xFFFFFFFFu, pending_osc;
static uint32_t write_us, count_start_us;
static const row_t *active;
static int32_t trim_ppb;     // applied to every row, kept across family moves
static volatile uint32_t pending_capture = 0xFFFFFFFFu;
static pll_out_capture_t cap;

static uint32_t micros(void) {
    return REG(0x3F003004u);
}

static uint32_t rd(uint32_t a) {
    return REG(a) & 0x00FFFFFFu;
}

static void wr(uint32_t a, uint32_t v) {
    REG(a) = PASSWD | (v & 0x00FFFFFFu);
}

static void save_firmware_values(void) {
    if (fw_saved) {
        return;
    }
    fw_saved = true;
    st.fw_ctrl = rd(A2W_PLLA_CTRL);
    st.fw_frac = rd(A2W_PLLA_FRAC);
    st.fw_per = rd(A2W_PLLA_PER);
    st.fw_cm_plla = rd(CM_PLLA);
}

// Moves NDIV and FRAC so the VCO never passes above the higher of the old and new values. CTRL is
// written last either way, in case FRAC only takes effect on a CTRL write.
static void write_multiplier(uint32_t ndiv, uint32_t frac) {
    uint32_t ctrl = rd(A2W_PLLA_CTRL);
    uint64_t cur = (uint64_t)(ctrl & NDIV_MASK) << 20 | (rd(A2W_PLLA_FRAC) & FRAC_MASK);
    uint64_t tgt = (uint64_t)ndiv << 20 | frac;
    ctrl = (ctrl & ~NDIV_MASK) | ndiv;
    if (tgt < cur) {
        wr(A2W_PLLA_CTRL, ctrl);
    }
    wr(A2W_PLLA_FRAC, frac);
    wr(A2W_PLLA_CTRL, ctrl);
}

// The row's multiplier (NDIV.FRAC, 20 fractional bits) scaled by trim_ppb, rounded to one FRAC step.
static uint32_t trimmed(const row_t *r) {
    int64_t base = (int64_t)r->ndiv << 20 | r->frac;
    int64_t d = base * trim_ppb;
    return (uint32_t)(base + (d >= 0 ? d + 500000000 : d - 500000000) / 1000000000);
}

// True when PLLA's registers hold the active row (with the trim) and its channel runs. Something else,
// such as the firmware, may have rewritten them since we did.
static bool registers_match(void) {
    if (!active) {
        return false;
    }
    uint32_t m = trimmed(active);
    return (rd(A2W_PLLA_CTRL) & NDIV_MASK) == m >> 20 && (rd(A2W_PLLA_FRAC) & FRAC_MASK) == (m & FRAC_MASK) &&
           (rd(A2W_PLLA_PER) & 0x1FFu) == active->per_div && !(rd(CM_PLLA) & HOLDPER) && (REG(CM_LOCK) & FLOCKA);
}

// A move between rows with the same PER divider, with the channel already running, is made live: the
// PLL slews to the new rate and the channel keeps its clock. Anything else holds the channel.
static bool needs_hold(uint32_t row) {
    return row == PLL_ROW_FIRMWARE || !active || active->per_div != rows[row].per_div ||
           (rd(A2W_PLLA_PER) & CHANNEL_DISABLE) || (rd(CM_PLLA) & HOLDPER);
}

static void retune(uint32_t row) {
    save_firmware_values();
    per_held = needs_hold(row);
    if (per_held) {     // the other hold bits stay as the firmware set them
        wr(CM_PLLA, (rd(CM_PLLA) & ~LOAD_BITS) | HOLDPER);
    }
    if (row == PLL_ROW_FIRMWARE) {
        write_multiplier(st.fw_ctrl & NDIV_MASK, st.fw_frac & FRAC_MASK);
        active = 0;
    } else {
        uint32_t m = trimmed(&rows[row]);
        write_multiplier(m >> 20, m & FRAC_MASK);
        active = &rows[row];
    }
    st.row = row;
    st.retunes++;
    st.state = PLL_WAITING;
    st.unlock_first_us = 0xFFFFFFFFu;
    st.unlock_reads = 0;
    st.lock_reads = 0;
    prev_set = false;
    write_us = micros();
}

// Called once FLOCKA is set: start the PER channel at the row's divider, or put it back as found.
static void release_per(void) {
    if (active) {
        wr(A2W_PLLA_PER, active->per_div);      // clears CHANNEL_DISABLE
        uint32_t cm = rd(CM_PLLA) & ~LOAD_BITS;
        wr(CM_PLLA, cm | LOADPER);
        wr(CM_PLLA, cm);
        wr(CM_PLLA, cm & ~HOLDPER);
    } else {
        wr(A2W_PLLA_PER, st.fw_per | CHANNEL_DISABLE);
        wr(CM_PLLA, st.fw_cm_plla & ~LOAD_BITS);
    }
}

// Samples FLOCKA during the lock wait. lock_us is when it was last seen to go from clear to set.
static bool lock_sample(void) {
    uint32_t t = micros() - write_us;
    st.lock_reads++;
    if (REG(CM_LOCK) & FLOCKA) {
        if (!prev_set) {
            st.lock_us = t;
        }
        prev_set = true;
        return true;
    }
    if (st.unlock_first_us == 0xFFFFFFFFu) {
        st.unlock_first_us = t;
    }
    st.unlock_reads++;
    prev_set = false;
    return false;
}

// FLOCKA may still be set for a moment after the write, so a set bit only counts as lock once the
// spin has run its course.
static void lock_wait_step(bool spin) {
    if (spin) {
        bool locked = false;
        while (micros() - write_us < LOCK_SPIN_US) {
            locked = lock_sample();
        }
        if (!locked) {
            return;
        }
    } else if (!lock_sample()) {
        if (micros() - write_us > LOCK_TIMEOUT_US) {
            st.state = PLL_TIMEOUT;
        }
        return;
    }
    st.state = PLL_LOCKED;
    if (per_held) {
        release_per();
        per_held = false;
    }
}

static void count_start(uint32_t mux, uint32_t osc) {
    counter_claimed = true;
    wr(CM_TCNTCTL, CM_KILL);
    wr(CM_TCNTCTL, (mux & 0xFu) | (mux >> 4) << 12);
    wr(CM_OSCCOUNT, osc);
    st.count_mux = mux;
    st.count_osc = osc;
    st.count_state = COUNT_RUNNING;
    count_start_us = micros();
}

static void count_step(void) {
    uint32_t t = micros() - count_start_us;
    if (t < st.count_osc / 19u + 1000u) {
        return;     // the gate is osc_cycles of 19.2 MHz; Linux also waits at least 1 ms
    }
    if (rd(CM_OSCCOUNT) == 0 && !(REG(CM_TCNTCTL) & CM_BUSY)) {
        st.count = rd(CM_TCNTCNT);      // 24 bits in hardware: keep gate x rate below 2^24
        st.count_us = t;
        st.count_seq++;
        st.count_state = COUNT_DONE;
        wr(CM_TCNTCTL, 0);
    } else if (t > st.count_osc / 19u + 100000u) {
        st.count_state = COUNT_TIMEOUT;
        wr(CM_TCNTCTL, 0);
    }
}

bool pll_out_select(uint32_t row) {
    if (row == PLL_ROW_FIRMWARE || row >= PLL_ROW_COUNT) {
        return false;
    }
    if (active == &rows[row] && st.state == PLL_LOCKED && registers_match()) {
        return true;
    }
    retune(row);
    if (!per_held) {
        return true;    // live move; pll_out_poll() records the lock
    }
    lock_wait_step(true);
    while (st.state == PLL_WAITING) {
        lock_wait_step(false);
    }
    return st.state == PLL_LOCKED;
}

// Phase 5: a live trim, FRAC (and NDIV when the fraction carries) only, with no hold, so the channel and
// the SMI keep running. One FRAC step is about 0.016 ppm on the A rows and 0.02 ppm on the B rows.
void pll_out_trim(int32_t ppb) {
    trim_ppb = ppb;
    st.trim_ppb = ppb;
    st.trims++;
    if (active && st.state == PLL_LOCKED) {
        uint32_t m = trimmed(active);
        write_multiplier(m >> 20, m & FRAC_MASK);
        write_us = micros();
    }
}

// Phase 6 watchdog, once a second while the SMI runs from PLLA. It reads back the multiplier, the PER
// divider, HOLDPER and FLOCKA. On a mismatch it counts the event and restores our values once; if a
// mismatch comes back before ten clean checks have passed, it asks the caller to fall back to PLLD.
static uint32_t wd_last_us, wd_strikes, wd_clean;
static uint32_t sabotage_until, sabotage_last;

uint32_t pll_out_watchdog(void) {
    uint32_t now = micros();
    if (now - wd_last_us < 1000000u) {
        return PLL_WD_OK;
    }
    wd_last_us = now;
    if (!in_use || !active || st.state != PLL_LOCKED || now - write_us < 100000u) {
        return PLL_WD_OK;   // not ours, or a move or trim is still settling
    }
    uint32_t m = trimmed(active);
    uint32_t ctrl = rd(A2W_PLLA_CTRL), frac = rd(A2W_PLLA_FRAC), per = rd(A2W_PLLA_PER), cm = rd(CM_PLLA);
    uint32_t lock = REG(CM_LOCK) & FLOCKA;
    if (registers_match()) {
        if (++wd_clean >= 10u) {
            wd_strikes = 0;
        }
        return PLL_WD_OK;
    }
    st.wd_events++;
    st.wd_seen[0] = ctrl;
    st.wd_seen[1] = frac;
    st.wd_seen[2] = per;
    st.wd_seen[3] = cm | (lock ? 1u << 31 : 0);
    wd_clean = 0;
    if (wd_strikes++ == 0) {
        write_multiplier(m >> 20, m & FRAC_MASK);
        wr(A2W_PLLA_PER, active->per_div);
        uint32_t c = rd(CM_PLLA) & ~LOAD_BITS;
        wr(CM_PLLA, c | LOADPER);
        wr(CM_PLLA, c & ~HOLDPER);
        write_us = micros();
        st.wd_restores++;
        return PLL_WD_RESTORED;
    }
    wd_strikes = 0;
    st.wd_fallbacks++;
    return PLL_WD_FALLBACK;
}

// Bench: stands in for the firmware rewriting PLLA, by writing its boot multiplier behind our back, once
// (ms 0) or every 100 ms for ms milliseconds.
void pll_out_sabotage(uint32_t ms) {
    save_firmware_values();
    write_multiplier(st.fw_ctrl & NDIV_MASK, st.fw_frac & FRAC_MASK);
    sabotage_last = micros();
    sabotage_until = ms ? sabotage_last + ms * 1000u : 0;
}

uint32_t pll_out_smi_row(uint32_t freq, uint32_t *divi) {
    // Option B: plla_per is 7680 fs at 48 or 44.1 kHz, and the SMI takes 768 fs.
    uint32_t base = freq == 0 ? 0 : freq % 11025u == 0 ? 44100u : freq % 8000u == 0 ? 48000u : 0;
    if (base == 0 || (10u * base) % freq) {
        return PLL_ROW_COUNT;
    }
    uint32_t d = 10u * base / freq;
    if (d < 1u || d > 15u) {
        return PLL_ROW_COUNT;   // CM_SMI has 4 integer bits
    }
    *divi = d;
    return base == 48000u ? PLL_ROW_B48 : PLL_ROW_B44;
}

void pll_out_set_in_use(bool on) {
    in_use = on;
}

// Counts mux in back-to-back windows of gate oscillator cycles, moving PLLA to row at the start of window
// PLL_CAPTURE_PRE. Blocks the main loop for the whole capture, so it is for the bench only.
static void capture(uint32_t row, uint32_t mux, uint32_t gate, uint32_t n) {
    counter_claimed = true;
    st.count_state = COUNT_IDLE;
    cap.row = row;
    cap.mux = mux;
    cap.gate = gate;
    cap.n = n;
    cap.from_row = st.row;
    uint32_t t_write = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t t0 = micros();
        if (i == PLL_CAPTURE_PRE) {
            if (in_use && needs_hold(row)) {
                st.refused++;
                break;
            }
            retune(row);
            t_write = t0;
        }
        wr(CM_TCNTCTL, CM_KILL);
        wr(CM_TCNTCTL, (mux & 0xFu) | (mux >> 4) << 12);
        wr(CM_OSCCOUNT, gate);
        while (rd(CM_OSCCOUNT) != 0 || (REG(CM_TCNTCTL) & CM_BUSY)) {
            if (micros() - t0 > gate / 19u + 2000u) {
                break;
            }
        }
        cap.count[i] = rd(CM_TCNTCNT);
        cap.start_us[i] = (int32_t)t0;
        if (i >= PLL_CAPTURE_PRE && st.state == PLL_WAITING) {
            lock_wait_step(false);
        }
    }
    wr(CM_TCNTCTL, 0);
    for (uint32_t i = 0; i < n; i++) {
        cap.start_us[i] -= (int32_t)t_write;
    }
    cap.seq++;
}

void pll_out_capture_request(uint32_t row, uint32_t n) {
    pending_capture = row | (n < PLL_CAPTURE_MAX ? n : PLL_CAPTURE_MAX) << 8;
}

const pll_out_capture_t *pll_out_capture(void) {
    return &cap;
}

void pll_out_request(uint32_t row) {
    if (row < PLL_ROW_COUNT) {
        pending_row = row;
    }
}

void pll_out_count_request(uint32_t mux, uint32_t osc_cycles) {
    pending_osc = osc_cycles;
    pending_mux = mux;
}

bool pll_out_counter_claimed(void) {
    return counter_claimed;
}

void pll_out_pcm_clock(uint32_t src, uint32_t divi) {
    wr(CM_PCMCTL, CM_KILL);
    uint32_t t0 = micros();
    while ((REG(CM_PCMCTL) & CM_BUSY) && micros() - t0 < 1000u) {
    }
    if (divi == 0) {
        wr(CM_PCMCTL, 0);
        return;
    }
    wr(CM_PCMDIV, (divi & 0xFFFu) << 12);
    wr(CM_PCMCTL, src & 0xFu);
    wr(CM_PCMCTL, (src & 0xFu) | CM_ENAB);
}

void pll_out_poll(void) {
    if (sabotage_until) {
        uint32_t now = micros();
        if ((int32_t)(now - sabotage_until) >= 0) {
            sabotage_until = 0;
        } else if (now - sabotage_last >= 100000u) {
            write_multiplier(st.fw_ctrl & NDIV_MASK, st.fw_frac & FRAC_MASK);
            sabotage_last = now;
        }
    }
    uint32_t row = pending_row;
    if (row != 0xFFFFFFFFu && st.state != PLL_WAITING) {
        pending_row = 0xFFFFFFFFu;
        if (in_use && needs_hold(row)) {
            st.refused++;   // holding the channel would stop the SMI
            return;
        }
        retune(row);
        lock_wait_step(per_held);
    } else if (st.state == PLL_WAITING) {
        lock_wait_step(false);
    } else if (st.state == PLL_LOCKED && !(REG(CM_LOCK) & FLOCKA)) {
        st.lock_lost++;
    }
    uint32_t c = pending_capture;
    if (c != 0xFFFFFFFFu && st.state != PLL_WAITING && st.count_state != COUNT_RUNNING) {
        pending_capture = 0xFFFFFFFFu;
        capture(c & 0xFFu, 27u, 19200u, c >> 8);     // the SMI clock, 1 ms windows
    }
    uint32_t mux = pending_mux;
    if (mux != 0xFFFFFFFFu && st.count_state != COUNT_RUNNING) {
        pending_mux = 0xFFFFFFFFu;
        count_start(mux, pending_osc);
    } else if (st.count_state == COUNT_RUNNING) {
        count_step();
    }
}

void pll_out_status(pll_out_status_t *out) {
    st.ctrl = rd(A2W_PLLA_CTRL);
    st.frac = rd(A2W_PLLA_FRAC);
    st.ana1 = rd(A2W_PLLA_ANA1);
    st.per = rd(A2W_PLLA_PER);
    st.core = rd(A2W_PLLA_CORE);
    st.cm_plla = rd(CM_PLLA);
    st.cm_lock = rd(CM_LOCK);
    st.v3d_ctl = rd(CM_V3DCTL);
    st.v3d_div = rd(CM_V3DDIV);
    st.pcm_ctl = rd(CM_PCMCTL);
    st.pcm_div = rd(CM_PCMDIV);
    st.smi_ctl = rd(CM_SMICTL);
    st.smi_div = rd(CM_SMIDIV);
    st.gp0_ctl = rd(CM_GP0CTL);
    st.gp0_div = rd(CM_GP0DIV);
    st.gpfsel0 = REG(GPFSEL0);
    st.gpfsel1 = REG(GPFSEL1);
    st.in_use = in_use;
    *out = st;
}
