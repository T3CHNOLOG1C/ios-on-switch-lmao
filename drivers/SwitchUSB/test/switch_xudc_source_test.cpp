// Guards on SwitchXUDC.cpp and XudcHardware.cpp for what xudc_transfer_test cannot see (the driver itself is not built
// on the host): the driver lays out and reads its transfers through XudcTransfer.h, the code the test runs; its
// buffers' cache maintenance, TRB chaining and contiguous allocations; the shared bounce pool's pages freed by one
// endpoint re-kick the others (after the event drain and after a cancel), or an OUT read waiting for pages behind a
// busy IN endpoint would never start; interrupt moderation is on; and the event ring holds what the rings can owe.
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

TEST(SwitchXudcSource, EventRingHoldsWhatTheRingsOwe)
{
    const std::string hardware = SourceText("../XudcHardware.cpp");
    EXPECT_THAT(hardware, HasSubstr("constexpr uint32_t kEventBytes = 2 * kEventSegmentTrbs * 16;"));
    EXPECT_THAT(hardware, HasSubstr("xw(ERSTSZ, kEventSegmentTrbs << 16 | kEventSegmentTrbs);"));
    EXPECT_THAT(hardware, HasSubstr("if (++eventIndex == 2 * kEventSegmentTrbs) {"));
    const std::string source = SourceText("../SwitchXUDC.cpp");
    EXPECT_THAT(source, HasSubstr("handled < 2 * kEventSegmentTrbs && hw.nextEvent(e)")) << "drained to empty";
    EXPECT_THAT(source, HasSubstr("panic(\"SwitchUSB: XUDC event ring full, events were lost"));
}
