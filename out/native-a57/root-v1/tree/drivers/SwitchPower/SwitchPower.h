// SPDX-License-Identifier: BSD-2-Clause-Patent
// Nintendo Switch OLED (T210B01) power services, statically linked into the kernel.
#ifndef SWITCH_POWER_H
#define SWITCH_POWER_H
#ifdef __cplusplus
extern "C" {
#endif

// Starts the thermal service (SwitchThermal.cpp), its single entry point: drives the fan to a fixed safe duty at once
// (MMIO only), claims the TMP451 ALERT interrupt (GPIO PX4, level low; panics if it cannot be had), then follows the
// L4T fan curve on every ALERT, with a re-read every 5 s as a staleness bound. With switch-perf=1 it also raises the
// CPU up a ladder of L4T rates (switch-perf-steps / -trace for staged tests) while the SoC is below its thermal trip,
// dropping to the inherited clocks at the trip and going up again below the hysteresis, at most once per hold-off.
// switch_pmic_start() calls it before the root mount; the board thread's later call is a no-op then. Thread context
// after IOKit is up; idempotent.
// Returns 0 on success, -1 if the service could not be created (the fan may still be running at the safe duty if
// the MMIO part succeeded).
int switch_thermal_start(void);

// With the CPU on the DFLL (switch-cpu-mode=dfll), moves it back to PLLX so that SwitchPMIC's shut-down/restart
// I2C5 traffic has the bus to itself; nothing otherwise. Thread context; waits for the thermal work loop.
void switch_thermal_cpu_quiesce(void);

// PMIC services (SwitchPMIC.cpp): logs the CPU/EMC clock line, publishes the MAX77620 RTC as the "IORTC" resource,
// installs PE_halt_restart (Shut Down / Restart), sets the EMC clock through the BPMP firmware (drivers/TegraBPMP)
// and logs the clock line again, then starts the thermal service. Called once by XNU's IOKitInitializeTime() on the
// bsd_init thread, before the calendar is read; sleeps on I2C5 and BPMP answers and, the first time, until the GIC
// is up. Returns 0, or -1 when the MAX77620 does not answer (no RTC or power-off then; the rest still runs).
int switch_pmic_start(void);

// System sleep hooks (drivers/SwitchSuspend), thread context with I2C up; no-ops when the service did not start.
// Thermal: before SC7 the CPU goes back to the boot chain's clocks and VDD_CPU and the fan stops; after it the fan,
// sensor and CPU operating point come back as at start. RTC: powerd's next scheduled wake ("wake" /
// "WakeByCalendarDate" PM settings) is set as MAX77620 ALARM1 before SC7; after it the wall clock is re-read from the
// RTC and the alarm turned off; switch_rtc_resume returns 1 when ALARM1 is what woke the console.
// switch_rtc_sleep_enable (SwitchSuspend's start, once): take powerd's scheduled wakes from the root domain from now on.
void switch_rtc_sleep_enable(void);
void switch_thermal_suspend(void);
void switch_thermal_resume(void);
void switch_rtc_suspend(void);
int switch_rtc_resume(void);
// The RTC part of the MAX77620 interrupt (SwitchButtons owns the nIRQ): reads RTCINT, which releases the line.
void switch_rtc_interrupt(void);

#ifdef __cplusplus
}
#endif
#endif
