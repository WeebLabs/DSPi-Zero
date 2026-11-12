// S/PDIF decoder over an oversampled line (spdif_decode.h). Biphase-mark coding makes every time slot two
// cells with a level change between slots, and a 1 has a second change between its two cells. Each
// subframe starts with a preamble that data cannot make: B 3-1-1-3, M 3-3-1-1 or W 3-2-1-2 cells.

#include "spdif_decode.h"

#define MEASURE_RUNS 256u   // runs to find the longest (a preamble's 3 cells) before searching
#define SEARCH_ERRORS 64u   // failed preambles before the cell length is measured again
#define MAX_CELL_Q8 (40u << 8)  // the longest cell tracked (32 kHz is 30.5); SPDIF_RING_TAIL covers it

void spdif_decode_reset(spdif_decoder_t *d) {
    *d = (spdif_decoder_t){0};
}

// Run lengths are sorted against the half-cell points, in samples with 8 fractional bits.
static void set_cell(spdif_decoder_t *d, uint32_t cell_q8) {
    d->cell_q8 = cell_q8;
    d->th[0] = cell_q8 / 2u;
    d->th[1] = cell_q8 + cell_q8 / 2u;
    d->th[2] = 2u * cell_q8 + cell_q8 / 2u;
    d->th[3] = 3u * cell_q8 + cell_q8 / 2u;
}

static inline uint32_t cells_of(const spdif_decoder_t *d, uint32_t run) {
    uint32_t r = (run > 0xFFFFu ? 0xFFFFu : run) << 8;
    if (r < d->th[1]) {
        return r < d->th[0] ? 0u : 1u;
    }
    return r < d->th[2] ? 2u : r < d->th[3] ? 3u : 4u;
}

static inline uint64_t full_index(uint64_t end, uint32_t low) {
    return end - (uint32_t)((uint32_t)end - low);
}

// ---------------------------------------------------------------------------------------------------
// Search: time the level changes and look for a preamble

// A run of `run` samples that started at sample `start`. Returns true when a preamble has been found.
static bool search_run(spdif_decoder_t *d, uint32_t run, uint32_t start) {
    if (d->cell_q8 == 0) {
        if (run > d->longest) {
            d->longest = run;
        }
        if (++d->runs_seen >= MEASURE_RUNS && d->longest >= 3u) {
            set_cell(d, (d->longest << 8) / 3u);
            d->runs_seen = 0;
        }
        return false;
    }
    uint32_t cells = cells_of(d, run);
    if (d->npre == 0) {
        if (cells == 3u) {
            d->pre[0] = run;
            d->pre_at = start;
            d->npre = 1;
        }
        return false;
    }
    d->pre[d->npre++] = run;
    if (d->npre < 4u) {
        return false;
    }
    uint32_t c0 = cells_of(d, d->pre[0]), c1 = cells_of(d, d->pre[1]), c2 = cells_of(d, d->pre[2]);
    uint32_t c3 = cells_of(d, d->pre[3]);
    bool match = c0 == 3u && c2 == 1u &&
                 ((c1 == 1u && c3 == 3u) || (c1 == 3u && c3 == 1u) || (c1 == 2u && c3 == 2u));
    d->npre = 0;
    if (match) {
        // One preamble's eight cells give the cell to within a few percent, too coarse to place 64 cells.
        // The next preamble, 64 cells on, measures it well enough, and tracking starts there.
        uint32_t span = d->pre[0] + d->pre[1] + d->pre[2] + d->pre[3];
        uint32_t dist = d->pre_at - d->first_at;
        if (d->have_first && (dist << 8) > 62u * d->cell_q8 && (dist << 8) < 66u * d->cell_q8) {
            set_cell(d, dist << 2);
            d->start = d->pre_at;
            d->tracking = 1;
            d->have_first = 0;
            d->runs_seen = 0;
            return true;
        }
        set_cell(d, span << 5);
        d->have_first = 1;
        d->first_at = d->pre_at;
        return false;
    }
    d->errors++;
    if (++d->runs_seen > SEARCH_ERRORS) {
        set_cell(d, 0);
        d->longest = 0;
        d->runs_seen = 0;
        return false;
    }
    if (cells == 3u) {     // this run may start the real preamble
        d->pre[0] = run;
        d->pre_at = start;
        d->npre = 1;
    }
    return false;
}

static void search(spdif_decoder_t *d, const uint32_t *ring, uint32_t mask, uint64_t end) {
    uint32_t level = d->level, last = d->last_edge;
    while (d->pos < end && !d->tracking) {
        uint32_t x = ring[(uint32_t)(d->pos >> 5) & mask];
        uint32_t t = x ^ (x >> 1 | level << 31);   // bit 31 - i set: a level change at sample i
        level = x & 1u;
        uint32_t pos = (uint32_t)d->pos;
        while (t) {
            uint32_t i = (uint32_t)__builtin_clz(t);
            t &= ~(0x80000000u >> i);
            uint32_t edge = pos + i;
            bool found = d->have_edge && search_run(d, edge - last, last);
            d->have_edge = 1;
            last = edge;
            if (found) {
                break;
            }
        }
        d->pos += 32u;
    }
    d->level = level;
    d->last_edge = last;
}

// ---------------------------------------------------------------------------------------------------
// Tracking: one sample a time slot, at the centre of its second cell

// The line level at sample r, counted from bit 31 of base[0]. The ring's tail keeps this from wrapping.
static inline uint32_t level_at(const uint32_t *base, uint32_t r) {
    return (base[r >> 5] << (r & 31u)) >> 31;
}

// The 32 samples from r on, sample r in bit 31.
static inline uint32_t window_at(const uint32_t *base, uint32_t r) {
    uint32_t w0 = base[r >> 5], sh = r & 31u;
    return sh ? (w0 << sh) | (base[(r >> 5) + 1u] >> (32u - sh)) : w0;
}

// Reads the subframe whose first edge is sample a of base[]. The low word is the level of each slot's
// second cell in bits 31:4 and of preamble cells 7, 1, 3 and 5 in bits 3:0, or 0 if an edge was missing.
// The high word is preamble cells 0, 2, 4 and 6 in bits 7:4 and the next subframe's first edge above.
#if defined(__arm__) && !defined(__thumb__) && !defined(SPDIF_DECODE_PORTABLE)
uint64_t spdif_read_levels(const uint32_t *base, uint32_t a, uint32_t c);     // spdif_decode_a32.S
#else
uint64_t spdif_read_levels(const uint32_t *base, uint32_t a, uint32_t c) {
    // Centres of the second, fourth, sixth and eighth cell after an anchor, the same for every group.
    uint32_t off1 = (3u * c) >> 9, off3 = (7u * c) >> 9, off5 = (11u * c) >> 9, off7 = (15u * c) >> 9;
    uint32_t limit = c >> 8;
    uint32_t pre = level_at(base, a + (c >> 9)) << 7 | level_at(base, a + ((5u * c) >> 9)) << 6 |
                   level_at(base, a + ((9u * c) >> 9)) << 5 | level_at(base, a + ((13u * c) >> 9)) << 4;
    uint32_t lv = level_at(base, a + off1) << 2 | level_at(base, a + off3) << 1 | level_at(base, a + off5);
    // Eight anchors, each the level change that starts a group of four slots (the last starts the next
    // preamble). The window that finds it starts on the cell before, which is the previous group's
    // fourth slot or the preamble's last cell.
    for (uint32_t g = 0; g < 8u; g++) {
        uint32_t from = a + off7;
        uint32_t x = window_at(base, from);
        uint32_t t = x ^ (uint32_t)((int32_t)x >> 31);
        uint32_t edge = t ? (uint32_t)__builtin_clz(t) : 32u;
        if (edge > limit) {
            return 0;
        }
        lv |= (x >> 31) << (3u + 4u * g);
        a = from + edge;
        if (g == 7u) {
            break;
        }
        lv |= level_at(base, a + off1) << (4u + 4u * g) | level_at(base, a + off3) << (5u + 4u * g) |
              level_at(base, a + off5) << (6u + 4u * g);
    }
    return (uint64_t)(a << 8 | pre) << 32 | lv;
}
#endif

// The sync code of a preamble's eight cells in the order read_levels leaves them (0, 2, 4, 6, 7, 1, 3,
// 5), either way up: B is 11101000, M 11100010 and W 11100100. Anything else is 0.
static const uint8_t sync_of[256] = {
    [0xE4] = SPDIF_SYNC_B, [0x1B] = SPDIF_SYNC_B, [0xD4] = SPDIF_SYNC_M,
    [0x2B] = SPDIF_SYNC_M, [0xC5] = SPDIF_SYNC_W, [0x3A] = SPDIF_SYNC_W,
};

// Reads subframes from d->start on and emits them. Returns 0 when the ring does not reach far enough
// for the next one, and -1 when what is there is not a subframe (the stream was lost).
static int track(spdif_decoder_t *d, const uint32_t *ring, uint32_t mask, uint64_t end, spdif_emit_fn emit,
                 void *ctx) {
    uint32_t s = d->start, c = d->cell_q8, n = 0;
    int r = 0;
    for (;;) {
        uint32_t need = s + ((65u * c) >> 8) + 64u;    // through the next preamble's edge, with window slack
        if ((int32_t)((uint32_t)end - need) < 0) {
            break;
        }
        r = -1;
        if (c > MAX_CELL_Q8) {
            break;      // reads would run past the ring's tail
        }
        uint32_t a0 = s & 31u;
        uint64_t got = spdif_read_levels(ring + ((s >> 5) & mask), a0, c);
        uint32_t lv = (uint32_t)got, hi = (uint32_t)(got >> 32);
        uint32_t sync = sync_of[(hi & 0xF0u) | (lv & 0xFu)];
        if (!lv || !sync) {
            break;
        }
        // Every slot starts with a level change, so a 1 is a second cell equal to the slot before's.
        emit((~(lv ^ lv << 1) & 0xFFFFFFF0u) | sync, full_index(end, s), ctx);
        n++;
        // 64 cells exactly: follow the source's rate and the sampling clock from every subframe.
        uint32_t span = (hi >> 8) - a0;
        c += (uint32_t)((int32_t)((span << 2) - c) >> 3);
        s += span;
        r = 0;
    }
    d->start = s;
    d->cell_q8 = c;
    d->subframes += n;
    return r;
}

static void lose(spdif_decoder_t *d, uint64_t end) {
    d->errors++;
    d->tracking = 0;
    set_cell(d, d->cell_q8);    // tracking moves the cell alone
    d->pos = full_index(end, (d->start & ~31u) + 32u);     // past the failed preamble's first edge
    d->have_edge = 0;
    d->npre = 0;
    d->have_first = 0;
    d->runs_seen = 0;
}

void spdif_decode_ring(spdif_decoder_t *d, const uint32_t *ring, uint32_t ring_words, uint64_t end,
                       spdif_emit_fn emit, void *ctx) {
    uint32_t mask = ring_words - 1u;
    for (;;) {
        if (d->tracking) {
            if (track(d, ring, mask, end, emit, ctx) == 0) {
                return;
            }
            lose(d, end);
        }
        search(d, ring, mask, end);
        if (!d->tracking) {
            return;
        }
    }
}

uint64_t spdif_decode_needs(const spdif_decoder_t *d, uint64_t end) {
    return d->tracking ? full_index(end, d->start) : d->pos;
}

void spdif_decode_skip(spdif_decoder_t *d, uint64_t from) {
    if (d->tracking) {
        set_cell(d, d->cell_q8);
    }
    d->tracking = 0;
    d->pos = from;
    d->have_edge = 0;
    d->npre = 0;
    d->have_first = 0;
}
