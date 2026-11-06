// Host test of drivers/SwitchTouch/SwitchTouchTrace.h (debug kernels' per-contact samples) over switch_touch_stubs.h.
#include <string>
#include <gtest/gtest.h>
#include "switch_touch_stubs.h"
#include "../SwitchTouchTrace.h"

namespace {
std::string all()
{
    std::string text;
    for (const std::string &line : gLog)
        text += line + "\n";
    return text;
}
}  // namespace

TEST(SwitchTouchTrace, ShortContactInFull)
{
    gLog.clear();
    SwitchTouchTrace trace = {};
    trace.start(40);
    for (unsigned i = 1; i < 20; ++i)
        trace.add(i * 5, 0, (int)i, 40 + i);
    trace.log(2, 300, 600, 100, 0, 3);
    ASSERT_EQ(gLog.size(), 2u) << all();
    EXPECT_NE(gLog[0].find("contact 2 at (300,600) 100 ms, 20 samples (0 not kept), lifted at +0,+3; 1/2 0:0,0,40 "
                           "5:0,1,41"),
              std::string::npos)
        << all();
    const std::string last = "2/2 80:0,16,56 85:0,17,57 90:0,18,58 95:0,19,59";
    EXPECT_EQ(gLog[1].substr(gLog[1].size() - last.size()), last) << all();
}

TEST(SwitchTouchTrace, LongContactKeepsItsStartAndItsEnd)
{
    gLog.clear();
    SwitchTouchTrace trace = {};
    trace.start(10);
    for (unsigned i = 1; i < 200; ++i)
        trace.add(i, (int)i, 0, 50);
    trace.log(0, 10, 20, 199, 190, 0);
    const std::string text = all();
    ASSERT_EQ(gLog.size(), 4u) << text;
    EXPECT_NE(text.find("200 samples (136 not kept)"), std::string::npos) << text;
    EXPECT_NE(text.find(" 23:23,0,50 160:160,0,50 "), std::string::npos) << "head, then the tail" << text;
    EXPECT_NE(text.find(" 199:199,0,50\n"), std::string::npos) << text;
    EXPECT_EQ(text.find(" 24:24,0,50 "), std::string::npos) << text;
}
