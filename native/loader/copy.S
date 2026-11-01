#define UINT64_C(value) value
#include "layout.h"
#include "copy.h"

/* Fixed-layout, integer-only temporary EL1 translation regime.
 * x0 = progress callback, x1 = start counter. Returns 0 or a refusal code.
 * Every destination ownership/source/reference gate runs before this call.
 * HCR, DAIF, VBAR, timer controls and CPUECTLR are never modified here. */
.section .text, "ax"
.balign 16
.global loader_copy_run
loader_copy_run:
    stp x19, x20, [sp, #-96]!
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    mov x29, x0
    mov x28, x1
    mrs x19, sctlr_el1
    mrs x20, ttbr0_el1
    mrs x21, ttbr1_el1
    mrs x22, tcr_el1
    mrs x23, mair_el1

    mrs x0, CurrentEL
    cmp x0, #4
    b.ne .Lstate_fail
    mov x0, #0x1005
    tst x19, x0
    b.ne .Lstate_fail
    mrs x0, daif
    and x0, x0, #0x3c0
    cmp x0, #0x3c0
    b.ne .Lstate_fail
    mov x0, sp
    tst x0, #15
    b.ne .Lstate_fail
    adr x1, probe_stack_bottom
    cmp x0, x1
    b.lo .Lstate_fail
    add x0, x0, #96
    adr x1, probe_stack_top
    cmp x0, x1
    b.hi .Lstate_fail
    adr x0, __image_start
    cmp x29, x0
    b.lo .Lstate_fail
    adr x0, __loader_rx_end
    cmp x29, x0
    b.hs .Lstate_fail
    tst x29, #3
    b.ne .Lstate_fail

    mrs x0, midr_el1
    ldr x1, =0xff0ffff0
    and x0, x0, x1
    ldr x1, =0x410fd070
    cmp x0, x1
    b.ne .Lcpu_fail
    /* A57 writes to CPUECTLR may trap unless EL3/EL2 grant permission.
     * Firmware must already have set SMPEN for its own cached execution.
     * Read/require it before ANY maintenance; never attempt a blind write. */
    mrs x0, S3_1_c15_c2_1
    tbz x0, #6, .Lsmp_fail
    mrs x0, id_aa64mmfr0_el1
    ubfx x0, x0, #28, #4
    cbnz x0, .Lgranule_fail
    mrs x0, ctr_el0
    ubfx x0, x0, #16, #4
    cmp x0, #4
    b.ne .Lline_fail

    /* U-Boot's clean/disable handoff remains a precondition. Invalidate
     * retained clean lines only in these owned ranges, never all cache sets.
     * With translation disabled, these VA operations use identity addresses.
     * Invalidate the adapter before populating its tables. */
    adr x0, __image_start
    adr x1, __image_end
.Linitial_adapter:
    dc ivac, x0
    add x0, x0, #64
    cmp x0, x1
    b.lo .Linitial_adapter
    dsb sy
    bl loader_copy_prepare
    cbnz w0, .Ltables_fail
    ldr x0, =LOAD_SOURCE
    ldr x1, =(LOAD_SOURCE + LOAD_BYTES)
.Linitial_source:
    dc ivac, x0
    add x0, x0, #64
    cmp x0, x1
    b.lo .Linitial_source
    ldr x0, =LOAD_DEST
    ldr x1, =(LOAD_DEST + LOAD_BYTES)
.Linitial_destination:
    dc ivac, x0
    add x0, x0, #64
    cmp x0, x1
    b.lo .Linitial_destination
    dsb sy
    ic iallu
    tlbi vmalle1
    dsb sy
    isb

    adr x0, loader_copy_tables
    msr ttbr0_el1, x0
    msr ttbr1_el1, xzr
    ldr x0, =LOADER_COPY_TCR
    msr tcr_el1, x0
    mov x0, #LOADER_COPY_MAIR
    msr mair_el1, x0
    isb
    mrs x0, ttbr0_el1
    adr x1, loader_copy_tables
    cmp x0, x1
    b.ne .Lconfigure_fail
    mrs x0, ttbr1_el1
    cbnz x0, .Lconfigure_fail
    mrs x0, tcr_el1
    ldr x1, =LOADER_COPY_TCR
    cmp x0, x1
    b.ne .Lconfigure_fail
    mrs x0, mair_el1
    mov x1, #LOADER_COPY_MAIR
    cmp x0, x1
    b.ne .Lconfigure_fail
    dsb sy
    mov x0, #0x1005
    orr x0, x19, x0
    msr sctlr_el1, x0
    isb
    mrs x1, sctlr_el1
    cmp x1, x0
    b.ne .Lenable_fail

    // Preparation is complete. The callback changes the displayed stage and
    // records counter time before any payload load/store starts.
    mov x0, #0
    ldr x1, =LOAD_BYTES
    mov x2, x28
    blr x29
    ldr x25, =LOAD_SOURCE
    ldr x26, =LOAD_DEST
    ldr x27, =LOAD_BYTES
.Lchunk:
    mov x24, #0x100000
    cmp x27, x24
    csel x24, x27, x24, lo
    mov x12, x24
.Lcopy:
    ldp x0, x1, [x25]
    ldp x2, x3, [x25, #16]
    ldp x4, x5, [x25, #32]
    ldp x6, x7, [x25, #48]
    stp x0, x1, [x26]
    stp x2, x3, [x26, #16]
    stp x4, x5, [x26, #32]
    stp x6, x7, [x26, #48]
    add x25, x25, #64
    add x26, x26, #64
    subs x12, x12, #64
    b.ne .Lcopy
    sub x27, x27, x24
    ldr x1, =LOAD_BYTES
    sub x0, x1, x27
    mov x2, x28
    blr x29
    cbnz x27, .Lchunk
    mov x24, #0
    b .Lclean

.Lenable_fail:
    mov x24, #0x707
.Lclean:
    /* From here until SCTLR disables M/C/I there are no stores, callbacks
     * or stack accesses. Every dirty destination and adapter/stack/table
     * line reaches PoC; read-only source/code/rodata cannot become dirty.
     * Normal source cache lines may remain clean, as in ordinary firmware
     * cache-disable handoffs; the next kernel owns its cache initialization. */
    ldr x0, =LOAD_DEST
    ldr x1, =(LOAD_DEST + LOAD_BYTES)
.Lclean_destination:
    dc civac, x0
    add x0, x0, #64
    cmp x0, x1
    b.lo .Lclean_destination
    adr x0, __loader_ro_end
    adr x1, __image_end
.Lclean_adapter:
    dc civac, x0
    add x0, x0, #64
    cmp x0, x1
    b.lo .Lclean_adapter
    dsb sy
    msr sctlr_el1, x19
    isb
    dsb sy
    mrs x0, sctlr_el1
    mov x1, #0x1005
    tst x0, x1
    b.ne .Lfatal_restore
    cmp x0, x19
    b.eq .Lrestore_tables
    mov x24, #0x708
.Lrestore_tables:
    tlbi vmalle1
    ic iallu
    dsb sy
    isb
    msr ttbr0_el1, x20
    msr ttbr1_el1, x21
    msr tcr_el1, x22
    msr mair_el1, x23
    isb
    mrs x0, ttbr0_el1
    cmp x0, x20
    b.ne .Lrestore_fail
    mrs x0, ttbr1_el1
    cmp x0, x21
    b.ne .Lrestore_fail
    mrs x0, tcr_el1
    cmp x0, x22
    b.ne .Lrestore_fail
    mrs x0, mair_el1
    cmp x0, x23
    b.ne .Lrestore_fail
    mov x0, x24
    b .Lreturn
.Lrestore_fail:
    mov x0, #0x708
    b .Lreturn
.Lconfigure_fail:
    /* No destination writes or cache enable occurred. Restore the saved
     * translation registers while M remains clear, then return refusal. */
    mov x24, #0x709
    b .Lrestore_tables
.Lfatal_restore:
    /* Never rewrite a live translation regime or enter XNU after a failed
     * disable. Our vectors, fault stack, diagnostic record and framebuffer
     * remain mapped, so the existing refusal path can display and halt. */
    mov x0, #0x708
    bl loader_copy_refuse
    b probe_halt
.Lstate_fail:
    mov x0, #0x701
    b .Lreturn
.Lcpu_fail:
    mov x0, #0x702
    b .Lreturn
.Lsmp_fail:
    mov x0, #0x703
    b .Lreturn
.Lgranule_fail:
    mov x0, #0x704
    b .Lreturn
.Lline_fail:
    mov x0, #0x705
    b .Lreturn
.Ltables_fail:
    mov x0, #0x706
.Lreturn:
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    ldp x19, x20, [sp], #96
    ret
