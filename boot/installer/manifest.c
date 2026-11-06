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

static uint32_t chunks(uint64_t bytes)
{
    return (uint32_t)((bytes + SWITCH_IMAGE_HASH_CHUNK - 1) / SWITCH_IMAGE_HASH_CHUNK);
}

// The length of `path`'s folder (up to its last '/'), 0 when it has none.
static uint32_t folder_length(const char *path)
{
    uint32_t n = 0;
    for (uint32_t i = 0; path[i]; i++) {
        if (path[i] == '/') {
            n = i;
        }
    }
    return n;
}

static int same_folder(const char *a, const char *b)
{
    uint32_t n = folder_length(a);
    return n && n == folder_length(b) && !memcmp(a, b, n);
}

// A part or boot record: a FAT path in the image's one folder, its size, reserved bytes zero.
static int record(const uint8_t *p, const char *first_path, char *path, uint64_t *bytes)
{
    *bytes = le64(p + SWITCH_IMAGE_PATH_BYTES);
    return !ascii(p, SWITCH_IMAGE_PATH_BYTES, path) && path_ok(path) && *bytes &&
           all_zero(p + SWITCH_IMAGE_PATH_BYTES + 8, SWITCH_IMAGE_RECORD_BYTES - SWITCH_IMAGE_PATH_BYTES - 8) &&
           same_folder(path, first_path ? first_path : path);
}

static int boot_name_ok(const char *path)
{
    const char *name = path + folder_length(path) + 1;
    return strcmp(name, "manifest.bin") && strcmp(name, "bootargs.txt");
}

static int unique(const struct switch_image *image, const char *path, uint32_t parts, uint32_t boots)
{
    for (uint32_t j = 0; j < parts; j++) {
        if (!strcmp(path, image->parts[j].path)) {
            return 0;
        }
    }
    for (uint32_t j = 0; j < boots; j++) {
        if (!strcmp(path, image->boot[j].path)) {
            return 0;
        }
    }
    return 1;
}

static int records_ok(const uint8_t *raw, struct switch_image *image)
{
    uint64_t sum = 0;
    for (uint32_t i = 0; i < image->part_count; i++) {
        const uint8_t *p = raw + SWITCH_IMAGE_HEADER_BYTES + i * SWITCH_IMAGE_RECORD_BYTES;
        struct switch_image_part *part = &image->parts[i];
        if (!record(p, i ? image->parts[0].path : NULL, part->path, &part->bytes) ||
            part->bytes > SWITCH_IMAGE_PART_LIMIT ||
            part->bytes % (i + 1 < image->part_count ? SWITCH_IMAGE_HASH_CHUNK : image->block_size) ||
            !unique(image, part->path, i, 0)) {
            return 0;
        }
        sum += part->bytes;
    }
    uint32_t next_hash = image->hash_count;
    for (uint32_t i = 0; i < image->boot_count; i++) {
        const uint8_t *p = raw + SWITCH_IMAGE_HEADER_BYTES + (image->part_count + i) * SWITCH_IMAGE_RECORD_BYTES;
        struct switch_image_boot *boot = &image->boot[i];
        if (!record(p, image->parts[0].path, boot->path, &boot->bytes) || boot->bytes > SWITCH_IMAGE_BOOT_LIMIT ||
            !boot_name_ok(boot->path) || !unique(image, boot->path, image->part_count, i)) {
            return 0;
        }
        boot->first_hash = next_hash;
        next_hash += chunks(boot->bytes);
    }
    return sum == image->source_bytes && next_hash == image->hash_total;
}

// The boot records' sizes, read before the rest is checked: they decide how many hashes the manifest holds.
static uint32_t boot_hashes(const uint8_t *raw, uint32_t parts, uint32_t boots)
{
    uint32_t total = 0;
    for (uint32_t i = 0; i < boots; i++) {
        uint64_t bytes =
            le64(raw + SWITCH_IMAGE_HEADER_BYTES + (parts + i) * SWITCH_IMAGE_RECORD_BYTES + SWITCH_IMAGE_PATH_BYTES);
        if (!bytes || bytes > SWITCH_IMAGE_BOOT_LIMIT) {
            return UINT32_MAX;
        }
        total += chunks(bytes);
    }
    return total;
}

int switch_image_decode(const uint8_t *raw, uint32_t size, struct switch_image *image,
                        struct switch_image_extent *extents, uint32_t capacity)
{
    if (!raw || !image || !extents || size < SWITCH_IMAGE_HEADER_BYTES || memcmp(raw, "SWIMAGE1", 8) ||
        le32(raw + 8) != SWITCH_IMAGE_VERSION || le32(raw + 68) != SWITCH_IMAGE_HASH_CHUNK || le32(raw + 124)) {
        return 1;
    }
    uint32_t parts = le32(raw + 12), boots = le32(raw + 120), count = le32(raw + 28), block = le32(raw + 24);
    uint64_t source = le64(raw + 32);
    uint32_t records = SWITCH_IMAGE_HEADER_BYTES + (parts + boots) * SWITCH_IMAGE_RECORD_BYTES;
    if (!parts || parts > SWITCH_IMAGE_MAX_PARTS || !boots || boots > SWITCH_IMAGE_MAX_BOOT || !count ||
        count > SWITCH_IMAGE_MAX_EXTENTS || count > capacity || !source ||
        source > SWITCH_IMAGE_MAX_PARTS * SWITCH_IMAGE_PART_LIMIT || block < 4096 || block > 65536 ||
        (block & (block - 1)) || size < records) {
        return 1;
    }
    uint32_t hashes = chunks(source), boot = boot_hashes(raw, parts, boots);
    if (boot == UINT32_MAX ||
        size != records + count * SWITCH_IMAGE_EXTENT_BYTES + (hashes + boot) * SWITCH_IMAGE_HASH_BYTES) {
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
    image->boot_count = boots;
    image->extent_count = count;
    image->source_bytes = source;
    image->hash_count = hashes;
    image->hash_total = hashes + boot;
    image->write_bytes = le64(raw + 40);
    image->extents = extents;
    memcpy(image->container_uuid, raw + 48, 16);
    if (!image->partition_bytes || image->partition_bytes % block ||
        ascii(raw + 72, SWITCH_IMAGE_LABEL_BYTES, image->label) || !records_ok(raw, image)) {
        return 1;
    }
    const uint8_t *e = raw + records;
    for (uint32_t i = 0; i < count; i++, e += SWITCH_IMAGE_EXTENT_BYTES) {
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

int switch_image_in(const struct switch_image *image, const char *folder)
{
    const char *path = image->parts[0].path;
    uint32_t n = folder_length(path);
    if (!n || n != strlen(folder)) {
        return 0;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (lower(path[i]) != lower(folder[i])) {
            return 0;
        }
    }
    return 1;
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
