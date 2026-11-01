// SPDX-License-Identifier: BSD-2-Clause-Patent
// ArmCpu's side of the GICv2 SPI controller in ArmGIC.cpp. Drivers use IOSwitchInterrupts.h instead.
#pragma once
#include <stdint.h>

// Runs the IOKit vector of an SPI (INTID 32..1019) that ArmCpu has just acknowledged through GICC_IAR, on any
// core with interrupts masked. Returns true if a driver owns the INTID: its handler ran, or the line was soft
// disabled and is now masked at the distributor. Returns false if nobody registered for it (or no driver has
// asked for an interrupt yet); the caller then disables the SPI itself. Never writes GICC_EOIR: ending the
// interrupt stays with the caller, exactly once.
extern "C" bool ioswitch_gic_dispatch(uint32_t intid);

// System sleep (SwitchSuspend's quiesce and active actions; boot CPU, interrupts off, other cores off): save the SPI
// distributor and Tegra LIC state before SC7, write it back after (ATF re-initialized the GIC by then).
extern "C" void arm_gic_suspend(void);
extern "C" void arm_gic_resume(void);
