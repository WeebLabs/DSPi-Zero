// S/PDIF decoder over an oversampled line (spdif_decode.h). Biphase-mark coding makes every time slot two
// cells with a level change between slots, and a 1 has a second change between its two cells. Each
// subframe starts with a preamble that data cannot make: B 3-1-1-3, M 3-3-1-1 or W 3-2-1-2 cells.

#include "spdif_decode.h"

#define MEASURE_RUNS 256u   // runs to find the longest (a preamble's 3 cells) before searching
#define SEARCH_ERRORS 64u   // failed preambles before the cell length is measured again

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
// Tracking: two samples a time slot, at the cell centres

// The line level at sample p.
static inline uint32_t level_at(const uint32_t *ring, uint32_t mask, uint32_t p) {
    return (ring[(p >> 5) & mask] << (p & 31u)) >> 31;
}

// The samples from p on that differ from `level`, as a 32-bit window with sample p in bit 31.
static inline uint32_t changes_from(const uint32_t *ring, uint32_t mask, uint32_t p, uint32_t level) {
    uint32_t w0 = ring[(p >> 5) & mask], sh = p & 31u;
    uint32_t x = sh ? (w0 << sh) | (ring[((p >> 5) + 1u) & mask] >> (32u - sh)) : w0;
    return x ^ (0u - level);
}

// Reads the subframe whose first edge is d->start. Returns 1 after emitting it, 0 if the ring does not
// reach far enough yet, and -1 if it is not a subframe (the stream was lost).
static int track_one(spdif_decoder_t *d, const uint32_t *ring, uint32_t mask, uint64_t end, spdif_emit_fn emit,
                     void *ctx) {
    uint32_t s = d->start, c = d->cell_q8;
    uint32_t need = s + ((65u * c) >> 8) + 64u;    // through the next preamble's edge, with window slack
    if ((int32_t)((uint32_t)end - need) < 0) {
        return 0;
    }
    // Centres of the eight cells after an anchor, the same for the whole subframe.
    uint32_t off[8];
    for (uint32_t j = 0; j < 8u; j++) {
        off[j] = ((2u * j + 1u) * c) >> 9;
    }
    // The preamble's eight cells, read so the first is 1: B 11101000, M 11100010, W 11100100.
    uint32_t flip = level_at(ring, mask, s + off[0]) ^ 1u, pat = 0;
    for (uint32_t j = 0; j < 8u; j++) {
        pat = pat << 1 | (level_at(ring, mask, s + off[j]) ^ flip);
    }
    uint32_t word = pat == 0xE8u ? SPDIF_SYNC_B : pat == 0xE2u ? SPDIF_SYNC_M : pat == 0xE4u ? SPDIF_SYNC_W : 0u;
    if (!word) {
        return -1;
    }
    uint32_t prev = (pat & 1u) ^ flip;
    uint32_t anchor = s, limit = c >> 8;
    // Seven groups of four time slots, each anchored on its first slot's leading edge, then the next
    // preamble's edge. Every slot starts with a level change, so its first cell is the inverse of the
    // cell before; only the second is read, and a 1 is a second cell equal to the cell before the slot.
    for (uint32_t g = 0; g < 8u; g++) {
        uint32_t from = anchor + off[7];
        uint32_t t = changes_from(ring, mask, from, prev);
        uint32_t edge = t ? (uint32_t)__builtin_clz(t) : 32u;
        if (edge > limit) {
            return -1;
        }
        anchor = from + edge;
        if (g == 7u) {
            break;
        }
        uint32_t b0 = level_at(ring, mask, anchor + off[1]);
        uint32_t b1 = level_at(ring, mask, anchor + off[3]);
        uint32_t b2 = level_at(ring, mask, anchor + off[5]);
        uint32_t b3 = level_at(ring, mask, anchor + off[7]);
        uint32_t bits = (b0 ^ prev ^ 1u) | (b1 ^ b0 ^ 1u) << 1 | (b2 ^ b1 ^ 1u) << 2 | (b3 ^ b2 ^ 1u) << 3;
        word |= bits << (4u + 4u * g);
        prev = b3;
    }
    emit(word, full_index(end, s), ctx);
    d->subframes++;
    // 64 cells exactly: follow the source's rate and the sampling clock from every subframe.
    uint32_t span = anchor - s;
    set_cell(d, c + (uint32_t)((int32_t)((span << 2) - c) >> 3));
    d->start = anchor;
    return 1;
}

static void lose(spdif_decoder_t *d, uint64_t end) {
    d->errors++;
    d->tracking = 0;
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
            int r = track_one(d, ring, mask, end, emit, ctx);
            if (r > 0) {
                continue;
            }
            if (r == 0) {
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
    d->tracking = 0;
    d->pos = from;
    d->have_edge = 0;
    d->npre = 0;
    d->have_first = 0;
}
