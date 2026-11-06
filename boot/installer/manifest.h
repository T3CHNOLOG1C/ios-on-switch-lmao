// SPDX-License-Identifier: GPL-2.0-only
#ifndef SWITCH_INSTALL_MANIFEST_H
#define SWITCH_INSTALL_MANIFEST_H

#include <stdint.h>

// tools/install/manifest.py writes this layout and documents it; the checks here are the same as its check().
#define SWITCH_IMAGE_HEADER_BYTES 128u
#define SWITCH_IMAGE_PART_BYTES   256u
#define SWITCH_IMAGE_EXTENT_BYTES 24u
#define SWITCH_IMAGE_MAX_PARTS    64u
#define SWITCH_IMAGE_MAX_EXTENTS  65536u
#define SWITCH_IMAGE_PATH_BYTES   200u
#define SWITCH_IMAGE_LABEL_BYTES  56u
#define SWITCH_IMAGE_HASH_BYTES   32u
#define SWITCH_IMAGE_HASH_CHUNK   (8u << 20)
// Every part but the last is a whole number of hash chunks, so no chunk spans two files.
#define SWITCH_IMAGE_PART_LIMIT ((UINT64_C(4) << 30) - SWITCH_IMAGE_HASH_CHUNK)
#define SWITCH_IMAGE_ZERO       UINT64_MAX
#define SWITCH_IMAGE_MAX_HASHES ((uint32_t)(SWITCH_IMAGE_MAX_PARTS * SWITCH_IMAGE_PART_LIMIT / SWITCH_IMAGE_HASH_CHUNK))
#define SWITCH_IMAGE_MAX_BYTES                                                      \
    (SWITCH_IMAGE_HEADER_BYTES + SWITCH_IMAGE_MAX_PARTS * SWITCH_IMAGE_PART_BYTES + \
     SWITCH_IMAGE_MAX_EXTENTS * SWITCH_IMAGE_EXTENT_BYTES + SWITCH_IMAGE_MAX_HASHES * SWITCH_IMAGE_HASH_BYTES)

struct switch_image_part {
    char path[SWITCH_IMAGE_PATH_BYTES];
    uint64_t bytes;
};

struct switch_image_extent {
    uint64_t offset;
    uint64_t source;
    uint64_t bytes;
};

struct switch_image {
    uint64_t partition_bytes;
    uint64_t source_bytes;
    uint64_t write_bytes;
    uint32_t block_size;
    uint32_t part_count;
    uint32_t extent_count;
    uint32_t hash_count;
    uint8_t container_uuid[16];
    char label[SWITCH_IMAGE_LABEL_BYTES + 1];
    struct switch_image_part parts[SWITCH_IMAGE_MAX_PARTS];
    struct switch_image_extent *extents;
    // Into the raw manifest the image was decoded from, which must outlive it.
    const uint8_t *hashes;
};

uint32_t switch_crc32(uint32_t crc, const uint8_t *bytes, uint32_t size);

// 0, or nonzero for any manifest tools/install/manifest.py would not have written. `extents` holds `capacity`.
int switch_image_decode(const uint8_t *raw, uint32_t size, struct switch_image *image,
                        struct switch_image_extent *extents, uint32_t capacity);

// 0 when `block` is the image's container superblock: magic, checksum, block size, size and UUID.
int switch_image_check_superblock(const struct switch_image *image, const uint8_t *block, uint32_t bytes);

#endif
