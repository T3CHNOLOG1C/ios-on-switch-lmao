// SPDX-License-Identifier: BSD-2-Clause-Patent
// The built-in display's backlight, display power and ambient light sensor for iPadOS 17 CoreBrightness
// (SwitchBacklight.cpp, SwitchALS.cpp; the panel itself is TegraPanel.h).
//
// Boot-args, all default off until proven on hardware:
//   switch-backlight=1  CoreBrightness's brightness reaches the panel (DCS 0x51). Without it the backlight service is
//                       still published (CoreBrightness finds its display) and records what it is asked for.
//   switch-sleep=1      display power: the render server's power requests turn the panel off and on and stop and
//                       restart the display controller's scanout.
//   switch-als=1        the BH1730FVC ambient light sensor as CoreBrightness's ALS (HID service 0xff00/0x0004).
#ifndef SWITCH_BRIGHTNESS_H
#define SWITCH_BRIGHTNESS_H
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

// Publishes the backlight service. Needs the original AppleARMPlatform kext loaded (it is a dependency of
// IOMobileGraphicsFamily, so call this after that family has loaded, e.g. at the end of the display probe). Thread
// context, idempotent; 0 on success, -1 if the original AppleARMBacklight class is not registered at the bound address
// or setup failed.
int switch_backlight_start(void);

// Display power, from the framebuffer when the render server asks for it (IOMobileFramebufferRequestPowerChange: 0 off,
// anything else on). Returns false (and does nothing) unless switch-sleep=1 and the backlight has started; the caller
// then keeps the original behaviour. Never blocks: the panel sequence (about 0.3 s each way) runs on the backlight's
// work loop, which calls native_legacy_display_set_scanout() below in the right order. Any thread context.
bool switch_display_power_request(bool on);

// False from the moment a power-off request is taken until the panel is lit again; for input drivers that must ignore
// the screen while it is dark. Lock-free, any context.
bool switch_display_is_on(void);

// Implemented by the display driver (NativeLegacyDisplay), weak here: stops (false) or restarts (true) display
// controller A's scanout and masks or unmasks its frame-end and vblank interrupts, under the display's work loop; returns
// true when done. Called from the backlight's work loop only: after the panel is off, and before it is turned on again.
bool native_legacy_display_set_scanout(bool on);

// Starts the ambient light sensor (switch-als=1; otherwise returns 0 and does nothing). Needs TegraI2C, the GIC and
// IOHIDFamily (switch_hid_create loads it); thread context, may sleep. Panics with the reason if the sensor does not
// answer. 0 on success.
int switch_als_start(void);

// The backlight tells the sensor when the panel goes dark or lights up: the sensor stops converting while it is dark.
void switch_als_set_active(bool active);

#ifdef __cplusplus
}
#endif
#endif
