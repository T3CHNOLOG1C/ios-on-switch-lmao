// SPDX-License-Identifier: BSD-2-Clause-Patent
// Waking the Switch OLED from the lock (display power) and the power journal. Sleep, the wake sources and the wake
// guarantee are described in SwitchWakeSources.h (the host-tested half); this is the kernel half (SwitchWake.cpp).
//
// The power journal: every sleep and wake step of SwitchBacklight (display power), SwitchSuspend (system sleep), the
// wake sources and the wake guarantee writes one line: os_log (the unified log; and the kernel log, which a saved panic
// keeps) and a RAM ring of the last 64. The ring is written to /private/var/ioswitch/power.log on Data (root 0644, the
// whole file, fsynced) after every display off, every wake guarantee and before system sleep, so the last steps before
// a forced power-off survive it: logd keeps minutes of the unified log in RAM (r7t's persisted log ended two minutes
// before the device died). The first write of a boot first copies the last boot's file to power.prev.log.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "SwitchWakeSources.h"

#ifdef __cplusplus
extern "C" {
#endif

// One journal line (printf format, no newline): "<uptime ms> <text>" to os_log and the ring. Thread context or a
// work-loop action; not from a primary interrupt filter.
void switch_power_journal(const char *format, ...) __attribute__((format(printf, 1, 2)));

// Writes the ring to power.log: on a kernel thread call (wait false: returns at once), or now (wait true: thread
// context, may block on the SD for tens of milliseconds; system sleep, before the SD suspends). Failures log one line.
void switch_power_journal_flush(bool wait);

// A wake source fired (SwitchWakeSources.h). With the panel lit it does nothing. With the panel off: one journal line,
// the Bluetooth Home also presses the console's Home button (SwitchButtons, Consumer Menu), and the wake guarantee arms
// (SwitchBacklight). At most one per source per 500 ms. Thread context or a work-loop action, never blocks.
void switch_wake_event(unsigned source);

// SwitchBacklight, as the panel goes off (false) and comes on (true): Bluetooth Home edges start afresh in each dark
// period. Any thread context.
void switch_wake_display(bool on);

// SwitchBluetooth, for every inbound HCI ACL packet while the panel is off: `acl` from its 4-byte header, the first
// `length` bytes (SWITCH_WAKE_BT_PEEK are enough). A Nintendo controller's Home press (an edge) is a SWITCH_WAKE_BT_HOME
// event. Its work loop; never blocks.
void switch_wake_bt_acl(const uint8_t *acl, size_t length);

// The wake guarantee fired (SwitchBacklight, which lights the panel): a stackshot of every task (kernel and user frames,
// wait info) is written to /private/var/ioswitch/wake-<UTC>.kcdata and, for `idevicecrashreport`, to mobile's
// CrashReporter as ioswitch-wake-<UTC>.kcdata. On a kernel thread call; returns at once. At most one per 60 s.
void switch_wake_stackshot(const char *why);

// SwitchButtons (SwitchButtons.cpp): one press and release of the console's Home button (Consumer Menu). Any context,
// never blocks; nothing before the buttons have started.
void switch_buttons_home_pulse(void);

// SwitchBacklight (SwitchBacklight.cpp): a wake source fired with the panel off; arms the wake guarantee. Any thread
// context, never blocks.
void switch_backlight_expect_wake(unsigned source);

#ifdef __cplusplus
}
#endif
