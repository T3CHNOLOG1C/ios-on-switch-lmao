// SwitchWLANTrace.h on the host: frames as en0 hands them over (Ethernet II), DNS queries paired with their answers
// and timed, ARP counted each way, and the firmware counters block's deltas.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "../SwitchWLANTrace.h"

namespace {

constexpr uint32_t kUs = 0x0A04E153;        // 10.4.225.83
constexpr uint32_t kResolver = 0x0A040001;  // 10.4.0.1
constexpr uint64_t kMs = 1000000ull;

void put16(std::vector<uint8_t> &f, size_t at, uint16_t v)
{
    f[at] = static_cast<uint8_t>(v >> 8);
    f[at + 1] = static_cast<uint8_t>(v);
}
void put32(std::vector<uint8_t> &f, size_t at, uint32_t v)
{
    put16(f, at, static_cast<uint16_t>(v >> 16));
    put16(f, at + 2, static_cast<uint16_t>(v));
}

std::vector<uint8_t> udp(uint32_t src, uint32_t dst, uint16_t sport, uint16_t dport, uint16_t id, bool answer,
                         uint16_t fragment = 0)
{
    std::vector<uint8_t> f(14 + 20 + 8 + 12 + 20, 0);
    f[0] = 0x9c;  // unicast destination
    put16(f, 12, 0x0800);
    f[14] = 0x45;
    put16(f, 14 + 6, fragment);
    f[14 + 9] = 17;
    put32(f, 14 + 12, src);
    put32(f, 14 + 16, dst);
    put16(f, 34, sport);
    put16(f, 36, dport);
    put16(f, 42, id);
    f[44] = answer ? 0x81 : 0x01;
    return f;
}
std::vector<uint8_t> query(uint16_t port, uint16_t id)
{
    return udp(kUs, kResolver, port, 53, id, false);
}
std::vector<uint8_t> answer(uint16_t port, uint16_t id)
{
    return udp(kResolver, kUs, 53, port, id, true);
}
std::vector<uint8_t> arp(uint16_t op, uint32_t sender, uint32_t target, bool broadcast)
{
    std::vector<uint8_t> f(42, 0);
    if (broadcast) memset(f.data(), 0xFF, 6);
    put16(f, 12, 0x0806);
    put16(f, 14, 1);
    put16(f, 16, 0x0800);
    f[18] = 6;
    f[19] = 4;
    put16(f, 20, op);
    put32(f, 28, sender);
    put32(f, 38, target);
    return f;
}

swlan_frame_kind kind(const std::vector<uint8_t> &f)
{
    return swlan_frame_classify(f.data(), f.size()).kind;
}

TEST(SwitchWlanTrace, ClassifiesDnsAndArp)
{
    EXPECT_EQ(kind(query(50000, 7)), SWLAN_FRAME_DNS_QUERY);
    EXPECT_EQ(kind(answer(50000, 7)), SWLAN_FRAME_DNS_ANSWER);
    const std::vector<uint8_t> q = query(50000, 7), a = answer(50000, 7);
    EXPECT_EQ(swlan_frame_classify(q.data(), q.size()).key, (50000u << 16) | 7u);
    EXPECT_EQ(swlan_frame_classify(a.data(), a.size()).key, (50000u << 16) | 7u);
    EXPECT_EQ(kind(arp(1, kResolver, kUs, true)), SWLAN_FRAME_ARP_REQUEST);
    EXPECT_EQ(kind(arp(2, kUs, kResolver, false)), SWLAN_FRAME_ARP_REPLY);
}

TEST(SwitchWlanTrace, IgnoresWhatIsNotDnsOverUdpOrIpv4Arp)
{
    EXPECT_EQ(kind(udp(kUs, kResolver, 50000, 53, 7, true)), SWLAN_FRAME_OTHER);   // QR set on a query
    EXPECT_EQ(kind(udp(kResolver, kUs, 53, 50000, 7, false)), SWLAN_FRAME_OTHER);  // QR clear on an answer
    EXPECT_EQ(kind(udp(kUs, kResolver, 5353, 5353, 7, false)), SWLAN_FRAME_OTHER);
    EXPECT_EQ(kind(udp(kUs, kResolver, 50000, 53, 7, false, 0x00B9)), SWLAN_FRAME_OTHER);  // a later fragment
    std::vector<uint8_t> tcp = query(50000, 7);
    tcp[14 + 9] = 6;
    EXPECT_EQ(kind(tcp), SWLAN_FRAME_OTHER);
    std::vector<uint8_t> ipv6 = query(50000, 7);
    put16(ipv6, 12, 0x86DD);
    EXPECT_EQ(kind(ipv6), SWLAN_FRAME_OTHER);
    std::vector<uint8_t> rarp = arp(3, kUs, kResolver, true);
    EXPECT_EQ(kind(rarp), SWLAN_FRAME_OTHER);
    const std::vector<uint8_t> q = query(50000, 7);
    EXPECT_EQ(swlan_frame_classify(q.data(), 14 + 20 + 8 + 2).kind, SWLAN_FRAME_OTHER);  // no QR byte
    EXPECT_EQ(swlan_frame_classify(q.data(), 13).kind, SWLAN_FRAME_OTHER);
}

TEST(SwitchWlanTrace, PairsAnswersWithQueriesAndTimesThem)
{
    swlan_trace t = {};
    const uint16_t ports[4] = {50001, 50002, 50003, 50004};
    const uint64_t delays[4] = {2 * kMs, 15 * kMs, 150 * kMs, 900 * kMs};
    for (unsigned i = 0; i < 4; ++i) {
        const std::vector<uint8_t> q = query(ports[i], 100);
        swlan_trace_sent(&t, q.data(), q.size(), 10 * kMs);
    }
    for (unsigned i = 0; i < 4; ++i) {
        const std::vector<uint8_t> a = answer(ports[i], 100);
        swlan_trace_received(&t, a.data(), a.size(), 10 * kMs + delays[i]);
    }
    EXPECT_EQ(t.c.dns_queries, 4u);
    EXPECT_EQ(t.c.dns_answers, 4u);
    EXPECT_EQ(t.c.dns_unmatched, 0u);
    for (unsigned b = 0; b < SWLAN_DNS_BUCKETS; ++b) EXPECT_EQ(t.c.dns_within[b], 1u) << "bucket " << b;
    const std::vector<uint8_t> again = answer(ports[0], 100);
    swlan_trace_received(&t, again.data(), again.size(), 2000 * kMs);
    EXPECT_EQ(t.c.dns_unmatched, 1u);
}

TEST(SwitchWlanTrace, ARetryRestartsTheClock)
{
    swlan_trace t = {};
    const std::vector<uint8_t> q = query(50000, 9), a = answer(50000, 9);
    swlan_trace_sent(&t, q.data(), q.size(), 0);
    swlan_trace_sent(&t, q.data(), q.size(), 1000 * kMs);
    swlan_trace_received(&t, a.data(), a.size(), 1002 * kMs);
    EXPECT_EQ(t.c.dns_queries, 2u);
    EXPECT_EQ(t.c.dns_within[0], 1u);
}

TEST(SwitchWlanTrace, OnlyTheNewestQueriesArePaired)
{
    swlan_trace t = {};
    for (uint16_t i = 0; i < SWLAN_DNS_OUTSTANDING + 1; ++i) {
        const std::vector<uint8_t> q = query(static_cast<uint16_t>(40000 + i), i);
        swlan_trace_sent(&t, q.data(), q.size(), 0);
    }
    const std::vector<uint8_t> oldest = answer(40000, 0),
                               newest = answer(40000 + SWLAN_DNS_OUTSTANDING, SWLAN_DNS_OUTSTANDING);
    swlan_trace_received(&t, oldest.data(), oldest.size(), kMs);
    swlan_trace_received(&t, newest.data(), newest.size(), kMs);
    EXPECT_EQ(t.c.dns_unmatched, 1u);
    EXPECT_EQ(t.c.dns_within[0], 1u);
}

TEST(SwitchWlanTrace, CountsArpRequestsForOurAddress)
{
    swlan_trace t = {};
    const std::vector<uint8_t> who = arp(1, kResolver, kUs, true);
    swlan_trace_received(&t, who.data(), who.size(), 0);
    EXPECT_EQ(t.c.arp_in_requests, 1u);
    EXPECT_EQ(t.c.arp_in_requests_for_us, 0u);  // not yet known: nothing sent
    const std::vector<uint8_t> q = query(50000, 1);
    swlan_trace_sent(&t, q.data(), q.size(), 0);
    const std::vector<uint8_t> probe = arp(1, kResolver, kUs, false), other = arp(1, kResolver, kUs + 1, true);
    swlan_trace_received(&t, probe.data(), probe.size(), 0);
    swlan_trace_received(&t, other.data(), other.size(), 0);
    const std::vector<uint8_t> reply = arp(2, kUs, kResolver, false), ask = arp(1, kUs, kResolver, true);
    swlan_trace_sent(&t, reply.data(), reply.size(), 0);
    swlan_trace_sent(&t, ask.data(), ask.size(), 0);
    const std::vector<uint8_t> told = arp(2, kResolver, kUs, false);
    swlan_trace_received(&t, told.data(), told.size(), 0);
    EXPECT_EQ(t.c.arp_in_requests, 3u);
    EXPECT_EQ(t.c.arp_in_requests_for_us, 1u);
    EXPECT_EQ(t.c.arp_in_requests_unicast, 1u);
    EXPECT_EQ(t.c.arp_out_replies, 1u);
    EXPECT_EQ(t.c.arp_out_requests, 1u);
    EXPECT_EQ(t.c.arp_in_replies, 1u);
}

TEST(SwitchWlanTrace, DescribesTheWindow)
{
    swlan_trace_counts was = {}, now = {};
    was.dns_queries = 10;
    now.dns_queries = 71;
    now.dns_answers = 2;
    now.dns_within[0] = 2;
    now.arp_out_requests = 1;
    now.arp_in_requests = 3;
    now.arp_in_requests_for_us = 3;
    now.arp_in_requests_unicast = 2;
    now.arp_in_replies = 1;
    char text[400];
    swlan_trace_describe(&now, &was, text, sizeof(text));
    EXPECT_STREQ(
        text, "DNS out 61 answered 2 (3 ms 2, 20 ms 0, 200 ms 0, later 0, unmatched 0), ARP out 1 request 0 replies, "
              "in 3 requests (3 for us, 2 unicast) 1 reply");
}

std::vector<uint8_t> block(unsigned version, std::vector<uint32_t> slots)
{
    std::vector<uint8_t> b(4 + 4 * slots.size());
    b[0] = static_cast<uint8_t>(version);
    b[2] = static_cast<uint8_t>(b.size());
    b[3] = static_cast<uint8_t>(b.size() >> 8);
    for (size_t i = 0; i < slots.size(); ++i)
        for (int k = 0; k < 4; ++k) b[4 + 4 * i + k] = static_cast<uint8_t>(slots[i] >> (8 * k));
    return b;
}

TEST(SwitchWlanTrace, FirmwareCountersAsDeltasBySlot)
{
    const std::vector<uint8_t> a = block(10, {5, 7, 9, 0}), b = block(10, {5, 19, 9, 2});
    char text[200];
    swlan_fw_counters_delta(a.data(), a.size(), nullptr, 0, text, sizeof(text));
    EXPECT_STREQ(text, "v10 20 B: first");
    swlan_fw_counters_delta(b.data(), b.size(), a.data(), a.size(), text, sizeof(text));
    EXPECT_STREQ(text, "v10 20 B: 2+12 4+2");
    swlan_fw_counters_delta(b.data(), b.size(), b.data(), b.size(), text, sizeof(text));
    EXPECT_STREQ(text, "v10 20 B: none");
    const std::vector<uint8_t> c = block(11, {5, 19, 9, 2});
    swlan_fw_counters_delta(c.data(), c.size(), b.data(), b.size(), text, sizeof(text));
    EXPECT_STREQ(text, "v11 20 B: first");
    swlan_fw_counters_delta(a.data(), 3, nullptr, 0, text, sizeof(text));
    EXPECT_STREQ(text, "3 bytes");
}

TEST(SwitchWlanTrace, FirmwareCountersLineStaysInItsBuffer)
{
    std::vector<uint32_t> was(200), now(200);
    for (size_t i = 0; i < now.size(); ++i) now[i] = 1000000u + static_cast<uint32_t>(i);
    const std::vector<uint8_t> a = block(10, was), b = block(10, now);
    char text[120];
    memset(text, 'x', sizeof(text));
    const int n = swlan_fw_counters_delta(b.data(), b.size(), a.data(), a.size(), text, sizeof(text));
    const std::string s(text);
    EXPECT_LT(s.size(), sizeof(text));
    EXPECT_EQ(static_cast<size_t>(n), s.size());
    EXPECT_NE(s.find(" more)"), std::string::npos) << s;
    EXPECT_EQ(s.rfind("v10 804 B: 1+1000000 ", 0), 0u) << s;
}

}  // namespace
