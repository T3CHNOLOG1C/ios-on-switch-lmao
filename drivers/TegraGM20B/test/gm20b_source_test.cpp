// SPDX-License-Identifier: MIT
// Guards on how TegraGM20B's kernel code uses what gm20b_pool_policy_test and gm20b_push_scan_test exercise, and on the
// pool's zeroing, which no host model runs (it lives in nvkm's shim and gpu.c's channels):
//  - the user client decides a coherent BO's memory through TegraGM20BPoolPolicy.h, serves a BO the pool does not from
//    ordinary pages under a write-combined physical view, never refuses a coherent BO for want of pool room, and the
//    stats line names the pool's holders;
//  - the CPU does not clear the reserved pool at boot; every page an allocation hands out is zero whether the CPU or
//    the copy engine zeroed it; the scrubber gives a range back clean only from its fence's interrupt pass, never waits
//    on the CPU, and panics with one line on a range not back in time;
//  - the user client reads pushes only through TegraGM20BPushScan.h and profiles only what it read;
//  - a fence-only present (direct scanout) copies nothing.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cctype>
#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Between;
using switch_ios::test::Count;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

namespace
{
const std::string &client()
{
	static const std::string source = SourceText("../TegraGM20BUserClient.cpp");
	return source;
}

const std::string &gpu()
{
	static const std::string source = SourceText("../gpu.c");
	return source;
}

// The body of the function whose definition (not a declaration of it) starts with `signature`.
std::string definition(const std::string &source, const std::string &signature)
{
	for (size_t at = source.find(signature); at != std::string::npos;
	     at = source.find(signature, at + 1)) {
		const std::string rest = source.substr(at + signature.size());
		if (rest.substr(0, rest.find('{')).find(';') != std::string::npos)
			continue;
		return rest.substr(0, rest.find("\n}\n"));
	}
	ADD_FAILURE() << "no definition of " << signature;
	return {};
}

// `text` with every run of whitespace one space (the checks read code, not its line breaks).
std::string flat(const std::string &text)
{
	std::string out;
	bool space = false;
	for (const char c : text) {
		if (std::isspace((unsigned char)c)) {
			space = true;
			continue;
		}
		if (space)
			out += ' ';
		space = false;
		out += c;
	}
	if (space)
		out += ' ';
	return out;
}

// `first` occurs in `text`, and before `second`.
void expectBefore(const std::string &text, const char *first, const char *second)
{
	const size_t a = text.find(first), b = text.find(second);
	ASSERT_NE(a, std::string::npos) << first;
	ASSERT_NE(b, std::string::npos) << second;
	EXPECT_LT(a, b) << first << " before " << second;
}
}  // namespace

TEST(GM20BPoolPolicySource, BoAllocNeverRefusesACoherentBoForPoolRoom)
{
	EXPECT_THAT(client(), HasSubstr("#include \"TegraGM20BPoolPolicy.h\""));
	const std::string refusal = flat(definition(
		client(),
		"const char *poolRefusal(uint64_t bytes, uint64_t held, bool compositor)"));
	EXPECT_THAT(refusal, HasSubstr("tgm_pool::refusal("));
	EXPECT_THAT(refusal, HasSubstr("compositor);"));  // the compositor keeps nvkm's floor
	const std::string alloc =
		flat(definition(client(), "IOReturn AGXDeviceUserClient::boAlloc("));
	EXPECT_THAT(alloc, HasSubstr("refused = poolRefusal(in->bytes, pool_bytes_, critical_);"));
	EXPECT_THAT(alloc, HasSubstr("tgm_pool::coherentSource(refused, bo->pool_va != nullptr)"));
	// A refusal does not fail the BO: no NoMemory between the tier check and the pool allocation.
	EXPECT_THAT(Between(alloc, "if (!ret && (in->flags & TegraGM20BBoCoherent)) {",
			    "if (!ret && bo->pool_va)"),
		    Not(HasSubstr("kIOReturnNoMemory")));
	EXPECT_THAT(client(), Not(HasSubstr("GPU pool BoAlloc of %llu KiB refused")));
	// A system BO: a write-combined physical view over its pages.
	EXPECT_THAT(
		Between(alloc, "if (!ret && bo->bmd && system) {", "} else if (!ret && bo->bmd) {"),
		HasSubstr("kIOMemoryTypePhysical64) | kPoolCacheMode"));
}

TEST(GM20BPoolPolicySource, SystemBosGoAsPoolViewsDo)
{
	const std::string release =
		flat(definition(client(), "void AGXDeviceUserClient::releaseBo(Bo *bo)"));
	EXPECT_THAT(release, HasSubstr("if ((bo->pool_va || bo->system) && bo->view && (orphan = "
				       "IONew(PoolOrphan, 1))) {"));
	EXPECT_THAT(release, HasSubstr("bo->system ? bo->bmd : nullptr"));
	EXPECT_THAT(
		release,
		HasSubstr(
			"!bo->system && !(bo->flags & TegraGM20BBoContiguous) &&"));  // its spare goes by its orphan
	expectBefore(flat(definition(client(), "void releaseOrphans()")),
		     "orphan->view->release();", "orphan->backing->release();");
	// Never read through the cacheable alias: the stats and a hang's line both read pushes through pushView.
	EXPECT_THAT(flat(definition(client(), "const uint32_t *AGXDeviceUserClient::pushView(")),
		    HasSubstr("!hit || hit->bo->system ? nullptr"));
	EXPECT_THAT(flat(definition(client(), "void AGXDeviceUserClient::scanPushes(")),
		    HasSubstr("pushView(va, bytes, hit)"));
	EXPECT_THAT(flat(definition(client(), "int AGXDeviceUserClient::hungInterlocks(")),
		    HasSubstr("pushView(va[i], uint64_t(dwords[i]) * 4, hit)"));
}

// A system BO takes a ready spare as a cached BO does, and its pages go back to the spares once its view is released.
// Every source leaves the pages in one state: zero in DRAM, no line in the cacheable kernel alias (cleanZeros: KMA_ZERO's
// zeros cleaned and invalidated, or a recycled spare zeroed through the alias and then cleaned and invalidated).
TEST(GM20BPoolPolicySource, SystemBosTakeAndReturnSpares)
{
	const std::string alloc =
		flat(definition(client(), "IOReturn AGXDeviceUserClient::boAlloc("));
	EXPECT_THAT(alloc, HasSubstr("} else if (!ret && !(in->flags & TegraGM20BBoContiguous) && "
				     "(spare = spareTake(in->bytes))) {"));
	EXPECT_THAT(alloc, Not(HasSubstr("|| system ? kClasses")));
	EXPECT_THAT(
		alloc,
		HasSubstr("(in->flags & TegraGM20BBoContiguous) ? kClasses : classUp(in->bytes);"));
	EXPECT_THAT(alloc, HasSubstr("cleanZeros(va, backing, false);"));
	EXPECT_THAT(flat(definition(client(), "Spare *spareMake(uint64_t bytes)")),
		    HasSubstr("cleanZeros(bmd->getBytesNoCopy(), bytes, false);"));
	const std::string work = flat(definition(
		client(), "void spareWork(thread_call_param_t param, thread_call_param_t)"));
	expectBefore(work, "cleanZeros(s->bmd->getBytesNoCopy(), s->bytes, true);",
		     "p->ready[c] = s;");
	const std::string zeros =
		flat(definition(client(), "void cleanZeros(void *va, uint64_t bytes, bool zero)"));
	expectBefore(zeros, "bzero(va, bytes);", "dc civac");
	// A spare is only ever a BO's pages once no task maps them, nothing imports them (the view's release waits for
	// every import's UPL) and the freeing connection's channels finished (reap).
	const std::string orphans = flat(definition(client(), "void releaseOrphans()"));
	expectBefore(orphans, "if (orphan->view->getRetainCount() > 1) {",
		     "orphan->view->release();");
	expectBefore(orphans, "orphan->view->release();",
		     "spareKeep(orphan->backing, orphan->backingPas, orphan->backingBytes)");
	EXPECT_THAT(orphans, HasSubstr("orphan->zero &&"));
	EXPECT_THAT(flat(definition(client(), "bool spareKeep(IOBufferMemoryDescriptor *bmd, "
					      "uint64_t *pas, uint64_t bytes)")),
		    HasSubstr("bmd->getRetainCount() == 1"));
	const std::string release =
		flat(definition(client(), "void AGXDeviceUserClient::releaseBo(Bo *bo)"));
	expectBefore(release, "tgm_mem_del(&bo->mem);", "bo->system ? bo->pas : nullptr");
	EXPECT_THAT(release, HasSubstr("bo->system ? uint64_t(bo->backing_pages) << 12 : 0"));
	const std::string free =
		flat(definition(client(), "IOReturn AGXDeviceUserClient::boFree("));
	expectBefore(free, "unbindOverlaps(binds_[i].va, binds_[i].range, bo);", "reap(false);");
}

TEST(GM20BPoolPolicySource, StatsNameThePoolHolders)
{
	const std::string stats = flat(gpu());
	EXPECT_THAT(
		stats,
		HasSubstr("tgm_pool_breakdown(&pool_clients, &pool_orphan_bytes, &pool_orphans, "
			  "&pool_system_bytes, &pool_systems);"));
	EXPECT_THAT(
		stats,
		HasSubstr(
			"pool %lu/%llu MiB (compositor %llu, clients \" \"%llu, orphans %llu in"));
}

TEST(GM20BPoolScrubSource, NoCpuClearOfTheReservedPool)
{
	const std::string init = definition(SourceText("../shim/xnu_glue.cpp"),
					    "extern \"C\" int nvxnu_pool_init(int *zeroed)");
	EXPECT_THAT(
		Between(init, "switch_ram_gpu_pool(&start, &end)", "IOBufferMemoryDescriptor *md"),
		Not(HasSubstr("memset")));
	EXPECT_THAT(init, HasSubstr("*zeroed = 0;"));
	EXPECT_THAT(init, HasSubstr("*zeroed = 1;"));  // the fallback still zeroes its own pages
	EXPECT_THAT(definition(SourceText("../shim/linux.c"), "int nvxnu_pool_setup(void)"),
		    HasSubstr("bitmap_set(pool_dirty, 0, pool_npages)"));
}

TEST(GM20BPoolScrubSource, EveryAllocationZeroesWhatAFreeLeftDirty)
{
	const std::string shim = SourceText("../shim/linux.c");
	for (const char *signature :
	     {"struct page *alloc_pages(gfp_t gfp, unsigned int order)",
	      "void *dma_alloc_attrs(struct device *dev, size_t size, dma_addr_t *handle, gfp_t "
	      "gfp,",
	      "void *nvxnu_pool_alloc_clean(unsigned long size, unsigned long align, unsigned long "
	      "long *pa)"})
		EXPECT_THAT(definition(shim, signature), HasSubstr("pool_zero_dirty("))
			<< signature;
	const std::string zero = definition(
		shim, "static void pool_zero_dirty(unsigned long first, unsigned long n, int all)");
	EXPECT_THAT(zero, HasSubstr("test_bit(first + k, pool_dirty)"));
	EXPECT_THAT(zero, HasSubstr("bitmap_clear(pool_dirty, first, n)"));
	// A plain free marks pages dirty; only a zeroed free (CPU or scrubber) marks them clean.
	EXPECT_THAT(definition(shim, "static void pool_free(unsigned long first, unsigned long n)"),
		    HasSubstr("bitmap_set(pool_dirty, first, n)"));
	const std::string claim = definition(shim, "unsigned long nvxnu_pool_claim_dirty(unsigned "
						   "long max_bytes, unsigned long long *pa)");
	EXPECT_THAT(claim, HasSubstr("pool_dirty[w] & ~pool_map[w]"));
	EXPECT_THAT(
		claim,
		HasSubstr(
			"bitmap_set(pool_map, first, n)"));  // claimed ranges are allocated: nobody takes them
}

// A process maps whole VM pages (16 KiB, four pool pages): every pool BO and fence slot it can map is whole VM pages,
// so its mapping never reaches another BO's, another connection's fence or nvkm's own memory.
TEST(GM20BPoolIsolationSource, CpuMappedPoolMemoryIsWholeVmPages)
{
	EXPECT_THAT(
		definition(gpu(), "void *tgm_pool_alloc(uint64_t bytes, uint64_t *pa)"),
		HasSubstr(
			"nvxnu_pool_alloc_clean(pool_bo_bytes(bytes), nvxnu_vm_page_size(), &h)"));
	EXPECT_THAT(definition(gpu(), "void tgm_pool_free(void *va, uint64_t bytes, uint64_t pa)"),
		    HasSubstr("pool_bo_bytes(bytes)"));
	EXPECT_THAT(definition(gpu(),
			       "void tgm_pool_free_zeroed(void *va, uint64_t bytes, uint64_t pa)"),
		    HasSubstr("bytes = pool_bo_bytes(bytes)"));
	EXPECT_THAT(definition(SourceText("../shim/linux.c"),
			       "void *nvxnu_pool_alloc_clean(unsigned long size, unsigned long "
			       "align, unsigned long long *pa)"),
		    HasSubstr("align >> PAGE_SHIFT"));
	EXPECT_THAT(
		definition(gpu(), "int tgm_fence_page_get(volatile uint32_t **va, uint64_t *pa)"),
		HasSubstr("i * TGM_FENCE_SLOT"));
	EXPECT_THAT(client(), HasSubstr("IOAddressRange range = {fence_pa_, "
					"static_cast<IOByteCount>(PAGE_SIZE)};"));
}

TEST(GM20BPoolScrubSource, ScrubberFreesOnlyFromItsFence)
{
	const std::string fenced =
		definition(gpu(), "static void scrub_fenced(struct tgm_chan *chan)");
	EXPECT_THAT(fenced, HasSubstr("nvxnu_pool_free_zeroed("));
	EXPECT_THAT(fenced, HasSubstr("tgm_chan_completed(chan)"));
	EXPECT_EQ(Count(gpu(), "nvxnu_pool_free_zeroed("), 2u)
		<< "tgm_pool_free_zeroed's and the scrubber's";
	const std::string submit = definition(gpu(), "static int scrub_submit(u64 pa, u64 bytes)");
	EXPECT_THAT(submit, HasSubstr("SET_REMAP_CONST_A, 0)"));
	EXPECT_THAT(
		submit,
		HasSubstr(
			"chan_fence(chan)"));  // WFI, FB flush, L2 flush, release, nonstall interrupt
	EXPECT_THAT(submit, HasSubstr("nvxnu_work_schedule_after(scrub.deadline, SCRUB_NS)"));
	EXPECT_THAT(submit, Not(HasSubstr("nvxnu_idle")));  // no CPU wait for the copy engine
	EXPECT_THAT(definition(gpu(), "static void scrub_deadline(void *arg)"),
		    HasSubstr("nvxnu_panic("));
	EXPECT_THAT(
		definition(
			gpu(),
			"static int chan_nonstall(struct nvif_event *event, void *repv, u32 repc)"),
		HasSubstr("chan->kernel == CHAN_SCRUB"));
}

TEST(GM20BPoolScrubSource, FreedPoolBosGoToTheGpuFirst)
{
	expectBefore(
		definition(client(), "bool poolZeroLater(void *va, uint64_t pa, uint64_t bytes)"),
		"tgm_pool_scrub(pa, bytes)", "spares()");
}

// The user client reads pushes only through the header gm20b_push_scan_test tests, and profiles only what it read.
TEST(GM20BPushScanSource, KernelUsesTheHeader)
{
	EXPECT_THAT(client(), HasSubstr("#include \"TegraGM20BPushScan.h\""));
	EXPECT_THAT(client(), Not(HasSubstr("void pushMethod(")));
	EXPECT_THAT(client(), Not(HasSubstr("void pushParse(")));
	const std::string scan =
		Between(client(), "void AGXDeviceUserClient::scanPushes(", "\n}\n");
	EXPECT_THAT(scan, HasSubstr("scanPush(c, p, whole, nullptr, 0)"));
	EXPECT_THAT(scan, HasSubstr("prof->cls[i] = cls;"));
	// A stamp only after a push whose class is not SKIP, only on a profiled submission.
	EXPECT_THAT(Between(gpu(), "static int chan_exec(", "\n}\n"),
		    HasSubstr("if (prof && prof->cls[i] != TGM_PC_SKIP) {"));
}

// Direct scanout's fence-only present (NativeMobileDisplay, TEGRA_SCANOUT_COPY_FENCE_ONLY) launches no copy.
TEST(GM20BPresentSource, FenceOnlyPresentHasNoCopy)
{
	expectBefore(Between(gpu(), "static int present_copy(", "\n}\n"), "if (dst_va) {",
		     "NVB0B5_LAUNCH_DMA_NON_PIPELINED");
}

// A tiled (block-linear) source: the copy engine reads it block-linear with the tiled block height, never as pitch, and
// the user client records a surface as tiled only from a bind of the whole surface.
TEST(GM20BPresentSource, TiledSourceIsCopiedBlockLinear)
{
	const std::string copy = flat(Between(gpu(), "static int present_copy(", "\n}\n"));
	expectBefore(copy, "if (src_block) PUSH_NVSQ(push, NVA0B5, NVB0B5_SET_SRC_BLOCK_SIZE",
		     "NVB0B5_LAUNCH_DMA");
	EXPECT_THAT(copy, HasSubstr("(src_block ? 0 : NVB0B5_LAUNCH_DMA_SRC_PITCH)"));
	EXPECT_THAT(flat(Between(client(), "IOReturn AGXDeviceUserClient::vmBind(", "\n}\n")),
		    HasSubstr("if (tiled) tgm_coherency_tiled_bind(bo->coh, 1);"));
	EXPECT_THAT(flat(Between(client(), "void AGXDeviceUserClient::unbindOverlaps(", "\n}\n")),
		    HasSubstr("if (b.tiled) tgm_coherency_tiled_bind(b.bo->coh, -1);"));
}

// System sleep (gpu.c "system sleep"): every path that touches the GPU outside gpu.lock goes through the gate
// gm20b_sleep_test runs, and sleep and wake keep the orders the hardware needs.
TEST(GM20BSleepSource, GpuTouchingPathsGoThroughTheGate)
{
	for (const char *waits : {"int tgm_chan_exec(", "int tgm_chan_new("}) {
		const std::string body = flat(definition(gpu(), waits));
		expectBefore(body, "gate_enter();", "gate_leave();");
	}
	for (const char *refused :
	     {"int tgm_present_copy(", "int tgm_pool_scrub(uint64_t pa, uint64_t bytes)",
	      "static void scrub_work(void *arg)"}) {
		const std::string body = flat(definition(gpu(), refused));
		expectBefore(body, "if (!gate_try())", "gate_leave();");
	}
	// the drain's and the wake's fence waits are woken by the scrubber's fences too
	EXPECT_THAT(flat(definition(gpu(), "static int chan_nonstall(struct nvif_event *event, "
					   "void *repv, u32 repc)")),
		    HasSubstr("if (tgm_gate_is_shut(&sc7_gate)) nvxnu_wake();"));
	// a pass for an interrupt taken before the GPU's power went reads nothing
	EXPECT_THAT(flat(definition(
			    SourceText("../shim/xnu_glue.cpp"),
			    "void gpuInterrupt(OSObject *, IOInterruptEventSource *source, int)")),
		    HasSubstr("if (__atomic_load_n(&gGpuOff, __ATOMIC_ACQUIRE)) return;"));
}

// Fence waits sleep on their channel's word: a fence interrupt wakes only the waiters of channels whose fence moved,
// a kill or a teardown wakes that channel's, and the interrupt pass's own wake is left to the kernel's waits.
TEST(GM20BFenceWakeSource, WaitersWakeByChannel)
{
	const std::string nonstall = flat(definition(
		gpu(), "static int chan_nonstall(struct nvif_event *event, void *repv, u32 repc)"));
	EXPECT_THAT(nonstall, HasSubstr("else chan_wake_waiters(chan);"));
	EXPECT_EQ(Count(nonstall, "nvxnu_wake();"),
		  1u);  // the scrubber's, while the sleep gate is shut
	EXPECT_THAT(flat(definition(gpu(), "static void chan_wake_waiters(struct tgm_chan *chan)")),
		    HasSubstr("if (done == chan->woken_done) return;"));
	EXPECT_THAT(flat(definition(gpu(), "static int chan_killed(struct nvif_event *event, void "
					   "*repv, u32 repc)")),
		    HasSubstr("chan_wake_all_waiters(chan);"));
	expectBefore(flat(definition(gpu(), "void tgm_chan_del(struct tgm_chan **pchan)")),
		     "*pchan = NULL;", "nvxnu_word_wake(&waiter_words[waiter]);");
	EXPECT_THAT(flat(definition(
			    SourceText("../shim/xnu_glue.cpp"),
			    "void gpuInterrupt(OSObject *, IOInterruptEventSource *source, int)")),
		    HasSubstr("nvxnu_wake();"));
	const std::string wait =
		flat(Between(client(), "IOReturn AGXDeviceUserClient::waitFence(", "\n}\n"));
	expectBefore(wait, "seq = tgm_waiter_seq(waiter);", "tgm_chan_completed(chan) - in->seqno");
	EXPECT_THAT(wait, HasSubstr("tgm_waiter_idle(waiter, seq, deadline)"));
	EXPECT_THAT(wait, Not(HasSubstr("tgm_idle(")));
}

// A killed channel is dead alone (client fault isolation): what names it refuses, the connection's memory, binds and other
// channels go on, a new channel can be made in it, and nothing another channel waits for stays on the dead one.
TEST(GM20BKillSource, AKilledChannelLeavesItsConnectionWorking)
{
	const std::string run =
		flat(definition(client(), "IOReturn AGXDeviceUserClient::run(uint32_t method,"));
	EXPECT_THAT(client(), Not(HasSubstr("dead_")));
	EXPECT_THAT(run, Not(HasSubstr("tgm_chan_error(")));
	EXPECT_THAT(flat(definition(gpu(), "static int chan_exec(struct tgm_chan *chan,")),
		    HasSubstr("if (chan->error) return -EIO;"));
	const std::string order =
		flat(definition(gpu(), "int tgm_chan_order(struct tgm_chan *chan,"));
	EXPECT_THAT(order, HasSubstr("if (READ_ONCE(chan->error)) return -EIO;"));
	EXPECT_THAT(order, HasSubstr("READ_ONCE(other->error)) continue;"));
	const std::string killed = flat(definition(
		gpu(), "static int chan_killed(struct nvif_event *event, void *repv, u32 repc)"));
	EXPECT_THAT(killed, HasSubstr("WRITE_ONCE(*chan->fence_cpu, chan->submitted);"));
	EXPECT_THAT(killed, HasSubstr("chan->kernel || !tgm_all_clients()"));
	EXPECT_THAT(flat(definition(client(), "void AGXDeviceUserClient::reap(bool force)")),
		    HasSubstr("chans_[i] && !tgm_chan_error(chans_[i]) &&"));
}

// The raw path's reopen after its channel is killed: the shader heap's BO and VA outlive the channel, so every program
// placed in it (Device::place) keeps its slot and every state that baked its offset stays drawable; the new channel gets
// what lives in a channel's context again, the program region at the heap, the texture and sampler pools and the root
// constant buffer (emitContext, from the pack's relocations) and the 2D state, and the first command buffer's prologue
// invalidates the shader caches before any draw.
TEST(GM20BRawReopenSource, ReopenKeepsTheHeapAndReplaysTheContext)
{
	const std::string raw = SourceText("../../TegraGM20BMetal/raw/runtime/raw_gpu.cpp");
	const std::string reopen = flat(definition(raw, "bool Device::reopen()"));
	EXPECT_THAT(reopen, Not(HasSubstr("placed_")));
	EXPECT_THAT(reopen, Not(HasSubstr("heapPlaced_")));
	EXPECT_THAT(reopen, Not(HasSubstr("pipelines_")));
	EXPECT_THAT(reopen, Not(HasSubstr("heap_")));
	expectBefore(reopen, "k_.channel(kEngines, channel_, fence_)", "emitContext(p);");
	expectBefore(reopen, "emitContext(p);", "fx::emitTwodInit(p);");
	// the interlock's ticket buffer zeroed by the copy engine in the new channel, after the context names it and before
	// the 3D engine runs again
	expectBefore(reopen, "emitContext(p);",
		     "uint32_t(tickets_.va >> 32), uint32_t(tickets_.va)}, SUBC_COPY);");
	expectBefore(reopen, "uint32_t(tickets_.va)}, SUBC_COPY);",
		     "p.m(CE_LAUNCH_DMA, kLaunchPitch, SUBC_COPY);");
	expectBefore(reopen, "p.m(CE_LAUNCH_DMA, kLaunchPitch, SUBC_COPY);",
		     "p.m(WAIT_FOR_IDLE, 0);");
	const std::string context = flat(definition(raw, "void Device::emitContext(Push &p)"));
	EXPECT_THAT(context, HasSubstr("p.m(LOAD_CB_OFFSET, kRootInterlockTickets);"));
	for (const char *reloc :
	     {"case RelocHeapHi:", "case RelocHeapLo:", "case RelocTicHi:", "case RelocTicLo:",
	      "case RelocTscHi:", "case RelocTscLo:", "case RelocRootHi:", "case RelocRootLo:"})
		EXPECT_THAT(context, HasSubstr(reloc));
	EXPECT_THAT(context, HasSubstr("p.inc(CB_SELECTOR_A,"));
	const std::string begin = flat(definition(raw, "CommandBuffer *Device::begin()"));
	expectBefore(begin, "reopen();", "p.m(INVALIDATE_SHADER_CACHES_NO_WFI,");
}

// No spin for GPFIFO or push room: every wait for room sleeps until a fence's interrupt, and the fence reports GET to nvif
// before that interrupt, so the room the GPU freed is there when the waiter looks.
TEST(GM20BRoomSource, RoomIsWaitedForByInterrupt)
{
	EXPECT_EQ(Count(gpu(), "PUSH_WAIT("), 0u);
	EXPECT_EQ(Count(gpu(), "nvif_chan_gpfifo_wait("),
		  2u);  // the room check's comment and chan_wait_room
	const std::string wait = flat(definition(
		gpu(),
		"static int chan_wait_room(struct tgm_chan *chan, u32 gpfifo_nr, u32 push_nr)"));
	expectBefore(wait, "if (!nvxnu_idle(seq, end)",
		     "nvif_chan_gpfifo_wait(&chan->chan, gpfifo_nr, push_nr)");
	expectBefore(flat(definition(gpu(), "static u32 chan_fence(struct tgm_chan *chan)")),
		     "nvif_chan_gpfifo_post(&chan->chan);", "NON_STALL_INTERRUPT, 0);");
}

// The L2 invalidate that makes the CPU's writes visible is ordered on the GPU, in the submission's own stream, ahead of
// a first push that is not prefetched; the CPU does not wait for it. A channel behind its space's GPU-ordered maps
// invalidates its TLB after the L2 (the walk reads page tables through it) in the same entry.
TEST(GM20BL2Source, InvalidateOnTheGpu)
{
	EXPECT_EQ(Count(gpu(), "FLUSH_L2_SYSTEM_INVALIDATE"), 0u);
	const std::string exec = flat(Between(gpu(), "static int chan_exec(", "\n}\n"));
	expectBefore(exec, "tlb = tgm_tlb_take(&chan->tlb, &chan->vm->tlb);",
		     "NVB06F_MEM_OP_D_OPERATION_L2_SYSMEM_INVALIDATE);");
	expectBefore(exec, "NVB06F_MEM_OP_D_OPERATION_L2_SYSMEM_INVALIDATE);",
		     "NVB06F_MEM_OP_D_OPERATION_MMU_TLB_INVALIDATE); PUSH_KICK(&chan->chan.push);");
	expectBefore(exec,
		     "NVB06F_MEM_OP_D_OPERATION_MMU_TLB_INVALIDATE); PUSH_KICK(&chan->chan.push);",
		     "pushes[i].no_prefetch || ((l2_on() || tlb) && !i));");
}

// A CPU IOSurfaceLock waits for the submissions that may write the surface: Exec names each one once it has its seqno.
TEST(GM20BCoherencySource, ExecNamesTheWriterOnceTheSubmissionHasItsSeqno)
{
	const std::string exec = flat(definition(client(), "IOReturn AGXDeviceUserClient::exec("));
	expectBefore(exec, "ioreturn(tgm_chan_exec(chan, pushes, count, &seqno,",
		     "tgm_coherency_wrote(bo->coh, importsWritten_, tgm_chan_uid(chan), seqno);");
}

// A SurfaceWrites a client could not deliver never leaves a GPU write unmarked: the client stops saying its imports are
// only sampled and flags every submission TegraGM20BPushImportsWritten, which makes the connection mark every writable
// import at every Exec.
TEST(GM20BCoherencySource, AFailedSurfaceWritesFallsBackToMarkingEveryImport)
{
	const std::string exec = flat(definition(client(), "IOReturn AGXDeviceUserClient::exec("));
	expectBefore(exec, "importsWritten_ = true;",
		     "tgm_coherency_wrote(bo->coh, importsWritten_, tgm_chan_uid(chan), seqno);");
	const std::string raw = SourceText("../../TegraGM20BMetal/raw/runtime/raw_kernel.cpp");
	const std::string declare = flat(definition(raw, "bool Kernel::surfaceGpuWrites("));
	EXPECT_THAT(declare, HasSubstr("importsWritten_.store(true"));
	EXPECT_THAT(flat(definition(raw, "bool Kernel::exec(")),
		    HasSubstr("e.pushes[0].flags |= TegraGM20BPushImportsWritten;"));
	EXPECT_THAT(flat(definition(raw, "bool Kernel::importPages(")),
		    HasSubstr("!importsWritten_.load("));
	const std::string nvk = SourceText(
		"../../../third_party/mesa/src/nouveau/vulkan/nvkmd/darwin/nvkmd_darwin.c");
	EXPECT_THAT(flat(definition(nvk, "switch_nvk_surface_gpu_writes(uint32_t surface_id)")),
		    HasSubstr("p_atomic_set(&surface_writes_failed, true);"));
	EXPECT_THAT(flat(definition(nvk, "nvkmd_darwin_exec_ctx_flush(struct nvkmd_ctx *_ctx,")),
		    HasSubstr("ctx->req.pushes[0].flags |= TegraGM20BPushImportsWritten;"));
	EXPECT_THAT(nvk,
		    HasSubstr("if (req.surface_id && !p_atomic_read(&surface_writes_failed))"));
	// The frontend and the raw path's kernel connection (raw_kernel.cpp) reach these with dlsym; Mesa builds hidden by
	// default. Without the connection's, the raw path opens no device and the render server stays on m2v alone.
	for (const char *exported :
	     {"__attribute__((visibility(\"default\"))) void\nswitch_nvk_import_surface(",
	      "__attribute__((visibility(\"default\"))) bool\nswitch_nvk_surface_gpu_writes(",
	      "__attribute__((visibility(\"default\"))) bool\nswitch_nvk_connection(",
	      "__attribute__((visibility(\"default\"))) void\nswitch_nvk_connection_release(",
	      "__attribute__((visibility(\"default\"))) void\nswitch_nvk_import_foreign("})
		EXPECT_THAT(nvk, HasSubstr(exported));
}

// Only a connection's maps skip the CPU's L2 and TLB maintenance; every removal keeps it (TegraGM20BTlb.h).
TEST(GM20BTlbSource, MapsOrderedOnTheGpuRemovalsSynchronous)
{
	EXPECT_THAT(client(), HasSubstr("tgm_vm_map_ordered(vm_, op.va"));
	EXPECT_EQ(Count(client(), "tgm_vm_map("), 0u);
	EXPECT_EQ(Count(gpu(), "nvxnu_gpu_ordered_begin();"), 1u);
	const std::string map = flat(definition(gpu(), "static int vm_map("));
	expectBefore(map, "nvxnu_gpu_ordered_end();", "nvif_vmm_raw_put(");
	const std::string unmap = flat(definition(gpu(), "int tgm_vm_unmap("));
	EXPECT_THAT(unmap, Not(HasSubstr("gpu_ordered")));
	EXPECT_THAT(flat(definition(gpu(), "void tgm_vm_del(")),
		    HasSubstr("mmu_invalidate_all();"));
}

TEST(GM20BSleepSource, SleepAndWakeOrder)
{
	const std::string body = flat(definition(gpu(), "void tgm_sleep(void)"));
	const std::string sleep = body.substr(body.find("case TGM_SLEEP_FULL:"));
	expectBefore(sleep, "sleep_gate();", "sleep_fences();");
	expectBefore(sleep, "sleep_fences();", "mutex_lock(&gpu.lock);");
	expectBefore(sleep, "sleep_idle_off();", "sleep_nvkm();");
	expectBefore(sleep, "sleep_nvkm();", "sleep_power_down();");
	const std::string nvkm = flat(definition(gpu(), "static void sleep_nvkm(void)"));
	expectBefore(nvkm, "nvif_client_suspend(&gpu.client, false)", "nvkm_ltc_flush(");
	expectBefore(nvkm, "nvkm_ltc_flush(", "nvkm_device_fini(device, NVKM_SUSPEND)");
	expectBefore(flat(definition(gpu(), "static void sleep_power_down(void)")),
		     "nvxnu_irq_suspend();", "regulator_disable(");
	// the boot's order: the GPCPLL relock (clk_raise) before ACR, the PMU and GR (m2_device_init); interrupts on before
	// nvkm's first init that may wait for one
	const std::string woken = flat(definition(gpu(), "void tgm_wake(void)"));
	const std::string wake = woken.substr(woken.find("case TGM_SLEEP_FULL:"));
	expectBefore(wake, "wake_power_up();", "wake_m1();");
	expectBefore(wake, "wake_m1();", "wake_clock();");
	expectBefore(wake, "wake_clock();", "wake_device();");
	expectBefore(wake, "wake_device();", "wake_nvkm();");
	expectBefore(wake, "wake_nvkm();", "wake_fence();");
	expectBefore(wake, "wake_fence();", "wake_open();");
	expectBefore(flat(definition(gpu(), "static void wake_power_up(void)")), "power_up(",
		     "nvxnu_irq_resume();");
	EXPECT_THAT(flat(definition(gpu(), "static void wake_clock(void)")),
		    HasSubstr("clk_raise(line, sizeof(line))"));
	EXPECT_THAT(flat(definition(gpu(), "static void wake_device(void)")),
		    HasSubstr("m2_device_init(device, &at)"));
}

// NVK binds a plane with a PTE kind over a VA rounded to the bind alignment (16 KiB: the page size), so a D32_SFLOAT_S8_UINT
// image's stencil copy temp starts on it, like its planes, and its bind ends inside the image's memory (TegraGM20B refuses a
// bind past its BO: RenderBox's memoryless depth/stencil targets came back nil and the widget drew black).
TEST(NvkImageSource, StencilCopyTempStartsOnTheBindAlignment)
{
	const std::string image =
		flat(SourceText("../../../third_party/mesa/src/nouveau/vulkan/nvk_image.c"));
	const std::string temp =
		image.substr(image.find("&image->stencil_copy_temp.nil, &stencil_nil_info);"));
	EXPECT_THAT(temp.substr(0, temp.find("uint64_t plane_offset_B = 0;")),
		    HasSubstr("if (image->stencil_copy_temp.nil.pte_kind) "
			      "image->stencil_copy_temp.plane_align_B = "
			      "MAX2(image->stencil_copy_temp.plane_align_B, "
			      "pdev->nvkmd->bind_align_B);"));
	EXPECT_THAT(flat(definition(image, "nvk_image_plane_alloc_va(struct nvk_device *dev,")),
		    HasSubstr("const uint64_t va_align_B = plane->plane_align_B;"));
}
