#ifndef SWITCH_XNU_DIAG_H
#define SWITCH_XNU_DIAG_H
#include <stdarg.h>
#include <stdint.h>
struct boot_args;
#if defined(XNU_SWITCH_VERIFIED_ROOT) && XNU_SWITCH_VERIFIED_ROOT
void switch_root_event(unsigned stage, int error, uint64_t detail);
void switch_root_data_ownership(uint64_t flags, uint64_t root, uint64_t mobile);
void switch_root_boot_prepare(void);
#endif
#if defined(SWITCH_XNU_DIAGNOSTICS) && SWITCH_XNU_DIAGNOSTICS
void switch_diag_early_stage(unsigned stage, const char *message);
void switch_diag_early_leave(void);
void switch_diag_early_fault(void) __attribute__((noreturn));
void switch_diag_user_syscall(unsigned code, int error, int pid, uint64_t pc, uint64_t cpsr, int result);
void switch_diag_init(const struct boot_args *args);
void switch_diag_stage(const char *message);
void switch_diag_panic(const char *format, va_list *args, unsigned long caller);
/* The display's panic hook (NativeLegacyDisplay), in every kernel config: PE_arm_debug_panic_hook is a const NULL outside
 * DEVELOPMENT and DEBUG. Called first thing in switch_diag_panic, before anything draws the panic screen, and again from
 * switch_panic_halted once the other CPUs are halted. Interrupts are off and other CPUs may still run: stores only. */
extern void (*volatile switch_display_panic_hook)(const char *message);
int switch_diag_console_active(void);
int switch_native_text_console_active(void);
void switch_diag_console_putc(char c);
void switch_diag_process_event(int pid, const char *name, const char *event, int status);
void switch_diag_graphics_takeover(void);
#else
#define switch_diag_early_stage(stage, message)                      ((void)0)
#define switch_diag_early_leave()                                    ((void)0)
#define switch_diag_user_syscall(code, error, pid, pc, cpsr, result) ((void)0)
#define switch_diag_init(args)                                       ((void)0)
#define switch_diag_stage(message)                                   ((void)0)
#define switch_diag_panic(format, args, caller)                      ((void)0)
#define switch_diag_console_active()                                 0
#define switch_native_text_console_active()                          0
#define switch_diag_console_putc(c)                                  ((void)0)
#define switch_diag_process_event(...)                               ((void)0)
#define switch_diag_graphics_takeover()                              ((void)0)
#endif
#endif
