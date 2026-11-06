// SPDX-License-Identifier: GPL-2.0-only
// What iPadOS sees of the TV: the plug state, the modes it may pick from (TimingElements and friends, as AppleCLCD's
// external pipes publish them) and window A of display controller B scanning the surfaces Core Animation swaps to it.
// The IOMobileFramebufferLegacy instance that carries these to user space is README.md's next step; until it exists
// the screen is driven with the best mode and an empty (black) window.
#ifndef SWITCH_EXTERNAL_SCREEN_H
#define SWITCH_EXTERNAL_SCREEN_H

#include <stdint.h>

#include "SwitchDPModes.h"

class TegraDisplayB;

class SwitchExternalScreen {
public:
    // A trained link scanning `modes.modes[chosen]` on `dcb`.
    void attach(const struct switch_dp_mode_list &modes, unsigned chosen, TegraDisplayB *dcb);
    void detach();
    // Display controller B's frame end: the flip asked for before it is now scanned.
    void frameEnd();
    bool attached() const
    {
        return dcb != nullptr;
    }

private:
    TegraDisplayB *dcb = nullptr;
    struct switch_dp_mode mode = {};
    uint64_t frameEnds = 0;
};

#endif
