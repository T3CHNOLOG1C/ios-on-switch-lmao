// Guards on SwitchJoyConRail.cpp's pairing per kernel: the iOS 17 build writes a new pairing over the rail when no slot
// names the console (joyconpaird hands its key to bluetoothd), and the iOS 27 build, which has no such hand-off, never
// writes one, so a docked Joy-Con keeps a pairing the OS can use.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

namespace {

using switch_ios::test::Configured;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

TEST(SwitchJoyConRailPairing, Ios17WritesOverTheRail)
{
    const std::string build = Configured(SourceText("../SwitchJoyConRail.cpp"), "XNU_12377_121_6", false);
    EXPECT_THAT(build, HasSubstr("s.step = kSendHost;"));
}

TEST(SwitchJoyConRailPairing, Ios27NeverWritesAKeyBluetoothdCannotLearn)
{
    const std::string build = Configured(SourceText("../SwitchJoyConRail.cpp"), "XNU_12377_121_6", true);
    EXPECT_THAT(build, Not(HasSubstr("kSendHost;")));
    EXPECT_THAT(build, HasSubstr("finishPairing(s, true, \"flash\");")) << "a slot naming the console is still used";
}

}  // namespace
