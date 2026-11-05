// The limits in drivers/SwitchBattery/SwitchChargePolicy.h are the Nintendo/switchroot values the header cites for each
// ([AMS-PARAM], [AMS-INIT], [DTB], [L4T-PD]), checked at compile time; the policy's behaviour is
// switch_charging_test's Policy scenario.
#include <gtest/gtest.h>

#include "../SwitchChargePolicy.h"

static_assert(charge::kChargeCurrentMaxMA == 2048);
static_assert(charge::kChargeVoltageMaxMV == 4208);
static_assert(charge::kCoolChargeCurrentMA == 768);
static_assert(charge::kPreChargeMA == 128);
static_assert(charge::kTerminationMA == 128);
static_assert(charge::kSystemMinimumMV == 3000);
static_assert(charge::kInputVoltageLimitMV == 4360);
static_assert(charge::kUnknownSourceMA == 500);
static_assert(charge::kPdMaxMillivolts == 15000);
static_assert(charge::kPdMaxMilliamps == 3000);
// The JEITA zones: cold below 4 C, cool below 17 C, hot from 51 C.
static_assert(charge::kColdBelowC == 4 && charge::kCoolBelowC == 17 && charge::kHotFromC == 51);
// REG05: termination on, watchdog and safety timer off.
static_assert(charge::kReg05Value == charge::bq::kEnTerm);
// The input limit cap per PDO voltage ([DTB] rohm,pd-*-current-limit-ma): 2 A at 5 and 9 V, 1.5 A at 12 V, 1.2 A
// above, each a BQ24193 input step.
static_assert(charge::pdInputLimit(5000, 3000) == 2000);
static_assert(charge::pdInputLimit(9000, 3000) == 2000);
static_assert(charge::pdInputLimit(12000, 3000) == 1500);
static_assert(charge::pdInputLimit(15000, 3000) == 1200);
// The J120 DT charger's charge-limits (thermal rate per limit index).
static_assert(charge::kThermalRatePercent[0] == 100 && charge::kThermalRatePercent[1] == 70 &&
              charge::kThermalRatePercent[2] == 50 && charge::kThermalRatePercent[3] == 30);

// The checks above are the test; this TEST makes the binary report them as run.
TEST(SwitchChargePolicy, LimitsAreTheCitedOnes)
{
    SUCCEED();
}
