// xnu_console_wide_harness.c: video_console.c's wide helpers against the 32-bit loops they replaced. It prints "PASS
// 201 scroll/clear cases, 20000 glyph columns", or the first failed check, and exits 1.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <string>

extern "C" int xnu_console_wide_main(void);

TEST(XnuConsoleWide, WidePathsMatchThe32BitLoops)
{
    testing::internal::CaptureStdout();
    const int code = xnu_console_wide_main();
    const std::string out = testing::internal::GetCapturedStdout();
    std::fputs(out.c_str(), stdout);
    EXPECT_EQ(code, 0);
    EXPECT_THAT(out, testing::HasSubstr("PASS 201 scroll/clear cases, 20000 glyph columns"));
}
