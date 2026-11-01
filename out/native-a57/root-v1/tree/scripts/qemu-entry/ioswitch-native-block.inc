// Signed own-process data-mount test after the third syscall completed SD setup.
// Kernel supplies only registry-observed names, through a bounded getpid hook.
.global _block_query_ack, _block_final_ack, _block_checkpoint
  adrp x20, _block_results@PAGE
  add x20, x20, _block_results@PAGEOFF
  mov x8, #1
  str x8, [x20]
  mov x21, #200
Lblock_poll:
  ldr x8, [x20, #32]
  add x8, x8, #1
  str x8, [x20, #32]
  mov x16, #20
  svc #0x80
_block_query_ack:
  adrp x9, _block_query@PAGE
  add x9, x9, _block_query@PAGEOFF
  ldr x8, [x9]
  cbz x8, Lblock_paths_ready
  cmp x8, #1
  b.ne Lblock_query_failed
  subs x21, x21, #1
  b.eq Lblock_query_failed
  // Real Mach thread_switch(MACH_PORT_NULL, SWITCH_OPTION_NONE, 0).
  // Yield runnable matching threads without depending on a timer wakeup.
  ldr x8, [x20, #40]
  add x8, x8, #1
  str x8, [x20, #40]
  mov x0, #0
  mov x1, #0
  mov x2, #0
  mov x16, #-61
  svc #0x80
  str x0, [x20, #48]
  cbnz x0, Lblock_failed // Mach returns kern_return_t directly, not carry errno.
  b Lblock_poll
Lblock_query_failed:
  mov x0, #100
  b Lblock_failed
Lblock_paths_ready:
  mov x8, #2
  str x8, [x20]
  // One per-volume result is16 qwords: mkdir pair, mount pair, open pair,
  // total read, last read pair, EOF pair, close pair, remaining reserved.
  adrp x22, Lblock_os_mount@PAGE
  add x22, x22, Lblock_os_mount@PAGEOFF
  adrp x23, Lblock_os_args@PAGE
  add x23, x23, Lblock_os_args@PAGEOFF
  adrp x24, Lblock_os_file@PAGE
  add x24, x24, Lblock_os_file@PAGEOFF
  adrp x25, _block_os_bytes@PAGE
  add x25, x25, _block_os_bytes@PAGEOFF
  mov x26, #574
  add x28, x20, #64
  bl Lblock_volume
  ldr x8, [x20, #8]
  cbnz x8, Lblock_finish
  mov x8, #3
  str x8, [x20]
  adrp x22, Lblock_system_mount@PAGE
  add x22, x22, Lblock_system_mount@PAGEOFF
  adrp x23, Lblock_system_args@PAGE
  add x23, x23, Lblock_system_args@PAGEOFF
  adrp x24, Lblock_system_file@PAGE
  add x24, x24, Lblock_system_file@PAGEOFF
  adrp x25, _block_system_bytes@PAGE
  add x25, x25, _block_system_bytes@PAGEOFF
  mov x26, #49152
  add x28, x20, #192
  bl Lblock_volume
  ldr x8, [x20, #8]
  cbnz x8, Lblock_finish
  mov x8, #4
  str x8, [x20]
  b Lblock_finish
Lblock_failed:
  // Positive errno or explicit fixture error; no success-path substitution.
  str x0, [x20, #8]
  mrs x9, nzcv
  str x9, [x20, #24]
Lblock_finish:
  mov x16, #20
  svc #0x80
_block_final_ack:
_block_checkpoint:
  b _block_checkpoint

Lblock_volume:
  // The only called helper; Darwin syscalls preserve non-result registers.
  mov x0, x22
  mov x1, #0755
  mov x16, #136
  svc #0x80
  str x0, [x28]
  mrs x9, nzcv
  str x9, [x28, #8]
  b.cs Lblock_volume_failed
  adrp x0, Lblock_apfs@PAGE
  add x0, x0, Lblock_apfs@PAGEOFF
  mov x1, x22
  mov x2, #1 // MNT_RDONLY
  mov x3, x23
  mov x16, #167
  svc #0x80
  str x0, [x28, #16]
  mrs x9, nzcv
  str x9, [x28, #24]
  b.cs Lblock_volume_failed
  mov x0, x24
  mov x1, #0 // O_RDONLY
  mov x2, #0
  mov x16, #5
  svc #0x80
  str x0, [x28, #32]
  mrs x9, nzcv
  str x9, [x28, #40]
  b.cs Lblock_volume_failed
  mov x27, #0
Lblock_read:
  ldr x0, [x28, #32]
  add x1, x25, x27
  sub x2, x26, x27
  mov x16, #3
  svc #0x80
  str x0, [x28, #56]
  mrs x9, nzcv
  str x9, [x28, #64]
  b.cs Lblock_volume_failed
  cbz x0, Lblock_short_read
  sub x8, x26, x27
  cmp x0, x8
  b.hi Lblock_short_read
  add x27, x27, x0
  str x27, [x28, #48]
  cmp x27, x26
  b.lo Lblock_read
  // Verify EOF after the complete expected file, using a separate byte.
  ldr x0, [x28, #32]
  adrp x1, Lblock_eof@PAGE
  add x1, x1, Lblock_eof@PAGEOFF
  mov x2, #1
  mov x16, #3
  svc #0x80
  str x0, [x28, #72]
  mrs x9, nzcv
  str x9, [x28, #80]
  b.cs Lblock_volume_failed
  cbnz x0, Lblock_short_read
  ldr x0, [x28, #32]
  mov x16, #6
  svc #0x80
  str x0, [x28, #88]
  mrs x9, nzcv
  str x9, [x28, #96]
  b.cs Lblock_volume_failed
  ret
Lblock_short_read:
  mov x0, #101
Lblock_volume_failed:
  str x0, [x20, #8]
  mrs x9, nzcv
  str x9, [x20, #24]
  ret
.data
.balign 8
.global _block_results, _block_query, _block_os_bytes, _block_system_bytes
_block_results:
  .fill 40, 8, 0
_block_query:
  .quad 1
Lblock_os_device:
  .fill 64, 1, 0
Lblock_system_device:
  .fill 64, 1, 0
Lblock_eof:
  .quad 0
Lblock_apfs:
  .asciz "apfs"
Lblock_os_mount:
  .asciz "/dev/ioswitch-os"
Lblock_system_mount:
  .asciz "/dev/ioswitch-system"
Lblock_os_file:
  .asciz "/dev/ioswitch-os/System/Library/CoreServices/SystemVersion.plist"
Lblock_system_file:
  .asciz "/dev/ioswitch-system/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64.02"
.balign 8
Lblock_os_args:
  .quad Lblock_os_device, 1
  .short 1 // Ordinary data-mount mode, never root mode0.
  .fill 0x144 - 18, 1, 0
  .word 99, 99, 0
Lblock_system_args:
  .quad Lblock_system_device, 1
  .short 1
  .fill 0x144 - 18, 1, 0
  .word 99, 99, 0
.balign 16
_block_os_bytes:
  .fill 574, 1, 0
.balign 16
_block_system_bytes:
  .fill 49152, 1, 0
.text
