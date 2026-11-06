// SPDX-License-Identifier: GPL-2.0-only
// The TV as iPadOS will see it; see SwitchExternalScreen.h.
#include "SwitchExternalScreen.h"

#include <IOKit/IOLib.h>

#include "TegraDisplayB.h"

void SwitchExternalScreen::attach(const struct switch_dp_mode_list &modes, unsigned chosen, TegraDisplayB *controller)
{
    mode = modes.modes[chosen];
    dcb = controller;
    frameEnds = 0;
    IOLog("SwitchDisplayPort: external screen %ux%u at %u.%03u Hz (%u modes in the EDID)\n", mode.hactive, mode.vactive,
          switch_dp_mode_refresh_mhz(&mode) / 1000, switch_dp_mode_refresh_mhz(&mode) % 1000, modes.count);
}

void SwitchExternalScreen::detach()
{
    dcb = nullptr;
}

void SwitchExternalScreen::frameEnd()
{
    ++frameEnds;
}
