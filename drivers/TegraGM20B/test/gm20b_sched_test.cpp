// SPDX-License-Identifier: MIT
// TegraGM20BSchedPolicy.h (what a compositor kick preempts, the interleaved runlist) and TegraGM20BRecover.h's
// tgm_hang_blame (which channel a late wait kills), over the registers and channels of deploy-2045's hang: GR stuck
// switching from backboardd's channel 3 to Maps' 26, PBDMA0 on 26.
#include <algorithm>
#include <cstdint>
#include <vector>
#include <gtest/gtest.h>
#include "drivers/TegraGM20B/TegraGM20BRecover.h"
#include "drivers/TegraGM20B/TegraGM20BSchedPolicy.h"

namespace
{
constexpr uint32_t kHangEng0 = 0x801ae003;  // busy, ch 3 -> ch 26, ctx_status 7 (switch)
constexpr uint32_t kCtxValid = 1u << 13;

uint32_t resident(uint32_t chid)
{
	return PFIFO_ENG_BUSY | kCtxValid | chid;
}

TEST(KickVictim, GrMidSwitchIsLeftToTheHost)
{
	EXPECT_EQ(tgm_kick_victim(kHangEng0, 0, 1, 26, 0), TGM_VICTIM_SWITCHING);
}

TEST(KickVictim, AppOnGrIsPreempted)
{
	EXPECT_EQ(tgm_kick_victim(resident(26), 0, 1, 26, 0), 26);
}

TEST(KickVictim, AppHoldingThePbdmaWhileGrRunsTheCompositor)
{
	EXPECT_EQ(tgm_kick_victim(resident(3), 1, 1, 26, 0), 26);
}

TEST(KickVictim, NothingWhenOnlyHighChannelsRun)
{
	EXPECT_EQ(tgm_kick_victim(resident(3), 1, 1, 5, 1), TGM_VICTIM_NONE);
	EXPECT_EQ(tgm_kick_victim(0, 0, 0, 0, 0), TGM_VICTIM_NONE);
}

TEST(KickVictim, IdleGrWithAContextLoadedIsNoVictim)
{
	EXPECT_EQ(tgm_kick_victim(kCtxValid | 26, 0, 0, 26, 0), TGM_VICTIM_NONE);
}

TEST(KickVictim, ATsgIdNamesNoChannel)
{
	EXPECT_EQ(tgm_kick_victim(resident(7) | PFIFO_ENG_ID_TSG, 0, 0, 0, 0), TGM_VICTIM_NONE);
}

std::vector<uint16_t> plan(const std::vector<uint8_t> &high, const std::vector<uint16_t> &entries,
			   unsigned max)
{
	std::vector<uint16_t> order(4096);
	order.resize(tgm_interleave(high.data(), entries.data(), unsigned(high.size()), max,
				    order.data(), unsigned(order.size())));
	return order;
}

unsigned entriesOf(const std::vector<uint16_t> &order, const std::vector<uint16_t> &entries)
{
	unsigned n = 0;
	for (uint16_t u : order)
		n += entries[u];
	return n;
}

TEST(Interleave, EveryHighUnitFollowsEachLowOne)
{
	const std::vector<uint8_t> high = {0, 1, 0, 0, 1};
	const std::vector<uint16_t> entries(5, 1);
	EXPECT_EQ(plan(high, entries, 64), (std::vector<uint16_t>{0, 1, 4, 2, 1, 4, 3, 1, 4}));
}

TEST(Interleave, OnlyHighOrOnlyLowListsEachOnce)
{
	EXPECT_EQ(plan({1, 1}, {1, 1}, 64), (std::vector<uint16_t>{0, 1}));
	EXPECT_EQ(plan({0, 0, 0}, {1, 1, 1}, 64), (std::vector<uint16_t>{0, 1, 2}));
	EXPECT_TRUE(plan({}, {}, 64).empty());
}

TEST(Interleave, AFullRunlistSpacesTheHighUnitsOutAndKeepsEveryUnit)
{
	std::vector<uint8_t> high(100, 0);
	std::vector<uint16_t> entries(100, 1);
	high[0] = high[50] = 1;
	entries[50] = 3;  // a group: header and two channels
	const unsigned max = 140;
	const auto order = plan(high, entries, max);
	EXPECT_LE(entriesOf(order, entries), max);
	for (uint16_t u = 0; u < 100; u++)
		EXPECT_NE(std::find(order.begin(), order.end(), u), order.end()) << "unit " << u;
	EXPECT_GT(std::count(order.begin(), order.end(), 0), 1);
}

TEST(Interleave, NoRoomForAnyRepeatListsEachOnce)
{
	const std::vector<uint8_t> high = {0, 1, 0};
	const std::vector<uint16_t> entries = {1, 1, 1};
	EXPECT_EQ(plan(high, entries, 3), (std::vector<uint16_t>{0, 1, 2}));
}

struct Candidates {
	std::vector<tgm_hang_candidate> c;
	int late_dead = 0, late_kernel = 0, progress = 0;

	tgm_hang verdict(int &blame) const
	{
		return tgm_hang_blame(c.data(), int(c.size()), late_dead, late_kernel, progress,
				      &blame);
	}
};

constexpr tgm_hang_candidate kStuckClient = {1, 1, 0}, kBusyClient = {1, 0, 0}, kNone = {0, 0, 0},
			     kStuckKernel = {1, 1, 1}, kBusyKernel = {1, 0, 1};

TEST(HangBlame, Deploy2045KillsTheContextGrCannotSwitch)
{
	// the present waited for backboardd; GR's switch blames Maps (restoring 26), PBDMA0 is on 26 too
	Candidates k{{kStuckClient, kNone, kStuckClient}};
	int blame;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_RECOVERING);
	EXPECT_EQ(blame, 0);
}

TEST(HangBlame, TheCompositorIsRecoveredWhenItIsTheOneStuck)
{
	// GR stuck saving backboardd's context: suspects 3 (backboardd) then 26 (Maps), late is backboardd
	Candidates k{{kStuckClient, kStuckClient, kNone, kStuckClient}};
	int blame;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_RECOVERING);
	EXPECT_EQ(blame, 0);
}

TEST(HangBlame, AChannelOnAnEndlessAcquireBlamesItself)
{
	// GR idle, the PBDMA switched away from it (acquire switch): no suspect, the late channel is stuck
	Candidates k{{kNone, kStuckClient}};
	int blame;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_RECOVERING);
	EXPECT_EQ(blame, 1);
}

TEST(HangBlame, TheTaskChannelItWaitsOnComesBeforeIt)
{
	Candidates k{{kBusyClient, kStuckClient, kStuckClient}};
	int blame;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_RECOVERING);
	EXPECT_EQ(blame, 1);
}

TEST(HangBlame, SlowWorkIsNotKilled)
{
	Candidates k{{kBusyClient, kNone, kBusyClient}};
	int blame;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_PROGRESSING);
	EXPECT_EQ(blame, -1);
}

TEST(HangBlame, AStuckKernelChannelIsFatal)
{
	Candidates k{{kStuckKernel, kNone, kStuckClient}};
	int blame;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_STUCK);
}

TEST(HangBlame, LateKernelWorkPanicsOnlyWhenNothingProgresses)
{
	Candidates k{{kNone, kBusyKernel}};
	k.late_kernel = 1;
	int blame;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_STUCK);
	k.progress = 1;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_PROGRESSING);
}

TEST(HangBlame, ALateChannelAlreadyKilledIsRecovering)
{
	Candidates k{{kNone, {1, 0, 0}}};
	k.late_dead = 1;
	int blame;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_RECOVERING);
	EXPECT_EQ(blame, -1);
}
}  // namespace
