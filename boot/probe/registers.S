// SPDX-License-Identifier: MIT
// U-Boot `go` ABI: x0=argc, x1=argv. Position independent AArch64 leaf.
// Only reads system registers and writes the 256-byte result within this file.
// Does not configure devices, change exception level, or make firmware calls.
.arch armv8-a
.section .probe, "awx", %progbits
.global _start
.global probe_result
.type _start, %function
_start:
    adr x9, probe_result
    stp xzr, xzr, [x9]
    mrs x10, CurrentEL
    str x10, [x9, #24]
    cmp x10, #4
    b.eq 1f
    cmp x10, #8
    b.ne 3f
1:
    mov x11, #1
    str x11, [x9, #8]
    mov x11, #256
    str x11, [x9, #16]
    mrs x11, MIDR_EL1
    str x11, [x9, #32]
    mrs x11, MPIDR_EL1
    str x11, [x9, #40]
    mrs x11, ID_AA64PFR0_EL1
    str x11, [x9, #48]
    mrs x11, ID_AA64MMFR0_EL1
    str x11, [x9, #56]
    mrs x11, ID_AA64MMFR1_EL1
    str x11, [x9, #64]
    mrs x11, ID_AA64ISAR0_EL1
    str x11, [x9, #72]
    mrs x11, CNTFRQ_EL0
    str x11, [x9, #80]
    mov x11, sp
    str x11, [x9, #88]
    str x0, [x9, #96]
    str x1, [x9, #104]
    mrs x11, DAIF
    str x11, [x9, #112]
    mrs x11, SCTLR_EL1
    str x11, [x9, #120]
    mrs x11, TCR_EL1
    str x11, [x9, #128]
    mrs x11, TTBR0_EL1
    str x11, [x9, #136]
    mrs x11, TTBR1_EL1
    str x11, [x9, #144]
    mrs x11, VBAR_EL1
    str x11, [x9, #152]
    stp xzr, xzr, [x9, #160]
    stp xzr, xzr, [x9, #176]
    stp xzr, xzr, [x9, #192]
    stp xzr, xzr, [x9, #208]
    stp xzr, xzr, [x9, #224]
    stp xzr, xzr, [x9, #240]
    cmp x10, #8
    b.ne 2f
    mrs x11, SCTLR_EL2
    str x11, [x9, #160]
    mrs x11, TCR_EL2
    str x11, [x9, #168]
    mrs x11, TTBR0_EL2
    str x11, [x9, #176]
    mrs x11, VBAR_EL2
    str x11, [x9, #184]
    mrs x11, HCR_EL2
    str x11, [x9, #192]
    mrs x11, CNTHCTL_EL2
    str x11, [x9, #200]
2:
    // Commit marker last: little-endian ASCII SWCHPRB1.
    movz x11, #0x5753
    movk x11, #0x4843, lsl #16
    movk x11, #0x5250, lsl #32
    movk x11, #0x3142, lsl #48
    str x11, [x9]
    mov x0, #0
    ret
3:
    mov x0, #2
    ret
.size _start, .-_start

.org 0x200
probe_result:
    .zero 256
