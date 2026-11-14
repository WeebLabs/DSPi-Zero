#pragma once

// The PCM block sampling GPIO 20 at 125 MHz from PLLD, which input tracking never moves, for the S/PDIF
// and ADAT receivers (spdif_rx_pi.c, adat_input_pi.c). DSPi runs one input source at a time, so one
// receiver has the capture at a time. A DMA loop fills a ring a chunk at a time, and each chunk's
// interrupt hands the ring to that receiver, in interrupt context.

#include <stdbool.h>
#include <stdint.h>

#define PCM_CAPTURE_SAMPLE_HZ 125000000u
#define PCM_CAPTURE_TAIL 96u        // words after the ring that repeat its start, for readers that look ahead

// ring holds ring_words (a power of two) words of samples, first sample in bit 31, then the tail. Every
// sample up to `end` (a word boundary, counted from the start) is in it, and nothing older than about
// six chunks before end is still there.
typedef void (*pcm_capture_fn)(const uint32_t *ring, uint32_t ring_words, uint64_t end);

// Starts the capture for fn, taking it from whichever receiver had it.
void pcm_capture_start(pcm_capture_fn fn);
// Stops it, if fn has it.
void pcm_capture_stop(pcm_capture_fn fn);
bool pcm_capture_running(void);

// The last interrupt's time (system timer) and the sample the DMA had reached then.
uint64_t pcm_capture_irq_us(void);
// The time of a sample, or the sample at a time, from the last interrupt.
uint64_t pcm_capture_time_of(uint64_t sample);
uint64_t pcm_capture_sample_at(uint64_t us);
uint64_t pcm_capture_micros64(void);

typedef struct {
    uint32_t running, dma_channel, irqs, inv_cycles, pcm_cs, dma_debug;
    uint64_t samples;
} pcm_capture_status_t;
void pcm_capture_status(pcm_capture_status_t *out);
