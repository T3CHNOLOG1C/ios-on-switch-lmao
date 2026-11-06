// SPDX-License-Identifier: GPL-2.0-only
#ifndef SWITCH_INSTALL_IMAGES_H
#define SWITCH_INSTALL_IMAGES_H

#include "manifest.h"

// One folder per kind (its IPSW build) holds that kind's image: tools/install/folder.py.
#define SWITCH_IMAGES_DIR   "switchroot/ios/install"
#define SWITCH_IMAGES_MAX   6u
#define SWITCH_SIZE_TEXT    16u
#define SWITCH_KEY_POWER    (1u << 0)
#define SWITCH_KEY_VOL_DOWN (1u << 1)
#define SWITCH_KEY_VOL_UP   (1u << 2)

enum switch_image_state {
    SWITCH_IMAGE_READY,
    // No manifest.bin, or one this installer refuses (made by another version of //:image, or damaged).
    SWITCH_IMAGE_UNREADABLE,
    // Its manifest names files outside its own folder.
    SWITCH_IMAGE_ELSEWHERE,
    // A part or boot file is missing or the wrong size.
    SWITCH_IMAGE_MISSING,
};

// 0 and the size of the file at `path`, or nonzero when there is no such file.
typedef int (*switch_file_size)(void *context, const char *path, uint64_t *bytes);

// What the image in `folder` is fit for: `image` is its decoded manifest, NULL when it did not decode. `*missing` is
// the first file that is not there at its size.
enum switch_image_state switch_image_examine(const struct switch_image *image, const char *folder,
                                             switch_file_size size, void *context, const char **missing);

enum switch_menu {
    SWITCH_MENU_MOVED,
    SWITCH_MENU_CHOSEN,
    SWITCH_MENU_CANCELLED,
};

// The first row that can be chosen, or `count` when none can.
uint32_t switch_menu_first(const uint8_t *ready, uint32_t count);

// One press of `keys` in a list of `count` rows: VOL+ and VOL- move `*current` to the previous or next row that is
// ready (wrapping), POWER chooses it, both volume buttons together cancel.
enum switch_menu switch_menu_press(const uint8_t *ready, uint32_t count, uint32_t *current, uint32_t keys);

// "59.6 GB" or "512 MB" into `out` (SWITCH_SIZE_TEXT bytes): binary units, as hekate's partition manager counts.
void switch_size_text(uint64_t bytes, char *out);

#endif
