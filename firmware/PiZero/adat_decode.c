#include "adat_decode.h"

// Samples of look-ahead a group of 32 cells needs past its last cell's centre: its edge window reads 32
// samples from the centre of a cell before it.
#define AHEAD_SAMPLES 64u
#define ADAT_FAR_GROUPS 4u      // groups in a row whose two edges lie over half a cell apart

void adat_decode_reset(adat_decoder_t *d, uint64_t from) {
    d->pos = from & ~(uint64_t)31u;
    d->centre_q8 = (d->pos << 8) + d->cell_q8 / 2u;
    d->level = 2u;      // not known yet: the first cell takes its own level
    d->far = 0;
}

void adat_decode_set_rate(adat_decoder_t *d, uint32_t sample_hz, uint32_t fs) {
    d->cell_q8 = (uint32_t)(((uint64_t)sample_hz + fs / 2u) / fs);     // sample_hz * 256 / (256 fs)
}

// How far the edge that starts the cell centred at ck lies from the grid's boundary before ck, in
// samples << 8: where the samples from the previous cell's centre first differ from it, less half a
// sample. The two centres' levels differ, so the edge lies between them, within 32 samples.
static inline int32_t edge_offset(const uint32_t *ring, uint32_t mask, uint32_t ck, uint32_t cell) {
    uint32_t c0 = ck - cell, s = c0 >> 8;
    uint64_t two = (uint64_t)ring[(s >> 5) & mask] << 32 | ring[((s >> 5) + 1u) & mask];
    uint32_t win = (uint32_t)((two << (s & 31u)) >> 32);
    win ^= (uint32_t)((int32_t)win >> 31);  // set where the samples differ from the first
    return (int32_t)((((s + (uint32_t)__builtin_clz(win | 1u)) << 8) - 128u) - (c0 + cell / 2u));
}

static inline uint32_t sample(const uint32_t *ring, uint32_t mask, uint32_t q8) {
    return (ring[(q8 >> 13) & mask] << ((q8 >> 8) & 31u)) >> 31;
}

// One call's state for the group loop, shared with adat_decode_a32.S (whose offsets follow it).
typedef struct {
    const uint32_t *ring;
    uint32_t mask;          // its word mask
    uint32_t cell;
    uint32_t qi;            // ~ the next group's first centre
    int32_t room;           // samples << 8 left past it, less the look-ahead
    uint32_t level, far, n, omask;
    uint32_t *out, *out_at;
    uint32_t flips, half, group, k;     // k: half + 383, for the edge windows
} adat_slice_t;
void adat_decode_groups(adat_slice_t *p);

#ifdef ADAT_DECODE_TEST
bool adat_decode_use_c;     // the test runs the C loop on ARM too, to compare the two
#endif

#if !defined(ADAT_DECODE_A32) || defined(ADAT_DECODE_TEST)
// The group loop in C, as adat_decode_a32.S does it: groups while the room holds one.
static void decode_groups_c(adat_slice_t *p, const uint32_t *ring, uint32_t mask) {
    uint32_t cell = p->cell, c = ~p->qi;
    while (p->room >= (int32_t)p->group) {
        // The levels at the centres of 32 cells, the first in bit 31; a cell holds a 1 when its level
        // differs from the cell's before it.
        uint32_t lv = 0;
        for (uint32_t k = 0; k < 32u; k++) {
            lv = lv << 1 | sample(ring, mask, c + k * cell);
        }
        uint32_t word = lv ^ (lv >> 1 | p->level << 31);
        p->level = lv & 1u;
        c += p->group;
        p->room -= (int32_t)p->group;
        p->out[p->n & p->omask] = word;
        p->out_at[p->n & p->omask] = c - cell;          // the last cell's centre
        p->n++;
        // The grid moves halfway to the mean of the group's last two edges, which rise and fall in turn,
        // so the optical receiver's pulse-width distortion cancels. That distortion also holds a grid
        // whose centres sit on the edges, where its two edges come nearly a cell apart instead of twice
        // the distortion: a few groups like that in a row move the grid half a cell, onto the centres.
        uint32_t rest = word & (word - 1u);
        if (rest) {
            uint32_t k1 = (uint32_t)__builtin_ctz(word), k2 = (uint32_t)__builtin_ctz(rest);
            int32_t r1 = edge_offset(ring, mask, c - (k1 + 1u) * cell, cell);
            int32_t r2 = edge_offset(ring, mask, c - (k2 + 1u) * cell, cell);
            uint32_t apart = (uint32_t)(r1 > r2 ? r1 - r2 : r2 - r1);
            p->far = apart > p->half ? p->far + 1u : 0u;
            int32_t step = (r1 + r2) >> 2;
            if (p->far >= ADAT_FAR_GROUPS) {
                step += (int32_t)p->half;
                p->far = 0;
                p->flips++;
            }
            c += (uint32_t)step;
            p->room -= step;
        }
    }
    p->qi = ~c;
}
#endif

void adat_decode_ring(adat_decoder_t *d, const uint32_t *ring, uint32_t ring_words, uint64_t end) {
    uint32_t mask = ring_words - 1u, cell = d->cell_q8;
    adat_slice_t p = {.ring = ring, .mask = mask, .cell = cell, .level = d->level, .far = d->far,
                      .n = d->out_n, .omask = d->out_mask, .out = d->out, .out_at = d->out_at, .flips = d->flips,
                      .half = cell / 2u, .group = 32u * cell, .k = cell / 2u + 383u};
    uint64_t centre = d->centre_q8;
    // A long call goes a slice at a time, so that the room and the centre's low half stay in 32 bits.
    for (;;) {
        int64_t room = (int64_t)(end << 8) - (int64_t)centre - (int64_t)(AHEAD_SAMPLES << 8);
        if (room < (int64_t)p.group) {
            break;
        }
        uint32_t c = (uint32_t)centre;
        if (p.level > 1u) {
            p.level = sample(ring, mask, c);    // the first cell takes its own level
        }
        p.qi = ~c;
        p.room = room > 0x40000000 ? 0x40000000 : (int32_t)room;
#if defined(ADAT_DECODE_A32) && defined(ADAT_DECODE_TEST)
        if (adat_decode_use_c) {
            decode_groups_c(&p, ring, mask);
        } else {
            adat_decode_groups(&p);
        }
#elif defined(ADAT_DECODE_A32)
        adat_decode_groups(&p);
#else
        decode_groups_c(&p, ring, mask);
#endif
        centre += (uint64_t)(int64_t)(int32_t)(~p.qi - c);    // under 2^31 either way
    }
    d->centre_q8 = centre;
    d->pos = (centre >> 8) & ~(uint64_t)31u;
    d->level = p.level;
    d->far = p.far;
    d->out_n = p.n;
    d->flips = p.flips;
}
