// SPDX-License-Identifier: BSD-2-Clause-Patent
// The panic screen and the persistent panic record (IOSwitchPanicSave.c). Hooks, in the order they run:
//   arm_init.c, after ioswitch_diag_init       ioswitch_panic_save_init
//   pe_init.c check_for_panic_log (no /pram)   ioswitch_panic_save_xnu_buffer
//   IOSwitchDiag.c ioswitch_diag_panic         ioswitch_panic_begin     (panic entry, other CPUs still running)
//   debug.c handle_debugger_trap               ioswitch_panic_halted    (other CPUs halted)
//   debug.c, after SavePanicInfo               ioswitch_panic_logged    (XNU's panic log written; may not return)
//   kern_exec.c, once Data is mounted          ioswitch_panic_save_pending, ioswitch_panic_save_done
#ifndef IOSWITCH_PANIC_SAVE_H
#define IOSWITCH_PANIC_SAVE_H
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#if defined(IOSWITCH_XNU_DIAGNOSTICS) && IOSWITCH_XNU_DIAGNOSTICS
void ioswitch_panic_save_init(void);
int ioswitch_panic_save_xnu_buffer(uintptr_t *base, unsigned int *bytes);
int ioswitch_panic_begin(const char *format, va_list *args, unsigned long caller, int take_screen);
void ioswitch_panic_halted(void);
void ioswitch_panic_logged(void);
const char *ioswitch_panic_save_pending(uint32_t *bytes, char *path, size_t path_bytes);
void ioswitch_panic_save_done(int error, const char *path);
#else
#define ioswitch_panic_save_init() ((void)0)
#define ioswitch_panic_save_xnu_buffer(base, bytes) 0
#define ioswitch_panic_begin(format, args, caller, take_screen) 0
#define ioswitch_panic_halted() ((void)0)
#define ioswitch_panic_logged() ((void)0)
#define ioswitch_panic_save_pending(bytes, path, path_bytes) ((const char *)0)
#define ioswitch_panic_save_done(error, path) ((void)0)
#endif
#endif
