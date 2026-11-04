// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchBattery's interface for other drivers (the battery itself is an IOPMPowerSource; see SwitchBattery.cpp).
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// The BQ24193's OTG boost, which sources VBUS on the USB-C port for a sink-only partner (drivers/SwitchUSB/SwitchPD):
// on = REG01 CHG_CONFIG 10 (OTG) with BOOST_LIM 1.3 A, refused while the charger sees an input on VBUS (REG08 PG_STAT
// or VBUS_STAT USB/adapter: the partner sources, we must sink); off = CHG_CONFIG 01 (charge). Absolute, never a
// restore: a charger left in OTG by an earlier boot is put back to charge at SwitchBattery's start. SwitchBattery is
// the one writer of REG01. Thread context (I2C1); 0 on success, SWITCH_BATTERY_BOOST_VBUS_PRESENT when refused, -1 on
// an I2C error (logged).
#define SWITCH_BATTERY_BOOST_VBUS_PRESENT (-2)
int switch_battery_set_otg_boost(bool on);

#ifdef __cplusplus
}
#endif
