// osfmk/ipc/ioswitch_render_senders.h on the host: what the display's clients line says for a window of messages.
#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "ioswitch_render_senders.h"

namespace {

std::string line(const ioswitch_render_senders &t, uint64_t ms, unsigned top, size_t size = 512)
{
    std::string out(size, '\0');
    const size_t n = ioswitch_render_senders_format(&t, ms, top, out.data(), size);
    EXPECT_EQ(n, strlen(out.c_str()));
    EXPECT_LT(n, size);
    out.resize(n);
    return out;
}

void send(ioswitch_render_senders &t, int pid, int id, const char *name, unsigned n)
{
    for (unsigned i = 0; i < n; i++) ioswitch_render_senders_note(&t, pid, id, name);
}

// A home-screen window: the clock widget at 20 Hz, SpringBoard's display links at 10 and 15 Hz under one id
// and a few of another, a client asking once
TEST(RenderSenders, LargestFirstWithRatesAndIds)
{
    ioswitch_render_senders t = {};
    send(t, 61, 40002, "WidgetRenderer_D", 100);
    send(t, 31, 40002, "SpringBoard", 50);
    send(t, 31, 40017, "SpringBoard", 2);
    send(t, 31, 40020, "SpringBoard", 1);
    send(t, 88, 40002, "Maps", 24);
    EXPECT_EQ(line(t, 5000, 6),
              "WidgetRenderer_D[61] 20.0/s (#40002 20.0), SpringBoard[31] 10.6/s (#40002 10.0, #40017 0.4), "
              "Maps[88] 4.8/s (#40002 4.8)");
}

TEST(RenderSenders, TopThenOthers)
{
    ioswitch_render_senders t = {};
    for (int pid = 1; pid <= 8; pid++) send(t, pid, 7, "p", unsigned(pid * 10));
    const std::string s = line(t, 5000, 2);
    EXPECT_EQ(s, "p[8] 16.0/s (#7 16.0), p[7] 14.0/s (#7 14.0), others 42.0/s");
}

TEST(RenderSenders, FullTableCountsTheRestAsOthers)
{
    ioswitch_render_senders t = {};
    for (int id = 0; id < IOSWITCH_RENDER_SENDERS + 5; id++) send(t, 9, id, "busy", 1);
    EXPECT_EQ(t.used, uint32_t(IOSWITCH_RENDER_SENDERS));
    EXPECT_EQ(t.dropped, 5u);
    EXPECT_EQ(line(t, 1000, 6), "busy[9] 32.0/s (#0 1.0, #1 1.0), others 5.0/s");
}

TEST(RenderSenders, NamesAndBounds)
{
    ioswitch_render_senders t = {};
    EXPECT_EQ(line(t, 5000, 6), "");
    send(t, 3, 1, "a-process-name-longer-than-sixteen", 1);
    EXPECT_STREQ(t.entry[0].name, "a-process-name-l");
    send(t, 4, 1, nullptr, 1);
    EXPECT_STREQ(t.entry[1].name, "");
    // a line cut at any size is still terminated and never longer
    for (size_t size = 1; size < 48; size++) line(t, 5000, 6, size);
    EXPECT_EQ(line(t, 0, 6),
              "a-process-name-l[3] 1000.0/s (#1 1000.0), [4] 1000.0/s (#1 1000.0)");  // a 0 ms window counts as 1 ms
}

}  // namespace
