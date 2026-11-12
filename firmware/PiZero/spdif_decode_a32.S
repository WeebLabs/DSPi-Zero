// spdif_decode.c's subframe reader for AArch32 ARM state. It does what the C read_levels does, sample
// for sample; the comments there describe the method.
//
// uint64_t spdif_read_levels(const uint32_t *base, uint32_t a, uint32_t c);

// Positions are kept inverted (q = ~sample): the word index is ~(q asr 5), and a rotate right by q
// brings the sample to bit 0 with no masking. r1 is the next anchor window's first sample, and each
// sample is a fixed distance back from it.

//   r0 base    r1 q    r4, r5, r6 distances to samples 1, 2, 3    r7 7.5 cells    r9 levels
//   r2, r3, r11 window work    r12 + lr, r8 + r10 one sample each    [sp] edge limit, [sp, #4] cells

// The order is for the Cortex-A53, which issues two instructions a cycle in order, one load and one
// shift by register among them. A load's word is ready 3 cycles later (2 for a plain operand), and a
// result 1 cycle later, 2 as a shifted operand or shift count and 3 as an address. Each instruction
// below sits where its inputs are just ready, so a group of 32 takes 16 cycles.

    .syntax unified
    .arch armv7-a
    .arm
    .text

// One anchor. Finishes the third sample of the anchor before (in \qa and \ta, to bit \bp), reads the
// window that finds the next anchor (its first sample to bit \bw) and the first two samples after the
// current anchor (bits \b1 and \b2), and starts the third in \qb and \tb for what follows to finish.
.macro GROUP qa, ta, qb, tb, bp, b1, b2, bw
    ror     \ta, \ta, \qa
    add     \qb, r1, r4
    mvn     r2, r1, asr #5              // the window: its first word index
    bfi     r9, \ta, #\bp, #1
    add     r3, r2, #1
    mvn     \tb, \qb, asr #5
    add     \qa, r1, r5
    and     r11, r1, #31
    ldr     r2, [r0, r2, lsl #2]
    rsb     r11, r11, #31               // the window's shift
    ldr     \tb, [r0, \tb, lsl #2]
    mvn     \ta, \qa, asr #5
    ldr     r3, [r0, r3, lsl #2]
    sub     r1, r1, r7                  // 7.5 cells on; the edge is added below
    lsl     r2, r2, r11
    rsb     r11, r11, #32
    ror     \tb, \tb, \qb
    add     \qb, r1, r6
    ldr     \ta, [r0, \ta, lsl #2]
    bfi     r9, \tb, #\b1, #1
    mvn     \tb, \qb, asr #5
    orrs    r3, r2, r3, lsr r11         // 32 samples, the first in bit 31 and in N
    orrmi   r9, r9, #1 << \bw
    ldr     r2, [sp]
    mvnmi   r3, r3                      // set where they differ from the first
    ror     \ta, \ta, \qa
    ldr     \tb, [r0, \tb, lsl #2]
    clz     r11, r3
    bfi     r9, \ta, #\b2, #1
    cmp     r11, r2
    sub     r1, r1, r11                 // the next window's first sample
    bhi     9f
.endm

    .global spdif_read_levels
    .type   spdif_read_levels, %function
    .align  6
spdif_read_levels:
    push    {r4-r11, lr}
    // Preamble cells 0, 2, 4 and 6 to r9 bits 7:4 and [sp, #4], and the distances (c is r2, in 1/256
    // sample), among the first anchor: GROUP without a sample to finish, where the samples are
    // preamble cells 1, 3 and 5 (bits 2, 1 and 0).
    mov     r9, #0
    lsr     r3, r2, #8
    str     r3, [sp, #-8]!
    mvn     r1, r1
    add     r3, r2, r2, lsl #2
    sub     r12, r1, r2, lsr #9
    add     r11, r2, r2, lsl #3
    rsb     r7, r2, r2, lsl #4
    mvn     lr, r12, asr #5
    sub     r8, r1, r3, lsr #9
    add     r3, r3, r2, lsl #3
    mvn     r10, r8, asr #5
    ldr     lr, [r0, lr, lsl #2]
    add     r4, r2, r2, lsl #1
    add     r6, r4, r2, lsl #3
    ldr     r10, [r0, r10, lsl #2]
    ror     lr, lr, r12
    sub     r12, r1, r11, lsr #9
    bfi     r9, lr, #7, #1
    lsr     r7, r7, #9
    ror     r10, r10, r8
    sub     r8, r1, r3, lsr #9
    sub     r1, r1, r7                  // the first window's first sample
    mvn     lr, r12, asr #5
    bfi     r9, r10, #6, #1
    mvn     r10, r8, asr #5
    sub     r4, r7, r4, lsr #9
    rsb     r5, r2, r2, lsl #3
    ldr     lr, [r0, lr, lsl #2]
    ldr     r10, [r0, r10, lsl #2]
    and     r11, r1, #31
    sub     r5, r7, r5, lsr #9
    rsb     r11, r11, #31               // the window's shift
    ror     lr, lr, r12
    bfi     r9, lr, #5, #1
    ror     r10, r10, r8
    add     r8, r1, r4
    lsr     r6, r6, #9
    mvn     r2, r1, asr #5              // the window: its first word index
    bfi     r9, r10, #4, #1
    mvn     r10, r8, asr #5
    str     r9, [sp, #4]
    add     r12, r1, r5
    add     r3, r2, #1
    sub     r1, r1, r7                  // 7.5 cells on; the edge is added below
    ldr     r2, [r0, r2, lsl #2]
    ldr     r10, [r0, r10, lsl #2]
    rsb     r6, r6, r7, lsl #1
    ldr     r3, [r0, r3, lsl #2]
    mvn     lr, r12, asr #5
    lsl     r2, r2, r11
    rsb     r11, r11, #32
    mov     r9, #0
    ror     r10, r10, r8
    ldr     lr, [r0, lr, lsl #2]
    add     r8, r1, r6
    bfi     r9, r10, #2, #1
    orrs    r3, r2, r3, lsr r11         // 32 samples, the first in bit 31 and in N
    mvnmi   r3, r3                      // set where they differ from the first
    mvn     r10, r8, asr #5
    orrmi   r9, r9, #1 << 3
    ldr     r2, [sp]
    clz     r11, r3
    ror     lr, lr, r12
    ldr     r10, [r0, r10, lsl #2]
    sub     r1, r1, r11                 // the next window's first sample
    cmp     r11, r2
    bfi     r9, lr, #1, #1
    bhi     9f
    // Seven groups of four slots, taking turns with the sample registers. The last window finds the
    // next subframe's first edge.
    GROUP   r8, r10, r12, lr, 0, 4, 5, 7
    GROUP   r12, lr, r8, r10, 6, 8, 9, 11
    GROUP   r8, r10, r12, lr, 10, 12, 13, 15
    GROUP   r12, lr, r8, r10, 14, 16, 17, 19
    GROUP   r8, r10, r12, lr, 18, 20, 21, 23
    GROUP   r12, lr, r8, r10, 22, 24, 25, 27
    GROUP   r8, r10, r12, lr, 26, 28, 29, 31
    ror     lr, lr, r12
    mvn     r1, r1
    ldr     r2, [sp, #4]
    bfi     r9, lr, #30, #1
    sub     r1, r1, r7                  // the next subframe's first edge
    mov     r0, r9
    orr     r1, r2, r1, lsl #8
    add     sp, sp, #8
    pop     {r4-r11, pc}
9:
    mov     r0, #0
    add     sp, sp, #8
    pop     {r4-r11, pc}
    .size   spdif_read_levels, . - spdif_read_levels
