// SPDX-License-Identifier: GPL-2.0-only
#ifndef SWITCH_INSTALL_CATALOG_H
#define SWITCH_INSTALL_CATALOG_H

#include "images.h"

// An install image on the FAT: SWITCH_IMAGES_DIR/NAME.SII.001 and the parts its manifest names.
struct catalog_entry {
    char path[SWITCH_IMAGE_PATH_BYTES];
    char name[SWITCH_IMAGE_PATH_BYTES];
    enum switch_image_state state;
    const char *missing;
    // NULL when its manifest did not decode.
    struct switch_image *image;
};

// The images on the mounted FAT, at most SWITCH_IMAGES_MAX, by name; returns how many.
uint32_t catalog_scan(struct catalog_entry *entries);

// 1 when the FAT still holds an image as //:image laid it out before .SII files (switchroot/ios/install/).
int catalog_legacy(void);

// bootfiles.h's read_file and write_file, on the FAT.
int catalog_read(void *context, const char *path, uint64_t at, void *buffer, uint32_t bytes);
int catalog_write(void *context, const char *path, uint64_t at, const void *buffer, uint32_t bytes);

#endif
