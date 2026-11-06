// SPDX-License-Identifier: GPL-2.0-only
#include "target.h"

#include "manifest.h"

#include <string.h>

const uint8_t switch_gpt_apfs[16] = {0xef, 0x57, 0x34, 0x7c, 0x00, 0x00, 0xaa, 0x11,
                                     0xaa, 0x11, 0x00, 0x30, 0x65, 0x43, 0xec, 0xac};
const uint8_t switch_gpt_linux[16] = {0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
                                      0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4};
static const uint8_t emummc[16] = {0x00, 0x7e, 0xca, 0x11, 0x00, 0x00, 0x00, 0x00,
                                   0x00, 0x00, 0x65, 0x6d, 0x75, 0x4d, 0x4d, 0x43};

static const char *const installed_names[] = {"iOS", "iOS-setup"};
static const char *const fresh_names[] = {"l4t", "userdata", "UDA"};

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t le64(const uint8_t *p)
{
    return le32(p) | (uint64_t)le32(p + 4) << 32;
}

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
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

static uint32_t header_crc(const uint8_t *header)
{
    uint32_t size = le32(header + 12);
    uint8_t copy[512];
    memcpy(copy, header, size);
    memset(copy + 16, 0, 4);
    return switch_crc32(0, copy, size);
}

static int header_ok(const struct switch_gpt *gpt, const uint8_t *h, uint64_t lba, uint64_t other)
{
    uint32_t size = le32(h + 12);
    if (memcmp(h, "EFI PART", 8) || le32(h + 8) != 0x10000 || size < 92 || size > 512 || le32(h + 20) ||
        le64(h + 24) != lba || le64(h + 32) != other || le32(h + 84) != SWITCH_GPT_ENTRY_BYTES || !le32(h + 80) ||
        le32(h + 80) > SWITCH_GPT_MAX_ENTRIES || all_zero(h + 56, 16) || header_crc(h) != le32(h + 16)) {
        return 0;
    }
    uint64_t first = le64(h + 40), last = le64(h + 48);
    return first >= 2 && first <= last && last < gpt->disk_sectors - 1;
}

int switch_gpt_headers(struct switch_gpt *gpt)
{
    uint64_t last = gpt->disk_sectors - 1;
    if (gpt->disk_sectors < 68 || !header_ok(gpt, gpt->primary, 1, last) || !header_ok(gpt, gpt->backup, last, 1)) {
        return 1;
    }
    const uint8_t *p = gpt->primary, *b = gpt->backup;
    if (memcmp(p + 40, b + 40, 16) || memcmp(p + 56, b + 56, 16) || memcmp(p + 80, b + 80, 12)) {
        return 1;
    }
    gpt->entry_count = le32(p + 80);
    gpt->table_sectors = (gpt->entry_count * SWITCH_GPT_ENTRY_BYTES + 511) / 512;
    gpt->entries_lba = le64(p + 72);
    gpt->backup_entries_lba = le64(b + 72);
    uint64_t first_usable = le64(p + 40), last_usable = le64(p + 48);
    if (gpt->entries_lba < 2 || gpt->entries_lba + gpt->table_sectors > first_usable ||
        gpt->backup_entries_lba <= last_usable || gpt->backup_entries_lba + gpt->table_sectors > last) {
        return 1;
    }
    return 0;
}

int switch_gpt_entries(const struct switch_gpt *gpt)
{
    uint32_t bytes = gpt->entry_count * SWITCH_GPT_ENTRY_BYTES;
    if (switch_crc32(0, gpt->entries, bytes) != le32(gpt->primary + 88) ||
        memcmp(gpt->entries, gpt->backup_entries, bytes)) {
        return 1;
    }
    uint64_t first_usable = le64(gpt->primary + 40), last_usable = le64(gpt->primary + 48);
    for (uint32_t i = 0; i < gpt->entry_count; i++) {
        const uint8_t *e = gpt->entries + i * SWITCH_GPT_ENTRY_BYTES;
        if (all_zero(e, 16)) {
            continue;
        }
        uint64_t first = le64(e + 32), last = le64(e + 40);
        if (all_zero(e + 16, 16) || first < first_usable || last > last_usable || first > last) {
            return 1;
        }
        for (uint32_t j = 0; j < i; j++) {
            const uint8_t *o = gpt->entries + j * SWITCH_GPT_ENTRY_BYTES;
            if (!all_zero(o, 16) && (!memcmp(e + 16, o + 16, 16) || (first <= le64(o + 40) && le64(o + 32) <= last))) {
                return 1;
            }
        }
    }
    return 0;
}

static int named(const uint8_t *entry, const char *name)
{
    const uint8_t *field = entry + 56;
    uint32_t i = 0;
    for (; name[i]; i++) {
        if (i == 36 || field[2 * i] != (uint8_t)name[i] || field[2 * i + 1]) {
            return 0;
        }
    }
    return i == 36 || (!field[2 * i] && !field[2 * i + 1]);
}

static int named_any(const uint8_t *entry, const char *const *names, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        if (named(entry, names[i])) {
            return 1;
        }
    }
    return 0;
}

static void describe(const uint8_t *entry, uint32_t index, int installed, struct switch_target *out)
{
    out->index = index;
    out->first = le64(entry + 32);
    out->sectors = le64(entry + 40) - out->first + 1;
    out->installed = installed;
    uint32_t n = 0;
    for (; n < 36 && (entry[56 + 2 * n] || entry[57 + 2 * n]); n++) {
        uint8_t c = entry[56 + 2 * n];
        out->name[n] = entry[57 + 2 * n] || c < 32 || c > 126 ? '?' : (char)c;
    }
    out->name[n] = 0;
}

int switch_targets(const struct switch_gpt *gpt, uint64_t fat_first, struct switch_target *out, uint32_t capacity)
{
    int ios = -1;
    uint32_t fresh = 0;
    for (uint32_t i = 0; i < gpt->entry_count; i++) {
        const uint8_t *e = gpt->entries + i * SWITCH_GPT_ENTRY_BYTES;
        if (all_zero(e, 16) || le64(e + 32) == fat_first || !memcmp(e, emummc, 16)) {
            continue;
        }
        int apfs = !memcmp(e, switch_gpt_apfs, 16), linux_data = !memcmp(e, switch_gpt_linux, 16);
        if (apfs || named_any(e, installed_names, 2)) {
            if (ios >= 0) {
                return -1;
            }
            ios = (int)i;
        } else if (linux_data && named_any(e, fresh_names, 3) && fresh < capacity) {
            describe(e, i, 0, &out[fresh++]);
        }
    }
    if (ios >= 0) {
        const uint8_t *e = gpt->entries + (uint32_t)ios * SWITCH_GPT_ENTRY_BYTES;
        if (memcmp(e, switch_gpt_apfs, 16) && memcmp(e, switch_gpt_linux, 16)) {
            return 0;
        }
        describe(e, (uint32_t)ios, 1, &out[0]);
        return 1;
    }
    return (int)fresh;
}

void switch_gpt_set(struct switch_gpt *gpt, uint32_t index, const uint8_t type[16], const char *name)
{
    uint8_t *entry = gpt->entries + index * SWITCH_GPT_ENTRY_BYTES;
    memcpy(entry, type, 16);
    memset(entry + 56, 0, 72);
    for (uint32_t i = 0; name[i] && i < 36; i++) {
        entry[56 + 2 * i] = (uint8_t)name[i];
    }
    memcpy(gpt->backup_entries + index * SWITCH_GPT_ENTRY_BYTES, entry, SWITCH_GPT_ENTRY_BYTES);
    uint32_t table_crc = switch_crc32(0, gpt->entries, gpt->entry_count * SWITCH_GPT_ENTRY_BYTES);
    put32(gpt->primary + 88, table_crc);
    put32(gpt->backup + 88, table_crc);
    put32(gpt->primary + 16, header_crc(gpt->primary));
    put32(gpt->backup + 16, header_crc(gpt->backup));
}
