// kits/SwitchMath against straightforward references, including the edges the hand-written copies got wrong.
#include "kits/SwitchMath/SwitchMath.h"

#include <gtest/gtest.h>

#include <random>

namespace {
TEST(SwitchMath, PowersAndAlignment)
{
    EXPECT_FALSE(switch_is_pow2(0));
    EXPECT_TRUE(switch_is_pow2(1));
    EXPECT_TRUE(switch_is_pow2(1ull << 63));
    EXPECT_FALSE(switch_is_pow2(6));
    EXPECT_EQ(switch_align_up(0x1001, 0x1000), 0x2000u);
    EXPECT_EQ(switch_align_up(0x1000, 0x1000), 0x1000u);
    EXPECT_EQ(switch_align_up(0x1'0000'0001ull, 0x1000), 0x1'0000'1000ull);
    EXPECT_EQ(switch_align_down(0x1fff, 0x1000), 0x1000u);
    EXPECT_TRUE(switch_is_aligned(0x4000, 0x1000));
    EXPECT_FALSE(switch_is_aligned(0x4040, 0x1000));
    EXPECT_EQ(switch_bytes_to_boundary(0x80003fc0, 0x1000), 0x40u);
    EXPECT_EQ(switch_bytes_to_boundary(0x80004000, 0x1000), 0x1000u);
}

TEST(SwitchMath, RoundingDivision)
{
    EXPECT_EQ(switch_div_round_up(0, 4), 0u);
    EXPECT_EQ(switch_div_round_up(9, 4), 3u);
    EXPECT_EQ(switch_div_round_up(UINT64_MAX, 2), (UINT64_MAX >> 1) + 1);
    std::mt19937_64 random(3);
    for (int i = 0; i < 100000; ++i) {
        const int64_t value = static_cast<int64_t>(random() % 2000001) - 1000000;
        int64_t divisor = static_cast<int64_t>(random() % 2001) - 1000;
        if (!divisor)
            divisor = 7;
        const double exact = static_cast<double>(value) / static_cast<double>(divisor);
        const int64_t want = static_cast<int64_t>(exact < 0 ? exact - 0.5 : exact + 0.5);
        ASSERT_EQ(switch_div_round_closest(value, divisor), want) << value << " / " << divisor;
    }
}

TEST(SwitchMath, ClampAndSaturate)
{
    EXPECT_EQ(switch_clamp_u64(5, 10, 20), 10u);
    EXPECT_EQ(switch_clamp_u64(25, 10, 20), 20u);
    EXPECT_EQ(switch_clamp_i64(-5, -3, 3), -3);
    EXPECT_EQ(switch_sat_i8(-200), INT8_MIN);
    EXPECT_EQ(switch_sat_i8(42), 42);
    EXPECT_EQ(switch_sat_i16(70000), INT16_MAX);
    EXPECT_EQ(switch_sat_i32(INT64_MIN), INT32_MIN);
}

TEST(SwitchMath, MinMaxEvaluateOnce)
{
    int calls = 0;
    auto next = [&] {
        return ++calls;
    };
    EXPECT_EQ(SWITCH_MIN(next(), 10), 1);
    EXPECT_EQ(SWITCH_MAX(next(), 0), 2);
    EXPECT_EQ(calls, 2);
    EXPECT_EQ(SWITCH_MIN(3u, 7u), 3u);
}

TEST(SwitchMath, Genmask)
{
    static_assert(SWITCH_GENMASK(7, 4) == 0xf0ull);
    static_assert(SWITCH_GENMASK(63, 0) == ~0ull);
    static_assert(SWITCH_GENMASK(31, 31) == 0x80000000ull);
}
}  // namespace
