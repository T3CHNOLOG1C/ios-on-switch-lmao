// SPDX-License-Identifier: BSD-2-Clause-Patent
// USB 2.0 device mode on the Switch's USB-C port: the Tegra X1 XUSB device controller (XUDC) under Apple's own
// IOUSBDeviceFamily, so a Mac sees an iPad (usbmuxd: idevice*, iproxy, Finder, Xcode; ptpd photos; atc sync).
//
// On when the ADT has /arm-io/usb-device (scripts/package_raw_boot.py writes it, with the properties an iPad's
// usb-device node has: vendor-id, product-id, product-string, vendor-string, configuration-string); without the node
// this driver does nothing. The node is also the controller's provider: IOUSBDeviceFamily reads those properties
// from it, and configd's USBDeviceConfiguration plug-in picks the functions by configuration-string from
// /System/Library/AppleUSBDevice/USBDeviceConfiguration.plist ("unknownHardware" = AppleUSBMux + PTP).
//
// Needs com.apple.iokit.IOUSBDeviceFamily, com.apple.driver.usb.AppleUSBCommon and com.apple.driver.AppleUSBDeviceMux
// in the prelink set (the pass's emit_prelink.py NEW_ORIGINAL_COMPONENTS and prelink-command.json).
//
// Never blocks boot: bring-up runs on its own thread, and anything short of a working interrupt path (no node, rail,
// MMIO, PLL, kext or class registration) logs one line, "SwitchUSB: device mode off: <why>", and leaves the port
// alone. Interrupt-driven only (XUDC: GIC SPI 44, INTID 76); an interrupt that cannot be attached or does not reach
// the CPU panics with one line (SwitchXUDC.cpp).
//
// State lines (kernel log; all one-shot per state change):
//   SwitchUSB: device mode ready (XUDC at 0x700d0000, configuration <configuration-string>)
//   SwitchUSB: functions registered, pull-up on (waiting for a host)
//   SwitchUSB: host connected (high speed)
//   SwitchUSB: bus suspended (host asleep or cable out)   SwitchUSB: host disconnected
//
// Not yet: host mode and role switching (the BM92T36 PD controller, the BQ24193 OTG boost and the XUSB host
// controller with its falcon firmware); the untested starting points are SwitchPD.cpp, XusbHost.cpp, SwitchXHCI.cpp,
// UsbDmaMapper.cpp and SwitchDPOut.cpp in this directory, none of which is built.
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// From the board thread (IOSwitchRootDiag.inc), thread context. Returns at once: 0 when started on its own thread or
// not configured (no /arm-io/usb-device), -1 if the thread could not be created.
int switch_usb_start(void);

#ifdef __cplusplus
}

class IOService;
// SwitchXUDC.cpp: loads IOUSBDeviceFamily and AppleUSBDeviceMux, registers SwitchXUDC, powers the XUDC and starts it
// on `provider`. False (with the reason in `why`) when device mode cannot run; the port is then left alone.
bool switchxudc_start(IOService *provider, char *why, size_t whySize);
#endif
