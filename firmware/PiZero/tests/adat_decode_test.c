// Host test for adat_decode.c: builds ADAT frames as DSPi's adat_output.c lays them out, NRZI-codes them,
// samples the line at 125 MHz with a clock offset and random edge jitter, decodes, and checks every bit.
// Build and run (any scratch path): cc -O2 -I.. -o adt adat_decode_test.c ../adat_decode.c && ./adt

// Built for ARM with -DADAT_DECODE_A32 -DADAT_DECODE_TEST and ../adat_decode_a32.S (QEMU, as
// spdif_decode_a32_test.c says), it runs the assembly group loop and checks it against the C one.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adat_decode.h"

#define SAMPLE_HZ 125000000u
#define RING_WORDS (1u << 18)           // 8.4 M samples, 67 ms
#define OUT_WORDS (1u << 16)

static uint32_t ring[RING_WORDS];
static uint32_t out[OUT_WORDS], out_at[OUT_WORDS];
static uint32_t out2[OUT_WORDS], out2_at[OUT_WORDS];
static uint8_t bits[RING_WORDS * 4];    // the decoded bits sent, one a byte
static double bit_end[RING_WORDS * 4];  // when each bit's cell ends, in samples
static uint32_t nbits;

static uint32_t rng = 1;
static uint32_t rnd(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}
static double urand(void) {
    return (double)rnd() / 4294967296.0;
}

static void put_bits(uint32_t v, int n) {
    for (int i = n - 1; i >= 0; i--) {
        bits[nbits++] = (uint8_t)((v >> i) & 1u);
    }
}

// One frame: [1][10 zeros][1][4 user bits], then each channel as six [1][nibble], MSB first.
static void put_frame(uint32_t f) {
    put_bits(0x801u, 12);
    put_bits(f & 0xFu, 4);
    for (uint32_t ch = 0; ch < 8u; ch++) {
        uint32_t s = (f * 2654435761u + ch * 40503u) >> 8;     // 24 bits
        for (int k = 5; k >= 0; k--) {
            put_bits(0x10u | ((s >> (4 * k)) & 0xFu), 5);
        }
    }
}

// Samples the bits' NRZI line into the ring from sample `start`: each 1 bit toggles the line at the start
// of its cell, moved by up to +-jitter_ns, rises pwd_ns early and falls pwd_ns late (an optical receiver's
// pulse-width distortion). Samples before start (and after the bits) hold the line's level.
static void render(uint32_t fs, double ppm, double jitter_ns, double pwd_ns, uint32_t start, uint32_t level0) {
    double cell = (double)SAMPLE_HZ / (256.0 * fs * (1.0 + ppm * 1e-6));
    double jit = jitter_ns * 1e-9 * SAMPLE_HZ, pwd = pwd_ns * 1e-9 * SAMPLE_HZ;
    double t0 = start + urand() * cell;
    uint32_t level = level0, b = 0;
    double next = 1e30;
    while (b < nbits && !bits[b]) {
        b++;
    }
    if (b < nbits) {
        next = t0 + b * cell + jit * (2.0 * urand() - 1.0) + (level ? pwd : -pwd);
    }
    for (uint32_t i = 0; i < nbits; i++) {
        bit_end[i] = t0 + (i + 1) * cell;
    }
    memset(ring, 0, sizeof(ring));
    for (uint32_t n = 0; n < RING_WORDS * 32u; n++) {
        while ((double)n >= next) {
            level ^= 1u;
            b++;
            while (b < nbits && !bits[b]) {
                b++;
            }
            next = b < nbits ? t0 + b * cell + jit * (2.0 * urand() - 1.0) + (level ? pwd : -pwd) : 1e30;
        }
        if (level) {
            ring[n >> 5] |= 0x80000000u >> (n & 31u);
        }
    }
}

static void decode(adat_decoder_t *d, uint32_t fs, uint32_t *o, uint32_t *oat, int chunked) {
    memset(d, 0, sizeof(*d));
    d->out = o;
    d->out_at = oat;
    d->out_mask = OUT_WORDS - 1u;
    adat_decode_set_rate(d, SAMPLE_HZ, fs);
    adat_decode_reset(d, 0);
    uint64_t end = (uint64_t)RING_WORDS * 32u, at = 0;
    while (at < end) {
        uint64_t step = chunked ? 32u * (1u + rnd() % 8192u) : end;
        at = at + step < end ? at + step : end;
        adat_decode_ring(d, ring, RING_WORDS, at);
    }
}

static uint32_t out_bit(const uint32_t *o, uint32_t k) {
    return (o[k >> 5] >> (31u - (k & 31u))) & 1u;
}

// Where bit 0 of what was sent lies in the output, found from frame 8, by when the grid and the skew have
// settled (DSPi needs 9 good frames to lock anyway). NOWHERE if not found.
#define NOWHERE (-100000L)
#define CHECK_FROM (8u * 256u)
static long align(const adat_decoder_t *d, const uint32_t *o) {
    uint32_t total = d->out_n * 32u;
    for (uint32_t k = 0; k + 64u < total && k < 65536u + CHECK_FROM; k++) {
        uint32_t ok = 1;
        for (uint32_t i = 0; i < 64u && ok; i++) {
            ok = out_bit(o, k + i) == bits[CHECK_FROM + i];
        }
        if (ok) {
            return (long)k - (long)CHECK_FROM;
        }
    }
    return NOWHERE;
}

// DSPi's acquisition: a [1][10 zeros][1] pattern, then 8 more headers at the 256-bit stride.
static uint32_t dspi_locks(const adat_decoder_t *d, const uint32_t *o) {
    uint32_t total = d->out_n * 32u, locks = 0;
    for (uint32_t k = 0; k + 12u * 256u < total; k++) {
        uint32_t ok = 1;
        for (uint32_t f = 0; f < 9u && ok; f++) {
            uint32_t v = 0;
            for (uint32_t i = 0; i < 12u; i++) {
                v = v << 1 | out_bit(o, k + f * 256u + i);
            }
            ok = v == 0x801u;
        }
        if (ok) {
            locks++;
            k += 9u * 256u;
        }
    }
    return locks;
}

static int fail;

static void stream_case(const char *name, uint32_t fs, uint32_t dec_fs, double ppm, double jitter_ns, double pwd_ns,
                        uint32_t frames, uint32_t dark_samples) {
    nbits = 0;
    for (uint32_t f = 0; f < frames; f++) {
        put_frame(f);
    }
    render(fs, ppm, jitter_ns, pwd_ns, dark_samples, 0);
    adat_decoder_t d, d2;
    decode(&d, dec_fs, out, out_at, 0);
    decode(&d2, dec_fs, out2, out2_at, 1);
    // A quiet line is counted out at the ends of calls, so only its stamps may differ between the two.
    int same = d.out_n == d2.out_n && !memcmp(out, out2, d.out_n * 4u) &&
               (dark_samples || !memcmp(out_at, out2_at, d.out_n * 4u));
    uint32_t locks = dspi_locks(&d, out);
    long k0 = align(&d, out);
    if (dec_fs != fs) {
        int ok = locks == 0 && same;
        printf("%-44s %u words, %u DSPi locks, chunked same %d: %s\n", name, d.out_n, locks, same, ok ? "ok" : "FAIL");
        fail |= !ok;
        return;
    }
    uint32_t wrong = 0, checked = 0, late = 0, early = 0;
    double cell = (double)SAMPLE_HZ / (256.0 * fs);
    if (k0 != NOWHERE) {
        uint32_t total = d.out_n * 32u;
        for (uint32_t i = CHECK_FROM; i < nbits && (uint32_t)(k0 + i) < total; i++) {
            wrong += out_bit(out, (uint32_t)(k0 + i)) != bits[i];
            checked++;
            uint32_t k = (uint32_t)(k0 + i);
            if ((k & 31u) == 31u) {     // this bit completes an output word
                double lag = (double)out_at[k >> 5] / 256.0 - bit_end[i];     // under 2^24 samples here
                late += lag > 12.5 * cell;
                early += lag < -1.5 * cell;
            }
        }
    }
    int ok = k0 != NOWHERE && wrong == 0 && checked + CHECK_FROM + 600u > nbits && locks >= 1 && late == 0 && early == 0 && same;
    printf("%-44s %u bits checked, %u wrong, %u locks, stamps late %u early %u, chunked same %d: %s\n", name, checked,
           wrong, locks, late, early, same, ok ? "ok" : "FAIL");
    fail |= !ok;
}

static void quiet_case(const char *name, uint32_t level) {
    nbits = 0;
    render(48000, 0, 0, 0, 0, level);
    adat_decoder_t d;
    decode(&d, 48000, out, out_at, 1);
    double cells = (double)RING_WORDS * 32.0 * 256.0 / d.cell_q8 - 12.0;   // the decoder's own cell
    uint32_t nonzero = 0, backwards = 0;
    for (uint32_t i = 0; i < d.out_n; i++) {
        nonzero += out[i] != 0;
        backwards += i && (int32_t)(out_at[i] - out_at[i - 1]) < 0;     // modulo 2^32, as the receiver compares
    }
    double expect = cells / 32.0;
    int ok = nonzero == 0 && backwards == 0 && d.out_n + 1.0 > expect - 1.0 && d.out_n < expect + 1.0;
    printf("%-44s %u words of zeros (%.1f cells' worth), %u not zero, %u stamps backwards: %s\n", name, d.out_n, expect,
           nonzero, backwards, ok ? "ok" : "FAIL");
    fail |= !ok;
}

#ifdef ADAT_DECODE_A32
extern bool adat_decode_use_c;

// The assembly group loop against the C one: the same streams, chunked at random, must give the same words
// and stamps.
static void asm_case(const char *name, uint32_t fs, double ppm, double jitter_ns, double pwd_ns) {
    nbits = 0;
    for (uint32_t f = 0; f < 1500u; f++) {
        put_frame(f);
    }
    render(fs, ppm, jitter_ns, pwd_ns, 0, 0);
    adat_decoder_t d, d2;
    uint32_t seed = rng;
    adat_decode_use_c = true;
    decode(&d, fs, out, out_at, 1);
    rng = seed;
    adat_decode_use_c = false;
    decode(&d2, fs, out2, out2_at, 1);
    int same = d.out_n == d2.out_n && !memcmp(out, out2, d.out_n * 4u) && !memcmp(out_at, out2_at, d.out_n * 4u) &&
               d.flips == d2.flips && d.centre_q8 == d2.centre_q8;
    printf("%-44s %u words, assembly same as C %d: %s\n", name, d.out_n, same, same ? "ok" : "FAIL");
    fail |= !same;
}
#endif

int main(void) {
#ifdef ADAT_DECODE_A32
    asm_case("assembly against C, 48 kHz, jitter 15 ns", 48000, 300, 15, 0);
    asm_case("assembly against C, 44.1 kHz, pulse width 18 ns", 44100, -300, 3, 18);
    asm_case("assembly against C, 48 kHz, pulse width 22 ns", 48000, 0, 0, 22);
#endif
    stream_case("48 kHz, exact, no jitter", 48000, 48000, 0, 0, 0, 3000, 0);
    stream_case("44.1 kHz, exact, no jitter", 44100, 44100, 0, 0, 0, 2800, 0);
    stream_case("48 kHz, +1000 ppm, jitter 15 ns", 48000, 48000, 1000, 15, 0, 3000, 0);
    stream_case("48 kHz, -1000 ppm, jitter 15 ns", 48000, 48000, -1000, 15, 0, 3000, 0);
    stream_case("44.1 kHz, +1000 ppm, jitter 15 ns", 44100, 44100, 1000, 15, 0, 2800, 0);
    stream_case("44.1 kHz, -1000 ppm, jitter 15 ns", 44100, 44100, -1000, 15, 0, 2800, 0);
    stream_case("48 kHz, +100 ppm, jitter 17 ns", 48000, 48000, 100, 17, 0, 3000, 0);
    stream_case("48 kHz, pulse width +-17 ns, jitter 3 ns", 48000, 48000, 50, 3, 17, 3000, 0);
    stream_case("44.1 kHz, pulse width +-15 ns, jitter 5 ns", 44100, 44100, -50, 5, 15, 2800, 0);
    stream_case("48 kHz after 2 ms of a dark line", 48000, 48000, 40, 10, 0, 2900, 250000);
    stream_case("48 kHz stream read with the 44.1 kHz cell", 48000, 44100, 0, 5, 0, 3000, 0);
    stream_case("44.1 kHz stream read with the 48 kHz cell", 44100, 48000, 0, 5, 0, 2800, 0);
    quiet_case("dark line, low", 0);
    quiet_case("dark line, high", 1);
    puts(fail ? "FAIL" : "PASS");
    return fail;
}
