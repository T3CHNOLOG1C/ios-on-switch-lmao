// Host test of drivers/SwitchTouch/SwitchTouchFilter.h: one contact's samples in, its reported point out.
#include <cstdlib>
#include <vector>
#include <gtest/gtest.h>
#include "../SwitchTouchFilter.h"

namespace stf = switch_touch_filter;

namespace {
constexpr uint64_t kScanUs = 5200;  // the FTM4's ~190 Hz with a finger down

struct Sample {
    int x, y;
    uint32_t z;
};

struct Trial {
    std::vector<std::pair<int, int>> reported;  // after each sample that changed it
    int x = 0, y = 0;
};

Trial feed(const std::vector<Sample> &samples)
{
    stf::Contact c = {};
    c.begin(samples[0].x, samples[0].y, samples[0].z, 1000000);
    Trial trial;
    for (size_t i = 1; i < samples.size(); ++i) {
        if (c.sample(samples[i].x, samples[i].y, samples[i].z, 1000000 + i * kScanUs))
            trial.reported.push_back({c.x.pixel(), c.y.pixel()});
    }
    trial.x = c.x.pixel();
    trial.y = c.y.pixel();
    return trial;
}
}  // namespace

TEST(SwitchTouchFilter, ConstantsAreMillimetresOnThisPanel)
{
    EXPECT_EQ(stf::kPixelsPerMeter, 8258);
    EXPECT_EQ(stf::kHysteresis, 39);  // 0.3 mm: 2.48 px in 1/16 px
    EXPECT_EQ(stf::fromMicrometers(1000) / stf::kQ, 8);
}

TEST(SwitchTouchFilter, InstabilityWhileSettling)
{
    EXPECT_EQ(stf::instability(0, kScanUs, 50, 50), stf::kFull);
    EXPECT_EQ(stf::instability(stf::kSettleUs / 2, kScanUs, 50, 50), stf::kFull / 2);
    EXPECT_EQ(stf::instability(stf::kSettleUs, kScanUs, 50, 50), stf::kFull * 2 / 3 / 5)
        << "the fifth, two thirds left";
    EXPECT_EQ(stf::instability(stf::kSettleTailUs, kScanUs, 50, 50), 0u);
}

TEST(SwitchTouchFilter, InstabilityFromStrengthRate)
{
    const uint64_t settled = stf::kSettleTailUs;
    // 5 % in 5.2 ms is 9.6/s: stable. 10 % is 19.2/s: partly. 20 % is 38/s: all of it.
    EXPECT_EQ(stf::instability(settled, kScanUs, 100, 95), 0u);
    const uint32_t partly = stf::instability(settled, kScanUs, 100, 90);
    EXPECT_GT(partly, 0u);
    EXPECT_LT(partly, stf::kFull);
    EXPECT_EQ(stf::instability(settled, kScanUs, 100, 80), stf::kFull);
    EXPECT_EQ(stf::instability(settled, kScanUs, 50, 100), stf::kFull) << "a lift: strength halving";
    EXPECT_EQ(stf::instability(settled, 0, 50, 100), 0u) << "no time between samples: no rate";
    EXPECT_EQ(stf::instability(settled, kScanUs, 0, 0), 0u);
}

// A still tap as the panel reports one: the centroid slides while the pad presses down (strength rising) and again
// while it lifts (strength falling), here by 12 px each way, which UIKit took as a 9.6 pt drag at 1.25 px per point.
TEST(SwitchTouchFilter, PressAndLiftDriftIsNotMotion)
{
    const Trial trial = feed({{300, 400, 12},
                              {300, 404, 30},
                              {300, 408, 55},
                              {300, 412, 70},
                              {300, 412, 72},
                              {301, 412, 71},
                              {300, 413, 70},
                              {300, 418, 45},
                              {300, 424, 20}});
    EXPECT_TRUE(trial.reported.empty()) << "reported " << trial.reported.size() << " moves, the last to (" << trial.x
                                        << "," << trial.y << ")";
    EXPECT_EQ(trial.x, 300);
    EXPECT_EQ(trial.y, 400);
}

TEST(SwitchTouchFilter, JitterInsideTheHysteresis)
{
    std::vector<Sample> s = {{500, 200, 60}};
    for (int i = 0; i < 40; ++i)
        s.push_back({500 + (i % 3) - 1, 200 + (i * 7) % 5 - 2, 60});
    const Trial trial = feed(s);
    EXPECT_TRUE(trial.reported.empty());
}

// A deliberate drag at steady strength once the contact has settled: every step after the hysteresis is taken is
// reported, nothing jumps, and the point trails the finger by no more than the hysteresis.
TEST(SwitchTouchFilter, SteadyDragFollowsTheFinger)
{
    std::vector<Sample> s;
    for (int i = 0; i < 6; ++i)
        s.push_back({100, 600, 60});
    for (int i = 1; i <= 40; ++i)
        s.push_back({100 + 3 * i, 600, 60});
    const Trial trial = feed(s);
    ASSERT_GE(trial.reported.size(), 39u);
    int last = 100;
    for (const auto &p : trial.reported) {
        EXPECT_LE(std::abs(p.first - last), 4) << "step to " << p.first;
        last = p.first;
    }
    EXPECT_GE(trial.x, 220 - 3);
    EXPECT_LE(trial.x, 220);
}

// A swipe that starts as the finger lands (strength still rising): its first motion is held back, never lost and
// never thrown in one step; the point catches up with the finger as the swipe goes on.
TEST(SwitchTouchFilter, SwipeFromTouchDownCatchesUp)
{
    std::vector<Sample> s = {{50, 300, 15}, {58, 300, 35}, {66, 300, 55}};
    for (int i = 1; i <= 30; ++i)
        s.push_back({66 + 8 * i, 300, 60});
    const Trial trial = feed(s);
    ASSERT_FALSE(trial.reported.empty());
    int last = 50;
    for (const auto &p : trial.reported) {
        EXPECT_GE(p.first, last) << "never backwards";
        EXPECT_LE(p.first - last, 12) << "no jump: at most 1.5 steps of 8 px";
        last = p.first;
    }
    EXPECT_GE(trial.x, 306 - 3) << "caught up to within the hysteresis";
}

// Strength changing while the finger moves (pressing harder mid-drag): the hidden part is paid back as the drag goes on.
TEST(SwitchTouchFilter, PressureChangeMidDragIsRepaid)
{
    std::vector<Sample> s;
    for (int i = 0; i < 6; ++i)
        s.push_back({400, 100, 50});
    for (int i = 1; i <= 30; ++i)
        s.push_back({400, 100 + 4 * i, i == 10 ? 80u : i > 10 ? 80u : 50u});
    const Trial trial = feed(s);
    EXPECT_GE(trial.y, 220 - 3);
    EXPECT_LE(trial.y, 220);
}
