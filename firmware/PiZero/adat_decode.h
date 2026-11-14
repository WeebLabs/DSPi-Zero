#pragma once

// ADAT decoder over a line sampled into a power-of-two ring of 32-bit words, first sample in bit 31. It
// reads each cell at its centre, 32 cells a group: a cell is a 1 when its level differs from the one before
// it, as DSPi's adat_input.pio emits them. After each group the grid moves halfway to the group's last two
// edges. Bits are packed MSB first, 32 to a word. No hardware is touched here.

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t pos;           // samples before this are no longer needed (a word boundary)
    uint64_t centre_q8;     // the next cell's centre, in samples << 8
    uint32_t level;         // the last cell's level (2 before the first)
    uint32_t cell_q8;       // one cell in samples << 8
    uint32_t *out, *out_at; // the caller's word ring, and where each word's last cell is, in samples << 8
                            // (the low 32 bits, so stamps under 2^31 apart compare by subtraction)
    uint32_t out_mask;      // its size - 1 (a power of two)
    uint32_t out_n;         // words written since the reset (the ring index is out_n & out_mask)
    uint32_t far;           // groups in a row whose last two edges were over half a cell apart
    uint32_t flips;         // times the grid was moved half a cell, off the edges
} adat_decoder_t;

// Starts again at sample `from` (a word boundary), keeping the cell and the output ring.
void adat_decode_reset(adat_decoder_t *d, uint64_t from);
// The cell of a stream at fs Hz sampled at sample_hz.
void adat_decode_set_rate(adat_decoder_t *d, uint32_t sample_hz, uint32_t fs);
// Decodes what the ring holds up to sample `end` (exclusive). ring_words is a power of two, and the ring
// must still hold every sample from the decoder's position to end.
void adat_decode_ring(adat_decoder_t *d, const uint32_t *ring, uint32_t ring_words, uint64_t end);
