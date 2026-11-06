// SPDX-License-Identifier: GPL-2.0-only
#ifndef SWITCH_INSTALL_BOOTFILES_H
#define SWITCH_INSTALL_BOOTFILES_H

#include "manifest.h"

// Where hekate's iOS entry (bootloader/ini/ios.ini, boot_prefixes) reads bl31.bin and bl33.bin.
#define SWITCH_BOOT_ENTRY        "switchroot/ios"
#define SWITCH_BOOT_TARGET_BYTES (SWITCH_IMAGE_PATH_BYTES + sizeof(SWITCH_BOOT_ENTRY))

struct switch_boot_io {
    void *context;
    // `bytes` of the image's stream from `at`.
    int (*read_stream)(void *context, uint64_t at, void *buffer, uint32_t bytes);
    // `bytes` of the file at `path`, from `at`.
    int (*read_file)(void *context, const char *path, uint64_t at, void *buffer, uint32_t bytes);
    // `bytes` into the file at `path` at `at`; `at` 0 starts the file afresh.
    int (*write_file)(void *context, const char *path, uint64_t at, const void *buffer, uint32_t bytes);
    // stream.h's verify: 0 when the bytes hash to the manifest's `chunk`th hash.
    int (*verify)(void *context, uint32_t chunk, const void *buffer, uint32_t bytes);
};

// The boot entry's copy of `boot`: SWITCH_BOOT_ENTRY/<its name>, SWITCH_BOOT_TARGET_BYTES at most.
void switch_boot_target(const struct switch_image_boot *boot, char *out);

// Every boot file read from the stream and checked against the manifest, nothing written. 0, or the 1-based number of
// the first that fails. `buffer` holds one hash chunk.
int switch_boot_check(const struct switch_image *image, const struct switch_boot_io *io, uint8_t *buffer);

// Every boot file copied into the boot entry, each chunk checked before it is written, then read back and checked
// again. 0, or the 1-based number of the first that fails.
int switch_boot_install(const struct switch_image *image, const struct switch_boot_io *io, uint8_t *buffer);

#endif
