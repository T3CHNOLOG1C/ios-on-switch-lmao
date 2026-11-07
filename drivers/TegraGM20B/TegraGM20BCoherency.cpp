// SPDX-License-Identifier: MIT
// Host imports kept coherent with the CPU caches, which the GM20B does not snoop on Tegra. The render path pays nothing:
// whoever touches an imported IOSurface on the CPU pays, at its IOSurfaceLock/Unlock. Those are IOKit traps
// (IOConnectTrap3 index 2 lock, 3 unlock, p1 the surface ID, p2 the lock options), which XNU's iokit_user_client_trap
// hands to tegra_gm20b_iosurface_trap after the trap ran.
//  - GPU -> CPU: Exec marks each writable import GPU-dirty (tgm_coherency_wrote), if the GPU may write the surface at
//    all: an import without TegraGM20BImportGpuReads, or a SurfaceWrites naming it (the Metal frontends send one before
//    a surface is first rendered, blitted or stored into). The next lock of the surface, in any process, invalidates
//    its lines once and clears the mark, so the CPU reads what the GPU wrote rather than lines it cached before (a
//    fresh surface's zeros came back as 16x1 black runs in a GPU-rendered launch snapshot). A surface the GPU only
//    samples (an app's backing store in the render server) is never marked, so the app's lock to redraw it costs no
//    invalidate.
//  - Lock and GPU work in flight: IOSurfaceLock is where a CPU reader synchronises with the GPU (Apple's: a lock without
//    kIOSurfaceLockAvoidSync waits for the GPU's pending writes; with it, a lock that would have to wait fails with
//    kIOReturnCannotLock), and readers count on it: a snapshot's reader locks its surface right after the render
//    server's reply, its render maybe still running. So Exec also records each marking submission (channel, seqno) on
//    the entry, and a lock (tegra_gm20b_iosurface_lock_will, before the trap) sleeps on those channels' fence
//    interrupts until they completed, then the trap invalidates. Only a lock of a surface whose writers are still
//    running waits.
//  - CPU -> GPU: an unlock that was not read-only cleans the surface, so the GPU reads what the CPU wrote. Read-only
//    imports included: the GPU samples what an app redraws into them.
//  - Import: the importer cleans the pages for what the CPU wrote before. Memory imported without a surface ID (no-copy
//    buffers, Core Animation's shared-memory images) never sees a lock, so the import clean is all it gets.
// Entries are keyed by the surface's first physical page (the display asks by that, tegra_gm20b_surface_tiled) and,
// when the importer named the IOSurface, by its ID too. An import holds a reference to its entry, so Exec's dirty
// mark is one store through the import's own pointer. Cache maintenance runs under the entry's own lock, never the
// registry's, so a large surface's clean does not hold up other surfaces' locks and imports.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <stdint.h>
#include <string.h>
#include "TegraGM20BGpu.h"
#include "../TegraDisplayFlip/TegraScanoutCopy.h"

#define TGM_COH_WRITERS \
	4 /* the raw path's channel and m2v's, in each of two processes, before one has to wait for all */

struct tgm_coh {
	tgm_coh *next;    // first-page chain
	tgm_coh *idNext;  // surface-ID chain, while surfaceId != 0
	uint64_t pa;      // the surface's first physical page
	uint32_t surfaceId;
	uint32_t refs;  // registry lock
	IOLock *lock;   // pas, npages: the cache ops and a larger import's list
	uint64_t *pas;  // the largest page list imported for it
	uint32_t npages;
	int writable;  // some import lets the GPU write it
	int gpuWrites;  // atomic: an import or a SurfaceWrites says the GPU may write it, so submissions mark it
	int cleaned;          // atomic: an import of it finished its own clean
	int gpuDirty;         // atomic: the GPU may have written it since the last CPU lock
	uint32_t tiledBinds;  // atomic: live whole-surface binds with TEGRA_SCANOUT_TILED_KIND
	uint64_t writers
		[TGM_COH_WRITERS];  // atomic: marking submissions, channel uid << 32 | seqno (0: free slot)
	int writersLost;  // atomic: one found no slot: a lock waits for every channel's submitted work
};

namespace
{
constexpr uint32_t kBuckets = 256;
constexpr uint32_t kTrapLock = 2, kTrapUnlock = 3, kLockReadOnly = 0x1, kLockAvoidSync = 0x2;
constexpr uint32_t kBatch = 4;

IOLock *gRegistryLock;
tgm_coh *gByPage[kBuckets], *gById[kBuckets];
uint32_t gWithId;  // atomic: entries reachable by surface ID; 0 lets every trap return at once

#if TGM_STATS
struct {
	uint64_t invalN, invalBytes, cleanN, cleanBytes, waitedN, cannotLockN;
} gStats;  // registry lock
#endif

void count(bool unlock, uint64_t n, uint64_t bytes)  // registry lock held
{
#if TGM_STATS
	(unlock ? gStats.cleanN : gStats.invalN) += n;
	(unlock ? gStats.cleanBytes : gStats.invalBytes) += bytes;
#else
	(void)unlock;
	(void)n;
	(void)bytes;
#endif
}

tgm_coh **pageSlot(uint64_t pa)
{
	tgm_coh **p = &gByPage[((pa >> 12) ^ (pa >> 20)) & (kBuckets - 1)];
	while (*p && (*p)->pa != pa)
		p = &(*p)->next;
	return p;
}

tgm_coh **idBucket(uint32_t surfaceId)
{
	return &gById[(surfaceId ^ (surfaceId >> 8)) & (kBuckets - 1)];
}

void idUnlink(tgm_coh *c)
{
	for (tgm_coh **p = idBucket(c->surfaceId); *p; p = &(*p)->idNext) {
		if (*p == c) {
			*p = c->idNext;
			__atomic_sub_fetch(&gWithId, 1, __ATOMIC_RELAXED);
			return;
		}
	}
}

void entryFree(tgm_coh *c)
{
	IOLockFree(c->lock);
	IODelete(c->pas, uint64_t, c->npages);
	IODelete(c, tgm_coh, 1);
}

void release(tgm_coh *c)  // registry lock held
{
	if (--c->refs)
		return;
	tgm_coh **p = pageSlot(c->pa);
	if (*p == c)
		*p = c->next;
	if (c->surfaceId)
		idUnlink(c);
	entryFree(c);
}

uint64_t maintain(tgm_coh *c)
{
	IOLockLock(c->lock);
	tgm_cpu_clean_pages(c->pas, c->npages);
	const uint64_t bytes = uint64_t(c->npages) << 12;
	IOLockUnlock(c->lock);
	return bytes;
}

// The entries of `surfaceId` from the `skip`th on, at most kBatch, each with a reference the caller drops. A surface has
// one entry unless its imports began at different pages.
uint32_t findById(uint32_t surfaceId, uint32_t skip, tgm_coh **out)
{
	uint32_t n = 0, seen = 0;
	IOLockLock(gRegistryLock);
	for (tgm_coh *c = *idBucket(surfaceId); c && n < kBatch; c = c->idNext) {
		if (c->surfaceId == surfaceId && seen++ >= skip) {
			c->refs++;
			out[n++] = c;
		}
	}
	IOLockUnlock(gRegistryLock);
	return n;
}

// A lock invalidates the entries the GPU may have written since the last lock; an unlock that wrote cleans them all.
void surfaceTrapped(uint32_t surfaceId, bool unlock)
{
	tgm_coh *found[kBatch];
	uint32_t n = kBatch;
	for (uint32_t skip = 0; n == kBatch && (n = findById(surfaceId, skip, found)); skip += n) {
		uint64_t bytes = 0, done = 0;
		for (uint32_t i = 0; i < n; i++) {
			if (unlock ||
			    __atomic_exchange_n(&found[i]->gpuDirty, 0, __ATOMIC_ACQ_REL)) {
				bytes += maintain(found[i]);
				done++;
			}
		}
		IOLockLock(gRegistryLock);
		count(unlock, done, bytes);
		for (uint32_t i = 0; i < n; i++)
			release(found[i]);
		IOLockUnlock(gRegistryLock);
	}
}

// A larger import of a surface already registered: its list replaces the entry's (entry lock held).
void adopt(tgm_coh *c, const uint64_t *pas, uint32_t npages)
{
	uint64_t *copy = IONew(uint64_t, npages);
	if (!copy)
		return;
	memcpy(copy, pas, size_t(npages) * sizeof(uint64_t));
	IODelete(c->pas, uint64_t, c->npages);
	c->pas = copy;
	c->npages = npages;
}

tgm_coh *entryNew(uint64_t pa, const uint64_t *pas, uint32_t npages)
{
	tgm_coh *c = IONew(tgm_coh, 1);
	if (!c)
		return nullptr;
	bzero(c, sizeof(*c));
	c->pa = pa;
	c->lock = IOLockAlloc();
	c->pas = IONew(uint64_t, npages);
	if (!c->lock || !c->pas) {
		if (c->lock)
			IOLockFree(c->lock);
		if (c->pas)
			IODelete(c->pas, uint64_t, npages);
		IODelete(c, tgm_coh, 1);
		return nullptr;
	}
	memcpy(c->pas, pas, size_t(npages) * sizeof(uint64_t));
	c->npages = npages;
	return c;
}
}  // namespace

extern "C" int tgm_coherency_init(void)
{
	if (!gRegistryLock)
		gRegistryLock = IOLockAlloc();
	return gRegistryLock ? 0 : -12;
}

extern "C" struct tgm_coh *tgm_coherency_add(uint64_t pa, uint32_t surface_id, const uint64_t *pas,
					     uint32_t npages, int writable, int gpu_writes,
					     int *covered)
{
	*covered = 0;
	if (!npages || !gRegistryLock)
		return nullptr;
	tgm_coh *fresh = entryNew(pa, pas, npages);
	IOLockLock(gRegistryLock);
	tgm_coh **p = pageSlot(pa), *c = *p;
	// A live import of the same surface that finished its own clean: every CPU write since came through an
	// IOSurfaceUnlock that cleaned these pages, so they are clean already.
	if (c && surface_id && c->surfaceId == surface_id && c->npages >= npages &&
	    __atomic_load_n(&c->cleaned, __ATOMIC_ACQUIRE))
		*covered = 1;
	if (!c && fresh) {
		c = *p = fresh;
		fresh = nullptr;
	}
	if (c) {
		c->refs++;
		c->writable |= writable;
		if (gpu_writes)
			__atomic_store_n(&c->gpuWrites, 1, __ATOMIC_RELAXED);
		if (surface_id && !c->surfaceId) {
			c->surfaceId = surface_id;
			tgm_coh **b = idBucket(surface_id);
			c->idNext = *b;
			*b = c;
			__atomic_add_fetch(&gWithId, 1, __ATOMIC_RELAXED);
		}
	}
	IOLockUnlock(gRegistryLock);
	if (fresh)
		entryFree(fresh);
	if (*covered) {
		IOLockLock(c->lock);
		*covered = !memcmp(c->pas, pas, size_t(npages) * sizeof(uint64_t));
		IOLockUnlock(c->lock);
	}
	if (c && npages > c->npages) {
		IOLockLock(c->lock);
		if (npages > c->npages)
			adopt(c, pas, npages);
		IOLockUnlock(c->lock);
	}
	return c;
}

extern "C" void tgm_coherency_cleaned(struct tgm_coh *c)
{
	if (c)
		__atomic_store_n(&c->cleaned, 1, __ATOMIC_RELEASE);
}

extern "C" void tgm_coherency_put(struct tgm_coh *c)
{
	if (!c)
		return;
	IOLockLock(gRegistryLock);
	release(c);
	IOLockUnlock(gRegistryLock);
}

// One submission of channel `uid` that may write `c`: a later one of the same channel takes its slot.
static void noteWriter(tgm_coh *c, uint32_t uid, uint32_t seqno)
{
	const uint64_t mine = uint64_t(uid) << 32 | seqno;
	for (uint32_t i = 0; i < TGM_COH_WRITERS; i++) {
		uint64_t seen = __atomic_load_n(&c->writers[i], __ATOMIC_ACQUIRE);
		while (seen && uint32_t(seen >> 32) == uid) {
			if (int32_t(seqno - uint32_t(seen)) <= 0 ||
			    __atomic_compare_exchange_n(&c->writers[i], &seen, mine, false,
							__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
				return;
		}
	}
	for (uint32_t i = 0; i < TGM_COH_WRITERS; i++) {
		uint64_t free = 0;
		if (__atomic_compare_exchange_n(&c->writers[i], &free, mine, false,
						__ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
			return;
	}
	__atomic_store_n(&c->writersLost, 1, __ATOMIC_RELEASE);
}

extern "C" void tgm_coherency_wrote(struct tgm_coh *c, int always, uint32_t uid, uint32_t seqno)
{
	if (c && (always || __atomic_load_n(&c->gpuWrites, __ATOMIC_RELAXED))) {
		noteWriter(c, uid, seqno);
		__atomic_store_n(&c->gpuDirty, 1, __ATOMIC_RELEASE);
	}
}

// Whether `c` has a writer still running; with `wait`, sleeps until none has, and frees the completed slots.
static bool writersPending(tgm_coh *c, bool wait)
{
	bool pending = false;
	if (__atomic_load_n(&c->writersLost, __ATOMIC_ACQUIRE)) {
		if (!wait)
			return !tgm_writers_all_done();
		__atomic_store_n(&c->writersLost, 0, __ATOMIC_RELAXED);
		tgm_writers_wait_all();
		pending = true;
	}
	for (uint32_t i = 0; i < TGM_COH_WRITERS; i++) {
		uint64_t seen = __atomic_load_n(&c->writers[i], __ATOMIC_ACQUIRE);
		if (!seen || tgm_writer_done(uint32_t(seen >> 32), uint32_t(seen))) {
			if (seen)
				__atomic_compare_exchange_n(&c->writers[i], &seen, 0, false,
							    __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
			continue;
		}
		if (!wait)
			return true;
		tgm_writer_wait(uint32_t(seen >> 32), uint32_t(seen));
		__atomic_compare_exchange_n(&c->writers[i], &seen, 0, false, __ATOMIC_ACQ_REL,
					    __ATOMIC_RELAXED);
		pending = true;
	}
	return pending;
}

extern "C" int tegra_gm20b_iosurface_lock_will(uint32_t surface_id, uint32_t options)
{
	if (!surface_id || !__atomic_load_n(&gWithId, __ATOMIC_RELAXED))
		return 0;
	const bool avoidSync = options & kLockAvoidSync;
	tgm_coh *found[kBatch];
	uint32_t n = kBatch;
	bool waited = false, refused = false;
	for (uint32_t skip = 0; n == kBatch && !refused && (n = findById(surface_id, skip, found));
	     skip += n) {
		for (uint32_t i = 0; i < n && !refused; i++) {
			if (avoidSync)
				refused = writersPending(found[i], false);
			else
				waited |= writersPending(found[i], true);
		}
		IOLockLock(gRegistryLock);
		for (uint32_t i = 0; i < n; i++)
			release(found[i]);
		IOLockUnlock(gRegistryLock);
	}
#if TGM_STATS
	if (waited || refused) {
		IOLockLock(gRegistryLock);
		gStats.waitedN += waited;
		gStats.cannotLockN += refused;
		IOLockUnlock(gRegistryLock);
	}
#else
	(void)waited;
#endif
	return refused;
}

extern "C" void tgm_coherency_gpu_writes(uint32_t surface_id)
{
	if (!surface_id || !gRegistryLock)
		return;
	IOLockLock(gRegistryLock);
	for (tgm_coh *c = *idBucket(surface_id); c; c = c->idNext) {
		if (c->surfaceId == surface_id)
			__atomic_store_n(&c->gpuWrites, 1, __ATOMIC_RELAXED);
	}
	IOLockUnlock(gRegistryLock);
}

extern "C" void tgm_coherency_tiled_bind(struct tgm_coh *c, int add)
{
	if (c)
		__atomic_fetch_add(&c->tiledBinds, uint32_t(add), __ATOMIC_ACQ_REL);
}

extern "C" int tegra_gm20b_surface_tiled(uint64_t pa)
{
	if (!gRegistryLock)
		return 0;
	IOLockLock(gRegistryLock);
	const tgm_coh *c = *pageSlot(pa);
	const int tiled = c && __atomic_load_n(&c->tiledBinds, __ATOMIC_ACQUIRE);
	IOLockUnlock(gRegistryLock);
	return tiled;
}

extern "C" void tgm_coherency_take_stats(uint64_t out[6])
{
	bzero(out, 6 * sizeof(uint64_t));
#if TGM_STATS
	if (!gRegistryLock)
		return;
	IOLockLock(gRegistryLock);
	out[0] = gStats.invalN;
	out[1] = gStats.invalBytes;
	out[2] = gStats.cleanN;
	out[3] = gStats.cleanBytes;
	out[4] = gStats.waitedN;
	out[5] = gStats.cannotLockN;
	bzero(&gStats, sizeof(gStats));
	IOLockUnlock(gRegistryLock);
#endif
}

extern "C" void tegra_gm20b_iosurface_trap(uint32_t index, uint32_t surface_id, uint32_t options)
{
	if (!surface_id || !__atomic_load_n(&gWithId, __ATOMIC_RELAXED))
		return;
	if (index == kTrapLock)
		surfaceTrapped(surface_id, false);
	else if (index == kTrapUnlock && !(options & kLockReadOnly))
		surfaceTrapped(surface_id, true);
}
