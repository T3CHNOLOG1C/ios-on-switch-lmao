// drivers/SwitchJoyCon/SwitchJoyConForward.h: which rail 0x30 reports reach the HID device, per input report mode.
#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "../SwitchJoyConForward.h"

namespace {

using Report = std::array<uint8_t, 49>;

void putStick(Report &r, unsigned at, unsigned x, unsigned y)
{
    r[at] = static_cast<uint8_t>(x & 0xff);
    r[at + 1] = static_cast<uint8_t>((x >> 8) | ((y & 0x0f) << 4));
    r[at + 2] = static_cast<uint8_t>(y >> 4);
}

Report resting(uint8_t timer = 0)
{
    Report r{};
    r[0] = 0x30;
    r[1] = timer;
    r[2] = 0x8e;
    putStick(r, 6, 2048, 2048);
    putStick(r, 9, 2048, 2048);
    for (unsigned i = 13; i < r.size(); ++i)
        r[i] = static_cast<uint8_t>(i * 7 + timer);
    return r;
}

void sendMode(switch_joycon_forward &f, uint8_t subcommand, uint8_t argument)
{
    uint8_t out[49] = {0x01, 0x00};
    out[10] = subcommand;
    out[11] = argument;
    switch_joycon_forward_output(&f, out, sizeof(out));
}

struct Simple : ::testing::Test {
    switch_joycon_forward f{};

    void SetUp() override
    {
        switch_joycon_forward_reset(&f);
        sendMode(f, 0x03, 0x3f);
    }

    bool take(const Report &r, uint64_t ms)
    {
        return switch_joycon_forward_report(&f, r.data(), r.size(), ms);
    }
};

TEST(SwitchJoyConForward, FullModeForwardsEveryReport)
{
    switch_joycon_forward f{};
    switch_joycon_forward_reset(&f);
    const Report r = resting();
    for (uint64_t ms = 0; ms < 150; ms += 15)
        EXPECT_TRUE(switch_joycon_forward_report(&f, r.data(), r.size(), ms));
}

TEST_F(Simple, RestingJoyConForwardsOnceAndThenOnlyTheKeepAlive)
{
    unsigned forwarded = 0;
    for (uint64_t ms = 0; ms < 3000; ms += 15)
        forwarded += take(resting(static_cast<uint8_t>(ms / 15)), ms);
    EXPECT_EQ(forwarded, 3u);
}

TEST_F(Simple, ButtonEdgesAlwaysForward)
{
    EXPECT_TRUE(take(resting(), 0));
    Report pressed = resting(1);
    pressed[3] = 0x08;
    EXPECT_TRUE(take(pressed, 15));
    EXPECT_FALSE(take(pressed, 30));
    EXPECT_TRUE(take(resting(3), 45));
    Report left = resting(4);
    left[5] = 0x01;
    EXPECT_TRUE(take(left, 60));
}

TEST_F(Simple, StickNoiseIsHeldBackAndMovementForwards)
{
    EXPECT_TRUE(take(resting(), 0));
    Report jitter = resting(1);
    putStick(jitter, 6, 2048 + SWITCH_JOYCON_FORWARD_STICK_NOISE, 2048 - SWITCH_JOYCON_FORWARD_STICK_NOISE);
    putStick(jitter, 9, 2047, 2049);
    EXPECT_FALSE(take(jitter, 15));
    Report moved = resting(2);
    putStick(moved, 9, 2048, 2048 + SWITCH_JOYCON_FORWARD_STICK_NOISE + 1);
    EXPECT_TRUE(take(moved, 30));
    Report further = moved;
    putStick(further, 9, 2048, 2048 + 2 * SWITCH_JOYCON_FORWARD_STICK_NOISE + 1);
    EXPECT_FALSE(take(further, 45));
    putStick(further, 9, 2048, 2048 + 2 * SWITCH_JOYCON_FORWARD_STICK_NOISE + 2);
    EXPECT_TRUE(take(further, 60));
}

TEST_F(Simple, BatteryChangesForwardButConnectionBitsDoNot)
{
    EXPECT_TRUE(take(resting(), 0));
    Report connection = resting(1);
    connection[2] = 0x8f;
    EXPECT_FALSE(take(connection, 15));
    Report charging = resting(2);
    charging[2] = 0x9e;
    EXPECT_TRUE(take(charging, 30));
}

TEST_F(Simple, ImuOnOrFullModeForwardsEveryReportAgain)
{
    EXPECT_TRUE(take(resting(), 0));
    EXPECT_FALSE(take(resting(1), 15));
    sendMode(f, 0x40, 0x01);
    EXPECT_TRUE(take(resting(2), 30));
    EXPECT_TRUE(take(resting(3), 45));
    sendMode(f, 0x40, 0x00);
    EXPECT_FALSE(take(resting(4), 60));
    sendMode(f, 0x03, 0x30);
    EXPECT_TRUE(take(resting(5), 75));
}

TEST_F(Simple, OtherReportsAlwaysForward)
{
    EXPECT_TRUE(take(resting(), 0));
    Report reply = resting(1);
    reply[0] = 0x21;
    EXPECT_TRUE(take(reply, 15));
    EXPECT_TRUE(take(reply, 30));
}

TEST_F(Simple, ANewDeviceStartsInFullModeAndForwardsItsFirstReport)
{
    EXPECT_TRUE(take(resting(), 0));
    EXPECT_FALSE(take(resting(1), 15));
    switch_joycon_forward_reset(&f);
    EXPECT_TRUE(take(resting(2), 30));
    sendMode(f, 0x03, 0x3f);
    EXPECT_TRUE(take(resting(3), 45));
    EXPECT_FALSE(take(resting(4), 60));
}

}  // namespace
