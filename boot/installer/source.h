// SPDX-License-Identifier: GPL-2.0-only
#ifndef SWITCH_INSTALL_SOURCE_H
#define SWITCH_INSTALL_SOURCE_H

#include "manifest.h"

// The image's part files on the FAT, read in order (stream.h's read and verify).
struct source {
    const struct switch_image *image;
    uint32_t part;
    uint64_t offset;
    int open;
};

// 0 when every part exists at its size; else the 1-based number of the first that does not.
int source_check(const struct switch_image *image);
int source_read(void *context, void *buffer, uint32_t bytes);
// The chunk's SHA-256 by the security engine, against the manifest's.
int source_verify(void *context, uint32_t chunk, const void *buffer, uint32_t bytes);
void source_end(struct source *s);

#endif
