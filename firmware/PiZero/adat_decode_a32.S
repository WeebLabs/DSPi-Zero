// adat_decode.c's group loop for AArch32 ARM state. It does what decode_groups_c does, group for group;
// the comments there describe the method.
//
// void adat_decode_groups(adat_slice_t *p);

// Positions are kept inverted (qi = ~position, in samples << 8): bits 12:8 of qi rotate the sample to bit
// 0, and one bic of qi >> 13 from the ring's word mask is its word. A cell takes bic, ubfx and sub, a load
// into the register that held its word index, a rotate and an insert. Four cells are in flight, so each
// load is used about six cycles after it issues, past the A53's three-cycle load latency.

// An edge window is the 32 samples from the centre c0 of the cell before the edge. Where they first
// differ from the first, k samples on, the edge lies k - 1/2 samples after c0 & ~255, so its distance
// from the grid's boundary c0 + half is (k << 8) + (qi0 & 255) - (half + 383), with qi0 = ~c0.

//   r0 ring    r1 qi    r2 cell    r3 mask    lr levels    r4-r12 work    [sp] p

    .syntax unified
    .arch armv7-a
    .arm
    .text

// adat_slice_t
    .equ    P_RING, 0
    .equ    P_MASK, 4
    .equ    P_CELL, 8
    .equ    P_QI, 12
    .equ    P_ROOM, 16
    .equ    P_LEVEL, 20
    .equ    P_FAR, 24
    .equ    P_N, 28
    .equ    P_OMASK, 32
    .equ    P_OUT, 36
    .equ    P_OUT_AT, 40
    .equ    P_FLIPS, 44
    .equ    P_HALF, 48
    .equ    P_GROUP, 52
    .equ    P_K, 56

    .global adat_decode_groups
    .type   adat_decode_groups, %function
    .p2align 6
adat_decode_groups:
    push    {r0, r4-r11, lr}
    ldr     r1, [r0, #P_QI]
    ldr     r2, [r0, #P_CELL]
    ldr     r3, [r0, #P_MASK]
    ldr     r0, [r0, #P_RING]
1:
    mov     lr, #0
    bic     r4, r3, r1, lsr #13
    ubfx    r5, r1, #8, #5
    sub     r1, r1, r2
    bic     r6, r3, r1, lsr #13
    ubfx    r7, r1, #8, #5
    sub     r1, r1, r2
    ldr     r4, [r0, r4, lsl #2]
    bic     r8, r3, r1, lsr #13
    ubfx    r9, r1, #8, #5
    sub     r1, r1, r2
    ldr     r6, [r0, r6, lsl #2]
    bic     r10, r3, r1, lsr #13
    ubfx    r11, r1, #8, #5
    sub     r1, r1, r2
    ldr     r8, [r0, r8, lsl #2]
    ror     r4, r4, r5
    bfi     lr, r4, #31, #1
    bic     r4, r3, r1, lsr #13
    ubfx    r5, r1, #8, #5
    sub     r1, r1, r2
    ldr     r10, [r0, r10, lsl #2]
    ror     r6, r6, r7
    bfi     lr, r6, #30, #1
    bic     r6, r3, r1, lsr #13
    ubfx    r7, r1, #8, #5
    sub     r1, r1, r2
    ldr     r4, [r0, r4, lsl #2]
    ror     r8, r8, r9
    bfi     lr, r8, #29, #1
    bic     r8, r3, r1, lsr #13
    ubfx    r9, r1, #8, #5
    sub     r1, r1, r2
    ldr     r6, [r0, r6, lsl #2]
    ror     r10, r10, r11
    bfi     lr, r10, #28, #1
    bic     r10, r3, r1, lsr #13
    ubfx    r11, r1, #8, #5
    sub     r1, r1, r2
    ldr     r8, [r0, r8, lsl #2]
    ror     r4, r4, r5
    bfi     lr, r4, #27, #1
    bic     r4, r3, r1, lsr #13
    ubfx    r5, r1, #8, #5
    sub     r1, r1, r2
    ldr     r10, [r0, r10, lsl #2]
    ror     r6, r6, r7
    bfi     lr, r6, #26, #1
    bic     r6, r3, r1, lsr #13
    ubfx    r7, r1, #8, #5
    sub     r1, r1, r2
    ldr     r4, [r0, r4, lsl #2]
    ror     r8, r8, r9
    bfi     lr, r8, #25, #1
    bic     r8, r3, r1, lsr #13
    ubfx    r9, r1, #8, #5
    sub     r1, r1, r2
    ldr     r6, [r0, r6, lsl #2]
    ror     r10, r10, r11
    bfi     lr, r10, #24, #1
    bic     r10, r3, r1, lsr #13
    ubfx    r11, r1, #8, #5
    sub     r1, r1, r2
    ldr     r8, [r0, r8, lsl #2]
    ror     r4, r4, r5
    bfi     lr, r4, #23, #1
    bic     r4, r3, r1, lsr #13
    ubfx    r5, r1, #8, #5
    sub     r1, r1, r2
    ldr     r10, [r0, r10, lsl #2]
    ror     r6, r6, r7
    bfi     lr, r6, #22, #1
    bic     r6, r3, r1, lsr #13
    ubfx    r7, r1, #8, #5
    sub     r1, r1, r2
    ldr     r4, [r0, r4, lsl #2]
    ror     r8, r8, r9
    bfi     lr, r8, #21, #1
    bic     r8, r3, r1, lsr #13
    ubfx    r9, r1, #8, #5
    sub     r1, r1, r2
    ldr     r6, [r0, r6, lsl #2]
    ror     r10, r10, r11
    bfi     lr, r10, #20, #1
    bic     r10, r3, r1, lsr #13
    ubfx    r11, r1, #8, #5
    sub     r1, r1, r2
    ldr     r8, [r0, r8, lsl #2]
    ror     r4, r4, r5
    bfi     lr, r4, #19, #1
    bic     r4, r3, r1, lsr #13
    ubfx    r5, r1, #8, #5
    sub     r1, r1, r2
    ldr     r10, [r0, r10, lsl #2]
    ror     r6, r6, r7
    bfi     lr, r6, #18, #1
    bic     r6, r3, r1, lsr #13
    ubfx    r7, r1, #8, #5
    sub     r1, r1, r2
    ldr     r4, [r0, r4, lsl #2]
    ror     r8, r8, r9
    bfi     lr, r8, #17, #1
    bic     r8, r3, r1, lsr #13
    ubfx    r9, r1, #8, #5
    sub     r1, r1, r2
    ldr     r6, [r0, r6, lsl #2]
    ror     r10, r10, r11
    bfi     lr, r10, #16, #1
    bic     r10, r3, r1, lsr #13
    ubfx    r11, r1, #8, #5
    sub     r1, r1, r2
    ldr     r8, [r0, r8, lsl #2]
    ror     r4, r4, r5
    bfi     lr, r4, #15, #1
    bic     r4, r3, r1, lsr #13
    ubfx    r5, r1, #8, #5
    sub     r1, r1, r2
    ldr     r10, [r0, r10, lsl #2]
    ror     r6, r6, r7
    bfi     lr, r6, #14, #1
    bic     r6, r3, r1, lsr #13
    ubfx    r7, r1, #8, #5
    sub     r1, r1, r2
    ldr     r4, [r0, r4, lsl #2]
    ror     r8, r8, r9
    bfi     lr, r8, #13, #1
    bic     r8, r3, r1, lsr #13
    ubfx    r9, r1, #8, #5
    sub     r1, r1, r2
    ldr     r6, [r0, r6, lsl #2]
    ror     r10, r10, r11
    bfi     lr, r10, #12, #1
    bic     r10, r3, r1, lsr #13
    ubfx    r11, r1, #8, #5
    sub     r1, r1, r2
    ldr     r8, [r0, r8, lsl #2]
    ror     r4, r4, r5
    bfi     lr, r4, #11, #1
    bic     r4, r3, r1, lsr #13
    ubfx    r5, r1, #8, #5
    sub     r1, r1, r2
    ldr     r10, [r0, r10, lsl #2]
    ror     r6, r6, r7
    bfi     lr, r6, #10, #1
    bic     r6, r3, r1, lsr #13
    ubfx    r7, r1, #8, #5
    sub     r1, r1, r2
    ldr     r4, [r0, r4, lsl #2]
    ror     r8, r8, r9
    bfi     lr, r8, #9, #1
    bic     r8, r3, r1, lsr #13
    ubfx    r9, r1, #8, #5
    sub     r1, r1, r2
    ldr     r6, [r0, r6, lsl #2]
    ror     r10, r10, r11
    bfi     lr, r10, #8, #1
    bic     r10, r3, r1, lsr #13
    ubfx    r11, r1, #8, #5
    sub     r1, r1, r2
    ldr     r8, [r0, r8, lsl #2]
    ror     r4, r4, r5
    bfi     lr, r4, #7, #1
    bic     r4, r3, r1, lsr #13
    ubfx    r5, r1, #8, #5
    sub     r1, r1, r2
    ldr     r10, [r0, r10, lsl #2]
    ror     r6, r6, r7
    bfi     lr, r6, #6, #1
    bic     r6, r3, r1, lsr #13
    ubfx    r7, r1, #8, #5
    sub     r1, r1, r2
    ldr     r4, [r0, r4, lsl #2]
    ror     r8, r8, r9
    bfi     lr, r8, #5, #1
    bic     r8, r3, r1, lsr #13
    ubfx    r9, r1, #8, #5
    sub     r1, r1, r2
    ldr     r6, [r0, r6, lsl #2]
    ror     r10, r10, r11
    bfi     lr, r10, #4, #1
    bic     r10, r3, r1, lsr #13
    ubfx    r11, r1, #8, #5
    sub     r1, r1, r2
    ldr     r8, [r0, r8, lsl #2]
    ror     r4, r4, r5
    bfi     lr, r4, #3, #1
    ldr     r10, [r0, r10, lsl #2]
    ror     r6, r6, r7
    bfi     lr, r6, #2, #1
    ror     r8, r8, r9
    bfi     lr, r8, #1, #1
    ror     r10, r10, r11
    bfi     lr, r10, #0, #1
    // The word: a 1 where a cell's level differs from the one before it. Stored with the last cell's
    // centre, ~qi - cell, in samples << 8.
    ldr     r12, [sp]
    ldr     r4, [r12, #P_LEVEL]
    ldr     r7, [r12, #P_N]
    ldr     r8, [r12, #P_OMASK]
    ldr     r9, [r12, #P_OUT]
    ldr     r10, [r12, #P_OUT_AT]
    lsl     r4, r4, #31
    orr     r4, r4, lr, lsr #1
    eor     r6, r4, lr
    and     r4, lr, #1
    and     r11, r7, r8
    mvn     r5, r1
    add     r7, r7, #1
    sub     r5, r5, r2
    str     r4, [r12, #P_LEVEL]
    str     r6, [r9, r11, lsl #2]
    str     r7, [r12, #P_N]
    str     r5, [r10, r11, lsl #2]
    ldr     r4, [r12, #P_ROOM]
    ldr     r5, [r12, #P_GROUP]
    sub     r7, r6, #1
    sub     r4, r4, r5
    ands    r7, r7, r6
    str     r4, [r12, #P_ROOM]
    beq     2f
    // The last two edges: cells k1 and k2 counted back from the end, their windows from the centres
    // of the cells before them, qi0 = qi + (k + 2) cell.
    rbit    r6, r6
    rbit    r7, r7
    clz     r6, r6
    clz     r7, r7
    add     r6, r6, #2
    add     r7, r7, #2
    mla     r6, r6, r2, r1
    mla     r7, r7, r2, r1
    ldr     r12, [r12, #P_K]
    bic     r4, r3, r6, lsr #13
    add     r5, r4, #1
    and     r5, r5, r3
    ldr     r8, [r0, r4, lsl #2]
    ldr     r9, [r0, r5, lsl #2]
    ubfx    r10, r6, #8, #5
    rsb     r4, r10, #31
    add     r10, r10, #1
    and     r5, r6, #255
    lsl     r8, r8, r4
    orr     r8, r8, r9, lsr r10
    eor     r8, r8, r8, asr #31
    orr     r8, r8, #1
    clz     r8, r8
    add     r8, r5, r8, lsl #8
    sub     lr, r8, r12
    bic     r4, r3, r7, lsr #13
    add     r5, r4, #1
    and     r5, r5, r3
    ldr     r8, [r0, r4, lsl #2]
    ldr     r9, [r0, r5, lsl #2]
    ubfx    r10, r7, #8, #5
    rsb     r4, r10, #31
    add     r10, r10, #1
    and     r5, r7, #255
    lsl     r8, r8, r4
    orr     r8, r8, r9, lsr r10
    eor     r8, r8, r8, asr #31
    orr     r8, r8, #1
    clz     r8, r8
    add     r8, r5, r8, lsl #8
    sub     r11, r8, r12
    // Halfway to their mean; their distance apart counts towards moving off the edges.
    ldr     r12, [sp]
    subs    r4, lr, r11
    add     r5, lr, r11
    rsbmi   r4, r4, #0
    ldr     r6, [r12, #P_HALF]
    ldr     r7, [r12, #P_FAR]
    asr     r5, r5, #2
    cmp     r4, r6
    addhi   r7, r7, #1
    movls   r7, #0
    cmp     r7, #4
    blo     3f
    ldr     r8, [r12, #P_FLIPS]
    add     r5, r5, r6
    mov     r7, #0
    add     r8, r8, #1
    str     r8, [r12, #P_FLIPS]
3:
    ldr     r4, [r12, #P_ROOM]
    str     r7, [r12, #P_FAR]
    sub     r1, r1, r5
    sub     r4, r4, r5
    str     r4, [r12, #P_ROOM]
2:
    ldr     r4, [r12, #P_ROOM]
    ldr     r5, [r12, #P_GROUP]
    cmp     r4, r5
    bge     1b
    str     r1, [r12, #P_QI]
    pop     {r0, r4-r11, pc}
    .size   adat_decode_groups, . - adat_decode_groups
