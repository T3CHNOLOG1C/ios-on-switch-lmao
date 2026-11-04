// SPDX-License-Identifier: BSD-2-Clause-Patent
// Nintendo Switch OLED motion sensors (6-axis IMU on SPI4) as iPadOS's accelerometer and gyroscope HID services
// (SwitchMotion.cpp has the hardware and the iPadOS side).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Identifies the IMU, publishes its HID services (loads IOHIDFamily on first use) and waits for clients; the part
// samples only while one has a report interval set. Board thread, after TegraGPIO is up. Idempotent. Returns 0; every
// failure panics with a one-line reason.
int switch_motion_start(void);

// System sleep (drivers/SwitchSuspend's sleep and wake actions; thread context, both from the same thread). Suspend
// stops sampling, powers the part down and gates the SPI4 clock; resume clocks SPI4 again and restores whatever the
// clients had asked for. Neither touches I2C, so their place in the sequence is free. No-ops before
// switch_motion_start().
void switch_motion_suspend(void);
void switch_motion_resume(void);

#ifdef __cplusplus
}
#endif
