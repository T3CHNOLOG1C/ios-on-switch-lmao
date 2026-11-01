#ifndef IOSWITCH_XNU_DIAG_H
#define IOSWITCH_XNU_DIAG_H
#include <stdarg.h>
#include <stdint.h>
struct boot_args;
#if defined(XNU_IOSWITCH_VERIFIED_ROOT) && XNU_IOSWITCH_VERIFIED_ROOT
void ioswitch_root_event(unsigned stage, int error, uint64_t detail);
void ioswitch_root_data_ownership(uint64_t flags, uint64_t root, uint64_t mobile);
void ioswitch_root_boot_prepare(void);
#endif
#if defined(IOSWITCH_XNU_DIAGNOSTICS) && IOSWITCH_XNU_DIAGNOSTICS
void ioswitch_diag_early_stage(unsigned stage, const char *message);
void ioswitch_diag_early_leave(void);
void ioswitch_diag_early_fault(void) __attribute__((noreturn));
void ioswitch_diag_user_syscall(unsigned code, int error, int pid, uint64_t pc, uint64_t cpsr, int result);
void ioswitch_diag_init(const struct boot_args *args);
void ioswitch_diag_stage(const char *message);
void ioswitch_diag_panic(const char *format, va_list *args, unsigned long caller);
int ioswitch_diag_console_active(void);
int ioswitch_native_text_console_active(void);
void ioswitch_diag_console_putc(char c);
void ioswitch_diag_process_event(int pid, const char *name, const char *event, int status);
void ioswitch_diag_graphics_takeover(void);
#else
#define ioswitch_diag_early_stage(stage, message) ((void)0)
#define ioswitch_diag_early_leave() ((void)0)
#define ioswitch_diag_user_syscall(code, error, pid, pc, cpsr, result) ((void)0)
#define ioswitch_diag_init(args) ((void)0)
#define ioswitch_diag_stage(message) ((void)0)
#define ioswitch_diag_panic(format, args, caller) ((void)0)
#define ioswitch_diag_console_active() 0
#define ioswitch_native_text_console_active() 0
#define ioswitch_diag_console_putc(c) ((void)0)
#define ioswitch_diag_process_event(...) ((void)0)
#define ioswitch_diag_graphics_takeover() ((void)0)
#endif
#endif
