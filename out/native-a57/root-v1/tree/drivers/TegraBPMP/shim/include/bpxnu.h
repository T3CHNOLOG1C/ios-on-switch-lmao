/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/*
 * Boundary between the Linux-flavoured C side of TegraBPMP (vendored L4T mail.c, mail_t210.c, mailman.c plus
 * shim/linux.c and TegraBPMP.c) and XNU (shim/xnu_glue.cpp). Plain C, fixed-width types only, so neither side
 * sees the other's names (the GM20B port's nvxnu.h is the model).
 */
#ifndef BPXNU_H
#define BPXNU_H
#ifdef __cplusplus
extern "C" {
#endif

void bpxnu_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void bpxnu_warn(const char *file, int line);
void *bpxnu_zalloc(unsigned long size);
void bpxnu_free(void *p);

/* Spin locks (IOSimpleLock): opaque, allocated on first use. The _irqsave pair masks interrupts. */
void *bpxnu_spin_alloc(void);
void bpxnu_spin_free(void *lock);
void bpxnu_spin_lock(void *lock);
void bpxnu_spin_unlock(void *lock);
unsigned long bpxnu_spin_lock_irqsave(void *lock);
void bpxnu_spin_unlock_irqrestore(void *lock, unsigned long flags);
int bpxnu_interrupts_disabled(void);
int bpxnu_cpu_number(void);

/* Time (ns of mach_absolute_time) and sleeping: bpxnu_assert_wait() under the caller's spin lock, unlock,
 * then bpxnu_block(), which returns 1 if the deadline passed (0: woken by bpxnu_wakeup). */
unsigned long long bpxnu_nanotime(void);
void bpxnu_assert_wait(const void *event, unsigned long long deadline_ns);
int bpxnu_block(void);
void bpxnu_wakeup(const void *event);

/* Device memory: an uncached kernel mapping of [pa, pa + size). */
volatile void *bpxnu_ioremap(unsigned long long pa, unsigned long size);

/* Claims the four inbound BPMP doorbells (GIC SPIs) once mail_t210.c has registered their handlers; panics
 * if one cannot be had. */
void bpxnu_doorbells_start(void);
/* A BPMP answer whose doorbell interrupt never came: one-line panic with the doorbell and GIC state. */
void bpxnu_doorbell_lost(int ch, int mrq) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
#endif
