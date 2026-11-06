// SPDX-License-Identifier: GPL-2.0-only
#include "bootfiles.h"

#include <string.h>

void switch_boot_target(const struct switch_image_boot *boot, char *out)
{
    const char *name = strrchr(boot->path, '/') + 1;
    memcpy(out, SWITCH_BOOT_ENTRY "/", sizeof(SWITCH_BOOT_ENTRY));
    strcpy(out + sizeof(SWITCH_BOOT_ENTRY), name);
}

static uint32_t chunk_bytes(const struct switch_image_boot *boot, uint64_t at)
{
    uint64_t left = boot->bytes - at;
    return left < SWITCH_IMAGE_HASH_CHUNK ? (uint32_t)left : SWITCH_IMAGE_HASH_CHUNK;
}

static int matches(const struct switch_image_boot *boot, const char *path, const struct switch_boot_io *io,
                   uint8_t *buffer)
{
    uint32_t chunk = boot->first_hash;
    for (uint64_t at = 0; at < boot->bytes; chunk++) {
        uint32_t n = chunk_bytes(boot, at);
        if (io->read(io->context, path, at, buffer, n) || io->verify(io->context, chunk, buffer, n)) {
            return 0;
        }
        at += n;
    }
    return 1;
}

int switch_boot_check(const struct switch_image *image, const struct switch_boot_io *io, uint8_t *buffer)
{
    for (uint32_t i = 0; i < image->boot_count; i++) {
        if (!matches(&image->boot[i], image->boot[i].path, io, buffer)) {
            return (int)i + 1;
        }
    }
    return 0;
}

static int copy(const struct switch_image_boot *boot, const char *target, const struct switch_boot_io *io,
                uint8_t *buffer)
{
    uint32_t chunk = boot->first_hash;
    for (uint64_t at = 0; at < boot->bytes; chunk++) {
        uint32_t n = chunk_bytes(boot, at);
        if (io->read(io->context, boot->path, at, buffer, n) || io->verify(io->context, chunk, buffer, n) ||
            io->write(io->context, target, at, buffer, n)) {
            return 0;
        }
        at += n;
    }
    return matches(boot, target, io, buffer);
}

int switch_boot_install(const struct switch_image *image, const struct switch_boot_io *io, uint8_t *buffer)
{
    char target[SWITCH_BOOT_TARGET_BYTES];
    for (uint32_t i = 0; i < image->boot_count; i++) {
        switch_boot_target(&image->boot[i], target);
        if (!copy(&image->boot[i], target, io, buffer)) {
            return (int)i + 1;
        }
    }
    return 0;
}
