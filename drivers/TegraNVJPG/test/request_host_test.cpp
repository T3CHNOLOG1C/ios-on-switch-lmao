// request_host.c's checks of the AppleJPEGDriver decode request's translation and refusals: it prints "PASS request"
// first and returns 0, or prints each failure and returns 1.
#include <gtest/gtest.h>

#include <string>

extern "C" int request_host_main(void);

TEST(TegraNVJPG, Request)
{
    testing::internal::CaptureStdout();
    const int code = request_host_main();
    const std::string out = testing::internal::GetCapturedStdout();
    EXPECT_EQ(code, 0) << out;
    EXPECT_EQ(out.rfind("PASS request", 0), 0u) << out;
}
