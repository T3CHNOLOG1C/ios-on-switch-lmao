// SPDX-License-Identifier: GPL-2.0-only
#ifndef SWITCH_INSTALL_UI_H
#define SWITCH_INSTALL_UI_H

#include <stdint.h>

#define UI_WIDTH  1280u
#define UI_HEIGHT 720u
#define UI_WHITE  0xFFFFFFFFu
#define UI_GREY   0xFF8A8A8Eu
#define UI_BLACK  0xFF000000u
#define UI_RED    0xFFFF453Au
#define UI_GREEN  0xFF30D158u

// The panel is 720 x 1280 portrait; the installer draws 1280 x 720 landscape, upright in handheld.
void ui_init(uint32_t *framebuffer);
void ui_clear(void);
void ui_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
// Text at `scale` times the 8 x 8 font; returns the x after it.
uint32_t ui_text(uint32_t x, uint32_t y, uint32_t scale, uint32_t color, const char *text);
uint32_t ui_textf(uint32_t x, uint32_t y, uint32_t scale, uint32_t color, const char *fmt, ...);
void ui_progress(uint32_t y, uint64_t done, uint64_t total);

#endif
