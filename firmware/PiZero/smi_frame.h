#pragma once

// One SMI frame (256 words) from four slots' ring frames, in NEON with no tables in the slot path. Each
// slot becomes 128 line levels, one per S/PDIF cell time (2 SMI words); the four are bit-interleaved into
// one nibble per cell by two rounds of polynomial squaring, then widened to words. Bring-up section 27.

#include <arm_acle.h>
#include <arm_neon.h>
#include <stdbool.h>
#include <stdint.h>

enum { SMI_SLOT_OFF, SMI_SLOT_SPDIF, SMI_SLOT_I2S };

#define SMI_FRAME_KEY_WORDS 16u     // what a frame was made from: 4 words a slot
#define SMI_FRAME_ADAT_WORDS 8u

// raw[s] is 4 words of slot s's ring: an S/PDIF frame's NRZI cells; for I2S the frame before's L and R,
// then L and R (only the R before's LSB and L, R are sent); zeros for a slot that is off. adat is the
// frame's 8 words of ADAT line levels, MSB first, one bit per SMI word on SD8, or NULL.
typedef struct {
    const uint32_t *raw[4];
    uint32_t type[4];
    uint32_t clocks;            // 1 puts BCK and LRCLK on the stream
    const uint32_t *adat;
    const uint8_t (*msb8)[8];   // 8 bits, MSB first, to 8 bytes, for each byte value
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

// NRZI cells (1 = toggle) to levels: a running XOR over the frame's 128 cells. Every subframe has an even
// number of transitions, so each frame starts from the same level.
static inline uint8x16_t smi_frame_levels_spdif(const uint32_t *w) {
    uint64x2_t x = vreinterpretq_u64_u32(vld1q_u32(w));
    x = veorq_u64(x, vshlq_n_u64(x, 1));
    x = veorq_u64(x, vshlq_n_u64(x, 2));
    x = veorq_u64(x, vshlq_n_u64(x, 4));
    x = veorq_u64(x, vshlq_n_u64(x, 8));
    x = veorq_u64(x, vshlq_n_u64(x, 16));
    x = veorq_u64(x, vshlq_n_u64(x, 32));
    uint64x1_t lo = vget_low_u64(x);
    uint64x1_t carry = vreinterpret_u64_s64(vshr_n_s64(vreinterpret_s64_u64(lo), 63));
    return vreinterpretq_u8_u64(vcombine_u64(lo, veor_u64(vget_high_u64(x), carry)));
}

static inline uint16x8_t smi_frame_square(uint8x8_t a) {    // bit i of each byte to bit 2i
    poly8x8_t p = vreinterpret_p8_u8(a);
    return vreinterpretq_u16_p16(vmull_p8(p, p));
}

static inline uint16x8_t smi_frame_pair(uint16x8_t even, uint16x8_t odd) {  // interleaves two squares
    return vorrq_u16(even, vshlq_n_u16(odd, 1));
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

// lv is each slot's 128 levels (zeros for a slot that is off); adat and msb8 are as in the job.
static inline void smi_frame_store(uint16_t *dst, const uint8x16_t lv[4], bool clocks, const uint32_t *adat,
                                   const uint8_t msb8[256][8]) {
    uint8x16_t n4[4];
    smi_frame_nibbles(lv, n4);
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
                uint32_t bits = adat[2u * k + j];
                uint8x16x2_t low = vzipq_u8(c.val[j], c.val[j]);
                uint8x16x2_t out;
                out.val[0] = low.val[0];
                out.val[1] = vcombine_u8(vld1_u8(msb8[bits >> 24]), vld1_u8(msb8[(bits >> 16) & 0xFFu]));
                vst2q_u8((uint8_t *)&dst[32u * j], out);
                out.val[0] = low.val[1];
                out.val[1] = vcombine_u8(vld1_u8(msb8[(bits >> 8) & 0xFFu]), vld1_u8(msb8[bits & 0xFFu]));
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
    smi_frame_store(dst, lv, job->clocks != 0, job->adat, job->msb8);
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
