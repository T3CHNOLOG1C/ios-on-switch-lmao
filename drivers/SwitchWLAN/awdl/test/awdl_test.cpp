// test_awdl.c and test_native.c: each failed check prints "FAIL file:line: ..." to stderr; main returns 1 if any did,
// else prints the summary line ending in ": ok".
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <string>

extern "C" int awdl_tests_main(int argc, char **argv);

TEST(Awdl, CoreApple80211AndFirmware)
{
    char name[] = "test_awdl";
    char *argv[] = {name, nullptr};
    testing::internal::CaptureStdout();
    const int code = awdl_tests_main(1, argv);
    const std::string out = testing::internal::GetCapturedStdout();
    std::fputs(out.c_str(), stdout);
    EXPECT_EQ(code, 0);
    EXPECT_THAT(out, testing::HasSubstr("creation failures: ok"));
}
