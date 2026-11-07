// SPDX-License-Identifier: BSD-2-Clause-Patent
// IOMobileFramebuffer's colour contract, as J120AP's AppleCLCD keeps it, on DC A's colour management unit: True Tone and
// Night Shift (CoreBrightness through QuartzCore: SetMatrix location 9, SetBrightnessCorrection), the accessibility
// filters (SwapSetColorMatrix location 2, SetParameter 3 for Reduce White Point, SetColorRemapMode for Classic Invert and
// Grayscale), SetContrast and the gamma table. The CMU sits after window blending, so window B's video gets the same
// transform. Only DC A's volatile registers change: CMU LUT1/LUT2/CSC and DISP_COLOR_CONTROL's CMU_ENABLE. With nothing
// asked for the CMU is bypassed and the picture is the one hekate set up.
//
// The IOMobileFramebuffer calls come from any thread. The tables change in the vertical blank after a frame end (the
// filter), and the enable bit latches at a frame boundary through GENERAL_ACT_REQ from the work loop (the action), after
// the tables it enables are written.
#pragma once
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOReturn.h>
#include "TegraDisplayColorMath.h"

class TegraDisplayColor {
public:
    bool init();
    void free();

    // Under the work-loop gate: DC A mapped, then unmapped.
    void attach(volatile uint32_t *dc);
    void detach();

    IOReturn getGammaTable(void *table);
    IOReturn setGammaTable(const void *table);
    IOReturn getMatrix(uint32_t location, void *matrix);
    IOReturn setMatrix(uint32_t location, const void *matrix);
    IOReturn setContrast(const void *contrast);
    IOReturn setColorRemapMode(uint32_t mode);
    IOReturn getColorRemapMode(uint32_t *mode);
    IOReturn setBrightnessCorrection(uint32_t correction);
    IOReturn swapSetColorMatrix(const void *matrix, uint32_t location, uint32_t flags);
    IOReturn setAccessibilityScale(uint32_t scale);
    void frameInfoNotify(void *client, bool enable);

    // DC A's frame end: the filter (primary interrupt context) writes changed tables, the action (work-loop gate) moves
    // the enable bit. True from changePending while either still has work, so the caller keeps the frame end coming.
    void frameEndFilter();
    void frameEndAction();
    bool changePending();

    // Under the gate, after tegra_display_power_up() reset DC A, before the resume's GENERAL_ACT_REQ: the tables and the
    // enable bit back, latching with that request.
    void restoreAfterPowerUp();

    // A panic's screen without the transform (a colour filter or a low white point could hide its text): the CMU bypassed
    // from the next frame. Stores only, from any context.
    static void bypassForPanic(volatile uint32_t *dc);

private:
    void commit();
    void writeTables(const switch_color::Tables &tables, const switch_color::Tables *written);
    void writeEnable(bool enable);

    IOSimpleLock *lock = nullptr;
    volatile uint32_t *dc = nullptr;
    uint32_t colorControl = 0;
    uint32_t generation = 0;
    uint32_t frameInfoClients = 0;
    bool pendingDirty = false, activeValid = false, hwEnabled = false;
    switch_color::Inputs inputs;
    uint32_t gammaTable[switch_color::kGammaWords];
    switch_color::Tables pending, active;
};
