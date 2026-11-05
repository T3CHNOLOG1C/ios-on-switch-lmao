// SPDX-License-Identifier: MIT
// AGXDeviceUserClient: one connection = one GPU address space, implementing scratchpad/gpu/CONTRACT.md v1 over the wire
// records of TegraGM20BABI.h (copied verbatim from the userland side). Transport modelled on
// drivers/NativeAdreno/NativeAdrenoPublish.cpp: Apple user-client class name (sandbox profiles admit it), one open type,
// struct-in/struct-out methods with exact sizes, the same methods tunnelled through any admitted selector, and
// clientMemoryForType views cached per token so IOConnectUnmapMemory64 finds the same descriptor.
// GPU work goes through gpu.c (TegraGM20BGpu.h). Fence waits sleep until the GPU's nonstall interrupt wakes them;
// a fence found complete without that wake panics (no polled mode), and so does one that never completes.
#include <os/log.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <IOKit/IOUserClient.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <stddef.h>
#include <string.h>
#include "TegraGM20BABI.h"
#include "TegraGM20BGpu.h"
#include "../TegraDisplayFlip/TegraScanoutCopy.h"

namespace
{
constexpr uint32_t kMaxChannels = 8;  // fence slots: 16 bytes each in the connection's fence page
constexpr uint64_t kMaxBoBytes = 256ULL << 20;
constexpr uint32_t kFenceToken = 0xfe000001u;
// Connections system-wide: 16, or 64 with tegra-gpu-all-clients=1 (gpu.c TGM_FENCE_PAGES: one fence page each).
constexpr uint32_t kMaxConnections = 16, kMaxConnectionsAll = 64;
// GPU pool pages are outside the VM (arm_vm_init.c pmap_ioswitch_ram_init), so the kernel reports them as device
// memory, and for a physical descriptor IOKit fixes the user mapping's memory type from that default when the descriptor
// is made: kIOMapWriteCombineCache at map time does not change it. Without this the client got Device-nGnRE and its first
// unaligned memmove into a BO was an alignment fault (10-30: backboardd SIGBUS in Texture::replace_region).
constexpr IOOptionBits kPoolCacheMode = static_cast<IOOptionBits>(kIOWriteCombineCache)
					<< kIOMemoryBufferCacheShift;
volatile uint32_t gConnections;

extern "C" boolean_t PE_parse_boot_argn(const char *arg_string, void *arg_ptr, int max_arg);
extern "C" void proc_selfname(char *buf, int size);
extern "C" vm_offset_t ml_vtophys(vm_offset_t vaddr);
extern "C" task_t current_task(void);

// A cached BO's view up to this size is mapped into its task with every page entered at once (kIOMapPrefault: one
// vm_map_enter_mem_object_prefault over the wired page list), not one page fault per 4 KiB page as the task first touches
// it. NVK fills such BOs (64 KiB command, upload and stream memory; small descriptor and query pools); larger ones (the
// runtime's pooled blocks, mapped whole and touched only where it places CPU-visible resources) keep lazy faults.
constexpr uint64_t kPrefaultMaxBytes = 256ULL << 10;

// boot-arg tegra-gpu-import-sync=1: clean+invalidate every host import of the connection at each submission, the
// visibility point Metal gives shared memory at commit, for CPU writes into surfaces the GPU already imported (the
// Metal frontend imports IOSurfaces as coherent memory and does no cache maintenance of its own). Costs about a
// millisecond per few MiB imported, per submission; off by default.
bool importSync()
{
	static int on = -1;
	if (on < 0) {
		int v = 0;
		on = PE_parse_boot_argn("tegra-gpu-import-sync", &v, sizeof(v)) && v;
	}
	return on;
}

// Freed pool BOs (see releaseBo): every one's view is released and its pages given back here, on a thread call, never on
// the freeing thread. A view a task still maps keeps its pages until the last such mapping is gone, so the task can never
// write pages the pool has handed out again (GPU page tables among them); IOKit keeps a task's mappings past its
// connection's close, until the task unmaps them or dies, so the list is global and any connection's BoAlloc and close
// kick the thread call to look at it again.
struct PoolOrphan {
	PoolOrphan *next;
	IOMemoryDescriptor *view;
	void *va;
	uint64_t pa, bytes;
	bool zero;  // a BO a client had: its pages are zeroed before they go back (poolZeroLater)
};
PoolOrphan *gPoolOrphans;

void pushOrphan(PoolOrphan *orphan)
{
	orphan->next = __atomic_load_n(&gPoolOrphans, __ATOMIC_RELAXED);
	while (!__atomic_compare_exchange_n(&gPoolOrphans, &orphan->next, orphan, true,
					    __ATOMIC_RELEASE, __ATOMIC_RELAXED))
		;
}

bool poolZeroLater(void *va, uint64_t pa, uint64_t bytes);

// A view only we hold is mapped nowhere (an IOMemoryMap holds its descriptor), and nothing can map it any more. But
// releasing it destroys its device pager's VM object, which first waits (uninterruptibly, vm_object_destroy ->
// vm_object_paging_wait) for every UPL on that object to be committed, and the retain count does not show those: an import
// of the view's mapping (BoImport of the WC pages, m2v's twin of a raw buffer) holds one until its own BO is released,
// maybe on another connection, maybe later on the very thread that freed the view. So no client thread releases a view:
//  - r7u: a dying task closes its connections one after the other on one thread; the raw connection's view went first,
//    and the twin's connection, which would complete that UPL, never got closed (backboardd's abort became a frozen
//    screen, its exit stuck in vm_object_destroy under AGXDeviceUserClient::releaseBo <- clientClose, then the watchdog
//    panic at 199 s).
//  - the same wait on a live task's thread: a BoFree of a raw buffer no task maps any more, or a BoAlloc that released
//    orphans inline (as this did before), with the buffer's twin import still live: the compositor's thread waits for
//    an import only it would free. (r7x froze on a Joy-Con screenshot; no log of it reached us, so that it was this
//    wait is inferred, not shown. With no client thread left to wait here, a freeze that remains is something else.)
// Here a run that meets such a view waits for its import to go, with the rest of the batch it took behind it; orphans
// freed meanwhile get a run of their own (the call is not ONCE: XNU thread_call.c _pending_call_enqueue queues a running
// non-ONCE call again, and the kernel group is TCG_PARALLEL, so another thread runs it). A view whose twin a long-lived
// process keeps holds its batch's pages until that import is freed or the process exits.
void releaseOrphans()
{
	for (PoolOrphan *orphan = __atomic_exchange_n(&gPoolOrphans, nullptr, __ATOMIC_ACQUIRE),
			*next;
	     orphan; orphan = next) {
		next = orphan->next;
		if (orphan->view->getRetainCount() > 1) {
			pushOrphan(orphan);
			continue;
		}
		orphan->view->release();
		if (!(orphan->zero && poolZeroLater(orphan->va, orphan->pa, orphan->bytes)))
			tgm_pool_free(orphan->va, orphan->bytes, orphan->pa);
		IODelete(orphan, PoolOrphan, 1);
	}
}

thread_call_t gOrphanCall;
void orphanCallMain(thread_call_param_t, thread_call_param_t)
{
	releaseOrphans();
}

// releaseOrphans, on the thread call. Never blocks: callable with the connection's lock held.
void scheduleOrphanRelease()
{
	if (!__atomic_load_n(&gOrphanCall, __ATOMIC_ACQUIRE)) {
		// not ONCE: entered while a run is blocked on one view, it runs again (concurrently) for the others
		thread_call_t fresh = thread_call_allocate_with_options(
			orphanCallMain, nullptr, THREAD_CALL_PRIORITY_KERNEL, 0);
		thread_call_t expected = nullptr;
		if (fresh && !__atomic_compare_exchange_n(&gOrphanCall, &expected, fresh, false,
							  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			thread_call_free(fresh);
	}
	thread_call_t call = __atomic_load_n(&gOrphanCall, __ATOMIC_ACQUIRE);
	if (call)
		thread_call_enter(call);
}

uint64_t nowNs()
{
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns;
}

// The GPU pool for clients other than the compositor (tegra-gpu-all-clients, on by default). Every coherent BO comes from
// the pool (NVK's coherent event heap, Metal's Shared storage through m2v), and so do nvkm's own page tables and channels,
// unpoliced. Tiers, so the compositor is never starved and nobody else is refused while there is room:
//  - a BO of up to kPoolSmallBytes (what a device needs to exist: NVK's event heap, upload chunks) is refused only when it
//    would leave less than kPoolFloorBytes free: one more of the compositor's 16 MiB m2v blocks, and with it the room
//    nvkm's page tables and channels for new clients take;
//  - a larger one also leaves tegra-gpu-pool-reserve-mb (default 32: one of the compositor's 16 MiB m2v blocks, and the raw
//    path's ~7 MiB of rings and tables should backboardd restart, with margin) free, and keeps the client within
//    tegra-gpu-client-pool-mb (default 64) of pool BOs, a fairness share, not a limit on GPU memory (cached BOs have none).
// Was: any BO kept the 32 MiB, so at 97 of 128 MiB used every poster extension's first 4 KiB was refused and its device
// never opened (r7j). A refused BoAlloc is kIOReturnNoMemory, logged once per connection with the rule that refused it.
constexpr uint64_t kPoolSmallBytes = 1ULL << 20, kPoolFloorBytes = 16ULL << 20;
uint64_t clientPoolBytes()
{
	static uint64_t bytes;
	if (!bytes) {
		uint32_t mb = 64;
		PE_parse_boot_argn("tegra-gpu-client-pool-mb", &mb, sizeof(mb));
		bytes = uint64_t(mb) << 20;
	}
	return bytes;
}

uint64_t poolReserveBytes()
{
	static uint64_t bytes = UINT64_MAX;
	if (bytes == UINT64_MAX) {
		uint32_t mb = 32;
		PE_parse_boot_argn("tegra-gpu-pool-reserve-mb", &mb, sizeof(mb));
		bytes = uint64_t(mb) << 20;
	}
	return bytes;
}

bool poolZeroNow();

// Why a non-compositor client's pool BO of `bytes` is refused while it holds `held` (the tiers above), or nullptr. Freed
// ranges the thread call has not zeroed yet are room too: when short, they are zeroed now rather than the BO refused.
const char *poolRefusal(uint64_t bytes, uint64_t held)
{
	uint64_t avail = tgm_pool_avail();
	if (avail < bytes + poolReserveBytes() && poolZeroNow())
		avail = tgm_pool_avail();
	if (avail < bytes + kPoolFloorBytes)
		return "the pool's last 16 MiB stay for the compositor and nvkm";
	if (bytes <= kPoolSmallBytes)
		return nullptr;
	if (avail < bytes + poolReserveBytes())
		return "the rest stays for the compositor";
	if (held + bytes > clientPoolBytes())
		return "past the client's share";
	return nullptr;
}

// ------------------------------------------------------------------ BoAlloc without waiting: spare BOs
// What a BoAlloc cost its caller before (r7l, after 315c6e5): a cached BO is allocated and zero-filled by the VM and then
// cleaned to DRAM line by line (0.34 ms per MiB), 18-26 ms for backboardd's 16 MiB m2v blocks, which Core Animation makes
// on its render thread; a pool BO was memset on the caller's thread (4.5 ms for 16 MiB). Now:
//  - Cached BOs come from spares in size classes (4-64 KiB by powers of two, then quarter steps to 256 MiB, so a BO
//    wastes at most a quarter of its spare): a BoAlloc takes the smallest ready spare of its class or up to twice its size
//    (classUp, spareTake) and maps it: no allocation, no zeroing, no clean on the caller's thread. A BO smaller than its
//    spare is a view of the spare's first pages. The class of a size missed is kept warm from then on, and a few classes
//    from the start (kWarm: what NVK and m2v make most: 64 KiB pieces, m2v's 4 MiB blocks, backboardd's 8 and 16 MiB
//    ones, two of the 16 MiB so that a pair of them in one frame waits for neither).
//  - A freed cached BO no task maps any more goes to the spares as it is (a list push: BoFree no longer unwires and
//    frees); a thread call zeroes it through its kernel mapping and cleans it to DRAM once, then files it by class, or
//    gives it back to the VM past kSpareCapBytes. The thread call runs on a free, a miss or a take below a class's mark:
//    never on a timer. Under memory pressure (memorystatus' level, the one apps' memory warnings follow) it gives every
//    spare back and keeps none.
//  - A freed pool BO is zeroed by the same thread call and given back clean (tgm_pool_free_zeroed), so the next pool
//    BoAlloc of those pages does no memset (shim nvxnu_pool_alloc_clean zeroes only pages a plain free left dirty).
// Each window's BoAllocs by process go to the "mem3" line (tgm_owner_stats).
constexpr uint32_t kClasses = 53;  // 4 KiB .. 256 MiB (kMaxBoBytes)
constexpr uint64_t kSpareCapBytes = 96ULL << 20, kLearnMaxBytes = 16ULL << 20;
struct WarmClass {
	uint64_t bytes;
	uint16_t ready;
};
constexpr WarmClass kWarm[] = {
	{64ULL << 10, 8}, {4ULL << 20, 2}, {8ULL << 20, 1}, {16ULL << 20, 2}};
constexpr uint32_t kOwners = 16;

uint64_t classBytes(uint32_t c)
{
	return c < 5 ? 4096ULL << c : ((64ULL << 10) << ((c - 5) / 4)) * (5 + (c - 5) % 4) / 4;
}

// The smallest class of at least `bytes`; kClasses past the largest.
uint32_t classUp(uint64_t bytes)
{
	uint32_t c = 0;
	while (c < kClasses && classBytes(c) < bytes)
		c++;
	return c;
}

struct Spare {
	Spare *next;
	IOBufferMemoryDescriptor *bmd;  // prepared; ours alone (no view, no task mapping)
	uint64_t *pas;
	uint64_t bytes;
};
struct PoolZero {
	PoolZero *next;
	void *va;
	uint64_t pa, bytes;
};
struct OwnerStat {
	char name[17];
	uint64_t n, bytes, ns, max_ns,
		made;  // made: cached BOs made on the caller's thread (a miss)
};
struct SparePool {
	IOLock *lock;
	thread_call_t call;
	Spare *dirty;         // freed, not yet zeroed
	PoolZero *poolDirty;  // freed pool BOs, not yet zeroed
	Spare *ready
		[kClasses];  // zeroed and clean, by class (classUp(bytes) <= class of a spare's bytes)
	uint16_t count[kClasses], target[kClasses];
	uint64_t clean, dirtyBytes,
		poolZeroing;  // bytes ready, freed and waiting, pool bytes waiting
	OwnerStat owners[kOwners];
};
SparePool *gSpares;
// XNU's vm_pressure_level_t: 0 normal, 1 warning, 2 urgent, 4 critical.
extern "C" int memorystatus_vm_pressure_level;

bool memoryPressure()
{
	return __atomic_load_n(&memorystatus_vm_pressure_level, __ATOMIC_RELAXED) != 0;
}

// The class a spare of `bytes` serves: the largest class not past it.
uint32_t spareClass(uint64_t bytes)
{
	const uint32_t c = classUp(bytes);
	return c < kClasses && classBytes(c) == bytes ? c : c - 1;
}

void spareFree(Spare *s)
{
	s->bmd->complete();
	s->bmd->release();
	IODelete(s->pas, uint64_t, s->bytes >> 12);
	IODelete(s, Spare, 1);
}

// Zero `bytes` through a cached BO's kernel mapping (when `zero`), then clean and invalidate its lines: the GPU and the
// next owner's view see zeros, and no dirty line is written back later over what the GPU writes.
void cleanZeros(void *va, uint64_t bytes, bool zero)
{
	if (zero)
		bzero(va, bytes);
	for (uint64_t a = reinterpret_cast<uint64_t>(va) & ~63ULL;
	     a < reinterpret_cast<uint64_t>(va) + bytes; a += 64)
		__asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

// The page list of a wired kernel buffer.
bool kernelPages(const void *va, uint64_t bytes, uint64_t *pas)
{
	for (uint64_t off = 0; off < bytes; off += 4096) {
		const uint64_t pa = ml_vtophys(reinterpret_cast<vm_offset_t>(va) + off);
		if (!pa || (pa & 0xfff) || pa + 4096 > (1ULL << 34))
			return false;
		pas[off >> 12] = pa;
	}
	return true;
}

// A wired, zeroed, clean cached BO of `bytes` (what BoAlloc's own path makes), or nullptr.
Spare *spareMake(uint64_t bytes)
{
	Spare *s = IONew(Spare, 1);
	uint64_t *pas = IONew(uint64_t, bytes >> 12);
	IOBufferMemoryDescriptor *bmd = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
		kernel_task,
		static_cast<IOOptionBits>(kIODirectionInOut) |
			static_cast<IOOptionBits>(kIOMemoryMapperNone),
		bytes, 0x3ffffffffULL);
	if (s && pas && bmd && bmd->prepare() == kIOReturnSuccess) {
		// KMA_ZERO zero-filled it through the cacheable aperture: clean the zeros out
		cleanZeros(bmd->getBytesNoCopy(), bytes, false);
		if (kernelPages(bmd->getBytesNoCopy(), bytes, pas)) {
			*s = {nullptr, bmd, pas, bytes};
			return s;
		}
		bmd->complete();
	}
	if (bmd)
		bmd->release();
	if (pas)
		IODelete(pas, uint64_t, bytes >> 12);
	if (s)
		IODelete(s, Spare, 1);
	return nullptr;
}

void spareWork(thread_call_param_t param, thread_call_param_t)
{
	SparePool *p = static_cast<SparePool *>(param);
	for (;;) {
		IOLockLock(p->lock);
		if (PoolZero *z = p->poolDirty) {  // first: pool pages are the scarcer
			p->poolDirty = z->next;
			IOLockUnlock(p->lock);
			tgm_pool_free_zeroed(z->va, z->bytes, z->pa);
			IOLockLock(p->lock);
			p->poolZeroing -= z->bytes;
			IOLockUnlock(p->lock);
			IODelete(z, PoolZero, 1);
			continue;
		}
		const bool pressure = memoryPressure();
		if (Spare *s = p->dirty) {
			p->dirty = s->next;
			p->dirtyBytes -= s->bytes;
			const bool keep = !pressure && p->clean + s->bytes <= kSpareCapBytes;
			if (keep)
				p->clean += s->bytes;  // held for it while it is zeroed
			IOLockUnlock(p->lock);
			if (!keep) {
				spareFree(
					s);  // past the cap or under pressure: back to the VM, unzeroed
				continue;
			}
			cleanZeros(s->bmd->getBytesNoCopy(), s->bytes, true);
			const uint32_t c = spareClass(s->bytes);
			IOLockLock(p->lock);
			s->next = p->ready[c];
			p->ready[c] = s;
			p->count[c]++;
			IOLockUnlock(p->lock);
			continue;
		}
		if (pressure) {  // give every spare back; refill nothing while it lasts
			Spare *all = nullptr;
			for (uint32_t c = 0; c < kClasses; c++) {
				while (Spare *s = p->ready[c]) {
					p->ready[c] = s->next;
					s->next = all;
					all = s;
				}
				p->count[c] = 0;
			}
			p->clean = 0;
			IOLockUnlock(p->lock);
			while (all) {
				Spare *next = all->next;
				spareFree(all);
				all = next;
			}
			return;
		}
		uint32_t c = 0;
		while (c < kClasses &&
		       (p->count[c] >= p->target[c] || p->clean + classBytes(c) > kSpareCapBytes))
			c++;
		if (c == kClasses) {
			IOLockUnlock(p->lock);
			return;
		}
		p->clean += classBytes(c);
		IOLockUnlock(p->lock);
		Spare *s = spareMake(classBytes(c));
		IOLockLock(p->lock);
		if (!s) {  // the VM could not give it: try again at the next event, not now
			p->clean -= classBytes(c);
			IOLockUnlock(p->lock);
			return;
		}
		s->next = p->ready[c];
		p->ready[c] = s;
		p->count[c]++;
		IOLockUnlock(p->lock);
	}
}

SparePool *spares()
{
	SparePool *p = __atomic_load_n(&gSpares, __ATOMIC_ACQUIRE);
	if (p)
		return p;
	SparePool *n = IONew(SparePool, 1);
	if (!n)
		return nullptr;
	bzero(n, sizeof(*n));
	for (const WarmClass &w : kWarm)
		n->target[classUp(w.bytes)] = w.ready;
	n->lock = IOLockAlloc();
	n->call = n->lock ? thread_call_allocate_with_priority(spareWork, n,
							       THREAD_CALL_PRIORITY_USER)
			  : nullptr;
	if (!n->call) {
		if (n->lock)
			IOLockFree(n->lock);
		IODelete(n, SparePool, 1);
		return nullptr;
	}
	if (!__atomic_compare_exchange_n(&gSpares, &p, n, false, __ATOMIC_ACQ_REL,
					 __ATOMIC_ACQUIRE)) {
		thread_call_free(n->call);
		IOLockFree(n->lock);
		IODelete(n, SparePool, 1);
		return p;
	}
	thread_call_enter(n->call);  // the warm classes, made before anyone waits for them
	return n;
}

// A ready spare for a cached BO of `bytes`: of its class, or of a larger one up to about twice its size; nullptr when
// none is ready, which marks the class to keep warm. Either way a class below its mark wakes the thread call.
Spare *spareTake(uint64_t bytes)
{
	SparePool *p = spares();
	const uint32_t c = classUp(bytes);
	if (!p || c >= kClasses)
		return nullptr;
	const uint32_t last = c + (c < 5 ? 1 : 4);
	IOLockLock(p->lock);
	Spare *s = nullptr;
	for (uint32_t d = c; d < kClasses && d <= last && !s; d++)
		if ((s = p->ready[d])) {
			p->ready[d] = s->next;
			p->count[d]--;
			p->clean -= s->bytes;
		}
	if (!s && classBytes(c) <= kLearnMaxBytes && !p->target[c])
		p->target[c] = 1;
	const bool kick = !memoryPressure() ? p->count[c] < p->target[c] : p->clean != 0;
	IOLockUnlock(p->lock);
	if (kick)
		thread_call_enter(p->call);
	return s;
}

// A freed cached BO (its prepared descriptor, which nothing else holds, with its `bytes` and page list) to the spares:
// true if taken, then the caller must not touch bmd or pas again. Zeroing, cleaning and the cap are the thread call's.
bool spareKeep(IOBufferMemoryDescriptor *bmd, uint64_t *pas, uint64_t bytes)
{
	SparePool *p = spares();
	Spare *s = p && bmd->getRetainCount() == 1 ? IONew(Spare, 1) : nullptr;
	if (!s)
		return false;
	*s = {nullptr, bmd, pas, bytes};
	IOLockLock(p->lock);
	s->next = p->dirty;
	p->dirty = s;
	p->dirtyBytes += bytes;
	IOLockUnlock(p->lock);
	thread_call_enter(p->call);
	return true;
}

// A freed pool BO's range (no task maps it) to be zeroed and given back clean by the thread call: true if taken.
bool poolZeroLater(void *va, uint64_t pa, uint64_t bytes)
{
	SparePool *p = spares();
	PoolZero *z = p ? IONew(PoolZero, 1) : nullptr;
	if (!z)
		return false;
	*z = {nullptr, va, pa, bytes};
	IOLockLock(p->lock);
	z->next = p->poolDirty;
	p->poolDirty = z;
	p->poolZeroing += bytes;
	IOLockUnlock(p->lock);
	thread_call_enter(p->call);
	return true;
}

// The freed pool ranges still waiting, zeroed and given back now: a pool BoAlloc that found no room tries again after.
bool poolZeroNow()
{
	SparePool *p = __atomic_load_n(&gSpares, __ATOMIC_ACQUIRE);
	if (!p)
		return false;
	IOLockLock(p->lock);
	PoolZero *all = p->poolDirty;
	p->poolDirty = nullptr;
	IOLockUnlock(p->lock);
	if (!all)
		return false;
	while (all) {
		PoolZero *next = all->next;
		tgm_pool_free_zeroed(all->va, all->bytes, all->pa);
		IOLockLock(p->lock);
		p->poolZeroing -= all->bytes;
		IOLockUnlock(p->lock);
		IODelete(all, PoolZero, 1);
		all = next;
	}
	return true;
}

// One BoAlloc of `owner`'s for the window's "mem3" line; `made`: a cached BO made on the caller's thread.
void ownerNote(const char *owner, uint64_t bytes, uint64_t ns, bool made)
{
	SparePool *p = spares();
	if (!p || !tgm_stats_on())
		return;
	IOLockLock(p->lock);
	OwnerStat *o = nullptr;
	for (uint32_t i = 0; i < kOwners && !o; i++)
		if (!p->owners[i].n || !strncmp(p->owners[i].name, owner, sizeof(o->name) - 1))
			o = &p->owners[i];
	if (!o) {  // more processes than slots this window: the last slot takes the rest
		o = &p->owners[kOwners - 1];
		strlcpy(o->name, "(others)", sizeof(o->name));
	} else if (!o->n) {
		strlcpy(o->name, owner, sizeof(o->name));
	}
	o->n++;
	o->bytes += bytes;
	o->ns += ns;
	o->made += made;
	if (ns > o->max_ns)
		o->max_ns = ns;
	IOLockUnlock(p->lock);
}

// boot-arg tegra-gpu-stats=1 (also gpu.c's stats lines): the method stream of every kScanEvery-th submission is read
// back and what it asks of the GPU is counted, one line per 5 s ("tegra-gm20b: push"), per submission: render passes,
// bound color targets (RTs) and the pitch-linear ones, draws, 3D wait-for-idles (WFI), texture and shader cache
// invalidates, clears, copy-engine copies and their bytes, 2D blits, compute dispatches, semaphore acquires and host
// flushes (MEM_OP, SET_REFERENCE, WFI); "bad" counts streams with an opcode NVK does not emit. Pushes in host imports
// (no kernel mapping) are not read; the line says what share of the push bytes was. Encoding: NV906F DMA headers (opcode
// 31:29, count or immediate 28:16, subchannel 15:13, dword method 11:0); NVK's subchannels (Mesa nv_push.h: 0 3D,
// 1 compute, 3 2D, 4 copy) and MME macros (nvk_mme.h: 2 CLEAR, 11..16 the draws); method numbers from Mesa's clb197.h,
// clb1c0.h, cl902d.h and cla0b5.h.
constexpr uint32_t kScanEvery = 4;
constexpr uint64_t kScanMaxBytes = 512 << 10, kScanWindowNs = 5000000000ULL;
struct PushCounts {
	uint64_t submits, scanned, bytes, readBytes, unparsed;
	uint64_t passes, targets, linearTargets, draws, wfi, texInval, shaderInval, clears,
		ceCopies, ceBytes, blits, dispatches, acquires, hostFlushes;
};
PushCounts gPush;
uint64_t gPushSince;

bool pushScanOn()
{
	static int on = -1;
	if (on < 0) {
		int v = 0;
		on = PE_parse_boot_argn("tegra-gpu-stats", &v, sizeof(v)) && v;
	}
	return on;
}

struct PushParse {
	uint32_t op, subc, mthd,
		left;  // the method in progress (it may continue in the next push of the submission)
	uint32_t ceLine, ceCount;  // the copy engine's LINE_LENGTH_IN / LINE_COUNT
};

void pushMethod(PushCounts &c, PushParse &p, uint32_t subc, uint32_t mthd, uint32_t data)
{
	if (mthd < 0x100) {                          // host methods, any subchannel
		if (mthd == 0x001c && (data & 0xd))  // SEMAPHORED: ACQUIRE, ACQ_GEQ, ACQ_AND
			c.acquires++;
		else if (mthd == 0x0034 || mthd == 0x0050 ||
			 mthd == 0x0078)  // MEM_OP_D, SET_REFERENCE (NVK's membar), WFI
			c.hostFlushes++;
		return;
	}
	const bool target =
		mthd >= 0x0800 && mthd < 0x0a00;  // SET_COLOR_TARGET_*(j), 64 bytes apart
	switch (subc) {
	case 0:  // MAXWELL_B
		if (mthd == 0x0110)
			c.wfi++;  // WAIT_FOR_IDLE
		else if (mthd == 0x1288 || mthd == 0x1338)
			c.texInval++;  // INVALIDATE_TEXTURE_DATA_CACHE(_NO_WFI)
		else if (mthd == 0x021c)
			c.shaderInval++;  // INVALIDATE_SHADER_CACHES (waits for idle)
		else if (mthd == 0x1538)
			c.passes++;  // SET_ZT_SELECT: once per vkCmdBeginRendering
		else if (mthd == 0x19d0)
			c.clears++;  // CLEAR_SURFACE
		else if (mthd == 0x1618)
			c.draws++;  // BEGIN outside an MME macro
		else if (target && (mthd & 0x3f) == 0x10 && data)
			c.targets++;  // SET_COLOR_TARGET_FORMAT, not DISABLED
		else if (target && (mthd & 0x3f) == 0x14 && (data & (1u << 12)))
			c.linearTargets++;  // SET_COLOR_TARGET_MEMORY LAYOUT_PITCH
		else if (mthd >= 0x3800 && mthd < 0x3c00 && !(mthd & 7)) {  // CALL_MME_MACRO(j)
			const uint32_t j = (mthd - 0x3800) / 8;
			c.draws += j >= 11 && j <= 16;
			c.clears += j == 2;
		}
		break;
	case 1:                                  // MAXWELL_COMPUTE_B
		c.dispatches += mthd == 0x02b4;  // SEND_PCAS_A
		c.wfi += mthd == 0x0110;
		break;
	case 3:                             // FERMI_TWOD_A
		c.blits += mthd == 0x08dc;  // PIXELS_FROM_MEMORY_SRC_Y0_INT
		break;
	case 4:  // MAXWELL_DMA_COPY_A
		if (mthd == 0x0418)
			p.ceLine = data;
		else if (mthd == 0x041c)
			p.ceCount = data;
		else if (mthd == 0x0300) {  // LAUNCH_DMA (bit 9 MULTI_LINE)
			c.ceCopies++;
			c.ceBytes += uint64_t(p.ceLine) * ((data & (1u << 9)) ? p.ceCount : 1);
		}
		break;
	default:
		break;
	}
}

void pushParse(PushCounts &c, PushParse &p, const uint32_t *w, uint64_t n)
{
	for (uint64_t i = 0; i < n; i++) {
		if (p.left) {  // a data word of the method in progress
			pushMethod(c, p, p.subc, p.mthd, w[i]);
			if (p.op == 1) {  // INC: the next word to the next method
				p.mthd += 4;
			} else if (p.op ==
				   5) {  // ONE_INC: every word after the first to the next method
				p.op = 3;
				p.mthd += 4;
			}
			p.left--;
			continue;
		}
		const uint32_t h = w[i], op = h >> 29, count = (h >> 16) & 0x1fff,
			       subc = (h >> 13) & 7, mthd = (h & 0xfff) << 2;
		if (!h)
			continue;  // padding
		if (op == 4) {     // IMMD_DATA_METHOD: the data is the count field
			pushMethod(c, p, subc, mthd, count);
		} else if (op == 1 || op == 3 || op == 5) {  // INC, NON_INC, ONE_INC
			p = PushParse{op, subc, mthd, count, p.ceLine, p.ceCount};
		} else {
			c.unparsed++;
			return;
		}
	}
}

IOReturn ioreturn(int err)
{
	switch (err) {
	case 0:
		return kIOReturnSuccess;
	case -12:
		return kIOReturnNoMemory;  // ENOMEM
	case -22:
		return kIOReturnBadArgument;  // EINVAL
	case -16:
		return kIOReturnBusy;  // EBUSY
	case -5:
		return kIOReturnNotResponding;  // EIO
	case -110:
		return kIOReturnTimeout;  // ETIMEDOUT
	case -19:
		return kIOReturnNotReady;  // ENODEV
	case -28:
		return kIOReturnNoResources;  // ENOSPC
	default:
		return kIOReturnError;
	}
}
}  // namespace

extern "C" void tgm_spare_counts(uint64_t *kept_bytes, uint64_t *ready,
				 uint64_t *pool_zeroing_bytes)
{
	SparePool *p = __atomic_load_n(&gSpares, __ATOMIC_ACQUIRE);
	uint64_t n = 0;
	if (p) {
		IOLockLock(p->lock);
		for (uint32_t c = 0; c < kClasses; c++)
			n += p->count[c];
		*kept_bytes = p->clean + p->dirtyBytes;
		*pool_zeroing_bytes = p->poolZeroing;
		IOLockUnlock(p->lock);
	} else {
		*kept_bytes = *pool_zeroing_bytes = 0;
	}
	*ready = n;
}

// The window's BoAllocs by process ("name n (KiB, mean/max us, made m)"), then cleared; 0 when there were none.
extern "C" unsigned long tgm_owner_stats(char *out, unsigned long size)
{
	SparePool *p = __atomic_load_n(&gSpares, __ATOMIC_ACQUIRE);
	unsigned long at = 0;
	if (!p || !size)
		return 0;
	out[0] = 0;
	IOLockLock(p->lock);
	for (uint32_t i = 0; i < kOwners && p->owners[i].n; i++) {
		const OwnerStat &o = p->owners[i];
		const int n = snprintf(out + at, size - at,
				       "%s%s %llu (%llu KiB, %llu/%llu us, made %llu)",
				       at ? "; " : "", o.name, o.n, o.bytes >> 10,
				       o.ns / o.n / 1000, o.max_ns / 1000, o.made);
		if (n > 0)
			at = at + n < size ? at + n : size - 1;
	}
	bzero(p->owners, sizeof(p->owners));
	IOLockUnlock(p->lock);
	return at;
}

class AGXDeviceUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(AGXDeviceUserClient)

		struct Bo {
		Bo *next;
		uint64_t handle, bytes;
		uint32_t token, flags, npages;
		uint32_t
			backing_pages;  // pas' length: npages, or a spare's pages when the BO is a view of its first ones
		bool readOnly;  // imports of pages the task maps read-only: the GPU maps them read-only
		uint64_t *pas;  // one per 4 KiB page; tgm_mem keeps a pointer to it
		void *pool_va;  // Coherent BOs: pool memory (write-combined, contiguous)
		uint64_t pool_pa;
		IOBufferMemoryDescriptor *bmd;  // cached BOs
		IOMemoryDescriptor *wired;      // imports
		IOMemoryDescriptor *view;       // cached clientMemoryForType descriptor
		tgm_mem *mem;
		tgm_coh *coh;  // imports: the surface's coherency registry entry (gpu.c), held
		uint32_t snapshot[kMaxChannels];  // submitted seqnos when freed (zombies only)
	};
	struct Binding {
		uint64_t va, range;
		Bo *bo;
		uint64_t bo_offset;
	};
	// A mapping method 12 (Map) made into the client's task: what IOKit's io_connect_map_memory_into_task keeps in the
	// client's mapping set, kept here (that set is IOKit's own). Unmap and close release it, and its release unmaps it.
	struct Mapping {
		Mapping *next;
		IOMemoryMap *map;
		uint32_t token;
		uint64_t address;
	};

	IOLock *lock_ = nullptr;
	task_t task_ = nullptr;
	tgm_vm *vm_ = nullptr;
	volatile uint32_t *fence_va_ =
		nullptr;  // tgm_fence_page_get: the GPU present maps every connection's
	uint64_t fence_pa_ = 0, fence_gpu_ = 0;
	tgm_mem *fence_mem_ = nullptr;
	IOMemoryDescriptor *fence_view_ = nullptr;
	tgm_chan *chans_[kMaxChannels] = {};
	Bo *bos_ = nullptr, *zombies_ = nullptr;
	Mapping *maps_ = nullptr;  // under lock_
	Binding *binds_ = nullptr;
	uint32_t nbinds_ = 0, cap_binds_ = 0;
	uint64_t next_handle_ = 1;
	uint32_t next_token_ = 1;
	bool counted_ = false;
	bool closing_ = false;  // clientClose/free: BOs still mapped are expected (a dying task's)
	char owner_[17] = {};   // the opening process
	bool critical_ =
		true;  // its GPU errors panic; no pool quota (all but the compositor with all-clients)
	bool dead_ = false;  // a channel was killed for a GPU error: no new work, BOs or binds
	bool quotaLogged_ = false;
	uint64_t pool_bytes_ = 0;  // pool BOs held

	Bo *findBo(uint64_t handle);
	Bo *findToken(uint32_t token);
	void releaseBo(Bo *bo);
	void reap(bool force);
	void unbindOverlaps(uint64_t va, uint64_t range, Bo *only);
	void scanPushes(const tgm_push *pushes, uint32_t count);
	IOReturn run(uint32_t method, const void *in, uint32_t inSize, void *out,
		     uint32_t *outSize);

	IOReturn getInfo(TegraGM20BAbiInfo *out);
	IOReturn boAlloc(const TegraGM20BAbiBoAlloc *in, TegraGM20BAbiBoAllocReply *out);
	IOReturn boImport(const TegraGM20BAbiBoImport *in, TegraGM20BAbiBoAllocReply *out);
	IOReturn boFree(const TegraGM20BAbiBoFree *in);
	IOReturn vmBind(const TegraGM20BAbiVmBind *in, uint32_t count);
	IOReturn channelCreate(const TegraGM20BAbiChannelCreate *in,
			       TegraGM20BAbiChannelCreateReply *out);
	IOReturn channelDestroy(const TegraGM20BAbiChannel *in);
	IOReturn exec(const TegraGM20BAbiExec *in, uint32_t count, TegraGM20BAbiExecReply *out);
	IOReturn waitFence(const TegraGM20BAbiWaitFence *in);
	IOReturn getStatus(const TegraGM20BAbiChannel *in, TegraGM20BAbiStatus *out);
	IOReturn mapToken(const TegraGM20BAbiMap *in, TegraGM20BAbiMapReply *out);
	IOReturn order(const TegraGM20BAbiOrder *in, TegraGM20BAbiOrderReply *out);
	IOReturn unmapToken(const TegraGM20BAbiUnmap *in);
	void releaseMappings();

public:
	bool setUp(task_t task);
	void countConnection()
	{
		counted_ = true;
	}
	IOReturn clientClose() APPLE_KEXT_OVERRIDE;
	void free() APPLE_KEXT_OVERRIDE;
	IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options,
				     IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *arguments,
				IOExternalMethodDispatch *dispatch, OSObject *target,
				void *reference) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(AGXDeviceUserClient, IOUserClient)

	// ------------------------------------------------------------------ lifetime
	bool AGXDeviceUserClient::setUp(task_t task)
{
	task_ = task;
	proc_selfname(owner_, sizeof(owner_));
	critical_ = !tgm_all_clients() || !strcmp(owner_, "backboardd");
	lock_ = IOLockAlloc();
	if (!lock_ || tgm_vm_new(&vm_))
		return false;
	// fence page: pool memory (write-combined), GPU-mapped in the kernel range, read-only to the client
	if (tgm_fence_page_get(&fence_va_, &fence_pa_) ||
	    tgm_mem_new(vm_, &fence_pa_, 1, &fence_mem_) ||
	    tgm_vm_kmap(vm_, fence_mem_, 4096, &fence_gpu_))
		return false;
	IOAddressRange range = {fence_pa_, 4096};
	fence_view_ = IOMemoryDescriptor::withAddressRanges(
		&range, 1,
		static_cast<IOOptionBits>(kIODirectionIn) |
			static_cast<IOOptionBits>(kIOMemoryTypePhysical64) | kPoolCacheMode,
		TASK_NULL);
	return fence_view_ != nullptr;
}

IOReturn AGXDeviceUserClient::clientClose()
{
	releaseMappings();  // first: a view still mapped would keep its pool pages as an orphan (releaseBo)
	IOLockLock(lock_);
	closing_ = true;
	for (uint32_t i = 0; i < kMaxChannels; i++)
		tgm_chan_del(
			&chans_[i]);  // bounded wait, then nvkm preempts and removes it from the runlist
	while (nbinds_)
		unbindOverlaps(binds_[0].va, binds_[0].range, nullptr);
	while (bos_) {
		Bo *bo = bos_;
		bos_ = bo->next;
		bo->next = zombies_;
		zombies_ = bo;
	}
	reap(true);  // no channel is left, so nothing can touch the pages any more
	IOLockUnlock(lock_);
	scheduleOrphanRelease();  // not here: an import of one of these views may be on a connection not closed yet
	terminate();
	return kIOReturnSuccess;
}

void AGXDeviceUserClient::free()
{
	if (lock_) {
		releaseMappings();
		closing_ = true;
		for (uint32_t i = 0; i < kMaxChannels; i++)
			tgm_chan_del(&chans_[i]);
		while (bos_) {
			Bo *bo = bos_;
			bos_ = bo->next;
			releaseBo(bo);
		}
		reap(true);
		scheduleOrphanRelease();
		if (binds_)
			IODelete(binds_, Binding, cap_binds_);
		if (fence_view_)
			fence_view_->release();
		tgm_mem_del(&fence_mem_);
		tgm_vm_del(&vm_);
		tgm_fence_page_put(fence_va_);
		IOLockFree(lock_);
		lock_ = nullptr;
	}
	if (counted_)
		__atomic_sub_fetch(&gConnections, 1, __ATOMIC_SEQ_CST);
	IOUserClient::free();
}

// ------------------------------------------------------------------ BOs
AGXDeviceUserClient::Bo *AGXDeviceUserClient::findBo(uint64_t handle)
{
	for (Bo *bo = bos_; bo; bo = bo->next)
		if (bo->handle == handle)
			return bo;
	return nullptr;
}

AGXDeviceUserClient::Bo *AGXDeviceUserClient::findToken(uint32_t token)
{
	for (Bo *bo = bos_; bo; bo = bo->next)
		if (bo->token && bo->token == token)
			return bo;
	return nullptr;
}

void AGXDeviceUserClient::releaseBo(Bo *bo)
{
	tgm_mem_del(&bo->mem);
	if (bo->pool_va && bo->handle) {
		pool_bytes_ -= bo->bytes;
		if (critical_)
			tgm_stats_compositor_pool(-static_cast<int64_t>(bo->bytes));
	}
	// A pool BO's view and pages go to the thread call (releaseOrphans: releasing the view can wait for an import on
	// another connection), which keeps the pages while a task still maps the view. Cached BOs need no such care: their
	// mappings hold the buffer itself, and releasing one waits for nothing.
	PoolOrphan *orphan = nullptr;
	if (bo->pool_va && bo->view && (orphan = IONew(PoolOrphan, 1))) {
		if (!closing_ && bo->view->getRetainCount() > 1)
			IOLog("tegra-gm20b: BoFree of BO %llu (%llu bytes) still mapped by its "
			      "task: pages kept until unmapped\n",
			      bo->handle, bo->bytes);
		*orphan = {nullptr, bo->view, bo->pool_va, bo->pool_pa, bo->bytes, bo->handle != 0};
		pushOrphan(orphan);
		scheduleOrphanRelease();
	} else {
		if (bo->view)
			bo->view->release();
		// a pool BO's pages are zeroed off this thread and go back clean; a cached BO no task maps any more goes to
		// the spares as it is (spareKeep: zeroed and cleaned by the thread call, or given back to the VM there)
		if (bo->pool_va &&
		    !(bo->handle && poolZeroLater(bo->pool_va, bo->pool_pa, bo->bytes)))
			tgm_pool_free(bo->pool_va, bo->bytes, bo->pool_pa);
	}
	if (bo->bmd && bo->handle && !(bo->flags & TegraGM20BBoContiguous) &&
	    spareKeep(bo->bmd, bo->pas, uint64_t(bo->backing_pages) << 12)) {
		bo->bmd = nullptr;
		bo->pas = nullptr;
	}
	if (bo->bmd) {
		bo->bmd->complete();
		bo->bmd->release();
	}
	if (bo->wired) {
		if (bo->handle) {
			tgm_stats_import(-static_cast<int64_t>(bo->bytes), bo->pas[0]);
			tgm_vm_import_note(vm_, bo->pas[0], 0);
		}
		tgm_coherency_put(bo->coh);
		bo->wired->complete();
		bo->wired->release();
	}
	if (bo->pas)
		IODelete(bo->pas, uint64_t, bo->backing_pages);
	IODelete(bo, Bo, 1);
}

// CONTRACT §4.1: a freed BO's pages go back only once every channel finished what it had submitted at BoFree time
// (or is dead and stopped). force: no channels remain.
void AGXDeviceUserClient::reap(bool force)
{
	for (Bo **p = &zombies_; *p;) {
		Bo *bo = *p;
		bool idle = true;
		for (uint32_t i = 0; i < kMaxChannels && !force; i++) {
			if (chans_[i] && !tgm_chan_error(chans_[i]) &&
			    static_cast<int32_t>(tgm_chan_completed(chans_[i]) - bo->snapshot[i]) <
				    0)
				idle = false;
		}
		if (idle) {
			*p = bo->next;
			releaseBo(bo);
		} else {
			p = &bo->next;
		}
	}
}

// The page list of a prepared descriptor: one getPhysicalSegment per physically contiguous run (each call is a walk of the
// descriptor's DMA state; per 4 KiB page that was 900 walks for a 1280x720 surface).
static IOReturn collectPages(IOMemoryDescriptor *md, uint64_t bytes, uint64_t *pas)
{
	for (uint64_t off = 0; off < bytes;) {
		IOByteCount len = 0;
		addr64_t pa = md->getPhysicalSegment(off, &len, kIOMemoryMapperNone);
		len &= ~0xfffULL;
		if (!len)
			len = 4096;  // a run shorter than a page: that page alone, as per-page collection took it
		if (len > bytes - off)
			len = bytes - off;
		// GM20B on Tegra210 addresses 34-bit physical memory (nvgpu platform_gk20a_tegra.c dma_mask DMA_BIT_MASK(34),
		// nouveau tegra iommu_bit 34): all of the Switch's RAM, including the bank above 4 GiB
		if (!pa || (pa & 0xfff) || pa + len > (1ULL << 34))
			return kIOReturnUnsupported;
		for (uint64_t at = 0; at < len; at += 4096)
			pas[(off + at) >> 12] = pa + at;
		off += len;
	}
	return kIOReturnSuccess;
}

// The page list of a wired kernel buffer (a cached BO), from its kernel mapping: one address translation per page.
static IOReturn collectKernelPages(const void *va, uint64_t bytes, uint64_t *pas)
{
	for (uint64_t off = 0; off < bytes; off += 4096) {
		const uint64_t pa = ml_vtophys(reinterpret_cast<vm_offset_t>(va) + off);
		if (!pa || (pa & 0xfff) || pa + 4096 > (1ULL << 34))
			return kIOReturnUnsupported;
		pas[off >> 12] = pa;
	}
	return kIOReturnSuccess;
}

IOReturn AGXDeviceUserClient::boAlloc(const TegraGM20BAbiBoAlloc *in,
				      TegraGM20BAbiBoAllocReply *out)
{
	if (!in->bytes || (in->bytes & 0xfff) || in->bytes > kMaxBoBytes || in->align_log2 < 12 ||
	    in->align_log2 > 21 ||
	    (in->flags & ~uint32_t(TegraGM20BBoCoherent | TegraGM20BBoContiguous)))
		return kIOReturnBadArgument;
	if (__atomic_load_n(&gPoolOrphans, __ATOMIC_RELAXED))
		scheduleOrphanRelease();  // not here: releasing a view can wait for another connection (releaseOrphans)
	const uint64_t start = nowNs();
	uint64_t phase[TGM_PHASES] = {},
		 mark = start;  // tegra-gpu-stats: kernel allocation and wiring, clean, pages, nvkm
	auto lap = [&](int i) {
		const uint64_t now = nowNs();
		phase[i] += now - mark;
		mark = now;
	};
	Bo *bo = IONew(Bo, 1);
	if (!bo)
		return kIOReturnNoMemory;
	bzero(bo, sizeof(*bo));
	bo->bytes = in->bytes;
	bo->flags = in->flags;
	bo->npages = static_cast<uint32_t>(in->bytes >> 12);
	bo->backing_pages = bo->npages;
	bo->pas = IONew(uint64_t, bo->npages);
	IOReturn ret = bo->pas ? kIOReturnSuccess : kIOReturnNoMemory;
	int kind = TGM_BO_CACHED;
	Spare *spare = nullptr;
	const char *refused = nullptr;
	if (!ret && (in->flags & TegraGM20BBoCoherent) && !critical_ &&
	    (refused = poolRefusal(in->bytes, pool_bytes_))) {
		if (!quotaLogged_)
			IOLog("tegra-gm20b: %s: GPU pool BoAlloc of %llu KiB refused: %s "
			      "(client holds %llu MiB of the pool, %llu MiB free); "
			      "further refusals not logged\n",
			      owner_, in->bytes >> 10, refused, pool_bytes_ >> 20,
			      tgm_pool_avail() >> 20);
		quotaLogged_ = true;
		ret = kIOReturnNoMemory;
	} else if (!ret && (in->flags & TegraGM20BBoCoherent)) {
		// write-combined pool memory: zeroed by the allocator, no cacheable alias (CONTRACT §3). No room: the freed
		// ranges the thread call has not zeroed yet are zeroed now, and the allocation tried again.
		bo->pool_va = tgm_pool_alloc(in->bytes, &bo->pool_pa);
		if (!bo->pool_va && poolZeroNow())
			bo->pool_va = tgm_pool_alloc(in->bytes, &bo->pool_pa);
		if (!bo->pool_va) {
			ret = kIOReturnNoMemory;
		} else {
			for (uint32_t i = 0; i < bo->npages; i++)
				bo->pas[i] = bo->pool_pa + (uint64_t(i) << 12);
			IOAddressRange range = {bo->pool_pa, bo->bytes};
			bo->view = IOMemoryDescriptor::withAddressRanges(
				&range, 1,
				static_cast<IOOptionBits>(kIODirectionInOut) |
					static_cast<IOOptionBits>(kIOMemoryTypePhysical64) |
					kPoolCacheMode,
				TASK_NULL);
			ret = bo->view ? kIOReturnSuccess : kIOReturnNoMemory;
		}
	} else if (!ret && !(in->flags & TegraGM20BBoContiguous) &&
		   (spare = spareTake(in->bytes))) {
		// a spare: zeroed and cleaned already, its page list made; the BO is its first pages
		IODelete(bo->pas, uint64_t, bo->npages);
		bo->pas = spare->pas;
		bo->backing_pages = static_cast<uint32_t>(spare->bytes >> 12);
		bo->bmd = spare->bmd;
		IODelete(spare, Spare, 1);
		kind = TGM_BO_SPARE;
	} else if (!ret) {
		IOOptionBits options = static_cast<IOOptionBits>(kIODirectionInOut) |
				       static_cast<IOOptionBits>(kIOMemoryMapperNone);
		// Made here only when no spare was ready (the class is kept warm from now on), at its class's size so that it
		// serves the class again once freed; the BO is its first pages.
		const uint32_t c =
			(in->flags & TegraGM20BBoContiguous) ? kClasses : classUp(in->bytes);
		const uint64_t backing = c < kClasses ? classBytes(c) : in->bytes;
		if (backing != in->bytes) {
			IODelete(bo->pas, uint64_t, bo->npages);
			bo->backing_pages = static_cast<uint32_t>(backing >> 12);
			bo->pas = IONew(uint64_t, bo->backing_pages);
		}
		if (in->flags & TegraGM20BBoContiguous)
			options |= kIOMemoryPhysicallyContiguous;
		// 34-bit, not 32: a mask below 4 GiB on a non-contiguous buffer makes IOKit use KMA_LOMEM, and arm64 XNU keeps
		// no low-page pool, so every such BO failed (10-30: NVK "device refused: OutOfMemory(Device)")
		// The alignment goes into the mask only for Contiguous BOs (ABI: "physical alignment only matters with
		// Contiguous"): IOBufferMemoryDescriptor takes the mask's low zero bits as the alignment, and any alignment
		// above the page size makes IOKernelAllocateWithPhysicalRestrict use kmem_alloc_contig, a physically
		// contiguous search that took up to 130 ms per BO on 11-04 for NVK's 64 KiB-aligned buffers. The GMMU maps
		// these page by page (bo->pas), so ordinary BOs need no physical alignment.
		const uint64_t mask = (in->flags & TegraGM20BBoContiguous)
					      ? 0x3ffffffffULL & ~((1ULL << in->align_log2) - 1)
					      : 0x3ffffffffULL;
		bo->bmd = bo->pas ? IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
					    kernel_task, options, backing, mask)
				  : nullptr;
		if (!bo->bmd || bo->bmd->prepare() != kIOReturnSuccess) {
			if (bo->bmd)
				bo->bmd->release();
			bo->bmd = nullptr;
			ret = kIOReturnNoMemory;
		} else {
			lap(0);
			// The pages come zeroed: the mask makes IOKit allocate with KMA_ZERO (IOKernelAllocateWithPhysicalRestrict),
			// and vm_page_alloc_list zero-fills each page through the cacheable physical aperture. Clean those zeros to
			// DRAM and drop the lines, so the GPU and a fresh client view see zeros and no dirty line is written back
			// later over what the GPU wrote. (A bzero here was a second pass over the whole buffer.)
			void *va = bo->bmd->getBytesNoCopy();
			cleanZeros(va, backing, false);
			lap(1);
			ret = collectKernelPages(va, backing, bo->pas);
			lap(2);
		}
	}
	// A cached BO's view, which its task maps: the descriptor itself, or its first in->bytes when it is a spare's or
	// a class's larger buffer.
	if (!ret && bo->bmd) {
		bo->view = bo->backing_pages == bo->npages
				   ? static_cast<IOMemoryDescriptor *>(bo->bmd)
				   : IOSubMemoryDescriptor::withSubRange(bo->bmd, 0, in->bytes,
									 kIODirectionInOut);
		if (bo->view == bo->bmd)
			bo->bmd->retain();
		else if (!bo->view)
			ret = kIOReturnNoMemory;
	}
	if (!ret)
		ret = ioreturn(tgm_mem_new(vm_, bo->pas, bo->npages, &bo->mem));
	lap(3);
	if (ret) {
		if (!quotaLogged_)
			IOLog("tegra-gm20b: BoAlloc %llu bytes flags %#x align 2^%u refused: %#x\n",
			      in->bytes, in->flags, in->align_log2, ret);
		releaseBo(bo);
		return ret;
	}
	if (bo->pool_va) {
		pool_bytes_ += bo->bytes;
		kind = TGM_BO_POOL;
		if (critical_)
			tgm_stats_compositor_pool(static_cast<int64_t>(bo->bytes));
	}
	tgm_stats_bo_alloc(bo->bytes, kind, mark - start, phase, owner_);
	ownerNote(owner_, bo->bytes, mark - start, kind == TGM_BO_CACHED);
	bo->handle = next_handle_++;
	bo->token = next_token_++;
	bo->next = bos_;
	bos_ = bo;
	out->handle = bo->handle;
	out->map_token = bo->token;
	out->reserved = 0;
	return kIOReturnSuccess;
}

IOReturn AGXDeviceUserClient::boImport(const TegraGM20BAbiBoImport *in,
				       TegraGM20BAbiBoAllocReply *out)
{
	if (!in->bytes || ((in->address | in->bytes) & 0xfff) || in->bytes > kMaxBoBytes)
		return kIOReturnBadArgument;
	const uint64_t start = nowNs();
	uint64_t phase[TGM_PHASES] = {},
		 mark = start;  // tegra-gpu-stats: wiring, pages, nvkm, clean
	auto lap = [&](int i) {
		const uint64_t now = nowNs();
		phase[i] += now - mark;
		mark = now;
	};
	Bo *bo = IONew(Bo, 1);
	if (!bo)
		return kIOReturnNoMemory;
	bzero(bo, sizeof(*bo));
	bo->bytes = in->bytes;
	bo->npages = static_cast<uint32_t>(in->bytes >> 12);
	bo->backing_pages = bo->npages;
	bo->pas = IONew(uint64_t, bo->npages);
	IOReturn ret = bo->pas ? kIOReturnSuccess : kIOReturnNoMemory,
		 wire[2] = {kIOReturnNoMemory, kIOReturnNoMemory};
	if (!ret) {
		// Wired for the GPU to write where the task's mapping allows that, else for it to read only (Core Animation
		// hands over image bytes it maps read-only); a read-only BO is mapped read-only in the GPU, so a GPU write to
		// it is an MMU fault (a panic naming it), never a store into memory the task cannot write.
		const IOOptionBits directions[2] = {kIODirectionInOut, kIODirectionOut};
		for (int i = 0; i < 2 && !bo->wired; i++) {
			bo->wired = IOMemoryDescriptor::withAddressRange(in->address, in->bytes,
									 directions[i], task_);
			wire[i] = bo->wired ? bo->wired->prepare() : kIOReturnNoMemory;
			if (wire[i] != kIOReturnSuccess)
				OSSafeReleaseNULL(bo->wired);
			bo->readOnly = i == 1;
		}
		lap(0);
		ret = bo->wired ? collectPages(bo->wired, in->bytes, bo->pas)
				: kIOReturnBadArgument;
		lap(1);
	}
	if (!ret)
		ret = ioreturn(tgm_mem_new(vm_, bo->pas, bo->npages, &bo->mem));
	lap(2);
	if (ret) {
		IOLog("tegra-gm20b: BoImport %llu bytes at %#llx refused: %#x (wiring for GPU "
		      "writes %#x, reads %#x)\n",
		      in->bytes, in->address, ret, wire[0], wire[1]);
		releaseBo(bo);
		return ret;
	}
	// Register the import for the lazy CPU<->GPU coherency the XNU IOSurface lock/unlock hook drives (tgm_coherency_*):
	// writable (the GPU may write it) unless the task maps the pages read-only.
	bo->coh = tgm_coherency_add(bo->pas[0], bo->pas, bo->npages, !bo->readOnly);
	// The GPU does not snoop the CPU caches: what the task wrote must be in DRAM before the GPU reads it, and no dirty
	// line (fresh pages are zeroed through the cache) may be written back later over what the GPU wrote. Always, even
	// when another import of these pages is live: that proves nothing about CPU writes since (gpu.c, the registry).
	tgm_cpu_clean_pages(bo->pas, bo->npages);
	lap(3);
	tgm_stats_import(static_cast<int64_t>(bo->bytes), bo->pas[0]);
	tgm_stats_bo_import(bo->bytes, bo->readOnly, 0, mark - start, phase);
	tgm_vm_import_note(
		vm_, bo->pas[0],
		1);  // this connection may draw into what it imports (the GPU present asks)
	bo->handle = next_handle_++;
	bo->next = bos_;
	bos_ = bo;
	out->handle = bo->handle;
	out->map_token = 0;
	out->reserved = 0;
	return kIOReturnSuccess;
}

IOReturn AGXDeviceUserClient::boFree(const TegraGM20BAbiBoFree *in)
{
	const uint64_t start = nowNs();
	Bo **p = &bos_;
	while (*p && (*p)->handle != in->handle)
		p = &(*p)->next;
	if (!*p)
		return kIOReturnBadArgument;
	Bo *bo = *p;
	*p = bo->next;
	for (uint32_t i = 0; i < nbinds_;) {
		if (binds_[i].bo == bo)
			unbindOverlaps(binds_[i].va, binds_[i].range, bo);
		else
			i++;
	}
	for (uint32_t i = 0; i < kMaxChannels; i++)
		bo->snapshot[i] = chans_[i] ? tgm_chan_submitted(chans_[i]) : 0;
	bo->next = zombies_;
	zombies_ = bo;
	reap(false);
	tgm_stats_bo_free(nowNs() - start);
	return kIOReturnSuccess;
}

// ------------------------------------------------------------------ VA binds
// Binds are tracked whole. A Map or Unmap that overlaps a recorded bind removes that whole bind first (NVK binds and
// unbinds whole VA ranges); this keeps nvkm's raw get/put page-table references balanced.
void AGXDeviceUserClient::unbindOverlaps(uint64_t va, uint64_t range, Bo *only)
{
	for (uint32_t i = 0; i < nbinds_;) {
		Binding &b = binds_[i];
		if (b.va < va + range && va < b.va + b.range && (!only || b.bo == only)) {
			tgm_vm_unmap(vm_, b.va, b.range);
			binds_[i] = binds_[--nbinds_];
		} else {
			i++;
		}
	}
}

IOReturn AGXDeviceUserClient::vmBind(const TegraGM20BAbiVmBind *in, uint32_t count)
{
	for (uint32_t i = 0; i < count; i++) {
		const TegraGM20BAbiBindOp &op = in->ops[i];
		if (((op.va | op.range | op.bo_offset) & 0xfff) || !op.range ||
		    op.va < TGM_USER_VA_START || op.range > TGM_KERNEL_VA ||
		    op.va > TGM_KERNEL_VA - op.range || op.kind > 0xff)
			return kIOReturnBadArgument;
		if (op.op == TegraGM20BBindMapSparse)
			return kIOReturnUnsupported;  // InfoSparse is not advertised in v1
		const uint64_t start = nowNs();
		if (op.op == TegraGM20BBindUnmap) {
			unbindOverlaps(op.va, op.range, nullptr);
			tgm_stats_bind(0, op.range >> 12, nowNs() - start);
			continue;
		}
		if (op.op != TegraGM20BBindMap)
			return kIOReturnBadArgument;
		Bo *bo = findBo(op.handle);
		if (!bo || op.bo_offset > bo->bytes || op.range > bo->bytes - op.bo_offset)
			return kIOReturnBadArgument;
		unbindOverlaps(op.va, op.range, nullptr);
		if (nbinds_ == cap_binds_) {
			uint32_t cap = cap_binds_ ? cap_binds_ * 2 : 64;
			Binding *grown = IONew(Binding, cap);
			if (!grown)
				return kIOReturnNoMemory;
			if (binds_) {
				memcpy(grown, binds_, sizeof(Binding) * nbinds_);
				IODelete(binds_, Binding, cap_binds_);
			}
			binds_ = grown;
			cap_binds_ = cap;
		}
		IOReturn ret = ioreturn(tgm_vm_map(vm_, op.va, op.range, bo->mem, op.bo_offset,
						   static_cast<uint8_t>(op.kind), bo->readOnly));
		if (ret)
			return ret;
		binds_[nbinds_++] = {op.va, op.range, bo, op.bo_offset};
		tgm_stats_bind(1, op.range >> 12, nowNs() - start);
	}
	return kIOReturnSuccess;
}

// ------------------------------------------------------------------ channels
IOReturn AGXDeviceUserClient::channelCreate(const TegraGM20BAbiChannelCreate *in,
					    TegraGM20BAbiChannelCreateReply *out)
{
	(void)in;  // engines is advisory: every channel is a full GR channel (CONTRACT §2)
	uint32_t slot = 0;
	while (slot < kMaxChannels && chans_[slot])
		slot++;
	if (slot == kMaxChannels)
		return kIOReturnNoResources;
	// not reset: a slot's seqno only moves forward (tgm_chan_new continues it)
	volatile uint32_t *fence = fence_va_ + slot * 4;
	IOReturn ret = ioreturn(
		tgm_chan_new(vm_, fence, fence_gpu_ + slot * 16, owner_, critical_, &chans_[slot]));
	if (ret)
		return ret;
	tgm_chan_set_group(chans_[slot], task_);  // Order relates the channels of one task
	out->channel = slot + 1;
	out->fence_token = kFenceToken;
	out->fence_offset = slot * 16;
	out->reserved = 0;
	return kIOReturnSuccess;
}

// Method 14: GPU-side order between this channel and the task's other channels (gpu.c tgm_chan_order); the peers are found by
// task in gpu.c, only the caller's own channel is named here.
IOReturn AGXDeviceUserClient::order(const TegraGM20BAbiOrder *in, TegraGM20BAbiOrderReply *out)
{
	static_assert(TegraGM20BOrderAfterTask == TGM_ORDER_AFTER &&
			      TegraGM20BOrderBeforeTask == TGM_ORDER_BEFORE &&
			      TegraGM20BMaxOrderWaits == TGM_ORDER_MAX,
		      "order flags");
	if (!in->channel || in->channel > kMaxChannels || !chans_[in->channel - 1])
		return kIOReturnBadArgument;
	uint32_t queued = 0;
	const IOReturn ret =
		ioreturn(tgm_chan_order(chans_[in->channel - 1], in->flags, in->seqno, &queued));
	out->waits = queued;
	out->reserved = 0;
	return ret;
}

IOReturn AGXDeviceUserClient::channelDestroy(const TegraGM20BAbiChannel *in)
{
	if (!in->channel || in->channel > kMaxChannels || !chans_[in->channel - 1])
		return kIOReturnBadArgument;
	tgm_chan_del(&chans_[in->channel - 1]);
	reap(false);
	return kIOReturnSuccess;
}

// tegra-gpu-stats (see pushScanOn): count what this submission's pushes ask of the GPU, through the kernel's view of the
// BOs they lie in (pool BOs write-combined, cached BOs their kernel mapping); every 5 s the window's line.
void AGXDeviceUserClient::scanPushes(const tgm_push *pushes, uint32_t count)
{
	PushCounts c = {};
	PushParse p = {};
	const Binding *hit = nullptr;
	uint64_t budget = kScanMaxBytes;
	for (uint32_t i = 0; i < count; i++) {
		const uint64_t va = pushes[i].va, bytes = uint64_t(pushes[i].dwords) * 4;
		c.bytes += bytes;
		if (!hit || va < hit->va || va + bytes > hit->va + hit->range) {
			hit = nullptr;
			for (uint32_t b = 0; b < nbinds_ && !hit; b++)
				if (va >= binds_[b].va &&
				    va + bytes <= binds_[b].va + binds_[b].range)
					hit = &binds_[b];
		}
		const void *base = !hit               ? nullptr
				   : hit->bo->pool_va ? hit->bo->pool_va
				   : hit->bo->bmd     ? hit->bo->bmd->getBytesNoCopy()
						      : nullptr;
		if (!base || bytes > budget) {
			p.left =
				0;  // a method straddling into a push not read is lost, not misread
			continue;
		}
		budget -= bytes;
		c.readBytes += bytes;
		pushParse(c, p,
			  reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(base) +
							     hit->bo_offset + (va - hit->va)),
			  bytes / 4);
	}
	c.scanned = 1;
	for (size_t k = 0; k < sizeof(c) / sizeof(uint64_t); k++)
		__atomic_fetch_add(&reinterpret_cast<uint64_t *>(&gPush)[k],
				   reinterpret_cast<uint64_t *>(&c)[k], __ATOMIC_RELAXED);

	const uint64_t now = nowNs();
	uint64_t since = __atomic_load_n(&gPushSince, __ATOMIC_RELAXED);
	if (!since) {
		__atomic_compare_exchange_n(&gPushSince, &since, now, false, __ATOMIC_RELAXED,
					    __ATOMIC_RELAXED);
		return;
	}
	if (now - since < kScanWindowNs ||
	    !__atomic_compare_exchange_n(&gPushSince, &since, now, false, __ATOMIC_RELAXED,
					 __ATOMIC_RELAXED))
		return;
	PushCounts w;
	for (size_t k = 0; k < sizeof(w) / sizeof(uint64_t); k++)
		reinterpret_cast<uint64_t *>(&w)[k] = __atomic_exchange_n(
			&reinterpret_cast<uint64_t *>(&gPush)[k], 0, __ATOMIC_RELAXED);
	const uint64_t n = w.scanned ? w.scanned : 1;
	char v[15][24];
	const uint64_t per[15] = {w.passes,      w.targets,     w.linearTargets, w.draws,
				  w.wfi,         w.texInval,    w.shaderInval,   w.clears,
				  w.ceCopies,    w.blits,       w.dispatches,    w.acquires,
				  w.hostFlushes, w.bytes >> 10, w.ceBytes >> 10};
	for (int k = 0; k < 15; k++)
		snprintf(v[k], sizeof(v[k]), "%llu.%llu", per[k] / n, per[k] * 10 / n % 10);
	// one string argument: os_log drops a message whose scalar arguments outgrow its buffer (gpu.c stats_tick)
	char line[600];
	snprintf(line, sizeof(line),
		 "%llu/%llu submits read, %s KiB each (%llu%% readable); per submit: %s passes, %s "
		 "RTs "
		 "(%s pitch), %s draws, %s WFI, %s texinv, %s shinv, %s clears, %s CE (%s KiB), %s "
		 "2D, %s disp, %s acq, %s host; "
		 "%llu bad",
		 w.scanned, w.submits, v[13], w.bytes ? w.readBytes * 100 / w.bytes : 0, v[0], v[1],
		 v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[14], v[9], v[10], v[11], v[12],
		 w.unparsed);
	os_log(OS_LOG_DEFAULT, "tegra-gm20b: push %s", line); /* log only */
}

IOReturn AGXDeviceUserClient::exec(const TegraGM20BAbiExec *in, uint32_t count,
				   TegraGM20BAbiExecReply *out)
{
	if (!in->channel || in->channel > kMaxChannels || !chans_[in->channel - 1])
		return kIOReturnBadArgument;
	tgm_chan *chan = chans_[in->channel - 1];
	// TegraGM20BAbiPush and tgm_push share one layout; `in` is our private copy, so normalise flags in place
	static_assert(sizeof(tgm_push) == sizeof(TegraGM20BAbiPush) &&
			      offsetof(tgm_push, no_prefetch) == offsetof(TegraGM20BAbiPush, flags),
		      "push layouts");
	tgm_push *pushes =
		reinterpret_cast<tgm_push *>(const_cast<TegraGM20BAbiPush *>(in->pushes));
	for (uint32_t i = 0; i < count; i++) {
		const TegraGM20BAbiPush &p = in->pushes[i];
		if ((p.va & 3) || !p.dwords || p.dwords >= (1u << 21) ||
		    (p.flags & ~uint32_t(TegraGM20BPushNoPrefetch)) || p.va < TGM_USER_VA_START ||
		    p.va + uint64_t(p.dwords) * 4 > TGM_KERNEL_VA)
			return kIOReturnBadArgument;
		pushes[i].no_prefetch = (p.flags & TegraGM20BPushNoPrefetch) ? 1u : 0u;
	}
	if (count && importSync()) {
		for (Bo *bo = bos_; bo; bo = bo->next)
			if (bo->wired)
				tgm_cpu_clean_pages(bo->pas, bo->npages);
	} else if (count) {
		// Mark every writable host import this submission may have written GPU-dirty (cheap flag stores, no cache
		// maintenance): the reader's next IOSurface read lock invalidates it then (tgm_coherency_*, the XNU hook). A read
		// lock between now and the fence completing would clear the mark early, but a CPU readback waits for completion.
		for (Bo *bo = bos_; bo; bo = bo->next)
			if (bo->coh && !bo->readOnly)
				tgm_coherency_wrote(bo->coh);
	}
	if (count && pushScanOn() &&
	    __atomic_fetch_add(&gPush.submits, 1, __ATOMIC_RELAXED) % kScanEvery == 0)
		scanPushes(pushes, count);
	uint32_t seqno = 0;
	IOReturn ret = ioreturn(tgm_chan_exec(chan, pushes, count, &seqno));
	reap(false);
	if (ret)
		return ret;
	out->seqno = seqno;
	out->reserved = 0;
	return kIOReturnSuccess;
}

// Sleeps without the connection lock (a waiter must not block submitters), so the channel can be destroyed meanwhile:
// it is looked up again, under the lock, before every look at it.
IOReturn AGXDeviceUserClient::waitFence(const TegraGM20BAbiWaitFence *in)
{
	if (!in->channel || in->channel > kMaxChannels)
		return kIOReturnBadArgument;
	const uint64_t limit_ns = in->timeout_ns == UINT64_MAX ? 10000000000ULL : in->timeout_ns;
	const uint64_t start = nowNs();
	const uint64_t deadline = limit_ns > UINT64_MAX - start ? UINT64_MAX : start + limit_ns;
	for (bool slept = false;; slept = true) {
		const uint32_t seq = tgm_wake_seq();
		IOReturn ret = kIOReturnNotReady;
		IOLockLock(lock_);
		tgm_chan *chan = chans_[in->channel - 1];
		const uint64_t now = nowNs();
		if (!chan)
			ret = slept ? kIOReturnNotResponding : kIOReturnBadArgument;
		else if (tgm_chan_error(chan))
			ret = kIOReturnNotResponding;
		else if (static_cast<int32_t>(tgm_chan_completed(chan) - in->seqno) >= 0)
			ret = kIOReturnSuccess;
		else if (in->timeout_ns == UINT64_MAX && now - start >= limit_ns)
			tgm_chan_hung(chan, "fence wait", in->seqno,
				      now - start);  // CONTRACT: UINT64_MAX means 10 s
		else if (!limit_ns || now - start >= limit_ns) {
			if (limit_ns)
				tgm_chan_check_progress(chan, "fence wait", in->seqno);
			ret = kIOReturnTimeout;
		}
		IOLockUnlock(lock_);
		if (ret != kIOReturnNotReady)
			return ret;
		if (!tgm_idle(seq, deadline)) {
			IOLockLock(lock_);
			chan = chans_[in->channel - 1];
			const bool unwoken = chan && static_cast<int32_t>(tgm_chan_completed(chan) -
									  in->seqno) >= 0;
			IOLockUnlock(lock_);
			if (unwoken)
				tgm_fence_unwoken(seq, in->seqno);
		}
	}
}

IOReturn AGXDeviceUserClient::getStatus(const TegraGM20BAbiChannel *in, TegraGM20BAbiStatus *out)
{
	if (!in->channel || in->channel > kMaxChannels || !chans_[in->channel - 1])
		return kIOReturnBadArgument;
	tgm_chan *chan = chans_[in->channel - 1];
	bzero(out, sizeof(*out));
	out->completed = tgm_chan_completed(chan);
	out->submitted = tgm_chan_submitted(chan);
	out->error = tgm_chan_error(chan);
	return kIOReturnSuccess;
}

// ------------------------------------------------------------------ CPU mappings (methods 12, 13)
// What io_connect_map_memory_into_task and io_connect_unmap_memory_from_task do for a task mapping its own connection's
// memory (IOUserClient::mapClientMemory64 over clientMemoryForType, the same options, the map kept until unmapped or the
// connection closes), reached through io_connect_method, which WebKit's GPU process profile admits where it refuses those two
// MIG routines (21H461 syscall-mig list). The same kernel work as IOConnectMapMemory64: nothing is slower.
IOReturn AGXDeviceUserClient::mapToken(const TegraGM20BAbiMap *in, TegraGM20BAbiMapReply *out)
{
	const uint32_t known = TegraGM20BMapReadOnly | TegraGM20BMapFixed;
	const bool fixed = in->flags & TegraGM20BMapFixed;
	if (!in->token || (in->flags & ~known) || fixed != (in->address != 0))
		return kIOReturnBadArgument;
	if (current_task() != task_)
		return kIOReturnNotPermitted;  // only the opener maps its connection's memory, into itself
	Mapping *mapping = IONew(Mapping, 1);
	if (!mapping)
		return kIOReturnNoMemory;
	IOOptionBits options = fixed ? kIOMapOverwrite : kIOMapAnywhere;
	if (in->flags & TegraGM20BMapReadOnly)
		options |= kIOMapReadOnly;
	// clientMemoryForType takes lock_: called without it, as IOKit's own map path calls it
	IOMemoryMap *map = mapClientMemory64(in->token, task_, options, in->address);
	if (!map) {
		IODelete(mapping, Mapping, 1);
		return kIOReturnBadArgument;  // IOConnectMapMemory64's answer
	}
	out->address = map->getAddress();
	out->bytes = map->getSize();
	IOLockLock(lock_);
	*mapping = {maps_, map, in->token, out->address};
	maps_ = mapping;
	IOLockUnlock(lock_);
	return kIOReturnSuccess;
}

IOReturn AGXDeviceUserClient::unmapToken(const TegraGM20BAbiUnmap *in)
{
	Mapping *found = nullptr;
	IOLockLock(lock_);
	for (Mapping **p = &maps_; *p; p = &(*p)->next) {
		if ((*p)->token == in->token && (*p)->address == in->address) {
			found = *p;
			*p = found->next;
			break;
		}
	}
	IOLockUnlock(lock_);
	if (!found)
		return kIOReturnBadArgument;
	found->map->unmap();
	found->map->release();
	IODelete(found, Mapping, 1);
	return kIOReturnSuccess;
}

// Every mapping method 12 made, unmapped (close and free: the task's mappings go with its connection).
void AGXDeviceUserClient::releaseMappings()
{
	IOLockLock(lock_);
	Mapping *list = maps_;
	maps_ = nullptr;
	IOLockUnlock(lock_);
	while (list) {
		Mapping *next = list->next;
		list->map->unmap();
		list->map->release();
		IODelete(list, Mapping, 1);
		list = next;
	}
}

IOReturn AGXDeviceUserClient::getInfo(TegraGM20BAbiInfo *out)
{
	tgm_info info;
	tgm_get_info(&info);
	bzero(out, sizeof(*out));
	out->abi_version = TegraGM20BAbiVersion;
	out->flags = TegraGM20BInfoHostImport | TegraGM20BInfoKernelMap | TegraGM20BInfoOrder;
	out->chipset = info.chipset;
	out->device_id = 0;
	out->gpc_count = info.gpc_count;
	out->tpc_count = info.tpc_count;
	out->cls_copy = 0xb0b5;
	out->cls_eng2d = 0x902d;
	out->cls_eng3d = 0xb197;
	out->cls_m2mf = 0xa140;
	out->cls_compute = 0xb1c0;
	out->cls_gpfifo = 0xb06f;
	out->va_start = TGM_USER_VA_START;
	out->va_end = TGM_KERNEL_VA;
	out->page_size = 4096;
	out->big_page_size = 0x20000;
	out->max_bo_bytes = kMaxBoBytes;
	strlcpy(out->chip_name, "GM20B", sizeof(out->chip_name));
	strlcpy(out->device_name, "NVIDIA Tegra X1 (GM20B)", sizeof(out->device_name));
	return kIOReturnSuccess;
}

// ------------------------------------------------------------------ transport
template <typename T> static bool reservedZero(const T &v)
{
	return v == 0;
}

IOReturn AGXDeviceUserClient::run(uint32_t method, const void *in, uint32_t inSize, void *out,
				  uint32_t *outSize)
{
	// exact record sizes; VmBind/Exec also accept just `count` entries
	uint32_t wantIn = 0, wantOut = 0, count = 0;
	switch (method) {
	case TegraGM20BMethodGetInfo:
		wantOut = sizeof(TegraGM20BAbiInfo);
		break;
	case TegraGM20BMethodBoAlloc:
		wantIn = sizeof(TegraGM20BAbiBoAlloc);
		wantOut = sizeof(TegraGM20BAbiBoAllocReply);
		break;
	case TegraGM20BMethodBoImport:
		wantIn = sizeof(TegraGM20BAbiBoImport);
		wantOut = sizeof(TegraGM20BAbiBoAllocReply);
		break;
	case TegraGM20BMethodBoFree:
		wantIn = sizeof(TegraGM20BAbiBoFree);
		break;
	case TegraGM20BMethodVmBind:
		if (inSize < offsetof(TegraGM20BAbiVmBind, ops))
			return kIOReturnBadArgument;
		count = static_cast<const TegraGM20BAbiVmBind *>(in)->count;
		if (!count || count > TegraGM20BMaxBindOps)
			return kIOReturnBadArgument;
		wantIn = inSize == sizeof(TegraGM20BAbiVmBind)
				 ? inSize
				 : uint32_t(offsetof(TegraGM20BAbiVmBind, ops) +
					    count * sizeof(TegraGM20BAbiBindOp));
		break;
	case TegraGM20BMethodChannelCreate:
		wantIn = sizeof(TegraGM20BAbiChannelCreate);
		wantOut = sizeof(TegraGM20BAbiChannelCreateReply);
		break;
	case TegraGM20BMethodChannelDestroy:
		wantIn = sizeof(TegraGM20BAbiChannel);
		break;
	case TegraGM20BMethodExec:
		if (inSize < offsetof(TegraGM20BAbiExec, pushes))
			return kIOReturnBadArgument;
		count = static_cast<const TegraGM20BAbiExec *>(in)->count;
		if (count > TegraGM20BMaxPushes)
			return kIOReturnBadArgument;
		wantIn = inSize == sizeof(TegraGM20BAbiExec)
				 ? inSize
				 : uint32_t(offsetof(TegraGM20BAbiExec, pushes) +
					    count * sizeof(TegraGM20BAbiPush));
		wantOut = sizeof(TegraGM20BAbiExecReply);
		break;
	case TegraGM20BMethodWaitFence:
		wantIn = sizeof(TegraGM20BAbiWaitFence);
		break;
	case TegraGM20BMethodGetTimestamp:
		wantOut = sizeof(TegraGM20BAbiTimestamp);
		break;
	case TegraGM20BMethodGetStatus:
		wantIn = sizeof(TegraGM20BAbiChannel);
		wantOut = sizeof(TegraGM20BAbiStatus);
		break;
	case TegraGM20BMethodMap:
		wantIn = sizeof(TegraGM20BAbiMap);
		wantOut = sizeof(TegraGM20BAbiMapReply);
		break;
	case TegraGM20BMethodUnmap:
		wantIn = sizeof(TegraGM20BAbiUnmap);
		break;
	case TegraGM20BMethodOrder:
		wantIn = sizeof(TegraGM20BAbiOrder);
		wantOut = sizeof(TegraGM20BAbiOrderReply);
		break;
	default:
		return kIOReturnUnsupported;
	}
	if (inSize != wantIn || *outSize != wantOut || (wantIn && !in) || (wantOut && !out))
		return kIOReturnBadArgument;
	if (tgm_stage() < 2)
		return kIOReturnNotReady;

	// reserved fields must be zero
	switch (method) {
	case TegraGM20BMethodVmBind:
		if (!reservedZero(static_cast<const TegraGM20BAbiVmBind *>(in)->reserved))
			return kIOReturnBadArgument;
		break;
	case TegraGM20BMethodChannelCreate:
		if (!reservedZero(static_cast<const TegraGM20BAbiChannelCreate *>(in)->reserved))
			return kIOReturnBadArgument;
		break;
	case TegraGM20BMethodChannelDestroy:
	case TegraGM20BMethodGetStatus:
		if (!reservedZero(static_cast<const TegraGM20BAbiChannel *>(in)->reserved))
			return kIOReturnBadArgument;
		break;
	case TegraGM20BMethodUnmap:
		if (!reservedZero(static_cast<const TegraGM20BAbiUnmap *>(in)->reserved))
			return kIOReturnBadArgument;
		break;
	case TegraGM20BMethodOrder:
		if (!reservedZero(static_cast<const TegraGM20BAbiOrder *>(in)->reserved))
			return kIOReturnBadArgument;
		break;
	default:
		break;
	}

	if (method == TegraGM20BMethodWaitFence)
		return waitFence(static_cast<const TegraGM20BAbiWaitFence *>(in));
	// Map and Unmap take lock_ themselves (Map's clientMemoryForType must run without it); a dead connection still maps and
	// unmaps what it has, as IOConnectMapMemory64 would.
	if (method == TegraGM20BMethodMap)
		return mapToken(static_cast<const TegraGM20BAbiMap *>(in),
				static_cast<TegraGM20BAbiMapReply *>(out));
	if (method == TegraGM20BMethodUnmap)
		return unmapToken(static_cast<const TegraGM20BAbiUnmap *>(in));
	if (method == TegraGM20BMethodGetTimestamp) {
		static_cast<TegraGM20BAbiTimestamp *>(out)->ns = tgm_timestamp_ns();
		return kIOReturnSuccess;
	}
	IOLockLock(lock_);
	// a channel of this connection was killed for a GPU error (only with tegra-gpu-all-clients): the connection takes no
	// new work, memory or binds; status, fence waits, frees and teardown still answer, so the client sees the error and
	// fails or falls back
	for (uint32_t i = 0; i < kMaxChannels && !dead_; i++)
		dead_ = chans_[i] && tgm_chan_error(chans_[i]) && tgm_stage() >= 3;
	if (dead_ && (method == TegraGM20BMethodBoAlloc || method == TegraGM20BMethodBoImport ||
		      method == TegraGM20BMethodVmBind || method == TegraGM20BMethodChannelCreate ||
		      method == TegraGM20BMethodExec || method == TegraGM20BMethodOrder)) {
		IOLockUnlock(lock_);
		return kIOReturnNotResponding;
	}
	IOReturn ret;
	switch (method) {
	case TegraGM20BMethodGetInfo:
		ret = getInfo(static_cast<TegraGM20BAbiInfo *>(out));
		break;
	case TegraGM20BMethodBoAlloc:
		ret = boAlloc(static_cast<const TegraGM20BAbiBoAlloc *>(in),
			      static_cast<TegraGM20BAbiBoAllocReply *>(out));
		break;
	case TegraGM20BMethodBoImport:
		ret = boImport(static_cast<const TegraGM20BAbiBoImport *>(in),
			       static_cast<TegraGM20BAbiBoAllocReply *>(out));
		break;
	case TegraGM20BMethodBoFree:
		ret = boFree(static_cast<const TegraGM20BAbiBoFree *>(in));
		break;
	case TegraGM20BMethodVmBind:
		ret = vmBind(static_cast<const TegraGM20BAbiVmBind *>(in), count);
		break;
	case TegraGM20BMethodChannelCreate:
		ret = channelCreate(static_cast<const TegraGM20BAbiChannelCreate *>(in),
				    static_cast<TegraGM20BAbiChannelCreateReply *>(out));
		break;
	case TegraGM20BMethodChannelDestroy:
		ret = channelDestroy(static_cast<const TegraGM20BAbiChannel *>(in));
		break;
	case TegraGM20BMethodExec:
		ret = exec(static_cast<const TegraGM20BAbiExec *>(in), count,
			   static_cast<TegraGM20BAbiExecReply *>(out));
		break;
	case TegraGM20BMethodGetStatus:
		ret = getStatus(static_cast<const TegraGM20BAbiChannel *>(in),
				static_cast<TegraGM20BAbiStatus *>(out));
		break;
	case TegraGM20BMethodOrder:
		ret = order(static_cast<const TegraGM20BAbiOrder *>(in),
			    static_cast<TegraGM20BAbiOrderReply *>(out));
		break;
	default:
		ret = kIOReturnUnsupported;
		break;
	}
	IOLockUnlock(lock_);
	return ret;
}

IOReturn AGXDeviceUserClient::externalMethod(uint32_t selector,
					     IOExternalMethodArguments *arguments,
					     IOExternalMethodDispatch *dispatch, OSObject *target,
					     void *reference)
{
	(void)dispatch;
	(void)target;
	(void)reference;
	if (!arguments || arguments->structureInputDescriptor ||
	    arguments->structureOutputDescriptor || arguments->scalarInputCount ||
	    arguments->scalarOutputCount)
		return kIOReturnBadArgument;  // every record is inline (<= 4096 bytes)
	const uint8_t *in = static_cast<const uint8_t *>(arguments->structureInput);
	uint32_t inSize = arguments->structureInputSize, outSize = arguments->structureOutputSize;
	uint32_t method = selector;
	// tunnel: {magic, method, record...} on any selector the sandbox admits
	if (in && inSize >= 8 &&
	    reinterpret_cast<const uint32_t *>(in)[0] == TegraGM20BTunnelMagic) {
		method = reinterpret_cast<const uint32_t *>(in)[1];
		if (method == TegraGM20BTunnelProbe)
			return kIOReturnSuccess;
		in += 8;
		inSize -= 8;
	}
	// copy the input: records are naturally aligned structs and the caller's buffer may not be. Small records use the
	// stack; VmBind/Exec (up to 4 KiB) a heap buffer, keeping kernel stack use small.
	uint64_t small[64];
	void *copy = small;
	if (inSize > TegraGM20BMaxPushes * sizeof(TegraGM20BAbiPush) + 8)
		return kIOReturnBadArgument;
	if (inSize > sizeof(small) && !(copy = IOMallocData(inSize)))
		return kIOReturnNoMemory;
	if (inSize)
		memcpy(copy, in, inSize);
	const IOReturn ret =
		run(method, inSize ? copy : nullptr, inSize, arguments->structureOutput, &outSize);
	if (copy != small)
		IOFreeData(copy, inSize);
	arguments->structureOutputSize = ret ? 0 : outSize;
	return ret;
}

IOReturn AGXDeviceUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options,
						  IOMemoryDescriptor **memory)
{
	IOLockLock(lock_);
	IOReturn ret = kIOReturnBadArgument;
	if (type == kFenceToken && fence_view_) {
		fence_view_->retain();
		*memory = fence_view_;
		*options = kIOMapAnywhere | kIOMapReadOnly | kIOMapWriteCombineCache;
		ret = kIOReturnSuccess;
	} else if (Bo *bo = findToken(type)) {
		bo->view->retain();
		*memory = bo->view;
		*options = kIOMapAnywhere |
			   ((bo->flags & TegraGM20BBoCoherent) ? kIOMapWriteCombineCache
							       : kIOMapDefaultCache);
		// Pool views are physical: IOKit enters their pages at map time already (populateDevicePager). A cached BO's
		// buffer is prepared (BoAlloc), which kIOMapPrefault requires.
		if (bo->bmd && bo->view == bo->bmd && bo->bytes <= kPrefaultMaxBytes) {
			*options |= kIOMapPrefault;
			tgm_stats_prefault(bo->bytes);
		}
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(lock_);
	return ret;
}

// Called by AGXAccelerator::newUserClient (TegraGM20BAccelerator.cpp).
extern "C" IOReturn tegra_gm20b_new_user_client(IOService *provider, task_t task, void *securityID,
						UInt32 type, OSDictionary *properties,
						IOUserClient **handler)
{
	if (type != TegraGM20BOpenType)
		return kIOReturnBadArgument;  // the original IOGPU library opens with type 1
	if (tgm_stage() < 2)
		return kIOReturnNotReady;
	if (__atomic_add_fetch(&gConnections, 1, __ATOMIC_SEQ_CST) >
	    (tgm_all_clients() ? kMaxConnectionsAll : kMaxConnections)) {
		__atomic_sub_fetch(&gConnections, 1, __ATOMIC_SEQ_CST);
		return kIOReturnNoResources;
	}
	AGXDeviceUserClient *client = OSTypeAlloc(AGXDeviceUserClient);
	if (!client) {
		__atomic_sub_fetch(&gConnections, 1, __ATOMIC_SEQ_CST);
		return kIOReturnNoMemory;
	}
	if (!client->initWithTask(task, securityID, type, properties)) {
		client->release();
		__atomic_sub_fetch(&gConnections, 1, __ATOMIC_SEQ_CST);
		return kIOReturnError;
	}
	client->countConnection();  // from here free() gives the connection slot back
	if (!client->setUp(task) || !client->attach(provider)) {
		client->release();
		return kIOReturnNoMemory;
	}
	if (!client->start(provider)) {
		client->detach(provider);
		client->release();
		return kIOReturnError;
	}
	*handler = client;
	static volatile int opened;
	if (!__atomic_exchange_n(&opened, 1,
				 __ATOMIC_SEQ_CST)) {  // the one "Metal reached the kernel" line
		char name[17] = {};
		proc_selfname(name, sizeof(name));
		IOLog("tegra-gm20b: AGXDeviceUserClient opened by %s\n", name);
	}
	return kIOReturnSuccess;
}

// ------------------------------------------------------------------ GPU frame copy for the display (TegraScanoutCopy.h)
// The display (boot-args ioswitch-gpu-copy=1 ioswitch-scanout-double=1) offers each swapped surface; gpu.c's present channel
// copies it, after the compositor's GPU work, into the block-linear target, and calls native_legacy_gpu_complete from the
// GPU interrupt pass. Offers come one at a time under the display's gate: nothing here waits for the GPU. tegra-gpu-present=0
// declines every offer (the display then copies on the CPU).
//   targets   the display's scanout buffers (three: r14-pacing's latest-frame-wins flips), physically contiguous, mapped once
//             each at kPresentTarget + i * 4 MiB, kind 0xfe
//   surfaces  the last four swapped surfaces, like the display's own surface maps: each keeps a retained and prepared
//             (wired) descriptor and its GPU mapping at kPresentSurface + i * 16 MiB; the least recently offered one goes
//             when a fifth surface comes. Its CPU caches are cleaned once, when it is imported (the GPU, which wrote it,
//             does not snoop them, and a dirty line written back later would land on its pixels).
// native_legacy_gpu_complete (NativeLegacyDisplay.cpp, declared by TegraScanoutCopy.h) is a plain reference: the kernel's static
// link refuses undefined weak ones, and the display is in every kernel that has this driver.
namespace
{
constexpr uint64_t kPresentTarget = 0x10000000, kPresentTargetSpan = 4ULL << 20;
constexpr uint64_t kPresentSurface = 0x40000000, kPresentSurfaceSpan = 16ULL << 20;
struct PresentTarget {
	uint64_t pa, bytes;
	uint64_t *pas;
	tgm_mem *mem;
};
struct PresentSurface {
	IOMemoryDescriptor *memory;
	uint64_t bytes, used;
	uint32_t npages;
	uint64_t *pas;
	tgm_mem *mem;
};
// TegraScanoutCopy.h: the boot framebuffer and the display's two other buffers
constexpr unsigned kPresentTargets = 3;
PresentTarget gTargets[kPresentTargets];
PresentSurface gSurfaces[4];
uint64_t gSurfaceUses;
IOLock *gPresentLock;

void presentDone(void *display)
{
	native_legacy_gpu_complete(display, 0);
}

void dropSurface(PresentSurface &s, unsigned slot)
{
	if (!s.memory)
		return;
	tgm_present_unmap(kPresentSurface + slot * kPresentSurfaceSpan, s.bytes);
	tgm_mem_del(&s.mem);
	s.memory->complete();
	s.memory->release();
	IODelete(s.pas, uint64_t, s.npages);
	s = {};
}

// The GPU address of the target at pa (one of the display's scanout buffers), mapped on first use.
uint64_t presentTarget(uint64_t pa, uint64_t bytes)
{
	unsigned slot = kPresentTargets;
	for (unsigned i = 0; i < kPresentTargets; i++) {
		if (gTargets[i].mem && gTargets[i].pa == pa && gTargets[i].bytes == bytes)
			return kPresentTarget + i * kPresentTargetSpan;
		if (!gTargets[i].mem && slot == kPresentTargets)
			slot = i;
	}
	if (slot == kPresentTargets || bytes > kPresentTargetSpan || (pa | bytes) & 0xfff ||
	    pa + bytes > (1ULL << 32))
		return 0;  // a fourth address: not the contract (TegraScanoutCopy.h)
	PresentTarget &t = gTargets[slot];
	const uint32_t npages = static_cast<uint32_t>(bytes >> 12);
	t.pas = IONew(uint64_t, npages);
	if (!t.pas)
		return 0;
	for (uint32_t i = 0; i < npages; i++)
		t.pas[i] = pa + (uint64_t(i) << 12);
	// The copy engine only places whole GOBs; the 16-byte sector order inside a GOB comes from the page kind. With kind 0
	// (pitch) the 10-31 r3l boot showed every GOB's inside scrambled (jagged edges, sliced text) while GOBs sat in the
	// right places. L4T maps display surfaces as 0xfe (generic 16Bx2, uncompressed), which the display reads.
	// tegra-gpu-present-kind=N overrides it for a hardware test.
	uint32_t kind = 0xfe;
	PE_parse_boot_argn("tegra-gpu-present-kind", &kind, sizeof(kind));
	tgm_vm *vm = nullptr;
	if (tgm_present_open(&vm) || tgm_mem_new(vm, t.pas, npages, &t.mem) ||
	    tgm_present_map(kPresentTarget + slot * kPresentTargetSpan, t.mem, bytes,
			    static_cast<uint8_t>(kind))) {
		tgm_mem_del(&t.mem);
		IODelete(t.pas, uint64_t, npages);
		t = {};
		return 0;
	}
	t.pa = pa;
	t.bytes = bytes;
	return kPresentTarget + slot * kPresentTargetSpan;
}

// The GPU address of the surface, imported on first use (or again after it fell out of the four); *pa its first page.
uint64_t presentSurface(IOMemoryDescriptor *memory, uint64_t bytes, uint64_t *pa)
{
	bytes = (bytes + 0xfff) & ~0xfffULL;
	unsigned slot = 0;
	for (unsigned i = 0; i < 4; i++) {
		if (gSurfaces[i].memory == memory && gSurfaces[i].bytes >= bytes) {
			gSurfaces[i].used = ++gSurfaceUses;
			*pa = gSurfaces[i].pas[0];
			return kPresentSurface + i * kPresentSurfaceSpan;
		}
		if (gSurfaces[i].used < gSurfaces[slot].used)
			slot = i;
	}
	if (bytes > kPresentSurfaceSpan || bytes > ((memory->getLength() + 0xfff) & ~0xfffULL))
		return 0;
	PresentSurface &s = gSurfaces[slot];
	dropSurface(s, slot);
	s.npages = static_cast<uint32_t>(bytes >> 12);
	s.pas = IONew(uint64_t, s.npages);
	if (!s.pas)
		return 0;
	memory->retain();
	s.memory = memory;
	s.bytes = bytes;
	tgm_vm *vm = nullptr;
	if (memory->prepare(kIODirectionOut) != kIOReturnSuccess) {
		memory->release();
		IODelete(s.pas, uint64_t, s.npages);
		s = {};
		return 0;
	}
	if (collectPages(memory, bytes, s.pas) || tgm_present_open(&vm) ||
	    tgm_mem_new(vm, s.pas, s.npages, &s.mem) ||
	    tgm_present_map(kPresentSurface + slot * kPresentSurfaceSpan, s.mem, bytes, 0)) {
		tgm_mem_del(&s.mem);
		memory->complete();
		memory->release();
		IODelete(s.pas, uint64_t, s.npages);
		s = {};
		return 0;
	}
	tgm_cpu_clean_pages(s.pas, s.npages);
	s.used = ++gSurfaceUses;
	*pa = s.pas[0];
	return kPresentSurface + slot * kPresentSurfaceSpan;
}

bool presentAllowed()
{
	static int allowed = -1;
	if (allowed < 0) {
		int v = 1;
		PE_parse_boot_argn("tegra-gpu-present", &v, sizeof(v));
		allowed = v != 0;
	}
	return allowed;
}
}  // namespace

extern "C" int tegra_gm20b_scanout_copy(const struct TegraScanoutCopy *copy)
{
	if (!copy || copy->version != TEGRA_SCANOUT_COPY_VERSION || !copy->display ||
	    !copy->source || !presentAllowed() || tgm_stage() < 3)
		return kIOReturnNotReady;
	IOMemoryDescriptor *memory =
		OSDynamicCast(IOMemoryDescriptor, static_cast<OSObject *>(copy->source));
	const bool fenceOnly = copy->flags & TEGRA_SCANOUT_COPY_FENCE_ONLY;
	if (!memory || !copy->width || !copy->height || copy->width > 16384 ||
	    copy->sourceStride < uint64_t(copy->width) * 4 || copy->sourceStride > UINT32_MAX ||
	    copy->sourceBytes < copy->sourceStride * copy->height ||
	    (!fenceOnly && (copy->targetPitch < copy->width * 4 || copy->blockHeightLog2 > 5)))
		return kIOReturnBadArgument;
	// the block-linear target must hold the frame: whole block rows of targetPitch bytes x (8 << blockHeightLog2) lines
	const uint64_t blockLines = 8ULL << copy->blockHeightLog2;
	if (!fenceOnly && copy->targetBytes < (copy->height + blockLines - 1) / blockLines *
						      blockLines * copy->targetPitch)
		return kIOReturnBadArgument;
	if (!gPresentLock) {
		IOLock *fresh = IOLockAlloc(), *expected = nullptr;
		if (!__atomic_compare_exchange_n(&gPresentLock, &expected, fresh, false,
						 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			IOLockFree(fresh);
		if (!gPresentLock)
			return kIOReturnNoMemory;
	}
	IOLockLock(gPresentLock);
	uint64_t sourcePa = 0;
	// fence only: no target; the source is still imported (its first page names the channels that write it)
	const uint64_t target = fenceOnly ? 0 : presentTarget(copy->target, copy->targetBytes);
	const uint64_t source =
		target || fenceOnly ? presentSurface(memory, copy->sourceBytes, &sourcePa) : 0;
	int ret = (target || fenceOnly) && source
			  ? tgm_present_copy(source, sourcePa, copy->sourceStride, copy->width,
					     copy->height, target, copy->targetPitch,
					     copy->blockHeightLog2, presentDone, copy->display)
			  : -12;  // ENOMEM: the target or the surface could not be mapped
	IOLockUnlock(gPresentLock);
	return ret ? ioreturn(ret) : 0;
}
