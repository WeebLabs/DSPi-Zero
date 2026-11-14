#pragma once

// All of DSPi's outputs on the SMI (secondary memory interface): a 16-bit parallel port that writes one
// word per cycle from a DMA loop, so every output pin changes on the same cycle. Each audio frame is 256
// words (256 fs). Register layout from Linux's bcm2835_smi.h; timing measured in bring-up section 19.

#include <stdbool.h>
#include <stdint.h>

#define SMI_FRAME_WORDS 256u

// Fills n frames starting at absolute frame `frame` into dst (n * SMI_FRAME_WORDS words), which is ring
// frame ring_idx on. It may leave a frame whose words would be what that ring frame already holds, and
// returns how many it wrote. Called from the DMA interrupt, a fixed lead ahead of the frame being sent.
typedef uint32_t (*smi_compose_fn)(uint32_t frame, uint16_t *dst, uint32_t ring_idx, uint32_t n);

typedef struct {
    uint32_t running;
    uint32_t dma_channel;
    uint32_t freq;
    uint32_t div_q8;        // SMI clock divider from 500 MHz PLLD, 8 fractional bits
    uint32_t frames;        // frames the DMA has read since the start
    uint32_t composed;      // frames composed (absolute)
    uint32_t late;          // times the composer fell behind the DMA
    uint32_t irqs;
    uint32_t cs;            // SMI CS
    uint32_t dma_debug;
    uint32_t compose_cycles;    // CPU cycles spent composing, wrapping (needs the PMU cycle counter on)
    uint32_t reused;            // composed frames whose ring frame already held them, so nothing was written
} smi_out_status_t;

// Starts streaming ring (ring_frames frames, a multiple of 16, in non-cacheable memory so the DMA reads
// what the composer wrote) on the GPIOs in pin_mask (SD0-SD15 are GPIO 8-23). Runs until stopped,
// composing as it goes.
bool smi_out_start(uint16_t *ring, uint32_t ring_frames, uint32_t freq, uint32_t pin_mask, smi_compose_fn compose);
void smi_out_set_rate(uint32_t freq);
void smi_out_reclock(void);     // sets the clock up again at the same rate, after PLLA's row choice changed

// Clock source. PLLA gives an integer divider (PiZero_PLLA_plan.md) at rates it covers; any other rate,
// or a PLLA that does not lock, falls back to PLLD with MASH. smi_out_source() reports the one in use.
enum { SMI_SRC_PLLD, SMI_SRC_PLLA };
void smi_out_set_source(uint32_t src);
uint32_t smi_out_source(void);
// MCK at mult (128 or 256) times the SMI's rate, locked to the SMI clock; 0 stops it. It comes out on
// GPIO 4 (GPCLK0) or, after smi_out_mck_pin(5), GPIO 5 (GPCLK1); any other pin is refused.
void smi_out_mck(uint32_t mult);
bool smi_out_mck_pin(uint32_t gpio);
// Hands a GPIO to the SMI (its SD bit in the stream) or returns it to an input, for pins used part-time.
void smi_out_pin(uint32_t gpio, bool on);
uint32_t smi_out_frames(void);      // frames the DMA has read; safe from any context
uint32_t smi_out_composed(void);    // frames composed so far, the reader position of the slot rings
uint32_t smi_out_ring_index(uint32_t frame);    // the ring frame holding a frame composed within the last ring
uint32_t smi_out_words2(void);      // frames the DMA has read, times 2, to 1/128 frame: I2S ring words
void smi_out_status(smi_out_status_t *out);

// Bench probe: samples GPIO gpio's level for ms milliseconds and returns the number of level changes.
uint32_t smi_out_probe_toggles(uint32_t gpio, uint32_t ms, uint32_t *samples);
