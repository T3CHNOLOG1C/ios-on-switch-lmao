// SPDX-License-Identifier: BSD-2-Clause-Patent
// Switch OLED Bluetooth (BCM4356A3 on UART-D) for iOS's Bluetooth daemons. Opt-in: boot-arg switch-bt=1.
//
// The kernel does what BlueTool does on an iPad (power the core, 3 Mbaud, patch RAM download), because BlueTool
// only knows Apple's own chips and firmware; see SwitchBluetooth.cpp. Userland then gets the controller as the
// kernel control "com.apple.uart.bluetooth": a SOCK_STREAM PF_SYSTEM socket carrying raw H4, with TIOCGETA /
// TIOCSETA as SYSPROTO_CONTROL socket options, the interface BlueTool's "device -D" and bluetoothd's POSIX HCI
// driver open (BT_HCI_TRANSPORT=H4:POSIX:com.apple.uart.bluetooth,3000000, which kern_exec.c gives bluetoothd with
// switch-bt=1). The rest of the stack, gated by the same boot-arg: the BlueTool stand-in (BlueTool/BlueTool.c) in the
// Data overlay answers bluetoothd's boot/init/deepsleep requests, and launchd's disabled list keeps bluetoothd and
// audiomxd off without it.
//
// ADT, from the packager: /arm-io/bluetooth with
//   "ioswitch,patchram"  the .hcd file (HCI Write_RAM / Launch_RAM records), downloaded at every bring-up
//   "local-mac-address"  6 bytes, display order; written to the controller (Broadcom 0xFC01) after the patch. When
//                        the fuses are readable the unit's own address (U-Boot's 98:B6:E9 + lot code scheme)
//                        replaces it here and in /chosen mac-address-bluetooth0 before userland starts; when deploy
//                        staged hekate's Joy-Con dump, Horizon's replaces that once Data is mounted, before launchd
//                        (switch_bluetooth_data_mounted), and the controller gets it at bluetoothd's first connect
//                        (SwitchBluetooth.cpp, "BD address").
//   "transport-speed" (u32 3000000) and "transport-encoding" (u32 1, H4) for bluetoothd and BlueTool.
#ifndef SWITCH_BLUETOOTH_H
#define SWITCH_BLUETOOTH_H

#ifdef __cplusplus
extern "C" {
#endif

// Without switch-bt=1: returns 0 and touches nothing. Otherwise starts a kernel thread that publishes the BD address,
// claims UART-D and its interrupt (waiting for the GIC), registers the kernel control and brings the controller up,
// then returns 0 at once (-1 if the thread could not be started). Call it first in the board thread, so the
// address is in place before bluetoothd can read it. Bring-up failures are logged in one line; a missing or lost UART-D
// interrupt panics. Call from thread context, any time after IOKit is up.
int switch_bluetooth_start(void);

// kern_exec.c load_init_program through SwitchNVRAM (ioswitch_nvram_data_mounted), once Data is mounted and before
// launchd: with switch-bt=1, publishes the address deploy staged on Data (/private/var/ioswitch/bluetooth/host-address)
// as local-mac-address and mac-address-bluetooth0, so bluetoothd reads it at its start. One line if the file is
// malformed; nothing without one.
void switch_bluetooth_data_mounted(void);

#ifdef __cplusplus
}
#endif
#endif
