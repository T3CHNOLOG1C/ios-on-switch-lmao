/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Boundary between the Linux-flavoured C world (vendored L4T DFLL code + shim/linux.c + platform.c + dfll.c) and XNU.
 * Plain C with fixed-width types only, so XNU translation units never see Linux names and the Linux units never see
 * XNU headers (the TegraGM20B nvxnu.h pattern). Implemented in shim/xnu_glue.cpp.
 */
#ifndef DFLLXNU_H
#define DFLLXNU_H
#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long long dfllxnu_u64;
typedef unsigned int dfllxnu_u32;

/* kernel log, one call per line; callers add the "TegraDFLL: " prefix */
void dfllxnu_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void dfllxnu_vlog(const char *fmt, __builtin_va_list ap);

/* zeroed heap; the glue tracks the size, so free needs none */
void *dfllxnu_malloc(unsigned long size);
void dfllxnu_free(void *p);

/* local interrupt mask (spin_lock_irqsave); any context */
unsigned long dfllxnu_irq_save(void);
void dfllxnu_irq_restore(unsigned long flags);

/* time: udelay busy-waits (any context, interrupts may be off); msleep blocks (thread context only) */
void dfllxnu_udelay(dfllxnu_u32 us);
void dfllxnu_msleep(dfllxnu_u32 ms);
dfllxnu_u64 dfllxnu_nanotime(void);

/* device memory (ml_io_map), mapped once per physical page and never unmapped; NULL on failure */
volatile void *dfllxnu_ioremap(dfllxnu_u64 pa, dfllxnu_u64 size);

/*
 * One-shot deferred calls on a kernel thread (XNU thread_call), the backing for Linux hrtimer / timer_list. Arming a
 * pending call moves its deadline (hrtimer_start / mod_timer semantics). Arm and cancel are safe with interrupts
 * masked; alloc is thread context only.
 */
void *dfllxnu_call_alloc(void (*fn)(void *), void *arg);
void dfllxnu_call_arm(void *call, dfllxnu_u64 delay_ns);
int dfllxnu_call_cancel(void *call);   /* 1 if it was pending */

/* sleeping lock for the public API (thread context only) */
void *dfllxnu_mutex_alloc(void);
void dfllxnu_mutex_lock(void *m);
void dfllxnu_mutex_unlock(void *m);

#ifdef __cplusplus
}
#endif
#endif
