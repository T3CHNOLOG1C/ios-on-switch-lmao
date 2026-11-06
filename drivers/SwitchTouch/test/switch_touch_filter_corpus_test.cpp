// drivers/SwitchTouch/SwitchTouchFilter.h replayed over real contacts (switch_touch_corpus.h) and key taps built like
// the ones the user saw turn into key flicks: what a drag, a flick and a tap report.
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include "../SwitchTouchFilter.h"
#include "switch_touch_corpus.h"

namespace stf = switch_touch_filter;
namespace corpus = switch_touch_corpus;

namespace {
// The traces' milliseconds are truncated; consecutive scans 4-6 ms apart were the FTM4's ~5.15 ms period.
constexpr uint64_t kScanUs = 5150;
// UIKit's 10 pt pan threshold at 1.25 px per point.
constexpr double kPanPx = 12.5;
// The keyboard's flick trigger, a quarter of a key's height (SwitchTouchFilter.h). On the 21:02 boot SpringBoard's
// keyboard was a 1024 x 370 pt keyplane of four rows under a 55.2 pt assistant bar: keys 92.5 pt tall, 1.25 px a point.
constexpr double kKeyFlickPx = 0.25 * (370.0 / 4) * 1.25;

struct Point {
    int ms;
    double rawX, rawY, x, y;
};

// Each contact from the middle of the report frame.
std::vector<Point> replay(const corpus::Sample *samples, unsigned count)
{
    constexpr int kX = 360, kY = 640;
    stf::Contact c = {};
    std::vector<Point> out;
    uint64_t us = 1000000;
    c.begin(kX + samples[0].dx, kY + samples[0].dy, samples[0].z, us);
    out.push_back({samples[0].ms, 0, 0, 0, 0});
    for (unsigned i = 1; i < count; ++i) {
        const int gap = samples[i].ms - samples[i - 1].ms;
        us += gap >= 4 && gap <= 6 ? kScanUs : (uint64_t)gap * 1000;
        c.sample(kX + samples[i].dx, kY + samples[i].dy, samples[i].z, us);
        out.push_back({samples[i].ms, (double)samples[i].dx, (double)samples[i].dy, (double)(c.x.pixel() - kX),
                       (double)(c.y.pixel() - kY)});
    }
    return out;
}

double travel(const std::vector<Point> &points, bool reported)
{
    double most = 0;
    for (const Point &p : points)
        most = std::max(most, std::hypot(reported ? p.x : p.rawX, reported ? p.y : p.rawY));
    return most;
}

double percentile(std::vector<double> values, double fraction)
{
    std::sort(values.begin(), values.end());
    return values.empty() ? 0 : values[std::min(values.size() - 1, (size_t)(values.size() * fraction))];
}

bool isTap(const corpus::Contact &contact, const std::vector<Point> &points)
{
    return contact.samples[contact.count - 1].ms < 300 && travel(points, false) < 40;
}
}  // namespace

// Before these numbers, the strength-rate term held back 26 % of every drag's steps of 5 px or more, the point trailed
// the finger by 10 px at the median and 90 px at p95, and flicks lifted 85 px short at 0.61 of the finger's speed.
TEST(SwitchTouchFilterCorpus, DragsFollowTheFinger)
{
    std::vector<double> lag, flickShort, release;
    unsigned steps = 0, frozen = 0, drags = 0;
    for (const corpus::Contact &contact : corpus::kContacts) {
        const std::vector<Point> points = replay(contact.samples, contact.count);
        if (isTap(contact, points))
            continue;
        ++drags;
        for (size_t i = 1; i < points.size(); ++i) {
            const Point &p = points[i], &q = points[i - 1];
            if (p.ms < 20)
                continue;
            lag.push_back(std::hypot(p.rawX - p.x, p.rawY - p.y));
            const double raw = std::hypot(p.rawX - q.rawX, p.rawY - q.rawY);
            if (raw >= 5) {
                ++steps;
                frozen += std::hypot(p.x - q.x, p.y - q.y) < 0.2 * raw;
            }
        }
        const Point &end = points.back();
        auto from = std::find_if(points.begin(), points.end(), [&](const Point &p) {
            return p.ms >= end.ms - 30;
        });
        const double rawThrow = std::hypot(end.rawX - from->rawX, end.rawY - from->rawY);
        const double perScan = rawThrow / std::max(1, end.ms - from->ms) * kScanUs / 1000;
        if (perScan < 10)
            continue;  // not a flick: a lift that slowed first
        flickShort.push_back(std::hypot(end.rawX - end.x, end.rawY - end.y));
        release.push_back(std::hypot(end.x - from->x, end.y - from->y) / rawThrow);
    }
    ASSERT_EQ(drags, 55u);
    ASSERT_EQ(flickShort.size(), 46u);
    EXPECT_LE(percentile(lag, 0.5), 3.0) << "the hysteresis, 2.5 px";
    EXPECT_LE(percentile(lag, 0.95), 15.0);
    EXPECT_LE(frozen * 100, steps * 5) << frozen << " of " << steps << " steps held back";
    EXPECT_LE(percentile(flickShort, 0.5), 3.0);
    EXPECT_LE(percentile(flickShort, 0.95), 12.0);
    EXPECT_GE(percentile(release, 0.5), 0.97);
    EXPECT_GE(percentile(release, 0.1), 0.9);
}

TEST(SwitchTouchFilterCorpus, TapsStayStill)
{
    unsigned taps = 0;
    for (const corpus::Contact &contact : corpus::kContacts) {
        const std::vector<Point> points = replay(contact.samples, contact.count);
        if (!isTap(contact, points))
            continue;
        ++taps;
        EXPECT_LE(travel(points, true), 6.0) << contact.at << ": raw travel " << travel(points, false);
    }
    EXPECT_EQ(taps, 20u);
}

// 20:29:00.115: a 40 ms key tap whose point slid 20 px while it lifted (the shape of the user's unwanted key flicks).
TEST(SwitchTouchFilterCorpus, KeyTapThatSlidWhileLifting)
{
    const auto at =
        std::find_if(std::begin(corpus::kContacts), std::end(corpus::kContacts), [](const corpus::Contact &c) {
            return std::string(c.at) == "20:29:00.115";
        });
    ASSERT_NE(at, std::end(corpus::kContacts));
    const std::vector<Point> points = replay(at->samples, at->count);
    EXPECT_GE(travel(points, false), 19.0);
    EXPECT_LE(travel(points, true), 1.0);
}

// Key taps of 40-120 ms whose point drifts 8-20 px, two thirds or all of it while the strength falls to the lift
// (2-6 scans, to 10-50), the rest as a creep of the held finger before it: none reports half the key flick's travel.
TEST(SwitchTouchFilterCorpus, KeyTapDriftNeverFlicksAKey)
{
    double most = 0;
    for (int duration : {40, 60, 80, 120})
        for (int drift : {8, 12, 16, 20})
            for (int liftScans : {2, 3, 4, 6})
                for (int endZ : {10, 30, 50})
                    for (int creepThirds : {0, 1}) {
                        std::vector<corpus::Sample> s = {{0, 0, 0, 40}, {5, 0, 0, 110}};
                        const int hold = std::max(1, (duration - 5 - 5 * liftScans) / 5);
                        const double creep = std::min(drift * creepThirds / 3.0, 1.3 * hold);
                        int ms = 5;
                        for (int i = 1; i <= hold; ++i)
                            s.push_back({(uint16_t)(ms += 5), (int16_t)std::lround(creep * i / hold), 0, 150});
                        for (int i = 1; i <= liftScans; ++i) {
                            const double x = creep + (drift - creep) * i / liftScans;
                            s.push_back({(uint16_t)(ms += 5), (int16_t)std::lround(x), 0,
                                         (uint16_t)(150 + (endZ - 150) * i / liftScans)});
                        }
                        most = std::max(most, travel(replay(s.data(), (unsigned)s.size()), true));
                    }
    EXPECT_LE(most, kKeyFlickPx / 2);
    EXPECT_LE(most, 4.0);
}

// A finger creeping 1-1.3 px a scan for 50 ms at steady strength, then lifting: no pan.
TEST(SwitchTouchFilterCorpus, SteadyCreepIsNoPan)
{
    for (double perScan : {1.0, 1.3}) {
        std::vector<corpus::Sample> s = {{0, 0, 0, 40}, {5, 0, 0, 110}, {10, 0, 0, 150}, {15, 0, 0, 150}};
        int ms = 15;
        for (int i = 1; i <= 10; ++i)
            s.push_back({(uint16_t)(ms += 5), 0, (int16_t)std::lround(perScan * i), 150});
        const int16_t y = s.back().dy;
        for (uint16_t z : {150, 150, 110, 60, 25})
            s.push_back({(uint16_t)(ms += 5), 0, (int16_t)(y + (z < 150 ? 2 * (150 - z) / 40 : 0)), z});
        EXPECT_LE(travel(replay(s.data(), (unsigned)s.size()), true), kPanPx / 2) << perScan << " px a scan";
    }
}
