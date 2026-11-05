/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/*
 * Switch TegraBPMP shim: the Linux primitives the vendored L4T mail.c / mail_t210.c / mailman.c use, on top of the
 * XNU boundary in bpxnu.h. Semantics follow the Linux originals closely enough for those three files:
 *   - spin locks are IOSimpleLocks allocated on first use (a compare-and-swap installs one, the loser frees its own);
 *   - completions and semaphores sleep on an event (assert_wait under the spin lock, then block), so a waiter is
 *     woken by complete()/up() from the doorbell work loop and never polls;
 *   - jiffies are milliseconds.
 */
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/of_address.h>
#include <linux/semaphore.h>

static void *lock_of(spinlock_t *l)
{
	void *lck = l->lck;
	if (lck)
		return lck;
	lck = bpxnu_spin_alloc();
	if (!__sync_bool_compare_and_swap(&l->lck, NULL, lck))
		bpxnu_spin_free(lck);
	return l->lck;
}

void spin_lock_init(spinlock_t *l)
{
	l->lck = NULL;
	(void)lock_of(l);
}
void spin_lock(spinlock_t *l)
{
	bpxnu_spin_lock(lock_of(l));
}
void spin_unlock(spinlock_t *l)
{
	bpxnu_spin_unlock(l->lck);
}
unsigned long bpxnu_linux_spin_lock_irqsave(spinlock_t *l)
{
	return bpxnu_spin_lock_irqsave(lock_of(l));
}
void spin_unlock_irqrestore(spinlock_t *l, unsigned long flags)
{
	bpxnu_spin_unlock_irqrestore(l->lck, flags);
}

/* Only the atomic (interrupts-off) path of mail.c uses these; TegraBPMP never takes it. */
static spinlock_t irq_lock;
unsigned long bpxnu_local_irq_save(void)
{
	return bpxnu_spin_lock_irqsave(lock_of(&irq_lock));
}
void local_irq_restore(unsigned long flags)
{
	bpxnu_spin_unlock_irqrestore(irq_lock.lck, flags);
}

void init_completion(struct completion *c)
{
	spin_lock_init(&c->lock);
	c->done = 0;
}

void complete(struct completion *c)
{
	unsigned long flags;
	spin_lock_irqsave(&c->lock, flags);
	c->done++;
	bpxnu_wakeup(c);
	spin_unlock_irqrestore(&c->lock, flags);
}

/* Linux returns 0 on timeout, else the jiffies left (at least 1). */
unsigned long wait_for_completion_timeout(struct completion *c, unsigned long timeout)
{
	const unsigned long long deadline =
		bpxnu_nanotime() + (unsigned long long)timeout * 1000000ULL;
	for (;;) {
		unsigned long flags;
		spin_lock_irqsave(&c->lock, flags);
		if (c->done) {
			unsigned long long now = bpxnu_nanotime();
			unsigned long left =
				now < deadline ? (unsigned long)((deadline - now) / 1000000ULL) : 0;
			c->done--;
			spin_unlock_irqrestore(&c->lock, flags);
			return left ? left : 1;
		}
		if (bpxnu_nanotime() >= deadline) {
			spin_unlock_irqrestore(&c->lock, flags);
			return 0;
		}
		bpxnu_assert_wait(c, deadline);
		spin_unlock_irqrestore(&c->lock, flags);
		(void)bpxnu_block();
	}
}

void sema_init(struct semaphore *s, int count)
{
	spin_lock_init(&s->lock);
	s->count = count;
}

/* Linux returns 0 when it got the semaphore, -ETIME(DOUT) when the timeout (jiffies) ran out. */
int down_timeout(struct semaphore *s, long timeout)
{
	const unsigned long long deadline =
		bpxnu_nanotime() + (unsigned long long)timeout * 1000000ULL;
	for (;;) {
		unsigned long flags;
		spin_lock_irqsave(&s->lock, flags);
		if (s->count > 0) {
			s->count--;
			spin_unlock_irqrestore(&s->lock, flags);
			return 0;
		}
		if (bpxnu_nanotime() >= deadline) {
			spin_unlock_irqrestore(&s->lock, flags);
			return -ETIMEDOUT;
		}
		bpxnu_assert_wait(s, deadline);
		spin_unlock_irqrestore(&s->lock, flags);
		(void)bpxnu_block();
	}
}

void up(struct semaphore *s)
{
	unsigned long flags;
	spin_lock_irqsave(&s->lock, flags);
	s->count++;
	bpxnu_wakeup(s);
	spin_unlock_irqrestore(&s->lock, flags);
}

void memcpy_toio(volatile void *dst, const void *src, size_t n)
{
	volatile u8 *d = dst;
	const u8 *s = src;
	while (n && ((uintptr_t)d & 3)) {
		*d++ = *s++;
		n--;
	}
	for (; n >= 4; n -= 4, d += 4, s += 4) {
		u32 w;
		memcpy(&w, s, 4);
		*(volatile u32 *)d = w;
	}
	while (n--)
		*d++ = *s++;
}

void memcpy_fromio(void *dst, const volatile void *src, size_t n)
{
	u8 *d = dst;
	const volatile u8 *s = src;
	while (n && ((uintptr_t)s & 3)) {
		*d++ = *s++;
		n--;
	}
	for (; n >= 4; n -= 4, d += 4, s += 4) {
		u32 w = *(const volatile u32 *)s;
		memcpy(d, &w, 4);
	}
	while (n--)
		*d++ = *s++;
}

void *of_iomap(struct device_node *np, int index)
{
	if (!np || index < 0 || (unsigned int)index >= np->nreg)
		return NULL;
	return (void *)bpxnu_ioremap(np->reg[index].base, (unsigned long)np->reg[index].size);
}
