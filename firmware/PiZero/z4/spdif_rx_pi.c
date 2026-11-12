// pico_spdif_rx's API (spdif_rx.h) on the Pi, for DSPi's spdif_input.c, which is compiled unchanged.
// The PCM block samples GPIO 20 at 125 MHz from PLLD, which input tracking never moves, and a DMA loop
// fills a ring a chunk at a time. Each chunk's interrupt decodes it (spdif_decode.c) and builds 384-
// subframe blocks into the library's FIFO, with the library's lock states, callbacks and block timing.

#include <string.h>

#include "spdif_rx.h"
#include "input_capture_arena.h"

#include "irq.h"
#include "mbox.h"
#include "pll_out.h"
#include "spdif_decode.h"
#include "spdif_rx_pi.h"
#include "pmu.h"

void data_invalidate(volatile void *starting_address, size_t size);

#define REG(a) (*(volatile uint32_t *)(a))

#define PCM_CS 0x3F203000u
#define PCM_MODE 0x3F203008u
#define PCM_RXC 0x3F20300Cu
#define PCM_TXC 0x3F203010u
#define PCM_DREQ 0x3F203014u
#define PCM_INTEN 0x3F203018u
#define PCM_FIFO_BUS 0x7E203004u
#define CS_EN (1u << 0)
#define CS_RXON (1u << 1)
#define CS_RXCLR (1u << 4)
#define CS_DMAEN (1u << 9)
#define CS_RXERR (1u << 16)
#define CS_SYNC (1u << 24)
#define CS_STBY (1u << 25)
#define GPFSEL2 0x3F200008u

#define DMA_BASE 0x3F007000u
#define DMA_ENABLE 0x3F007FF0u
#define DMA_CS(ch) (DMA_BASE + 0x100u * (ch) + 0x00u)
#define DMA_CONBLK(ch) (DMA_BASE + 0x100u * (ch) + 0x04u)
#define DMA_DEST(ch) (DMA_BASE + 0x100u * (ch) + 0x10u)
#define DMA_DEBUG(ch) (DMA_BASE + 0x100u * (ch) + 0x20u)
#define DMA_CS_RESET (1u << 31)
#define DMA_CS_INT (1u << 2)
#define DMA_CS_RUN ((1u << 28) | (8u << 20) | (8u << 16) | 1u)
#define DMA_TI_INTEN (1u << 0)
#define DMA_TI_WAIT_RESP (1u << 3)
#define DMA_TI_DEST_INC (1u << 4)
#define DMA_TI_SRC_DREQ (1u << 10)
#define DMA_TI_PERMAP_PCM_RX (3u << 16)
#define DMA_TI_NO_WIDE_BURSTS (1u << 26)
#define BUS(p) ((uint32_t)(p) | 0xC0000000u)

#define SRC_PLLD 6u
#define PLLD_DIV 4u
#define SAMPLE_HZ 125000000u        // 500 MHz PLLD / 4
#define CHUNK_WORDS 4096u           // 131072 samples, about 1 ms
#define CHUNKS 8u
#define CHUNK_BYTES (CHUNK_WORDS * 4u)
#define DIN_GPIO_SHIFT 0u           // GPIO 20 in GPFSEL2
#define STABLE_BLOCKS 4u            // aligned blocks at one rate before STABLE
#define DECODE_TIMEOUT_US 10000u    // no block for this long loses the signal

typedef struct {
    uint32_t ti, src, dst, len, stride, next, pad[2];
} dma_cb_t;

static dma_cb_t cbs[CHUNKS] __attribute__((section(".nocache"), aligned(32)));
#define RING_WORDS (CHUNKS * CHUNK_WORDS)
// The DMA fills RING_WORDS; the tail after them repeats the ring's start for the decoder.
static uint32_t ring[RING_WORDS + SPDIF_RING_TAIL] __attribute__((aligned(64)));

#define fifo_buff (input_capture_arena.spdif_fifo)
_Static_assert(((CHUNKS * CHUNK_WORDS) & (CHUNKS * CHUNK_WORDS - 1u)) == 0, "the decoder's ring is a power of two");
_Static_assert(sizeof(input_capture_arena.spdif_fifo) == SPDIF_RX_FIFO_SIZE * 4, "arena S/PDIF FIFO size");

static spdif_rx_config_t gcfg;
static uint32_t dma_ch = 0xFF;
static volatile bool running;
static uint32_t next_chunk;
static uint64_t cap_end;        // samples in completed chunks since the start
static spdif_decoder_t dec;

static volatile spdif_rx_state_t state;
static void (*on_stable_func)(spdif_rx_samp_freq_t samp_freq);
static void (*on_lost_stable_func)(void);
static spdif_rx_samp_freq_t samp_freq;
static float samp_freq_actual;
static uint32_t stable_run;
static uint32_t parity_err_count;
static uint8_t c_bits[SPDIF_BLOCK_SIZE / 16];

// What every subframe touches, together so one base address reaches it all. The block being assembled
// starts at a B preamble: 384 subframes alternating B or M, then W. The FIFO's write and read pointers
// count modulo twice the FIFO, as pico_spdif_rx's do, so full and empty differ.
#define PTR_SPAN (SPDIF_RX_FIFO_SIZE * 2u)
static struct {
    uint32_t blk_pos, blk_parity;
    volatile uint32_t wr_ptr, rd_ptr;
    uint64_t blk_at;
    bool blk_ok;
    uint8_t blk_c[SPDIF_BLOCK_SIZE / 16];
} rx = {.blk_pos = SPDIF_BLOCK_SIZE};
static uint64_t prev_blk_at;
static uint32_t block_count;
static uint32_t last_block_us;

static inline uint32_t fifo_used(void) {
    return (rx.wr_ptr + PTR_SPAN - rx.rd_ptr) % PTR_SPAN;
}

static volatile uint32_t block_ref_seq, block_ref_count;
static volatile uint64_t block_ref_time_us;

// The interrupt's time and the sample index the DMA had reached then, for timing a sample index.
static uint64_t irq_us, irq_index;
static uint32_t irqs, decode_us;
static uint32_t inv_cycles, dec_cycles, dec_ev[PMU_N_EVENTS];
static bool monitor;

static uint32_t micros(void) {
    return REG(0x3F003004u);
}

static uint64_t micros64(void) {
    uint32_t hi = REG(0x3F003008u), lo = REG(0x3F003004u);
    if (hi != REG(0x3F003008u)) {
        hi = REG(0x3F003008u);
        lo = REG(0x3F003004u);
    }
    return (uint64_t)hi << 32 | lo;
}

static uint64_t time_of(uint64_t at) {
    return irq_us - (irq_index - at) / (SAMPLE_HZ / 1000000u);
}

static spdif_rx_samp_freq_t classify(float hz) {
    static const spdif_rx_samp_freq_t rates[] = {SAMP_FREQ_44100, SAMP_FREQ_48000, SAMP_FREQ_88200,
                                                 SAMP_FREQ_96000, SAMP_FREQ_176400, SAMP_FREQ_192000};
    for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        float r = (float)rates[i];
        if (hz > r * 0.98f && hz < r * 1.02f) {
            return rates[i];
        }
    }
    return SAMP_FREQ_NONE;
}

static void lose_signal(void) {
    bool was_stable = state == SPDIF_RX_STATE_STABLE;
    state = SPDIF_RX_STATE_NO_SIGNAL;
    stable_run = 0;
    rx.blk_pos = SPDIF_BLOCK_SIZE;
    prev_blk_at = 0;
    if (was_stable && on_lost_stable_func) {
        on_lost_stable_func();
    }
}

static __attribute__((noinline)) void block_done(void) {
    float hz = prev_blk_at ? (float)(SPDIF_BLOCK_SIZE / 2) * (float)SAMPLE_HZ / (float)(rx.blk_at - prev_blk_at) : 0.0f;
    prev_blk_at = rx.blk_at;
    spdif_rx_samp_freq_t sf = classify(hz);
    samp_freq_actual = hz;
    parity_err_count += rx.blk_parity;
    block_count++;
    last_block_us = (uint32_t)irq_us;
    block_ref_seq++;
    __asm__ volatile("dmb" ::: "memory");
    block_ref_count = block_count;
    block_ref_time_us = time_of(rx.blk_at);
    __asm__ volatile("dmb" ::: "memory");
    block_ref_seq++;
    if (!rx.blk_ok || sf == SAMP_FREQ_NONE || (state == SPDIF_RX_STATE_STABLE && sf != samp_freq)) {
        if (state == SPDIF_RX_STATE_STABLE) {
            lose_signal();
        }
        samp_freq = sf;
        stable_run = 0;
        return;
    }
    memcpy(c_bits, rx.blk_c, sizeof(c_bits));
    stable_run = sf == samp_freq ? stable_run + 1u : 1u;
    samp_freq = sf;
    if (state == SPDIF_RX_STATE_NO_SIGNAL) {
        state = SPDIF_RX_STATE_WAITING_STABLE;
    }
    if (state == SPDIF_RX_STATE_WAITING_STABLE && stable_run >= STABLE_BLOCKS) {
        state = SPDIF_RX_STATE_STABLE;
        if (on_stable_func) {
            on_stable_func(sf);
        }
    }
}

// A B preamble starts a block. Kept out of on_subframe so the other 383 subframes of a block run
// without a stack frame.
static __attribute__((noinline)) void block_start(uint64_t at) {
    if (rx.blk_pos != SPDIF_BLOCK_SIZE) {
        stable_run = 0;     // a B inside a block: the stream slipped
        if (state == SPDIF_RX_STATE_STABLE) {
            lose_signal();
        }
    }
    rx.blk_ok = true;
    rx.blk_at = at;
    rx.blk_parity = 0;
    memset(rx.blk_c, 0, sizeof(rx.blk_c));
}

static void on_subframe(uint32_t w, uint64_t at, void *ctx) {
    (void)ctx;
    uint32_t sync = w & 0xFu, pos = rx.blk_pos;
    if (sync == SPDIF_SYNC_B) {
        block_start(at);
        pos = 0;
    } else if (pos == SPDIF_BLOCK_SIZE) {
        return;     // waiting for the next block's B
    } else if ((pos & 1u) ? sync != SPDIF_SYNC_W : sync == SPDIF_SYNC_W) {
        rx.blk_ok = false;     // an M where a W belongs, or the reverse
    }
    uint32_t v = w & 0xFFFFFFF0u;     // even parity over time slots 4 to 31
    v ^= v >> 16;
    v ^= v >> 8;
    v ^= v >> 4;
    rx.blk_parity += (0x6996u >> (v & 0xFu)) & 1u;
    if ((w & 0x40000000u) && !(pos & 1u)) {     // the C bit, from the frame's first subframe
        rx.blk_c[pos / 16u] |= (uint8_t)(1u << ((pos / 2u) % 8u));
    }
    // A reader that has fallen a whole FIFO behind loses its oldest pair. Both pointers stay below
    // PTR_SPAN, so each wrap is one subtraction.
    uint32_t wr = rx.wr_ptr, rd = rx.rd_ptr;
    if ((wr >= rd ? wr - rd : wr + PTR_SPAN - rd) >= SPDIF_RX_FIFO_SIZE) {
        rd += 2u;
        rx.rd_ptr = rd >= PTR_SPAN ? rd - PTR_SPAN : rd;
    }
    fifo_buff[wr >= SPDIF_RX_FIFO_SIZE ? wr - SPDIF_RX_FIFO_SIZE : wr] = w;
    rx.wr_ptr = wr + 1u == PTR_SPAN ? 0u : wr + 1u;
    rx.blk_pos = ++pos;
    if (pos == SPDIF_BLOCK_SIZE) {
        block_done();
    }
}

// Drops a chunk the DMA has rewritten from the cache. Chunks are whole cache lines that only the DMA
// writes, so there is nothing to clean first, and discarding is cheaper than data_invalidate.
static void chunk_invalidate(const uint32_t *chunk) {
    for (uintptr_t a = (uintptr_t)chunk, end = a + CHUNK_BYTES; a < end; a += 64u) {
        __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" ::"r"(a));     // DCIMVAC
    }
    __asm__ volatile("dsb" ::: "memory");
}

static void dma_irq(void) {
    REG(DMA_CS(dma_ch)) = DMA_CS_RUN | DMA_CS_INT;
    if (!running) {
        return;
    }
    irq_us = micros64();
    uint32_t off = REG(DMA_DEST(dma_ch)) - BUS(ring);
    if (off >= RING_WORDS * 4u) {
        return;     // between control blocks; the next interrupt catches up
    }
    uint32_t cur = off / CHUNK_BYTES;
    irqs++;
    uint32_t c0 = pmu_cycles();
    while (next_chunk != cur) {
        chunk_invalidate(&ring[next_chunk * CHUNK_WORDS]);
        if (next_chunk == 0) {
            memcpy(&ring[RING_WORDS], ring, SPDIF_RING_TAIL * 4u);
        }
        cap_end += CHUNK_WORDS * 32u;
        next_chunk = (next_chunk + 1u) % CHUNKS;
    }
    inv_cycles += pmu_cycles() - c0;
    irq_index = cap_end + (off % CHUNK_BYTES) / 4u * 32u;
    // A decoder that fell most of a ring behind gives up the old samples rather than read overwritten ones.
    if (cap_end - spdif_decode_needs(&dec, cap_end) > (CHUNKS - 2u) * CHUNK_WORDS * 32u) {
        spdif_decode_skip(&dec, cap_end - CHUNK_WORDS * 32u);
    }
    pmu_snap_t a, b;
    pmu_read(&a);
    spdif_decode_ring(&dec, ring, RING_WORDS, cap_end, on_subframe, NULL);
    pmu_read(&b);
    dec_cycles += b.cycles - a.cycles;
    for (uint32_t i = 0; i < PMU_N_EVENTS; i++) {
        dec_ev[i] += b.ev[i] - a.ev[i];
    }
    decode_us += micros() - (uint32_t)irq_us;
    if (state != SPDIF_RX_STATE_NO_SIGNAL && (uint32_t)irq_us - last_block_us > DECODE_TIMEOUT_US) {
        lose_signal();
    }
}

static uint32_t pick_channel(void) {
    static const uint8_t prefer[] = {6, 5, 2};      // smi_out.c takes 4
    uint32_t mask = mbox_dma_channels();
    for (unsigned i = 0; i < sizeof(prefer); i++) {
        if (mask & (1u << prefer[i])) {
            return prefer[i];
        }
    }
    return 6;
}

static void capture_stop(void) {
    if (dma_ch == 0xFF) {
        return;
    }
    running = false;
    irq_disable(16u + dma_ch);
    REG(PCM_CS) = CS_EN | CS_STBY;
    REG(DMA_CS(dma_ch)) = DMA_CS_RESET;
    REG(PCM_CS) = 0;
    pll_out_pcm_clock(SRC_PLLD, 0);
    REG(GPFSEL2) &= ~(7u << DIN_GPIO_SHIFT);
}

// Clock and frame master, 64-bit frames of two 32-bit channels, so every PCM clock is one sample.
// Only the data pin is routed; the clock and frame stay inside the block.
static void capture_start(void) {
    if (dma_ch == 0xFF) {
        dma_ch = pick_channel();
    }
    uint32_t ch = dma_ch;
    capture_stop();
    pll_out_pcm_clock(SRC_PLLD, PLLD_DIV);
    REG(GPFSEL2) = (REG(GPFSEL2) & ~(7u << DIN_GPIO_SHIFT)) | (4u << DIN_GPIO_SHIFT);   // ALT0: PCM_DIN
    REG(PCM_CS) = 0;
    REG(PCM_CS) = CS_EN | CS_STBY;
    REG(PCM_MODE) = (63u << 10) | 32u;
    REG(PCM_RXC) = (1u << 31) | (1u << 30) | (8u << 16) | (1u << 15) | (1u << 14) | (32u << 4) | 8u;
    REG(PCM_TXC) = 0;
    REG(PCM_DREQ) = (0x10u << 24) | (0x30u << 16) | (0x30u << 8) | 0x20u;
    REG(PCM_INTEN) = 0;
    REG(PCM_CS) = CS_EN | CS_STBY | CS_RXCLR;
    REG(PCM_CS) = CS_EN | CS_STBY | CS_SYNC;
    uint32_t t0 = micros();
    while (!(REG(PCM_CS) & CS_SYNC) && micros() - t0 < 100u) {
    }
    REG(PCM_CS) = CS_EN | CS_STBY | CS_RXERR;
    uint32_t ti = DMA_TI_INTEN | DMA_TI_WAIT_RESP | DMA_TI_DEST_INC | DMA_TI_SRC_DREQ | DMA_TI_PERMAP_PCM_RX |
                  DMA_TI_NO_WIDE_BURSTS;
    for (uint32_t i = 0; i < CHUNKS; i++) {
        cbs[i] = (dma_cb_t){ti, PCM_FIFO_BUS, BUS(&ring[i * CHUNK_WORDS]), CHUNK_BYTES, 0,
                            BUS(&cbs[(i + 1u) % CHUNKS]), {0, 0}};
    }
    data_invalidate(ring, RING_WORDS * 4u);
    __asm__ volatile("dsb" ::: "memory");
    spdif_decode_reset(&dec);
    next_chunk = 0;
    cap_end = 0;
    REG(DMA_ENABLE) |= 1u << ch;
    REG(DMA_CS(ch)) = DMA_CS_RESET;
    t0 = micros();
    while (micros() - t0 < 2u) {
    }
    REG(DMA_DEBUG(ch)) = 7u;
    REG(DMA_CONBLK(ch)) = BUS(&cbs[0]);
    irq_set_handler(16u + ch, dma_irq);
    irq_enable(16u + ch);
    running = true;
    REG(DMA_CS(ch)) = DMA_CS_RUN;
    REG(PCM_CS) = CS_EN | CS_STBY | CS_DMAEN | CS_RXON;
}

void spdif_rx_start(const spdif_rx_config_t *config) {
    if (monitor) {
        spdif_rx_pi_monitor(false);     // DSPi's input takes the receiver from the bench
    }
    uint32_t save = irq_global_disable();
    gcfg = *config;
    state = SPDIF_RX_STATE_NO_SIGNAL;
    samp_freq = SAMP_FREQ_NONE;
    samp_freq_actual = 0.0f;
    stable_run = 0;
    parity_err_count = 0;
    rx.blk_pos = SPDIF_BLOCK_SIZE;
    prev_blk_at = 0;
    rx.wr_ptr = rx.rd_ptr = 0;
    block_count = 0;
    last_block_us = micros();
    memset(c_bits, 0, sizeof(c_bits));
    irq_global_restore(save);
    capture_start();
}

void spdif_rx_end(void) {
    uint32_t save = irq_global_disable();
    capture_stop();
    state = SPDIF_RX_STATE_NO_SIGNAL;
    irq_global_restore(save);
}

void spdif_rx_set_callback_on_stable(void (*func)(spdif_rx_samp_freq_t samp_freq)) {
    on_stable_func = func;
}

void spdif_rx_set_callback_on_lost_stable(void (*func)()) {
    on_lost_stable_func = func;
}

spdif_rx_state_t spdif_rx_get_state(void) {
    return state;
}

float spdif_rx_get_samp_freq_actual(void) {
    return samp_freq_actual;
}

void spdif_rx_get_block_ref(uint32_t *block_cnt, uint64_t *time_us) {
    uint32_t s1, s2;
    do {
        s1 = block_ref_seq;
        __asm__ volatile("dmb" ::: "memory");
        *block_cnt = block_ref_count;
        *time_us = block_ref_time_us;
        __asm__ volatile("dmb" ::: "memory");
        s2 = block_ref_seq;
    } while ((s1 & 1u) || s1 != s2);
}

spdif_rx_samp_freq_t spdif_rx_get_samp_freq(void) {
    return samp_freq;
}

void spdif_rx_get_c_bits(void *ptr, size_t size, uint32_t offset) {
    uint32_t save = irq_global_disable();
    for (size_t i = 0; i < size && offset + i < sizeof(c_bits); i++) {
        ((uint8_t *)ptr)[i] = c_bits[offset + i];
    }
    irq_global_restore(save);
}

uint32_t spdif_rx_get_parity_err_count(void) {
    return parity_err_count;
}

// Every word in the FIFO is decoded and final, so the live count and the block count are the same.
uint32_t spdif_rx_get_fifo_count(void) {
    return fifo_used();
}

uint32_t spdif_rx_get_fifo_count_live(void) {
    return state == SPDIF_RX_STATE_STABLE ? fifo_used() : 0u;
}

static uint32_t read_fifo(uint32_t **buff, uint32_t req_count, bool live) {
    uint32_t save = irq_global_disable();
    uint32_t avail = (live ? spdif_rx_get_fifo_count_live() : spdif_rx_get_fifo_count()) & ~1u;
    uint32_t n = req_count < avail ? req_count : avail;
    uint32_t at = rx.rd_ptr % SPDIF_RX_FIFO_SIZE;
    if (at + n > SPDIF_RX_FIFO_SIZE) {
        n = SPDIF_RX_FIFO_SIZE - at;
    }
    *buff = &fifo_buff[at];
    rx.rd_ptr = (rx.rd_ptr + n) % PTR_SPAN;
    irq_global_restore(save);
    return n;
}

uint32_t spdif_rx_read_fifo_live(uint32_t **buff, uint32_t req_count) {
    return read_fifo(buff, req_count, true);
}

uint32_t spdif_rx_read_fifo(uint32_t **buff, uint32_t req_count) {
    return read_fifo(buff, req_count, false);
}

void spdif_rx_pi_status(spdif_rx_pi_status_t *out) {
    uint32_t save = irq_global_disable();
    *out = (spdif_rx_pi_status_t){
        .magic = SPDIF_RX_PI_MAGIC, .running = running, .state = state, .dma_channel = dma_ch,
        .samples_lo = (uint32_t)cap_end, .samples_hi = (uint32_t)(cap_end >> 32), .subframes = dec.subframes,
        .decode_errors = dec.errors, .cell_q8 = dec.cell_q8, .blocks = block_count, .samp_freq = samp_freq,
        .samp_freq_actual_mhz = (uint32_t)(samp_freq_actual * 1000.0f), .parity_errors = parity_err_count,
        .fifo_used = fifo_used(), .irqs = irqs, .decode_us = decode_us, .pcm_cs = REG(PCM_CS),
        .dma_debug = dma_ch == 0xFF ? 0 : REG(DMA_DEBUG(dma_ch)) & 7u, .monitor = monitor,
    };
    out->inv_cycles = inv_cycles;
    out->dec_cycles = dec_cycles;
    out->dec_inst = dec_ev[0];
    out->dec_l1d_refill = dec_ev[2];
    out->dec_l2d_refill = dec_ev[3];
    out->dec_br_mispred = dec_ev[4];
    for (uint32_t i = 0; i < 8u; i++) {
        out->last[i] = fifo_buff[(rx.wr_ptr + PTR_SPAN - 8u + i) % SPDIF_RX_FIFO_SIZE];
    }
    irq_global_restore(save);
}

bool spdif_rx_pi_monitor(bool on) {
    if (on == monitor) {
        return true;
    }
    if (on) {
        if (running) {
            return false;   // DSPi's S/PDIF input is using it
        }
        input_arena_claim(INPUT_ARENA_SPDIF);
        spdif_rx_config_t cfg = {.data_pin = 20, .flags = SPDIF_RX_FLAGS_ALL};
        spdif_rx_start(&cfg);
    } else {
        spdif_rx_end();
        input_arena_release(INPUT_ARENA_SPDIF);
    }
    monitor = on;
    return true;
}
