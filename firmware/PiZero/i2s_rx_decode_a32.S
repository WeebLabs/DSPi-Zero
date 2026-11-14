// The two hot pieces of i2s_rx_decode.c for AArch32 with NEON. Each does what its C version does
// (i2s_rx_scan_c, i2s_rx_frame_c), word for word; tests/i2s_rx_decode_test.c compares them under QEMU.

    .syntax unified
    .arch armv7-a
    .fpu neon
    .arm
    .text

// uint32_t i2s_rx_scan_a32(const uint32_t *samples, uint32_t n, i2s_rx_scan_t *s, uint16_t *out);

// n is a multiple of 32. BCK's 32 levels in each 32 snapshots are packed into one word, the first
// snapshot in bit 31. Eight registers hold four snapshots each. Shift and insert merges them in three
// rounds, keeping one nibble of each, and one bit of each nibble is BCK. The rises are then walked in
// a core register, and each one's code is stored.

//   r0 block    r1 snapshots left    r2 state    r3 out    r4 shift    r5 load pointer    r6 levels, code
//   r7 rises    r8 index    r9 out at entry    r12 BCK of the snapshot before, in bit 31    lr bit 31
//   q12 each lane's shift to its place in the nibble    q13 each lane's bits

// An insert keeps the destination's top bits (vsri) or its bottom bits (vsli). The rounds work in byte,
// 16-bit and 32-bit lanes, and each uses the insert that keeps the part of the lane where BCK's nibble,
// byte or half is, with the registers ordered so that the earlier snapshots end on top. The seven
// inserts and the register that holds the result are given for each of the eight cases below.
.macro LEVELS i1, a1, b1, i2, a2, b2, i3, a3, b3, i4, a4, b4, i5, a5, b5, i6, a6, b6, i7, a7, b7, q, lo, hi
    vld1.32 {d0-d3}, [r5]!              // snapshots 0 to 7, four a register
    vld1.32 {d4-d7}, [r5]!
    vld1.32 {d16-d19}, [r5]!
    vld1.32 {d20-d23}, [r5]!
    \i1     \a1, \b1, #4                // BCK's byte now holds two registers' nibbles
    \i2     \a2, \b2, #4
    \i3     \a3, \b3, #4
    \i4     \a4, \b4, #4
    \i5     \a5, \b5, #8
    \i6     \a6, \b6, #8
    \i7     \a7, \b7, #16               // a nibble for each register, the first on top
    vshl.u32 \q, \q, q12
    vand    \q, \q, q13                 // lane j keeps bit 3 - j of every nibble
    vorr    \lo, \lo, \hi
.endm

// The loop for one case. The next block's levels are worked out before the current block's rises are
// walked, so the move to the core registers never waits.
.macro SCAN i1, a1, b1, i2, a2, b2, i3, a3, b3, i4, a4, b4, i5, a5, b5, i6, a6, b6, i7, a7, b7, q, lo, hi
    LEVELS  \i1, \a1, \b1, \i2, \a2, \b2, \i3, \a3, \b3, \i4, \a4, \b4, \i5, \a5, \b5, \i6, \a6, \b6, \
            \i7, \a7, \b7, \q, \lo, \hi
1:  vmov    r6, r7, \lo
    subs    r1, r1, #32
    orr     r6, r6, r7
    beq     2f
    LEVELS  \i1, \a1, \b1, \i2, \a2, \b2, \i3, \a3, \b3, \i4, \a4, \b4, \i5, \a5, \b5, \i6, \a6, \b6, \
            \i7, \a7, \b7, \q, \lo, \hi
2:  orr     r7, r12, r6, lsr #1         // each snapshot's predecessor, the first one's in bit 31
    bics    r7, r6, r7                  // high now and low before
    and     r12, lr, r6, lsl #31
    beq     4f
    // Each pass finds a rise and loads its snapshot, and shifts and stores the one before, so no
    // instruction waits on the one just ahead of it.
    clz     r8, r7
    bics    r7, r7, lr, lsr r8
    ldr     r6, [r0, r8, lsl #2]
    beq     33f
3:  clz     r8, r7
    lsr     r6, r6, r4
    bics    r7, r7, lr, lsr r8
    strh    r6, [r3], #2
    ldr     r6, [r0, r8, lsl #2]
    bne     3b
33: lsr     r6, r6, r4
    strh    r6, [r3], #2
4:  add     r0, r0, #128
    cmp     r1, #0
    bne     1b
    b       9f
.endm

    .balign 16
scan_lanes:
    .long   3, 2, 1, 0
    .long   0x88888888, 0x44444444, 0x22222222, 0x11111111

    .p2align 6
    .global i2s_rx_scan_a32
    .type i2s_rx_scan_a32, %function
i2s_rx_scan_a32:
    push    {r4-r9, lr}
    ldm     r2, {r4, r5, r12}           // shift, BCK's bit, BCK before
    mov     r9, r3
    mov     lr, #0x80000000
    lsl     r12, r12, #31
    cmp     r1, #0
    beq     9f
    and     r6, r5, #3                  // BCK's bit in its nibble
    vdup.32 q15, r6
    adr     r6, scan_lanes
    vld1.32 {q12, q13}, [r6]
    vsub.s32 q12, q12, q15
    // The case: bit 0 for BCK in its byte's high nibble, bit 1 for an odd byte, bit 2 for the
    // word's upper half.
    lsr     r6, r5, #2
    and     r6, r6, #1
    tst     r5, #8
    orrne   r6, r6, #2
    tst     r5, #16
    orrne   r6, r6, #4
    mov     r5, r0
    add     pc, pc, r6, lsl #2
    nop
    b       10f
    b       11f
    b       12f
    b       13f
    b       14f
    b       15f
    b       16f
    b       17f
10:
    SCAN    vsli.8, q1, q0, vsli.8, q3, q2, vsli.8, q9, q8, vsli.8, q11, q10, vsli.16, q3, q1, \
            vsli.16, q11, q9, vsli.32, q11, q3, q11, d22, d23
11:
    SCAN    vsri.8, q0, q1, vsri.8, q2, q3, vsri.8, q8, q9, vsri.8, q10, q11, vsli.16, q2, q0, \
            vsli.16, q10, q8, vsli.32, q10, q2, q10, d20, d21
12:
    SCAN    vsli.8, q1, q0, vsli.8, q3, q2, vsli.8, q9, q8, vsli.8, q11, q10, vsri.16, q1, q3, \
            vsri.16, q9, q11, vsli.32, q9, q1, q9, d18, d19
13:
    SCAN    vsri.8, q0, q1, vsri.8, q2, q3, vsri.8, q8, q9, vsri.8, q10, q11, vsri.16, q0, q2, \
            vsri.16, q8, q10, vsli.32, q8, q0, q8, d16, d17
14:
    SCAN    vsli.8, q1, q0, vsli.8, q3, q2, vsli.8, q9, q8, vsli.8, q11, q10, vsli.16, q3, q1, \
            vsli.16, q11, q9, vsri.32, q3, q11, q3, d6, d7
15:
    SCAN    vsri.8, q0, q1, vsri.8, q2, q3, vsri.8, q8, q9, vsri.8, q10, q11, vsli.16, q2, q0, \
            vsli.16, q10, q8, vsri.32, q2, q10, q2, d4, d5
16:
    SCAN    vsli.8, q1, q0, vsli.8, q3, q2, vsli.8, q9, q8, vsli.8, q11, q10, vsri.16, q1, q3, \
            vsri.16, q9, q11, vsri.32, q1, q9, q1, d2, d3
17:
    SCAN    vsri.8, q0, q1, vsri.8, q2, q3, vsri.8, q8, q9, vsri.8, q10, q11, vsri.16, q0, q2, \
            vsri.16, q8, q10, vsri.32, q0, q8, q0, d0, d1
9:  lsr     r12, r12, #31
    str     r12, [r2, #8]
    sub     r0, r3, r9
    lsr     r0, r0, #1
    pop     {r4-r9, pc}

    .size i2s_rx_scan_a32, . - i2s_rx_scan_a32

// uint32_t i2s_rx_frame_a32(const uint16_t *codes, const i2s_rx_frame_t *f);

// 64 codes are one frame when LRCLK reads low in the first 31, high in the next 32 and low in the last.
// Each data line's 32 bits a word are then gathered with a weight for each code and pairwise adds, which
// leave the word's four bytes first code first, so one byte reversal makes the stored word.

//   q0-q3 codes 0 to 31, then their data bytes in q0, q1    q8-q11 codes 32 to 63, their bytes in q2, q3
//   q12 work    q13 weights    q14 work    d30 the lines' masks    r2 LRCLK's mask    r3 the lowest data
//   line's bit    r12 destinations

// Data line \k: its bit of each code's byte (mask d30[\k]) into the two words at [\dst].
.macro LINE k, dst
    vdup.8  q12, d30[\k]
    vtst.8  q8, q0, q12
    vtst.8  q9, q1, q12
    vtst.8  q10, q2, q12
    vtst.8  q11, q3, q12
    vand    q8, q8, q13
    vand    q9, q9, q13
    vand    q10, q10, q13
    vand    q11, q11, q13
    vpadd.u8 d16, d16, d17
    vpadd.u8 d18, d18, d19
    vpadd.u8 d20, d20, d21
    vpadd.u8 d22, d22, d23
    vpadd.u8 d16, d16, d18              // the left word's four bytes twice over
    vpadd.u8 d20, d20, d22
    vpadd.u8 d16, d16, d20              // left, then right
    vrev32.8 d16, d16
    vst1.32 {d16}, [\dst]
.endm

    .p2align 6
    .global i2s_rx_frame_a32
    .type i2s_rx_frame_a32, %function
i2s_rx_frame_a32:
    ldm     r1, {r2, r3}
    add     r12, r1, #8
    vld1.16 {q0, q1}, [r0]!
    vld1.16 {q2, q3}, [r0]!
    vld1.16 {q8, q9}, [r0]!
    vld1.16 {q10, q11}, [r0]
    // LRCLK. With the last code of each half flipped, the left half must be all low and the right
    // half all high.
    vdup.16 q12, r2
    adr     r0, frame_last
    vld1.16 {q13}, [r0]
    vand    q13, q13, q12               // LRCLK's bit in the last code only
    veor    q14, q3, q13
    veor    q15, q11, q13
    vorr    q14, q14, q0
    vand    q15, q15, q8
    vorr    q14, q14, q1
    vand    q15, q15, q9
    vorr    q14, q14, q2
    vand    q15, q15, q10
    vorn    q14, q14, q15               // high in the left half, or low in the right
    vand    q14, q14, q12
    vorr    d28, d28, d29
    vmov    r0, r2, d28
    orrs    r0, r0, r2
    movne   r0, #0
    bxne    lr
    // Each code's data lines into a byte, the lowest line in bit 0.
    rsb     r3, r3, #0
    vdup.16 q12, r3
    adr     r0, frame_weights
    vld1.8  {q13}, [r0]
    vshl.u16 q0, q0, q12
    vshl.u16 q1, q1, q12
    vshl.u16 q2, q2, q12
    vshl.u16 q3, q3, q12
    vshl.u16 q8, q8, q12
    vshl.u16 q9, q9, q12
    vshl.u16 q10, q10, q12
    vshl.u16 q11, q11, q12
    vmovn.i16 d0, q0
    vmovn.i16 d1, q1
    vmovn.i16 d2, q2
    vmovn.i16 d3, q3
    vmovn.i16 d4, q8
    vmovn.i16 d5, q9
    vmovn.i16 d6, q10
    vmovn.i16 d7, q11
    add     r0, r12, #16
    vld1.32 {d30[0]}, [r0]
    ldm     r12, {r0, r1, r2, r3}
    LINE    0, r0
    LINE    1, r1
    LINE    2, r2
    LINE    3, r3
    mov     r0, #1
    bx      lr

    .balign 16
frame_last:
    .short  0, 0, 0, 0, 0, 0, 0, 0xFFFF
    .balign 16
frame_weights:
    .byte   128, 64, 32, 16, 8, 4, 2, 1, 128, 64, 32, 16, 8, 4, 2, 1
    .size i2s_rx_frame_a32, . - i2s_rx_frame_a32
