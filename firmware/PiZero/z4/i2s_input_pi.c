// The Pi's I2S receiver (i2s_input_pi.h). One DMA control block that points back at itself copies GPLEV0
// into a 64 MB cached ring with no pacing, and a 500 us system timer interrupt drops the new lines from
// the cache and decodes them (i2s_rx_decode.h) into DSPi's rings. A second channel reads the block just
// before each reload, which keeps the reload short. Register layouts as in the BCM2835 datasheet.

#include <math.h>
#include <string.h>

#include "i2s_input_pi.h"

#include "i2s_rx_decode.h"
#include "irq.h"
#include "mbox.h"
#include "pcm_out.h"
#include "pll_out.h"
#include "pmu.h"

#include "hardware/dma.h"
#include "audio_input.h"
#include "config.h"
#include "input_capture_arena.h"
#include "usb_audio.h"

#include "broadcom/defines.h"
#include "broadcom/interrupts.h"

#define REG(a) (*(volatile uint32_t *)(a))

#define GPLEV0_BUS 0x7E200034u
#define GPFSEL(n) (0x3F200000u + 4u * (n))
#define DMA_BASE 0x3F007000u
#define DMA_ENABLE 0x3F007FF0u
#define DMA_CS(ch) (DMA_BASE + 0x100u * (ch) + 0x00u)
#define DMA_CONBLK(ch) (DMA_BASE + 0x100u * (ch) + 0x04u)
#define DMA_DEST(ch) (DMA_BASE + 0x100u * (ch) + 0x10u)
#define DMA_DEBUG(ch) (DMA_BASE + 0x100u * (ch) + 0x20u)
#define DMA_CS_RESET (1u << 31)
#define DMA_CS_WRITES_OUTSTANDING (1u << 6)
#define DMA_CS_WAIT_WRITES (1u << 28)
#define DMA_CS_RUN(pri) (((pri) << 20) | ((pri) << 16) | 1u)
#define DMA_TI_DEST_INC (1u << 4)
#define DMA_TI_DEST_WIDTH (1u << 5)
#define DMA_TI_SRC_INC (1u << 8)
#define DMA_TI_WAITS(n) ((uint32_t)(n) << 21)
// The capture writes four snapshots as one 128-bit burst. With DMA_TI's no-wide-bursts bit it writes them
// as two transfers, and stalls long enough to hide a BCK edge were then 3 to 50 times as frequent.
#define CAPTURE_TI (DMA_TI_DEST_INC | DMA_TI_DEST_WIDTH)
#define BUS(p) ((uint32_t)(p) | 0xC0000000u)

// The capture ring sits from 128 MB, clear of the image, the loader at 32 MB and the GPU's memory at
// the top; mmu_enable() maps it cached like the rest of SDRAM. At 16.5 or 30.9 M snapshots a second
// the DMA laps it every 1 or 0.5 s, and each lap's reload is one gap the decoder is told about.
#define CAP_BASE 0x08000000u
#define CAP_BYTES (64u << 20)
#define CAP_WORDS (CAP_BYTES / 4u)
#define LINE 64u
#define MARGIN 256u             // bytes behind the DMA's destination that may not have landed yet
#define TICK_US 500u            // short decode bursts disturb the capture least
#define WARM_US 2000u           // how long before a reload the control block is read, once a tick
#define CAL_WORDS (1u << 15)    // the calibration run (1-2 ms): snapshot rate and the external BCK
#define BLOCK (I2S_RX_SCAN_SAMPLES * 4u)   // bytes decoded between cache drops, whole lines and 2048 snapshots
#define PHASE_SAMPLES 2048u     // snapshots the clock follower looks at
#define PHASE_EVERY 4u          // on one tick in this many
#define RING_WORDS INPUT_ARENA_I2S_WORDS
#define DEF_SHIFT 12u           // the fallback pins: window GPIO 12-27, data 22-25, BCK 12 or 26
#define DEF_DATA 22u
#define DEF_OWN_BCK 12u
#define DEF_EXT_BCK 26u

typedef struct {
    uint32_t ti, src, dst, len, stride, next, pad[2];
} dma_cb_t;

// The capture reload reads its control block through the VideoCore's cache (the fast reload), which never
// sees ARM writes and may hold whatever was there before, even from an earlier image. refresh_cb() writes
// the block through that cache too. One block per DMA wait setting (0 or 8 cycles).
static dma_cb_t cbs[2] __attribute__((section(".nocache"), aligned(64)));
static dma_cb_t cal_cb __attribute__((section(".nocache"), aligned(32)));
static dma_cb_t warm_cb __attribute__((section(".nocache"), aligned(32)));
static uint32_t warm_to[8] __attribute__((section(".nocache"), aligned(32)));
static int32_t pcm_ring[4096 * 2] __attribute__((section(".nocache"), aligned(32)));

static uint32_t *const cap = (uint32_t *)CAP_BASE;
static i2s_rx_t dec;
static i2s_rx_pins_t pins;
static uint32_t own_bck;        // the SMI's BCK, LRCLK next to it, while the capture runs
static uint32_t period_q8;
static i2s_input_pi_bench_t bench;
static i2s_input_pi_status_t st = {
    .magic = I2S_INPUT_PI_MAGIC, .dma_channel = 0xFF, .warm_channel = 0xFF};
static volatile bool running, slip;
static dma_cb_t *cb;
static uint32_t done, last_tick_us, start_us, lap_us, warm_words, bad_seen, bad_bucket, checked_wr;
static uint32_t rate_hz, wrap_us, waits_index;
static uint64_t samples, dec_cycles;
static float sample_ns, sample_ns_at[2];

// The clock follower: what the tick measured, taken by the main loop. Both are in the SMI's own bit
// clocks, whose length the pipeline's rate gives, since the snapshot rate is only known to a few percent.
static volatile uint32_t phase_seq;
static volatile float phase_lag_bits, phase_ratio;
static uint32_t phase_taken, lock_good, lock_bad;
static float lock_integral;
static float trim_memory[3];    // the last locked trim at 44.1, 48 and 96 kHz

static uint32_t micros(void) {
    return REG(0x3F003004u);
}

static void spin_us(uint32_t us) {
    uint32_t t0 = micros();
    while (micros() - t0 < us) {
    }
}

// Stops a channel that may be mid-transfer, as Linux's bcm2835_dma_abort() does: pause it, let the
// writes already issued land, and only then reset, so that no reset meets a write in flight.
static void dma_abort(uint32_t ch) {
    REG(DMA_CS(ch)) = 0;
    uint32_t t0 = micros();
    while ((REG(DMA_CS(ch)) & DMA_CS_WRITES_OUTSTANDING) && micros() - t0 < 1000u) {
    }
    REG(DMA_CS(ch)) = DMA_CS_RESET;
}

static uint32_t rate_index(uint32_t hz) {
    return hz == 44100u ? 0u : hz == 96000u ? 2u : 1u;
}

static void invalidate(const uint32_t *p, uint32_t bytes) {
    for (uintptr_t a = (uintptr_t)p, end = a + bytes; a < end; a += LINE) {
        __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" ::"r"(a));     // DCIMVAC
    }
    __asm__ volatile("dsb" ::: "memory");
}

// Where pair p's next word goes, as its DMA write address.
static void publish(void) {
    for (uint32_t p = 0; p < I2S_RX_LINES; p++) {
        uint32_t at = (dec.wr + dec.lead[p]) & (RING_WORDS - 1u);
        pico_shim_dma_hw.ch[PICO_SPDIF_RX_DMA_CH0 + p].write_addr =
            (uint32_t)(uintptr_t)&input_capture_arena.i2s_ring[p][at];
    }
}

static void raise_slip(void) {
    slip = true;
    st.slips++;
}

// The bench's sine test: a sine of fixed frequency obeys x[n] + x[n-2] = 2 cos(w) x[n-1].
static void check_sine(void) {
    for (; (int32_t)(dec.wr - checked_wr) > 0; checked_wr += 2u) {
        if (checked_wr < 4096u) {
            continue;
        }
        for (uint32_t k = 0; k < I2S_RX_LINES; k++) {
            if (!(bench.check_lines & (1u << k))) {
                continue;
            }
            const uint32_t *r = input_capture_arena.i2s_ring[k];
            for (uint32_t c = 0; c < 2u; c++) {
                uint32_t at = checked_wr + dec.lead[k] + c;
                int32_t x0 = (int32_t)r[at & (RING_WORDS - 1u)] >> 8;
                int32_t x1 = (int32_t)r[(at - 2u) & (RING_WORDS - 1u)] >> 8;
                int32_t x2 = (int32_t)r[(at - 4u) & (RING_WORDS - 1u)] >> 8;
                int32_t e = x0 + x2 - (int32_t)(((int64_t)bench.sine_c_q30 * x1) >> 30);
                uint32_t a = (uint32_t)(e < 0 ? -e : e);
                st.sine_checked++;
                st.sine_worst = a > st.sine_worst ? a : st.sine_worst;
                st.sine_errors += a > bench.sine_tol;
            }
        }
    }
}

// How far the SMI's clocks lag the master's, from the same snapshots: BCK rises give the lag within one
// bit clock, as a circular mean over many edges, and LRCLK falls say which bit clock.
static void measure_phase(const uint32_t *s, uint32_t n) {
    uint32_t eb = 1u << pins.bck, el = 1u << pins.lr, ob = 1u << own_bck, ol = 1u << (own_bck + 1u);
    uint32_t hist[32] = {0}, last_e = 0, have_e = 0, rises = 0, first_e = 0, last_el = 0, have_el = 0, nl = 0;
    uint32_t first_o = 0, last_o = 0, own_rises = 0;
    int32_t dl[8];
    uint32_t p = s[0];
    for (uint32_t i = 1; i < n; i++) {
        uint32_t w = s[i], up = w & ~p, down = p & ~w;
        p = w;
        if (!((up | down) & (eb | el | ob | ol))) {
            continue;
        }
        if (up & eb) {
            first_e = have_e ? first_e : i;
            last_e = i;
            have_e = 1;
            rises++;
        }
        if (up & ob) {
            first_o = own_rises++ ? first_o : i;
            last_o = i;
            if (have_e && i - last_e < 32u) {
                hist[i - last_e]++;
            }
        }
        if (down & el) {
            last_el = i;
            have_el = 1;
        }
        if ((down & ol) && have_el && nl < 8u) {
            dl[nl++] = (int32_t)(i - last_el);
        }
    }
    if (rises < 16u || own_rises < 16u || nl == 0u) {
        return;
    }
    float period = (float)(last_e - first_e) / (float)(rises - 1u);    // snapshots a bit clock
    float own = (float)(last_o - first_o) / (float)(own_rises - 1u);
    float sx = 0, sy = 0;
    for (uint32_t d = 0; d < 32u; d++) {
        if (hist[d]) {
            float a = 6.2831853f * (float)d / period;
            sx += (float)hist[d] * cosf(a);
            sy += (float)hist[d] * sinf(a);
        }
    }
    float fine = atan2f(sy, sx) / 6.2831853f * period;      // within half a bit clock either way
    float frame = period * 64.0f, coarse = 0;
    for (uint32_t i = 0; i < nl; i++) {
        float v = (float)dl[i];
        coarse += v > frame / 2.0f ? v - frame : v;
    }
    coarse /= (float)nl;
    phase_lag_bits = (fine + period * roundf((coarse - fine) / period)) / own;
    phase_ratio = period / own;
    phase_seq++;
}

static uint32_t hist_lo, hist_hi;   // rise distances at or beyond these are capture stalls
static uint32_t dumps[I2S_INPUT_PI_DUMPS][I2S_INPUT_PI_DUMP_WORDS];

const uint32_t *i2s_input_pi_dump(uint32_t k) {
    return dumps[k % I2S_INPUT_PI_DUMPS];
}

// Bench: the distance between BCK rises over every snapshot, and where in the tick's run the stalls fell.
static void count_rises(const uint32_t *p, uint32_t n) {
    static uint32_t last, prev = 1;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t b = p[i] >> pins.bck & 1u;
        last++;
        if (b & ~prev) {
            st.hist[last > 10u ? 9u : last - 1u]++;
            if (last <= hist_lo || last >= hist_hi) {
                st.when[i * 16u / n]++;
            }
            last = 0;
        }
        prev = b;
    }
}

// Bench: keeps the snapshots around the rise that ended a bad half.
static void keep_dump(const uint32_t *end) {
    const uint32_t *e = dec.bad_edge, *lo = e - cap > 800 ? e - 800u : cap, *hi = e + 200u;
    dec.bad_edge = 0;
    hi = hi > end ? end : hi;
    if (st.dumps < I2S_INPUT_PI_DUMPS && hi > lo) {
        uint32_t *o = dumps[st.dumps++];
        o[0] = (uint32_t)(hi - lo);
        o[1] = (uint32_t)(e - lo);
        o[2] = dec.bad_n;
        o[3] = (uint32_t)(lo - cap);
        memcpy(&o[4], lo, (size_t)(hi - lo) * 4u);
    }
}

// Decodes the snapshots of [from, from + bytes) a block at a time and drops each block's lines from the
// cache again once it is decoded. Left in, the stream fills the L2 within 4 ms and pushes DSPi's dirty
// data out of it; the main loop's refills then write that data back all over SDRAM, and such scattered
// write-backs are what stalled the capture at 96 kHz.
static void decode_part(uint32_t from, uint32_t bytes) {
    const uint32_t *p = &cap[from / 4u];
    bool measure = st.role == 3u && st.ticks % PHASE_EVERY == 0u;
    for (uint32_t off = 0; off < bytes; off += BLOCK) {
        uint32_t len = bytes - off < BLOCK ? bytes - off : BLOCK, n = len / 4u;
        const uint32_t *q = p + off / 4u;
        invalidate(q, len);     // whatever the prefetcher or a look back brought in since the last lap
        uint32_t c0 = pmu_cycles();
        i2s_rx_decode(&dec, q, n);
        dec_cycles += pmu_cycles() - c0;
        if (bench.flags & I2S_INPUT_PI_HIST) {
            count_rises(q, n);
        }
        if (dec.bad_edge) {
            keep_dump(q + n);
        }
        if (dec.rescued_at) {
            st.rescued_when[dec.rescued_at > p ? (uint32_t)(dec.rescued_at - p) * 16u / (bytes / 4u) : 0u]++;
            dec.rescued_at = 0;
        }
        if (measure && off == 0u) {
            measure_phase(q, n < PHASE_SAMPLES ? n : PHASE_SAMPLES);
        }
        if (!(bench.flags & I2S_INPUT_PI_KEEP_LINES)) {
            invalidate(q, len);
        }
    }
    samples += bytes / 4u;
}

static void set_sample_ns(float ns) {
    sample_ns = sample_ns_at[waits_index] = ns;
    st.sample_ps = (uint32_t)(ns * 1000.0f);
    lap_us = (uint32_t)((float)CAP_WORDS * ns / 1000.0f);
    warm_words = (uint32_t)((float)WARM_US * 1000.0f / ns);
    period_q8 = (uint32_t)(1e9f / (64.0f * (float)rate_hz) / ns * 256.0f);
}

static void tick_irq(void) {
    SYSTMR->CS = 1u << 1;
    SYSTMR->C1 = SYSTMR->CLO + TICK_US;
    (void)SYSTMR->CS;
    if (!running) {
        return;
    }
    uint32_t c0 = pmu_cycles(), now = micros();
    uint32_t pos = REG(DMA_DEST(st.dma_channel)) - BUS(cap);
    st.ticks++;
    if (pos >= CAP_BYTES) {
        st.skipped++;       // between control blocks; the next tick catches up
        return;
    }
    if (st.warm_channel != 0xFF && (CAP_BYTES - pos) / 4u < warm_words) {
        dma_abort(st.warm_channel);
        REG(DMA_CONBLK(st.warm_channel)) = BUS(&warm_cb);
        REG(DMA_CS(st.warm_channel)) = DMA_CS_RUN(8u);
        st.warms++;
    }
    // A stall of half a lap means the DMA may have passed the decoder: carry on from here.
    uint32_t end = ((pos + CAP_BYTES - MARGIN) & (CAP_BYTES - 1u)) & ~(LINE - 1u);
    if (now - last_tick_us > lap_us / 2u) {
        st.overruns++;
        i2s_rx_flush(&dec);
        done = end;
    }
    last_tick_us = now;
    if (end < done) {
        decode_part(done, CAP_BYTES - done);
        st.wraps++;
        i2s_rx_gap(&dec, &cap[CAP_WORDS - 48u], 48u);
        done = 0;
        // The snapshot rate under load, from the lap just finished (a tick late at most, 0.05%).
        if (wrap_us && st.overruns == 0u) {
            set_sample_ns((float)(now - wrap_us) * 1000.0f / (float)CAP_WORDS);
            i2s_rx_set_period(&dec, period_q8);
        }
        wrap_us = now;
    }
    if (end > done) {
        decode_part(done, end - done);
        done = end;
    }
    publish();
    // External clock: a burst of bad halves is a framing slip (a lone one is a capture stall).
    if (st.role == 3u) {
        bad_bucket += (dec.bad_halves - bad_seen) * 8u;
        bad_seen = dec.bad_halves;
        bad_bucket -= bad_bucket ? 1u : 0u;
        if (bad_bucket > 24u) {
            bad_bucket = 0;
            raise_slip();
        }
    }
    if (bench.check_lines) {
        check_sine();
    }
    uint32_t c = pmu_cycles() - c0;
    st.tick_cycles_max = c > st.tick_cycles_max ? c : st.tick_cycles_max;
}

// A short run at the given wait cycles into the ring's start: the snapshot period in ns, and the rising
// edges of `pin` (0 for none) per second.
static float calibrate(uint32_t waits, uint32_t pin, uint32_t *rises_hz) {
    cal_cb = (dma_cb_t){CAPTURE_TI | DMA_TI_WAITS(waits), GPLEV0_BUS, BUS(cap), CAL_WORDS * 4u, 0, 0, {0, 0}};
    __asm__ volatile("dsb" ::: "memory");
    uint32_t ch = st.dma_channel;
    dma_abort(ch);
    spin_us(2);
    REG(DMA_CONBLK(ch)) = BUS(&cal_cb);
    uint32_t t0 = micros();
    REG(DMA_CS(ch)) = DMA_CS_RUN(15u) | DMA_CS_WAIT_WRITES;
    while ((REG(DMA_CS(ch)) & 1u) && micros() - t0 < 100000u) {
    }
    uint32_t t1 = micros();
    dma_abort(ch);
    float ns = (float)(t1 - t0) * 1000.0f / (float)CAL_WORDS;
    if (rises_hz) {
        invalidate(cap, CAL_WORDS * 4u);
        uint32_t n = 0, prev = cap[0] >> pin & 1u;
        for (uint32_t i = 1; i < CAL_WORDS; i++) {
            uint32_t b = cap[i] >> pin & 1u;
            n += b & ~prev;
            prev = b;
        }
        *rises_hz = (uint32_t)((float)n * 1e9f / (ns * (float)CAL_WORDS));
    }
    return ns;
}

// Copies the control block onto itself with the DMA, reading memory and writing through the VideoCore's
// cache (bus alias 0x0), so that the cached copy the reloads read is what the ARM wrote.
static void refresh_cb(void) {
    cal_cb = (dma_cb_t){DMA_TI_SRC_INC | DMA_TI_DEST_INC, BUS(cb), (uint32_t)cb, sizeof(*cb), 0, 0, {0, 0}};
    __asm__ volatile("dsb" ::: "memory");
    uint32_t ch = st.dma_channel;
    dma_abort(ch);
    spin_us(2);
    REG(DMA_CONBLK(ch)) = BUS(&cal_cb);
    uint32_t t0 = micros();
    REG(DMA_CS(ch)) = DMA_CS_RUN(15u) | DMA_CS_WAIT_WRITES;
    while ((REG(DMA_CS(ch)) & 1u) && micros() - t0 < 1000u) {
    }
    dma_abort(ch);
}

static uint32_t pick_channel(uint32_t mask, const uint8_t *prefer, uint32_t n, uint32_t not) {
    for (uint32_t i = 0; i < n; i++) {
        if ((mask & (1u << prefer[i])) && prefer[i] != not) {
            return prefer[i];
        }
    }
    return 0xFF;
}

static bool pins_fit(uint32_t shift, uint32_t pin) {
    return pin >= shift && pin < shift + 16u;
}

// A plain output back to an input; a pin an ALT function holds is left alone.
static void make_input(uint32_t gpio) {
    uint32_t shift = 3u * (gpio % 10u);
    uint32_t save = irq_global_disable();
    uint32_t v = REG(GPFSEL(gpio / 10u));
    if ((v >> shift & 7u) == 1u) {
        REG(GPFSEL(gpio / 10u)) = v & ~(7u << shift);
    }
    irq_global_restore(save);
}

// DSPi's pins (GPIO plan, phase 5), or the bench's: the data line of each pair in use, the outputs' BCK
// and, with an external master, DSPi's slave BCK. The window starts at the lowest pin the capture reads.
// A set that breaks the decoder's rules (i2s_rx_pins_ok()) falls back to the pins of the README.
static void choose_pins(bool ext) {
    uint32_t own = i2s_bck_pin, ext_bck = bench.ext_bck ? bench.ext_bck : i2s_bck_pin_slave;
    uint32_t bck = ext ? ext_bck : own, lo = bck;
    i2s_rx_pins_t p = {0, (uint8_t)bck, (uint8_t)(bck + 1u), {0}};
    for (uint32_t k = 0; k < I2S_RX_LINES; k++) {
        uint32_t d = bench.data ? bench.data + k : i2s_rx_pin[k];
        p.data[k] = (uint8_t)(k < st.pairs ? d : p.data[0]);     // a pair not in use repeats line 0
        lo = p.data[k] < lo ? p.data[k] : lo;
    }
    lo = ext && own < lo ? own : lo;
    p.shift = (uint8_t)(bench.shift ? bench.shift : lo);
    if (!i2s_rx_pins_ok(&p) || p.lr > 27u || (ext && (!pins_fit(p.shift, own) || !pins_fit(p.shift, own + 1u)))) {
        own = DEF_OWN_BCK;
        bck = ext ? DEF_EXT_BCK : DEF_OWN_BCK;
        p = (i2s_rx_pins_t){DEF_SHIFT, (uint8_t)bck, (uint8_t)(bck + 1u),
                            {DEF_DATA, DEF_DATA + 1u, DEF_DATA + 2u, DEF_DATA + 3u}};
    }
    pins = p;
    own_bck = own;
}

static void start(void) {
    static uint32_t arm_size;
    for (uint32_t i = 0, base = 0; i < 4u && arm_size == 0u; i++) {
        mbox_arm_memory(&base, &arm_size);
    }
    if (st.dma_channel == 0xFF) {
        static const uint8_t cap_prefer[] = {6, 2, 3}, warm_prefer[] = {2, 3};
        uint32_t mask = 0;
        for (uint32_t i = 0; i < 4u && mask == 0u; i++) {
            mask = mbox_dma_channels();     // the first call after a start has come back empty
        }
        st.dma_channel = pick_channel(mask, cap_prefer, sizeof(cap_prefer), 0xFF);
        st.warm_channel = pick_channel(mask, warm_prefer, sizeof(warm_prefer), st.dma_channel);
    }
    if (arm_size < CAP_BASE + CAP_BYTES || st.dma_channel == 0xFF) {
        return;     // stays silent, as a receiver with nothing on its pins
    }

    // DSPi's role, as i2s_input_start() and main.c elect it.
    bool ext = i2s_clock_mode == I2S_CLOCK_MODE_SLAVE, any_out = false;
    for (int i = 0; i < NUM_SPDIF_INSTANCES; i++) {
        any_out |= output_types[i] == OUTPUT_TYPE_I2S;
    }
    st.role = ext ? 3u : any_out ? 2u : 1u;
    uint32_t pairs = i2s_input_channels / 2u;
    st.pairs = pairs < 1u ? 1u : pairs > I2S_RX_LINES ? I2S_RX_LINES : pairs;

    choose_pins(ext);
    uint32_t bck = pins.bck;
    // The pins read as inputs; the bench sets up its own.
    for (uint32_t k = 0; k < I2S_RX_LINES && !bench.data; k++) {
        make_input(pins.data[k]);
    }
    if (ext && !bench.ext_bck) {
        make_input(pins.bck);
        make_input(pins.lr);
    }

    // At 48 kHz and below 8 wait cycles halve the snapshots and the decoding. An external master's
    // rate is measured first: until DSPi has followed it, the pipeline's rate says nothing.
    uint32_t rate = audio_state.freq, waits = rate > 48000u ? 0u : 8u;
    REG(DMA_ENABLE) |= 1u << st.dma_channel;
    st.ext_bck_hz = 0;
    if (ext) {
        float ns = calibrate(0, bck, &st.ext_bck_hz);
        sample_ns_at[0] = sample_ns_at[0] == 0.0f ? ns : sample_ns_at[0];
        // To the nearest rate DSPi takes, as its own detection snaps it; with no clock, the fast setting.
        uint32_t f = st.ext_bck_hz / 64u;
        if (f > 20000u) {
            rate = f < 46000u ? 44100u : f < 72000u ? 48000u : 96000u;
        }
        waits = f > 20000u && rate <= 48000u ? 8u : 0u;
    }
    // The calibration run's rate is a few percent fast; the first lap under load corrects it.
    uint32_t wi = waits ? 1u : 0u;
    if (sample_ns_at[wi] == 0.0f) {
        sample_ns_at[wi] = calibrate(waits, 0, 0);
    }
    waits_index = wi;
    rate_hz = rate;
    st.waits = waits;
    set_sample_ns(sample_ns_at[wi]);
    hist_lo = (period_q8 >> 8) - 2u;
    hist_hi = (period_q8 >> 8) + 3u;

    uint32_t *rings[I2S_RX_LINES];
    for (uint32_t p = 0; p < I2S_RX_LINES; p++) {
        rings[p] = input_capture_arena.i2s_ring[p];
    }
    i2s_rx_init(&dec, &pins, rings, RING_WORDS, period_q8);
    // DSPi's master role reads pair 0 one frame on (its PIO pair 0 starts a frame early); so it is written.
    dec.lead[0] = st.role == 1u && st.pairs > 1u ? 2u : 0u;
    publish();

    cb = &cbs[wi];
    *cb = (dma_cb_t){CAPTURE_TI | DMA_TI_WAITS(waits), GPLEV0_BUS, BUS(cap), CAP_BYTES, 0, (uint32_t)cb, {0, 0}};
    refresh_cb();
    if (st.warm_channel != 0xFF) {
        warm_cb = (dma_cb_t){DMA_TI_SRC_INC | DMA_TI_DEST_INC, (uint32_t)cb, BUS(warm_to), 32, 0, 0, {0, 0}};
        REG(DMA_ENABLE) |= 1u << st.warm_channel;
    }
    __asm__ volatile("dsb" ::: "memory");

    done = checked_wr = bad_seen = bad_bucket = wrap_us = 0;
    samples = dec_cycles = 0;
    slip = false;
    lock_good = lock_bad = 0;
    phase_taken = phase_seq;
    st.lock_state = 0;
    st.trim_min_ppb = st.phase_min_x100 = 0x7FFFFFFF;
    st.trim_max_ppb = st.phase_max_x100 = (int32_t)0x80000000;
    if (ext) {
        // From the trim this rate last locked at: the same master is then found at once.
        lock_integral = trim_memory[rate_index(rate)];
        pll_out_trim((int32_t)lrintf(lock_integral));
    }
    uint32_t ch = st.dma_channel;
    dma_abort(ch);
    spin_us(2);
    REG(DMA_DEBUG(ch)) = 7u;
    REG(DMA_CONBLK(ch)) = (uint32_t)cb;     // through the VideoCore's L1 and L2, no wait for writes
    irq_set_handler(TIMER_1_IRQn, tick_irq);
    start_us = last_tick_us = micros();
    running = true;
    st.running = 1;
    st.starts++;
    REG(DMA_CS(ch)) = DMA_CS_RUN(15u);
    SYSTMR->CS = 1u << 1;
    SYSTMR->C1 = SYSTMR->CLO + TICK_US;
    irq_enable(TIMER_1_IRQn);
}

static void stop(void) {
    if (!running) {
        return;
    }
    irq_disable(TIMER_1_IRQn);
    running = false;
    st.running = 0;
    dma_abort(st.dma_channel);
    if (st.warm_channel != 0xFF) {
        dma_abort(st.warm_channel);
    }
    st.elapsed_us = micros() - start_us;
    if (st.role == 3u) {
        pll_out_trim(0);
        st.lock_state = 0;
    }
    slip = false;
}

void i2s_input_pi_enable(void) {
    if (!running) {
        start();
        return;
    }
    // i2s_input_resync(): DSPi has anchored its read indices at the write addresses, so the decoder
    // frames afresh from the next LRCLK edge and goes on writing whole frames from there.
    uint32_t save = irq_global_disable();
    uint32_t wr = dec.wr, lead[I2S_RX_LINES];
    memcpy(lead, dec.lead, sizeof(lead));
    uint32_t *rings[I2S_RX_LINES];
    memcpy(rings, dec.ring, sizeof(rings));
    i2s_rx_init(&dec, &pins, rings, RING_WORDS, period_q8);
    dec.wr = wr;
    memcpy(dec.lead, lead, sizeof(lead));
    bad_seen = 0;
    st.reframes++;
    irq_global_restore(save);
}

void i2s_input_pi_release(void) {
    stop();
}

bool i2s_input_pi_slipped(void) {
    return slip;
}

void i2s_input_pi_clear_slip(void) {
    slip = false;
}

bool i2s_input_pi_running(void) {
    return running;
}

bool i2s_input_pi_owns_pll(void) {
    return running && st.role == 3u;
}

bool i2s_input_pi_follower_locked(void) {
    return running && st.role == 3u && st.lock_state == 2u;
}

// The clock follower: a proportional and integral loop from the measured lag to the PLLA trim. A lag of
// 1 ns moves the trim 100 ppb, and the trim is held within 250 ppm while a large lag is slewed off.
static void follow(void) {
    uint32_t save = irq_global_disable();      // one measurement, whole
    uint32_t seq = phase_seq;
    float ratio = phase_ratio, lag = phase_lag_bits;
    irq_global_restore(save);
    if (seq == phase_taken) {
        return;
    }
    float dt = (float)(seq - phase_taken) * (float)(TICK_US * PHASE_EVERY) * 1e-6f;
    phase_taken = seq;
    uint32_t fs = audio_state.freq;
    lag *= fs ? 1e9f / (64.0f * (float)fs) : 0.0f;
    st.bck_ratio_ppm = (int32_t)lrintf((ratio - 1.0f) * 1e6f);
    // A master at another rate (they are 8.8% apart) waits for DSPi's rate change, which restarts the input.
    if (!(ratio > 0.98f && ratio < 1.02f)) {
        st.rate_mismatch++;
        lock_good = 0;
        st.lock_state = 0;
        return;
    }
    const float kp = 100.0f, ki = 2500.0f, limit = 250000.0f;
    float trim = lock_integral + kp * lag;
    if (trim > limit || trim < -limit) {
        trim = trim > 0 ? limit : -limit;
    } else {
        lock_integral += ki * lag * dt;
    }
    pll_out_trim((int32_t)lrintf(trim));
    bool near = lag > -20.0f && lag < 20.0f;
    lock_good = near ? lock_good + 1u : 0u;
    if (st.lock_state == 2u) {
        lock_bad = near ? 0u : lock_bad + 1u;
        if (lock_bad >= 5u) {
            st.unlocks++;
            st.lock_state = 1;
            raise_slip();       // the master's clocks jumped: DSPi restarts the input and the outputs
        }
    } else {
        st.lock_state = lock_good >= 200u ? 2u : 1u;
        if (st.lock_state == 2u) {
            st.locked_us = micros() - start_us;
            trim_memory[rate_index(fs)] = lock_integral;
            lock_bad = 0;
        }
    }
    st.lock_updates++;
    st.trim_ppb = (int32_t)lrintf(trim);
    st.phase_ns_x100 = (int32_t)lrintf(lag * 100.0f);
    if (st.lock_state == 2u) {
        st.trim_min_ppb = st.trim_ppb < st.trim_min_ppb ? st.trim_ppb : st.trim_min_ppb;
        st.trim_max_ppb = st.trim_ppb > st.trim_max_ppb ? st.trim_ppb : st.trim_max_ppb;
        st.phase_min_x100 = st.phase_ns_x100 < st.phase_min_x100 ? st.phase_ns_x100 : st.phase_min_x100;
        st.phase_max_x100 = st.phase_ns_x100 > st.phase_max_x100 ? st.phase_ns_x100 : st.phase_max_x100;
    }
}

void i2s_input_pi_poll(void) {
    if (running && st.role == 3u) {
        follow();
    }
}

// The bench's stand-in master: a sine of 85 cycles in a 4096-frame ring, 24 bits, both channels.
static void pcm_master(bool on, uint32_t hz) {
    if (on == (st.pcm_master != 0u)) {
        return;
    }
    if (on) {
        for (uint32_t i = 0; i < 4096u; i++) {
            int32_t v = (int32_t)lrintf(2000000.0f * sinf(6.2831853f * 85.0f * (float)i / 4096.0f));
            pcm_ring[2u * i] = pcm_ring[2u * i + 1u] = (int32_t)((uint32_t)v << 8);
        }
        __asm__ volatile("dsb" ::: "memory");
        uint32_t save = irq_global_disable();
        pcm_out_start(pcm_ring, 4096, 0, hz);
        irq_global_restore(save);
    } else {
        pcm_out_stop();
        REG(GPFSEL(1)) &= ~((7u << 24) | (7u << 27));     // GPIO 18 and 19 back to inputs
        REG(GPFSEL(2)) &= ~(7u << 3);                    // GPIO 21
    }
    st.pcm_master = on;
}

void i2s_input_pi_bench(const i2s_input_pi_bench_t *b) {
    i2s_input_pi_bench_t d = {0};
    uint32_t save = irq_global_disable();      // the tick reads these
    bench = b ? *b : d;
    irq_global_restore(save);
    pcm_master((bench.flags & I2S_INPUT_PI_PCM_MASTER) != 0u, bench.pcm_hz ? bench.pcm_hz : 48000u);
    save = irq_global_disable();
    st.sine_checked = st.sine_errors = st.sine_worst = st.dumps = 0;
    memset(st.hist, 0, sizeof(st.hist));
    memset(st.when, 0, sizeof(st.when));
    memset(st.rescued_when, 0, sizeof(st.rescued_when));
    checked_wr = dec.wr;
    irq_global_restore(save);
}

void i2s_input_pi_status(i2s_input_pi_status_t *out) {
    uint32_t save = irq_global_disable();
    st.frames = dec.frames;
    st.bad_halves = dec.bad_halves;
    st.concealed = dec.concealed;
    st.repaired = dec.repaired;
    st.gaps = dec.gaps;
    st.rescued = dec.rescued;
    st.wr = dec.wr;
    memcpy(st.rescue_fail, dec.rescue_fail, sizeof(st.rescue_fail));
    st.bad_short = dec.bad_short;
    st.bad_one_short = dec.bad_one_short;
    st.bad_one_long = dec.bad_one_long;
    st.bad_long = dec.bad_long;
    st.bad_at_gap = dec.bad_at_gap;
    st.rescued_by_step = dec.rescued_by_step;
    memcpy(st.rescued_dist, dec.rescued_dist, sizeof(st.rescued_dist));
    st.samples_lo = (uint32_t)samples;
    st.samples_hi = (uint32_t)(samples >> 32);
    st.dec_cycles_lo = (uint32_t)dec_cycles;
    st.dec_cycles_hi = (uint32_t)(dec_cycles >> 32);
    if (running) {
        st.elapsed_us = micros() - start_us;
    }
    if (st.dma_channel != 0xFF) {
        st.dma_cs = REG(DMA_CS(st.dma_channel));
        st.dma_conblk = REG(DMA_CONBLK(st.dma_channel));
        st.dma_dest = REG(DMA_DEST(st.dma_channel));
        st.dma_debug = REG(DMA_DEBUG(st.dma_channel));
    }
    *out = st;
    irq_global_restore(save);
}
