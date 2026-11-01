// SPDX-License-Identifier: BSD-2-Clause-Patent
// Nintendo Switch OLED touch screen (STMicro FingerTip S FTM4 on I2C3 0x49) as an iPadOS multitouch digitizer.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Creates the touch HID device (loads IOHIDFamily on first use) and the driver, whose work loop then powers the
// panel up and takes its interrupt. Thread context, after bsd_autoconf() (IOKit, OSKext and the GIC up). Idempotent.
// Returns 0; every failure panics with a one-line reason.
int switch_touch_start(void);

// System sleep (drivers/SwitchSuspend), thread context with I2C up: panel off before SC7, brought up again after it.
// No-ops before switch_touch_start().
void switch_touch_suspend(void);
void switch_touch_resume(void);

#ifdef __cplusplus
}
#endif
