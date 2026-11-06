// SPDX-License-Identifier: BSD-2-Clause-Patent
// A contact's reported position (SwitchTouch.cpp), in report-frame pixels: the filtering Apple's touch screens get from
// MultitouchHID before IOHIDFamily and UIKit see a contact (21H461 MTParserPath::filterContactForScreenUI), which UIKit's
// gesture thresholds assume. Our path has none (IOHIDEventDriver hands the descriptor's X and Y straight on), and the
// FTM4's centroid moves while a fingertip lands and lifts: on 11-06, still taps travelled 6 px on average (11 at most)
// in the middle of the screen, 10 (24) on the status bar, and keyboard taps of 60-85 ms 15 (60), of which the first
// 30 ms held only 4 (18). UIKit's thresholds are points: at 1.25 px per point, 10 pt (UIPanGestureRecognizer) is
// 12.5 px, 13 pt (where an edge swipe's back-projected start must land) 16 px and 15 pt (edge-swipe hysteresis) 19 px.
// That travel became Control Centre and Notification Centre pulls, Spotlight pull-downs, keyboard flicks and
// dismissals, and a finger paused for the app switcher never read as still.
//
// Two of MultitouchHID's mechanisms, in millimetres and seconds so they hold at any UI scale:
//  * Instability: while the contact is settling (all of a change at touch-down fading to none at kSettleUs, and a fifth
//    of it to none at kSettleTailUs: MTParserPath::computeDebounceTimeInstability's 0.02 s, 0.06 s and 0.2) or its
//    strength is changing fast (a press or a lift: MTParserPath::computeZSignalInstability), position changes are not
//    motion. Here they are kept in an offset instead of reported, and the offset is paid back only while the finger
//    visibly moves (at most half of each step), so a tap reports no travel and a drag that starts during a press loses
//    nothing and never jumps.
//  * Hysteresis: the reported point is a drag centre that follows the finger only once it is kHysteresis away on an
//    axis (MTParserPath::updateHysteresisDragCenter_mm), so a held finger's jitter is no velocity.
#pragma once
#include <stdint.h>

namespace switch_touch_filter {

constexpr int32_t kQ = 16;  // positions in 1/16 report-frame pixel

// The OLED panel: 1280 x 720 px over 155 x 87 mm (8.26 px per mm).
constexpr int32_t kPixelsPerMeter = 1280 * 1000 / 155;

constexpr int32_t fromMicrometers(int32_t um)
{
    return (int32_t)((int64_t)um * kPixelsPerMeter * kQ / 1000000);
}

constexpr int32_t kHysteresis = fromMicrometers(300);
constexpr uint32_t kSettleUs = 20000, kSettleTailUs = 60000;
// Strength change, relative, per second: instability rises from nothing at the first to full at the second.
constexpr uint32_t kStableRate = 12, kUnstableRate = 32;
constexpr uint32_t kFull = 256;  // instability 1.0

constexpr uint32_t ramp(uint64_t value, uint64_t from, uint64_t to)
{
    return value <= from ? 0 : value >= to ? kFull : (uint32_t)((value - from) * kFull / (to - from));
}

// 0 (stable) .. kFull: how much of a position change since the last sample is not finger motion.
constexpr uint32_t instability(uint64_t sinceDownUs, uint64_t sinceLastUs, uint32_t z, uint32_t lastZ)
{
    const uint32_t settled = sinceDownUs >= kSettleUs ? 0 : (uint32_t)((kSettleUs - sinceDownUs) * kFull / kSettleUs);
    const uint32_t tail =
        sinceDownUs >= kSettleTailUs ? 0 : (uint32_t)((kSettleTailUs - sinceDownUs) * kFull / kSettleTailUs / 5);
    const uint32_t settling = settled > tail ? settled : tail;
    const uint32_t larger = z > lastZ ? z : lastZ, change = z > lastZ ? z - lastZ : lastZ - z;
    // change / larger per sinceLastUs, in units of 1/kFull per second
    const uint64_t rate = larger && sinceLastUs ? (uint64_t)change * 1000000 * kFull / larger / sinceLastUs : 0;
    const uint32_t pressing = ramp(rate, (uint64_t)kStableRate * kFull, (uint64_t)kUnstableRate * kFull);
    return settling > pressing ? settling : pressing;
}

struct Axis {
    int32_t raw, offset, centre;  // 1/kQ px; offset = raw - (what the finger's motion has earned)

    void begin(int32_t px)
    {
        raw = centre = px * kQ;
        offset = 0;
    }

    void sample(int32_t px, uint32_t unstable)
    {
        const int32_t now = px * kQ, delta = now - raw;
        const int32_t target = raw - offset;
        raw = now;
        offset += (int32_t)((int64_t)delta * unstable / kFull);
        int32_t moved = raw - offset - target;
        if (moved && offset) {
            int32_t repay = (moved < 0 ? -moved : moved) / 2, owed = offset < 0 ? -offset : offset;
            repay = repay < owed ? repay : owed;
            offset += offset < 0 ? repay : -repay;
        }
        const int32_t follow = raw - offset;
        if (follow - centre > kHysteresis)
            centre = follow - kHysteresis;
        else if (centre - follow > kHysteresis)
            centre = follow + kHysteresis;
    }

    int32_t pixel() const
    {
        return (centre + (centre >= 0 ? kQ / 2 : -kQ / 2)) / kQ;
    }
};

struct Contact {
    Axis x, y;
    uint32_t z;
    uint64_t downUs, lastUs;

    void begin(int32_t px, int32_t py, uint32_t strength, uint64_t us)
    {
        x.begin(px);
        y.begin(py);
        z = strength;
        downUs = lastUs = us;
    }

    // True when the reported pixel changed.
    bool sample(int32_t px, int32_t py, uint32_t strength, uint64_t us)
    {
        const int32_t wasX = x.pixel(), wasY = y.pixel();
        const uint32_t unstable = instability(us - downUs, us - lastUs, strength, z);
        x.sample(px, unstable);
        y.sample(py, unstable);
        z = strength;
        lastUs = us;
        return x.pixel() != wasX || y.pixel() != wasY;
    }
};

}  // namespace switch_touch_filter
