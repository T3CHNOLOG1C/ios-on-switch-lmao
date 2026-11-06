// NativeSwitchBlock.cpp moves request bytes only through SwitchClientCopy.h's helper (sd_client_copy_test.cpp
// tests it): readBytes/writeBytes, and the cache flush they add, stay only behind copyFlush (off; an A/B build's switch).
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "bazel/testing/source_text.h"
#include "source_checks.h"

using switch_ios::test::LinesMatching;
using switch_ios::test::SourceText;
using testing::HasSubstr;

TEST(SdClientCopySource, RequestBytesMoveOnlyThroughTheHelper)
{
    const std::string source = SourceText("../NativeSwitchBlock.cpp");
    std::vector<std::string> uses = LinesMatching(source, R"(->\s*(readBytes|writeBytes)\()");
    for (std::string &line : uses)
        line = line.substr(line.find_first_not_of(" \t"));
    ASSERT_EQ(uses.size(), 1u) << testing::PrintToString(uses);
    // The one use sits in stage(), behind copyFlush.
    const std::string before = source.substr(0, source.find(uses[0]));
    const std::string signature = "bool stage(";
    const size_t stage = before.rfind(signature);
    ASSERT_NE(stage, std::string::npos) << "the readBytes/writeBytes use is not inside stage()";
    EXPECT_THAT(before.substr(stage + signature.size()), HasSubstr("copyFlush"));
    EXPECT_THAT(source, HasSubstr("switchClientCopy(memory, at, length, toClient, copypv, from)"));
}
