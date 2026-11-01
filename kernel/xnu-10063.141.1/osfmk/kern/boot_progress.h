// SPDX-License-Identifier: MIT
#ifndef XNU_BOOT_PROGRESS_H
#define XNU_BOOT_PROGRESS_H
#include <stdint.h>
#if defined(XNU_BOOT_PROGRESS) && XNU_BOOT_PROGRESS
void boot_progress_init(void);
void boot_progress_start(void);
int boot_progress_active(void);
void boot_progress_bind(uintptr_t, unsigned, unsigned, unsigned, unsigned);
void boot_progress_putc(char);
void boot_progress_stage(const char *);
void boot_progress_irq(uint64_t, uint64_t, uint64_t);
void boot_progress_poll(void);
void boot_progress_user_breakpoint(void *, unsigned);
void boot_progress_user_abort(void *, unsigned, uint64_t);
void boot_progress_critical_exit(int, char *, unsigned long);
void boot_progress_process_stage(int, const char *, const char *);
void boot_progress_exit(int, const char *, int, unsigned, uint64_t);
#else
#define boot_progress_init() ((void)0)
#define boot_progress_start() ((void)0)
#define boot_progress_active() 0
#define boot_progress_bind(...) ((void)0)
static inline void boot_progress_putc(char c) { (void)c; }
#define boot_progress_stage(s) ((void)0)
#define boot_progress_irq(...) ((void)0)
#define boot_progress_poll() ((void)0)
#define boot_progress_user_breakpoint(...) ((void)0)
#define boot_progress_user_abort(...) ((void)0)
#define boot_progress_critical_exit(...) ((void)0)
#define boot_progress_process_stage(...) ((void)0)
#define boot_progress_exit(...) ((void)0)
#endif
#endif
