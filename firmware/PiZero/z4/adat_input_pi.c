// The Pi's ADAT receiver (adat_input_pi.h). Each capture chunk is decoded in its interrupt into a word
// ring of our own, each word stamped with where its last cell is. Reads of adat_input.c's dma_hw
// copy the words stamped a fixed delay before now into DSPi's ring and move channel 15's write address
// past them. The delay outlasts a chunk and its decoding, so the address moves smoothly with the wire.

#include <string.h>

#include "hardware/dma.h"
#include "input_capture_arena.h"

#include "adat_decode.h"
#include "adat_input_pi.h"
#include "irq.h"
#include "pcm_capture_pi.h"
#include "pmu.h"

#define DMA_CH 15u                      // adat_input.c's ADAT_RX_DMA_CH
#define RING_WORDS INPUT_ARENA_ADAT_WORDS
#define OUT_WORDS 4096u                 // 10 ms at 48 kHz, more than the delay and a chunk
#define DELAY_SAMPLES (2000u * (PCM_CAPTURE_SAMPLE_HZ / 1000000u))     // 2 ms
#define CHUNK_SAMPLES 131072u           // pcm_capture_pi.c's chunk
_Static_assert((OUT_WORDS & (OUT_WORDS - 1u)) == 0, "the decoder's ring is a power of two");

static uint32_t out[OUT_WORDS], out_at[OUT_WORDS];
static adat_decoder_t dec = {.out = out, .out_at = out_at, .out_mask = OUT_WORDS - 1u};
static volatile bool running;
static uint32_t rate = 48000u;
static uint32_t pub;                    // words copied into DSPi's ring
static uint64_t pub_total;              // the same, without wrapping, for the bench

static adat_input_pi_bench_t bench;
static adat_input_pi_status_t st;
static uint64_t samples, dec_cycles;
static uint32_t chunks, dec_ev[PMU_N_EVENTS];

// The bench's frame reader: published bit `fbit` starts a frame once synced. x1 and x2 are the two
// previous samples of each channel; hist counts how many of them are valid.
static bool bench_on;
static uint64_t fbit;
static uint32_t synced, hist;
static int32_t x1[8], x2[8];

static void on_capture(const uint32_t *ring, uint32_t ring_words, uint64_t end) {
    // A decoder that fell most of a ring behind gives up the old samples rather than read overwritten ones.
    if (end - dec.pos > (uint64_t)ring_words * 32u - 2u * CHUNK_SAMPLES) {
        adat_decode_reset(&dec, end - CHUNK_SAMPLES);
    }
    pmu_snap_t a, b;
    pmu_read(&a);
    adat_decode_ring(&dec, ring, ring_words, end);
    pmu_read(&b);
    for (uint32_t i = 0; i < PMU_N_EVENTS; i++) {
        dec_ev[i] += b.ev[i] - a.ev[i];
    }
    uint32_t c = b.cycles - a.cycles;
    dec_cycles += c;
    st.chunk_cycles_max = c > st.chunk_cycles_max ? c : st.chunk_cycles_max;
    samples = end;
    chunks++;
}

static uint32_t ring_bits(uint64_t k, uint32_t n) {     // n <= 32 published bits from bit k, MSB first
    const uint32_t *r = input_capture_arena.adat_ring;
    uint32_t i = (uint32_t)(k >> 5);
    uint64_t v = (uint64_t)r[i & (RING_WORDS - 1u)] << 32 | r[(i + 1u) & (RING_WORDS - 1u)];
    return (uint32_t)(v >> (64u - (k & 31u) - n)) & (n == 32u ? 0xFFFFFFFFu : (1u << n) - 1u);
}

// Reads the published frames as DSPi does, and checks the bench's channels for a clean sine.
static void bench_frames(void) {
    uint64_t avail = (uint64_t)pub_total * 32u;
    if (!synced) {
        uint64_t from = avail > (RING_WORDS - 64u) * 32u ? avail - (RING_WORDS - 64u) * 32u : 0u;
        if (fbit < from) {
            fbit = from;
        }
        while (fbit + 44u <= avail) {
            if (ring_bits(fbit, 32) == 0u) {
                fbit += 21u;    // no sync starts in the first 21 of 32 zeros: a dark line is skipped fast
                continue;
            }
            if (ring_bits(fbit, 12) == 0x801u) {
                synced = 1;
                hist = 0;
                st.syncs++;
                break;
            }
            fbit++;
        }
    }
    while (synced && fbit + 256u + 32u <= avail) {
        if (avail - fbit > (RING_WORDS - 64u) * 32u || ring_bits(fbit, 12) != 0x801u) {
            st.header_errors++;
            synced = 0;
            fbit++;
            return;
        }
        st.frames++;
        for (uint32_t ch = 0; ch < 8u; ch++) {
            uint32_t v = ring_bits(fbit + 16u + 30u * ch, 30);
            uint32_t s24 = (((v >> 25) & 0xFu) << 20) | (((v >> 20) & 0xFu) << 16) | (((v >> 15) & 0xFu) << 12) |
                           (((v >> 10) & 0xFu) << 8) | (((v >> 5) & 0xFu) << 4) | (v & 0xFu);
            int32_t x = (int32_t)(s24 << 8) >> 8;
            int32_t a = x < 0 ? -x : x;
            st.peak[ch] = a > st.peak[ch] ? a : st.peak[ch];
            if ((bench.check >> ch) & 1u) {
                if (hist >= 2u) {
                    int32_t e = x + x2[ch] - (int32_t)(((int64_t)bench.sine_c_q30 * x1[ch]) >> 30);
                    uint32_t ae = (uint32_t)(e < 0 ? -e : e);
                    st.sine_checked++;
                    st.sine_worst = ae > st.sine_worst ? ae : st.sine_worst;
                    st.sine_errors += ae > bench.sine_tol;
                }
                x2[ch] = x1[ch];
                x1[ch] = x;
            }
        }
        hist += hist < 2u;
        fbit += 256u;
    }
}

static void publish(void) {
    if (!running) {
        return;
    }
    uint32_t n = *(volatile uint32_t *)&dec.out_n;
    uint64_t now = pcm_capture_sample_at(pcm_capture_micros64());
    uint32_t limit = (uint32_t)((now - DELAY_SAMPLES) << 8);   // the stamps are in samples << 8
    uint32_t save = irq_global_disable();
    uint32_t decoded = (uint32_t)dec.pos;
    irq_global_restore(save);
    int32_t headroom = (int32_t)(decoded - (uint32_t)(now - DELAY_SAMPLES));   // in samples
    st.headroom_min = headroom < st.headroom_min ? headroom : st.headroom_min;
    // Unpublished words about to be overwritten are given up whole frames at a time (8 words), so DSPi
    // keeps its place in the frame, as its own lap guard does.
    if (n - pub > OUT_WORDS - 256u) {
        uint32_t skip = (n - pub - OUT_WORDS / 2u) & ~7u;
        pub += skip;
        pub_total += skip;
        st.overflows++;
    }
    while (pub != n && (int32_t)(out_at[pub & (OUT_WORDS - 1u)] - limit) <= 0) {
        input_capture_arena.adat_ring[pub & (RING_WORDS - 1u)] = out[pub & (OUT_WORDS - 1u)];
        pub++;
        pub_total++;
    }
    pico_shim_dma_hw.ch[DMA_CH].write_addr = (uint32_t)(uintptr_t)&input_capture_arena.adat_ring[pub & (RING_WORDS - 1u)];
    if (bench_on) {
        bench_frames();
    }
}

dma_hw_t *adat_input_pi_dma_hw(void) {
    publish();
    return &pico_shim_dma_hw;
}

void adat_input_pi_set_y(uint32_t y) {
    // adat_input.c sets a cell of 2 y + 5 cycles of its 307.2 MHz clock: 25 for 48 kHz, 27 for 44.1 kHz.
    uint32_t fs = 1200000u / (2u * y + 5u) > 46000u ? 48000u : 44100u;
    uint32_t save = irq_global_disable();
    rate = fs;
    adat_decode_set_rate(&dec, PCM_CAPTURE_SAMPLE_HZ, fs);
    irq_global_restore(save);
}

void adat_input_pi_enable(bool on) {
    if (on == running) {
        return;
    }
    if (!on) {
        pcm_capture_stop(on_capture);
        running = false;
        return;
    }
    uint32_t save = irq_global_disable();
    adat_decode_set_rate(&dec, PCM_CAPTURE_SAMPLE_HZ, rate);
    adat_decode_reset(&dec, 0);
    dec.out_n = 0;
    pub = 0;
    pub_total = 0;
    fbit = 0;
    synced = hist = 0;
    st.headroom_min = 0x7FFFFFFF;
    pico_shim_dma_hw.ch[DMA_CH].write_addr = (uint32_t)(uintptr_t)input_capture_arena.adat_ring;
    running = true;
    irq_global_restore(save);
    pcm_capture_start(on_capture);
}

bool adat_input_pi_running(void) {
    return running;
}

void adat_input_pi_bench(const adat_input_pi_bench_t *b) {
    uint32_t save = irq_global_disable();
    if (b) {
        bench = *b;
    } else {
        memset(&bench, 0, sizeof(bench));
    }
    bench_on = b != NULL;
    st.frames = st.header_errors = st.syncs = 0;
    st.sine_checked = st.sine_errors = st.sine_worst = 0;
    memset(st.peak, 0, sizeof(st.peak));
    st.overflows = 0;
    st.chunk_cycles_max = 0;
    st.headroom_min = 0x7FFFFFFF;
    synced = hist = 0;
    fbit = pub_total * 32u;
    irq_global_restore(save);
}

void adat_input_pi_status(adat_input_pi_status_t *out_st) {
    uint32_t save = irq_global_disable();
    st.magic = ADAT_INPUT_PI_MAGIC;
    st.running = running;
    st.rate = rate;
    st.chunks = chunks;
    st.samples_lo = (uint32_t)samples;
    st.samples_hi = (uint32_t)(samples >> 32);
    st.words = dec.out_n;
    st.published = pub;
    st.flips = dec.flips;
    st.dec_cycles_lo = (uint32_t)dec_cycles;
    st.dec_cycles_hi = (uint32_t)(dec_cycles >> 32);
    st.dec_inst = dec_ev[0];
    st.dec_l1d_refill = dec_ev[2];
    st.dec_l2d_refill = dec_ev[3];
    st.dec_br_mispred = dec_ev[4];
    *out_st = st;
    irq_global_restore(save);
}
