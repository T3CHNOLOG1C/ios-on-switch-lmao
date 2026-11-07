// SPDX-License-Identifier: MIT
// TegraGM20BCoherency.cpp on the host: which IOSurface lock/unlock traps clean or invalidate an import, with how many
// pages, across imports of the same surface and their release.
#include <atomic>
#include <cstdio>
#include <map>
#include <set>
#include <thread>
#include <vector>
#include <gtest/gtest.h>
#include "drivers/TegraGM20B/TegraGM20BGpu.h"
#include "drivers/TegraDisplayFlip/TegraScanoutCopy.h"

std::vector<std::vector<uint64_t>> &cleanCalls()
{
	static std::vector<std::vector<uint64_t>> calls;
	return calls;
}

static std::mutex gCleanLock;

// The kernel's process name and ID, for the debug traces of CPU locks and unlocks.
extern "C" void proc_selfname(char *buf, int size)
{
	snprintf(buf, size_t(size), "coherency_test");
}

extern "C" int proc_selfpid(void)
{
	return 1;
}

extern "C" void tgm_cpu_clean_pages(const uint64_t *pas, uint32_t npages)
{
	std::lock_guard<std::mutex> hold(gCleanLock);
	cleanCalls().emplace_back(pas, pas + npages);
}

// The GPU's fences as the registry asks after them: each channel's last completed seqno; a channel not listed is gone.
// A wait is the fence interrupt arriving: the channel completes what the wait was for.
struct FakeGpu {
	std::map<uint32_t, uint32_t> completed;
	std::set<uint32_t> killed;
	std::vector<std::pair<uint32_t, uint32_t>> waits;
	bool allDone = true;
	int waitAll = 0;
};

static FakeGpu gGpu;

extern "C" int tgm_writer_done(uint32_t uid, uint32_t seqno)
{
	const auto it = gGpu.completed.find(uid);
	return gGpu.killed.count(uid) || it == gGpu.completed.end() ||
	       int32_t(it->second - seqno) >= 0;
}

extern "C" void tgm_writer_wait(uint32_t uid, uint32_t seqno)
{
	gGpu.waits.emplace_back(uid, seqno);
	gGpu.completed[uid] = seqno;
}

extern "C" int tgm_writers_all_done(void)
{
	return gGpu.allDone;
}

extern "C" void tgm_writers_wait_all(void)
{
	gGpu.waitAll++;
	gGpu.allDone = true;
}

namespace
{
constexpr uint32_t kLock = 2, kUnlock = 3, kReadOnly = 1, kAvoidSync = 2;
constexpr uint32_t kDone = 1;  // a channel whose submissions all completed (FakeGpu: not listed)

std::vector<uint64_t> pages(uint64_t first, uint32_t n)
{
	std::vector<uint64_t> v;
	for (uint32_t i = 0; i < n; i++)
		v.push_back(first + i * 0x2000);
	return v;
}

tgm_coh *add(uint64_t pa, uint32_t surfaceId, const uint64_t *pas, uint32_t npages, int writable,
	     int *covered = nullptr)
{
	int ignored = 0;
	return tgm_coherency_add(pa, surfaceId, pas, npages, writable, writable,
				 covered ? covered : &ignored);
}

tgm_coh *sampled(uint64_t pa, uint32_t surfaceId, const uint64_t *pas, uint32_t npages)
{
	int ignored = 0;
	return tgm_coherency_add(pa, surfaceId, pas, npages, 1, 0, &ignored);
}

class Coherency : public ::testing::Test
{
protected:
	void SetUp() override
	{
		ASSERT_EQ(tgm_coherency_init(), 0);
		cleanCalls().clear();
		gGpu = FakeGpu{};
	}
};

TEST_F(Coherency, LockInvalidatesOnlyAfterGpuWrites)
{
	const auto p = pages(0x80000000, 4);
	tgm_coh *c = add(p[0], 7, p.data(), 4, 1);
	ASSERT_NE(c, nullptr);
	tegra_gm20b_iosurface_trap(kLock, 7, 0);
	EXPECT_TRUE(cleanCalls().empty());
	tgm_coherency_wrote(c, 0, kDone, 1);
	tegra_gm20b_iosurface_trap(kLock, 7, kReadOnly);
	ASSERT_EQ(cleanCalls().size(), 1u);
	EXPECT_EQ(cleanCalls()[0], p);
	tegra_gm20b_iosurface_trap(kLock, 7, 0);
	EXPECT_EQ(cleanCalls().size(), 1u);
	tgm_coherency_put(c);
}

TEST_F(Coherency, ASurfaceTheGpuOnlySamplesIsNeverInvalidatedOnLock)
{
	const auto p = pages(0x8a000000, 4);
	tgm_coh *c = sampled(p[0], 61, p.data(), 4);
	tgm_coherency_wrote(c, 0, kDone, 1);
	tegra_gm20b_iosurface_trap(kLock, 61, 0);
	EXPECT_TRUE(cleanCalls().empty());
	tgm_coherency_gpu_writes(61);
	tgm_coherency_wrote(c, 0, kDone, 1);
	tegra_gm20b_iosurface_trap(kLock, 61, 0);
	ASSERT_EQ(cleanCalls().size(), 1u);
	EXPECT_EQ(cleanCalls()[0], p);
	tgm_coherency_put(c);
}

// A SurfaceWrites the client could not deliver: its submissions carry TegraGM20BPushImportsWritten from then on, which
// marks even the imports that said the GPU only samples them.
TEST_F(Coherency, AnUndeclaredWriteStillInvalidatesOnLockOnceTheClientFallsBack)
{
	const auto p = pages(0x8d000000, 3);
	tgm_coh *c = sampled(p[0], 63, p.data(), 3);
	tgm_coherency_wrote(c, 1, kDone, 1);
	tegra_gm20b_iosurface_trap(kLock, 63, kReadOnly);
	ASSERT_EQ(cleanCalls().size(), 1u);
	EXPECT_EQ(cleanCalls()[0], p);
	tgm_coherency_put(c);
}

TEST_F(Coherency, ARenderedToImportMakesTheSurfaceGpuWrittenForEveryImporter)
{
	const auto p = pages(0x8b000000, 2);
	tgm_coh *reader = sampled(p[0], 62, p.data(), 2);
	tgm_coh *renderer = add(p[0], 62, p.data(), 2, 1);
	ASSERT_EQ(reader, renderer);
	tgm_coherency_wrote(reader, 0, kDone, 1);
	tegra_gm20b_iosurface_trap(kLock, 62, kReadOnly);
	EXPECT_EQ(cleanCalls().size(), 1u);
	tgm_coherency_put(renderer);
	tgm_coherency_put(reader);
}

TEST_F(Coherency, TrapsReachEveryEntryOfASurface)
{
	std::vector<std::vector<uint64_t>> lists;
	std::vector<tgm_coh *> entries;
	for (uint64_t i = 0; i < 6; i++) {
		lists.push_back(pages(0x8c000000 + (i << 24), 2));
		entries.push_back(add(lists.back()[0], 71, lists.back().data(), 2, 1));
		tgm_coherency_wrote(entries.back(), 0, kDone, 1);
	}
	tegra_gm20b_iosurface_trap(kUnlock, 71, 0);
	EXPECT_EQ(cleanCalls().size(), 6u);
	tegra_gm20b_iosurface_trap(kLock, 71, 0);
	EXPECT_EQ(cleanCalls().size(), 12u);
	for (tgm_coh *c : entries)
		tgm_coherency_put(c);
}

TEST_F(Coherency, UnlockCleansUnlessReadOnly)
{
	const auto p = pages(0x81000000, 3);
	tgm_coh *c = add(p[0], 9, p.data(), 3, 0);
	tegra_gm20b_iosurface_trap(kUnlock, 9, kReadOnly);
	EXPECT_TRUE(cleanCalls().empty());
	tegra_gm20b_iosurface_trap(kUnlock, 9, 0);
	ASSERT_EQ(cleanCalls().size(), 1u);
	EXPECT_EQ(cleanCalls()[0], p);
	tgm_coherency_put(c);
}

TEST_F(Coherency, OtherSurfacesAndUntaggedImportsAreLeftAlone)
{
	const auto p = pages(0x82000000, 2), q = pages(0x83000000, 2);
	tgm_coh *tagged = add(p[0], 11, p.data(), 2, 1);
	tgm_coh *untagged = add(q[0], 0, q.data(), 2, 1);
	tgm_coherency_wrote(tagged, 0, kDone, 1);
	tgm_coherency_wrote(untagged, 0, kDone, 1);
	tegra_gm20b_iosurface_trap(kUnlock, 12, 0);
	tegra_gm20b_iosurface_trap(kLock, 12, 0);
	tegra_gm20b_iosurface_trap(kLock, 0, 0);
	tegra_gm20b_iosurface_trap(1, 11, 0);
	EXPECT_TRUE(cleanCalls().empty());
	tgm_coherency_put(tagged);
	tgm_coherency_put(untagged);
}

TEST_F(Coherency, OneEntryPerSurfaceUntilTheLastImportGoes)
{
	const auto small = pages(0x84000000, 2), large = pages(0x84000000, 5);
	tgm_coh *a = add(small[0], 0, small.data(), 2, 0);
	tgm_coh *b = add(large[0], 21, large.data(), 5, 1);
	EXPECT_EQ(a, b);
	tegra_gm20b_iosurface_trap(kUnlock, 21, 0);
	ASSERT_EQ(cleanCalls().size(), 1u);
	EXPECT_EQ(cleanCalls()[0], large);
	tgm_coherency_put(b);
	tegra_gm20b_iosurface_trap(kUnlock, 21, 0);
	EXPECT_EQ(cleanCalls().size(), 2u);
	tgm_coherency_put(a);
	tegra_gm20b_iosurface_trap(kUnlock, 21, 0);
	EXPECT_EQ(cleanCalls().size(), 2u);
}

TEST_F(Coherency, OnlyALiveImportOfTheSameSurfaceCoversAnother)
{
	const auto p = pages(0x87000000, 4), more = pages(0x87000000, 6),
		   other = pages(0x88000000, 4);
	int covered = -1;
	tgm_coh *first = add(p[0], 51, p.data(), 4, 1, &covered);
	EXPECT_EQ(covered, 0);
	tgm_coh *early = add(p[0], 51, p.data(), 4, 0, &covered);
	EXPECT_EQ(covered, 0);
	tgm_coherency_cleaned(first);
	tgm_coh *twin = add(p[0], 51, p.data(), 4, 0, &covered);
	EXPECT_EQ(covered, 1);
	tgm_coh *untagged = add(p[0], 0, p.data(), 4, 0, &covered);
	EXPECT_EQ(covered, 0);
	tgm_coh *larger = add(more[0], 51, more.data(), 6, 0, &covered);
	EXPECT_EQ(covered, 0);
	tgm_coh *elsewhere = add(other[0], 51, other.data(), 4, 0, &covered);
	EXPECT_EQ(covered, 0);
	auto moved = p;
	moved[2] = 0x89000000;
	tgm_coh *recycled = add(moved[0], 51, moved.data(), 4, 0, &covered);
	EXPECT_EQ(covered, 0);
	for (tgm_coh *c : {first, early, twin, untagged, larger, elsewhere, recycled})
		tgm_coherency_put(c);
	tgm_coh *again = add(p[0], 51, p.data(), 4, 1, &covered);
	EXPECT_EQ(covered, 0);
	tgm_coherency_put(again);
}

TEST_F(Coherency, TiledWhileAnyTiledBindLives)
{
	const auto p = pages(0x85000000, 2);
	tgm_coh *c = add(p[0], 31, p.data(), 2, 1);
	EXPECT_FALSE(tegra_gm20b_surface_tiled(p[0]));
	tgm_coherency_tiled_bind(c, 1);
	tgm_coherency_tiled_bind(c, 1);
	tgm_coherency_tiled_bind(c, -1);
	EXPECT_TRUE(tegra_gm20b_surface_tiled(p[0]));
	tgm_coherency_tiled_bind(c, -1);
	EXPECT_FALSE(tegra_gm20b_surface_tiled(p[0]));
	tgm_coherency_put(c);
	EXPECT_FALSE(tegra_gm20b_surface_tiled(p[0]));
}

TEST_F(Coherency, TrapsRaceReleaseWithoutUseAfterFree)
{
	std::atomic<bool> stop{false};
	std::thread trapper([&] {
		while (!stop.load()) {
			tegra_gm20b_iosurface_trap(kUnlock, 41, 0);
			tegra_gm20b_iosurface_trap(kLock, 41, 0);
		}
	});
	for (int i = 0; i < 2000; i++) {
		const auto p = pages(0x86000000 + (uint64_t(i % 8) << 24), 8);
		tgm_coh *c = add(p[0], 41, p.data(), 8, 1);
		tgm_coherency_wrote(c, 0, kDone, 1);
		tgm_coherency_put(c);
	}
	stop.store(true);
	trapper.join();
}
}  // namespace

// Apple's IOSurfaceLock synchronises with the GPU: a lock of a surface whose writer is still running sleeps until it
// completed, then the trap invalidates what it wrote.
TEST_F(Coherency, ALockBeforeTheWriterCompletesWaitsForIt)
{
	const auto p = pages(0x9a000000, 2);
	tgm_coh *c = add(p[0], 71, p.data(), 2, 1);
	gGpu.completed[5] = 9;
	tgm_coherency_wrote(c, 0, 5, 10);
	EXPECT_EQ(tegra_gm20b_iosurface_lock_will(71, kReadOnly), 0);
	ASSERT_EQ(gGpu.waits.size(), 1u);
	EXPECT_EQ(gGpu.waits[0], std::make_pair(5u, 10u));
	tegra_gm20b_iosurface_trap(kLock, 71, kReadOnly);
	ASSERT_EQ(cleanCalls().size(), 1u);
	EXPECT_EQ(tegra_gm20b_iosurface_lock_will(71, 0), 0);
	EXPECT_EQ(gGpu.waits.size(), 1u);
	tgm_coherency_put(c);
}

TEST_F(Coherency, ALockAfterTheWriterCompletedDoesNotWait)
{
	const auto p = pages(0x9b000000, 2);
	tgm_coh *c = add(p[0], 72, p.data(), 2, 1);
	gGpu.completed[5] = 9;
	tgm_coherency_wrote(c, 0, 5, 9);
	EXPECT_EQ(tegra_gm20b_iosurface_lock_will(72, 0), 0);
	EXPECT_TRUE(gGpu.waits.empty());
	tegra_gm20b_iosurface_trap(kLock, 72, 0);
	EXPECT_EQ(cleanCalls().size(), 1u);
	tgm_coherency_put(c);
}

// kIOSurfaceLockAvoidSync: a lock that would have to wait fails (kIOReturnCannotLock) without waiting.
TEST_F(Coherency, AnAvoidSyncLockWithWritesPendingCannotLock)
{
	const auto p = pages(0x9c000000, 2);
	tgm_coh *c = add(p[0], 73, p.data(), 2, 1);
	gGpu.completed[6] = 1;
	tgm_coherency_wrote(c, 0, 6, 2);
	EXPECT_NE(tegra_gm20b_iosurface_lock_will(73, kAvoidSync | kReadOnly), 0);
	EXPECT_TRUE(gGpu.waits.empty());
	gGpu.completed[6] = 2;
	EXPECT_EQ(tegra_gm20b_iosurface_lock_will(73, kAvoidSync | kReadOnly), 0);
	tgm_coherency_put(c);
}

// A killed channel's fence is released: its writes count as complete.
TEST_F(Coherency, AKilledWritersChannelCountsAsComplete)
{
	const auto p = pages(0x9d000000, 2);
	tgm_coh *c = add(p[0], 74, p.data(), 2, 1);
	gGpu.completed[8] = 0;
	tgm_coherency_wrote(c, 0, 8, 4);
	gGpu.killed.insert(8);
	EXPECT_EQ(tegra_gm20b_iosurface_lock_will(74, 0), 0);
	EXPECT_TRUE(gGpu.waits.empty());
	tgm_coherency_put(c);
}

// A later submission of the same channel takes its writer's slot; more channels than slots make a lock wait for all.
TEST_F(Coherency, WritersPerChannelAndTooManyChannels)
{
	const auto p = pages(0x9e000000, 2);
	tgm_coh *c = add(p[0], 75, p.data(), 2, 1);
	gGpu.completed[2] = 0;
	tgm_coherency_wrote(c, 0, 2, 1);
	tgm_coherency_wrote(c, 0, 2, 3);
	EXPECT_EQ(tegra_gm20b_iosurface_lock_will(75, 0), 0);
	ASSERT_EQ(gGpu.waits.size(), 1u);
	EXPECT_EQ(gGpu.waits[0].second, 3u);
	for (uint32_t uid = 10; uid < 16; uid++) {
		gGpu.completed[uid] = 0;
		tgm_coherency_wrote(c, 0, uid, 1);
	}
	gGpu.allDone = false;
	EXPECT_NE(tegra_gm20b_iosurface_lock_will(75, kAvoidSync), 0);
	EXPECT_EQ(tegra_gm20b_iosurface_lock_will(75, 0), 0);
	EXPECT_EQ(gGpu.waitAll, 1);
	tgm_coherency_put(c);
}
