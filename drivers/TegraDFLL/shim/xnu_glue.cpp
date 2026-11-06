// SPDX-License-Identifier: GPL-2.0
// XNU side of the TegraDFLL shim (see shim/include/dfllxnu.h): log, heap, interrupt mask, delays and time, device
// mappings, thread calls for the DFLL's timers, the API mutex. No Linux names in here.
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread_call.h>
#include <stdarg.h>
#include <string.h>
#include "include/dfllxnu.h"

extern "C" {
vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
}

namespace
{
lck_grp_t *group()
{
	static lck_grp_t *grp;
	if (!grp) {
		lck_grp_t *fresh = lck_grp_alloc_init("TegraDFLL", LCK_GRP_ATTR_NULL);
		// NOLINTNEXTLINE(misc-const-correctness): the compare-exchange writes it
		lck_grp_t *expected = nullptr;
		if (!__atomic_compare_exchange_n(&grp, &expected, fresh, false, __ATOMIC_ACQ_REL,
						 __ATOMIC_ACQUIRE))
			lck_grp_free(fresh);
	}
	return grp;
}

// Lines logged with interrupts masked (clk-dfll.c error paths under its td->lock spinlock, the only code here that runs
// masked) are copied here and printed by a thread call, since IOLog may block. That spinlock serializes the writers.
constexpr uint32_t kDeferredLines = 8, kLineBytes = 192;
char gDeferred[kDeferredLines][kLineBytes];
uint32_t gDeferredHead, gDeferredTail, gDeferredLost;
thread_call_t gFlush;

void flushDeferred(thread_call_param_t, thread_call_param_t)
{
	const uint32_t tail = __atomic_load_n(&gDeferredTail, __ATOMIC_ACQUIRE);
	for (uint32_t head = __atomic_load_n(&gDeferredHead, __ATOMIC_ACQUIRE); head != tail;
	     ++head)
		IOLog("%s", gDeferred[head % kDeferredLines]);
	__atomic_store_n(&gDeferredHead, tail, __ATOMIC_RELEASE);
	if (const uint32_t lost = __atomic_exchange_n(&gDeferredLost, 0u, __ATOMIC_ACQ_REL))
		IOLog("TegraDFLL: %u log lines lost with interrupts masked\n", lost);
}

struct Call {
	void (*fn)(void *);
	void *arg;
	thread_call_t call;
};

void callTrampoline(thread_call_param_t p0, thread_call_param_t)
{
	const Call *c = static_cast<Call *>(p0);
	c->fn(c->arg);
}

void allocFlush()
{
	if (__atomic_load_n(&gFlush, __ATOMIC_ACQUIRE))
		return;
	thread_call_t flush = thread_call_allocate(flushDeferred, nullptr);
	thread_call_t expected = nullptr;
	if (flush && !__atomic_compare_exchange_n(&gFlush, &expected, flush, false,
						  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		thread_call_free(flush);
}

struct Mapping {
	uint64_t page;
	vm_offset_t va;
};

constexpr uint64_t kWindow =
	0x1000;  // every block here fits one 4 KiB window (CAR, FUSE, APB_MISC, I2C5, DFLL)
Mapping gMappings[8];
}  // namespace

extern "C" void dfllxnu_vlog(const char *fmt, va_list ap)
{
	if (ml_get_interrupts_enabled()) {
		IOLogv(fmt, ap);
		return;
	}
	const uint32_t slot = __atomic_load_n(&gDeferredTail, __ATOMIC_ACQUIRE);
	if (!gFlush || slot - __atomic_load_n(&gDeferredHead, __ATOMIC_ACQUIRE) >= kDeferredLines) {
		__atomic_add_fetch(&gDeferredLost, 1u, __ATOMIC_RELAXED);
		return;
	}
	vsnprintf(gDeferred[slot % kDeferredLines], kLineBytes, fmt, ap);
	__atomic_store_n(&gDeferredTail, slot + 1, __ATOMIC_RELEASE);
	thread_call_enter(gFlush);  // safe with interrupts masked
}

extern "C" void dfllxnu_log(const char *fmt, ...)
{
	// NOLINTNEXTLINE(cppcoreguidelines-init-variables): va_start initializes it
	va_list ap;
	va_start(ap, fmt);
	dfllxnu_vlog(fmt, ap);
	va_end(ap);
}

// 16-byte size header, zeroed
extern "C" void *dfllxnu_malloc(unsigned long size)
{
	if (size > (1UL << 24))
		return nullptr;
	unsigned long *p = static_cast<unsigned long *>(IOMallocZero(size + 16));
	if (!p)
		return nullptr;
	p[0] = size;
	return p + 2;
}

extern "C" void dfllxnu_free(void *ptr)
{
	if (!ptr)
		return;
	unsigned long *p = static_cast<unsigned long *>(ptr) - 2;
	IOFree(p, p[0] + 16);
}

extern "C" unsigned long dfllxnu_irq_save(void)
{
	return ml_set_interrupts_enabled(FALSE) ? 1UL : 0UL;
}

extern "C" void dfllxnu_irq_restore(unsigned long flags)
{
	ml_set_interrupts_enabled(static_cast<boolean_t>(flags));
}

extern "C" void dfllxnu_udelay(dfllxnu_u32 us)
{
	IODelay(us);
}

extern "C" void dfllxnu_msleep(dfllxnu_u32 ms)
{
	IOSleep(ms);
}

extern "C" dfllxnu_u64 dfllxnu_nanotime(void)
{
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns;
}

// Called in thread context under the API mutex only (platform.c bring-up, the DFLL probe): the table needs no lock.
extern "C" volatile void *dfllxnu_ioremap(dfllxnu_u64 pa, dfllxnu_u64 size)
{
	const uint64_t page = pa & ~(kWindow - 1);
	if (pa + size > page + kWindow) {
		return reinterpret_cast<volatile void *>(
			ml_io_map(static_cast<vm_offset_t>(pa), static_cast<vm_size_t>(size)));
	}
	for (Mapping &m : gMappings) {
		if (m.va && m.page == page)
			return reinterpret_cast<volatile void *>(m.va + (pa - page));
		if (!m.va) {
			m.va = ml_io_map(static_cast<vm_offset_t>(page),
					 static_cast<vm_size_t>(kWindow));
			if (!m.va)
				return nullptr;
			m.page = page;
			return reinterpret_cast<volatile void *>(m.va + (pa - page));
		}
	}
	return nullptr;
}

extern "C" void *dfllxnu_call_alloc(void (*fn)(void *), void *arg)
{
	allocFlush();
	Call *c = static_cast<Call *>(IOMallocZero(sizeof(Call)));
	if (!c)
		return nullptr;
	c->fn = fn;
	c->arg = arg;
	c->call = thread_call_allocate(callTrampoline, c);
	if (!c->call) {
		IOFree(c, sizeof(Call));
		return nullptr;
	}
	return c;
}

extern "C" void dfllxnu_call_arm(void *call, dfllxnu_u64 delay_ns)
{
	const Call *c = static_cast<Call *>(call);
	if (!delay_ns) {
		thread_call_enter(c->call);
		return;
	}
	uint64_t interval = 0;
	uint64_t deadline = 0;
	nanoseconds_to_absolutetime(delay_ns, &interval);
	deadline = mach_absolute_time() + interval;
	thread_call_enter_delayed(c->call, deadline);
}

extern "C" int dfllxnu_call_cancel(void *call)
{
	return thread_call_cancel(static_cast<Call *>(call)->call) ? 1 : 0;
}

extern "C" void *dfllxnu_mutex_alloc(void)
{
	allocFlush();
	return lck_mtx_alloc_init(group(), LCK_ATTR_NULL);
}

extern "C" void dfllxnu_mutex_lock(void *m)
{
	lck_mtx_lock(static_cast<lck_mtx_t *>(m));
}

extern "C" void dfllxnu_mutex_unlock(void *m)
{
	lck_mtx_unlock(static_cast<lck_mtx_t *>(m));
}
