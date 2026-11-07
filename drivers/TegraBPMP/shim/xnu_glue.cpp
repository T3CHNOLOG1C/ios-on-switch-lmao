// SPDX-License-Identifier: BSD-2-Clause-Patent
// XNU side of the TegraBPMP shim (bpxnu.h): heap, IOSimpleLock spin locks, event sleeps, device mappings, and the
// T210 BPMP doorbells that L4T's drivers/irqchip/irq-tegra.c provides (tegra_ring_doorbell /
// tegra_register_doorbell_handler, soc/tegra/doorbell.h).
//
// Doorbells (irq-tegra.c tegra_ictlr_init + l4t-hardware-soc-t210 tegra210-soc-base.dtsi lic node):
//   * inbound, BPMP -> CPU: the ICTLR node's `interrupts = <0 4 4  0 5 4  0 7 4  0 18 4>`, i.e. GIC SPIs 4, 5, 7, 18
//     (INTID 36, 37, 39, 50), level high, doorbell 0..3 in that order; mail_t210.c registers doorbell i as inbound
//     channel index i (bpmp_init_irq). The BPMP raises one by setting its bit in the legacy interrupt controller's
//     forced-interrupt register (LIC bank 0 CPU_IEP_FIR, 0x60004014); the CPU handler clears it through FIR_CLR
//     (0x6000401c) first, as irq-tegra.c's doorbell_handler -> ack_doorbell does, then runs mail.c's handler.
//   * outbound, CPU -> BPMP: `outgoing-doorbell = <6>`: FIR_SET bit 6 (tegra_ring_doorbell, CPU_OB_DOORBELL 4).
// Each inbound doorbell is an IOFilterInterruptEventSource on this service's work loop: the filter (primary
// interrupt context) only clears the FIR bit; the action runs mail.c's bpmp_handle_irq() in thread context, so the
// vendored code's spin locks and complete() never run at interrupt level. ArmGIC gates the LIC CPU_IER bit with the
// SPI. A doorbell that cannot be claimed panics; one that never comes for an answered request panics from mail.c
// (bpxnu_doorbell_lost). No polled mode.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include "../../ArmGIC/SwitchInterrupts.h"
#include "include/bpxnu.h"

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
// NOLINTNEXTLINE(readability-redundant-declaration): not every kernel configuration's headers declare it
extern "C" boolean_t ml_get_interrupts_enabled(void);
// NOLINTNEXTLINE(readability-redundant-declaration): not every kernel configuration's headers declare it
extern "C" int cpu_number(void);

namespace
{
constexpr vm_offset_t kLicPA = 0x60004000;

enum : uint32_t {
	ICTLR_CPU_IEP_FIR = 0x14,
	ICTLR_CPU_IEP_FIR_SET = 0x18,
	ICTLR_CPU_IEP_FIR_CLR = 0x1c,
	ICTLR_CPU_IER = 0x20
};

constexpr unsigned kDoorbells = 4;
constexpr uint32_t kInboundSpi[kDoorbells] = {4, 5, 7, 18};
constexpr uint32_t kOutboundBit = 6;
constexpr unsigned kOutboundDoorbell = 4;  // mail_t210.c CPU_OB_DOORBELL

volatile uint8_t *gLic;

struct Doorbell {
	void (*handler)(void *data);
	void *data;
};

Doorbell gDoorbells[kDoorbells];

volatile uint8_t *lic()
{
	if (!gLic)
		gLic = reinterpret_cast<volatile uint8_t *>(ml_io_map(kLicPA, 0x1000));
	return gLic;
}

inline uint32_t licRead(uint32_t offset)
{
	return *reinterpret_cast<volatile uint32_t *>(lic() + offset);
}

inline void licWrite(uint32_t offset, uint32_t value)
{
	*reinterpret_cast<volatile uint32_t *>(lic() + offset) = value;
}
}  // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage
class TegraBPMPDoorbells : public IOService
{
	// NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
	OSDeclareDefaultStructors(TegraBPMPDoorbells) IOWorkLoop *loop = nullptr;
	IOFilterInterruptEventSource *sources[kDoorbells] = {};
	static bool filter(OSObject *owner, IOFilterInterruptEventSource *sender);
	static void action(OSObject *owner, IOInterruptEventSource *sender, int count);
	int indexOf(IOInterruptEventSource *sender) const;

public:
	bool claim();
};

OSDefineMetaClassAndStructors(TegraBPMPDoorbells, IOService)

	int TegraBPMPDoorbells::indexOf(IOInterruptEventSource *sender) const
{
	for (unsigned i = 0; i < kDoorbells; ++i) {
		if (sources[i] == sender)
			return static_cast<int>(i);
	}
	return -1;
}

// Primary interrupt context: acknowledge the forced interrupt so the level drops, then hand over to the work loop.
bool TegraBPMPDoorbells::filter(OSObject *owner, IOFilterInterruptEventSource *sender)
{
	const auto *self = static_cast<TegraBPMPDoorbells *>(owner);
	const int i = self->indexOf(sender);
	if (i < 0)
		return false;
	licWrite(ICTLR_CPU_IEP_FIR_CLR, 1u << kInboundSpi[i]);
	(void)licRead(ICTLR_CPU_IEP_FIR);
	return true;
}

void TegraBPMPDoorbells::action(OSObject *owner, IOInterruptEventSource *sender, int count)
{
	const auto *self = static_cast<TegraBPMPDoorbells *>(owner);
	const int i = self->indexOf(sender);
	if (i >= 0 && gDoorbells[i].handler)
		gDoorbells[i].handler(gDoorbells[i].data);
}

bool TegraBPMPDoorbells::claim()
{
	loop = IOWorkLoop::workLoop();
	if (!loop)
		return false;
	for (unsigned i = 0; i < kDoorbells; ++i) {
		const uint32_t intid = 32 + kInboundSpi[i];
		IOService *nub = switch_interrupt_nub(intid, false);
		if (!nub)
			panic("TegraBPMP: no interrupt for BPMP doorbell %u (INTID %u)", i, intid);
		sources[i] = IOFilterInterruptEventSource::filterInterruptEventSource(
			this, &TegraBPMPDoorbells::action, &TegraBPMPDoorbells::filter, nub, 0);
		if (!sources[i] || loop->addEventSource(sources[i]) != kIOReturnSuccess) {
			panic("TegraBPMP: cannot attach BPMP doorbell %u (INTID %u)", i, intid);
		}
	}
	// A doorbell the BPMP forced before this point is not cleared: it fires as soon as its source is enabled and
	// mail.c then answers whatever inbound mail it signalled, as Linux does after request_irq().
	for (IOFilterInterruptEventSource *source : sources)
		source->enable();
	return true;
}

extern "C" {

int tegra_register_doorbell_handler(unsigned int doorbell_id, void (*handler)(void *data),
				    void *data)
{
	if (doorbell_id >= kDoorbells)
		return -22;  // -EINVAL, as irq-tegra.c
	gDoorbells[doorbell_id].handler = handler;
	gDoorbells[doorbell_id].data = data;
	return 0;
}

int tegra_ring_doorbell(unsigned int doorbell_id)
{
	if (doorbell_id != kOutboundDoorbell)
		return -22;
	__asm__ volatile("dsb st" ::
				 : "memory");  // the channel area writes land before the BPMP looks
	licWrite(ICTLR_CPU_IEP_FIR_SET, 1u << kOutboundBit);
	return 0;
}

void bpxnu_doorbells_start(void)
{
	static TegraBPMPDoorbells *service;
	if (service)
		return;
	IOService *resources = IOService::getResourceService();
	TegraBPMPDoorbells *candidate = OSTypeAlloc(TegraBPMPDoorbells);
	if (!lic() || !resources || !candidate || !candidate->init() ||
	    !candidate->attach(resources) || !candidate->start(resources)) {
		panic("TegraBPMP: cannot create the BPMP doorbell service");
	}
	if (!candidate->claim())
		panic("TegraBPMP: no work loop for the BPMP doorbells");
	candidate->setName("TegraBPMP");
	candidate->registerService();
	service = candidate;
}

void bpxnu_doorbell_lost(int ch, int mrq)
{
	const uint32_t fir = licRead(ICTLR_CPU_IEP_FIR);
	const uint32_t ier = licRead(ICTLR_CPU_IER);
	uint32_t spi = kInboundSpi[0];
	for (const uint32_t inbound : kInboundSpi) {
		if (fir & (1u << inbound)) {
			spi = inbound;
			break;
		}
	}
	char gic[160];
	switch_interrupt_describe(32 + spi, gic, sizeof(gic));
	panic("TegraBPMP: doorbell lost: BPMP answered mrq %d on ch %d, no interrupt in 4 s; LIC "
	      "FIR %08x IER %08x; %s",
	      mrq, ch, fir, ier, gic);
}

void bpxnu_log(const char *fmt, ...)
{
	char line[256];
	// NOLINTNEXTLINE(cppcoreguidelines-init-variables): va_start initializes it
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	IOLog("%s", line);
}

void bpxnu_warn(const char *file, int line)
{
	IOLog("TegraBPMP: WARN_ON at %s:%d\n", file, line);
}

void *bpxnu_zalloc(unsigned long size)
{
	unsigned long *p = static_cast<unsigned long *>(IOMallocZero(size + 16));
	if (!p)
		return nullptr;
	p[0] = size;
	return p + 2;
}

void bpxnu_free(void *p)
{
	unsigned long *block = static_cast<unsigned long *>(p) - 2;
	IOFree(block, block[0] + 16);
}

void *bpxnu_spin_alloc(void)
{
	return IOSimpleLockAlloc();
}

void bpxnu_spin_free(void *lock)
{
	IOSimpleLockFree(static_cast<IOSimpleLock *>(lock));
}

void bpxnu_spin_lock(void *lock)
{
	IOSimpleLockLock(static_cast<IOSimpleLock *>(lock));
}

void bpxnu_spin_unlock(void *lock)
{
	IOSimpleLockUnlock(static_cast<IOSimpleLock *>(lock));
}

unsigned long bpxnu_spin_lock_irqsave(void *lock)
{
	return static_cast<unsigned long>(
		IOSimpleLockLockDisableInterrupt(static_cast<IOSimpleLock *>(lock)));
}

void bpxnu_spin_unlock_irqrestore(void *lock, unsigned long flags)
{
	IOSimpleLockUnlockEnableInterrupt(static_cast<IOSimpleLock *>(lock),
					  static_cast<IOInterruptState>(flags));
}

int bpxnu_interrupts_disabled(void)
{
	return static_cast<int>(static_cast<int>(ml_get_interrupts_enabled()) == 0);
}

int bpxnu_cpu_number(void)
{
	return cpu_number();
}

unsigned long long bpxnu_nanotime(void)
{
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns;
}

void bpxnu_assert_wait(const void *event, unsigned long long deadline_ns)
{
	uint64_t deadline = 0;
	nanoseconds_to_absolutetime(deadline_ns, &deadline);
	assert_wait_deadline((event_t)event, THREAD_UNINT, deadline);
}

int bpxnu_block(void)
{
	return static_cast<int>(thread_block(THREAD_CONTINUE_NULL) == THREAD_TIMED_OUT);
}

void bpxnu_wakeup(const void *event)
{
	thread_wakeup((event_t)event);
}

volatile void *bpxnu_ioremap(unsigned long long pa, unsigned long size)
{
	const vm_offset_t base =
		static_cast<vm_offset_t>(pa) & ~static_cast<vm_offset_t>(PAGE_MASK);
	const vm_size_t span = round_page(static_cast<vm_offset_t>(pa) - base + size);
	const vm_offset_t va = ml_io_map(base, span);
	return va ? reinterpret_cast<volatile void *>(va + (static_cast<vm_offset_t>(pa) - base))
		  : nullptr;
}

}  // extern "C"
