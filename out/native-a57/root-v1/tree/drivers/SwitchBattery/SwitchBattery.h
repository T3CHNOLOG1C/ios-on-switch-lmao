// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchBattery's interface for other drivers (the battery itself is an IOPMPowerSource; see SwitchBattery.cpp).
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// The BQ24193's OTG boost, which sources VBUS on the USB-C port for USB host mode (drivers/SwitchUSB): on = REG01
// CHG_CONFIG OTG with BOOST_LIM 1.3 A; off = REG01's two fields as they were before. SwitchBattery is the one writer
// of REG01. Thread context (I2C1); 0 on success, -1 on an I2C error (logged).
int switch_battery_set_otg_boost(bool on);

#ifdef __cplusplus
}
#endif
