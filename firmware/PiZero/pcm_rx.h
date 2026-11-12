#pragma once

// Bench measurement of the PCM block as a clock-master receiver (port plan section 6): how fast it can
// capture with DMA. Two 32-bit channels fill each 64-bit frame, so every bit clock is one captured bit.
// The PCM clock must already run (bench request 0x46 op 2); clock on GPIO 18, frame on 19, data on 20.

#include <stdint.h>

typedef struct {
    uint32_t seq;           // counts finished measurements
    uint32_t ms;            // requested window
    uint32_t bytes;         // bytes the DMA wrote in the window
    uint32_t elapsed_us;    // the window as measured
    uint32_t rx_errors;     // polls that found the RX FIFO overflow flag set
    uint32_t dma_debug;     // DMA DEBUG error flags at the end
    uint32_t dma_channel;
    uint32_t cs;            // PCM CS at the end of the window
    uint32_t words[64];     // the last 64 words captured, oldest first
} pcm_rx_result_t;

void pcm_rx_request(uint32_t ms);      // carried out by pcm_rx_poll(), which stalls the main loop for ms
void pcm_rx_poll(void);
const pcm_rx_result_t *pcm_rx_result(void);
