#pragma once

// One SMI frame (256 words) from four slots' ring frames, in NEON with no tables in the slot path. Each
// slot becomes 128 line levels, one per S/PDIF cell time (2 SMI words); the four are bit-interleaved into
// one nibble per cell by two rounds of polynomial squaring, then widened to words. Bring-up sections 27
// and 28.

#include <arm_acle.h>
#include <arm_neon.h>
#include <stdbool.h>
#include <stdint.h>

enum { SMI_SLOT_OFF, SMI_SLOT_SPDIF, SMI_SLOT_I2S };

#define SMI_FRAME_KEY_WORDS 16u     // what a frame was made from: 4 words a slot
#define SMI_FRAME_ADAT_WORDS 8u

// An ADAT ring frame is 8 words, channel c's 24-bit sample in bits 0 to 23 of word c, and the composer
// encodes it (bring-up section 29). Bit 31 of word 7 is the line level the frame starts from, so a frame
// stands alone. A frame sends an even number of 1 bits apart from its samples', so it ends at that level
// XOR the parity of its samples.
#define SMI_ADAT_SAMPLE 0x00FFFFFFu
#define SMI_ADAT_LEVEL (1u << 31)   // in word 7
#define SMI_ADAT_SYNC 0x0801u       // sent first, from bit 0 (a 1, ten 0s, a 1 and four user bits of 0)

// An S/PDIF slot ring holds two words a frame, left then right, which the composer encodes. A word has
// one bit for each of its subframe's 32 time slots, stored so that a running XOR over the word gives the
// line level in the second half of every slot. Biphase mark toggles the line at each slot's start and
// again at its middle for a 1, so the sample and the V, U and C bits are stored inverted.
#define SMI_SPDIF_PRE_LEFT 0x3u             // bits 0-3 of a left word (preamble X or Z)
#define SMI_SPDIF_PRE_RIGHT 0xFu            // bits 0-3 of a right word (preamble Y)
#define SMI_SPDIF_DATA_ZERO 0x3FFFFFF0u     // bits 4-27 a sample of 0, bits 28 and 29 V and U of 0
#define SMI_SPDIF_NOT_C (1u << 30)          // a channel status bit of 0
#define SMI_SPDIF_BLOCK_START (1u << 31)    // left word of a block's first frame (preamble Z)
// The parity bit is not stored. It is whatever returns the line to the level the subframe began at.
#define SMI_SPDIF_BLOCK 192u
#define SMI_SPDIF_STRIDE 8u         // bytes a frame

// A sample's bits in a ring word. XORed into a silent word they give the word for that sample.
static inline uint32_t smi_spdif_sample(int32_t sample) {
    return (uint32_t)sample << 8 >> 4;
}

// The silent frame for each phase of a channel-status block (5 bytes of status, the rest zeros), as DSPi's
// RP2350 encoder sends it, with validity and user bits of 0 and the status bit in both subframes.
static inline void smi_spdif_silence_init(uint32_t idle[SMI_SPDIF_BLOCK][2], const uint8_t status[5]) {
    for (uint32_t phase = 0; phase < SMI_SPDIF_BLOCK; phase++) {
        uint32_t c = phase < 40u ? (status[phase / 8u] >> (phase % 8u)) & 1u : 0u;
        uint32_t data = SMI_SPDIF_DATA_ZERO | (c ? 0u : SMI_SPDIF_NOT_C);
        idle[phase][0] = data | SMI_SPDIF_PRE_LEFT | (phase == 0 ? SMI_SPDIF_BLOCK_START : 0u);
        idle[phase][1] = data | SMI_SPDIF_PRE_RIGHT;
    }
}

// Ring frames w to w + n - 1 (w is the absolute frame number, whose block phase is w modulo 192) from n
// stereo samples, or as silence when lr is NULL. ring_frames is a power of two.
static inline void smi_spdif_write(uint32_t *ring, uint32_t ring_frames, const uint32_t idle[SMI_SPDIF_BLOCK][2],
                                   uint32_t w, const int32_t *lr, uint32_t n) {
    uint32_t phase = w % SMI_SPDIF_BLOCK;
    while (n) {
        uint32_t at = w & (ring_frames - 1u), run = n;
        run = run < ring_frames - at ? run : ring_frames - at;
        run = run < SMI_SPDIF_BLOCK - phase ? run : SMI_SPDIF_BLOCK - phase;
        uint32_t *f = &ring[at * 2u];
        const uint32_t *from = &idle[0][0] + 2u * phase;
        uint32_t words = 2u * run, i = 0;
        if (lr) {
            for (; i + 4u <= words; i += 4u) {
                uint32x4_t v = vshrq_n_u32(vshlq_n_u32(vreinterpretq_u32_s32(vld1q_s32(&lr[i])), 8), 4);
                vst1q_u32(&f[i], veorq_u32(vld1q_u32(&from[i]), v));
            }
            for (; i < words; i++) {
                f[i] = from[i] ^ smi_spdif_sample(lr[i]);
            }
            lr += words;
        } else {
            for (; i + 4u <= words; i += 4u) {
                vst1q_u32(&f[i], vld1q_u32(&from[i]));
            }
            for (; i < words; i++) {
                f[i] = from[i];
            }
        }
        w += run;
        n -= run;
        phase = phase + run == SMI_SPDIF_BLOCK ? 0 : phase + run;
    }
}

// n ADAT ring frames at f from 8 rows of samples (channel c's sample i is rows[c * row_words + i]), which
// may be sign-extended. level is the line level before the first frame. Returns the level after the last.
static inline uint32_t smi_adat_write(uint32_t *f, const int32_t *rows, uint32_t row_words, uint32_t n,
                                      uint32_t level) {
    const uint32x4_t mask = vdupq_n_u32(SMI_ADAT_SAMPLE);
    uint32x4_t lv = vdupq_n_u32(level & 1u);
    uint32_t i = 0;
#define SMI_ADAT_ROW(c) vandq_u32(vreinterpretq_u32_s32(vld1q_s32(&rows[(c) * row_words + i])), mask)
    for (; i + 4u <= n; i += 4u, f += 32) {
        uint32x4_t r0 = SMI_ADAT_ROW(0), r1 = SMI_ADAT_ROW(1), r2 = SMI_ADAT_ROW(2), r3 = SMI_ADAT_ROW(3);
        uint32x4_t r4 = SMI_ADAT_ROW(4), r5 = SMI_ADAT_ROW(5), r6 = SMI_ADAT_ROW(6), r7 = SMI_ADAT_ROW(7);
        uint32x4_t x = veorq_u32(veorq_u32(veorq_u32(r0, r1), veorq_u32(r2, r3)),
                                 veorq_u32(veorq_u32(r4, r5), veorq_u32(r6, r7)));
        // Each frame's parity, then the level before each of the four and after the last.
        uint32x4_t p = vandq_u32(vpaddlq_u16(vpaddlq_u8(vcntq_u8(vreinterpretq_u8_u32(x)))), vdupq_n_u32(1));
        p = veorq_u32(p, vreinterpretq_u32_u64(vshlq_n_u64(vreinterpretq_u64_u32(p), 32)));
        p = veorq_u32(p, vcombine_u32(vdup_n_u32(0), vdup_lane_u32(vget_low_u32(p), 1)));
        p = veorq_u32(p, lv);
        r7 = vorrq_u32(r7, vshlq_n_u32(vextq_u32(lv, p, 3), 31));
        lv = vdupq_lane_u32(vget_high_u32(p), 1);
        // Rows to frames, four at a time.
        uint32x4x2_t a = vzipq_u32(r0, r2), b = vzipq_u32(r1, r3), c = vzipq_u32(r4, r6), d = vzipq_u32(r5, r7);
        uint32x4x2_t lo01 = vzipq_u32(a.val[0], b.val[0]), lo23 = vzipq_u32(a.val[1], b.val[1]);
        uint32x4x2_t hi01 = vzipq_u32(c.val[0], d.val[0]), hi23 = vzipq_u32(c.val[1], d.val[1]);
        vst1q_u32(&f[0], lo01.val[0]);
        vst1q_u32(&f[4], hi01.val[0]);
        vst1q_u32(&f[8], lo01.val[1]);
        vst1q_u32(&f[12], hi01.val[1]);
        vst1q_u32(&f[16], lo23.val[0]);
        vst1q_u32(&f[20], hi23.val[0]);
        vst1q_u32(&f[24], lo23.val[1]);
        vst1q_u32(&f[28], hi23.val[1]);
    }
#undef SMI_ADAT_ROW
    level = vgetq_lane_u32(lv, 0);
    for (; i < n; i++, f += 8) {
        uint32_t x = 0;
        for (uint32_t c = 0; c < 8u; c++) {
            f[c] = (uint32_t)rows[c * row_words + i] & SMI_ADAT_SAMPLE;
            x ^= f[c];
        }
        f[7] |= level << 31;
        level ^= (uint32_t)__builtin_parity(x);
    }
    return level;
}

// n silent ADAT ring frames at f, which leave the line at the level they start from.
static inline void smi_adat_silence(uint32_t *f, uint32_t n, uint32_t level) {
    const uint32x4_t zero = vdupq_n_u32(0), last = vsetq_lane_u32(level << 31, zero, 3);
    for (uint32_t i = 0; i < n; i++, f += 8) {
        vst1q_u32(f, zero);
        vst1q_u32(f + 4, last);
    }
}

// An ADAT ring frame's 256 line levels, the first sent in bit 0 of w[0]. Each sample is sent MSB first
// as six nibbles, a 1 before each, after the sync pattern, and a 1 is a change of level (NRZI). So the
// sample is reversed, nibble g moved to bit 5g + 1, and the levels are the running XOR of all 256 bits.
static inline void smi_adat_levels(const uint32_t *f, uint32_t w[8]) {
    uint64_t acc = SMI_ADAT_SYNC;
    uint32_t have = 16, k = 0, level = f[7] >> 31;
    for (uint32_t c = 0; c < 8u; c++) {
        uint32_t t = __rbit(f[c]) >> 7;
        t = (t & ~0xFFE00000u) | (t << 4 & 0xFFE00000u);    // nibbles 4 and 5 up 4
        t = (t & ~0x0007F800u) | (t << 2 & 0x0007F800u);    // nibbles 2 and 3 up 2
        t = (t & ~0x3C0F03C0u) | (t << 1 & 0x3C0F03C0u);    // nibbles 1, 3 and 5 up 1
        t |= 0x02108421u;
        acc |= (uint64_t)t << have;
        have += 30u;
        if (have >= 32u) {
            w[k++] = (uint32_t)acc;
            acc >>= 32;
            have -= 32u;
        }
    }
    for (k = 0; k < 8u; k++) {
        uint32_t x = w[k];
        x ^= x << 1;
        x ^= x << 2;
        x ^= x << 4;
        x ^= x << 8;
        x ^= x << 16;
        w[k] = level ? ~x : x;
        level = w[k] >> 31;
    }
}

// raw[s] is 4 words of slot s's ring. For S/PDIF they are the frame's left and right words, then the
// next frame's, which only the key takes in. For I2S they are the frame before's L and R, then L and R,
// of which the R before's LSB and L, R are sent. A slot that is off has zeros. adat is the frame's 8
// words of the ADAT ring, whose 256 line levels go one per SMI word on SD8, or NULL.
typedef struct {
    const uint32_t *raw[4];
    uint32_t type[4];
    uint32_t clocks;            // 1 puts BCK and LRCLK on the stream
    const uint32_t *adat;
    uint32_t *adat_key;         // the ADAT words each frame was made from, 8 a frame, used while adat is set
    uint32_t stride[4];         // bytes from one frame's raw words to the next frame's
    uint32_t stale;             // not 0: the keys do not describe dst, so every frame is written
} smi_frame_job_t;

// Composes n frames of 256 words to dst, the inputs (raw by its stride, adat by 8 words) and the 16-word
// keys moving on a frame at a time. A frame whose key already holds its inputs is skipped, since dst
// still holds it, until the first that differs, which is written with every later one; the keys take the
// inputs either way. Returns the frames written. smi_frames_c below is the reference the assembly matches.
uint32_t smi_frames_a32(uint16_t *dst, smi_frame_job_t *job, uint32_t *key, uint32_t n);

#define SMI_FRAME_BIT_BCK 4u
#define SMI_FRAME_BIT_LRCLK 5u

static inline uint16x8_t smi_frame_square(uint8x8_t a) {    // bit i of each byte to bit 2i
    poly8x8_t p = vreinterpret_p8_u8(a);
    return vreinterpretq_u16_p16(vmull_p8(p, p));
}

static inline uint16x8_t smi_frame_pair(uint16x8_t even, uint16x8_t odd) {  // interleaves two squares
    return vorrq_u16(even, vshlq_n_u16(odd, 1));
}

// One S/PDIF ring word to its 32 slots' levels, first halves in half[0] and second halves in half[1].
// The running XOR is the second halves, and its bit 31 is the parity slot, which ends at the level the
// subframe began at, 0. A first half is the level before it, inverted; the low 4 slots are the preamble.
static inline void smi_frame_spdif_halves(uint32_t w, bool left, uint32_t half[2]) {
    uint32_t x = w;
    x ^= x << 1;
    x ^= x << 2;
    x ^= x << 4;
    x ^= x << 8;
    x ^= x << 16;
    half[0] = ~(x << 1) ^ 6u ^ (left && w >> 31 ? 12u : 0u);
    half[1] = x & 0x7FFFFFFFu;
}

// An S/PDIF frame's 128 levels from its left and right ring words. Every subframe has an even number of
// transitions, so each starts from the same level.
static inline uint8x16_t smi_frame_levels_spdif(const uint32_t *w) {
    uint32_t l[2], r[2];
    smi_frame_spdif_halves(w[0], true, l);
    smi_frame_spdif_halves(w[1], false, r);
    uint16x8_t first = smi_frame_square(vcreate_u8((uint64_t)r[0] << 32 | l[0]));
    uint16x8_t second = smi_frame_square(vcreate_u8((uint64_t)r[1] << 32 | l[1]));
    return vreinterpretq_u8_u16(smi_frame_pair(first, second));
}

// Philips I2S, 64 bit clocks of 2 cell times each: clock 0 is the last frame's right LSB, 1-32 the left
// word MSB first, 33-63 the right word's top 31 bits.
static inline uint8x16_t smi_frame_levels_i2s(uint32_t l, uint32_t r, uint32_t prev_r) {
    uint64_t bits = ((uint64_t)__rbit(r) << 32 | __rbit(l)) << 1 | (prev_r & 1u);
    uint16x8_t once = smi_frame_square(vcreate_u8(bits));
    return vreinterpretq_u8_u16(smi_frame_pair(once, once));
}

// Slots 0 and 2, then 1 and 3, interleave into 2-bit fields; squaring those and interleaving again gives
// slot n in bit n of each nibble, two cells a byte. n4[k] holds cells 32k to 32k + 31.
static inline void smi_frame_nibbles(const uint8x16_t lv[4], uint8x16_t n4[4]) {
    uint8x16_t a[2], b[2];
    for (uint32_t h = 0; h < 2u; h++) {
        uint8x8_t s0 = h ? vget_high_u8(lv[0]) : vget_low_u8(lv[0]), s1 = h ? vget_high_u8(lv[1]) : vget_low_u8(lv[1]);
        uint8x8_t s2 = h ? vget_high_u8(lv[2]) : vget_low_u8(lv[2]), s3 = h ? vget_high_u8(lv[3]) : vget_low_u8(lv[3]);
        a[h] = vreinterpretq_u8_u16(smi_frame_pair(smi_frame_square(s0), smi_frame_square(s2)));
        b[h] = vreinterpretq_u8_u16(smi_frame_pair(smi_frame_square(s1), smi_frame_square(s3)));
    }
    for (uint32_t h = 0; h < 2u; h++) {
        n4[2u * h] = vreinterpretq_u8_u16(
            smi_frame_pair(smi_frame_square(vget_low_u8(a[h])), smi_frame_square(vget_low_u8(b[h]))));
        n4[2u * h + 1u] = vreinterpretq_u8_u16(
            smi_frame_pair(smi_frame_square(vget_high_u8(a[h])), smi_frame_square(vget_high_u8(b[h]))));
    }
}

// One byte per cell from a nibble vector, with the I2S clocks. BCK is low then high within a bit clock,
// which is the odd cell of each pair; LRCLK is high for the second half of the frame.
static inline uint8x16x2_t smi_frame_cells(uint8x16_t n, uint32_t k, bool clocks) {
    uint8_t lr = clocks && k >= 2u ? 1u << SMI_FRAME_BIT_LRCLK : 0u;
    uint8_t bck = clocks ? 1u << SMI_FRAME_BIT_BCK : 0u;
    uint8x16_t even = vbslq_u8(vdupq_n_u8(0x0F), n, vdupq_n_u8(lr));
    uint8x16_t odd = vorrq_u8(vshrq_n_u8(n, 4), vdupq_n_u8(lr | bck));
    return vzipq_u8(even, odd);
}

// lv is each slot's 128 levels (zeros for a slot that is off); adat is as in the job.
static inline void smi_frame_store(uint16_t *dst, const uint8x16_t lv[4], bool clocks, const uint32_t *adat) {
    uint8x16_t n4[4];
    uint8_t level[256];     // ADAT's level in each SMI word
    smi_frame_nibbles(lv, n4);
    if (adat) {
        uint32_t w[8];
        smi_adat_levels(adat, w);
        for (uint32_t j = 0; j < 256u; j++) {
            level[j] = (uint8_t)(w[j / 32u] >> (j % 32u) & 1u);
        }
    }
    for (uint32_t k = 0; k < 4u; k++, dst += 64) {
        uint8x16x2_t c = smi_frame_cells(n4[k], k, clocks);
        if (!adat) {
            // Each cell is two equal words with a zero high byte.
            for (uint32_t j = 0; j < 2u; j++) {
                uint16x8_t w0 = vmovl_u8(vget_low_u8(c.val[j])), w1 = vmovl_u8(vget_high_u8(c.val[j]));
                vst2q_u16(&dst[32u * j], ((uint16x8x2_t){{w0, w0}}));
                vst2q_u16(&dst[32u * j + 16u], ((uint16x8x2_t){{w1, w1}}));
            }
        } else {
            for (uint32_t j = 0; j < 2u; j++) {
                uint8x16x2_t low = vzipq_u8(c.val[j], c.val[j]);
                uint8x16x2_t out;
                out.val[0] = low.val[0];
                out.val[1] = vld1q_u8(&level[64u * k + 32u * j]);
                vst2q_u8((uint8_t *)&dst[32u * j], out);
                out.val[0] = low.val[1];
                out.val[1] = vld1q_u8(&level[64u * k + 32u * j + 16u]);
                vst2q_u8((uint8_t *)&dst[32u * j + 16u], out);
            }
        }
    }
}

static inline bool smi_frame_c(uint16_t *dst, const uint32_t *const raw[4], const smi_frame_job_t *job, uint32_t *key,
                               uint32_t diff) {
    for (uint32_t s = 0; s < 4u; s++) {
        for (uint32_t j = 0; j < 4u; j++) {
            diff |= key[4u * s + j] ^ raw[s][j];
            key[4u * s + j] = raw[s][j];
        }
    }
    for (uint32_t j = 0; job->adat && j < SMI_FRAME_ADAT_WORDS; j++) {
        diff |= job->adat_key[j] ^ job->adat[j];
        job->adat_key[j] = job->adat[j];
    }
    if (!diff) {
        return false;
    }
    uint8x16_t lv[4];
    for (uint32_t s = 0; s < 4u; s++) {
        lv[s] = job->type[s] == SMI_SLOT_SPDIF ? smi_frame_levels_spdif(raw[s])
                : job->type[s] == SMI_SLOT_I2S ? smi_frame_levels_i2s(raw[s][2], raw[s][3], raw[s][1])
                                               : vdupq_n_u8(0);
    }
    smi_frame_store(dst, lv, job->clocks != 0, job->adat);
    return true;
}

static inline uint32_t smi_frames_c(uint16_t *dst, smi_frame_job_t *job, uint32_t *key, uint32_t n) {
    uint32_t wrote = 0, differs = job->stale;
    for (uint32_t k = 0; k < n; k++, dst += 256, key += SMI_FRAME_KEY_WORDS) {
        const uint32_t *raw[4];
        for (uint32_t s = 0; s < 4u; s++) {
            raw[s] = (const uint32_t *)((const uint8_t *)job->raw[s] + k * job->stride[s]);
        }
        differs |= smi_frame_c(dst, raw, job, key, differs);
        wrote += differs != 0;
        if (job->adat) {
            job->adat += SMI_FRAME_ADAT_WORDS;
            job->adat_key += SMI_FRAME_ADAT_WORDS;
        }
    }
    return wrote;
}
