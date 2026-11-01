// SPDX-License-Identifier: BSD-2-Clause-Patent
// Nintendo Switch OLED volume and power buttons as iPadOS consumer-control buttons.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Creates the buttons HID device (loads IOHIDFamily on first use) and the driver, and takes the volume GPIO and
// MAX77620 power-key interrupts. Thread context, after bsd_autoconf() (IOKit, OSKext and the GIC up). Idempotent.
// Returns 0; every failure panics with a one-line reason.
int switch_buttons_start(void);

#ifdef __cplusplus
}
#endif
