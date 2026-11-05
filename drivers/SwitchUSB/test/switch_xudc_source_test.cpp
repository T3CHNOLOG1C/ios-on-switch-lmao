// Guards on SwitchXUDC.cpp and XudcHardware.cpp for what xudc_transfer_test cannot see (the driver itself is not built
// on the host): the driver lays out and reads its transfers through XudcTransfer.h, the code the test runs; its
// buffers' cache maintenance, TRB chaining and contiguous allocations; the shared bounce pool's pages freed by one
// endpoint re-kick the others (after the event drain and after a cancel), or an OUT read waiting for pages behind a
// busy IN endpoint would never start; and interrupt moderation is on.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using ioswitch::test::Count;
using ioswitch::test::SourceText;
using testing::HasSubstr;
using testing::Not;

TEST(SwitchXudcSource, DriverUsesTheHeader)
{
    const std::string source = SourceText("../SwitchXUDC.cpp");
    for (const char *needle : {
             "#include \"XudcTransfer.h\"",
             "plan(runs, runCount, r.length, ep.mps, out, probe, kReach",
             "outEvent(residue, lengths, r.trbs, pos, e.residue(), shortPacket)",
             "kIOMemoryIncoherentIOStore",
             "kIOMemoryIncoherentIOFlush",
             "TRB_CHAIN",
             "kIOMemoryPhysicallyContiguous",
         })
        EXPECT_THAT(source, HasSubstr(needle));
}

TEST(SwitchXudcSource, FreedPoolPagesKickEveryWaitingEndpoint)
{
    const std::string source = SourceText("../SwitchXUDC.cpp");
    EXPECT_THAT(source, HasSubstr("poolFreed = true;"));
    EXPECT_EQ(Count(source, "    kickWaiting();"), 2u) << "after the event drain and after a cancel";
}

TEST(SwitchXudcSource, InterruptModerationOn)
{
    const std::string hardware = SourceText("../XudcHardware.cpp");
    EXPECT_THAT(hardware, HasSubstr("xw(RT_IMOD, (kImodUs * 4) << 16 | (kImodUs * 4));"));
    EXPECT_THAT(hardware, Not(HasSubstr("xw(RT_IMOD, 0);")));
}
