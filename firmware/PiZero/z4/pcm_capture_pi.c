// The PCM block's 125 MHz capture of GPIO 20, shared by the S/PDIF and ADAT receivers (pcm_capture_pi.h).
// Clock and frame master, 64-bit frames of two 32-bit channels, so every PCM clock is one sample. Only
// the data pin is routed; the clock and frame stay inside the block.

#include <string.h>

#include "pcm_capture_pi.h"

#include "irq.h"
#include "mbox.h"
#include "pll_out.h"
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
#define DMA_CS_WAIT_WRITES (1u << 6)
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
#define CHUNK_WORDS 4096u           // 131072 samples, about 1 ms
#define CHUNKS 8u
#define CHUNK_BYTES (CHUNK_WORDS * 4u)
#define RING_WORDS (CHUNKS * CHUNK_WORDS)
#define DIN_GPIO_SHIFT 0u           // GPIO 20 in GPFSEL2

typedef struct {
    uint32_t ti, src, dst, len, stride, next, pad[2];
} dma_cb_t;

static dma_cb_t cbs[CHUNKS] __attribute__((section(".nocache"), aligned(32)));
// The DMA fills RING_WORDS; the tail after them repeats the ring's start.
static uint32_t ring[RING_WORDS + PCM_CAPTURE_TAIL] __attribute__((aligned(64)));
_Static_assert((RING_WORDS & (RING_WORDS - 1u)) == 0, "the readers' ring is a power of two");

static uint32_t dma_ch = 0xFF;
static volatile bool running;
static pcm_capture_fn owner;
static uint32_t next_chunk;
static uint64_t cap_end;        // samples in completed chunks since the start
static uint64_t irq_us, irq_index;
static uint32_t irqs, inv_cycles;

static uint32_t micros(void) {
    return REG(0x3F003004u);
}

uint64_t pcm_capture_micros64(void) {
    uint32_t hi = REG(0x3F003008u), lo = REG(0x3F003004u);
    if (hi != REG(0x3F003008u)) {
        hi = REG(0x3F003008u);
        lo = REG(0x3F003004u);
    }
    return (uint64_t)hi << 32 | lo;
}

uint64_t pcm_capture_irq_us(void) {
    return irq_us;
}

uint64_t pcm_capture_time_of(uint64_t sample) {
    return irq_us - (irq_index - sample) / (PCM_CAPTURE_SAMPLE_HZ / 1000000u);
}

uint64_t pcm_capture_sample_at(uint64_t us) {
    uint32_t save = irq_global_disable();
    uint64_t s = irq_index + (us - irq_us) * (PCM_CAPTURE_SAMPLE_HZ / 1000000u);
    irq_global_restore(save);
    return s;
}

bool pcm_capture_running(void) {
    return running;
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
    irq_us = pcm_capture_micros64();
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
            memcpy(&ring[RING_WORDS], ring, PCM_CAPTURE_TAIL * 4u);
        }
        cap_end += CHUNK_WORDS * 32u;
        next_chunk = (next_chunk + 1u) % CHUNKS;
    }
    inv_cycles += pmu_cycles() - c0;
    irq_index = cap_end + (off % CHUNK_BYTES) / 4u * 32u;
    owner(ring, RING_WORDS, cap_end);
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

// Pauses the channel and lets its writes land before the reset, as Linux's bcm2835_dma_abort() does.
static void dma_abort(uint32_t ch) {
    REG(DMA_CS(ch)) = 0;
    uint32_t t0 = micros();
    while ((REG(DMA_CS(ch)) & DMA_CS_WAIT_WRITES) && micros() - t0 < 1000u) {
    }
    REG(DMA_CS(ch)) = DMA_CS_RESET;
}

static void capture_stop(void) {
    if (dma_ch == 0xFF) {
        return;
    }
    running = false;
    irq_disable(16u + dma_ch);
    REG(PCM_CS) = CS_EN | CS_STBY;
    dma_abort(dma_ch);
    REG(PCM_CS) = 0;
    pll_out_pcm_clock(SRC_PLLD, 0);
    REG(GPFSEL2) &= ~(7u << DIN_GPIO_SHIFT);
}

static void capture_start(pcm_capture_fn fn) {
    if (dma_ch == 0xFF) {
        dma_ch = pick_channel();
    }
    uint32_t ch = dma_ch;
    capture_stop();
    owner = fn;     // no interrupt reaches it until the capture runs again
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
    next_chunk = 0;
    cap_end = 0;
    irq_index = 0;
    irq_us = pcm_capture_micros64();
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

void pcm_capture_start(pcm_capture_fn fn) {
    capture_start(fn);
}

void pcm_capture_stop(pcm_capture_fn fn) {
    uint32_t save = irq_global_disable();
    if (owner == fn) {
        capture_stop();
    }
    irq_global_restore(save);
}

void pcm_capture_status(pcm_capture_status_t *out) {
    uint32_t save = irq_global_disable();
    *out = (pcm_capture_status_t){
        .running = running, .dma_channel = dma_ch, .irqs = irqs, .inv_cycles = inv_cycles, .pcm_cs = REG(PCM_CS),
        .dma_debug = dma_ch == 0xFF ? 0 : REG(DMA_DEBUG(dma_ch)) & 7u, .samples = cap_end,
    };
    irq_global_restore(save);
}
