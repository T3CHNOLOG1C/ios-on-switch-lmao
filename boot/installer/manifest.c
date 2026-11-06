// SPDX-License-Identifier: GPL-2.0-only
#include "manifest.h"

#include <stddef.h>
#include <string.h>

#include "apfs.h"

#define BOOT_LIMIT (SWITCH_IMAGE_BODY_OFFSET - SWITCH_IMAGE_BOOT_OFFSET)

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
            crc = crc >> 1 ^ (0xEDB88320u & (0u - (crc & 1u)));
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
            size_t n = (size_t)(p - component);
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

// The length of `path`'s folder (up to its last '/'), 0 when it has none.
static size_t folder_length(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? (size_t)(slash - path) : 0;
}

static uint32_t chunks(uint64_t bytes)
{
    return (uint32_t)((bytes + SWITCH_IMAGE_HASH_CHUNK - 1) / SWITCH_IMAGE_HASH_CHUNK);
}

static const uint8_t *record(const uint8_t *raw, uint32_t index)
{
    return raw + SWITCH_IMAGE_HEADER_BYTES + (size_t)index * SWITCH_IMAGE_RECORD_BYTES;
}

static int extents_ok(const struct switch_image *image)
{
    uint64_t block = image->block_size;
    uint64_t end = 0;
    uint64_t source = 0;
    uint64_t written = 0;
    for (uint32_t i = 0; i < image->extent_count; i++) {
        const struct switch_image_extent *e = &image->extents[i];
        if (!e->bytes || e->offset % block || e->bytes % block || e->offset < end || e->offset > image->minimum_bytes ||
            e->bytes > image->minimum_bytes - e->offset) {
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

static int parts_ok(const uint8_t *raw, struct switch_image *image)
{
    uint64_t sum = 0;
    for (uint32_t i = 0; i < image->part_count; i++) {
        const uint8_t *p = record(raw, i);
        struct switch_image_part *part = &image->parts[i];
        part->bytes = le64(p + SWITCH_IMAGE_PATH_BYTES);
        int last = i + 1 == image->part_count;
        if (ascii(p, SWITCH_IMAGE_PATH_BYTES, part->path) || !path_ok(part->path) || !folder_length(part->path) ||
            !part->bytes || part->bytes > SWITCH_IMAGE_PART_LIMIT ||
            part->bytes % (last ? image->block_size : SWITCH_IMAGE_HASH_CHUNK) ||
            !all_zero(p + SWITCH_IMAGE_PATH_BYTES + 8, 48)) {
            return 0;
        }
        size_t folder = folder_length(image->parts[0].path);
        if (folder_length(part->path) != folder || memcmp(part->path, image->parts[0].path, folder)) {
            return 0;
        }
        for (uint32_t j = 0; j < i; j++) {
            if (!strcmp(part->path, image->parts[j].path)) {
                return 0;
            }
        }
        sum += part->bytes;
    }
    return sum == SWITCH_IMAGE_BODY_OFFSET + image->source_bytes && image->parts[0].bytes >= SWITCH_IMAGE_BODY_OFFSET;
}

static int boot_name_ok(const char *name)
{
    return name[0] && !strchr(name, '/') && strcmp(name, ".") && strcmp(name, "..") && strcmp(name, "manifest.bin") &&
           strcmp(name, "bootargs.txt");
}

static int boot_ok(const uint8_t *raw, struct switch_image *image)
{
    uint64_t end = SWITCH_IMAGE_BOOT_OFFSET;
    uint32_t next_hash = image->hash_count;
    for (uint32_t i = 0; i < image->boot_count; i++) {
        const uint8_t *p = record(raw, image->part_count + i);
        struct switch_image_boot *boot = &image->boot[i];
        boot->bytes = le64(p + SWITCH_IMAGE_PATH_BYTES);
        boot->offset = le64(p + SWITCH_IMAGE_PATH_BYTES + 8);
        if (ascii(p, SWITCH_IMAGE_PATH_BYTES, boot->name) || !boot_name_ok(boot->name) || !boot->bytes ||
            boot->offset < end || boot->offset > SWITCH_IMAGE_BODY_OFFSET ||
            boot->bytes > SWITCH_IMAGE_BODY_OFFSET - boot->offset || !all_zero(p + SWITCH_IMAGE_PATH_BYTES + 16, 40)) {
            return 0;
        }
        for (uint32_t j = 0; j < i; j++) {
            if (!strcmp(boot->name, image->boot[j].name)) {
                return 0;
            }
        }
        end = boot->offset + boot->bytes;
        boot->first_hash = next_hash;
        next_hash += chunks(boot->bytes);
    }
    return next_hash == image->hash_total;
}

// The boot records' sizes, read before the rest is checked: they decide how many hashes the manifest holds.
static uint32_t boot_hashes(const uint8_t *raw, uint32_t parts, uint32_t boots)
{
    uint32_t total = 0;
    for (uint32_t i = 0; i < boots; i++) {
        uint64_t bytes = le64(record(raw, parts + i) + SWITCH_IMAGE_PATH_BYTES);
        if (!bytes || bytes > BOOT_LIMIT) {
            return UINT32_MAX;
        }
        total += chunks(bytes);
    }
    return total;
}

static int header_ok(const uint8_t *raw, uint32_t available)
{
    return raw && available >= SWITCH_IMAGE_HEADER_BYTES && !memcmp(raw, "SWIMAGE1", 8) &&
           le32(raw + 8) == SWITCH_IMAGE_VERSION && le32(raw + 68) == SWITCH_IMAGE_HASH_CHUNK && !le32(raw + 124);
}

// The manifest's length from its counts, or 0 for counts no manifest has.
static uint32_t manifest_length(const uint8_t *raw, uint32_t available, uint32_t capacity)
{
    uint32_t parts = le32(raw + 12);
    uint32_t boots = le32(raw + 120);
    uint32_t count = le32(raw + 28);
    uint32_t block = le32(raw + 24);
    uint64_t source = le64(raw + 32);
    uint64_t records = SWITCH_IMAGE_HEADER_BYTES + (uint64_t)(parts + boots) * SWITCH_IMAGE_RECORD_BYTES;
    if (!parts || parts > SWITCH_IMAGE_MAX_PARTS || !boots || boots > SWITCH_IMAGE_MAX_BOOT || !count ||
        count > SWITCH_IMAGE_MAX_EXTENTS || count > capacity || !source ||
        source > SWITCH_IMAGE_MAX_PARTS * SWITCH_IMAGE_PART_LIMIT || block < 4096 || block > 65536 ||
        (block & (block - 1)) || available < records) {
        return 0;
    }
    uint32_t boot = boot_hashes(raw, parts, boots);
    uint64_t length = records + (uint64_t)count * SWITCH_IMAGE_EXTENT_BYTES +
                      ((uint64_t)chunks(source) + boot) * SWITCH_IMAGE_HASH_BYTES;
    if (boot == UINT32_MAX || length > available || length > SWITCH_IMAGE_BOOT_OFFSET) {
        return 0;
    }
    return (uint32_t)length;
}

static void read_header(const uint8_t *raw, struct switch_image *image, struct switch_image_extent *extents)
{
    memset(image, 0, sizeof(*image));
    image->container_bytes = le64(raw + 16);
    image->block_size = le32(raw + 24);
    image->part_count = le32(raw + 12);
    image->boot_count = le32(raw + 120);
    image->extent_count = le32(raw + 28);
    image->source_bytes = le64(raw + 32);
    image->write_bytes = le64(raw + 40);
    image->minimum_bytes = le64(raw + 112);
    image->hash_count = chunks(image->source_bytes);
    image->hash_total = image->hash_count + boot_hashes(raw, image->part_count, image->boot_count);
    image->extents = extents;
    memcpy(image->container_uuid, raw + 48, 16);
}

int switch_image_decode(const uint8_t *raw, uint32_t available, struct switch_image *image,
                        struct switch_image_extent *extents, uint32_t capacity)
{
    if (!image || !extents || !header_ok(raw, available)) {
        return 1;
    }
    uint32_t length = manifest_length(raw, available, capacity);
    if (!length) {
        return 1;
    }
    const uint8_t zeros[4] = {0};
    uint32_t crc = switch_crc32(0, raw, 64);
    crc = switch_crc32(crc, zeros, 4);
    crc = switch_crc32(crc, raw + 68, length - 68);
    if (crc != le32(raw + 64)) {
        return 1;
    }
    read_header(raw, image, extents);
    image->manifest_bytes = length;
    uint64_t block = image->block_size;
    if (!image->container_bytes || image->container_bytes % block || !image->minimum_bytes ||
        image->minimum_bytes % block || image->minimum_bytes > image->container_bytes ||
        ascii(raw + 72, SWITCH_IMAGE_LABEL_BYTES, image->label) || !parts_ok(raw, image) || !boot_ok(raw, image)) {
        return 1;
    }
    const uint8_t *e = record(raw, image->part_count + image->boot_count);
    for (uint32_t i = 0; i < image->extent_count; i++, e += SWITCH_IMAGE_EXTENT_BYTES) {
        extents[i].offset = le64(e);
        extents[i].source = le64(e + 8);
        extents[i].bytes = le64(e + 16);
    }
    image->hashes = e;
    return !extents_ok(image);
}

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

int switch_image_named(const struct switch_image *image, const char *path)
{
    const char *mine = image->parts[0].path;
    for (; *mine && *path; mine++, path++) {
        if (lower(*mine) != lower(*path)) {
            return 0;
        }
    }
    return !*mine && !*path;
}

int switch_image_check_superblock(const struct switch_image *image, const uint8_t *block, uint32_t bytes)
{
    if (!image || !block || bytes < image->block_size || memcmp(block + 32, "NXSB", 4)) {
        return 1;
    }
    if (le32(block + 36) != image->block_size || le64(block + 40) * image->block_size != image->container_bytes ||
        memcmp(block + 72, image->container_uuid, 16)) {
        return 1;
    }
    return switch_apfs_checksum(block, image->block_size) != le64(block);
}
