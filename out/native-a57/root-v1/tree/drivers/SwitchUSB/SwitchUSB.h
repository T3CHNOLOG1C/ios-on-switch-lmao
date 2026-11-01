// SPDX-License-Identifier: BSD-2-Clause-Patent
// C entry points of drivers/SwitchUSB (USB device mode on the Switch's USB-C port).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Starts USB once IOKit is up. Boot-arg switchusb: 0 off (default), 1 log the
// USB clock/power/pad state only (no writes), 2 CDC-ACM console gadget,
// 3 Apple stack (IOUSBDeviceFamily + AppleUSBDeviceMux: usbmuxd/iproxy).
// Mode 3 is driven by the XUDC interrupt (GIC SPI 44 = INTID 76) alone and
// panics if it cannot be attached or stops arriving. Mode 2 (debug console)
// still runs a 1 ms loop: it also pumps console output, which raises no
// interrupt.
// Returns 0 when started or not requested, -1 on failure; never fatal.
int switchusb_start(void);

// Console tap: copies console output into the gadget's ring. Callable from any
// context and before switchusb_start(); never blocks (drops on contention).
void switchusb_console_write(const char *bytes, int count);

#ifdef __cplusplus
}
#endif
