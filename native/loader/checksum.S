// Standard IEEE CRC32 (not CRC32C). Caller supplies an uncomplemented
// accumulator and checks ID_AA64ISAR0_EL1.CRC32 before calling this routine.
// No cache, MMU or SIMD state is changed. All wide reads are 8-byte aligned.
.arch armv8-a+crc
.text
.global loader_crc32_update
.type loader_crc32_update, %function
loader_crc32_update:
    cbz x2, 4f
1:  tst x1, #7
    b.eq 2f
    ldrb w3, [x1], #1
    crc32b w0, w0, w3
    subs x2, x2, #1
    b.ne 1b
    ret
2:  cmp x2, #8
    b.lo 3f
    ldr x3, [x1], #8
    crc32x w0, w0, x3
    sub x2, x2, #8
    b 2b
3:  cbz x2, 4f
    ldrb w3, [x1], #1
    crc32b w0, w0, w3
    sub x2, x2, #1
    b 3b
4:  ret
.size loader_crc32_update, .-loader_crc32_update
