// SPDX-License-Identifier: GPL-2.0-only
#ifndef SWITCH_INSTALL_TARGET_H
#define SWITCH_INSTALL_TARGET_H

#include <stdint.h>

#define SWITCH_GPT_MAX_ENTRIES  128u
#define SWITCH_GPT_ENTRY_BYTES  128u
#define SWITCH_GPT_TABLE_BYTES  (SWITCH_GPT_MAX_ENTRIES * SWITCH_GPT_ENTRY_BYTES)
#define SWITCH_TARGET_MAX       8u
#define SWITCH_TARGET_MAX_BYTES (UINT64_C(1) << 40)

// Both copies of the card's GPT, as read: headers at LBA 1 and the last LBA, the entry arrays they point at.
struct switch_gpt {
    uint64_t disk_sectors;
    uint8_t primary[512];
    uint8_t backup[512];
    uint8_t entries[SWITCH_GPT_TABLE_BYTES];
    uint8_t backup_entries[SWITCH_GPT_TABLE_BYTES];
    uint64_t entries_lba;
    uint64_t backup_entries_lba;
    uint32_t entry_count;
    uint32_t table_sectors;
};

struct switch_target {
    uint32_t index;
    uint64_t first;
    uint64_t sectors;
    char name[37];
    int installed;
};

extern const uint8_t switch_gpt_apfs[16];
extern const uint8_t switch_gpt_linux[16];

// After reading the two headers: where the entry arrays are and how long (0, or nonzero for a header to refuse).
int switch_gpt_headers(struct switch_gpt *gpt);

// After reading both arrays: CRCs, the two copies equal, every used entry inside the disk, no overlaps.
int switch_gpt_entries(const struct switch_gpt *gpt);

// The partitions tools/install/target.py would choose from, never the FAT (the entry starting at `fat_first`) or
// emuMMC: the one APFS or "iOS"/"iOS-setup" entry if there is one, else every Linux "l4t", "userdata" or "UDA" entry.
// Returns the count, or -1 when there is more than one iOS entry.
int switch_targets(const struct switch_gpt *gpt, uint64_t fat_first, struct switch_target *out, uint32_t capacity);

// Entry `index`'s type and name in both arrays, both headers' CRCs recomputed. Nothing else changes.
void switch_gpt_set(struct switch_gpt *gpt, uint32_t index, const uint8_t type[16], const char *name);

#endif
