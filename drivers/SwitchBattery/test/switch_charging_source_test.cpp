// Guards on the charging drivers' source for what switch_charging_test's models cannot see: the PD request never waits
// (every step is an ALERT or a one-shot timer; the models' clock does not run while code spins), the charger's REG05
// is written only from the policy's value and by the watchdog's owner, and SwitchBattery never addresses the BM92T36
// (SwitchPD owns it).
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <regex>
#include <set>
#include <string>
#include <vector>

#include "bazel/testing/source_text.h"

using switch_ios::test::Between;
using switch_ios::test::Body;
using switch_ios::test::Count;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

TEST(SwitchChargingSource, PdRequestNeverWaits)
{
    const std::string negotiation = Between(SourceText("../../SwitchUSB/SwitchPD.cpp"),
                                            "// --- USB-PD request (SwitchPD.h)", "namespace {\nSwitchPD *startPD()");
    ASSERT_FALSE(negotiation.empty());
    for (const char *wait : {"IOSleep", "IODelay", "while (", "for (;;)", "check_delivery"})
        EXPECT_THAT(negotiation, Not(HasSubstr(wait))) << wait;
}

TEST(SwitchChargingSource, Reg05HasTwoWriters)
{
    const std::string battery = SourceText("../SwitchBattery.cpp");
    // The managed settings (writeCharger) write REG05 only from the policy's value...
    std::vector<std::string> writes;
    const std::regex update(R"(chargerUpdate\(bq::TerminationTimer, ([^;]*)\);)");
    for (std::sregex_iterator it(battery.begin(), battery.end(), update), end; it != end; ++it)
        writes.push_back((*it)[1]);
    EXPECT_EQ(writes, std::vector<std::string>{"charge::kReg05Mask, s.reg05"});
    // ...and the watchdog's owner is the one direct writer.
    EXPECT_EQ(Count(battery, "tegra_i2c_write(kBus, kCharger, bq::TerminationTimer"), 1u);
    EXPECT_THAT(Body(battery, "int watchdogLocked()"),
                HasSubstr("tegra_i2c_write(kBus, kCharger, bq::TerminationTimer"));
}

TEST(SwitchChargingSource, BatteryAddressesOnlyGaugeAndCharger)
{
    const std::string battery = SourceText("../SwitchBattery.cpp");
    std::set<std::string> addresses;
    const std::regex access(R"(tegra_i2c_(?:read|write)\(kBus, (\w+),)");
    for (std::sregex_iterator it(battery.begin(), battery.end(), access), end; it != end; ++it)
        addresses.insert((*it)[1]);
    EXPECT_EQ(addresses, (std::set<std::string>{"kCharger", "kGauge"}));
}
