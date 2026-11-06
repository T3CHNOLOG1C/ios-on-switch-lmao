// kernel/switch/diag/latency.h on the host: a touch report followed through its delivery, the app's commit, the render
// server's vsync and swap to the scan-out, and the latency line.
#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "diag/latency.h"

namespace {

std::string line(const switch_latency &l, size_t size = 1024)
{
    switch_latency_summary m = {};
    switch_latency_summarize(&l, &m);
    std::string out(size, '\0');
    const size_t n = switch_latency_format(&m, out.data(), size);
    EXPECT_EQ(n, strlen(out.c_str()));
    out.resize(n);
    return out;
}

// One touch through every stage: report at t, delivered 1 ms later, committed 4 ms after that, the next vsync 6 ms
// later, the swap 3 ms after it, scanned out 13 ms after that (total 27 ms).
void touch(switch_latency &l, uint64_t t, unsigned changes, int pid = 27)
{
    switch_latency_report(&l, changes);
    switch_latency_delivered(&l, t, pid, t + 1000);
    switch_latency_message(&l, pid, SWITCH_LATENCY_COMMIT_ID, t + 5000);
    switch_latency_present(&l, t + 11000, t + 14000, t + 27000);
}

}  // namespace

TEST(Latency, EveryStageOfOneTouch)
{
    switch_latency l = {};
    touch(l, 1000000, SWITCH_LATENCY_BEGAN_BIT);
    EXPECT_EQ(l.stage, SWITCH_LATENCY_IDLE);
    EXPECT_EQ(line(l), "began 1 (dropped 0): total 27.0/27.0/27.0 = deliver 1.0/1.0/1.0 + app 4.0/4.0/4.0 + "
                       "wait 6.0/6.0/6.0 + compose 3.0/3.0/3.0 + show 13.0/13.0/13.0 ms p50/p95/max");
}

TEST(Latency, KindsFollowTheReportsSinceTheLastDelivery)
{
    switch_latency l = {};
    touch(l, 1000000, SWITCH_LATENCY_BEGAN_BIT | SWITCH_LATENCY_ENDED_BIT);
    touch(l, 2000000, SWITCH_LATENCY_ENDED_BIT);
    touch(l, 3000000, 0);
    touch(l, 4000000, 0);
    EXPECT_EQ(l.hist[SWITCH_LATENCY_BEGAN][SWITCH_LATENCY_TOTAL].n, 1u);
    EXPECT_EQ(l.hist[SWITCH_LATENCY_ENDED][SWITCH_LATENCY_TOTAL].n, 1u);
    EXPECT_EQ(l.hist[SWITCH_LATENCY_MOVED][SWITCH_LATENCY_TOTAL].n, 2u);
}

TEST(Latency, OnlyTheAppsCommitAndAVsyncAfterItAnswer)
{
    switch_latency l = {};
    switch_latency_report(&l, 0);
    switch_latency_delivered(&l, 1000, 27, 2000);
    switch_latency_also_delivered(&l, 300);                          // the app under the finger, after SpringBoard
    switch_latency_also_delivered(&l, 400);                          // a third receiver: not the app
    switch_latency_message(&l, 99, SWITCH_LATENCY_COMMIT_ID, 3000);  // another client's commit
    switch_latency_message(&l, 27, SWITCH_LATENCY_COMMIT_ID, 3200);  // SpringBoard's, not the app's
    switch_latency_message(&l, 300, 70010, 3500);                    // not a commit
    EXPECT_EQ(l.stage, SWITCH_LATENCY_DELIVERED);
    switch_latency_message(&l, 300, SWITCH_LATENCY_COMMIT_ID, 4000);
    EXPECT_EQ(l.stage, SWITCH_LATENCY_COMMITTED);
    switch_latency_present(&l, 3500, 6000, 20000);  // drawn for a vsync before the commit
    EXPECT_EQ(l.stage, SWITCH_LATENCY_COMMITTED);
    switch_latency_present(&l, 20500, 23000, 37000);
    EXPECT_EQ(l.stage, SWITCH_LATENCY_IDLE);
    const switch_latency_hist &total = l.hist[SWITCH_LATENCY_MOVED][SWITCH_LATENCY_TOTAL];
    EXPECT_EQ(total.n, 1u);
    EXPECT_EQ(total.max_us, 36000u);
    EXPECT_EQ(l.hist[SWITCH_LATENCY_MOVED][SWITCH_LATENCY_WAIT].max_us, 16500u);
}

TEST(Latency, ASoleReceiversCommitAnswers)
{
    switch_latency l = {};
    switch_latency_report(&l, 0);
    switch_latency_delivered(&l, 1000, 27, 2000);
    switch_latency_also_delivered(&l, 27);  // the same process again: still the sole receiver
    switch_latency_message(&l, 27, SWITCH_LATENCY_COMMIT_ID, 3000);
    EXPECT_EQ(l.stage, SWITCH_LATENCY_COMMITTED);
}

TEST(Latency, OneSampleInFlightAndStaleOnesDropped)
{
    switch_latency l = {};
    switch_latency_report(&l, SWITCH_LATENCY_BEGAN_BIT);
    switch_latency_delivered(&l, 1000, 27, 2000);
    switch_latency_report(&l, 0);
    switch_latency_delivered(&l, 7000, 27, 8000);  // in flight: not a new sample
    EXPECT_EQ(l.report_us, 1000u);
    EXPECT_EQ(l.kind, SWITCH_LATENCY_BEGAN);
    switch_latency_report(&l, 0);
    switch_latency_delivered(&l, 300000, 27, 301000);  // nothing answered the first in 250 ms
    EXPECT_EQ(l.dropped[SWITCH_LATENCY_BEGAN], 1u);
    EXPECT_EQ(l.report_us, 300000u);
    EXPECT_EQ(l.kind, SWITCH_LATENCY_MOVED);
    EXPECT_EQ(line(l), "began 0 (dropped 1)");
}

TEST(Latency, PercentilesAreBinTopsUnderTheMaximum)
{
    switch_latency_hist h = {};
    for (unsigned i = 0; i < 95; i++)
        switch_latency_add(&h, 10200);  // bin [10.0, 10.5) ms
    for (unsigned i = 0; i < 5; i++)
        switch_latency_add(&h, 40100);  // bin [40, 42) ms
    EXPECT_EQ(switch_latency_percentile(&h, 50), 10500u);
    EXPECT_EQ(switch_latency_percentile(&h, 95), 10500u);
    EXPECT_EQ(switch_latency_percentile(&h, 96), 40100u);
    EXPECT_EQ(switch_latency_bin(UINT64_MAX), unsigned(SWITCH_LATENCY_BINS - 1));
    EXPECT_EQ(switch_latency_bin_top(SWITCH_LATENCY_FINE_BINS), 34000u);
}

TEST(Latency, RestartKeepsTheSampleInFlight)
{
    switch_latency l = {};
    touch(l, 1000000, 0);
    switch_latency_report(&l, 0);
    switch_latency_delivered(&l, 2000000, 27, 2001000);
    switch_latency_restart(&l);
    EXPECT_EQ(line(l), "");
    switch_latency_message(&l, 27, SWITCH_LATENCY_COMMIT_ID, 2003000);
    switch_latency_present(&l, 2010000, 2012000, 2020000);
    EXPECT_EQ(l.hist[SWITCH_LATENCY_MOVED][SWITCH_LATENCY_TOTAL].n, 1u);
}

TEST(Latency, LinesAreCutAndTerminated)
{
    switch_latency l = {};
    touch(l, 1000000, SWITCH_LATENCY_BEGAN_BIT);
    touch(l, 2000000, SWITCH_LATENCY_ENDED_BIT);
    touch(l, 3000000, 0);
    for (size_t size = 1; size < 400; size++)
        EXPECT_LT(line(l, size).size(), size);
}
