// SPDX-License-Identifier: BSD-2-Clause-Patent
// Portrait-native presentation (compile-time switch, default on; build with -DSWITCH_PORTRAIT_NATIVE=0 for the old way).
//
// The Switch OLED (Samsung AMS699VC01) is a portrait 720x1280 panel that the user always holds in landscape. With the
// switch on, iOS is given the panel as it is, 720x1280 portrait, the way a real iPad presents its portrait panel, and
// runs its landscape interface rotated into it itself (backboardd's device orientation). The display scans iOS's
// frames out unrotated (NativeScanout.h, NativeLegacyDisplay.cpp) and the touch screen reports panel coordinates
// (SwitchTouch). XNU's -v console, the console tape, SwitchDiag's banner and the panic screen are unchanged: they
// still draw landscape text rotated into the same portrait framebuffer (PE_Video v_rotate 3).
//
// Off: iOS is given a 1280x720 landscape screen (which it treats as a portrait device), the display rotates each
// frame with window A's column scan, and touch reports in that landscape frame.
//
// Geometry shared by both modes: landscape pixel (x, y), upright in handheld use, is panel row 1279 - x, column y.
#pragma once

#ifndef SWITCH_PORTRAIT_NATIVE
#define SWITCH_PORTRAIT_NATIVE 1  // 11-04 r7e: on (user: landscape-only). 0 restores the rotated 1280x720 path.
#endif
