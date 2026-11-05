// vp9_host.c: VP9NVDEC against libvpx's own trace of the two clips.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "run_host.h"

extern "C" int vp9_host_main(int argc, char **argv);

TEST(SwitchVideoDecoders, Vp9AgainstLibvpx)
{
    const HostRun run = RunHost(vp9_host_main, {"vp9_host"});
    EXPECT_EQ(run.code, 0) << run.out;
    EXPECT_THAT(run.out, testing::HasSubstr("PASS vp9: clip A 18 frames (16 shown), clip B 13 frames"));
}
