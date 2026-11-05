// ../vdectest.c: the clip check against Apple's own decoders. Only the software runs are required: a Mac (or sandbox)
// without a reachable hardware decoder fails the others, so its exit status is not checked.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "run_host.h"

extern "C" int vdectest_main(void);

TEST(SwitchVideoDecoders, ClipCheckOnAppleDecoders)
{
    const HostRun run = RunHost([](int, char **) { return vdectest_main(); }, {"vdectest"});
    EXPECT_THAT(run.out, testing::HasSubstr("software: software decoder, 30/30 frames match x264"));
    EXPECT_THAT(run.out,
                testing::HasSubstr("hevc-software: software decoder, frames matching FFmpeg: A 20/20, V 14/14"));
}
