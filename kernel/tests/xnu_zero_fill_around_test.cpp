// xnu_zero_fill_around_harness.c: vm_fault.c's switch_zero_fill_around over the model of one VM object and the pmap.
// It prints "PASS 18 zero-fill fault-around cases", or the failed case, and returns 1 (the model's own invariants exit
// 1 with their line).
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <string>

extern "C" int xnu_zero_fill_around_main(void);

TEST(XnuZeroFillAround, GroupBoundsAndPlainCase)
{
    testing::internal::CaptureStdout();
    const int code = xnu_zero_fill_around_main();
    const std::string out = testing::internal::GetCapturedStdout();
    std::fputs(out.c_str(), stdout);
    EXPECT_EQ(code, 0);
    EXPECT_THAT(out, testing::HasSubstr("PASS 18 zero-fill fault-around cases"));
}
