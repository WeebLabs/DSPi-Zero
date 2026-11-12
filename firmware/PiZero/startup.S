/* Entry point. The GPU firmware loads kernel.img at 0x8000 and jumps here with the MMU and caches off,
 * in SVC mode on the BCM2835 and in HYP mode on core 0 of the BCM2837 (cores 1-3 stay parked).
 * Adapted from broadcom-peripherals boot.s; see Documentation/Porting/PiZero_Z0_bringup.md. */

    .syntax unified
    .arm
#if __ARM_ARCH >= 7
    .arch_extension virt
#define ISB isb
#else
#define ISB mcr p15, 0, r0, c7, c5, 4   /* ARMv6 prefetch flush; r0 must be 0 */
#endif

    .section .text.boot, "ax"
    .globl _start
_start:
    cpsid   aif
    /* Keep the firmware's r0-r2 and our real load address for the boot report in main. */
    mov     r8, r0
    mov     r9, r1
    mov     r10, r2
    adr     r11, _start

#if BCM_VERSION == 2837
    /* A reboot meant for the USB loader leaves a marker (LOADER_HANDOFF_* in loader/loader_proto.h). An image
     * the loader started (r3 = LOADER_LAUNCH_MAGIC) has been through the loader, so it clears any marker and
     * skips the flicker below. Otherwise a marker means: clear it and start the loader the firmware placed at
     * 0x2000000, with the firmware's r0-r2. The MMU and caches are off, so this is RAM itself. */
    ldr     r4, =0x01F00000
    mov     r5, #0
    ldr     r0, =0x4C4F4144
    cmp     r3, r0
    streq   r5, [r4]
    streq   r5, [r4, #4]
    beq     6f
    ldr     r6, [r4]
    ldr     r7, =0x48414E44
    cmp     r6, r7
    ldreq   r6, [r4, #4]
    mvneq   r7, r7
    cmpeq   r6, r7
    bne     5f
    str     r5, [r4]
    str     r5, [r4, #4]
    dsb
    mov     r0, r8
    mov     r1, r9
    mov     r2, r10
    mov     r3, #0
    ldr     r4, =0x02000000
    bx      r4
5:
    /* Sign of life without a UART: 4 s of fast LED flicker (GPIO 29) before anything can hang, ending
     * with the pin high (LED off if active low), unlike the firmware's solid on. Uses r0-r7 only. */
    ldr     r0, =0x3F200000             /* GPIO */
    ldr     r1, [r0, #0x08]             /* GPFSEL2: GPIO 29 is bits 29:27 */
    bic     r1, r1, #(7 << 27)
    orr     r1, r1, #(1 << 27)
    str     r1, [r0, #0x08]
    ldr     r3, =0x3F003004             /* system timer CLO */
    ldr     r7, =40000                  /* 40 ms per half period */
    mov     r1, #(1 << 29)
    mov     r4, #100
7:  tst     r4, #1
    streq   r1, [r0, #0x28]             /* GPCLR0 */
    strne   r1, [r0, #0x1C]             /* GPSET0, last on the final pass */
    ldr     r5, [r3]
8:  ldr     r6, [r3]
    sub     r6, r6, r5
    cmp     r6, r7
    blo     8b
    subs    r4, r4, #1
    bne     7b
6:
#endif

#if __ARM_ARCH >= 7
    /* CPS cannot leave HYP mode, so drop to SVC with an exception return. First stop HYP trapping
     * coprocessor and CP15 accesses, since an FPU trap would land at an HYP vector we never set. */
    mrs     r0, cpsr
    and     r1, r0, #0x1F
    cmp     r1, #0x1A
    bne     9f
    ldr     r1, =0x33FF                 /* HCPTR: only the RES1 bits, so no CP10/CP11 traps */
    mcr     p15, 4, r1, c1, c1, 2
    mov     r1, #0
    mcr     p15, 4, r1, c1, c1, 3       /* HSTR: no CP15 traps */
    mcr     p15, 4, r1, c1, c1, 0       /* HCR: no virtualisation traps or routing */
    mrc     p15, 0, r1, c9, c12, 0      /* PMCR */
    ubfx    r1, r1, #11, #5             /* PMCR.N, the number of event counters */
    mcr     p15, 4, r1, c1, c1, 1       /* HDCR: SVC owns every PMU counter, no PMU or debug traps */
    isb
    bic     r0, r0, #0x1F
    orr     r0, r0, #0x13               /* SVC */
    orr     r0, r0, #0x1C0              /* A, I and F masked */
    msr     spsr_hyp, r0
    adr     r1, 9f
    msr     elr_hyp, r1
    eret
9:
#endif

    /* One stack per exception mode. CPS takes the mode number directly. */
    cps     #0x11               /* FIQ */
    ldr     sp, =__fiq_stack_top
    cps     #0x12               /* IRQ */
    ldr     sp, =__irq_stack_top
    cps     #0x17               /* Abort */
    ldr     sp, =__abt_stack_top
    cps     #0x1B               /* Undefined */
    ldr     sp, =__und_stack_top
    cps     #0x1F               /* System: main runs here */
    ldr     sp, =__sys_stack_top

    ldr     r0, =__vectors
    mcr     p15, 0, r0, c12, c0, 0      /* VBAR */

    /* I-cache and branch prediction are safe with the MMU off. Without them every
     * instruction is fetched from uncached SDRAM. The D-cache waits for the MMU in Z1. */
    mov     r0, #0
    mcr     p15, 0, r0, c7, c5, 0       /* invalidate I-cache */
    mcr     p15, 0, r0, c7, c5, 6       /* flush branch target cache */
    mrc     p15, 0, r0, c1, c0, 0       /* SCTLR */
    orr     r0, r0, #(1 << 12)          /* I */
    orr     r0, r0, #(1 << 11)          /* Z */
    mcr     p15, 0, r0, c1, c0, 0
    mov     r0, #0
    ISB

    /* RunFast mode (FZ + DN, no traps). The VFP11 would otherwise bounce denormals and some
     * underflows to support code we do not have. The A53 handles them, but FZ keeps DSP timing flat. */
    mrc     p15, 0, r0, c1, c0, 2       /* CPACR */
    orr     r0, r0, #(0xF << 20)        /* cp10 and cp11 full access */
    mcr     p15, 0, r0, c1, c0, 2
    mov     r0, #0
    ISB
    mov     r0, #0x40000000             /* FPEXC.EN */
    vmsr    fpexc, r0
    mov     r0, #0x03000000             /* FPSCR.FZ | FPSCR.DN */
    vmsr    fpscr, r0

    ldr     r0, =__bss_start
    ldr     r1, =__bss_end
    mov     r2, #0
1:  cmp     r0, r1
    strlo   r2, [r0], #4
    blo     1b

    ldr     r0, =boot_info
    stmia   r0, {r8-r11}

    bl      main
2:  wfi
    b       2b

    .section .bss.boot_info, "aw", %nobits
    .balign 4
    .globl boot_info
boot_info:
    .space  16
    .globl irq_return_pc
irq_return_pc:
    .space  4

/* VBAR needs 32-byte alignment. */
    .section .text.vectors, "ax"
    .balign 32
    .globl __vectors
__vectors:
    ldr     pc, =_start
    ldr     pc, =undef_entry
    ldr     pc, =swi_entry
    ldr     pc, =pabt_entry
    ldr     pc, =dabt_entry
    ldr     pc, =unused_entry
    ldr     pc, =irq_entry
    ldr     pc, =fiq_entry
    .ltorg

/* Every exception reports its type and return address to fault_handler, which never returns. */
    .macro fault_stub name, code, adjust
\name:
    sub     r1, lr, #\adjust
    mov     r0, #\code
    b       fault_common
    .endm

    fault_stub undef_entry,  1, 4
    fault_stub swi_entry,    2, 4
    fault_stub pabt_entry,   3, 4
    fault_stub dabt_entry,   4, 8
    fault_stub unused_entry, 5, 4

/* IRQ: saves what a C call may clobber, including the VFP caller-saved registers and FPSCR, because the
 * interrupted code may be in the middle of floating-point DSP. irq_handler() is weak in board.c, where it
 * reports a fault, so images that never enable interrupts behave as before. */
irq_entry:
    sub     lr, lr, #4
    srsdb   sp!, #0x12                  /* lr_irq and spsr_irq onto the IRQ stack */
    push    {r0-r3, r12, lr}            /* 8 + 24 bytes keeps the stack 8-byte aligned */
    ldr     r1, =irq_return_pc          /* the interrupted PC, for the sampling profiler */
    str     lr, [r1]
    vmrs    r0, fpscr
    push    {r0, r1}
    vpush   {d0-d7}
#if __ARM_ARCH >= 7
    vpush   {d16-d31}                   /* VFPv4/NEON has 32 D registers; the VFP11 has 16 */
#endif
    bl      irq_handler
#if __ARM_ARCH >= 7
    vpop    {d16-d31}
#endif
    vpop    {d0-d7}
    pop     {r0, r1}
    vmsr    fpscr, r0
    pop     {r0-r3, r12, lr}
    rfeia   sp!
    fault_stub fiq_entry,    7, 4

fault_common:
    mrs     r2, spsr
    mrc     p15, 0, r3, c5, c0, 0       /* DFSR */
    bl      fault_handler
3:  b       3b
