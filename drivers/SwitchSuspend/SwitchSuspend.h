// SPDX-License-Identifier: BSD-2-Clause-Patent
// System sleep (SC7) for iPadOS on the Switch OLED (T210B01). It needs display power (panel, scanout and touch off/on
// through SwitchBrightness and the NativeLegacyDisplay selector-12 wrap; off only with switch-sleep=0).
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
//   The ADT's /defaults sleep-disabled is 0 (tools/package/adt_edit.py); while sleep cannot be taken this driver
//   holds a CPU power assertion instead (kPMCPUAssertion), since ArmCpu panics on a halt without SC7.
//
// Order (one driver here runs every step, so the order is this list; each hook is <driver>_suspend/_resume):
//   sleep action, thread context, interrupts on, secondaries still running:
//     0 als (BH1730 off: shares LDO6, clocked by DC A)   1 display (panel off, DC quiesced)   1 gpu (submissions
//     held, channels idle, nvkm suspended, VDD_GPU off)   2 touch (FTM4, LDO6 off)
//     2 motion (IMU off, SPI4 gated)   3 joycon   3 usb (XUDC off the bus)   3 bluetooth (controllers dropped, host
//     wake set up, UART-D stopped)
//     3 wifi (chip let go, worker parked)   4 host1x (NVDEC, NVENC, VIC, NVJPG quiesced, then host1x)   4 audio
//     5 thermal (CPU off the DFLL to PLLX at the boot clocks, fan off)   5 se (both engines idle for BL31's save)
//     6 PESyncNVRAM (SD still up), journal to Data
//     7 sd (drained, off)   8 rtc (MAX77620 ALARM1 for powerd's next wake)   8 battery (charge-zone alert), gauge read
//     9 PMC wake events armed   9 bpmp (EMC to 204 MHz, MRQ_ENABLE_SUSPEND SC7)   10 i2c (last: holds every bus)
//   quiesce action, boot CPU, interrupts off, secondaries already off:
//     tegra_gpio_suspend, pinmux saved (here), arm_gic_suspend, Tegra RTC read (here)
//   (PSCI SYSTEM_SUSPEND; the SoC is in SC7)
//   active action, boot CPU, interrupts off, before the secondaries start:
//     Tegra RTC read, pinmux restored (here), tegra_gpio_resume, pads out of deep power-down (here), arm_gic_resume,
//     PMC wake status read (here)
//   wake action, thread context, every core running:
//     0 i2c, gauge read   0 bpmp (the EMC rate back)   1 rtc (wall clock re-read before clock_wakeup_calendar)
//     2 smmu (every attached swgroup)   2 se   3 sd   4 battery   5 thermal (DFLL re-initialized; SwitchThermal
//     enables it again)   6 host1x (before the display, which sits behind it)   6 gpu (power-up, ACR, nvkm objects
//     resumed, submissions open)   6 audio (APE partition, codec)
//     7 wifi (PCIe and the chip brought up whole on its worker)   7 bluetooth   7 usb (XUDC powered on whole, back
//     on the bus; the role again from SwitchPD)   7 joycon   8 motion   8 touch
//     9 display   10 als (after LDO6 and the frame ends)
// A driver that is turned on but has no SC7 path yet holds the system awake instead (switch_suspend_hold). The display
// hooks need the DC A interrupt (sheng-vsync / ioswitch-vblank) and SwitchBrightness, as TegraDisplayPower.h says.
//
// Wake sources (SwitchWakeSources.h switch_wake_pmc_events): the MAX77620's nIRQ (PMC wake 51, level low) carries the
// power key (ONOFFIRQ EN0, SwitchButtons) and the RTC alarm (ALARM1, SwitchPMIC); the BM92T36 USB-PD ALERT (14, PK4
// level low, SwitchPD) a USB-C plug or unplug, unless that line is stuck low; the gauge's ALRT (27) a charge-zone edge;
// the Bluetooth host wake (52, PH5 level low) a controller paging the console or data for a kept link, when
// SwitchBluetooth set it up. A Bluetooth wake keeps the system up for SWITCH_BT_RECONNECT_MS and the reconnecting
// controller's first report presses Home.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Board thread, after the board drivers have started. Returns 0, also when sleep has to stay off (one log line and one
// power-journal line list every reason); never panics at start.
int switch_suspend_start(void);

#ifdef __cplusplus
}
#endif
