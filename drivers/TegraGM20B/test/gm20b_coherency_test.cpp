// SPDX-License-Identifier: MIT
// TegraGM20BCoherency.cpp on the host: which IOSurface lock/unlock traps clean or invalidate an import, with how many
// pages, across imports of the same surface and their release.
#include <atomic>
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

extern "C" void tgm_cpu_clean_pages(const uint64_t *pas, uint32_t npages)
{
	std::lock_guard<std::mutex> hold(gCleanLock);
	cleanCalls().emplace_back(pas, pas + npages);
}

namespace
{
constexpr uint32_t kLock = 2, kUnlock = 3, kReadOnly = 1;

std::vector<uint64_t> pages(uint64_t first, uint32_t n)
{
	std::vector<uint64_t> v;
	for (uint32_t i = 0; i < n; i++)
		v.push_back(first + i * 0x2000);
	return v;
}

class Coherency : public ::testing::Test
{
protected:
	void SetUp() override
	{
		ASSERT_EQ(tgm_coherency_init(), 0);
		cleanCalls().clear();
	}
};

TEST_F(Coherency, LockInvalidatesOnlyAfterGpuWrites)
{
	const auto p = pages(0x80000000, 4);
	tgm_coh *c = tgm_coherency_add(p[0], 7, p.data(), 4, 1);
	ASSERT_NE(c, nullptr);
	tegra_gm20b_iosurface_trap(kLock, 7, 0);
	EXPECT_TRUE(cleanCalls().empty());
	tgm_coherency_wrote(c);
	tegra_gm20b_iosurface_trap(kLock, 7, kReadOnly);
	ASSERT_EQ(cleanCalls().size(), 1u);
	EXPECT_EQ(cleanCalls()[0], p);
	tegra_gm20b_iosurface_trap(kLock, 7, 0);
	EXPECT_EQ(cleanCalls().size(), 1u);
	tgm_coherency_put(c);
}

TEST_F(Coherency, UnlockCleansUnlessReadOnly)
{
	const auto p = pages(0x81000000, 3);
	tgm_coh *c = tgm_coherency_add(p[0], 9, p.data(), 3, 0);
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
	tgm_coh *tagged = tgm_coherency_add(p[0], 11, p.data(), 2, 1);
	tgm_coh *untagged = tgm_coherency_add(q[0], 0, q.data(), 2, 1);
	tgm_coherency_wrote(tagged);
	tgm_coherency_wrote(untagged);
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
	tgm_coh *a = tgm_coherency_add(small[0], 0, small.data(), 2, 0);
	tgm_coh *b = tgm_coherency_add(large[0], 21, large.data(), 5, 1);
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

TEST_F(Coherency, TiledWhileAnyTiledBindLives)
{
	const auto p = pages(0x85000000, 2);
	tgm_coh *c = tgm_coherency_add(p[0], 31, p.data(), 2, 1);
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
		tgm_coh *c = tgm_coherency_add(p[0], 41, p.data(), 8, 1);
		tgm_coherency_wrote(c);
		tgm_coherency_put(c);
	}
	stop.store(true);
	trapper.join();
}
}  // namespace
