// SPDX-License-Identifier: BSD-2-Clause-Patent
// USB 2.0 on the Switch's USB-C port, on Apple's own USB stacks.
//
// Device mode (on when the ADT has /arm-io/usb-device): the Tegra X1 XUSB device controller (XUDC, SwitchXUDC.cpp)
// under IOUSBDeviceFamily, so a Mac sees an iPad (usbmuxd: idevice*, iproxy, Finder, Xcode; ptpd photos; atc sync).
// The node carries an iPad usb-device node's identity, and IOUSBDeviceFamily reads it; configd's USBDeviceConfiguration
// plug-in picks the functions by configuration-string from /System/Library/AppleUSBDevice/USBDeviceConfiguration.plist
// ("unknownHardware" = AppleUSBMux + PTP).
//
// Host mode (on when the ADT has /arm-io/usb-xhci): the XUSB host controller (XusbHost.cpp, falcon firmware
// tegra210b01_xusb_firmware) under IOUSBHostFamily + AppleUSBXHCI (SwitchXHCI.cpp), with Apple's hub, composite
// device, HID and mass storage drivers: keyboards, mice, flash drives, through a USB-C OTG adapter or an unpowered hub.
//
// The power role is SwitchPD's (SwitchPD.h: sink first; VBUS only to a partner proven sink-only, and SwitchPD alone
// turns it on and off). The data role follows it: SwitchPD sourcing (an OTG adapter, a keyboard, a flash drive) makes
// us the host: the host controller is brought up the first time, the XUDC goes off the bus and USB2 pad 0's ID is
// grounded. When SwitchPD stops sourcing, that is undone in reverse and the XUDC goes back on the bus. Nothing else
// changes the role (a bus suspend, a host asleep). A PD source that wants us as host (docks, hubs with power in) needs
// a PD data-role swap, which is not done: they stay device.
//
// Never blocks boot: everything runs on its own thread and work loops, and anything short of a working interrupt path
// (no node, rail, MMIO, PLL, firmware, kext or class registration) logs one line, "SwitchUSB: device mode off: <why>"
// or "SwitchUSB: host mode off: <why>", and leaves that mode alone. Interrupts only (XUDC INTID 76, xHCI 71, falcon
// mailbox 72, PD ALERT PK4). An interrupt that cannot be attached panics; a lost one panics only on positive evidence
// that it was due (the PD controller saw a cable, and the controller then shows a pending interrupt nobody took).
//
// State lines (kernel log; one per state change):
//   SwitchUSB: device mode ready (XUDC at 0x700d0000, configuration <configuration-string>)
//   SwitchUSB: functions registered, pull-up on (waiting for a host)
//   SwitchUSB: host connected (high speed)  /  bus suspended (host asleep or cable out)  /  host disconnected
//   SwitchPD: started (...)  /  one SwitchPD line per power-role decision (sink, classifying, dual-role, sink-only,
//   sourcing, not sourcing, detached), with the controller's ALERT_STATUS/STATUS1/STATUS2
//   SwitchUSB: XUSB firmware <version> (built <time>) running, HCSPARAMS1 <value>
//   SwitchUSB: host controller up (AppleUSBXHCI, <n> interrupters)
//   SwitchUSB: host role (a device on the port, VBUS on)  /  SwitchUSB: device role
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// From the board thread (IOSwitchRootDiag.inc), thread context. Returns at once: 0 when started on its own thread or
// not configured (neither ADT node), -1 if the thread could not be created.
int switch_usb_start(void);

// Prelink cut stub (emit_prelink.py CUT_DEPENDENCIES): 8 zero bytes; a cut dependency's metaclass GOT entry points here.
extern const unsigned long long ioswitch_prelink_cut_null;

// SwitchXUDC.cpp, from the role manager: a host's cable is in (its bus reset is due); USB2 pad 0 to or from the host.
void switchxudc_host_expected(void);
void switchxudc_set_host_owns_port(bool host);

#ifdef __cplusplus
}

class IOService;
// SwitchXUDC.cpp: loads IOUSBDeviceFamily and AppleUSBDeviceMux, registers SwitchXUDC, powers the XUDC and starts it
// on `provider`. False (with the reason in `why`) when device mode cannot run; the port is then left alone.
bool switchxudc_start(IOService *provider, char *why, size_t whySize);
// SwitchXHCI.cpp: powers the XUSB host, loads its firmware and Apple's host stack, starts SwitchXHCI. Thread context.
bool switchxhci_start(char *why, size_t whySize);
void switchxhci_set_host_role(bool host);  // USB2 pad 0's ID override (after switchxhci_start)
void switchxhci_expect_port_change(void);  // a device appeared in host role: arm the lost-interrupt check
#endif
