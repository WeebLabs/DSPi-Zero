#pragma once

// Decoder for an 8-channel I2S input captured as snapshots of the GPIO level register (GPLEV0): four data
// lines on consecutive GPIOs sharing one BCK and LRCLK, 64 bit clocks a frame, Philips framing. It takes
// the data at the first snapshot after each BCK rising edge and writes 32-bit words, MSB first and left
// first, into one ring per stereo pair, as DSPi's PIO receiver does. No hardware is touched here.

#include <stdbool.h>
#include <stdint.h>

#define I2S_RX_LINES 4u
#define I2S_RX_HALF_MAX 64u         // codes kept for one LRCLK half; more than 32 is already an error
#define I2S_RX_SCAN_SAMPLES 2048u   // samples scanned before their frames are built
#define I2S_RX_PENDING (I2S_RX_SCAN_SAMPLES / 2u + 64u)

// Where the signals sit. A snapshot's code is its 16 bits from bit `shift` up, and every signal must
// lie in that window: BCK at GPIO bck, LRCLK at GPIO lr, data line k at GPIO data + k.
typedef struct {
    uint8_t shift, bck, lr, data;
} i2s_rx_pins_t;

enum { I2S_RX_GAP_NONE, I2S_RX_GAP_ARMED, I2S_RX_GAP_FOUND };

typedef struct {
    i2s_rx_pins_t pins;
    uint16_t bck_mask, lr_mask;     // in the code
    uint32_t data_shift;            // code bit of data line 0
    uint32_t *ring[I2S_RX_LINES];   // one per stereo pair
    uint32_t ring_mask;             // ring words - 1; the ring length is an even power of two
    uint32_t wr;                    // words written to each ring so far, two a frame
    uint32_t lead[I2S_RX_LINES];    // even: line k's frame n goes to ring index wr + lead[k]; 0 from init

    uint32_t frames;                // frames written, good or concealed
    uint32_t bad_halves;            // LRCLK halves without 32 bit clocks, after the first good frame
    uint32_t concealed;             // frames written as a repeat of the frame before
    uint32_t repaired;              // capture gaps where a lost bit clock was put back
    uint32_t gaps;                  // capture gaps declared
    uint32_t bad_short, bad_one_short, bad_one_long, bad_long;     // bad halves by their bit clocks
    uint32_t bad_at_gap;            // bad halves that held a capture gap

    uint32_t prev;                  // 1 when the last snapshot had BCK high
    uint32_t lr;                    // LRCLK level of the half being received, 2 before the first clock
    uint32_t n;                     // bit clocks seen in this half
    uint32_t synced;                // a good frame has been written
    uint32_t left_ok;
    uint32_t left[I2S_RX_LINES];
    uint16_t half[I2S_RX_HALF_MAX];

    uint32_t gap;                   // I2S_RX_GAP_*
    uint32_t gap_at;                // bit clocks seen in the half when the gap came
    uint32_t gap_since;             // snapshots from the last BCK rise to the gap, then on to the next rise
    uint32_t gap_dist;              // across a gap, that many snapshots between two rises hides a lost one
    uint16_t gap_pre, gap_post, gap_code;
    bool gap_have_post;

    // For putting back a bit clock lost to a stalled capture, which needs the snapshots themselves.
    const uint32_t *run_start;      // the first snapshot of the unbroken run being decoded
    const uint32_t *scan_end;       // just past the last snapshot taken in
    const uint32_t *edge_at;        // the snapshot of the rise being handled, or 0 to count back
    uint32_t edge_back;             // rises taken in after the one being handled
    uint32_t stall_dist;            // that many snapshots or more between two rises hides a lost one
    uint32_t phase_max;             // the most snapshots one BCK phase holds
    uint32_t rescued;               // halves one bit clock short that were put right
    // Why others were not: the snapshots were out of reach, no long distance between rises, more
    // than one, the lost bit's data in doubt, no BCK run long enough to hide a phase.
    uint32_t rescue_fail[5];
    const uint32_t *bad_edge;       // the rise that ended the last bad half, when its snapshot was in reach
    uint32_t bad_n;                 // that half's bit clocks
    const uint32_t *rescued_at;     // where the last stall put right was: the rise after the lost one
    uint32_t rescued_by_step;       // of those put right, the ones found by the step in the spacing
    uint32_t rescued_dist[4];       // the others by the two rises' distance over stall_dist: 0, 1, 2, more

    uint32_t npend;                 // codes scanned but not yet framed (the fast path only)
    uint16_t pend[I2S_RX_PENDING];
} i2s_rx_t;

// ring_words is a power of two, at least 4. period_q8 is one bit clock in snapshots, 8 fractional bits.
void i2s_rx_init(i2s_rx_t *d, const i2s_rx_pins_t *pins, uint32_t *const ring[I2S_RX_LINES],
                 uint32_t ring_words, uint32_t period_q8);
void i2s_rx_set_period(i2s_rx_t *d, uint32_t period_q8);    // a better measured bit clock, between calls

// Decodes n snapshots that follow the ones before with no gap. i2s_rx_decode is the fast path (NEON and
// assembly on the Pi), i2s_rx_decode_ref the plain one; both write the same words and counters. The fast
// path may hold back the codes of an unfinished frame until the next call or i2s_rx_flush.

// While calls continue one memory block (samples is where the last call ended), the snapshots already
// given must stay readable: a half found one bit clock short is looked up again in the last 500 or so.
void i2s_rx_decode(i2s_rx_t *d, const uint32_t *samples, uint32_t n);
void i2s_rx_decode_ref(i2s_rx_t *d, const uint32_t *samples, uint32_t n);
void i2s_rx_flush(i2s_rx_t *d);

// Says that snapshots were missed between the last one decoded and the next one given (a DMA control
// block reload). before points at the n_before snapshots that came last. If the gap swallowed one BCK
// rise, the decoder puts its bit back from the snapshot on the side of the gap where it was still valid.
void i2s_rx_gap(i2s_rx_t *d, const uint32_t *before, uint32_t n_before);

// The pieces of the fast path, in C here and in i2s_rx_decode_a32.S on the Pi. The scan writes the code
// of each snapshot at a BCK rise (n is a multiple of 32, *prev carries BCK's level) and returns the
// count. The frame builder returns 0 unless 64 codes are one whole frame, and then writes its 8 words.
typedef struct {
    uint32_t shift, bck_bit, prev;
} i2s_rx_scan_t;
uint32_t i2s_rx_scan_c(const uint32_t *samples, uint32_t n, i2s_rx_scan_t *s, uint16_t *out);
uint32_t i2s_rx_scan_a32(const uint32_t *samples, uint32_t n, i2s_rx_scan_t *s, uint16_t *out);
typedef struct {
    uint32_t lr_mask, data_shift;
    uint32_t *dst[I2S_RX_LINES];    // where each pair's left and right words go
} i2s_rx_frame_t;
uint32_t i2s_rx_frame_c(const uint16_t *codes, const i2s_rx_frame_t *f);
uint32_t i2s_rx_frame_a32(const uint16_t *codes, const i2s_rx_frame_t *f);
