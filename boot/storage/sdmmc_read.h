#ifndef SWITCH_SDMMC_READ_H
#define SWITCH_SDMMC_READ_H
#include <stdint.h>

/* Single-owner, read-only SDHCI PIO transport. No physical MMIO is selected by
 * this module. A board adapter must qualify the inherited selected SDHC/SDXC
 * card, map SDMMC1 as Device memory, and serialize all calls before using it. */
struct iosw_sd_io {
    uint8_t (*read8)(void *, uint32_t);
    uint16_t (*read16)(void *, uint32_t);
    uint32_t (*read32)(void *, uint32_t);
    void (*write16)(void *, uint32_t, uint16_t);
    void (*write32)(void *, uint32_t, uint32_t);
    uint64_t (*ticks)(void *);
    void (*barrier)(void *);
};
struct iosw_sd_host {
    struct iosw_sd_io io;
    void *context;
    uint64_t frequency;
    uint64_t capacity_sectors;
    uint64_t permitted_first_sector;
    uint64_t permitted_sector_count;
    /* 1: independently qualified SDHC/SDXC block addressing. 0: permit only
     * LBA zero with first=0/count=capacity=1; byte/block addressing coincide. */
    uint32_t block_addressing_verified;
    uint32_t blocked;
};
struct iosw_sd_result {
    uint32_t stage;
    uint32_t status;
    uint32_t response;
    uint32_t words_read;
    /* Exact samples used by the pre-command readiness decision. */
    uint32_t readiness_failures;
    uint32_t present;
    uint32_t version;
    uint32_t host_control;
    uint32_t power;
    uint32_t clock;
    uint32_t reset;
    uint32_t block_size;
    uint32_t initial_status;
};
enum iosw_sd_readiness_failure {
    IOSW_SD_READY_VERSION=1u<<0, IOSW_SD_READY_ACTIVE=1u<<1,
    IOSW_SD_READY_CARD=1u<<2, IOSW_SD_READY_BUS=1u<<3,
    IOSW_SD_READY_POWER=1u<<4, IOSW_SD_READY_CLOCK=1u<<5,
    IOSW_SD_READY_RESET=1u<<6, IOSW_SD_READY_BLOCK_SIZE=1u<<7
};
enum iosw_sd_error {
    IOSW_SD_OK=0, IOSW_SD_ARGUMENT=1, IOSW_SD_UNQUALIFIED=2,
    IOSW_SD_NOT_READY=3, IOSW_SD_PREEXISTING_ERROR=4,
    IOSW_SD_TIMEOUT=5, IOSW_SD_CONTROLLER_ERROR=6, IOSW_SD_CARD_ERROR=7,
    IOSW_SD_BUFFER_STATE=8, IOSW_SD_ALREADY_BLOCKED=9
};
/* Returns a full verified 512-byte sector or leaves output unchanged.
 * A post-command error blocks this host for the rest of the boot. This module
 * never attempts recovery, reset, reinitialization, erase or a media write. */
int iosw_sd_read_sector(struct iosw_sd_host *, uint64_t lba, uint8_t output[512],
                       struct iosw_sd_result *);
#endif
