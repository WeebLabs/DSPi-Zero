// I2S decoder over GPLEV0 snapshots (i2s_rx_decode.h). The plain decoder below is the definition: one
// state machine fed the code at every BCK rise. The fast path finds the rises 32 snapshots at a time and
// builds whole frames from 64 codes when they are one frame, and gives everything else to the same
// state machine, so both write the same words.

#include "i2s_rx_decode.h"

#include <string.h>

#ifdef I2S_RX_A32
#define SCAN i2s_rx_scan_a32
#define FRAME i2s_rx_frame_a32
#else
#define SCAN i2s_rx_scan_c
#define FRAME i2s_rx_frame_c
#endif

void i2s_rx_set_period(i2s_rx_t *d, uint32_t period_q8) {
    // Two rises nearly 1.5 bit clocks apart have lost one between them. The same holds across a reload
    // gap, which takes the place of at most two snapshots: one less let two ordinary rises six snapshots
    // apart at 96 kHz pass for a lost one, about once in 600 laps.
    d->stall_dist = (period_q8 * 3u / 2u) >> 8;
    d->gap_dist = d->stall_dist;
    d->phase_max = (period_q8 / 2u + 255u) >> 8;
}

void i2s_rx_init(i2s_rx_t *d, const i2s_rx_pins_t *pins, uint32_t *const ring[I2S_RX_LINES],
                 uint32_t ring_words, uint32_t period_q8) {
    memset(d, 0, sizeof(*d));
    d->pins = *pins;
    d->bck_mask = (uint16_t)(1u << (pins->bck - pins->shift));
    d->lr_mask = (uint16_t)(1u << (pins->lr - pins->shift));
    uint32_t lo = pins->data[0];
    for (uint32_t k = 1; k < I2S_RX_LINES; k++) {
        lo = pins->data[k] < lo ? pins->data[k] : lo;
    }
    d->data_shift = lo - pins->shift;
    for (uint32_t k = 0; k < I2S_RX_LINES; k++) {
        d->line_bit[k] = (uint8_t)(pins->data[k] - lo);
        d->data_mask |= (uint16_t)(1u << (pins->data[k] - pins->shift));
    }
    for (uint32_t k = 0; k < I2S_RX_LINES; k++) {
        d->ring[k] = ring[k];
    }
    d->ring_mask = ring_words - 1u;
    i2s_rx_set_period(d, period_q8);
    d->lr = 2;
    d->prev = 1;    // no rise is taken from the first snapshot alone
}

static inline uint32_t code_of(const i2s_rx_t *d, uint32_t sample) {
    return (sample >> d->pins.shift) & 0xFFFFu;
}

// The 32 bits of one data line from 32 codes, the first code in bit 31.
static uint32_t word_of(const uint16_t *codes, uint32_t bit) {
    uint32_t w = 0;
    for (uint32_t i = 0; i < 32u; i++) {
        w = w << 1 | ((codes[i] >> bit) & 1u);
    }
    return w;
}

// Ends the half that ran at LRCLK level `level` with n bit clocks. A right half ends a frame, and one
// frame is written for it whatever happened, so the rings keep step with LRCLK.
static void finish_half(i2s_rx_t *d, uint32_t level, uint32_t n, bool gap) {
    bool ok = n == 32u;
    uint32_t w[I2S_RX_LINES] = {0, 0, 0, 0};
    if (ok) {
        for (uint32_t k = 0; k < I2S_RX_LINES; k++) {
            w[k] = word_of(d->half, d->data_shift + d->line_bit[k]);
        }
    } else if (d->synced) {
        d->bad_halves++;
        *(n < 31u ? &d->bad_short : n == 31u ? &d->bad_one_short : n == 33u ? &d->bad_one_long : &d->bad_long) += 1u;
        d->bad_at_gap += gap;
    }
    if (level == 0u) {
        memcpy(d->left, w, sizeof(w));
        d->left_ok = ok;
        return;
    }
    if (ok && d->left_ok) {
        for (uint32_t k = 0; k < I2S_RX_LINES; k++) {
            uint32_t at = (d->wr + d->lead[k]) & d->ring_mask;
            d->ring[k][at] = d->left[k];
            d->ring[k][at + 1u] = w[k];
        }
        d->synced = 1;
    } else if (d->synced) {
        for (uint32_t k = 0; k < I2S_RX_LINES; k++) {
            uint32_t at = (d->wr + d->lead[k]) & d->ring_mask, from = (at - 2u) & d->ring_mask;
            d->ring[k][at] = d->ring[k][from];
            d->ring[k][at + 1u] = d->ring[k][from + 1u];
        }
        d->concealed++;
    } else {
        d->left_ok = 0;
        return;
    }
    d->left_ok = 0;
    d->wr += 2u;
    d->frames++;
}

static inline void keep(i2s_rx_t *d, uint32_t c) {
    if (d->n < I2S_RX_HALF_MAX) {
        d->half[d->n] = (uint16_t)c;
    }
    d->n++;
}

static inline uint32_t bck_of(const i2s_rx_t *d, const uint32_t *p) {
    return (*p >> d->pins.bck) & 1u;
}

// A longer stall leaves no distance between rises that stands out: the two rises around the lost one
// are as far apart as two ordinary ones can be. But the rises after it then sit a snapshot or two later
// than the even spacing before it predicts. Returns the index of the rise after that step, or 0.
static uint32_t find_step(const i2s_rx_t *d, const uint32_t *const r[33]) {
    // The spacing, from up to 96 rises before these. The nearest 8 join the rises looked at, so the
    // step is always judged against several rises before it.
    enum { LEAD = 8, TAIL = 3 };
    const uint32_t *e[LEAD + 33];
    const uint32_t *first = r[0];
    uint32_t n = 0;
    for (const uint32_t *p = r[0] - 1; p > d->run_start && n < 96u; p--) {
        if (bck_of(d, p) && !bck_of(d, p - 1)) {
            first = p;
            if (n < LEAD) {
                e[LEAD - 1u - n] = p;
            }
            n++;
        }
    }
    if (n < 32u) {
        return 0;
    }
    for (uint32_t k = 0; k <= 32u; k++) {
        e[LEAD + k] = r[k];
    }
    int32_t period = (int32_t)((uint32_t)(r[0] - first) * 256u / n);
    // Each rise's distance from where the spacing puts it, in 1/256 snapshots, summed from both ends.
    int32_t res[LEAD + 33], sum = 0, before = 0;
    for (uint32_t k = 0; k < LEAD + 33u; k++) {
        res[k] = (int32_t)(e[k] - e[0]) * 256 - period * (int32_t)k;
        sum += res[k];
    }
    int32_t best = 0, second = 0;
    uint32_t at = 0;
    for (uint32_t k = 1; k + TAIL <= LEAD + 33u; k++) {
        before += res[k - 1u];
        if (k <= LEAD || (uint32_t)(e[k] - e[k - 1u]) + 1u != d->stall_dist) {
            continue;
        }
        int32_t step = (sum - before) / (int32_t)(LEAD + 33u - k) - before / (int32_t)k;
        if (step > best) {
            second = best;
            best = step;
            at = k - LEAD;
        } else if (step > second) {
            second = step;
        }
    }
    return best >= 192 && second <= best - 128 ? at : 0u;    // 0.75 of a snapshot, and half clear of the next
}

// The snapshot of the rise being handled: given, or found by counting rises back from the end.
static const uint32_t *edge_snapshot(const i2s_rx_t *d) {
    const uint32_t *t = d->edge_at;
    if (t || d->run_start == 0) {
        return t;
    }
    uint32_t back = d->edge_back + 1u;
    for (t = d->scan_end - 1; t > d->run_start; t--) {
        if (bck_of(d, t) && !bck_of(d, t - 1) && --back == 0u) {
            return t;
        }
    }
    return 0;
}

// A half ended with 31 bit clocks and no reload gap in it: the capture stalled for about one BCK phase
// and a rise went unseen. The snapshots still show where, as two rises 1.5 bit clocks or more apart,
// and the lost bit's data is in the long BCK run between them. Puts that bit into the half's codes.
static bool rescue(i2s_rx_t *d) {
    if (d->run_start == 0) {
        d->rescue_fail[0]++;
        return false;
    }
    const uint32_t *t = edge_snapshot(d);
    if (t == 0) {
        d->rescue_fail[0]++;
        return false;
    }
    // The 32 rises before it. The half's 31 are r[2] to r[32], r[1] ended the half before, and r[0]
    // is the rise before that.
    const uint32_t *r[33];
    r[32] = t;
    uint32_t have = 32;
    for (const uint32_t *p = t - 1; p > d->run_start && have; p--) {
        if (bck_of(d, p) && !bck_of(d, p - 1)) {
            r[--have] = p;
        }
    }
    if (have) {
        d->rescue_fail[0]++;
        return false;
    }
    uint32_t m = 0, found = 0;
    for (uint32_t k = 1; k <= 32u; k++) {
        if ((uint32_t)(r[k] - r[k - 1u]) >= d->stall_dist) {
            m = k;
            found++;
        }
    }
    bool by_step = found == 0u;
    if (by_step) {
        m = find_step(d, r);
    }
    if (found > 1u || m == 0u) {
        d->rescue_fail[found ? 2 : 1]++;
        return false;
    }
    uint32_t v;
    if (m == 1u) {
        // The rise lost was the last of the half before, so the rise taken for it is this half's first.
        v = code_of(d, *r[1]);
    } else {
        // BCK between the two rises: a run high, then a run low. The longer one hides the lost phase.
        const uint32_t *p = r[m - 1u];
        uint32_t high = 0, low;
        while (bck_of(d, p + high)) {
            high++;
        }
        low = (uint32_t)(r[m] - p) - high;
        if (high <= d->phase_max && low <= d->phase_max) {
            d->rescue_fail[4]++;
            return false;       // neither run is longer than a phase can be: a whole bit clock went
        }
        if (high > low) {
            v = code_of(d, p[high - 1u]);      // the low phase was lost: the last high snapshot is the bit's
        } else if (low > high) {
            // The high phase was lost. The bit's data was set up in the low phase before it, where
            // the first low snapshot may still show the bit before (the one at r[m - 1]) and a later
            // one may already show the bit after (the one at r[m]). A snapshot that shows neither
            // of those can only be the lost bit.
            const uint32_t *q = p + high;
            uint32_t data = d->data_mask;
            uint32_t before = code_of(d, *p) & data, after = code_of(d, *r[m]) & data;
            uint32_t q0 = code_of(d, q[0]) & data, q1 = code_of(d, q[1]) & data;
            if (q1 != after || low >= d->phase_max + 2u) {
                v = code_of(d, q[1]);       // not the bit after, or too early in the run to be it
            } else if (q0 != before) {
                v = code_of(d, q[0]);       // not the bit before
            } else if (before == after) {
                v = code_of(d, q[1]);       // all four agree, so the lost bit almost surely does too
            } else {
                d->rescue_fail[3]++;
                return false;
            }
        } else {
            d->rescue_fail[4]++;
            return false;
        }
    }
    // r[m] is the half's code m - 2 (r[2] is its first), so the lost one goes in before it.
    uint32_t at = m == 1u ? 0u : m - 2u;
    memmove(&d->half[at + 1u], &d->half[at], (30u - at) * sizeof(d->half[0]));
    d->half[at] = (uint16_t)v;
    d->n++;
    d->rescued++;
    d->rescued_at = r[m];
    uint32_t over = (uint32_t)(r[m] - r[m - 1u]) - d->stall_dist;
    *(by_step ? &d->rescued_by_step : &d->rescued_dist[over > 3u ? 3u : over]) += 1u;
    return true;
}

// One BCK rise. LRCLK changes on the falling edge before a half's last bit, so the code whose LRCLK
// differs from the half's level is that half's 32nd.
static void edge(i2s_rx_t *d, uint32_t c) {
    uint32_t l = (c & d->lr_mask) ? 1u : 0u;
    if (d->lr == 2u) {
        d->lr = l;
    }
    if (l == d->lr) {
        keep(d, c);
        return;
    }
    uint32_t level = d->lr;
    bool gap = d->gap != I2S_RX_GAP_NONE;
    if (d->gap == I2S_RX_GAP_FOUND) {
        d->gap = I2S_RX_GAP_NONE;
        uint32_t n = d->n + 1u, at = d->gap_at;
        if (n == 31u && at <= 30u) {
            // One rise short, and the gap is in this half: its bit goes back where the gap was. (A
            // stall elsewhere in the same half would be taken for the gap; the two do not meet often.)
            memmove(&d->half[at + 1u], &d->half[at], (30u - at) * sizeof(d->half[0]));
            d->half[at] = d->gap_code;
            d->n++;
            d->repaired++;
        } else if (n == 32u && at == 31u && d->gap_since >= d->gap_dist &&
                   ((d->gap_code & d->lr_mask) ? 1u : 0u) == l) {
            // The gap took this half's last rise, and c is already the next half's first bit.
            d->half[31] = d->gap_code;
            d->repaired++;
            finish_half(d, level, 32u, false);
            d->lr = l;
            d->half[0] = (uint16_t)c;
            d->n = 1;
            return;
        }
    }
    if (!gap && d->n == 30u) {
        rescue(d);
    }
    keep(d, c);
    if (d->n != 32u && d->synced) {
        d->bad_edge = edge_snapshot(d);
        d->bad_n = d->n;
    }
    finish_half(d, level, d->n, gap);
    d->lr = l;
    d->n = 0;
}

// After a gap: takes snapshots up to and including the next BCK rise, and decides from the distance
// between the two rises which code would stand in for a lost one. Returns the snapshots used.
static uint32_t gap_resume(i2s_rx_t *d, const uint32_t *samples, uint32_t n) {
    uint32_t i = 0;
    if (!d->gap_have_post && n) {
        d->gap_post = (uint16_t)code_of(d, samples[0]);
        d->gap_have_post = true;
    }
    for (; i < n; i++) {
        uint32_t c = code_of(d, samples[i]), b = c & d->bck_mask;
        bool rise = b && !d->prev;
        d->prev = b != 0u;
        d->gap_since++;
        if (rise) {
            // In BCK's high phase the data is that rise's; in its low phase the next rise's is set up.
            d->gap_code = (d->gap_pre & d->bck_mask) ? d->gap_post : d->gap_pre;
            d->gap = I2S_RX_GAP_FOUND;
            d->edge_at = &samples[i];
            edge(d, c);
            return i + 1u;
        }
    }
    return i;
}

void i2s_rx_decode_ref(i2s_rx_t *d, const uint32_t *samples, uint32_t n) {
    if (samples != d->scan_end) {
        d->run_start = samples;
    }
    uint32_t i = d->gap == I2S_RX_GAP_ARMED ? gap_resume(d, samples, n) : 0u;
    for (; i < n; i++) {
        uint32_t c = code_of(d, samples[i]), b = c & d->bck_mask;
        if (b && !d->prev) {
            d->edge_at = &samples[i];
            edge(d, c);
        }
        d->prev = b != 0u;
    }
    d->scan_end = samples + n;
}

uint32_t i2s_rx_scan_c(const uint32_t *samples, uint32_t n, i2s_rx_scan_t *s, uint16_t *out) {
    uint32_t k = 0, prev = s->prev;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t b = (samples[i] >> s->bck_bit) & 1u;
        if (b && !prev) {
            out[k++] = (uint16_t)(samples[i] >> s->shift);
        }
        prev = b;
    }
    s->prev = prev;
    return k;
}

uint32_t i2s_rx_frame_c(const uint16_t *codes, const i2s_rx_frame_t *f) {
    for (uint32_t i = 0; i < 64u; i++) {
        uint32_t want = i >= 31u && i < 63u ? f->lr_mask : 0u;
        if ((codes[i] & f->lr_mask) != want) {
            return 0;
        }
    }
    for (uint32_t k = 0; k < I2S_RX_LINES; k++) {
        uint32_t bit = f->data_shift + (uint32_t)__builtin_ctz(f->line_mask[k]);
        f->dst[k][0] = word_of(codes, bit);
        f->dst[k][1] = word_of(codes + 32, bit);
    }
    return 1;
}

// Builds frames from the pending codes. A whole frame at a frame boundary is built at once; any other
// code goes through the state machine. With `all` clear, the codes of an unfinished frame stay pending.
static void frames(i2s_rx_t *d, bool all) {
    uint32_t i = 0, n = d->npend;
    i2s_rx_frame_t f = {.lr_mask = d->lr_mask, .data_shift = d->data_shift};
    for (uint32_t k = 0; k < I2S_RX_LINES; k++) {
        f.line_mask[k] = (uint8_t)(1u << d->line_bit[k]);
    }
    while (i < n) {
        if (d->n == 0u && d->lr == 0u && d->gap == I2S_RX_GAP_NONE) {
            if (n - i < 64u) {
                if (!all) {
                    break;
                }
            } else {
                for (uint32_t k = 0; k < I2S_RX_LINES; k++) {
                    f.dst[k] = &d->ring[k][(d->wr + d->lead[k]) & d->ring_mask];
                }
                if (FRAME(&d->pend[i], &f)) {
                    d->synced = 1;
                    d->left_ok = 0;
                    d->wr += 2u;
                    d->frames++;
                    i += 64u;
                    continue;
                }
            }
        }
        d->edge_at = 0;
        d->edge_back = n - 1u - i;
        edge(d, d->pend[i++]);
    }
    if (i && i < n) {
        memmove(d->pend, &d->pend[i], (n - i) * sizeof(d->pend[0]));
    }
    d->npend = n - i;
}

void i2s_rx_flush(i2s_rx_t *d) {
    frames(d, true);
}

void i2s_rx_decode(i2s_rx_t *d, const uint32_t *samples, uint32_t n) {
    if (samples != d->scan_end) {
        frames(d, true);    // what is pending belongs to the snapshots before the break
        d->run_start = samples;
    }
    if (d->gap == I2S_RX_GAP_ARMED) {
        uint32_t used = gap_resume(d, samples, n);
        samples += used;
        n -= used;
    }
    d->scan_end = samples;
    i2s_rx_scan_t s = {.shift = d->pins.shift, .bck_bit = d->pins.bck, .prev = d->prev};
    while (n) {
        uint32_t take = n < I2S_RX_SCAN_SAMPLES ? n : I2S_RX_SCAN_SAMPLES;
        uint32_t whole = take & ~31u;
        uint16_t *out = &d->pend[d->npend];
        uint32_t k = whole ? SCAN(samples, whole, &s, out) : 0u;
        if (take != whole) {
            k += i2s_rx_scan_c(samples + whole, take - whole, &s, out + k);
        }
        d->npend += k;
        d->scan_end = samples + take;
        frames(d, false);
        samples += take;
        n -= take;
    }
    d->prev = s.prev;
}

void i2s_rx_gap(i2s_rx_t *d, const uint32_t *before, uint32_t n_before) {
    i2s_rx_flush(d);
    d->gaps++;
    d->scan_end = 0;    // the next snapshots start a new run
    if (n_before == 0u) {
        return;
    }
    // Snapshots since the last rise, counted back from the gap. The first of a rise has BCK high and
    // the one before it low.
    uint32_t since = 0;
    for (uint32_t i = n_before; i-- > 1u;) {
        uint32_t b1 = code_of(d, before[i]) & d->bck_mask, b0 = code_of(d, before[i - 1u]) & d->bck_mask;
        if (b1 && !b0) {
            break;
        }
        since++;
    }
    d->gap = I2S_RX_GAP_ARMED;
    d->gap_at = d->n;
    d->gap_since = since;
    d->gap_pre = (uint16_t)code_of(d, before[n_before - 1u]);
    d->gap_have_post = false;
}
