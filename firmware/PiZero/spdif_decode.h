#pragma once

// S/PDIF decoder over a line sampled into a power-of-two ring of 32-bit words, first sample in bit 31.
// It finds the stream by timing level changes until two preambles a subframe apart measure the cell,
// then reads each time slot from one sample at the centre of its second cell, re-anchoring on a slot
// edge every 8 cells. Subframes come out in pico_spdif_rx's word layout. No hardware is touched here.

#include <stdbool.h>
#include <stdint.h>

// Words the caller keeps after the ring's end as a copy of its first words, so a subframe that starts
// near the end is read without wrapping.
#define SPDIF_RING_TAIL 96u

#define SPDIF_SYNC_B 0xFu   // sync codes in bits 3:0, as pico_spdif_rx writes them
#define SPDIF_SYNC_M 0xBu
#define SPDIF_SYNC_W 0x7u

// One subframe: bits 31:4 are time slots 31 to 4 (bit n is slot n), bits 3:0 the sync code. at is the
// sample index of the preamble's first edge.
typedef void (*spdif_emit_fn)(uint32_t word, uint64_t at, void *ctx);

typedef struct {
    uint64_t pos;           // the next sample the preamble search reads (a word boundary)
    uint32_t tracking;      // 1 while reading subframes at the cell centres
    uint32_t start;         // low 32 bits of the sample index of the current subframe's first edge
    uint32_t last_edge;     // search: low 32 bits of the sample index of the last level change
    uint32_t have_edge;     // search: last_edge is valid
    uint32_t level;         // search: line level after the last sample read
    uint32_t cell_q8;       // one cell in samples, 8 fractional bits; 0 until measured
    uint32_t th[4];         // search: 0.5, 1.5, 2.5 and 3.5 cells, the same units
    uint32_t longest;       // search: longest run while measuring
    uint32_t runs_seen;     // search: runs seen while measuring
    uint32_t npre;          // search: runs of a possible preamble so far
    uint32_t pre[4];
    uint32_t pre_at;        // search: low 32 bits of the possible preamble's first edge
    uint32_t have_first;    // search: a preamble was found; tracking starts at the next one
    uint32_t first_at;      // search: that preamble's first edge, low 32 bits
    uint32_t errors;        // tracking lost, or a preamble that did not match
    uint32_t subframes;     // subframes emitted
} spdif_decoder_t;

void spdif_decode_reset(spdif_decoder_t *d);
// Decodes what the ring holds up to sample `end` (exclusive, a word boundary). ring_words is a power of
// two, the ring must still hold every sample from the decoder's position to end, and its tail must
// already copy any of the first SPDIF_RING_TAIL words that lie before end.
void spdif_decode_ring(spdif_decoder_t *d, const uint32_t *ring, uint32_t ring_words, uint64_t end,
                       spdif_emit_fn emit, void *ctx);
// The oldest sample the decoder still needs, so a caller can tell when it has fallen too far behind.
uint64_t spdif_decode_needs(const spdif_decoder_t *d, uint64_t end);
// Gives up on everything before sample `from` (a word boundary) and searches again from there.
void spdif_decode_skip(spdif_decoder_t *d, uint64_t from);
