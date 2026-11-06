// SPDX-License-Identifier: MIT
// Host imports kept coherent with the CPU caches, which the GM20B does not snoop on Tegra. The render path pays nothing:
// whoever touches an imported IOSurface on the CPU pays, at its IOSurfaceLock/Unlock. Those are IOKit traps
// (IOConnectTrap3 index 2 lock, 3 unlock, p1 the surface ID, p2 the lock options), which XNU's iokit_user_client_trap
// hands to tegra_gm20b_iosurface_trap after the trap ran.
//  - GPU -> CPU: Exec marks each writable import GPU-dirty (tgm_coherency_wrote). The next lock of the surface, in any
//    process, invalidates its lines once and clears the mark, so the CPU reads what the GPU wrote rather than lines it
//    cached before (a fresh surface's zeros came back as 16x1 black runs in a GPU-rendered launch snapshot).
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

struct tgm_coh {
	tgm_coh *next;    // first-page chain
	tgm_coh *idNext;  // surface-ID chain, while surfaceId != 0
	uint64_t pa;      // the surface's first physical page
	uint32_t surfaceId;
	uint32_t refs;  // registry lock
	IOLock *lock;   // pas, npages: the cache ops and a larger import's list
	uint64_t *pas;  // the largest page list imported for it
	uint32_t npages;
	int writable;         // some import lets the GPU write it
	int cleaned;          // atomic: an import of it finished its own clean
	int gpuDirty;         // atomic: the GPU may have written it since the last CPU lock
	uint32_t tiledBinds;  // atomic: live whole-surface binds with TEGRA_SCANOUT_TILED_KIND
};

namespace
{
constexpr uint32_t kBuckets = 256;
constexpr uint32_t kTrapLock = 2, kTrapUnlock = 3, kLockReadOnly = 0x1;

IOLock *gRegistryLock;
tgm_coh *gByPage[kBuckets], *gById[kBuckets];
uint32_t gWithId;  // atomic: entries reachable by surface ID; 0 lets every trap return at once

#if TGM_STATS
struct {
	uint64_t invalN, invalBytes, cleanN, cleanBytes;
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

// The entries a trap of `surfaceId` reaches, each with a reference the caller drops (at most `max`; a surface has one
// entry unless its imports began at different pages).
uint32_t findById(uint32_t surfaceId, tgm_coh **out, uint32_t max)
{
	uint32_t n = 0;
	IOLockLock(gRegistryLock);
	for (tgm_coh *c = *idBucket(surfaceId); c && n < max; c = c->idNext) {
		if (c->surfaceId == surfaceId) {
			c->refs++;
			out[n++] = c;
		}
	}
	IOLockUnlock(gRegistryLock);
	return n;
}

void surfaceLocked(uint32_t surfaceId)
{
	tgm_coh *found[4];
	const uint32_t n = findById(surfaceId, found, 4);
	uint64_t bytes = 0, invalidated = 0;
	for (uint32_t i = 0; i < n; i++) {
		if (__atomic_exchange_n(&found[i]->gpuDirty, 0, __ATOMIC_ACQ_REL)) {
			bytes += maintain(found[i]);
			invalidated++;
		}
	}
	IOLockLock(gRegistryLock);
	count(false, invalidated, bytes);
	for (uint32_t i = 0; i < n; i++)
		release(found[i]);
	IOLockUnlock(gRegistryLock);
}

void surfaceUnlocked(uint32_t surfaceId)
{
	tgm_coh *found[4];
	const uint32_t n = findById(surfaceId, found, 4);
	uint64_t bytes = 0;
	for (uint32_t i = 0; i < n; i++)
		bytes += maintain(found[i]);
	IOLockLock(gRegistryLock);
	count(true, n, bytes);
	for (uint32_t i = 0; i < n; i++)
		release(found[i]);
	IOLockUnlock(gRegistryLock);
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
					     uint32_t npages, int writable, int *covered)
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

extern "C" void tgm_coherency_wrote(struct tgm_coh *c)
{
	if (c)
		__atomic_store_n(&c->gpuDirty, 1, __ATOMIC_RELEASE);
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

extern "C" void tgm_coherency_take_stats(uint64_t out[4])
{
	bzero(out, 4 * sizeof(uint64_t));
#if TGM_STATS
	if (!gRegistryLock)
		return;
	IOLockLock(gRegistryLock);
	out[0] = gStats.invalN;
	out[1] = gStats.invalBytes;
	out[2] = gStats.cleanN;
	out[3] = gStats.cleanBytes;
	bzero(&gStats, sizeof(gStats));
	IOLockUnlock(gRegistryLock);
#endif
}

extern "C" void tegra_gm20b_iosurface_trap(uint32_t index, uint32_t surface_id, uint32_t options)
{
	if (!surface_id || !__atomic_load_n(&gWithId, __ATOMIC_RELAXED))
		return;
	if (index == kTrapLock)
		surfaceLocked(surface_id);
	else if (index == kTrapUnlock && !(options & kLockReadOnly))
		surfaceUnlocked(surface_id);
}
