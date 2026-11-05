// vic_host.c's checks (layout, colour, ideal reference, jobs, requests, block linear): it prints "PASS vic (N checks)"
// and returns 0, or prints each failed case ("FAIL file:line: ...") and returns 1.
#include <gtest/gtest.h>

#include <string>

extern "C" int vic_host_main(void);

TEST(TegraVIC, HostChecks)
{
    testing::internal::CaptureStdout();
    const int code = vic_host_main();
    const std::string out = testing::internal::GetCapturedStdout();
    EXPECT_EQ(code, 0) << out;
    EXPECT_EQ(out.rfind("PASS vic", 0), 0u) << out;
}
