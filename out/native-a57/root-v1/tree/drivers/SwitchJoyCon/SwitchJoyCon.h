// SPDX-License-Identifier: BSD-2-Clause-Patent
// Nintendo Switch Joy-Cons as iPadOS game controllers, on the console rails and over Bluetooth, and the
// game-controller personalities every other gamepad (Xbox, DualShock 4, DualSense, generic HID) needs.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Boot-args, all default 0 (off):
//   switch-joycon-rail=1    rail Joy-Cons (UART-B right, UART-C left) as Joy-Con HID devices for GameController, with
//                           attach/detach detection and rail charging (SwitchJoyConRail.cpp); their Home and Capture
//                           buttons are also the iPad's Home button and screenshot key (SwitchJoyConHID.cpp);
//   switch-joycon-bt=1      the game-controller HID personalities for Bluetooth Joy-Cons, and with the rail driver on,
//                           pairing on attach: the Joy-Con is paired to the console's Bluetooth address over the rail
//                           and the link key is published for joyconpaird to hand to bluetoothd (helper/);
//   switch-gc-personality=1 use iPadOS's own AppleGameControllerPersonality.kext personalities (Xbox, DualShock 4,
//                           DualSense, Luna, Nintendo, and the generic-gamepad AppleGCHIDEventDummyService) instead of
//                           this driver's Nintendo-only set. Needs the kext in the prelink set (panics if it is not);
//                           without the boot-arg its personalities are taken out of the catalogue at start, so a kernel
//                           that has it prelinked boots as one without it (SwitchJoyConHID.cpp);
//   switch-joycon-nfc=1     Joy-Con (R) / Pro Controller NFC: the rail keeps feeding GameController standard reports
//                           while the NFC/IR MCU streams 0x31 reports, and the SwitchJoyConNFC marker service is
//                           published so launchd starts joyconnfc (helper/), which reads tags over HID on the rail and
//                           over Bluetooth alike.
// Thread context, after bsd_autoconf() with the GIC up (the board thread). Idempotent. Returns 0; failures panic.
int switch_joycon_start(void);

// Rail state across SC7 (thread context, may sleep; both are no-ops unless the rail driver runs).
// switch_joycon_suspend: stops both rails: timers cancelled, detect and UART interrupts masked, rail HID devices
// terminated (GameController sees the Joy-Cons disconnect). The UART, CAR and pinmux state may then be lost.
// switch_joycon_resume: call after SC7 exit once TegraGPIO has restored its pin configuration (the detect pins'
// falling-edge triggers) and the GIC is up: re-initialises pinmux, clocks and UARTs and waits for Joy-Cons again; a
// Joy-Con still seated is found by level and handshaken as on a fresh attach.
void switch_joycon_suspend(void);
void switch_joycon_resume(void);

#ifdef __cplusplus
}
#endif
