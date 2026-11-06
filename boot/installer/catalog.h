// SPDX-License-Identifier: GPL-2.0-only
#ifndef SWITCH_INSTALL_CATALOG_H
#define SWITCH_INSTALL_CATALOG_H

#include "images.h"

// An install image on the FAT: a folder under SWITCH_IMAGES_DIR.
struct catalog_entry {
    char folder[SWITCH_IMAGE_PATH_BYTES];
    enum switch_image_state state;
    const char *missing;
    // NULL when its manifest did not decode.
    struct switch_image *image;
};

// The images on the mounted FAT, at most SWITCH_IMAGES_MAX, by folder name; returns how many.
uint32_t catalog_scan(struct catalog_entry *entries);

// 1 when the FAT still holds an image as //:image laid it out before one folder per kind (install/manifest.bin).
int catalog_legacy(void);

// bootfiles.h's read and write, on the FAT.
int catalog_read(void *context, const char *path, uint64_t at, void *buffer, uint32_t bytes);
int catalog_write(void *context, const char *path, uint64_t at, const void *buffer, uint32_t bytes);

#endif
