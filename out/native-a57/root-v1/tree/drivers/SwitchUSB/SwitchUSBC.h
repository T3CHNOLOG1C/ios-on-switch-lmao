// SPDX-License-Identifier: BSD-2-Clause-Patent
// The USB-C port beyond device mode: host, roles, charging, SuperSpeed, DisplayPort alt mode.
//
// Boot-args (all default off; device mode stays switchusb=, SwitchUSB.h):
//   switch-usb-host=1  USB host on the USB-C port. The Tegra XUSB host controller runs Apple's own
//                      AppleUSBXHCI (SwitchXHCI.cpp); the BM92T36 PD controller decides data role
//                      (host/device) and power role (sink/source), negotiates the charging contract
//                      and the BQ24193 follows it (SwitchPD.cpp). The port's D+/D- move between the
//                      XUDC (device, switchusb=3) and the host by pad overrides.
//   switch-usb3=1      SuperSpeed on SS port 0 (UPHY lane pcie-5). Needs switch-usb-host=1.
//   switch-dp=1        DisplayPort alt mode to the dock or a USB-C DP adapter: PD VDMs, DPAUX1, SOR1,
//                      display controller B mirroring the internal display (SwitchDPOut.cpp). Runs the
//                      PD controller too; needs ioswitch-hw-rotate=1 (landscape scanout to mirror).
// Everything is interrupt-driven: the PD ALERT line (PK4), the xHCI host (INTID 71) and its falcon
// mailbox (INTID 72), DPAUX1 (INTID 43). A lost or missing interrupt panics with one line naming it.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Starts whatever the boot-args ask for, on its own thread (it sleeps: GIC, I2C, firmware, kexts).
// Call once IOKit is up, from thread context; next to switchusb_start(). Returns 0 when started or
// not requested, -1 if the thread could not be created.
int switch_usbc_start(void);

// SwitchBattery coordination. True when SwitchPD owns the BM92T36: then the PK4 ALERT pin and the
// clear-on-read ALERT_STATUS register are SwitchPD's (a second reader would steal its events), and
// SwitchBattery learns about plug/contract changes through the listener instead. The listener runs on
// SwitchPD's work loop after every change of attachment, power role or contract; keep it short.
bool switch_pd_owns_alert(void);
void switch_pd_set_power_listener(void (*listener)(void *context), void *context);

// Internal wiring between the units of libSwitchUSBHost.a (not for other drivers).
int switchxhci_start(bool superSpeed);       // SwitchXHCI.cpp: bring-up + AppleUSBXHCI; panics on a lost interrupt
void switchxhci_set_role(bool host);         // pad overrides + XUDC hand-over; callable before the host starts
void switch_dp_alt_mode(bool entered, unsigned lanes);   // SwitchDPOut.cpp, from SwitchPD's work loop
int switch_dp_start(void);                   // SwitchDPOut.cpp: DPAUX1 and its HPD interrupt

#ifdef __cplusplus
}
#endif
