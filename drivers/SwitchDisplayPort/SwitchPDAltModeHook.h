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

// SwitchPD's start, before its first look at the port: the engine's timer joins `loop`.
void switch_pd_alt_attach(IOService *owner, IOWorkLoop *loop);

// After SwitchPD has handled an ALERT (or looked at the port): `sink_session` is its sink phase with a source partner,
// `at_rest` that its request is not using COMMAND (settling, published or given up), `dock_power` that the source's
// PDOs are the Nintendo dock's signature.
void switch_pd_alt_alert(uint16_t alert, uint16_t status1, bool sink_session, bool at_rest, bool dock_power);

// SwitchPD's answer to "is your request at rest now", for the engine's timer.
extern "C" bool switch_pd_alt_at_rest(void);

#endif
