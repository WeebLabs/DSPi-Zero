// smi_frames_a32 (smi_frame.h): matches smi_frames_c bit for bit, as tests/smi_frame_test.c checks under
// QEMU. Scheduled for the in-order Cortex-A53, with the S/PDIF running XOR in core registers, left and
// right words side by side. A written frame costs its 512 bytes' trip to the uncached ring, about 275 cycles, which
// hides the work; a frame the ring already holds costs the compare, about 45 (bring-up section 27).

    .syntax unified
    .arch armv8-a
    .fpu neon-fp-armv8
    .arm

    .equ JOB_TYPE, 16
    .equ JOB_CLOCKS, 32
    .equ JOB_ADAT, 36
    .equ JOB_MSB8, 40
    .equ JOB_ADAT_KEY, 44
    .equ JOB_STRIDE, 48
    .equ JOB_STALE, 64

    // Stack while composing: the four raw pointers, then the frames written.
    .equ SP_WROTE, 16

// Slot s's 128 levels into q (dlo, dhi), from its raw words. Uses r4-r7, r12, lr and q8.
.macro LEVELS s, q, dlo, dhi
    ldr     lr, [r1, #JOB_TYPE + 4 * \s]
    ldr     r12, [sp, #4 * \s]
    cmp     lr, #1
    blo     3f
    bhi     2f
    // S/PDIF: a running XOR over the left and right words gives each slot's second half, less the
    // parity slot. A first half is the level before it inverted, with the preamble in the low 4 slots.
    // The carry is the left word's bit 31, the start of a block, whose preamble is Z.
    ldm     r12, {r4-r5}
    eors    r4, r4, r4, lsl #1
    eor     r5, r5, r5, lsl #1
    eor     r4, r4, r4, lsl #2
    eor     r5, r5, r5, lsl #2
    eor     r4, r4, r4, lsl #4
    eor     r5, r5, r5, lsl #4
    eor     r4, r4, r4, lsl #8
    eor     r5, r5, r5, lsl #8
    eor     r4, r4, r4, lsl #16
    eor     r5, r5, r5, lsl #16
    mvn     r6, r4, lsl #1
    mvn     r7, r5, lsl #1
    bic     r4, r4, #0x80000000
    bic     r5, r5, #0x80000000
    eor     r6, r6, #6
    eor     r7, r7, #6
    eorcs   r6, r6, #12
    vmov    \dhi, r4, r5
    vmov    \dlo, r6, r7
    vmull.p8 q8, \dhi, \dhi
    vmull.p8 \q, \dlo, \dlo
    vshl.i16 q8, q8, #1
    vorr    \q, \q, q8
    b       4f
    // I2S: the last R's LSB, L and R's top 31 bits, MSB first, each bit clock two cell times.
2:  ldmib   r12, {r4-r6}
    rbit    r5, r5
    rbit    r6, r6
    and     r4, r4, #1
    adds    r5, r5, r5
    adc     r6, r6, r6
    orr     r5, r5, r4
    vmov    \dlo, r5, r6
    vmull.p8 \q, \dlo, \dlo
    vshl.i16 q8, \q, #1
    vorr    \q, \q, q8
    b       4f
3:  vmov.i8 \q, #0
4:
.endm

// 16 cells (a q register) to 32 words with a zero high byte. q13 and q15 are zero.
.macro OUT32 q
    vmov    q12, \q
    vmov    q14, \q
    vst4.8  {d24, d26, d28, d30}, [r0:256]!
    vst4.8  {d25, d27, d29, d31}, [r0:256]!
.endm

// 16 cells to 32 words with ADAT word j in the high bytes. r12 is the msb8 table, lr the ADAT words.
.macro OUTA da, db, j
    ldr     r4, [lr, #4 * \j]
    vmov    d24, \da
    vmov    d25, \da
    vmov    d28, \db
    vmov    d29, \db
    lsr     r5, r4, #24
    ubfx    r6, r4, #16, #8
    ubfx    r7, r4, #8, #8
    uxtb    r4, r4
    add     r5, r12, r5, lsl #3
    add     r6, r12, r6, lsl #3
    add     r7, r12, r7, lsl #3
    add     r4, r12, r4, lsl #3
    vzip.8  d24, d25
    vzip.8  d28, d29
    vld1.8  {d26}, [r5]
    vld1.8  {d27}, [r6]
    vld1.8  {d30}, [r7]
    vld1.8  {d31}, [r4]
    vst2.8  {d24-d27}, [r0:256]!
    vst2.8  {d28-d31}, [r0:256]!
.endm

    .text
    .p2align 6
    .global smi_frames_a32
    .type smi_frames_a32, %function
smi_frames_a32:
    push    {r4-r11, lr}
    sub     sp, sp, #12
    ldr     r12, [r1, #JOB_STALE]
    add     lr, r1, #JOB_STRIDE
    ldm     r1, {r4-r7}
    ldm     lr, {r8-r11}
    mov     lr, #0
    cmp     r12, #0
    str     lr, [sp, #SP_WROTE - 16]
    bne     39f

    // Each frame's inputs against its key. r4-r7 are the slots' raw words, r8-r11 their strides, r2 the
    // key, r3 the frames left.
10: vld1.32 {d0-d1}, [r4]
    vld1.32 {d2-d3}, [r5]
    vld1.32 {d4-d5}, [r6]
    vld1.32 {d6-d7}, [r7]
    mov     r12, r2
    vld1.32 {d16-d19}, [r12:256]!
    vld1.32 {d20-d23}, [r12:256]
    ldr     lr, [r1, #JOB_ADAT]
    veor    q8, q8, q0
    veor    q9, q9, q1
    veor    q10, q10, q2
    veor    q11, q11, q3
    vorr    q8, q8, q9
    vorr    q10, q10, q11
    cmp     lr, #0
    vorr    q8, q8, q10
    beq     11f
    ldr     r12, [r1, #JOB_ADAT_KEY]
    vld1.32 {d0-d3}, [lr]
    vld1.32 {d4-d7}, [r12]
    veor    q2, q2, q0
    veor    q3, q3, q1
    vorr    q8, q8, q2
    vorr    q8, q8, q3
    vorr    d16, d16, d17
    vmov    r12, lr, d16
    orrs    r12, r12, lr
    bne     39f
    ldr     lr, [r1, #JOB_ADAT]
    ldr     r12, [r1, #JOB_ADAT_KEY]
    add     lr, lr, #32
    add     r12, r12, #32
    str     lr, [r1, #JOB_ADAT]
    str     r12, [r1, #JOB_ADAT_KEY]
    b       12f
11: vorr    d16, d16, d17
    vmov    r12, lr, d16
    orrs    r12, r12, lr
    bne     39f
12: add     r0, r0, #512
    add     r2, r2, #64
    add     r4, r4, r8
    add     r5, r5, r9
    add     r6, r6, r10
    add     r7, r7, r11
    subs    r3, r3, #1
    bne     10b
    mov     r0, #0
    add     sp, sp, #12
    pop     {r4-r11, pc}

    // A frame the ring does not hold: it and the rest of the run are written without looking at the
    // keys, which take the inputs. Audio and silence both come in long runs.
39: str     r3, [sp, #SP_WROTE - 16]
41: vld1.32 {d0-d1}, [r4]
    vld1.32 {d2-d3}, [r5]
    vld1.32 {d4-d5}, [r6]
    vld1.32 {d6-d7}, [r7]
    ldr     lr, [r1, #JOB_ADAT]
    vst1.32 {d0-d3}, [r2:256]!
    vst1.32 {d4-d7}, [r2:256]!
    cmp     lr, #0
    beq     20f
    ldr     r12, [r1, #JOB_ADAT_KEY]
    vld1.32 {d0-d3}, [lr]
    vst1.32 {d0-d3}, [r12]!
    str     r12, [r1, #JOB_ADAT_KEY]
20: push    {r4-r7}
    LEVELS  0, q0, d0, d1
    LEVELS  1, q1, d2, d3
    LEVELS  2, q2, d4, d5
    LEVELS  3, q3, d6, d7

    // Slots 0 and 2, and 1 and 3, to 2-bit fields: squaring spreads a byte's bits to the even bits.
    ldr     r4, [r1, #JOB_CLOCKS]
    vmull.p8 q8, d0, d0
    vmull.p8 q9, d4, d4
    vmull.p8 q10, d2, d2
    vmull.p8 q11, d6, d6
    vmull.p8 q12, d1, d1
    vmull.p8 q13, d5, d5
    vmull.p8 q14, d3, d3
    vmull.p8 q15, d7, d7
    lsl     r5, r4, #4
    lsl     r6, r4, #5
    vshl.i16 q9, q9, #1
    vshl.i16 q11, q11, #1
    vshl.i16 q13, q13, #1
    vshl.i16 q15, q15, #1
    vorr    q8, q8, q9
    vorr    q10, q10, q11
    vorr    q12, q12, q13
    vorr    q14, q14, q15
    // Those to nibbles, slot n in bit n, two cells a byte: q0, q2, q8, q10 hold 32 cells each.
    vmull.p8 q0, d16, d16
    vmull.p8 q1, d20, d20
    vmull.p8 q2, d17, d17
    vmull.p8 q3, d21, d21
    vmull.p8 q8, d24, d24
    vmull.p8 q9, d28, d28
    vmull.p8 q10, d25, d25
    vmull.p8 q11, d29, d29
    vdup.8  q12, r5
    vdup.8  q13, r6
    vshl.i16 q1, q1, #1
    vshl.i16 q3, q3, #1
    vshl.i16 q9, q9, #1
    vshl.i16 q11, q11, #1
    vmov.i8 q15, #0x0F
    vorr    q14, q12, q13
    vorr    q0, q0, q1
    vorr    q2, q2, q3
    vorr    q8, q8, q9
    vorr    q10, q10, q11
    // A byte a cell. BCK (q12) is high in the odd cell of each pair, LRCLK (q13) in the second half.
    vshr.u8 q1, q0, #4
    vshr.u8 q3, q2, #4
    vshr.u8 q9, q8, #4
    vshr.u8 q11, q10, #4
    vand    q0, q0, q15
    vand    q2, q2, q15
    vbif    q8, q13, q15
    vbif    q10, q13, q15
    vorr    q1, q1, q12
    vorr    q3, q3, q12
    vorr    q9, q9, q14
    vorr    q11, q11, q14
    vzip.8  q0, q1
    vzip.8  q2, q3
    vzip.8  q8, q9
    vzip.8  q10, q11
    ldr     lr, [r1, #JOB_ADAT]
    cmp     lr, #0
    bne     30f

    vmov.i8 q13, #0
    vmov.i8 q15, #0
    OUT32   q0
    OUT32   q1
    OUT32   q2
    OUT32   q3
    OUT32   q8
    OUT32   q9
    OUT32   q10
    OUT32   q11
    pop     {r4-r7}
    b       50f

30: ldr     r12, [r1, #JOB_MSB8]
    OUTA    d0, d1, 0
    OUTA    d2, d3, 1
    OUTA    d4, d5, 2
    OUTA    d6, d7, 3
    OUTA    d16, d17, 4
    OUTA    d18, d19, 5
    OUTA    d20, d21, 6
    OUTA    d22, d23, 7
    pop     {r4-r7}
    ldr     lr, [r1, #JOB_ADAT]
    add     lr, lr, #32
    str     lr, [r1, #JOB_ADAT]

    // The next frame.
50: add     r4, r4, r8
    add     r5, r5, r9
    add     r6, r6, r10
    add     r7, r7, r11
    subs    r3, r3, #1
    bne     41b
    ldr     r0, [sp, #SP_WROTE - 16]
    add     sp, sp, #12
    pop     {r4-r11, pc}
    .size smi_frames_a32, . - smi_frames_a32
