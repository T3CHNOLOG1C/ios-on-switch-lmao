// SPDX-License-Identifier: BSD-2-Clause-Patent
// System sleep (SC7) for iPadOS on the Switch OLED (T210B01), boot-arg switch-sleep=2. Display power (panel, scanout and
// touch off/on through SwitchBrightness and the NativeLegacyDisplay selector-12 wrap) is on unless switch-sleep=0; =2 adds
// this driver on top of it.
//
// How sleep happens (all stock XNU, iokit/Kernel/IOPMrootDomain.cpp, IOCPU.cpp, osfmk/kern/machine.c, arm64/cpu.c):
//   powerd's idle-sleep policy (no user activity, display off, no PreventUserIdleSystemSleep assertions) makes
//   IOPMrootDomain sleep -> IOPlatformExpert::sleepKernel() -> IOCPUSleepKernel(): platform sleep actions (below),
//   ArmCpu::haltCPU() on each running secondary and then the boot CPU = processor_exit() -> cpu_sleep() on that
//   core -> ArmCpu::quiesceCPU() -> ml_arm_sleep() -> arm64_prepare_for_sleep() -> ArmCpu's PSCI handler:
//   CPU_OFF on a secondary, SYSTEM_SUSPEND (ATF -> BPMP firmware -> SC7) on the boot CPU.
//   Wake: PMC wake event -> bootrom -> warmboot (hekate's sc7exit_b01.bin) -> ATF -> ioswitch_secondary_entry
//   (EL2 -> EL1) -> XNU reset vector -> start_cpu -> arm_init_cpu -> ArmCpu::initCPU(false) -> back in
//   IOCPUSleepKernel, which CPU_ONs the secondaries and runs the platform wake actions.
//   The ADT's /defaults sleep-disabled must be 0 (scripts/package_raw_boot.py writes 1 unless switch-sleep=2).
//
// Order (one driver here runs every step, so the order is this list; each hook is <driver>_suspend/_resume):
//   sleep action, thread context, interrupts on, secondaries still running:
//     0 switch_als_suspend (BH1730 off: shares LDO6, clocked by DC A)
//     1 tegra_display_suspend (display agent: panel off, DC quiesced)   2 switch_touch_suspend (FTM4 + LDO6 off)
//     3 switch_joycon_suspend (controllers agent)                       4 (tegra_nvdec_suspend, once it exists)
//     5 switch_thermal_suspend (CPU back to the boot clocks, fan off)   PESyncNVRAM (SwitchNVRAM's sync, SD still up)
//     6 ioswitch_block_suspend (SD drained, off)
//     7 switch_rtc_suspend (MAX77620 ALARM1 for powerd's next wake)     8 PMC wake events armed (here)
//     9 tegra_i2c_suspend (last: holds every bus until resume)
//   quiesce action, boot CPU, interrupts off, secondaries already off:
//     tegra_gpio_suspend, pinmux saved (here), arm_gic_suspend
//   (PSCI SYSTEM_SUSPEND; the SoC is in SC7)
//   active action, boot CPU, interrupts off, before the secondaries start:
//     pinmux restored (here), tegra_gpio_resume, arm_gic_resume, PMC wake status read (here)
//   wake action, thread context, every core running, the reverse of the sleep action:
//     tegra_i2c_resume, switch_rtc_resume (wall clock re-read before clock_wakeup_calendar), ioswitch_block_resume,
//     switch_thermal_resume, (tegra_nvdec_resume), switch_joycon_resume, switch_touch_resume, tegra_display_resume,
//     switch_als_resume
// The display and Joy-Con hooks are the display and controllers agents' (no-ops / panics as their headers say). A driver
// that is turned on but has no suspend path (Wi-Fi, Bluetooth, audio, GPU, EMC/DFLL, NVDEC/host1x, USB-C) holds a CPU
// power assertion instead, so the system stays awake exactly as without this driver. The display hooks need the DC A
// interrupt (sheng-vsync / ioswitch-vblank) and SwitchBrightness, as TegraDisplayPower.h says.
//
// Wake sources: the MAX77620's nIRQ (PMC wake 51, level low) carries the power key (ONOFFIRQ EN0, SwitchButtons) and
// the RTC alarm (ALARM1, SwitchPMIC); the BM92T36 USB-PD ALERT (PMC wake 14, PK4 level low, SwitchPD) a USB-C plug or
// unplug, unless that line is stuck low. Nothing else wakes the console (Wi-Fi wake 8 and BT wake 52 are commented out
// in the OLED DTB too).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Board thread, after the board drivers have started. Returns 0, also when switch-sleep is not 2 (then it does
// nothing) or when sleep has to stay off (one log line says why); never panics at start.
int switch_suspend_start(void);

#ifdef __cplusplus
}
#endif
