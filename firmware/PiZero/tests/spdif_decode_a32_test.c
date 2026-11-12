// Checks spdif_decode_a32.S against the C reader in spdif_decode.c on random lines, cells and anchors.
// It runs as ARM code under QEMU, and spdif_decode_test.c builds the same way (with ../spdif_decode.c
// in place of this file) to run the whole decoder on the assembly.

// Build, to any scratch path:
//   arm-none-eabi-gcc -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -marm -O2 -I.. --specs=rdimon.specs
//     -Wl,-e,qemu_start -Wl,-Ttext-segment=0x40010000 -o a32t.elf spdif_decode_a32_test.c
//     ../spdif_decode_a32.S qemu_start.S -lm

// Run (it prints PASS):
//   qemu-system-arm -M virt -cpu cortex-a7 -m 512 -semihosting-config enable=on,target=native
//     -display none -serial none -monitor none -kernel a32t.elf

#include <stdio.h>
#include <stdlib.h>

#define SPDIF_DECODE_PORTABLE
#define spdif_read_levels ref_read_levels
#include "../spdif_decode.c"
#undef spdif_read_levels

uint64_t spdif_read_levels(const uint32_t *base, uint32_t a, uint32_t c);

// A line of runs one to three cells long with jittered edges, so most anchors are found and some are not.
static void fill(uint32_t *w, uint32_t words, double cell) {
    uint32_t level = 0;
    double edge = 0;
    for (uint32_t s = 0; s < words * 32u; s++) {
        if (s % 32u == 0) {
            w[s / 32u] = 0;
        }
        while ((double)s >= edge) {
            level ^= 1u;
            edge += cell * (1 + rand() % 3 + (rand() % 100 - 50) / 120.0);
        }
        w[s / 32u] |= level << (31u - s % 32u);
    }
}

int main(void) {
    enum { WORDS = 256 };
    static uint32_t w[WORDS];
    uint32_t bad = 0, found = 0, n = 0;
    for (uint32_t t = 0; t < 4000u; t++) {
        uint32_t c = (4u << 8) + (uint32_t)rand() % (36u << 8);
        fill(w, WORDS, (t & 1u) ? c / 256.0 : 3.0 + rand() % 40);
        for (uint32_t k = 0; k < 50u; k++, n++) {
            uint32_t a = (uint32_t)rand() % 2000u;
            uint64_t r1 = ref_read_levels(w + 4, a, c), r2 = spdif_read_levels(w + 4, a, c);
            found += (uint32_t)r1 != 0;
            // A read that misses an edge returns a low word of 0 and nothing else.
            if ((uint32_t)r1 ? r1 != r2 : (uint32_t)r2 != 0) {
                if (bad++ < 5u) {
                    printf("c %u a %u: C %016llx, asm %016llx\n", (unsigned)c, (unsigned)a, (unsigned long long)r1,
                           (unsigned long long)r2);
                }
            }
        }
    }
    printf("%u reads, %u with every edge found, %u differ\n", (unsigned)n, (unsigned)found, (unsigned)bad);
    puts(bad || found < n / 20u ? "FAIL" : "PASS");
    return bad != 0;
}
