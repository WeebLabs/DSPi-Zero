// Host test for i2s_rx_decode.c. It models four I2S data lines with BCK and LRCLK as signals in time,
// takes GPLEV0 snapshots of them at the uneven spacing the DMA gives, and checks that every word comes
// out, through the plain decoder and the fast path alike, across control block gaps and capture stalls.

// Build and run (any scratch path): cc -O2 -I.. -o i2st i2s_rx_decode_test.c ../i2s_rx_decode.c && ./i2st
// That runs the C scan and frame builder. Built as below it runs the assembly under QEMU, started as
// spdif_decode_a32_test.c says, and prints PASS.

//   arm-none-eabi-gcc -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -marm -O2 -I.. -DI2S_RX_A32
//     --specs=rdimon.specs -Wl,-e,qemu_start -Wl,-Ttext-segment=0x40010000 -o i2st.elf
//     i2s_rx_decode_test.c ../i2s_rx_decode.c ../i2s_rx_decode_a32.S qemu_start.S

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "i2s_rx_decode.h"

#define RING 16384u
#define MAX_SAMPLES 400000u

static uint32_t rings[2][I2S_RX_LINES][RING];
static uint32_t samples[MAX_SAMPLES + 64];
static uint32_t sent[RING][I2S_RX_LINES];     // word w of line k; even words are left
static int fails;

static uint32_t rnd(void) {
    static uint32_t s = 12345u;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

static double frand(void) {
    return (double)(rnd() >> 8) / 16777216.0;
}

typedef struct {
    double bck_ns;      // BCK period
    double step_ns;     // snapshot spacing, with step_extra_ns more before every fourth
    double step_extra_ns, jitter_ns, tco_ns;
    double hiccup_ns;   // an occasional longer step, 0 for none
    double stall_ns;    // a step about this long every stall_every snapshots or so, 0 for none
    uint32_t stall_every;
    double gap_ns;      // a control block gap about every gap_every snapshots, 0 for none
    uint32_t gap_every;
    i2s_rx_pins_t pins;
} model_t;

// The level of every pin at time t. A bit cell starts at BCK's falling edge; the data and LRCLK of
// cell n appear tco after it. LRCLK is low for the left word and changes one cell early, as Philips
// I2S does.
static uint32_t pins_at(const model_t *m, double t) {
    double cell = t / m->bck_ns;
    uint64_t n = (uint64_t)cell;
    double in = (cell - (double)n) * m->bck_ns;
    uint32_t bck = in >= m->bck_ns / 2.0;
    uint64_t dn = in >= m->tco_ns ? n : n - 1u;    // the cell whose data is on the pins
    uint32_t word = (uint32_t)(dn / 32u), bit = 31u - (uint32_t)(dn % 32u);
    uint32_t lr = (uint32_t)(((dn + 1u) / 32u) & 1u);
    uint32_t used = 1u << m->pins.bck | 1u << m->pins.lr;
    for (uint32_t k = 0; k < I2S_RX_LINES; k++) {
        used |= 1u << m->pins.data[k];
    }
    uint32_t v = rnd() & ~used;
    for (uint32_t k = 0; k < I2S_RX_LINES; k++) {
        v |= ((sent[word % RING][k] >> bit) & 1u) << m->pins.data[k];
    }
    return v | bck << m->pins.bck | lr << m->pins.lr;
}

enum { CLEAN, NOTHING_SILENT, AGREE };

// Runs one model through both decoders, in random pieces of one unbroken capture, and checks the rings
// against what was sent. CLEAN wants every word right. NOTHING_SILENT allows concealed frames (a repeat
// of the frame before) but no other wrong word, bit 0 aside, which a stall can cost. AGREE only wants
// the two decoders to do the same.
static void run(const char *name, const model_t *m, uint32_t n_samples, int want) {
    for (uint32_t w = 0; w < RING; w++) {
        for (uint32_t k = 0; k < I2S_RX_LINES; k++) {
            uint32_t r = rnd();
            sent[w][k] = r % 5u == 0 ? 0u : r % 7u == 0 ? 0xFFFFFFFFu : rnd();
        }
    }
    i2s_rx_t *d = calloc(2, sizeof(i2s_rx_t));
    uint32_t *rp[2][I2S_RX_LINES];
    double mean_step = m->step_ns + m->step_extra_ns / 4.0;
    for (uint32_t v = 0; v < 2u; v++) {
        for (uint32_t k = 0; k < I2S_RX_LINES; k++) {
            rp[v][k] = rings[v][k];
        }
        memset(rings[v], 0, sizeof(rings[v]));
        i2s_rx_init(&d[v], &m->pins, rp[v], RING, (uint32_t)(256.0 * m->bck_ns / mean_step));
    }
    // The capture, with a note of where each gap falls.
    static uint8_t gap_before[MAX_SAMPLES + 1];
    memset(gap_before, 0, sizeof(gap_before));
    double t = 64.0 * m->bck_ns + frand() * m->bck_ns * 64.0;
    uint32_t next_gap = m->gap_every ? m->gap_every / 2u + rnd() % m->gap_every : 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n_samples; i++) {
        if (i == next_gap) {
            // The gap replaces one step, as a control block reload does.
            t += m->gap_ns * (0.85 + 0.3 * frand()) - m->step_ns;
            gap_before[i] = 1;
            next_gap += m->gap_every / 2u + rnd() % m->gap_every;
        }
        samples[i] = pins_at(m, t);
        t += m->step_ns + (i % 4u == 3u ? m->step_extra_ns : 0.0) + (frand() - 0.5) * m->jitter_ns;
        if (m->hiccup_ns != 0.0 && rnd() % 500u == 0u) {
            t += frand() * m->hiccup_ns;
        }
        if (m->stall_ns != 0.0 && rnd() % m->stall_every == 0u) {
            t += m->stall_ns * (0.85 + 0.3 * frand()) - m->step_ns;
        }
    }
    for (uint32_t done = 0; done < n_samples;) {
        uint32_t piece = 1u + rnd() % (rnd() % 4u ? 3000u : 70u);
        if (piece > n_samples - done) {
            piece = n_samples - done;
        }
        for (uint32_t i = 1; i < piece; i++) {
            if (gap_before[done + i]) {
                piece = i;
            }
        }
        if (gap_before[done] && done) {
            uint32_t back = done < 48u ? done : 48u;
            i2s_rx_gap(&d[0], &samples[done - back], back);
            i2s_rx_gap(&d[1], &samples[done - back], back);
        }
        i2s_rx_decode_ref(&d[0], &samples[done], piece);
        i2s_rx_decode(&d[1], &samples[done], piece);
        done += piece;
    }
    i2s_rx_flush(&d[1]);
    bool same = d[0].wr == d[1].wr && d[0].frames == d[1].frames && d[0].bad_halves == d[1].bad_halves &&
                d[0].concealed == d[1].concealed && d[0].repaired == d[1].repaired &&
                d[0].rescued == d[1].rescued && memcmp(rings[0], rings[1], sizeof(rings[0])) == 0;
    // The first frame decoded is the first whole one after the start; find it among what was sent,
    // going by a frame a little way in, in case a stall spoiled the first.
    uint32_t wrong = 0, silent = 0, first = 0xFFFFFFFFu;
    for (uint32_t at = 0; at < 64u && first == 0xFFFFFFFFu; at += 8u) {
        for (uint32_t w = at; w < at + 400u && first == 0xFFFFFFFFu; w += 2u) {
            bool hit = true;
            for (uint32_t k = 0; k < I2S_RX_LINES; k++) {
                hit = hit && rings[0][k][at] == sent[w][k] && rings[0][k][at + 1u] == sent[w + 1u][k];
            }
            if (hit) {
                first = w - at;
            }
        }
    }
    uint32_t words = d[0].wr < RING - 512u ? d[0].wr : RING - 512u;
    for (uint32_t w = 0; first != 0xFFFFFFFFu && w < words; w += 2u) {
        uint32_t bad = 0, major = 0, repeat = w >= 2u;
        for (uint32_t k = 0; k < I2S_RX_LINES; k++) {
            for (uint32_t c = 0; c < 2u; c++) {
                uint32_t got = rings[0][k][w + c], exp = sent[(first + w + c) % RING][k];
                bad += got != exp;
                major += (got | 1u) != (exp | 1u);
                repeat = repeat && got == rings[0][k][w + c - 2u];
            }
        }
        wrong += bad;
        silent += repeat ? 0u : major;
    }
    bool clean = first != 0xFFFFFFFFu && wrong == 0u && d[0].bad_halves == 0u && d[0].concealed == 0u;
    bool pass = same && d[0].frames > 8u &&
                (want == AGREE || (want == CLEAN && clean) ||
                 (want == NOTHING_SILENT && first != 0xFFFFFFFFu && silent == 0u));
    printf("%-34s %5u frames, %u bad halves, %u concealed, %u of %u gaps repaired, %u stalls put right, "
           "%u wrong words (%u outside concealed frames), fast %s: %s\n",
           name, d[0].frames, d[0].bad_halves, d[0].concealed, d[0].repaired, d[0].gaps, d[0].rescued, wrong, silent,
           same ? "same" : "DIFFERS", pass ? "ok" : "FAIL");
    fails += !pass;
    free(d);
}

// Arbitrary snapshots: nothing to decode, but both decoders must do the same thing with them.
static void fuzz(uint32_t rounds) {
    static const i2s_rx_pins_t pins = {.shift = 8, .bck = 12, .lr = 13, .data = {8, 9, 10, 11}};
    uint32_t differ = 0, total = 0;
    for (uint32_t r = 0; r < rounds; r++) {
        i2s_rx_t *d = calloc(2, sizeof(i2s_rx_t));
        uint32_t *rp[2][I2S_RX_LINES];
        for (uint32_t v = 0; v < 2u; v++) {
            for (uint32_t k = 0; k < I2S_RX_LINES; k++) {
                rp[v][k] = rings[v][k];
            }
            memset(rings[v], 0, sizeof(rings[v]));
            i2s_rx_init(&d[v], &pins, rp[v], RING, 1290);
        }
        // BCK toggles every 1 to 3 snapshots and LRCLK about every 32 rises, with slips now and then.
        uint32_t bck = 0, lr = 0, left = 1, rises = 0, lr_len = 32;
        for (uint32_t done = 0; done < 60000u;) {
            uint32_t piece = 1u + rnd() % 2500u;
            for (uint32_t i = 0; i < piece; i++) {
                if (--left == 0u) {
                    bck ^= 1u;
                    left = 1u + rnd() % 3u;
                    if (!bck && ++rises >= lr_len) {
                        rises = 0;
                        lr ^= 1u;
                        lr_len = rnd() % 40u ? 32u : 28u + rnd() % 8u;
                    }
                }
                samples[i] = (rnd() & ~0x3000u) | bck << 12 | lr << 13;
            }
            i2s_rx_decode_ref(&d[0], samples, piece);
            i2s_rx_decode(&d[1], samples, piece);
            if (rnd() % 8u == 0u) {
                uint32_t back = piece < 48u ? piece : 48u;
                i2s_rx_gap(&d[0], samples + piece - back, back);
                i2s_rx_gap(&d[1], samples + piece - back, back);
            }
            done += piece;
        }
        i2s_rx_flush(&d[1]);
        differ += !(d[0].wr == d[1].wr && d[0].frames == d[1].frames && d[0].bad_halves == d[1].bad_halves &&
                    d[0].concealed == d[1].concealed && d[0].repaired == d[1].repaired &&
                    d[0].rescued == d[1].rescued &&
                    memcmp(rings[0], rings[1], sizeof(rings[0])) == 0);
        total += d[0].frames;
        free(d);
    }
    printf("%-34s %u rounds, %u frames, %u rounds differ: %s\n", "arbitrary clocks, fast against plain",
           rounds, total, differ, differ ? "FAIL" : "ok");
    fails += differ != 0u;
}

// A ring lead (DSPi's master role reads pair 0 one frame on) must move that line's words and no others.
static void lead(uint32_t rounds) {
    static const i2s_rx_pins_t pins = {.shift = 8, .bck = 12, .lr = 13, .data = {8, 9, 10, 11}};
    uint32_t differ = 0, total = 0;
    for (uint32_t r = 0; r < rounds; r++) {
        i2s_rx_t *d = calloc(2, sizeof(i2s_rx_t));
        uint32_t *rp[2][I2S_RX_LINES];
        for (uint32_t v = 0; v < 2u; v++) {
            for (uint32_t k = 0; k < I2S_RX_LINES; k++) {
                rp[v][k] = rings[v][k];
            }
            memset(rings[v], 0, sizeof(rings[v]));
            i2s_rx_init(&d[v], &pins, rp[v], RING, 1290);
        }
        d[1].lead[0] = 2;
        uint32_t bck = 0, lr = 0, left = 1, rises = 0, lr_len = 32;
        for (uint32_t done = 0; done < 60000u;) {
            uint32_t piece = 1u + rnd() % 2500u;
            for (uint32_t i = 0; i < piece; i++) {
                if (--left == 0u) {
                    bck ^= 1u;
                    left = 1u + rnd() % 3u;
                    if (!bck && ++rises >= lr_len) {
                        rises = 0;
                        lr ^= 1u;
                        lr_len = rnd() % 40u ? 32u : 28u + rnd() % 8u;
                    }
                }
                samples[i] = (rnd() & ~0x3000u) | bck << 12 | lr << 13;
            }
            i2s_rx_decode_ref(&d[0], samples, piece);
            i2s_rx_decode(&d[1], samples, piece);
            done += piece;
        }
        i2s_rx_flush(&d[1]);
        bool same = d[0].wr == d[1].wr && d[0].concealed == d[1].concealed && d[0].wr < RING - 2u;
        for (uint32_t w = 0; same && w < d[0].wr; w++) {
            same = rings[1][0][w + 2u] == rings[0][0][w];
        }
        for (uint32_t k = 1; same && k < I2S_RX_LINES; k++) {
            same = memcmp(rings[0][k], rings[1][k], sizeof(rings[0][k])) == 0;
        }
        differ += !same;
        total += d[0].frames;
        free(d);
    }
    printf("%-34s %u rounds, %u frames, %u rounds differ: %s\n", "line 0 a frame ahead in its ring",
           rounds, total, differ, differ ? "FAIL" : "ok");
    fails += differ != 0u;
}

int main(void) {
    static const i2s_rx_pins_t loop = {.shift = 8, .bck = 12, .lr = 13, .data = {8, 9, 10, 11}};
    static const i2s_rx_pins_t own = {.shift = 12, .bck = 12, .lr = 13, .data = {22, 23, 24, 25}};
    static const i2s_rx_pins_t ext = {.shift = 12, .bck = 26, .lr = 27, .data = {22, 23, 24, 25}};
    static const struct {
        const char *name;
        double fs, step, extra;
    } rates[] = {{"44.1 kHz, 30.5 MS/s", 44100, 31.5, 5.5}, {"48 kHz, 30.5 MS/s", 48000, 31.5, 5.5},
                 {"96 kHz, 30.5 MS/s", 96000, 31.5, 5.5},   {"44.1 kHz, 18 MS/s", 44100, 50, 22},
                 {"48 kHz, 18 MS/s", 48000, 50, 22},        {"48 kHz, 13.4 MS/s", 48000, 62, 50}};
    char name[80];
    // BCK in every nibble, byte and half of the word, which are the assembly scan's eight cases, and the
    // data lines in other orders and spread over up to 8 GPIOs.
    static const i2s_rx_pins_t spread[] = {
        {0, 2, 3, {4, 5, 6, 7}},         {0, 4, 5, {8, 11, 9, 10}},       {8, 9, 10, {15, 14, 13, 12}},
        {8, 12, 13, {11, 8, 10, 9}},     {16, 18, 19, {20, 27, 22, 25}},  {16, 22, 23, {26, 24, 27, 25}},
        {12, 26, 27, {22, 23, 24, 25}},  {16, 30, 31, {24, 29, 26, 28}}};
    for (unsigned v = 0; v < sizeof(spread) / sizeof(spread[0]); v++) {
        model_t m = {.bck_ns = 1e9 / (64.0 * 96000), .step_ns = 31.5, .step_extra_ns = 5.5, .jitter_ns = 4,
                     .tco_ns = 6, .gap_ns = 90, .gap_every = 3000, .pins = spread[v]};
        snprintf(name, sizeof(name), "96 kHz, gaps, BCK on GPIO %d", spread[v].bck);
        run(name, &m, 150000, CLEAN);
    }
    for (unsigned r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
        model_t m = {.bck_ns = 1e9 / (64.0 * rates[r].fs), .step_ns = rates[r].step, .step_extra_ns = rates[r].extra,
                     .jitter_ns = 4, .tco_ns = 6, .pins = r % 3u == 0 ? loop : r % 3u == 1 ? own : ext};
        run(rates[r].name, &m, 300000, CLEAN);
        m.hiccup_ns = 25;
        snprintf(name, sizeof(name), "%s, hiccups", rates[r].name);
        run(name, &m, 300000, CLEAN);
        // A control block reload: one step of 70 to 110 ns in place of a normal one, which at 96 kHz
        // can swallow a BCK phase. Every one must be repaired.
        m.hiccup_ns = 0;
        m.gap_ns = 90;
        m.gap_every = 3000;
        snprintf(name, sizeof(name), "%s, gaps", rates[r].name);
        run(name, &m, 300000, CLEAN);
    }
    // Stalls of about one BCK phase anywhere in the capture, as the board shows at 96 kHz while audio
    // plays. A lost bit clock is put back from the snapshots, or the frame is concealed; nothing else
    // may come out wrong.
    for (unsigned r = 0; r < 6u; r++) {
        model_t m = {.bck_ns = 1e9 / (64.0 * 96000), .step_ns = 31.5, .step_extra_ns = 3, .jitter_ns = 3,
                     .tco_ns = r % 4u < 2u ? 6 : 20, .stall_ns = r > 3u ? 145 : r % 2u ? 125 : 105,
                     .stall_every = 1500, .pins = r % 2u ? ext : loop};
        snprintf(name, sizeof(name), "96 kHz, stalls %d ns, tco %d ns", (int)m.stall_ns, (int)m.tco_ns);
        run(name, &m, 380000, NOTHING_SILENT);
    }
    model_t both = {.bck_ns = 1e9 / (64.0 * 96000), .step_ns = 31.5, .step_extra_ns = 3, .jitter_ns = 3, .tco_ns = 6,
                    .stall_ns = 105, .stall_every = 2500, .gap_ns = 90, .gap_every = 3000, .pins = loop};
    run("96 kHz, stalls and gaps", &both, 380000, AGREE);    // a stall beside a gap is taken for the gap
    // A gap longer than a BCK period can lose a bit for good: the decoders must still agree.
    model_t worst = {.bck_ns = 1e9 / (64.0 * 96000), .step_ns = 31.5, .step_extra_ns = 5.5, .jitter_ns = 4,
                     .tco_ns = 6, .gap_ns = 190, .gap_every = 2500, .pins = loop};
    run("96 kHz, gaps over a BCK period", &worst, 300000, AGREE);
    fuzz(40);
    lead(10);
    // Last, so that the cases above keep their random sequences. The reload as the board has it with
    // 128-bit writes: hardly longer than a step, and often, with a bit clock of just over five snapshots.
    // No rise is lost, and none may be taken for lost.
    model_t brief = {.bck_ns = 1e9 / (64.0 * 96000), .step_ns = 31.0, .step_extra_ns = 5, .jitter_ns = 4,
                     .tco_ns = 6, .gap_ns = 40, .gap_every = 300, .pins = loop};
    run("96 kHz, brief gaps", &brief, 380000, CLEAN);
    printf(fails ? "FAIL\n" : "PASS\n");
    return fails != 0;
}
