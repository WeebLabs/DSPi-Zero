// Host test for spdif_decode.c: encodes known stereo samples as S/PDIF (preambles, biphase-mark, parity),
// samples the line at 125 MHz with random edge jitter, decodes, and checks every subframe.
// Build and run (any scratch path): cc -O2 -I.. -o sdt spdif_decode_test.c ../spdif_decode.c && ./sdt
// That runs the C reader; spdif_decode_a32_test.c covers the ARM assembly one.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "spdif_decode.h"

#ifndef SAMPLE_HZ
#define SAMPLE_HZ 125000000.0
#endif

typedef struct {
    uint32_t got[4096];
    uint64_t at[4096];
    uint32_t n;
} sink_t;

static void emit(uint32_t word, uint64_t at, void *ctx) {
    sink_t *s = ctx;
    if (s->n < 4096) {
        s->at[s->n] = at;
        s->got[s->n++] = word;
    }
}

// Subframe time slots 4-31 for a 24-bit sample, with V=0, U=0, C from the block, and even parity.
static uint32_t slots(int32_t sample, int c) {
    uint32_t w = ((uint32_t)sample & 0xFFFFFFu) << 4 | (uint32_t)c << 30;
    uint32_t v = w >> 4, p = 0;
    while (v) {
        p ^= v & 1u;
        v >>= 1;
    }
    return w | p << 31;
}

// Appends the subframe's 64 cell levels to cells[], continuing from *level.
static void encode(uint8_t *cells, uint32_t *nc, uint32_t sync, uint32_t w, uint32_t *level) {
    static const uint8_t pre[3][8] = {{1, 1, 1, 0, 1, 0, 0, 0}, {1, 1, 1, 0, 0, 0, 1, 0}, {1, 1, 1, 0, 0, 1, 0, 0}};
    int idx = sync == SPDIF_SYNC_B ? 0 : sync == SPDIF_SYNC_M ? 1 : 2;
    // Preambles are written for a line that was low; invert them if it was high.
    for (int i = 0; i < 8; i++) {
        cells[(*nc)++] = pre[idx][i] ^ *level;
    }
    uint32_t l = cells[*nc - 1];
    for (int s = 4; s < 32; s++) {
        l ^= 1u;
        cells[(*nc)++] = (uint8_t)l;
        if (w >> s & 1u) {
            l ^= 1u;
        }
        cells[(*nc)++] = (uint8_t)l;
    }
    *level = l;
}

static int run(uint32_t rate, double jitter_cells, uint32_t frames, uint64_t seed) {
    srand((unsigned)seed);
    uint32_t nc = 0, level = 0;
    uint8_t *cells = malloc(frames * 128u + 64u);
    int32_t *want_l = malloc(frames * 4u), *want_r = malloc(frames * 4u);
    for (uint32_t f = 0; f < frames; f++) {
        want_l[f] = (int32_t)(sin(f * 0.13) * 8000000) & 0xFFFFFF;
        want_r[f] = (int32_t)(rand() & 0xFFFFFF);
        int c = (f % 192) == 1;     // one C bit set in each block
        encode(cells, &nc, f % 192 == 0 ? SPDIF_SYNC_B : SPDIF_SYNC_M, slots(want_l[f], c), &level);
        encode(cells, &nc, SPDIF_SYNC_W, slots(want_r[f], c), &level);
    }
    // Sample the line: each cell boundary moves by up to +-jitter cells.
    double cell_s = 1.0 / (rate * 128.0);
    double t_end = nc * cell_s;
    uint32_t nsamp = (uint32_t)(t_end * SAMPLE_HZ);
    uint32_t nw = nsamp / 32u;
    uint32_t *words = calloc(nw + 1, 4);
    double *edges = malloc((nc + 1) * sizeof(double));
    for (uint32_t i = 0; i <= nc; i++) {
        edges[i] = (i + jitter_cells * ((rand() / (double)RAND_MAX) * 2 - 1)) * cell_s;
    }
    uint32_t ci = 0;
    for (uint32_t s = 0; s < nw * 32u; s++) {
        double t = s / SAMPLE_HZ;
        while (ci + 1 < nc && t >= edges[ci + 1]) {
            ci++;
        }
        if (cells[ci]) {
            words[s / 32u] |= 0x80000000u >> (s % 32u);
        }
    }
    sink_t sink = {0};
    spdif_decoder_t d;
    spdif_decode_reset(&d);
    // Feed through a ring a chunk at a time, as the DMA does, with the ring's end moving in uneven steps.
    enum { RING_WORDS = 1u << 15 };
    static uint32_t ring[RING_WORDS + SPDIF_RING_TAIL];
    uint64_t end = 0;
    for (uint32_t k = 0; k < nw;) {
        uint32_t m = 1 + rand() % 4096;
        if (k + m > nw) {
            m = nw - k;
        }
        for (uint32_t j = 0; j < m; j++) {
            uint32_t at = (k + j) & (RING_WORDS - 1u);
            ring[at] = words[k + j];
            if (at < SPDIF_RING_TAIL) {
                ring[RING_WORDS + at] = words[k + j];   // the tail the decoder reads past the end
            }
        }
        k += m;
        end = (uint64_t)k * 32u;
        spdif_decode_ring(&d, ring, RING_WORDS, end, emit, &sink);
    }
    // Find the first decoded subframe in the sent stream and compare from there.
    int bad = 0, first = -1;
    for (uint32_t i = 0; i < sink.n && first < 0; i++) {
        if ((sink.got[i] & 0xF) == SPDIF_SYNC_B) {
            first = (int)i;
        }
    }
    uint32_t checked = 0;
    if (first >= 0) {
        uint32_t f = 192;   // the second block: the decoder measures the cell during the first
        for (uint32_t i = (uint32_t)first; i + 1 < sink.n && f < frames; i += 2, f++) {
            uint32_t l = sink.got[i], r = sink.got[i + 1];
            if ((l & 0xF) != (f % 192 == 0 ? SPDIF_SYNC_B : SPDIF_SYNC_M) || (r & 0xF) != SPDIF_SYNC_W ||
                ((l >> 4) & 0xFFFFFF) != (uint32_t)want_l[f] || ((r >> 4) & 0xFFFFFF) != (uint32_t)want_r[f] ||
                l != (slots(want_l[f], f % 192 == 1) | (l & 0xF)) ) {
                bad++;
            }
            checked++;
        }
    }
    double cell_samples = SAMPLE_HZ * cell_s;
    printf("%6u Hz, jitter +-%.2f cells (%.1f samples a cell): %u subframes, %u frames checked, %d bad, "
           "%u decoder errors, cell estimate %.3f\n", rate, jitter_cells, cell_samples, sink.n, checked, bad,
           d.errors, d.cell_q8 / 256.0);
    free(cells), free(want_l), free(want_r), free(words), free(edges);
    return bad || checked < frames - 400u;
}

int main(void) {
    int fail = 0;
    uint32_t rates[] = {44100, 48000, 96000};
    for (int i = 0; i < 3; i++) {
        fail |= run(rates[i], 0.0, 2000, 1);
        fail |= run(rates[i], 0.2, 2000, 2);
    }
    puts(fail ? "FAIL" : "PASS");
    return fail;
}
