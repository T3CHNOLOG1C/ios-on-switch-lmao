// SPDX-License-Identifier: GPL-2.0-only
// Between SwitchPD (drivers/SwitchUSB/SwitchPD.cpp, the BM92T36's one owner) and the alternate-mode engine
// (SwitchPDAltMode.cpp), linked when DisplayPort or the dock's USB 3 is built in. Everything runs on SwitchPD's work
// loop; the engine uses the controller's COMMAND and VDM registers only while SwitchPD's own USB-PD request is at rest.
#ifndef SWITCH_PD_ALT_MODE_HOOK_H
#define SWITCH_PD_ALT_MODE_HOOK_H

#include <stdbool.h>
#include <stdint.h>

class IOService;
class IOWorkLoop;

// After SwitchPD has handled an ALERT (or looked at the port), on its work loop: `at_rest(pd)` says whether its USB-PD
// request leaves COMMAND free (settling, published or given up), now or whenever the engine's timer asks;
// `sink_session` is its sink phase with a source partner; `dock_power` that the contract it made is with a source
// carrying the Nintendo dock's PDO signature. The first call adds the engine's timer to `loop`.
void switch_pd_alt_alert(IOService *pd, IOWorkLoop *loop, bool (*at_rest)(const void *pd), uint16_t alert,
                         uint16_t status1, bool sink_session, bool dock_power);

#endif
