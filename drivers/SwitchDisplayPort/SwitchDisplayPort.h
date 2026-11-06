// SPDX-License-Identifier: GPL-2.0-only
// SwitchDisplayPort's entry from the USB-C side: the BM92T36 alternate-mode engine (SwitchPDAltMode.cpp) reports the
// DisplayPort lanes its Configure gave the partner, or 0 when DisplayPort ended (detach, exit, failure).
#ifndef SWITCH_DISPLAY_PORT_H
#define SWITCH_DISPLAY_PORT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Any thread, never blocks (SwitchPD's work loop calls it): the last call wins, applied on SwitchDisplayPort's own work
// loop. `lanes` is 2 (pin assignment D) or 4 (C); `flipped` is the plug's orientation (CC2), for the log.
void switch_displayport_alt_mode(unsigned lanes, bool flipped);

#ifdef __cplusplus
}
#endif
#endif
