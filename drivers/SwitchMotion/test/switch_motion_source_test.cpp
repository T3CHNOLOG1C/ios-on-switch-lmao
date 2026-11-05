// Guards for what switch_motion_test's models cannot see: the sample and FIFO drain paths never wait or poll (the
// models' clock does not run while code spins), the motion services' HID descriptor (SwitchHIDDevice.cpp, a stand-in in
// the scenarios) carries the Sensor-page usages 21H461's IOHIDEventDriver knows, and SwitchBacklight follows the
// panel's power into the orientation detector.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using ioswitch::test::Between;
using ioswitch::test::Body;
using ioswitch::test::Count;
using ioswitch::test::SourceText;
using testing::HasSubstr;
using testing::Not;

TEST(SwitchMotionSource, SamplePathsNeverWait)
{
    const std::string source = SourceText("../SwitchMotion.cpp");
    for (const char *signature : {"void SwitchMotion::sample(", "void SwitchMotion::drainFifo("}) {
        const std::string body = Body(source, signature);
        ASSERT_FALSE(body.empty()) << signature;
        for (const char *wait : {"IOSleep", "IODelay", "while (", "for (;;)"})
            EXPECT_THAT(body, Not(HasSubstr(wait))) << signature << " " << wait;
    }
}

// Report Interval 0x30e and Report Latency 0x31b; 0x316 is Reporting State, which no element takes (BatchInterval never
// reached the driver and FIFO batching never ran before r17).
TEST(SwitchMotionSource, SensorPropertyUsages)
{
    const std::string motion = Between(SourceText("../../SwitchTouch/SwitchHIDDevice.cpp"), "#define MOTION_DESCRIPTOR",
                                       "#undef MOTION_DESCRIPTOR");
    EXPECT_THAT(motion, HasSubstr("0x0a, 0x0e, 0x03,"));
    EXPECT_THAT(motion, HasSubstr("0x0a, 0x1b, 0x03,"));
    EXPECT_THAT(motion, Not(HasSubstr("0x0a, 0x16, 0x03")));
}

TEST(SwitchMotionSource, BacklightFollowsPanelPower)
{
    // The weak default, off and on.
    EXPECT_EQ(Count(SourceText("../../SwitchBrightness/SwitchBacklight.cpp"), "switch_motion_display_power("), 3u);
}
