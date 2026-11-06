// SPDX-License-Identifier: GPL-2.0-only
// XusbSuperSpeed.cpp (//drivers/SwitchUSB:superspeed): SS port 0's lane, from the role thread and the falcon mailbox.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// UPHY PLL0 locked (PLLE and PLL0 are the PCIe controller's; the lane needs them running).
bool switch_usb_ss_pll_locked(void);
// The lane up (defaults, mux out of IDDQ, port map, equalisation, clamps released) or down (clamped, VCORE down).
// Role thread. False when the pad controller cannot be mapped.
bool switch_usb_ss_lane(bool up);
// LFPS detection on the lane, as the falcon asks (DISABLE/ENABLE_SS_LFPS_DETECTION). Mailbox work loop.
void switch_usb_ss_lfps(bool enable);
// One clause for the role line: lane state, pad mux, port map, clamps, PLL lock.
void switch_usb_ss_describe(char *buffer, size_t size);

#ifdef __cplusplus
}
#endif
