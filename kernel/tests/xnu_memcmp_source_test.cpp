// The harness tests what XNU runs: subrs.c (patched) takes switch_memcmp.h on arm64 only, in two #if
// defined(__arm64__) blocks the patch adds, with exactly the two bodies xnu_memcmp_harness.c copies.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Count;
using switch_ios::test::SourceText;
using testing::HasSubstr;

namespace {
// A file of the test's data by its runfiles path (an external repository's: ../<repo>/<path>).
std::string ReadRunfile(const char *path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) ADD_FAILURE() << "cannot read " << path;
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

constexpr const char *kBodies[] = {
    "len -= switch_cmp_first_difference((const unsigned char *)pa, (const unsigned char *)pb, len);",
    "const size_t i = switch_cmp_first_difference(p1, p2, n);",
    "return i < n ? p1[i] - p2[i] : 0;",
};
}  // namespace

TEST(XnuMemcmpSource, SubrsUsesItOnArm64Only)
{
    const std::string patched = ReadRunfile(SUBRS_PATCHED), base = ReadRunfile(SUBRS_BASE);
    EXPECT_THAT(patched, HasSubstr("#include <arm64/switch_memcmp.h>"));
    for (const char *line : kBodies) EXPECT_THAT(patched, HasSubstr(line));
    // The patch adds exactly two arm64-only blocks: other architectures keep the byte loops.
    EXPECT_EQ(Count(patched, "#if defined(__arm64__)\n") - Count(base, "#if defined(__arm64__)\n"), 2u);
}

TEST(XnuMemcmpSource, HarnessCopiesSubrsBodies)
{
    const std::string harness = SourceText("xnu_memcmp_harness.c");
    for (const char *line : kBodies) EXPECT_THAT(harness, HasSubstr(line));
}
