// PCM block as a clock-master receiver, for the bench. Register layouts from the BCM2835 ARM Peripherals
// datasheet (PCM chapter 8, DMA chapter 4), as in pcm_out.c. The DMA loops over a ring in the uncached
// region, and the window is timed by polling its write address.

#include "pcm_rx.h"

#include "mbox.h"

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

#define GPFSEL1 0x3F200004u
#define GPFSEL2 0x3F200008u

#define DMA_BASE 0x3F007000u
#define DMA_ENABLE 0x3F007FF0u
#define DMA_CS(ch) (DMA_BASE + 0x100u * (ch) + 0x00u)
#define DMA_CONBLK(ch) (DMA_BASE + 0x100u * (ch) + 0x04u)
#define DMA_DEST(ch) (DMA_BASE + 0x100u * (ch) + 0x10u)
#define DMA_DEBUG(ch) (DMA_BASE + 0x100u * (ch) + 0x20u)
#define DMA_CS_RESET (1u << 31)
#define DMA_CS_RUN ((1u << 28) | (8u << 20) | (8u << 16) | 1u)  // wait for writes, priorities, active
#define DMA_TI_WAIT_RESP (1u << 3)
#define DMA_TI_DEST_INC (1u << 4)
#define DMA_TI_SRC_DREQ (1u << 10)
#define DMA_TI_PERMAP_PCM_RX (3u << 16)
#define DMA_TI_NO_WIDE_BURSTS (1u << 26)

#define BUS(p) ((uint32_t)(p) | 0xC0000000u)

#define CBS 4u
#define RING_BYTES (256u * 1024u)

typedef struct {
    uint32_t ti, src, dst, len, stride, next, pad[2];
} dma_cb_t;

static dma_cb_t cbs[CBS] __attribute__((section(".nocache"), aligned(32)));
static uint32_t ring[RING_BYTES / 4u] __attribute__((section(".nocache"), aligned(32)));
static pcm_rx_result_t res = {.dma_channel = 0xFF};
static volatile uint32_t request_ms;

static uint32_t micros(void) {
    return REG(0x3F003004u);
}

static void set_fsel(uint32_t reg, uint32_t shift, uint32_t fn) {
    REG(reg) = (REG(reg) & ~(7u << shift)) | (fn << shift);
}

static uint32_t pick_channel(void) {
    static const uint8_t prefer[] = {5, 6, 2};     // smi_out.c takes 4
    uint32_t mask = mbox_dma_channels();
    for (unsigned i = 0; i < sizeof(prefer); i++) {
        if (mask & (1u << prefer[i])) {
            return prefer[i];
        }
    }
    return 5;
}

static void measure(uint32_t ms) {
    if (res.dma_channel == 0xFF) {
        res.dma_channel = pick_channel();
    }
    uint32_t ch = res.dma_channel;
    set_fsel(GPFSEL1, 24, 4);      // GPIO 18 PCM_CLK, ALT0
    set_fsel(GPFSEL1, 27, 4);      // GPIO 19 PCM_FS
    set_fsel(GPFSEL2, 0, 4);       // GPIO 20 PCM_DIN

    // Clock and frame master, 64-bit frames: channel 1 takes bits 0-31 and channel 2 bits 32-63.
    REG(PCM_CS) = 0;
    REG(PCM_CS) = CS_EN | CS_STBY;
    REG(PCM_MODE) = (63u << 10) | 32u;
    REG(PCM_RXC) = (1u << 31) | (1u << 30) | (0u << 20) | (8u << 16) | (1u << 15) | (1u << 14) | (32u << 4) | 8u;
    REG(PCM_TXC) = 0;
    REG(PCM_DREQ) = (0x10u << 24) | (0x30u << 16) | (0x30u << 8) | 0x20u;    // RX: DREQ at 32 words, panic 48
    REG(PCM_INTEN) = 0;
    REG(PCM_CS) = CS_EN | CS_STBY | CS_RXCLR;
    REG(PCM_CS) = CS_EN | CS_STBY | CS_SYNC;
    uint32_t t0 = micros();
    while (!(REG(PCM_CS) & CS_SYNC) && micros() - t0 < 100u) {
    }
    REG(PCM_CS) = CS_EN | CS_STBY | CS_RXERR;

    uint32_t ti = DMA_TI_WAIT_RESP | DMA_TI_DEST_INC | DMA_TI_SRC_DREQ | DMA_TI_PERMAP_PCM_RX | DMA_TI_NO_WIDE_BURSTS;
    uint32_t part = RING_BYTES / CBS;
    for (uint32_t i = 0; i < CBS; i++) {
        cbs[i] = (dma_cb_t){ti, PCM_FIFO_BUS, BUS((uint8_t *)ring + part * i), part, 0, BUS(&cbs[(i + 1u) % CBS]),
                            {0, 0}};
    }
    __asm__ volatile("dsb" ::: "memory");
    REG(DMA_ENABLE) |= 1u << ch;
    REG(DMA_CS(ch)) = DMA_CS_RESET;
    t0 = micros();
    while (micros() - t0 < 2u) {
    }
    REG(DMA_DEBUG(ch)) = 7u;
    REG(DMA_CONBLK(ch)) = BUS(&cbs[0]);
    REG(DMA_CS(ch)) = DMA_CS_RUN;
    uint32_t run = CS_EN | CS_STBY | CS_DMAEN | CS_RXON;
    REG(PCM_CS) = run;

    // Let it settle for a millisecond, then count what the DMA writes. The ring wraps every few ms at
    // the fastest clocks, so the write address is polled continuously.
    t0 = micros();
    while (micros() - t0 < 1000u) {
    }
    uint32_t ring_bus = BUS(ring), errors = 0, bytes = 0;
    uint32_t last = REG(DMA_DEST(ch)) - ring_bus;
    uint32_t start = micros(), now = start;
    while (now - start < ms * 1000u) {
        uint32_t off = REG(DMA_DEST(ch)) - ring_bus;
        if (off <= RING_BYTES) {
            bytes += (off - last + RING_BYTES) % RING_BYTES;
            last = off;
        }
        if (REG(PCM_CS) & CS_RXERR) {
            errors++;
            REG(PCM_CS) = run | CS_RXERR;
        }
        now = micros();
    }
    res.cs = REG(PCM_CS);
    REG(PCM_CS) = CS_EN | CS_STBY;
    res.dma_debug = REG(DMA_DEBUG(ch)) & 7u;
    REG(DMA_CS(ch)) = DMA_CS_RESET;
    REG(PCM_CS) = 0;
    set_fsel(GPFSEL1, 24, 0);
    set_fsel(GPFSEL1, 27, 0);
    set_fsel(GPFSEL2, 0, 0);

    uint32_t end_word = (last % RING_BYTES) / 4u, n = RING_BYTES / 4u;
    for (uint32_t i = 0; i < 64u; i++) {
        res.words[i] = ring[(end_word + n - 64u + i) % n];
    }
    res.ms = ms;
    res.bytes = bytes;
    res.elapsed_us = now - start;
    res.rx_errors = errors;
    res.seq++;
}

void pcm_rx_request(uint32_t ms) {
    request_ms = ms > 1000u ? 1000u : ms;
}

void pcm_rx_poll(void) {
    uint32_t ms = request_ms;
    if (ms) {
        request_ms = 0;
        measure(ms);
    }
}

const pcm_rx_result_t *pcm_rx_result(void) {
    return &res;
}
