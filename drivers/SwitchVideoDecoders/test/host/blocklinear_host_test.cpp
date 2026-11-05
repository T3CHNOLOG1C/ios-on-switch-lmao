// blocklinear_host.c: BlockLinear.c's de-swizzle against the per-byte GOB formula; prints "OK" or "FAIL".
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "run_host.h"

extern "C" int blocklinear_host_main(void);

TEST(SwitchVideoDecoders, BlockLinearCopy)
{
    const HostRun run = RunHost([](int, char **) { return blocklinear_host_main(); }, {"blocklinear_host"});
    EXPECT_EQ(run.code, 0) << run.out;
    EXPECT_THAT(run.out, testing::HasSubstr("OK"));
}
