// SPDX-License-Identifier: BSD-2-Clause-Patent
// A signed, static Darwin test process. No Apple userspace bytes or MMIO.
.text
.global _start, _checkpoint, _after_getpid, _after_sysctl
_start:
  mov x16, #20
  svc #0x80
_after_getpid:
  adrp x19, _results@PAGE
  add x19, x19, _results@PAGEOFF
  str x0, [x19]
  mrs x9, nzcv
  str x9, [x19, #16]
  adrp x0, _mib@PAGE
  add x0, x0, _mib@PAGEOFF
  mov x1, #2
  adrp x2, _user_page_size@PAGE
  add x2, x2, _user_page_size@PAGEOFF
  adrp x3, _old_length@PAGE
  add x3, x3, _old_length@PAGEOFF
  mov x4, #0
  mov x5, #0
  mov x16, #202
  svc #0x80
_after_sysctl:
  str x0, [x19, #8]
  mrs x9, nzcv
  str x9, [x19, #24]
#ifdef NATIVE_FP_CONTEXT_PROBE
#include "native-fp-context.inc"
#endif
#ifdef NATIVE_APFS_LAUNCHD_PROBE
#include "native-apfs-launchd.inc"
#endif
#ifdef NATIVE_APFS_FILE_PROBE
#include "native-apfs-mount.inc"
#endif
#ifdef NATIVE_CACHE_LEAF_PROBE
#include "native-cache-leaf.inc"
#endif
#ifdef NATIVE_FILE_PROBE
#include "native-file-probe.inc"
#endif
  // Third real EL0 trap acknowledges that userspace stored both results.
  mov x16, #20
  svc #0x80
_checkpoint:
#ifdef IOSW_NATIVE_BLOCK_PROBE
#include "ioswitch-native-block.inc"
#else
  b _checkpoint
#endif
.data
.balign 8
.global _results, _user_page_size, _old_length
_results:
  .quad 0, 0, 0, 0
_mib:
  .word 6, 7 // CTL_HW, HW_PAGESIZE from Darwin 23 sys/sysctl.h
_user_page_size:
  .quad 0
_old_length:
  .quad 8
