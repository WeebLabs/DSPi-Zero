#pragma once

// Bench view of the Pi's S/PDIF receiver (spdif_rx_pi.c), read with bench request 0x44.

#include <stdbool.h>
#include <stdint.h>

#define SPDIF_RX_PI_MAGIC 0x58524453u   // "SDRX"

typedef struct {
    uint32_t magic, running, state, dma_channel;
    uint32_t samples_lo, samples_hi;    // samples captured and decoded
    uint32_t subframes, decode_errors;  // from the decoder
    uint32_t cell_q8;                   // one cell in samples, 8 fractional bits; 0 while measuring
    uint32_t blocks, samp_freq, samp_freq_actual_mhz, parity_errors, fifo_used;
    uint32_t irqs, decode_us;           // chunk interrupts and the time spent in them
    uint32_t pcm_cs, dma_debug;
    uint32_t monitor;                   // 1 while running for the bench, not for DSPi
    uint32_t last[8];                   // the newest FIFO words, oldest first
    uint32_t inv_cycles;                // cycles invalidating chunks out of the cache
    uint32_t dec_cycles, dec_inst, dec_l1d_refill, dec_l2d_refill, dec_br_mispred;    // decoding
} spdif_rx_pi_status_t;

void spdif_rx_pi_status(spdif_rx_pi_status_t *out);
// Bench: runs the receiver with nothing reading it, while DSPi's input is not S/PDIF. Returns false if
// DSPi's S/PDIF input owns the receiver.
bool spdif_rx_pi_monitor(bool on);
