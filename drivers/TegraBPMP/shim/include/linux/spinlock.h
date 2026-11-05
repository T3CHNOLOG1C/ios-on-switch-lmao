/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Switch TegraBPMP shim: spin locks on IOSimpleLock, allocated on first use (shim/linux.c). Nothing here runs in
 * primary interrupt context: the doorbell filter only acknowledges the LIC, and mail.c's handler runs on a work loop
 * thread, so the irqsave variants only have to keep the caller from being preempted by another locker. */
#ifndef BPXNU_LINUX_SPINLOCK_H
#define BPXNU_LINUX_SPINLOCK_H
/* The GM20B shim (same kernel) defines Linux names too: every function here is bpmp_linux_-prefixed. */
#define spin_lock_init(a0) bpmp_linux_spin_lock_init(a0)
#define spin_lock(a0) bpmp_linux_spin_lock(a0)
#define spin_unlock(a0) bpmp_linux_spin_unlock(a0)
#define spin_unlock_irqrestore(a0, a1) bpmp_linux_spin_unlock_irqrestore(a0, a1)
#define local_irq_restore(a0) bpmp_linux_local_irq_restore(a0)
typedef struct { void *volatile lck; } spinlock_t;
typedef spinlock_t raw_spinlock_t;
#define DEFINE_SPINLOCK(x) spinlock_t x = { 0 }
#define DEFINE_RAW_SPINLOCK(x) raw_spinlock_t x = { 0 }
void spin_lock_init(spinlock_t *l);
void spin_lock(spinlock_t *l);
void spin_unlock(spinlock_t *l);
unsigned long bpxnu_linux_spin_lock_irqsave(spinlock_t *l);
void spin_unlock_irqrestore(spinlock_t *l, unsigned long flags);
#define spin_lock_irqsave(l, flags) ((flags) = bpxnu_linux_spin_lock_irqsave(l))
#define raw_spin_lock(l) spin_lock(l)
#define raw_spin_unlock(l) spin_unlock(l)
unsigned long bpxnu_local_irq_save(void);
void local_irq_restore(unsigned long flags);
#define local_irq_save(flags) ((flags) = bpxnu_local_irq_save())
#endif
