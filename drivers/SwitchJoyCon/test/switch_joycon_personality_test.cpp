// Guards on SwitchJoyConHID.cpp's game-controller personalities, per kernel: the iOS 17 build (xnu-10063) carries
// AppleGameControllerPersonality's 21H461 Nintendo entries itself (IOHIDEventDummyService on the interface,
// HIDServiceSupport on the device), and the iOS 27 build (xnu-12377) adds none, so 24B119's own AppleGCHIDUserEventDriver
// (score 1000) claims the IOHIDInterface and JoyConHIDServicePlugin finds GameControllerSupport on it. A dummy service
// at score 10000 there would take the interface and leave the Joy-Cons without a GCController.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <vector>

#include "bazel/testing/source_text.h"

namespace {

using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

// The lines the compiler sees with XNU_12377_121_6 set or not; other conditionals are kept whole.
std::string Preprocessed(const std::string &source, bool xnu12377)
{
    enum Branch {
        kOther,
        kTaken,
        kSkipped
    };

    std::vector<Branch> open;
    std::istringstream lines(source);
    std::string out;
    const auto skipping = [&] {
        for (const Branch branch : open)
            if (branch == kSkipped)
                return true;
        return false;
    };
    for (std::string line; std::getline(lines, line);) {
        if (line.rfind("#if !XNU_12377_121_6", 0) == 0) {
            open.push_back(xnu12377 ? kSkipped : kTaken);
            continue;
        }
        if (line.rfind("#if XNU_12377_121_6", 0) == 0) {
            open.push_back(xnu12377 ? kTaken : kSkipped);
            continue;
        }
        if (line.rfind("#if", 0) == 0) {
            open.push_back(kOther);
        } else if (line.rfind("#else", 0) == 0 && !open.empty() && open.back() != kOther) {
            open.back() = open.back() == kTaken ? kSkipped : kTaken;
            continue;
        } else if (line.rfind("#endif", 0) == 0 && !open.empty()) {
            const bool ours = open.back() != kOther;
            open.pop_back();
            if (ours)
                continue;
        }
        if (!skipping())
            out += line + "\n";
    }
    EXPECT_TRUE(open.empty()) << "unbalanced conditionals";
    return out;
}

TEST(SwitchJoyConPersonalities, Ios17CarriesTheNintendoEntries)
{
    const std::string build = Preprocessed(SourceText("../SwitchJoyConHID.cpp"), false);
    EXPECT_THAT(build, HasSubstr("        addNintendoPersonalities();"));
    EXPECT_THAT(build, HasSubstr("OSString::withCString(\"IOHIDEventDummyService\")"));
    EXPECT_THAT(build, HasSubstr("setProperty(\"HIDServiceSupport\", true);"));
    EXPECT_THAT(build, HasSubstr("        addButtonsPersonality();"));
}

TEST(SwitchJoyConPersonalities, Ios27LeavesTheInterfaceToApplesDriver)
{
    const std::string build = Preprocessed(SourceText("../SwitchJoyConHID.cpp"), true);
    EXPECT_THAT(build, Not(HasSubstr("addNintendoPersonalities")));
    EXPECT_THAT(build, Not(HasSubstr("OSString::withCString(\"IOHIDEventDummyService\")")));
    EXPECT_THAT(build, Not(HasSubstr("IOHIDProviderPropertyMerger\")")));
    EXPECT_THAT(build, Not(HasSubstr("setProperty(\"HIDServiceSupport\"")));
    EXPECT_THAT(build, HasSubstr("        addButtonsPersonality();")) << "Home and Capture stay the console's buttons";
}

}  // namespace
