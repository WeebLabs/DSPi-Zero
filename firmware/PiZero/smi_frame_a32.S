// smi_frames_a32 (smi_frame.h): matches smi_frames_c bit for bit, as tests/smi_frame_test.c checks under
// QEMU. Scheduled for the in-order Cortex-A53, with left and right words side by side. A written frame
// costs its 512 bytes' trip to the uncached ring, about 275 cycles, and a frame the ring already holds
// costs the compare, about 45 (bring-up section 27). ADAT is encoded here from samples (section 29).

    .syntax unified
    .arch armv8-a
    .fpu neon-fp-armv8
    .arm

    .equ JOB_TYPE, 16
    .equ JOB_CLOCKS, 32
    .equ JOB_ADAT, 36
    .equ JOB_ADAT_KEY, 40
    .equ JOB_STRIDE, 44
    .equ JOB_STALE, 60

    // Stack while composing: the four raw pointers, then the frames written. While ADAT runs, a copy
    // of smi_adat_consts and d8-d15 lie between them: the copy is in a cache line every frame uses.
    .equ SP_WROTE, 16
    .equ SP_CONSTS, 16

// While ADAT runs, the job's ADAT pointers are kept at the end of the run, so a frame's are those less
// 32 bytes for each frame left (r3). Uses lr and r12.
.macro ADATPTRS
    ldr     r12, [r1, #JOB_ADAT_KEY]
    sub     lr, lr, r3, lsl #5
    sub     r12, r12, r3, lsl #5
.endm

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

// 32 cells (qa, qb) to 64 words with ADAT's levels in the high bytes. w is 8 bytes of levels, a bit a
// word. Squaring twice puts a cell's two levels in bits 0 and 4 of a byte. q7 is 0x0F in every byte.
.macro OUTV qa, qb, w
    vmull.p8 q6, \w, \w
    vmov    q12, \qa
    vmov    q14, \qa
    vmull.p8 q13, d12, d12
    vshr.u8 q15, q13, #4
    vand    q13, q13, q7
    vst4.8  {d24, d26, d28, d30}, [r0:256]!
    vst4.8  {d25, d27, d29, d31}, [r0:256]!
    vmov    q12, \qb
    vmov    q14, \qb
    vmull.p8 q13, d13, d13
    vshr.u8 q15, q13, #4
    vand    q13, q13, q7
    vst4.8  {d24, d26, d28, d30}, [r0:256]!
    vst4.8  {d25, d27, d29, d31}, [r0:256]!
.endm

// The ADAT ring frame at lr, and in q1 (words 4-7), to its 256 line levels in q4 and q5, the first
// sent in bit 0, as smi_adat_levels does. NEON has no bit reversal in AArch32, so the words go through
// r4-r7. Uses q0-q3, q8-q10, d30, r4-r7, r12 and lr.
.macro ADATLEVELS
    ldm     lr!, {r4-r7}
    add     r12, sp, #SP_CONSTS
    vshr.s64 d30, d3, #63               // the level the frame starts from, as a mask
    rbit    r4, r4
    rbit    r5, r5
    rbit    r6, r6
    rbit    r7, r7
    vmov    d0, r4, r5
    vmov    d1, r6, r7
    ldm     lr, {r4-r7}
    vmvn.i32 q8, #0x001FFFFF
    rbit    r4, r4
    rbit    r5, r5
    rbit    r6, r6
    rbit    r7, r7
    vmov    d2, r4, r5
    vmov    d3, r6, r7
    vld1.32 {d18-d21}, [r12:64]!
    vshr.u32 q0, q0, #7                 // reversed, the sample's MSB in bit 1
    vshr.u32 q1, q1, #7
    vshl.i32 q2, q0, #4                 // nibbles 4 and 5 up 4
    vshl.i32 q3, q1, #4
    vbit    q0, q2, q8
    vbit    q1, q3, q8
    vshl.i32 q2, q0, #2                 // nibbles 2 and 3 up 2
    vshl.i32 q3, q1, #2
    vbit    q0, q2, q9
    vbit    q1, q3, q9
    vld1.32 {d16-d18}, [r12:64]
    vshl.i32 q2, q0, #1                 // nibbles 1, 3 and 5 up 1
    vshl.i32 q3, q1, #1
    vbit    q0, q2, q10
    vbit    q1, q3, q10
    vorr    q0, q0, q8                  // the 1 before each nibble: 30 bits a sample
    vorr    q1, q1, q8
    // Two samples to 60 bits, then the four to the 256-bit frame after the 16 bits of sync in d18.
    vshr.u64 q2, q0, #32
    vshr.u64 q3, q1, #32
    vsli.64 q0, q2, #30
    vsli.64 q1, q3, #30
    vshr.u64 d19, d0, #48
    vshr.u64 d20, d1, #52
    vshr.u64 d21, d2, #56
    vsli.64 d18, d0, #16
    vsli.64 d19, d1, #12
    vsli.64 d20, d2, #8
    vsli.64 d21, d3, #4
    // NRZI: the running XOR within each 64 bits.
    vshl.i64 q2, q9, #1
    vshl.i64 q3, q10, #1
    veor    q9, q9, q2
    veor    q10, q10, q3
    vshl.i64 q2, q9, #2
    vshl.i64 q3, q10, #2
    veor    q9, q9, q2
    veor    q10, q10, q3
    vshl.i64 q2, q9, #4
    vshl.i64 q3, q10, #4
    veor    q9, q9, q2
    veor    q10, q10, q3
    vshl.i64 q2, q9, #8
    vshl.i64 q3, q10, #8
    veor    q9, q9, q2
    veor    q10, q10, q3
    vshl.i64 q2, q9, #16
    vshl.i64 q3, q10, #16
    veor    q9, q9, q2
    veor    q10, q10, q3
    vshl.i64 q2, q9, #32
    vshl.i64 q3, q10, #32
    veor    q9, q9, q2
    veor    q10, q10, q3
    // Each 64 bits is inverted if the level before it is 1: the starting level, XORed with the last
    // level of each 64 before.
    vshr.s64 q2, q9, #63
    vshr.s64 d6, d20, #63
    veor    d4, d4, d30                 // before the second
    veor    d6, d6, d5
    veor    d5, d5, d4                  // before the third
    veor    d6, d6, d4                  // before the fourth
    veor    d8, d18, d30
    veor    d9, d19, d4
    veor    d10, d20, d5
    veor    d11, d21, d6
.endm

    .section .rodata
    .p2align 5
smi_adat_consts:
    .word   0x0007F800, 0x0007F800, 0x0007F800, 0x0007F800      // nibbles 2 and 3, moved
    .word   0x3C0F03C0, 0x3C0F03C0, 0x3C0F03C0, 0x3C0F03C0      // nibbles 1, 3 and 5, moved
    .word   0x02108421, 0x02108421, 0x02108421, 0x02108421      // the 1 before each nibble
    .word   0x0801, 0                                           // the sync pattern

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
    str     lr, [sp, #SP_WROTE - 16]
    ldr     lr, [r1, #JOB_ADAT]
    cmp     lr, #0
    beq     9f
    add     lr, lr, r3, lsl #5
    str     lr, [r1, #JOB_ADAT]
    ldr     lr, [r1, #JOB_ADAT_KEY]
    add     lr, lr, r3, lsl #5
    str     lr, [r1, #JOB_ADAT_KEY]
9:  cmp     r12, #0
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
    ADATPTRS
    vld1.32 {d0-d3}, [lr]
    vld1.32 {d4-d7}, [r12]
    veor    q2, q2, q0
    veor    q3, q3, q1
    vorr    q8, q8, q2
    vorr    q8, q8, q3
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
    // keys, which take the inputs. Audio and silence both come in long runs. ADAT uses q4-q7, which
    // are the caller's to keep, and its constants.
39: str     r3, [sp, #SP_WROTE - 16]
    ldr     lr, [r1, #JOB_ADAT]
    cmp     lr, #0
    beq     41f
    vpush   {d8-d15}
    ldr     r12, =smi_adat_consts
    sub     sp, sp, #56
    vld1.32 {d0-d3}, [r12:256]!
    vld1.32 {d4-d6}, [r12:64]
    mov     r12, sp
    vst1.32 {d0-d3}, [r12:64]!
    vst1.32 {d4-d6}, [r12:64]
41: vld1.32 {d0-d1}, [r4]
    vld1.32 {d2-d3}, [r5]
    vld1.32 {d4-d5}, [r6]
    vld1.32 {d6-d7}, [r7]
    ldr     lr, [r1, #JOB_ADAT]
    vst1.32 {d0-d3}, [r2:256]!
    vst1.32 {d4-d7}, [r2:256]!
    cmp     lr, #0
    beq     20f
    ADATPTRS
    vld1.32 {d0-d3}, [lr]
    vst1.32 {d0-d3}, [r12]
    push    {r4-r7}
    ADATLEVELS
    b       21f
20: push    {r4-r7}
21:
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

30: vmov.i8 q7, #0x0F
    OUTV    q0, q1, d8
    OUTV    q2, q3, d9
    OUTV    q8, q9, d10
    OUTV    q10, q11, d11
    pop     {r4-r7}

    // The next frame.
50: add     r4, r4, r8
    add     r5, r5, r9
    add     r6, r6, r10
    add     r7, r7, r11
    subs    r3, r3, #1
    bne     41b
    ldr     lr, [r1, #JOB_ADAT]
    cmp     lr, #0
    addne   sp, sp, #56
    vpopne  {d8-d15}
    ldr     r0, [sp, #SP_WROTE - 16]
    add     sp, sp, #12
    pop     {r4-r11, pc}
    .ltorg
    .size smi_frames_a32, . - smi_frames_a32
