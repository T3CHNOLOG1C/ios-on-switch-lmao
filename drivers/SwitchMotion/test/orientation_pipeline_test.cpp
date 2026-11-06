// SPDX-License-Identifier: BSD-2-Clause-Patent
// The device orientation report against orientation_pipeline_model.h: the layout kSwitchOrientationDescriptor declares,
// the stale-orientation failure the descriptor and reports without a sample-time child produce, and what a report
// that makes no event does with its child.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "drivers/SwitchTouch/SwitchHID.h"
#include "orientation_pipeline_model.h"

using orientation_model::Descriptor;
using orientation_model::Element;
using orientation_model::EventDriver;
using orientation_model::Notifier;

namespace {
void post(EventDriver &driver, const SwitchOrientationReport &report)
{
    driver.report(reinterpret_cast<const uint8_t *>(&report), sizeof(report), report.timestamp);
}

void deliver(const EventDriver &driver, Notifier &coreMotion)
{
    for (const orientation_model::Event &event : driver.dispatched)
        coreMotion.onEvent(event);
}
}  // namespace

TEST(OrientationPipeline, DescriptorDeclaresTheReport)
{
    const Descriptor d = orientation_model::parse(kSwitchOrientationDescriptor, sizeof(kSwitchOrientationDescriptor));
    ASSERT_EQ(d.topLevel.size(), 1u);
    EXPECT_EQ(d.topLevel[0].page, 0x20u);
    EXPECT_EQ(d.topLevel[0].usage, 0x8au);
    EXPECT_EQ(d.topLevel[0].type, 1u);
    EXPECT_EQ(d.inputBits[1], (sizeof(SwitchOrientationReport) - 1) * 8);

    std::vector<const Element *> sets[2];
    std::vector<const Element *> times, others;
    for (const Element &e : d.elements) {
        EXPECT_EQ(e.reportID, 1u);
        if (orientation_model::isOrientation(e))
            sets[e.bit / 8].push_back(&e);
        else if (orientation_model::isVendorMessageChild(e))
            times.push_back(&e);
        else
            others.push_back(&e);
    }
    EXPECT_TRUE(others.empty());
    for (unsigned s = 0; s < 2; ++s) {
        ASSERT_EQ(sets[s].size(), 7u);
        for (unsigned k = 0; k < 7; ++k) {
            EXPECT_EQ(sets[s][k]->usage, 0x64u + k);
            EXPECT_EQ(sets[s][k]->size, 1u);
            EXPECT_EQ(sets[s][k]->bit, (offsetof(SwitchOrientationReport, orientation) + s - 1) * 8 + k);
            EXPECT_EQ(sets[s][k]->parent.page, 0x20u);
        }
    }
    ASSERT_EQ(times.size(), 1u);
    EXPECT_EQ(times[0]->page, 0xff15u);
    EXPECT_EQ(times[0]->usage, 2u);
    EXPECT_EQ(times[0]->size, 64u);
    EXPECT_EQ(times[0]->bit, (offsetof(SwitchOrientationReport, timestamp) - 1) * 8);
    EXPECT_EQ(times[0]->parent.type, 0u);
}

// The descriptor and report pattern of a build without the child: a report with no bit, then the bit. Every event
// reaches CoreMotion with time 0 and is dropped, as the field logs read ("Skipping stale orientation (1 @ 0)").
TEST(OrientationPipeline, WithoutTheChildEveryEventIsStale)
{
    const uint8_t descriptor[] = {
        0x05, 0x20, 0x09, 0x8a, 0xa1, 0x01, 0x85, 0x01, 0x06, 0x0c, 0xff, 0x19, 0x64, 0x29, 0x6a,
        0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x07, 0x81, 0x02, 0x95, 0x01, 0x81, 0x03, 0xc0,
    };
    EventDriver driver(orientation_model::parse(descriptor, sizeof(descriptor)));
    uint64_t time = 1000;
    for (const uint8_t orientation : {4, 1, 4}) {
        const uint8_t none[2] = {1, 0};
        const uint8_t set[2] = {1, static_cast<uint8_t>(1u << orientation)};
        driver.report(none, sizeof(none), time++);
        driver.report(set, sizeof(set), time++);
    }
    Notifier coreMotion;
    deliver(driver, coreMotion);
    EXPECT_EQ(driver.dispatched.size(), 3u);
    EXPECT_TRUE(coreMotion.delivered.empty());
    EXPECT_EQ(coreMotion.noChildren, 3u);
    EXPECT_EQ(coreMotion.stale, 3u);
    ASSERT_EQ(coreMotion.log.size(), 6u);
    EXPECT_EQ(coreMotion.log[0], "Event has no children");
    EXPECT_EQ(coreMotion.log[3], "Skipping stale orientation (1 @ 0)");
}

// Alternating sets: one event per report, an unchanged orientation too, each with its own time.
TEST(OrientationPipeline, AlternatingSetsMakeAnEventPerReport)
{
    EventDriver driver(orientation_model::parse(kSwitchOrientationDescriptor, sizeof(kSwitchOrientationDescriptor)));
    post(driver, {1, {1u << 4, 0}, 100});
    post(driver, {1, {0, 1u << 4}, 101});
    post(driver, {1, {1u << 1, 0}, 102});
    post(driver, {1, {0, 1u << 1}, 103});
    Notifier coreMotion;
    deliver(driver, coreMotion);
    EXPECT_EQ(driver.dispatched.size(), 4u);
    EXPECT_EQ(coreMotion.delivered, std::vector<uint8_t>({4, 4, 1, 1}));
    EXPECT_EQ(coreMotion.times, std::vector<uint64_t>({100, 101, 102, 103}));
    EXPECT_EQ(coreMotion.stale + coreMotion.wrongType + coreMotion.noChildren, 0u);
}

// A report that makes no event (all bits clear, or the same set twice) still makes the child; IOHIDEventDriver
// dispatches it alone and CoreMotion logs it as a wrong event type.
TEST(OrientationPipeline, AReportWithoutAnEventStraysItsChild)
{
    EventDriver driver(orientation_model::parse(kSwitchOrientationDescriptor, sizeof(kSwitchOrientationDescriptor)));
    post(driver, {1, {0, 0}, 100});
    post(driver, {1, {1u << 4, 0}, 101});
    post(driver, {1, {1u << 4, 0}, 102});
    Notifier coreMotion;
    deliver(driver, coreMotion);
    ASSERT_EQ(driver.dispatched.size(), 3u);
    EXPECT_EQ(driver.dispatched[0].type, 1u);
    EXPECT_EQ(driver.dispatched[1].type, 10u);
    EXPECT_EQ(driver.dispatched[2].type, 1u);
    EXPECT_EQ(coreMotion.delivered, std::vector<uint8_t>({4}));
    EXPECT_EQ(coreMotion.wrongType, 2u);
    ASSERT_FALSE(coreMotion.log.empty());
    EXPECT_EQ(coreMotion.log[0], "Received wrong event type 1");
}
