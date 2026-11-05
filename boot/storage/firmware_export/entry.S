.arch armv8-a
.section .text.entry,"ax",%progbits
.global _start
.type _start,%function
_start:
    // U-Boot go ABI. x18 is its global-data pointer. Preserve its stack and
    // link register in this image, then run C on our own 16KiB stack.
    adr x9, saved_caller
    mov x10, sp
    stp x10, x30, [x9]
    str x18, [x9, #16]
    adr x10, stack_top
    mov sp, x10
    mov x0, x18
    bl iosw_fw_native
    adr x9, saved_caller
    ldp x10, x30, [x9]
    ldr x18, [x9, #16]
    mov sp, x10
    ret
.size _start,.-_start

.section .private,"aw",%progbits
.balign 16
saved_caller:
    .zero 32
.balign 16
stack_bottom:
    .zero 16384
stack_top:
