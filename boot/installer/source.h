// SPDX-License-Identifier: GPL-2.0-only
#ifndef SWITCH_INSTALL_SOURCE_H
#define SWITCH_INSTALL_SOURCE_H

#include "manifest.h"

// The image's stream: its .SII parts on the FAT, read in order from where source_seek put it.
struct source {
    const struct switch_image *image;
    uint32_t part;
    uint64_t offset;
    int open;
};

int source_seek(struct source *s, uint64_t at);
// stream.h's read: the next `bytes` of the stream, across parts.
int source_read(void *context, void *buffer, uint32_t bytes);
// The chunk's SHA-256 by the security engine, against the manifest's (the body's or a boot file's).
int source_verify(void *context, uint32_t chunk, const void *buffer, uint32_t bytes);
void source_end(struct source *s);

#endif
