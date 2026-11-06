// avd_host.c: AVD.videodecoder itself on this Mac's VideoToolbox over a fake TegraNVDEC client.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "run_host.h"

extern "C" int avd_host_main(void);

TEST(SwitchVideoDecoders, AvdPluginOnVideoToolbox)
{
    const HostRun run = RunHost(
        [](int, char **) {
            return avd_host_main();
        },
        {"avd_host"});
    EXPECT_EQ(run.code, 0) << run.out;
    EXPECT_THAT(run.out, testing::HasSubstr("PASS avd"));
}
