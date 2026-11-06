// SPDX-License-Identifier: GPL-2.0-only
#ifndef SWITCH_INSTALL_MANIFEST_H
#define SWITCH_INSTALL_MANIFEST_H

#include <stdint.h>

// tools/install/manifest.py writes this layout and documents it; the checks here are the same as its check().
// An image is one stream, split into NAME.SII.001, .002, ... only because FAT32 caps a file below 4 GiB: the manifest
// at 0, the boot files from SWITCH_IMAGE_BOOT_OFFSET, the container's bytes from SWITCH_IMAGE_BODY_OFFSET.
#define SWITCH_IMAGE_VERSION      3u
#define SWITCH_IMAGE_HEADER_BYTES 128u
#define SWITCH_IMAGE_RECORD_BYTES 256u
#define SWITCH_IMAGE_EXTENT_BYTES 24u
#define SWITCH_IMAGE_MAX_PARTS    64u
#define SWITCH_IMAGE_MAX_BOOT     8u
#define SWITCH_IMAGE_MAX_EXTENTS  65536u
#define SWITCH_IMAGE_PATH_BYTES   200u
#define SWITCH_IMAGE_LABEL_BYTES  40u
#define SWITCH_IMAGE_HASH_BYTES   32u
#define SWITCH_IMAGE_HASH_CHUNK   (8u << 20)
#define SWITCH_IMAGE_BOOT_OFFSET  SWITCH_IMAGE_HASH_CHUNK
#define SWITCH_IMAGE_BODY_OFFSET  (SWITCH_IMAGE_BOOT_OFFSET + (64u << 20))
// Every part but the last is a whole number of hash chunks, so no chunk spans two files.
#define SWITCH_IMAGE_PART_LIMIT ((UINT64_C(4) << 30) - SWITCH_IMAGE_HASH_CHUNK)
#define SWITCH_IMAGE_ZERO       UINT64_MAX
#define SWITCH_IMAGE_MAX_HASHES                                                                   \
    ((uint32_t)(SWITCH_IMAGE_MAX_PARTS * SWITCH_IMAGE_PART_LIMIT / SWITCH_IMAGE_HASH_CHUNK +      \
                (SWITCH_IMAGE_BODY_OFFSET - SWITCH_IMAGE_BOOT_OFFSET) / SWITCH_IMAGE_HASH_CHUNK + \
                SWITCH_IMAGE_MAX_BOOT))

struct switch_image_part {
    char path[SWITCH_IMAGE_PATH_BYTES];
    uint64_t bytes;
};

// A file of the version's hekate boot entry, inside the stream; installing the image copies it to switchroot/ios/.
struct switch_image_boot {
    char name[SWITCH_IMAGE_PATH_BYTES];
    uint64_t offset;
    uint64_t bytes;
    uint32_t first_hash;
};

struct switch_image_extent {
    uint64_t offset;
    uint64_t source;
    uint64_t bytes;
};

struct switch_image {
    // The container the image holds, and the smallest partition it may be fitted to.
    uint64_t container_bytes;
    uint64_t minimum_bytes;
    uint64_t source_bytes;
    uint64_t write_bytes;
    uint32_t manifest_bytes;
    uint32_t block_size;
    uint32_t part_count;
    uint32_t boot_count;
    uint32_t extent_count;
    // The body's chunk hashes come first in `hashes`; each boot file's follow from its first_hash, hash_total in all.
    uint32_t hash_count;
    uint32_t hash_total;
    uint8_t container_uuid[16];
    char label[SWITCH_IMAGE_LABEL_BYTES + 1];
    struct switch_image_part parts[SWITCH_IMAGE_MAX_PARTS];
    struct switch_image_boot boot[SWITCH_IMAGE_MAX_BOOT];
    struct switch_image_extent *extents;
    // Into the raw manifest the image was decoded from, which must outlive it.
    const uint8_t *hashes;
};

uint32_t switch_crc32(uint32_t crc, const uint8_t *bytes, uint32_t size);

// 0, or nonzero for any manifest tools/install/manifest.py would not have written. `raw` holds `available` bytes from
// the stream's start, at least the manifest's; `extents` holds `capacity`.
int switch_image_decode(const uint8_t *raw, uint32_t available, struct switch_image *image,
                        struct switch_image_extent *extents, uint32_t capacity);

// 1 when the image's first part is the file at `path` (FAT compares names without case).
int switch_image_named(const struct switch_image *image, const char *path);

// 0 when `block` is the image's container superblock: magic, checksum, block size, size and UUID.
int switch_image_check_superblock(const struct switch_image *image, const uint8_t *block, uint32_t bytes);

#endif
