// SPDX-License-Identifier: GPL-2.0-only
// The BDK's GFX_INC: what its drivers print through. They print only on errors, which the installer's own screens
// report, so the console is muted.
#ifndef SWITCH_INSTALL_GFX_H
#define SWITCH_INSTALL_GFX_H

#include <utils/types.h>

typedef struct _gfx_con_t {
    bool mute;
} gfx_con_t;

extern gfx_con_t gfx_con;

void gfx_printf(const char *fmt, ...);

#define EPRINTF(text)
#define EPRINTFARGS(text, args...)
#define WPRINTF(text)
#define WPRINTFARGS(text, args...)

#endif
