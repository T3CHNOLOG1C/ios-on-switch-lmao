// SPDX-License-Identifier: MIT
// AGXDeviceUserClient: one connection = one GPU address space, implementing scratchpad/gpu/CONTRACT.md v1 over the wire
// records of TegraGM20BABI.h (copied verbatim from the userland side). Transport modelled on
// drivers/NativeAdreno/NativeAdrenoPublish.cpp: Apple user-client class name (sandbox profiles admit it), one open type,
// struct-in/struct-out methods with exact sizes, the same methods tunnelled through any admitted selector, and
// clientMemoryForType views cached per token so IOConnectUnmapMemory64 finds the same descriptor.
// GPU work goes through gpu.c (TegraGM20BGpu.h). Fence waits sleep until the GPU's nonstall interrupt wakes them;
// a fence found complete without that wake panics (no polled mode), and so does one that never completes.
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOUserClient.h>
#include <kern/clock.h>
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

// Pool BOs released while a task still maps their view (see releaseBo): the pages go back to the pool only once the
// last such mapping is gone, so the task can never write pages the pool has handed out again (GPU page tables among
// them). IOKit keeps a task's mappings past its connection's close, until the task unmaps them or dies, so the list is
// global; any connection's BoAlloc and close look at it again.
struct PoolOrphan {
	PoolOrphan *next;
	IOMemoryDescriptor *view;
	void *va;
	uint64_t pa, bytes;
};
PoolOrphan *gPoolOrphans;

void pushOrphan(PoolOrphan *orphan)
{
	orphan->next = __atomic_load_n(&gPoolOrphans, __ATOMIC_RELAXED);
	while (!__atomic_compare_exchange_n(&gPoolOrphans, &orphan->next, orphan, true,
					    __ATOMIC_RELEASE, __ATOMIC_RELAXED))
		;
}

// A view only we hold is mapped nowhere (an IOMemoryMap holds its descriptor), and nothing can map it any more.
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
		tgm_pool_free(orphan->va, orphan->bytes, orphan->pa);
		IODelete(orphan, PoolOrphan, 1);
	}
}

uint64_t nowNs()
{
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns;
}

// tegra-gpu-all-clients=1: a client other than the compositor may hold at most tegra-gpu-client-pool-mb (default 64) of
// the GPU pool in BOs, and gets none of its last tegra-gpu-pool-reserve-mb (default 32), which stay for the compositor
// (CA's next allocation must not be the one that fails). A refused BoAlloc is kIOReturnNoMemory, logged once per
// connection; Metal then fails that allocation cleanly.
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
	default:
		return kIOReturnError;
	}
}
}  // namespace

class AGXDeviceUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(AGXDeviceUserClient)

		struct Bo {
		Bo *next;
		uint64_t handle, bytes;
		uint32_t token, flags, npages;
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
	releaseOrphans();
	terminate();
	return kIOReturnSuccess;
}

void AGXDeviceUserClient::free()
{
	if (lock_) {
		closing_ = true;
		for (uint32_t i = 0; i < kMaxChannels; i++)
			tgm_chan_del(&chans_[i]);
		while (bos_) {
			Bo *bo = bos_;
			bos_ = bo->next;
			releaseBo(bo);
		}
		reap(true);
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
	if (bo->pool_va && bo->handle)
		pool_bytes_ -= bo->bytes;
	// A pool view still mapped in a task keeps its pages until unmapped (PoolOrphan). Cached BOs need no such care:
	// their mappings hold the buffer itself.
	PoolOrphan *orphan = nullptr;
	if (bo->pool_va && bo->view && bo->view->getRetainCount() > 1 &&
	    (orphan = IONew(PoolOrphan, 1))) {
		if (!closing_)
			IOLog("tegra-gm20b: BoFree of BO %llu (%llu bytes) still mapped by its "
			      "task: pages kept until unmapped\n",
			      bo->handle, bo->bytes);
		*orphan = {nullptr, bo->view, bo->pool_va, bo->pool_pa, bo->bytes};
		pushOrphan(orphan);
	} else {
		if (bo->view)
			bo->view->release();
		if (bo->pool_va)
			tgm_pool_free(bo->pool_va, bo->bytes, bo->pool_pa);
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
		IODelete(bo->pas, uint64_t, bo->npages);
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
	releaseOrphans();
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
	bo->pas = IONew(uint64_t, bo->npages);
	IOReturn ret = bo->pas ? kIOReturnSuccess : kIOReturnNoMemory;
	if (!ret && (in->flags & TegraGM20BBoCoherent) && !critical_ &&
	    (pool_bytes_ + in->bytes > clientPoolBytes() ||
	     tgm_pool_avail() < in->bytes + poolReserveBytes())) {
		if (!quotaLogged_)
			IOLog("tegra-gm20b: %s: GPU pool BoAlloc of %llu KiB refused (client holds "
			      "%llu of %llu MiB, pool %llu MiB "
			      "free, %llu kept for the compositor); further refusals not logged\n",
			      owner_, in->bytes >> 10, pool_bytes_ >> 20, clientPoolBytes() >> 20,
			      tgm_pool_avail() >> 20, poolReserveBytes() >> 20);
		quotaLogged_ = true;
		ret = kIOReturnNoMemory;
	} else if (!ret && (in->flags & TegraGM20BBoCoherent)) {
		// write-combined pool memory: zeroed by the allocator, no cacheable alias (CONTRACT §3)
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
	} else if (!ret) {
		IOOptionBits options = static_cast<IOOptionBits>(kIODirectionInOut) |
				       static_cast<IOOptionBits>(kIOMemoryMapperNone);
		if (in->flags & TegraGM20BBoContiguous)
			options |= kIOMemoryPhysicallyContiguous;
		// 34-bit, not 32: a mask below 4 GiB on a non-contiguous buffer makes IOKit use KMA_LOMEM, and arm64 XNU keeps
		// no low-page pool, so every such BO failed (10-30: NVK "device refused: OutOfMemory(Device)")
		const uint64_t mask = 0x3ffffffffULL & ~((1ULL << in->align_log2) - 1);
		bo->bmd = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, options,
									   in->bytes, mask);
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
			for (uint64_t a = reinterpret_cast<uint64_t>(va) & ~63ULL;
			     a < reinterpret_cast<uint64_t>(va) + in->bytes; a += 64)
				__asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
			__asm__ volatile("dsb sy" ::: "memory");
			lap(1);
			ret = collectKernelPages(va, in->bytes, bo->pas);
			lap(2);
			bo->view = bo->bmd;
			bo->view->retain();
		}
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
	if (bo->pool_va)
		pool_bytes_ += bo->bytes;
	tgm_stats_bo_alloc(bo->bytes, bo->pool_va != nullptr, mark - start, phase);
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
	int clean = 0;
	bo->coh = tgm_coherency_add(bo->pas[0], bo->pas, bo->npages, !bo->readOnly, &clean);
	// The GPU does not snoop the CPU caches: what the task wrote must be in DRAM before the GPU reads it, and no dirty
	// line (fresh pages are zeroed through the cache) may be written back later over what the GPU wrote. Not needed when
	// the surface was already imported with these pages: its writers' unlocks have been cleaning it since (the hook).
	if (!clean)
		tgm_cpu_clean_pages(bo->pas, bo->npages);
	lap(3);
	tgm_stats_import(static_cast<int64_t>(bo->bytes), bo->pas[0]);
	tgm_stats_bo_import(bo->bytes, bo->readOnly, clean, mark - start, phase);
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
	out->channel = slot + 1;
	out->fence_token = kFenceToken;
	out->fence_offset = slot * 16;
	out->reserved = 0;
	return kIOReturnSuccess;
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
	IOLog("tegra-gm20b: push %s\n", line);
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

IOReturn AGXDeviceUserClient::getInfo(TegraGM20BAbiInfo *out)
{
	tgm_info info;
	tgm_get_info(&info);
	bzero(out, sizeof(*out));
	out->abi_version = TegraGM20BAbiVersion;
	out->flags = TegraGM20BInfoHostImport;
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
	default:
		break;
	}

	if (method == TegraGM20BMethodWaitFence)
		return waitFence(static_cast<const TegraGM20BAbiWaitFence *>(in));
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
		      method == TegraGM20BMethodExec)) {
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
		if (bo->bmd && bo->bytes <= kPrefaultMaxBytes) {
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
//   targets   the two scanout buffers, physically contiguous, mapped once each at kPresentTarget + i * 4 MiB, kind 0xfe
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
PresentTarget gTargets[2];
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

// The GPU address of the target at pa (one of the display's two scanout buffers), mapped on first use.
uint64_t presentTarget(uint64_t pa, uint64_t bytes)
{
	for (unsigned i = 0; i < 2; i++)
		if (gTargets[i].mem && gTargets[i].pa == pa && gTargets[i].bytes == bytes)
			return kPresentTarget + i * kPresentTargetSpan;
	unsigned slot = gTargets[0].mem ? 1 : 0;
	PresentTarget &t = gTargets[slot];
	if (t.mem || bytes > kPresentTargetSpan || (pa | bytes) & 0xfff ||
	    pa + bytes > (1ULL << 32))
		return 0;  // a third address: not the contract (TegraScanoutCopy.h)
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
	if (!memory || !copy->width || !copy->height || copy->width > 16384 ||
	    copy->sourceStride < uint64_t(copy->width) * 4 || copy->sourceStride > UINT32_MAX ||
	    copy->sourceBytes < copy->sourceStride * copy->height ||
	    copy->targetPitch < copy->width * 4 || copy->blockHeightLog2 > 5)
		return kIOReturnBadArgument;
	// the block-linear target must hold the frame: whole block rows of targetPitch bytes x (8 << blockHeightLog2) lines
	const uint64_t blockLines = 8ULL << copy->blockHeightLog2;
	if (copy->targetBytes <
	    (copy->height + blockLines - 1) / blockLines * blockLines * copy->targetPitch)
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
	const uint64_t target = presentTarget(copy->target, copy->targetBytes);
	const uint64_t source = target ? presentSurface(memory, copy->sourceBytes, &sourcePa) : 0;
	int ret = target && source
			  ? tgm_present_copy(source, sourcePa, copy->sourceStride, copy->width,
					     copy->height, target, copy->targetPitch,
					     copy->blockHeightLog2, presentDone, copy->display)
			  : -12;  // ENOMEM: the target or the surface could not be mapped
	IOLockUnlock(gPresentLock);
	return ret ? ioreturn(ret) : 0;
}
