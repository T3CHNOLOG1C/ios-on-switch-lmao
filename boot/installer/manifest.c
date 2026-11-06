// SPDX-License-Identifier: GPL-2.0-only
#include "manifest.h"

#include <string.h>

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t le64(const uint8_t *p)
{
    return le32(p) | (uint64_t)le32(p + 4) << 32;
}

static int all_zero(const uint8_t *p, uint32_t n)
{
    while (n--) {
        if (*p++) {
            return 0;
        }
    }
    return 1;
}

uint32_t switch_crc32(uint32_t crc, const uint8_t *bytes, uint32_t size)
{
    crc = ~crc;
    while (size--) {
        crc ^= *bytes++;
        for (int bit = 0; bit < 8; bit++) {
            crc = crc >> 1 ^ (0xEDB88320u & (0u - (crc & 1)));
        }
    }
    return ~crc;
}

// A NUL-padded printable ASCII string of `size` bytes with at least one NUL.
static int ascii(const uint8_t *p, uint32_t size, char *out)
{
    uint32_t n = 0;
    while (n < size && p[n]) {
        if (p[n] < 32 || p[n] > 126) {
            return 1;
        }
        n++;
    }
    if (n == size || !all_zero(p + n, size - n)) {
        return 1;
    }
    memcpy(out, p, n);
    out[n] = 0;
    return 0;
}

// A FAT path the installer may open: relative, '/'-separated, no empty, "." or ".." component, no ':' or '\\'.
static int path_ok(const char *path)
{
    if (!path[0]) {
        return 0;
    }
    const char *component = path;
    for (const char *p = path;; p++) {
        if (*p == ':' || *p == '\\') {
            return 0;
        }
        if (*p == '/' || !*p) {
            uint32_t n = (uint32_t)(p - component);
            if (!n || (n == 1 && component[0] == '.') || (n == 2 && component[0] == '.' && component[1] == '.')) {
                return 0;
            }
            if (!*p) {
                return 1;
            }
            component = p + 1;
        }
    }
}

static int extents_ok(const struct switch_image *image)
{
    uint64_t block = image->block_size, end = 0, source = 0, written = 0;
    for (uint32_t i = 0; i < image->extent_count; i++) {
        const struct switch_image_extent *e = &image->extents[i];
        if (!e->bytes || e->offset % block || e->bytes % block || e->offset < end ||
            e->offset > image->partition_bytes || e->bytes > image->partition_bytes - e->offset) {
            return 0;
        }
        if (e->source == SWITCH_IMAGE_ZERO) {
            if (!i) {
                return 0;
            }
        } else {
            if (e->source != source || e->bytes > image->source_bytes - source) {
                return 0;
            }
            source += e->bytes;
        }
        end = e->offset + e->bytes;
        written += e->bytes;
    }
    return image->extents[0].offset == 0 && source == image->source_bytes && written == image->write_bytes;
}

int switch_image_decode(const uint8_t *raw, uint32_t size, struct switch_image *image,
                        struct switch_image_extent *extents, uint32_t capacity)
{
    if (!raw || !image || !extents || size < SWITCH_IMAGE_HEADER_BYTES || memcmp(raw, "SWIMAGE1", 8) ||
        le32(raw + 8) != 1 || le32(raw + 68) != SWITCH_IMAGE_HASH_CHUNK) {
        return 1;
    }
    uint32_t parts = le32(raw + 12), count = le32(raw + 28), block = le32(raw + 24);
    uint64_t source = le64(raw + 32), hashes = (source + SWITCH_IMAGE_HASH_CHUNK - 1) / SWITCH_IMAGE_HASH_CHUNK;
    if (!parts || parts > SWITCH_IMAGE_MAX_PARTS || !count || count > SWITCH_IMAGE_MAX_EXTENTS || count > capacity ||
        !hashes || hashes > SWITCH_IMAGE_MAX_HASHES || block < 4096 || block > 65536 || (block & (block - 1)) ||
        size != SWITCH_IMAGE_HEADER_BYTES + parts * SWITCH_IMAGE_PART_BYTES + count * SWITCH_IMAGE_EXTENT_BYTES +
                    (uint32_t)hashes * SWITCH_IMAGE_HASH_BYTES) {
        return 1;
    }
    uint8_t zeros[4] = {0};
    uint32_t crc = switch_crc32(0, raw, 64);
    crc = switch_crc32(crc, zeros, 4);
    crc = switch_crc32(crc, raw + 68, size - 68);
    if (crc != le32(raw + 64)) {
        return 1;
    }
    memset(image, 0, sizeof(*image));
    image->partition_bytes = le64(raw + 16);
    image->block_size = block;
    image->part_count = parts;
    image->extent_count = count;
    image->source_bytes = source;
    image->hash_count = (uint32_t)hashes;
    image->write_bytes = le64(raw + 40);
    image->extents = extents;
    memcpy(image->container_uuid, raw + 48, 16);
    if (!image->partition_bytes || image->partition_bytes % block ||
        ascii(raw + 72, SWITCH_IMAGE_LABEL_BYTES, image->label)) {
        return 1;
    }
    uint64_t sum = 0;
    for (uint32_t i = 0; i < parts; i++) {
        const uint8_t *p = raw + SWITCH_IMAGE_HEADER_BYTES + i * SWITCH_IMAGE_PART_BYTES;
        struct switch_image_part *part = &image->parts[i];
        part->bytes = le64(p + 200);
        if (ascii(p, SWITCH_IMAGE_PATH_BYTES, part->path) || !path_ok(part->path) || !part->bytes ||
            part->bytes > SWITCH_IMAGE_PART_LIMIT || part->bytes % (i + 1 < parts ? SWITCH_IMAGE_HASH_CHUNK : block) ||
            !all_zero(p + 208, 48)) {
            return 1;
        }
        for (uint32_t j = 0; j < i; j++) {
            if (!strcmp(part->path, image->parts[j].path)) {
                return 1;
            }
        }
        sum += part->bytes;
    }
    if (sum != image->source_bytes) {
        return 1;
    }
    const uint8_t *e = raw + SWITCH_IMAGE_HEADER_BYTES + parts * SWITCH_IMAGE_PART_BYTES;
    for (uint32_t i = 0; i < count; i++, e += SWITCH_IMAGE_EXTENT_BYTES) {
        extents[i].offset = le64(e);
        extents[i].source = le64(e + 8);
        extents[i].bytes = le64(e + 16);
    }
    image->hashes = e;
    return !extents_ok(image);
}

// APFS's object checksum: Fletcher-64 over 32-bit words, everything after the 8 checksum bytes.
static uint64_t fletcher64(const uint8_t *block, uint32_t bytes)
{
    const uint64_t mod = 0xFFFFFFFFu;
    uint64_t lo = 0, hi = 0;
    for (uint32_t at = 8; at < bytes; at += 4) {
        lo = (lo + le32(block + at)) % mod;
        hi = (hi + lo) % mod;
    }
    uint64_t c1 = mod - (lo + hi) % mod;
    uint64_t c2 = mod - (lo + c1) % mod;
    return c2 << 32 | c1;
}

int switch_image_check_superblock(const struct switch_image *image, const uint8_t *block, uint32_t bytes)
{
    if (!image || !block || bytes < image->block_size || memcmp(block + 32, "NXSB", 4)) {
        return 1;
    }
    if (le32(block + 36) != image->block_size || le64(block + 40) * image->block_size != image->partition_bytes ||
        memcmp(block + 72, image->container_uuid, 16)) {
        return 1;
    }
    return fletcher64(block, image->block_size) != le64(block);
}
