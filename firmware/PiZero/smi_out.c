// SMI output engine. Register offsets and fields from Linux include/linux/broadcom/bcm2835_smi.h; the start
// order (configure disabled, enable, clear FIFO, DMA, START) follows iosoft's SMI notes. DMA DREQ 4 is the
// SMI's. Bench findings that shape this file are in PiZero_Z0_bringup.md section 19.

#include "smi_out.h"

#include "irq.h"
#include "broadcom/caches.h"
#include "mbox.h"
#include "pll_out.h"

#define REG(a) (*(volatile uint32_t *)(a))

#define SMI_BASE 0x3F600000u
#define SMI_CS (SMI_BASE + 0x00u)
#define SMI_L (SMI_BASE + 0x04u)
#define SMI_A (SMI_BASE + 0x08u)
#define SMI_DSR0 (SMI_BASE + 0x10u)
#define SMI_DSW0 (SMI_BASE + 0x14u)
#define SMI_DMC (SMI_BASE + 0x30u)
#define SMI_D_BUS 0x7E60000Cu
#define CS_ENABLE (1u << 0)
#define CS_ACTIVE (1u << 2)
#define CS_START (1u << 3)
#define CS_CLEAR (1u << 4)
#define CS_WRITE (1u << 5)
#define CS_SETERR (1u << 13)
#define CS_PXLDAT (1u << 14)
#define CS_AFERR (1u << 25)

#define CM_SMICTL 0x3F1010B0u
#define CM_SMIDIV 0x3F1010B4u
#define CM_PASSWD (0x5Au << 24)
#define CM_SRC_PLLD 6u
#define CM_SRC_PLLA 4u
#define CM_ENAB (1u << 4)
#define CM_KILL (1u << 5)
#define CM_BUSY (1u << 7)
#define CM_MASH1 (1u << 9)
#define CM_GP0CTL 0x3F101070u
#define CM_GP0DIV 0x3F101074u
#define GPFSEL0 0x3F200000u

#define DMA_BASE 0x3F007000u
#define DMA_ENABLE 0x3F007FF0u
#define DMA_CS(ch) (DMA_BASE + 0x100u * (ch) + 0x00u)
#define DMA_CONBLK(ch) (DMA_BASE + 0x100u * (ch) + 0x04u)
#define DMA_SOURCE(ch) (DMA_BASE + 0x100u * (ch) + 0x0Cu)
#define DMA_DEBUG(ch) (DMA_BASE + 0x100u * (ch) + 0x20u)
#define DMA_CS_RUN ((1u << 28) | (8u << 20) | (8u << 16) | 1u)  // wait for writes, priorities, active
#define DMA_CS_INT (1u << 2)
#define DMA_CS_END (1u << 1)

#define BUS(p) ((uint32_t)(p) | 0xC0000000u)

// One write cycle is setup + strobe + hold SMI clocks, here 1 + 1 + 1.
#define CYCLE_CLOCKS 3u
#define CHUNK_FRAMES 16u            // one DMA control block, and one interrupt, per 16 frames
#define LEAD_FRAMES 64u             // the composer stays this far ahead of the DMA
#define MAX_CHUNKS 64u

typedef struct {
    uint32_t ti, src, dst, len, stride, next, pad[2];
} dma_cb_t;

static dma_cb_t cbs[MAX_CHUNKS] __attribute__((section(".nocache"), aligned(32)));

static smi_out_status_t st = {.dma_channel = 0xFF};
static uint16_t *ring;
static uint32_t ring_frames, ring_bus, ring_bytes;
static smi_compose_fn compose;
static uint32_t last_off_words;     // DMA offset in the ring at the last read, words
static uint32_t word_rem;           // words read past the last whole frame
static uint32_t last_us;
static uint32_t l_refresh_at;
static uint32_t source = SMI_SRC_PLLA;     // the preferred source; PLLD is the fallback (PLLA plan phase 6)
static uint32_t mck_mult, mck_ctl, mck_div;     // MCK: multiplier (0 = off) and what GPCLK0 was given

static uint32_t micros(void) {
    return REG(0x3F003004u);
}

static void wait_us(uint32_t us) {
    uint32_t t0 = micros();
    while (micros() - t0 < us) {
    }
}

static uint32_t pick_channel(void) {
    static const uint8_t prefer[] = {4, 6, 2};     // pcm_out.c prefers 5
    uint32_t mask = mbox_dma_channels();
    for (unsigned i = 0; i < sizeof(prefer); i++) {
        if (mask & (1u << prefer[i])) {
            return prefer[i];
        }
    }
    return 4;
}

// Stops a clock-manager clock at the end of its cycle, and kills it only if it will not stop.
static void cm_stop(uint32_t ctl_reg) {
    REG(ctl_reg) = CM_PASSWD | (REG(ctl_reg) & 0x60Fu);
    uint32_t t0 = micros();
    while ((REG(ctl_reg) & CM_BUSY) && micros() - t0 < 1000u) {
    }
    if (REG(ctl_reg) & CM_BUSY) {
        REG(ctl_reg) = CM_PASSWD | CM_KILL;
        while ((REG(ctl_reg) & CM_BUSY) && micros() - t0 < 2000u) {
        }
    }
}

// MCK on GPCLK0 (GPIO 4) runs from the SMI's source at the SMI divider times 768 / multiplier, so it is
// a fixed fraction of the SMI clock through every PLLA move, trim and fallback. On PLLA the divider is
// an integer; on PLLD it carries the SMI's fraction, dithered by MASH 1 the same way.
static void mck_apply(void) {
    uint32_t ctl = 0, div = 0;
    if (mck_mult && st.div_q8) {
        div = st.div_q8 * (SMI_FRAME_WORDS * CYCLE_CLOCKS / mck_mult) << 4;    // DIVI 23:12, DIVF 11:0
        ctl = (REG(CM_SMICTL) & 15u) | ((div & 0xFFFu) ? CM_MASH1 : 0u);
    }
    if (ctl == mck_ctl && div == mck_div) {
        return;
    }
    cm_stop(CM_GP0CTL);
    uint32_t fsel = REG(GPFSEL0) & ~(7u << 12);
    if (ctl) {
        REG(CM_GP0DIV) = CM_PASSWD | div;
        REG(CM_GP0CTL) = CM_PASSWD | ctl;
        REG(CM_GP0CTL) = CM_PASSWD | ctl | CM_ENAB;
        fsel |= 4u << 12;   // ALT0: GPCLK0
    }
    REG(GPFSEL0) = fsel;
    mck_ctl = ctl;
    mck_div = div;
}

// 256 fs words of CYCLE_CLOCKS clocks each. From PLLA the divider is an integer (PiZero_PLLA_plan.md).
// From PLLD the fraction is dithered by MASH 1, so the average is exact to the 1/256 step (about 70 ppm
// at worst here). A move between PLLA rows is live, so the clock never waits for a lock here.
static void clock_set(uint32_t freq) {
    uint32_t divi = 0;
    uint32_t row = source == SMI_SRC_PLLA ? pll_out_smi_row(freq, &divi) : PLL_ROW_COUNT;
    bool plla = row < PLL_ROW_COUNT && pll_out_select(row);
    uint32_t div = divi << 8, ctl = CM_SRC_PLLA;
    if (!plla) {
        uint64_t want = (uint64_t)freq * SMI_FRAME_WORDS * CYCLE_CLOCKS;
        div = (uint32_t)((((uint64_t)500000000u << 8) + want / 2u) / want);
        ctl = CM_SRC_PLLD | CM_MASH1;
    }
    // CM_SMI's MASH bit reads back set whatever is written; with DIVF 0 it never dithers.
    cm_stop(CM_SMICTL);
    REG(CM_SMIDIV) = CM_PASSWD | ((div >> 8) & 15u) << 12 | (div & 255u) << 4;     // DIVI 15:12, DIVF 11:4
    REG(CM_SMICTL) = CM_PASSWD | ctl;
    REG(CM_SMICTL) = CM_PASSWD | ctl | CM_ENAB;
    pll_out_set_in_use(plla);
    st.div_q8 = div;
    st.freq = freq;
    mck_apply();
}

// Brings st.frames up to the DMA position. Interrupts must be masked. Whole rings are inferred from the
// elapsed time, except right after a clock change, when the time since the last read says nothing.
static void track_words(bool infer_rings) {
    uint32_t now = micros();
    uint32_t off = (REG(DMA_SOURCE(st.dma_channel)) - ring_bus) / 2u;
    if (off > ring_bytes / 2u) {
        return;     // between control blocks
    }
    uint32_t n = ring_bytes / 2u;
    uint32_t d = (off - last_off_words) & (n - 1u);
    // Add whole rings so the advance is the one nearest what the elapsed time predicts.
    uint32_t expect = (uint32_t)((uint64_t)(now - last_us) * st.freq * SMI_FRAME_WORDS / 1000000u);
    if (infer_rings && expect > d + n / 2u) {
        d += ((expect - d + n / 2u) / n) * n;
    }
    last_off_words = off;
    last_us = now;
    uint32_t total = word_rem + d;
    st.frames += total / SMI_FRAME_WORDS;
    word_rem = total % SMI_FRAME_WORDS;
}

static void track(void) {
    track_words(true);
}

// Composes up to LEAD_FRAMES past the DMA, and keeps the SMI's length target out of reach: the transfer
// ends only when its word count equals L exactly, so L is set one word behind the count about once a second.
static void service(void) {
    track();
    uint32_t pos = st.frames;
    if (pos - l_refresh_at >= st.freq) {
        REG(SMI_L) = REG(SMI_L) - 1u;
        l_refresh_at = pos;
    }
    if ((int32_t)(st.composed - pos) < 2) {
        st.late++;
        st.composed = pos + 2u;     // the next couple of frames are already in the SMI FIFO or on the bus
    }
    uint32_t target = pos + LEAD_FRAMES;
    while ((int32_t)(target - st.composed) > 0) {
        uint32_t idx = st.composed & (ring_frames - 1u);
        uint32_t n = target - st.composed;
        if (n > ring_frames - idx) {
            n = ring_frames - idx;
        }
        uint32_t c0;
        __asm__ volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(c0));   // PMCCNTR
        compose(st.composed, &ring[idx * SMI_FRAME_WORDS], n);
        uint32_t cm;
        __asm__ volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(cm));
        data_clean(&ring[idx * SMI_FRAME_WORDS], n * SMI_FRAME_WORDS * 2u);  // out to SDRAM for the DMA
        uint32_t c1;
        __asm__ volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(c1));
        st.compose_cycles += cm - c0;
        st.clean_cycles += c1 - cm;
        st.composed += n;
    }
}

static void dma_irq(void) {
    REG(DMA_CS(st.dma_channel)) = DMA_CS_RUN | DMA_CS_INT;
    st.irqs++;
    service();
}

bool smi_out_start(uint16_t *r, uint32_t frames, uint32_t freq, uint32_t pin_mask, smi_compose_fn fn) {
    if (st.running || (frames & (frames - 1u)) || frames % CHUNK_FRAMES || frames / CHUNK_FRAMES > MAX_CHUNKS ||
        frames < 2u * LEAD_FRAMES) {
        return false;
    }
    if (st.dma_channel == 0xFF) {
        st.dma_channel = pick_channel();
    }
    uint32_t ch = st.dma_channel;
    ring = r;
    ring_frames = frames;
    ring_bus = BUS(r);
    ring_bytes = frames * SMI_FRAME_WORDS * 2u;
    compose = fn;

    REG(SMI_CS) = 0;
    uint32_t t0 = micros();
    while ((REG(SMI_CS) & CS_ACTIVE) && micros() - t0 < 1000u) {
    }
    REG(SMI_CS) = CS_SETERR | CS_AFERR;
    clock_set(freq);
    for (uint32_t g = 0; g < 28u; g++) {
        if (pin_mask & (1u << g)) {
            uint32_t reg = 0x3F200000u + 4u * (g / 10u), shift = 3u * (g % 10u);
            REG(reg) = (REG(reg) & ~(7u << shift)) | (5u << shift);     // ALT1: SMI
        }
    }
    REG(SMI_A) = 0;
    uint32_t dsw = 1u << 30 | 1u << 24 | 1u << 16 | 1u;    // 16-bit, setup 1, hold 1, strobe 1
    REG(SMI_DSW0) = dsw;
    REG(SMI_DSR0) = dsw;
    REG(SMI_DMC) = (1u << 28) | (0x20u << 18) | (0x20u << 12) | (1u << 6) | 0x3Fu;   // Linux's defaults
    REG(SMI_L) = 0xFFFFFFFFu;

    // Silence, then the first frames composed before anything is sent.
    for (uint32_t i = 0; i < frames * SMI_FRAME_WORDS; i++) {
        r[i] = 0;
    }
    data_clean(r, frames * SMI_FRAME_WORDS * 2u);
    st.frames = 0;
    st.composed = 0;
    word_rem = 0;
    last_off_words = 0;
    l_refresh_at = 0;
    compose(0, r, LEAD_FRAMES);
    data_clean(r, LEAD_FRAMES * SMI_FRAME_WORDS * 2u);
    st.composed = LEAD_FRAMES;

    uint32_t chunk_bytes = CHUNK_FRAMES * SMI_FRAME_WORDS * 2u;
    uint32_t n_cbs = frames / CHUNK_FRAMES;
    // 32-bit reads, DREQ 4, bursts of 4, an interrupt at the end of every block.
    uint32_t ti = 1u | (1u << 3) | (1u << 6) | (1u << 8) | (4u << 16) | (3u << 12);
    for (uint32_t i = 0; i < n_cbs; i++) {
        cbs[i] = (dma_cb_t){ti, BUS((uint8_t *)r + chunk_bytes * i), SMI_D_BUS, chunk_bytes, 0,
                            BUS(&cbs[(i + 1u) % n_cbs]), {0, 0}};
    }
    __asm__ volatile("dsb" ::: "memory");

    REG(SMI_CS) = CS_ENABLE | CS_WRITE | CS_PXLDAT;
    REG(SMI_CS) = CS_ENABLE | CS_WRITE | CS_PXLDAT | CS_CLEAR;
    irq_set_handler(16u + ch, dma_irq);
    irq_enable(16u + ch);
    REG(DMA_ENABLE) |= 1u << ch;
    REG(DMA_CS(ch)) = 1u << 31;
    wait_us(2);
    REG(DMA_DEBUG(ch)) = 7u;
    REG(DMA_CONBLK(ch)) = BUS(&cbs[0]);
    last_us = micros();
    st.running = 1;
    REG(DMA_CS(ch)) = DMA_CS_RUN | DMA_CS_END | DMA_CS_INT;
    wait_us(5);
    REG(SMI_CS) = CS_ENABLE | CS_WRITE | CS_PXLDAT | CS_START;
    return true;
}

void smi_out_set_rate(uint32_t freq) {
    uint32_t save = irq_global_disable();
    if (st.running) {
        track();
    }
    clock_set(freq);
    if (st.running) {
        track_words(false);
    }
    irq_global_restore(save);
}

void smi_out_set_source(uint32_t src) {
    uint32_t divi, row = pll_out_smi_row(st.freq, &divi);
    if (src == SMI_SRC_PLLA && row < PLL_ROW_COUNT) {
        pll_out_select(row);    // a first start waits for lock, so it is done with interrupts on
    }
    source = src;
    smi_out_set_rate(st.freq);
}

void smi_out_pin(uint32_t gpio, bool on) {
    uint32_t reg = 0x3F200000u + 4u * (gpio / 10u), shift = 3u * (gpio % 10u);
    uint32_t save = irq_global_disable();
    REG(reg) = (REG(reg) & ~(7u << shift)) | (on ? 5u << shift : 0u);   // ALT1: SMI, or an input
    irq_global_restore(save);
}

void smi_out_mck(uint32_t mult) {
    uint32_t save = irq_global_disable();
    mck_mult = mult == 128u || mult == 256u ? mult : 0u;
    mck_apply();
    irq_global_restore(save);
}

uint32_t smi_out_source(void) {
    return (REG(CM_SMICTL) & 15u) == CM_SRC_PLLA ? SMI_SRC_PLLA : SMI_SRC_PLLD;
}

uint32_t smi_out_frames(void) {
    uint32_t save = irq_global_disable();
    if (st.running) {
        track();
    }
    uint32_t f = st.frames;
    irq_global_restore(save);
    return f;
}

// For USB feedback: consumption in slot-0 I2S ring words (2 a frame), word-precise from the DMA position.
uint32_t smi_out_words2(void) {
    uint32_t save = irq_global_disable();
    if (st.running) {
        track();
    }
    uint32_t w = st.frames * 2u + word_rem / (SMI_FRAME_WORDS / 2u);
    irq_global_restore(save);
    return w;
}

uint32_t smi_out_composed(void) {
    return st.composed;     // one word, written only by the DMA interrupt
}

void smi_out_status(smi_out_status_t *out) {
    uint32_t save = irq_global_disable();
    if (st.running) {
        track();
    }
    st.cs = REG(SMI_CS);
    if (st.dma_channel != 0xFF) {
        st.dma_debug = REG(DMA_DEBUG(st.dma_channel));
    }
    *out = st;
    irq_global_restore(save);
}

uint32_t smi_out_probe_toggles(uint32_t gpio, uint32_t ms, uint32_t *samples) {
    uint32_t bit = 1u << (gpio & 31u), toggles = 0, n = 0;
    uint32_t last = REG(0x3F200034u) & bit;     // GPLEV0
    uint32_t t0 = micros();
    while (micros() - t0 < ms * 1000u) {
        uint32_t v = REG(0x3F200034u) & bit;
        toggles += v != last;
        last = v;
        n++;
    }
    *samples = n;
    return toggles;
}
