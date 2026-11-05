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

    /* U-Boot's clean/disable handoff remains a precondition. Invalidate the
     * adapter by VA (identity addresses, translation off) before populating
     * its tables, so the set/way pass below has no line of it to write back
     * over them. */
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
    /* The staged file and the window (89 MB: 1.4 million lines by VA, ~200 ms
     * measured on r7n) are cleaned and invalidated by set/way instead: every
     * L1/L2 data line, ~33K operations. Clean-and-invalidate, never
     * invalidate-only, so no dirty line anywhere (ours, U-Boot's, or the
     * secure world's in the shared L2) is ever dropped; with U-Boot's
     * disable-then-flush handoff there are none, and nothing could allocate
     * since (SCTLR.C clear). One core runs. The adapter was invalidated by VA
     * above, before its tables were written, so no line of it is left. */
    bl loader_dcache_cisw_all
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

    // Preparation is complete. The callback records counter time before
    // any payload load/store starts.
    mov x0, #0
    ldr x1, =LOAD_BYTES
    mov x2, x28
    blr x29
    // The staged file is the window's nonzero extents, LZ4-compressed in blocks:
    // copy.c decodes them into the window and zeroes the rest, in this regime.
    mov x0, x29
    mov x1, x28
    bl loader_copy_payload
    mov x24, #0x70a
    cmp w0, #0
    csel x24, xzr, x24, eq
    b .Lclean

.Lenable_fail:
    mov x24, #0x707
.Lclean:
    /* From here until every line is clean there are no loads, stores,
     * callbacks or stack accesses. M/C/I go off first, so nothing can
     * allocate behind the pass; then every dirty line (the window, the
     * adapter's data, stack and tables) reaches PoC and every line is
     * invalidated by set/way, ~33K operations instead of 1.15 million by VA
     * over the window. The next kernel starts from empty caches. */
    dsb sy
    msr sctlr_el1, x19
    isb
    bl loader_dcache_cisw_all
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

/* Clean and invalidate every data cache level to the Level of Coherence by
 * set/way (CLIDR/CCSIDR walk, the Arm ARM's example sequence). Registers
 * x0-x11 only: no stack and no memory access, so it can run between turning
 * the caches off and their contents reaching memory. */
.balign 16
.global loader_dcache_cisw_all
loader_dcache_cisw_all:
    mrs x0, clidr_el1
    ubfx x3, x0, #24, #3        /* LoC */
    lsl x3, x3, #1              /* LoC * 2, the CSSELR level field */
    cbz x3, 5f
    mov x10, #0                 /* level * 2 */
1:  add x2, x10, x10, lsr #1    /* level * 3 */
    lsr x1, x0, x2
    and x1, x1, #7              /* cache type at this level */
    cmp x1, #2
    b.lt 4f                     /* no data cache here */
    msr csselr_el1, x10
    isb
    mrs x1, ccsidr_el1
    and x2, x1, #7
    add x2, x2, #4              /* log2(line bytes) */
    ubfx x4, x1, #3, #10        /* ways - 1 */
    clz w5, w4                  /* way field shift */
    ubfx x7, x1, #13, #15       /* sets - 1 */
2:  mov x9, x4
3:  lsl x6, x9, x5
    orr x11, x10, x6
    lsl x6, x7, x2
    orr x11, x11, x6
    dc cisw, x11
    subs x9, x9, #1
    b.ge 3b
    subs x7, x7, #1
    b.ge 2b
4:  add x10, x10, #2
    cmp x3, x10
    b.gt 1b
5:  mov x10, #0
    msr csselr_el1, x10
    dsb sy
    isb
    ret
