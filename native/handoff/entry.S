#include "record.h"

.section .text.entry, "ax"
.global _start
_start:
    // Preserve the handoff arguments and original PSTATE/SP before using them.
    mov x20, x0
    mov x21, x1
    mov x22, x2
    mov x23, x3
    mrs x24, daif
    mov x28, sp
    msr daifset, #15
    mrs x25, CurrentEL
    cmp x25, #8
    b.ne probe_halt
    mrs x26, sctlr_el2
    // No framebuffer or C code if cleanup did not disable M/C/I.
    mov x27, #0x1005
    tst x26, x27
    b.ne probe_halt
    msr spsel, #1
    adr x0, probe_stack_top
    mov sp, x0
    adr x19, probe_record
    stp x20, x21, [x19, #OFF(R_FDT)]
    stp x22, x23, [x19, #OFF(R_X2)]
    stp x25, x24, [x19, #OFF(R_ENTRY_EL)]
    stp x28, x26, [x19, #OFF(R_ENTRY_SP)]
    adr x0, probe_vectors
    mrs x1, tcr_el2
    mrs x2, ttbr0_el2
    mrs x3, vbar_el2
    msr vbar_el2, x0
    mrs x4, hcr_el2
    str x4, [x19, #OFF(R_HCR)]
    mrs x4, cnthctl_el2
    str x4, [x19, #OFF(R_CNTHCTL)]
    mrs x4, cntvoff_el2
    str x4, [x19, #OFF(R_CNTVOFF)]
    mrs x4, cptr_el2
    str x4, [x19, #OFF(R_CPTR)]
    mrs x4, hstr_el2
    str x4, [x19, #OFF(R_HSTR)]
    str x1, [x19, #OFF(R_ENTRY_TCR)]
    str x2, [x19, #OFF(R_ENTRY_TTBR0)]
    str x3, [x19, #OFF(R_ENTRY_VBAR)]
    isb
    mrs x0, midr_el1
    str x0, [x19, #OFF(R_MIDR)]
    mrs x0, cntfrq_el0
    str x0, [x19, #OFF(R_CNTFRQ)]
    isb
    mrs x0, cntpct_el0
    str x0, [x19, #OFF(R_COUNTER_PRE)]
    mov x0, #1
    str x0, [x19, #OFF(R_STAGE)]
    bl probe_before_transition
    cbz w0, probe_halt

    // Armv8.0 EL1 setup follows this firmware's armv8_switch_to_el1_m
    // contract. Stage 2, virtualization traps and interrupt rerouting stay off.
    // All exceptions remain masked. No GIC or timer interrupt is enabled.
    mov x0, #3
    str x0, [x19, #OFF(R_STAGE)]
    msr cnthctl_el2, x0
    msr cntvoff_el2, xzr
    mrs x0, midr_el1
    msr vpidr_el2, x0
    mrs x0, mpidr_el1
    msr vmpidr_el2, x0
    mov x0, #0x33ff
    msr cptr_el2, x0
    msr hstr_el2, xzr
    // The diagnostic uses integer registers only; FP/SIMD stays disabled.
    msr cpacr_el1, xzr
    ldr x0, =0x30d00800
    msr sctlr_el1, x0
    msr ttbr0_el1, xzr
    msr ttbr1_el1, xzr
    msr tcr_el1, xzr
    msr mair_el1, xzr
    msr contextidr_el1, xzr
    msr cntkctl_el1, xzr
    adr x0, probe_stack_top
    msr sp_el1, x0
    adr x0, probe_vectors
    msr vbar_el1, x0
    // Cortex-A57 implements EL3, so HCR.HCD is RES0. Only RW is set;
    // stage 2, interrupt routing and general traps remain disabled.
    ldr x0, =0x80000000
    msr hcr_el2, x0
    isb
    dsb sy
    tlbi vmalle1
    dsb sy
    isb
    mrs x0, hcr_el2
    str x0, [x19, #OFF(R_HCR_AFTER)]
    mrs x0, cnthctl_el2
    str x0, [x19, #OFF(R_CNTHCTL_AFTER)]
    mrs x0, cntvoff_el2
    str x0, [x19, #OFF(R_CNTVOFF_AFTER)]
    mov x0, #0x3c5  // D/A/I/F masked, AArch64 EL1h.
    msr spsr_el2, x0
    adr x0, probe_el1
    msr elr_el2, x0
    dsb sy
    isb
    eret

probe_el1:
    adr x19, probe_record
    mrs x0, CurrentEL
    str x0, [x19, #OFF(R_EL1_EL)]
    mrs x0, sctlr_el1
    str x0, [x19, #OFF(R_EL1_SCTLR)]
    mrs x0, daif
    str x0, [x19, #OFF(R_EL1_DAIF)]
    mov x0, sp
    str x0, [x19, #OFF(R_EL1_SP)]
    mrs x0, vbar_el1
    str x0, [x19, #OFF(R_EL1_VBAR)]
    mrs x0, cntfrq_el0
    str x0, [x19, #OFF(R_EL1_CNTFRQ)]
    isb
    mrs x0, cntpct_el0
    str x0, [x19, #OFF(R_EL1_COUNTER)]
    bl probe_after_transition

.global probe_halt
probe_halt:
    msr daifset, #15
5:  wfe
    b 5b

.section .vectors, "ax"
.balign 2048
.global probe_vectors
probe_vectors:
.rept 16
    b probe_exception
    .balign 128
.endr

.section .text, "ax"
probe_exception:
    msr daifset, #15
    adr x19, probe_record
    ldr x0, [x19, #OFF(R_EXCEPTION)]
    cbnz x0, probe_halt
    mov x0, #1
    str x0, [x19, #OFF(R_EXCEPTION)]
    msr spsel, #1
    adr x0, probe_fault_stack_top
    mov sp, x0
    mrs x0, CurrentEL
    cmp x0, #8
    b.eq 6f
    mrs x1, esr_el1
    mrs x2, far_el1
    mrs x3, elr_el1
    b 7f
6:  mrs x1, esr_el2
    mrs x2, far_el2
    mrs x3, elr_el2
7:  str x0, [x19, #OFF(R_FAIL_EL)]
    str x1, [x19, #OFF(R_ESR)]
    str x2, [x19, #OFF(R_FAR)]
    str x3, [x19, #OFF(R_ELR)]
    bl probe_show_exception
    b probe_halt

.section .data, "aw"
.balign 16
.global probe_record
probe_record:
    .zero OFF(R_RECORD_WORDS)

// File-backed zero-fill avoids relying on a firmware BSS initialization ABI.
#ifndef PROBE_STACK_BYTES
#define PROBE_STACK_BYTES 0x10000
#endif
.section .stacks, "aw", @progbits
.balign 16
.global probe_stack_bottom
probe_stack_bottom:
    .zero PROBE_STACK_BYTES
.global probe_stack_top
probe_stack_top:
    .zero 0x1000
probe_fault_stack_top:
