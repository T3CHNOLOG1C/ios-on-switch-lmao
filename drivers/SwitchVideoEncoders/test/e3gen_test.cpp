// The kernel's E3 vectors (drivers/TegraNVENC/selftest/E3Vectors.inc) are exactly what e3gen prints today from the
// plug-in's H264NVENC.c and AVD's H264NVDEC.c; when this fails, regenerate the file with e3gen's output
// (bazel-bin/.../SwitchVideoEncoders/test/e3gen_test --gtest_filter=*Print* prints it).
#include <gtest/gtest.h>

#include <cstdio>
#include <string>

#include "bazel/testing/source_text.h"

extern "C" int e3gen_main(void);

namespace {
std::string generate(int *code)
{
    testing::internal::CaptureStdout();
    *code = e3gen_main();
    return testing::internal::GetCapturedStdout();
}
}  // namespace

TEST(SwitchVideoEncoders, E3VectorsAreCurrent)
{
    int code = -1;
    const std::string out = generate(&code);
    EXPECT_EQ(code, 0);
    EXPECT_TRUE(out == ioswitch::test::SourceText("../../TegraNVENC/selftest/E3Vectors.inc"))
        << "regenerate TegraNVENC/selftest/E3Vectors.inc with e3gen (run e3gen_test with --gtest_filter=*Print* "
           "--gtest_also_run_disabled_tests)";
}

// Prints e3gen's output, for regenerating the file.
TEST(SwitchVideoEncoders, DISABLED_PrintE3Vectors)
{
    int code = -1;
    const std::string out = generate(&code);
    std::fwrite(out.data(), 1, out.size(), stdout);
    EXPECT_EQ(code, 0);
}
