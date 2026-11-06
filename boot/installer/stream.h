// SPDX-License-Identifier: GPL-2.0-only
#ifndef SWITCH_INSTALL_STREAM_H
#define SWITCH_INSTALL_STREAM_H

#include "manifest.h"

struct switch_install_io {
    void *context;
    // The next `bytes` of the parts concatenated: calls read the source in order, one hash chunk at a time.
    int (*read)(void *context, void *buffer, uint32_t bytes);
    // 0 when `bytes` at `buffer` hash to the manifest's `chunk`th hash.
    int (*verify)(void *context, uint32_t chunk, const void *buffer, uint32_t bytes);
    int (*write)(void *context, uint64_t sector, uint32_t count, const void *buffer);
    void (*progress)(void *context, uint64_t done, uint64_t total);
};

// Every extent onto the partition at `first_sector`. `buffer` holds one hash chunk; `zeros` is `zero_bytes` of
// zeros. A chunk is read and checked whole before any of it is written, and the first must start with the image's
// container superblock. Stops at the first failure.
int switch_install_stream(const struct switch_image *image, const struct switch_install_io *io, uint64_t first_sector,
                          uint64_t partition_sectors, uint8_t *buffer, const uint8_t *zeros, uint32_t zero_bytes);

#endif
