#pragma once

// The Pi's ADAT receiver under DSPi's adat_input.c, which is compiled unchanged. On the RP2350 a PIO
// program writes the decoded bit stream (1 for a level change, MSB first) into the arena's ADAT ring
// through DMA channel 15. Here the PCM capture of GPIO 20 (pcm_capture_pi.c) is decoded by adat_decode.c
// into the same ring, and channel 15's write address says how far it is filled.

#include <stdbool.h>
#include <stdint.h>

// From the shim, for DSPi's receiver on pio1 SM 2: enabling it starts the capture and disabling it stops
// it, and `set y` (pio_sm_exec) is the cell, which sets the rate the line is read at.
void adat_input_pi_enable(bool on);
void adat_input_pi_set_y(uint32_t y);
bool adat_input_pi_running(void);

// adat_input.c's dma_hw is adat_input_pi_dma_hw() (hardware/dma.h). Each read first publishes the words
// completed a fixed delay before now, so the write address moves with the wire as the RP2350's DMA does
// and DSPi's slave-mode rate measurement sees the wire rate, not the capture's chunks.

// Bench (request 0x52). Channel k in `check` must carry a sine obeying x[n] + x[n-2] = c x[n-1].
#define ADAT_INPUT_PI_MAGIC 0x50494441u     // "ADIP"
typedef struct {
    uint8_t check;
    uint8_t pad[3];
    int32_t sine_c_q30;                     // 2 cos(w)
    uint32_t sine_tol;                      // largest residual that passes
} adat_input_pi_bench_t;

typedef struct {
    uint32_t magic, running, rate;          // rate: what the cell reads, 44100 or 48000
    uint32_t chunks, samples_lo, samples_hi;
    uint32_t words, published, overflows, flips;     // flips: adat_decoder_t's
    uint32_t dec_cycles_lo, dec_cycles_hi, chunk_cycles_max;
    int32_t headroom_min;                   // decoded samples ahead of what is published, least seen
    // The bench's own frame reader over the published words.
    uint32_t frames, header_errors, syncs;
    uint32_t sine_checked, sine_errors, sine_worst;
    int32_t peak[8];                        // largest sample magnitude per channel since the bench request
    uint32_t dec_inst, dec_l1d_refill, dec_l2d_refill, dec_br_mispred;  // decoding, from the PMU
} adat_input_pi_status_t;

void adat_input_pi_bench(const adat_input_pi_bench_t *b);   // starts the frame reader afresh; NULL stops it
void adat_input_pi_status(adat_input_pi_status_t *out);
