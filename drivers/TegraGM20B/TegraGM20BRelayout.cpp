// SPDX-License-Identifier: MIT
// The copy engine's moves of block-linear IOSurfaces (TegraGM20BRelayout.h) on the kernel's own GPU client: one address
// space and copy channel made at the first move, a push page and a fence page from the GPU pool; a move maps the surface's
// pages twice (TGM_LAYOUT_KIND and kind 0) and the scratch once, and unmaps them after it. Moves are serialised; the caller
// sleeps on the move's fence interrupt (tgm_writer_wait, which recovers a copy that stops progressing as it does a stuck
// writer), so the bytes are in DRAM where they were asked for when it returns. The channel is a client channel to the
// rest of the driver (kernel 0): the present's acquires and a lock's wait for every channel include it, and a fault on it
// kills it alone (the next move makes another).
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <stdint.h>
#include <string.h>
#include "TegraGM20BGpu.h"
#include "TegraGM20BRelayout.h"
#include "TegraGM20BTiled.h"

namespace
{
constexpr uint64_t kPage = 4096;
constexpr uint64_t kPushVa = 1ull << 20, kSurfaceVa = 1ull << 32, kSurfaceRowsVa = 2ull << 32,
		   kRowsVa = 3ull << 32;

struct Relayout {
	IOLock *lock;
	tgm_vm *vm;
	tgm_chan *chan;
	tgm_mem *push, *fence;
	uint32_t *pushCpu;
	void *fenceCpu;
	uint64_t pushPa, fencePa, fenceGpu;
	bool failed;  // no address space: every move is refused
} gRelayout;

bool openSpace()
{
	Relayout &r = gRelayout;
	r.pushCpu = static_cast<uint32_t *>(tgm_pool_alloc(kPage, &r.pushPa));
	r.fenceCpu = tgm_pool_alloc(kPage, &r.fencePa);
	if (!r.pushCpu || !r.fenceCpu || tgm_vm_new(&r.vm))
		return false;
	tgm_vm_claim(r.vm, &gRelayout);
	return !tgm_mem_new(r.vm, &r.pushPa, 1, &r.push) &&
	       !tgm_vm_map_ordered(r.vm, kPushVa, kPage, r.push, 0, 0, 1) &&
	       !tgm_mem_new(r.vm, &r.fencePa, 1, &r.fence) &&
	       !tgm_vm_kmap(r.vm, r.fence, kPage, &r.fenceGpu);
}

tgm_chan *channel()
{
	Relayout &r = gRelayout;
	if (r.chan && tgm_chan_error(r.chan))
		tgm_chan_del(&r.chan);
	if (!r.vm && !r.failed && !openSpace()) {
		r.failed = true;
		IOLog("tegra-gm20b: block-linear surfaces cannot be moved: no address space\n");
	}
	if (!r.chan && !r.failed)
		tgm_chan_new(r.vm, &gRelayout, static_cast<volatile uint32_t *>(r.fenceCpu),
			     r.fenceGpu, "relayout", 1, &r.chan);
	return r.chan;
}

int move(const tgm_relayout_pages &surface, const tgm_relayout_pages &rows, const tgm_layout &l,
	 uint32_t bytesPerRow, tgm_relayout_op op, const tgm_relayout_writer *writers,
	 uint32_t nwriters)
{
	Relayout &r = gRelayout;
	const uint64_t surfaceBytes = uint64_t(surface.npages) * kPage,
		       rowsBytes = uint64_t(rows.npages) * kPage;
	const uint64_t rowBytes = uint64_t(bytesPerRow) * l.height;
	if (l.bytes > surfaceBytes || bytesPerRow < l.width * l.bpp || rowBytes > surfaceBytes ||
	    rowBytes > rowsBytes)
		return -22;
	tgm_chan *chan = channel();
	if (!chan)
		return -12;
	tgm_relayout_wait waits[TGM_RELAYOUT_MAX_WAITS];
	uint32_t nwaits = 0;
	for (uint32_t i = 0; i < nwriters && nwaits < TGM_RELAYOUT_MAX_WAITS; i++)
		if (!tgm_writer_fence_va(r.vm, writers[i].uid, &waits[nwaits].va))
			waits[nwaits++].seqno = writers[i].seqno;
	tgm_mem *surfaceMem = nullptr, *rowsMem = nullptr;
	int ret = tgm_mem_new(r.vm, surface.pas, surface.npages, &surfaceMem);
	if (!ret)
		ret = tgm_mem_new(r.vm, rows.pas, rows.npages, &rowsMem);
	if (!ret)
		ret = tgm_vm_map_ordered(r.vm, kSurfaceVa, surfaceBytes, surfaceMem, 0,
					 TGM_LAYOUT_KIND, 0);
	if (!ret)
		ret = tgm_vm_map_ordered(r.vm, kSurfaceRowsVa, surfaceBytes, surfaceMem, 0, 0, 0);
	if (!ret)
		ret = tgm_vm_map_ordered(r.vm, kRowsVa, rowsBytes, rowsMem, 0, 0, 0);
	if (!ret) {
		const tgm_relayout_move m = {kSurfaceVa, kSurfaceRowsVa, kRowsVa, bytesPerRow, op};
		const tgm_push push = {kPushVa, tgm_relayout_push(r.pushCpu, &l, &m, waits, nwaits),
				       0};
		uint32_t seqno = 0;
		ret = tgm_chan_exec(chan, &push, 1, &seqno, nullptr);
		if (!ret) {
			tgm_writer_wait(tgm_chan_uid(chan), seqno);
			ret = tgm_chan_error(chan) ? -5 : 0;
		}
	}
	tgm_vm_unmap(r.vm, kRowsVa, rowsBytes);
	tgm_vm_unmap(r.vm, kSurfaceRowsVa, surfaceBytes);
	tgm_vm_unmap(r.vm, kSurfaceVa, surfaceBytes);
	tgm_mem_del(&rowsMem);
	tgm_mem_del(&surfaceMem);
	return ret;
}
}  // namespace

extern "C" int tgm_relayout_init(void)
{
	if (!gRelayout.lock)
		gRelayout.lock = IOLockAlloc();
	return gRelayout.lock ? 0 : -12;
}

extern "C" int tgm_relayout(const struct tgm_relayout_pages *surface,
			    const struct tgm_relayout_pages *rows, const struct tgm_layout *layout,
			    uint32_t bytes_per_row, int op,
			    const struct tgm_relayout_writer *writers, uint32_t nwriters)
{
	if (!gRelayout.lock || !surface || !rows || !layout)
		return -22;
	IOLockLock(gRelayout.lock);
	const int ret = move(*surface, *rows, *layout, bytes_per_row, tgm_relayout_op(op), writers,
			     nwriters);
	IOLockUnlock(gRelayout.lock);
	return ret;
}

struct tgm_scratch {
	void *va;
	uint64_t pa, bytes;
	uint64_t *pas;
};

// GPU pool pages, contiguous: the copy engine writes them, nothing reads them on the CPU
extern "C" struct tgm_scratch *tgm_scratch_get(uint64_t bytes, struct tgm_relayout_pages *pages)
{
	bytes = (bytes + kPage - 1) & ~(kPage - 1);
	tgm_scratch *s = bytes ? IONew(tgm_scratch, 1) : nullptr;
	if (!s)
		return nullptr;
	s->bytes = bytes;
	s->va = tgm_pool_alloc(bytes, &s->pa);
	s->pas = s->va ? IONew(uint64_t, bytes / kPage) : nullptr;
	if (!s->pas) {
		if (s->va)
			tgm_pool_free(s->va, bytes, s->pa);
		IODelete(s, tgm_scratch, 1);
		return nullptr;
	}
	for (uint64_t i = 0; i < bytes / kPage; i++)
		s->pas[i] = s->pa + i * kPage;
	*pages = {s->pas, uint32_t(bytes / kPage)};
	return s;
}

extern "C" void tgm_scratch_put(struct tgm_scratch *s)
{
	if (!s)
		return;
	tgm_pool_free(s->va, s->bytes, s->pa);
	IODelete(s->pas, uint64_t, s->bytes / kPage);
	IODelete(s, tgm_scratch, 1);
}
