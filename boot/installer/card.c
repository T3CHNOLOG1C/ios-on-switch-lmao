// SPDX-License-Identifier: GPL-2.0-only
#include "card.h"

#include <bdk.h>
#include <string.h>

static uint64_t permit_first, permit_sectors;
static uint8_t check[SWITCH_GPT_TABLE_BYTES] __attribute__((aligned(64)));

void card_permit(uint64_t first, uint64_t sectors)
{
    permit_first = first;
    permit_sectors = sectors;
}

void card_permit_none(void)
{
    card_permit(0, 0);
}

int __real_sdmmc_storage_write(sdmmc_storage_t *storage, u32 sector, u32 count, void *buffer);

// Linked with --wrap=sdmmc_storage_write: the BDK's and FatFs's writes land here too.
int __wrap_sdmmc_storage_write(sdmmc_storage_t *storage, u32 sector, u32 count, void *buffer)
{
    if (storage != &sd_storage || !count || sector < permit_first || count > permit_sectors ||
        sector - permit_first > permit_sectors - count) {
        return 1;
    }
    return __real_sdmmc_storage_write(storage, sector, count, buffer);
}

int card_read(uint64_t sector, uint32_t count, void *buffer)
{
    if (sector > UINT32_MAX || count > UINT32_MAX - sector) {
        return 1;
    }
    return sdmmc_storage_read(&sd_storage, (u32)sector, count, buffer);
}

int card_write(uint64_t sector, uint32_t count, const void *buffer)
{
    if (sector > UINT32_MAX || count > UINT32_MAX - sector) {
        return 1;
    }
    return sdmmc_storage_write(&sd_storage, (u32)sector, count, (void *)buffer);
}

int card_read_gpt(struct switch_gpt *gpt)
{
    gpt->disk_sectors = sd_storage.sec_cnt;
    if (card_read(1, 1, gpt->primary) || card_read(gpt->disk_sectors - 1, 1, gpt->backup) || switch_gpt_headers(gpt) ||
        card_read(gpt->entries_lba, gpt->table_sectors, gpt->entries) ||
        card_read(gpt->backup_entries_lba, gpt->table_sectors, gpt->backup_entries)) {
        return 1;
    }
    return switch_gpt_entries(gpt);
}

static int write_checked(uint64_t lba, uint32_t sectors, const void *bytes)
{
    card_permit(lba, sectors);
    int error =
        card_write(lba, sectors, bytes) || card_read(lba, sectors, check) || memcmp(bytes, check, sectors * 512);
    card_permit_none();
    return error;
}

int card_write_gpt(const struct switch_gpt *gpt)
{
    return write_checked(gpt->backup_entries_lba, gpt->table_sectors, gpt->backup_entries) ||
           write_checked(gpt->disk_sectors - 1, 1, gpt->backup) ||
           write_checked(gpt->entries_lba, gpt->table_sectors, gpt->entries) || write_checked(1, 1, gpt->primary);
}
