// <switch/cpulimit.h>: a blocking per-thread CPU limit's share scaled to this CPU, and its hook in both trees'
// thread_set_cpulimit ahead of the branch that removes a limit.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>

#include <switch/cpulimit.h>

using testing::HasSubstr;

namespace {

std::string Text(const char *path)
{
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

TEST(SwitchCpuLimit, SmallSharesScaleAndKeepTheirLimit)
{
    for (const auto [asked, share] : {std::pair{1, 3}, {10, 25}, {20, 50}, {39, 98}}) {
        uint8_t percentage = static_cast<uint8_t>(asked);
        EXPECT_FALSE(switch_cpulimit_lift(&percentage)) << asked;
        EXPECT_EQ(percentage, share) << asked;
    }
}

TEST(SwitchCpuLimit, AWholeCoreOrMoreIsLiftedUntouched)
{
    for (const int asked : {40, 50, 70, 75, 80, 99, 100}) {
        uint8_t percentage = static_cast<uint8_t>(asked);
        EXPECT_TRUE(switch_cpulimit_lift(&percentage)) << asked;
        EXPECT_EQ(percentage, asked);
    }
}

TEST(SwitchCpuLimit, BothTreesLiftBeforeRemovingALimit)
{
    for (const char *path : {THREAD_10063, THREAD_12377}) {
        const std::string s = Text(path);
        const size_t fn = s.find("\nthread_set_cpulimit(int action, uint8_t percentage, uint64_t interval_ns)\n");
        ASSERT_NE(fn, std::string::npos) << path;
        const std::string body = s.substr(fn, s.find("\n}\n", fn) - fn);
        const size_t lift = body.find("if (action == THREAD_CPULIMIT_BLOCK && "
                                      "switch_sched_cpulimit_lift(&percentage, interval_ns)) {\n"
                                      "\t\taction = THREAD_CPULIMIT_DISABLE;");
        ASSERT_NE(lift, std::string::npos) << path;
        EXPECT_LT(body.find("TH_OPT_FORCED_LEDGER"), lift) << path;
        EXPECT_LT(lift, body.find("if (action == THREAD_CPULIMIT_DISABLE) {")) << path;
        EXPECT_THAT(s, HasSubstr("#include <switch/sched.h>")) << path;
    }
}

}  // namespace
