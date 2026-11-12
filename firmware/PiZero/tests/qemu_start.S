// Entry for running a host test as AArch32 code under QEMU: newlib's start-up needs the FPU on first.
// See spdif_decode_a32_test.c for the build and run lines.

    .syntax unified
    .arm
    .text
    .global qemu_start
qemu_start:
    mrc     p15, 0, r0, c1, c0, 2       // CPACR: full access to cp10 and cp11
    orr     r0, r0, #0xF00000
    mcr     p15, 0, r0, c1, c0, 2
    isb
    mov     r0, #0x40000000             // FPEXC.EN
    vmsr    fpexc, r0
    b       _mainCRTStartup
