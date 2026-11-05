#ifndef SWITCH_SDMMC_READ_H
#define SWITCH_SDMMC_READ_H
#include <stdint.h>
#define IOSW_SD_MAX_READ_SECTORS  128u
#define IOSW_SD_MAX_WRITE_SECTORS 128u

/* Single-owner SDHCI transport (native SDMA, inert reference PIO). No physical MMIO is selected by
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
struct iosw_sd_dma;
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
    /* Appended write-only qualification; zero-initialized readers stay read-only.
     * The caller binds permit_write to immutable, exact file backing extents.
     * It must return exactly 1 for the ENTIRE requested range, before any I/O. */
    uint32_t rca;
    uint32_t card_write_protected;
    uint32_t write_cache_off_verified;
    int (*permit_write)(void *write_context, uint64_t lba, uint32_t count);
    void *write_context;
    /* Native boot requires qualified SDMA for every data phase. A missing or
     * failed binding is fatal; zero retains only the inert PIO test fixture. */
    struct iosw_sd_dma *dma;
    uint32_t dma_required;
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
    uint32_t stop_response;
    uint32_t auto_cmd_error;
    uint32_t words_written;
    uint32_t card_status;
    uint32_t scr_high;
    uint32_t scr_low;
    uint32_t cache_evidence;
};
enum iosw_sd_readiness_failure {
    IOSW_SD_READY_VERSION = 1u << 0,
    IOSW_SD_READY_ACTIVE = 1u << 1,
    IOSW_SD_READY_CARD = 1u << 2,
    IOSW_SD_READY_BUS = 1u << 3,
    IOSW_SD_READY_POWER = 1u << 4,
    IOSW_SD_READY_CLOCK = 1u << 5,
    IOSW_SD_READY_RESET = 1u << 6,
    IOSW_SD_READY_BLOCK_SIZE = 1u << 7
};
enum iosw_sd_error {
    IOSW_SD_OK = 0,
    IOSW_SD_ARGUMENT = 1,
    IOSW_SD_UNQUALIFIED = 2,
    IOSW_SD_NOT_READY = 3,
    IOSW_SD_PREEXISTING_ERROR = 4,
    IOSW_SD_TIMEOUT = 5,
    IOSW_SD_CONTROLLER_ERROR = 6,
    IOSW_SD_CARD_ERROR = 7,
    IOSW_SD_BUFFER_STATE = 8,
    IOSW_SD_ALREADY_BLOCKED = 9,
    IOSW_SD_WRITE_PROTECTED = 10,
    IOSW_SD_WRITE_DENIED = 11,
    IOSW_SD_CACHE_UNQUALIFIED = 12,
    IOSW_SD_CARD_STATE = 13
};
enum iosw_sd_cache_evidence {
    IOSW_SD_CACHE_UNKNOWN = 0,
    IOSW_SD_CACHE_NO_CMD48 = 1,
    IOSW_SD_CACHE_NO_PERF = 2,
    IOSW_SD_CACHE_UNSUPPORTED = 3,
    IOSW_SD_CACHE_OBSERVED_OFF = 4
};
/* Returns a full verified 512-byte sector or leaves output unchanged.
 * A post-command error blocks this host for the rest of the boot. This module
 * never attempts recovery, reset, reinitialization or erase. */
int iosw_sd_read_sector(struct iosw_sd_host *, uint64_t lba, uint8_t output[512], struct iosw_sd_result *);
/* Bounded CMD18 + Auto CMD12 for 2..128 sectors, CMD17 for one. The caller
 * serializes this host and provides PRIVATE staging for count*512 bytes. On
 * error that staging may contain a prefix and MUST be discarded in full; it
 * must never alias client-visible memory. Publish/copy only after IOSW_SD_OK.
 * No allocation, read-ahead, media writes, clock changes or recovery. */
int iosw_sd_read_sectors(struct iosw_sd_host *, uint64_t lba, uint32_t count, uint8_t *private_output,
                         struct iosw_sd_result *);
/* Read-only CMD13, CMD55/ACMD51 and, when advertised, bounded CMD48 queries.
 * Qualify only a known unsupported or observed-disabled optional write cache.
 * Unknown/enabled cache, unsupported extension layouts and write protection
 * refuse writes. No CMD49, cache enable/disable, flush or media write occurs.
 * This remains valid only while this transport is the card's exclusive owner. */
int iosw_sd_qualify_writes(struct iosw_sd_host *, struct iosw_sd_result *);
/* CMD24/CMD25 + Auto CMD12, at most 128 sectors, followed by DAT0 idle and
 * CMD13 READY_FOR_DATA/TRAN. Every media write requires permit_write == 1
 * before any register I/O. On failure media may contain a prefix; the host is
 * blocked with no automatic retry. The input must remain private and stable. */
int iosw_sd_write_sectors(struct iosw_sd_host *, uint64_t lba, uint32_t count, const uint8_t *private_input,
                          struct iosw_sd_result *);
/* With the cache proven off, DAT0 idle + CMD13 READY/TRAN is the durability
 * barrier. An enabled or unqualified cache can never be acknowledged here. */
int iosw_sd_sync(struct iosw_sd_host *, struct iosw_sd_result *);
#endif
