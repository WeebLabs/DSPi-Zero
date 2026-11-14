// Checks smi_frame.h's smi_frames_c against the table-driven composer it replaced (kept here as the
// reference), and smi_frames_a32 against both: random ring frames for every mix of slot types, with and
// without the I2S clocks and ADAT, must give the same 256 words, and the key must skip only a repeat.
// For the Mac it builds without the assembly: cc -O2 -I.. smi_frame_test.c

// S/PDIF slots are checked against the cell format their rings held before (bring-up section 28). The
// same samples go through the old encoder, kept here, whose NRZI cells the reference turns into words,
// and through smi_spdif_write, whose ring words the composer encodes. Both must give the same words.

// ADAT is checked the same way (bring-up section 29). DSPi's encoder (adat_output.c), kept here, makes
// the line-level words the ADAT ring held before, which the reference plays. smi_adat_write makes the
// ring frames the composer now encodes. Both must give the same words and leave the same line level.

// Build for QEMU, to any scratch path:
//   arm-none-eabi-gcc -mcpu=cortex-a53 -mfpu=neon-fp-armv8 -mfloat-abi=hard -marm -O2 -I..
//     --specs=rdimon.specs -Wl,-e,qemu_start -Wl,-Ttext-segment=0x40010000 -o smif.elf smi_frame_test.c
//     ../smi_frame_a32.S qemu_start.S

// Run (it prints PASS):
//   qemu-system-arm -M virt -cpu max -m 512 -semihosting-config enable=on,target=native
//     -display none -serial none -monitor none -kernel smif.elf

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../smi_frame.h"

static uint8_t lut_cell2[256][16], lut_bck4[256][32], clock_bytes[256], lut_msb8[256][8];

static void tables_init(void) {
    for (uint32_t v = 0; v < 256u; v++) {
        for (uint32_t j = 0; j < 8u; j++) {
            uint8_t b = (uint8_t)((v >> j) & 1u);
            lut_cell2[v][2u * j] = lut_cell2[v][2u * j + 1u] = b;
            for (uint32_t k = 0; k < 4u; k++) {
                lut_bck4[v][4u * j + k] = b;
            }
            lut_msb8[v][j] = (uint8_t)((v >> (7u - j)) & 1u);
        }
    }
    for (uint32_t w = 0; w < 256u; w++) {
        clock_bytes[w] = (uint8_t)(((w % 4u) >= 2u) << 4 | ((w / 4u) >= 32u) << 5);
    }
}

static uint32_t rbit32(uint32_t x) {
    uint32_t r = 0;
    for (uint32_t i = 0; i < 32u; i++) {
        r |= ((x >> i) & 1u) << (31u - i);
    }
    return r;
}

static uint32_t nrzi_levels(uint32_t x, uint32_t *carry) {
    x ^= x << 1;
    x ^= x << 2;
    x ^= x << 4;
    x ^= x << 8;
    x ^= x << 16;
    if (*carry) {
        x = ~x;
    }
    *carry = x >> 31;
    return x;
}

// The encoder the S/PDIF rings were written with before, as pico_audio_spdif_multi's (pico-extras
// audio_spdif.c and sample_encoding.h, BSD-3-Clause, Raspberry Pi Ltd): 4 words of NRZI cells a frame.
#define PREAMBLE_X 0b11001001u
#define PREAMBLE_Y 0b01101001u
#define PREAMBLE_Z 0b00111001u

static uint32_t spdif_lookup[256];

static void spdif_lookup_init(void) {
    for (uint32_t i = 0; i < 256u; i++) {
        uint32_t v = 0x5555u, p = 0;
        for (uint32_t j = 0; j < 8u; j++) {
            if (i & (1u << j)) {
                p ^= 1u;
                v |= 2u << (j * 2u);
            }
        }
        spdif_lookup[i] = v | (p << 16u);
    }
}

static uint32_t spdif_cs_bit(const uint8_t status[5], uint32_t phase) {
    return phase < 40u ? (status[phase / 8u] >> (phase % 8u)) & 1u : 0u;
}

static void spdif_update_subframe(uint32_t *sf, int32_t sample) {
    uint32_t s0 = spdif_lookup[(uint8_t)sample];
    uint32_t s1 = spdif_lookup[(uint8_t)(sample >> 8u)];
    uint32_t s2 = spdif_lookup[(uint8_t)(sample >> 16u)];
    sf[0] = (sf[0] & 0xFFu) | ((uint32_t)(uint16_t)s0 << 8u) | (s1 << 24u);
    uint32_t ph = sf[1] >> 24u;
    uint32_t h = ((uint32_t)(uint16_t)s1 >> 8u) | ((uint32_t)(uint16_t)s2 << 8u);
    uint32_t p = (s0 >> 16u) ^ (s1 >> 16u) ^ (s2 >> 16u);
    p ^= (((ph & 0x2Au) * 0x2Au) >> 6u) & 1u;
    sf[1] = h | ((ph & 0x7Fu) << 24u) | (p << 31u);
}

// Ring frames w to w + n - 1 as cells, from samples or (lr NULL) as the old silence stamp wrote them.
static void old_spdif_write(uint32_t *ring, uint32_t ring_frames, const uint8_t status[5], uint32_t w,
                            const int32_t *lr, uint32_t n) {
    uint32_t phase = w % 192u;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t *f = &ring[((w + i) & (ring_frames - 1u)) * 4u];
        uint32_t c = spdif_cs_bit(status, phase);
        if (lr) {
            f[0] = phase == 0 ? PREAMBLE_Z : PREAMBLE_X;
            f[1] = 0x55000000u | (c << 29u);
            spdif_update_subframe(&f[0], lr[2u * i]);
            f[2] = PREAMBLE_Y;
            f[3] = 0x55000000u | (c << 29u);
            spdif_update_subframe(&f[2], lr[2u * i + 1u]);
        } else {
            uint32_t h = 0x55555555u | (c << 29u) | (c << 31u);
            f[0] = 0x55555500u | (phase == 0 ? PREAMBLE_Z : PREAMBLE_X);
            f[1] = h;
            f[2] = 0x55555500u | PREAMBLE_Y;
            f[3] = h;
        }
        if (++phase == 192u) {
            phase = 0;
        }
    }
}

// in[s] is the slot's raw frame: S/PDIF's 4 words of cells, or I2S's last L and R, then L and R.
static void reference(uint16_t *dst, uint32_t in[4][4], const uint32_t type[4], bool clocks, const uint32_t *adat) {
    uint8_t low[4][256];
    memset(low, 0, sizeof low);
    for (uint32_t s = 0; s < 4u; s++) {
        if (type[s] == SMI_SLOT_I2S) {
            uint32_t lo = (in[s][1] & 1u) | rbit32(in[s][2]) << 1;
            uint32_t hi = rbit32(in[s][2]) >> 31 | rbit32(in[s][3]) << 1;
            for (uint32_t q = 0; q < 8u; q++) {
                memcpy(&low[s][16u * q], &lut_bck4[(lo >> (4u * (q & ~1u))) & 0xFFu][16u * (q & 1u)], 16);
                memcpy(&low[s][16u * q + 128u], &lut_bck4[(hi >> (4u * (q & ~1u))) & 0xFFu][16u * (q & 1u)], 16);
            }
        } else if (type[s] == SMI_SLOT_SPDIF) {
            uint32_t carry = 0;
            for (uint32_t j = 0; j < 4u; j++) {
                uint32_t lv = nrzi_levels(in[s][j], &carry);
                for (uint32_t b = 0; b < 4u; b++) {
                    memcpy(&low[s][64u * j + 16u * b], lut_cell2[(lv >> (8u * b)) & 0xFFu], 16);
                }
            }
        }
    }
    for (uint32_t w = 0; w < 256u; w++) {
        uint32_t v = low[0][w] | low[1][w] << 1 | low[2][w] << 2 | low[3][w] << 3;
        if (clocks) {
            v |= clock_bytes[w];
        }
        if (adat) {
            uint32_t q = w / 16u;
            uint32_t bits = adat[q >> 1] >> ((q & 1u) ? 0u : 16u);
            v |= (uint32_t)lut_msb8[(w % 16u) < 8u ? (bits >> 8) & 0xFFu : bits & 0xFFu][w % 8u] << 8;
        }
        dst[w] = (uint16_t)v;
    }
}

// DSPi's ADAT encoder, as in adat_output.c: 8 words of line levels a frame, w[0]'s MSB sent first.
#define ADAT_SYNC_HEADER 0x8010u

static uint16_t adat_token_lut[256], adat_hdr_nrzi[2];
static uint32_t adat_hdr_exit[2], adat_silence_nrzi[2][8], adat_silence_exit[2];

static uint32_t adat_encode_frame(uint32_t *w, const int32_t s24[8], uint32_t entry) {
    uint32_t hdr = adat_hdr_nrzi[entry];
    uint32_t m = (0u - adat_hdr_exit[entry]) & 0x3FFu;
    uint32_t c[8];
    for (int ch = 0; ch < 8; ch++) {
        uint32_t s = (uint32_t)s24[ch];
        uint32_t t2 = adat_token_lut[(s >> 16) & 0xFFu] ^ m;
        m = (0u - (t2 & 1u)) & 0x3FFu;
        uint32_t t1 = adat_token_lut[(s >> 8) & 0xFFu] ^ m;
        m = (0u - (t1 & 1u)) & 0x3FFu;
        uint32_t t0 = adat_token_lut[s & 0xFFu] ^ m;
        m = (0u - (t0 & 1u)) & 0x3FFu;
        c[ch] = (t2 << 20) | (t1 << 10) | t0;
    }
    w[0] = (hdr << 16) | (c[0] >> 14);
    w[1] = (c[0] << 18) | (c[1] >> 12);
    w[2] = (c[1] << 20) | (c[2] >> 10);
    w[3] = (c[2] << 22) | (c[3] >> 8);
    w[4] = (c[3] << 24) | (c[4] >> 6);
    w[5] = (c[4] << 26) | (c[5] >> 4);
    w[6] = (c[5] << 28) | (c[6] >> 2);
    w[7] = (c[6] << 30) | c[7];
    return m & 1u;
}

static void adat_encoder_init(void) {
    for (uint32_t b = 0; b < 256; b++) {
        uint32_t stuffed = 0x210u | ((b & 0xF0u) << 1) | (b & 0x0Fu);
        uint32_t t = 0, l = 0;
        for (int k = 9; k >= 0; k--) {
            l ^= (stuffed >> k) & 1u;
            t = (t << 1) | l;
        }
        adat_token_lut[b] = (uint16_t)t;
    }
    for (uint32_t lvl = 0; lvl < 2; lvl++) {
        uint32_t t = 0, l = lvl;
        for (int k = 15; k >= 0; k--) {
            l ^= (ADAT_SYNC_HEADER >> k) & 1u;
            t = (t << 1) | l;
        }
        adat_hdr_nrzi[lvl] = (uint16_t)t;
        adat_hdr_exit[lvl] = l;
    }
    const int32_t zero[8] = {0};
    for (uint32_t lvl = 0; lvl < 2; lvl++) {
        adat_silence_exit[lvl] = adat_encode_frame(adat_silence_nrzi[lvl], zero, lvl);
    }
}

static uint32_t rnd(void) {
    static uint32_t s = 0x12345678u;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

#define RUN 5u  // frames a call, at most

typedef uint32_t (*frames_fn)(uint16_t *dst, smi_frame_job_t *job, uint32_t *key, uint32_t n);

// A run of n frames through fn four times: with new keys (all written), again (none written), marked
// stale (all written), then with one frame's key a bit off (written from that frame on). The keys must
// end as the inputs.
static uint32_t check(frames_fn fn, const char *name, const smi_frame_job_t *job, uint32_t n, const uint16_t *want) {
    uint32_t key[RUN * SMI_FRAME_KEY_WORDS + 1] __attribute__((aligned(32)));
    uint32_t akey[RUN * SMI_FRAME_ADAT_WORDS + 1], adat_was[RUN * SMI_FRAME_ADAT_WORDS];
    static uint16_t got[RUN * 256 + 2] __attribute__((aligned(64)));
    uint32_t bad = 0, kw = n * SMI_FRAME_KEY_WORDS, aw = n * SMI_FRAME_ADAT_WORDS;
    for (uint32_t j = 0; j <= kw; j++) {
        key[j] = rnd();
    }
    for (uint32_t j = 0; j <= aw; j++) {
        akey[j] = rnd();
    }
    uint32_t guard = key[kw], aguard = akey[aw];
    for (uint32_t pass = 0; pass < 4u && !bad; pass++) {
        smi_frame_job_t j = *job;
        j.adat_key = akey;
        j.stale = pass == 2u ? 1u + rnd() : 0u;
        uint32_t odd = n;   // the frame whose key is changed
        memcpy(adat_was, akey, sizeof adat_was);
        if (pass == 3u) {
            odd = rnd() % n;
            uint32_t w = rnd() % (job->adat ? 24u : 16u);
            *(w < 16u ? &key[odd * 16u + w] : &akey[odd * 8u + w - 16u]) ^= 1u << (rnd() % 32u);
        }
        memset(got, 0x5A, sizeof got);
        uint32_t wrote = fn(got, &j, key, n), expect = 0;
        for (uint32_t k = 0; k < n; k++) {
            bool written = pass == 0 || pass == 2u || k >= odd;
            expect += written;
            if (written) {
                bad |= memcmp(&got[k * 256u], &want[k * 256u], 512) != 0;
            } else {
                bad |= got[k * 256u] != 0x5A5Au || got[k * 256u + 255u] != 0x5A5Au;
            }
            for (uint32_t s = 0; s < 4u; s++) {
                bad |= memcmp(&key[k * 16u + 4u * s], (const uint8_t *)job->raw[s] + k * job->stride[s], 16) != 0;
            }
        }
        bad |= wrote != expect || got[n * 256u] != 0x5A5Au || key[kw] != guard || akey[aw] != aguard;
        bad |= memcmp(akey, job->adat ? job->adat : adat_was, aw * 4u) != 0;
        bad |= job->adat && (j.adat != job->adat + aw || j.adat_key != akey + aw);
        if (bad) {
            printf("%s: types %u %u %u %u clocks %u adat %u frames %u pass %u wrote %u\n", name, (unsigned)job->type[0],
                   (unsigned)job->type[1], (unsigned)job->type[2], (unsigned)job->type[3], (unsigned)job->clocks,
                   job->adat != NULL, (unsigned)n, (unsigned)pass, (unsigned)wrote);
        }
    }
    return bad != 0;
}

// Seven different SD lines for the slots, BCK, LRCLK and ADAT, sometimes the native ones.
static void random_lines(uint8_t line[SMI_LINES]) {
    static const uint8_t native[SMI_LINES] = {0, 1, 2, 3, 4, 5, 8};
    uint8_t all[16];
    for (uint32_t i = 0; i < 16u; i++) {
        all[i] = (uint8_t)i;
    }
    for (uint32_t i = 15; i > 0; i--) {
        uint32_t j = rnd() % (i + 1u);
        uint8_t x = all[i];
        all[i] = all[j];
        all[j] = x;
    }
    memcpy(line, rnd() % 8u == 0 ? native : all, SMI_LINES);
}

// A native word on those lines, a bit at a time.
static uint16_t moved_word(uint16_t w, const uint8_t line[SMI_LINES]) {
    static const uint8_t bit[SMI_LINES] = {0, 1, 2, 3, 4, 5, 8};
    uint32_t out = 0;
    for (uint32_t b = 0; b < SMI_LINES; b++) {
        out |= (uint32_t)(w >> bit[b] & 1u) << line[b];
    }
    return (uint16_t)out;
}

static uint32_t shaped(uint32_t shape) {  // random, all zeros, all ones, sparse
    return shape == 0 ? rnd() : shape == 1 ? 0u : shape == 2 ? 0xFFFFFFFFu : 1u << (rnd() % 32u);
}

static void random_status(uint8_t status[5]) {
    static const uint8_t rates[4] = {0x00, 0x02, 0x0A, 0x01};
    memcpy(status, (const uint8_t[5]){0x04, 0x00, 0x00, rates[rnd() % 4u], 0x0B}, 5);
    for (uint32_t j = 0; j < 5u && rnd() % 4u == 0; j++) {
        status[j] = (uint8_t)rnd();
    }
}

#define BLOCK_RING 512u

// Whole channel-status blocks. A 512-frame ring is written in runs of audio and silence that wrap it,
// by the old encoder and by smi_spdif_write, then every ring frame is composed and compared.
static uint32_t block_check(frames_fn fn, const char *name, const uint8_t status[5]) {
    static const uint32_t zeros[4];
    static uint32_t cells[BLOCK_RING * 4u], words[BLOCK_RING * 2u + 2u], idle[SMI_SPDIF_BLOCK][2];
    static uint32_t key[BLOCK_RING * SMI_FRAME_KEY_WORDS] __attribute__((aligned(32)));
    static uint16_t got[BLOCK_RING * 256u] __attribute__((aligned(64))), want[256];
    int32_t lr[2u * 64u];
    uint32_t w = rnd(), bad = 0, slot = rnd() % 4u, done = 0;
    smi_spdif_silence_init(idle, status);
    while (done < BLOCK_RING) {
        uint32_t n = 1u + rnd() % 64u, shape = rnd() % 4u;
        bool silence = rnd() % 3u == 0;
        n = n < BLOCK_RING - done ? n : BLOCK_RING - done;
        for (uint32_t j = 0; j < 2u * n; j++) {
            lr[j] = (int32_t)shaped(shape);
        }
        old_spdif_write(cells, BLOCK_RING, status, w, silence ? NULL : lr, n);
        smi_spdif_write(words, BLOCK_RING, idle, w, silence ? NULL : lr, n);
        w += n;
        done += n;
    }
    smi_frame_job_t job = {.raw = {zeros, zeros, zeros, zeros}, .stale = 1};
    uint32_t type[4] = {0};
    job.raw[slot] = words;
    job.type[slot] = type[slot] = SMI_SLOT_SPDIF;
    job.stride[slot] = SMI_SPDIF_STRIDE;
    bad |= fn(got, &job, key, BLOCK_RING) != BLOCK_RING;
    for (uint32_t k = 0; k < BLOCK_RING; k++) {
        uint32_t in[4][4] = {{0}};
        memcpy(in[slot], &cells[4u * k], 16);
        reference(want, in, type, false, NULL);
        bad |= memcmp(&got[k * 256u], want, 512) != 0;
    }
    if (bad) {
        printf("%s: block of status %02x %02x %02x %02x %02x differs\n", name, status[0], status[1], status[2],
               status[3], status[4]);
    }
    return bad != 0;
}

#define ADAT_ROW 192u   // DSPi's block rows

// Runs of audio and silence through DSPi's encoder a frame at a time, as adat_output_push_block and
// adat_write_silence did, and through smi_adat_write and smi_adat_silence. Every frame's levels must be
// DSPi's words bit for bit (sent first is DSPi's MSB and bit 0 here), and the line level must agree.
static uint32_t adat_check(void) {
    static int32_t rows[8u * ADAT_ROW];
    static uint32_t old[ADAT_ROW * 8u], ring[ADAT_ROW * 8u + 1u];
    uint32_t bad = 0, old_level = rnd() & 1u, level = old_level;
    for (uint32_t t = 0; t < 40u && !bad; t++) {
        uint32_t n = t % 5u == 0 ? ADAT_ROW : 1u + rnd() % ADAT_ROW, shape = rnd() % 5u, first = rnd() % (ADAT_ROW - n + 1u);
        bool silence = rnd() % 4u == 0;
        for (uint32_t j = 0; j < 8u * ADAT_ROW; j++) {
            // Sign-extended 24-bit samples, as DSPi's rows hold, or any word.
            rows[j] = shape == 4u ? (int32_t)(rnd() << 8) >> 8 : (int32_t)shaped(shape);
        }
        ring[n * 8u] = 0xA5A5A5A5u;
        if (silence) {
            for (uint32_t i = 0; i < n; i++) {
                memcpy(&old[8u * i], adat_silence_nrzi[old_level], 32);
                old_level = adat_silence_exit[old_level];
            }
            smi_adat_silence(ring, n, level);
        } else {
            for (uint32_t i = 0; i < n; i++) {
                int32_t s[8];
                for (uint32_t c = 0; c < 8u; c++) {
                    s[c] = rows[c * ADAT_ROW + first + i];
                }
                old_level = adat_encode_frame(&old[8u * i], s, old_level);
            }
            level = smi_adat_write(ring, rows + first, ADAT_ROW, n, level);
        }
        bad |= level != old_level || ring[n * 8u] != 0xA5A5A5A5u;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t w[8];
            smi_adat_levels(&ring[8u * i], w);
            for (uint32_t k = 0; k < 8u; k++) {
                bad |= w[k] != rbit32(old[8u * i + k]);
            }
        }
        if (bad) {
            printf("adat: run of %u frames, %s, differs from DSPi's encoder\n", (unsigned)n, silence ? "silence" : "audio");
        }
    }
    return bad != 0;
}

int main(void) {
    static const uint32_t zeros[4];
    tables_init();
    spdif_lookup_init();
    adat_encoder_init();
    uint32_t fails = 0, runs = 0;
    for (uint32_t t = 0; t < 81u * 4u * 100u; t++) {
        smi_frame_job_t job = {.clocks = (t / 81u) & 1u};
        // Each slot's ring words, with room for the 4 the key reads at the last frame. cells holds the
        // same S/PDIF frames as the old encoder wrote them, and adat_old the ADAT frames as DSPi's did.
        uint32_t buf[4][RUN * 4u], cells[4][8u * 4u], adat[RUN * 8u], adat_old[RUN * 8u];
        uint32_t c = t % 81u, shape = rnd() % 4u, n = 1u + rnd() % RUN;
        for (uint32_t s = 0; s < 4u; s++, c /= 3u) {
            job.type[s] = c % 3u;
            job.raw[s] = job.type[s] == SMI_SLOT_OFF ? zeros : buf[s];
            job.stride[s] = job.type[s] == SMI_SLOT_SPDIF ? SMI_SPDIF_STRIDE : job.type[s] == SMI_SLOT_I2S ? 8u : 0u;
            for (uint32_t j = 0; j < RUN * 4u; j++) {
                buf[s][j] = shaped(shape);
            }
            if (job.type[s] == SMI_SLOT_SPDIF) {
                uint32_t idle[SMI_SPDIF_BLOCK][2], w = rnd() & ~7u;     // any block phase, ring frame 0
                uint8_t status[5];
                int32_t lr[2u * RUN];
                bool silence = rnd() % 4u == 0;
                random_status(status);
                smi_spdif_silence_init(idle, status);
                for (uint32_t j = 0; j < 2u * RUN; j++) {
                    lr[j] = (int32_t)shaped(shape);
                }
                old_spdif_write(cells[s], 8u, status, w, silence ? NULL : lr, RUN);
                smi_spdif_write(buf[s], 8u, idle, w, silence ? NULL : lr, RUN);
            }
        }
        // ADAT frames from a random line level, as audio, as silence, or as one after the other.
        int32_t rows[8u * RUN];
        uint32_t old_level = rnd() & 1u, level = old_level, quiet = rnd() % 4u, split = quiet == 0 ? rnd() % RUN : 0;
        for (uint32_t j = 0; j < 8u * RUN; j++) {
            rows[j] = (int32_t)shaped(shape);
        }
        for (uint32_t k = 0; k < RUN; k++) {
            if (quiet == 1u || (quiet == 0 && k < split)) {
                memcpy(&adat_old[8u * k], adat_silence_nrzi[old_level], 32);
                old_level = adat_silence_exit[old_level];
                smi_adat_silence(&adat[8u * k], 1, level);
            } else {
                int32_t smp[8];
                for (uint32_t ch = 0; ch < 8u; ch++) {
                    smp[ch] = rows[ch * RUN + k];
                }
                old_level = adat_encode_frame(&adat_old[8u * k], smp, old_level);
                uint32_t run = quiet == 0 ? RUN - k : 1u;   // the rest in one call, or a frame at a time
                level = smi_adat_write(&adat[8u * k], rows + k, RUN, run, level);
                if (run > 1u) {
                    for (k++; k < RUN; k++) {
                        for (uint32_t ch = 0; ch < 8u; ch++) {
                            smp[ch] = rows[ch * RUN + k];
                        }
                        old_level = adat_encode_frame(&adat_old[8u * k], smp, old_level);
                    }
                }
            }
        }
        fails += level != old_level;
        job.adat = (t / 162u) & 1u ? adat : NULL;
        static uint16_t want[RUN * 256];
        for (uint32_t k = 0; k < n; k++) {
            uint32_t in[4][4];
            for (uint32_t s = 0; s < 4u; s++) {
                memcpy(in[s], (const uint8_t *)job.raw[s] + k * job.stride[s], 16);
                if (job.type[s] == SMI_SLOT_SPDIF) {
                    memcpy(in[s], &cells[s][4u * k], 16);
                }
            }
            reference(&want[k * 256u], in, job.type, job.clocks, job.adat ? &adat_old[8u * k] : NULL);
        }
        fails += check(smi_frames_c, "smi_frames_c", &job, n, want);
        // The same frames on other lines.
        {
            static smi_pinmap_t map __attribute__((aligned(16)));
            static uint16_t moved[RUN * 256];
            uint8_t line[SMI_LINES];
            random_lines(line);
            smi_pinmap_build(&map, line);
            for (uint32_t k = 0; k < n * 256u; k++) {
                moved[k] = moved_word(want[k], line);
            }
            smi_frame_job_t mj = job;
            mj.map = &map;
            fails += check(smi_frames_c, "smi_frames_c on other lines", &mj, n, moved);
#ifdef __arm__
            fails += check(smi_frames_a32, "smi_frames_a32 on other lines", &mj, n, moved);
#endif
        }
#ifdef __arm__
        fails += check(smi_frames_a32, "smi_frames_a32", &job, n, want);
        // Ring words the writer never makes must still compose as the C does.
        for (uint32_t s = 0; s < 4u; s++) {
            for (uint32_t j = 0; j < RUN * 4u; j++) {
                buf[s][j] = shaped(rnd() % 4u);
            }
        }
        for (uint32_t j = 0; j < RUN * 8u; j++) {
            adat[j] = shaped(rnd() % 4u);
        }
        uint32_t key[RUN * SMI_FRAME_KEY_WORDS], akey[RUN * SMI_FRAME_ADAT_WORDS];
        smi_frame_job_t any = job;
        any.adat_key = akey;
        any.stale = 1;
        smi_frames_c(want, &any, key, n);
        fails += check(smi_frames_a32, "smi_frames_a32 on any words", &job, n, want);
#endif
        runs += n;
    }
    for (uint32_t t = 0; t < 200u; t++) {
        fails += adat_check();
    }
    for (uint32_t t = 0; t < 200u; t++) {
        uint8_t status[5];
        random_status(status);
        fails += block_check(smi_frames_c, "smi_frames_c", status);
#ifdef __arm__
        fails += block_check(smi_frames_a32, "smi_frames_a32", status);
#endif
        runs += BLOCK_RING;
    }
    printf("%s: %u frames, %u differ\n", fails ? "FAIL" : "PASS", (unsigned)runs, (unsigned)fails);
    return fails != 0;
}
