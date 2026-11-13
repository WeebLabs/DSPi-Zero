// Checks smi_frame.h's smi_frames_c against the table-driven composer it replaced (kept here as the
// reference), and smi_frames_a32 against both: random ring frames for every mix of slot types, with and
// without the I2S clocks and ADAT, must give the same 256 words, and the key must skip only a repeat.
// For the Mac it builds without the assembly: cc -O2 -I.. smi_frame_test.c

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

// in[s] is the slot's raw frame: S/PDIF words 0-3, or I2S's last L and R, then L and R.
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

int main(void) {
    static const uint32_t zeros[4];
    tables_init();
    uint32_t fails = 0, runs = 0;
    for (uint32_t t = 0; t < 81u * 4u * 100u; t++) {
        smi_frame_job_t job = {.clocks = (t / 81u) & 1u, .msb8 = lut_msb8};
        uint32_t buf[4][RUN * 4u], adat[RUN * 8u];
        uint32_t c = t % 81u, shape = rnd() % 4u, n = 1u + rnd() % RUN;   // random, all zeros, all ones, sparse
        for (uint32_t s = 0; s < 4u; s++, c /= 3u) {
            job.type[s] = c % 3u;
            job.raw[s] = job.type[s] == SMI_SLOT_OFF ? zeros : buf[s];
            job.stride[s] = job.type[s] == SMI_SLOT_SPDIF ? 16u : job.type[s] == SMI_SLOT_I2S ? 8u : 0u;
            for (uint32_t j = 0; j < RUN * 4u; j++) {
                buf[s][j] = shape == 0 ? rnd() : shape == 1 ? 0u : shape == 2 ? 0xFFFFFFFFu : 1u << (rnd() % 32u);
            }
        }
        for (uint32_t j = 0; j < RUN * 8u; j++) {
            adat[j] = rnd();
        }
        job.adat = (t / 162u) & 1u ? adat : NULL;
        static uint16_t want[RUN * 256];
        for (uint32_t k = 0; k < n; k++) {
            uint32_t in[4][4];
            for (uint32_t s = 0; s < 4u; s++) {
                memcpy(in[s], (const uint8_t *)job.raw[s] + k * job.stride[s], 16);
            }
            reference(&want[k * 256u], in, job.type, job.clocks, job.adat ? &adat[8u * k] : NULL);
        }
        fails += check(smi_frames_c, "smi_frames_c", &job, n, want);
#ifdef __arm__
        fails += check(smi_frames_a32, "smi_frames_a32", &job, n, want);
#endif
        runs += n;
    }
    printf("%s: %u frames, %u differ\n", fails ? "FAIL" : "PASS", (unsigned)runs, (unsigned)fails);
    return fails != 0;
}
