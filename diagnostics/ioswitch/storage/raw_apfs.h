#ifndef IOSWITCH_RAW_APFS_H
#define IOSWITCH_RAW_APFS_H
#include "block_backend.h"
#ifdef __cplusplus
extern "C" {
#endif

#define IOSW_RAW_APFS_MAX_PARTITIONS 128u
#define IOSW_RAW_APFS_SCRATCH_BYTES  65536u

/* Only USED entries from a fully validated GPT, type GUID in on-disk byte order.
 * The caller MUST validate both header/array CRCs and their agreement, usable
 * geometry, all partition bounds/overlaps and unique partition GUIDs first.
 * This backend independently checks supplied extent bounds/overlaps and
 * requires exactly one Apple APFS type. It does not authenticate GPT itself. */
struct iosw_raw_apfs_partition {
    uint64_t first_sector, sector_count;
    uint8_t type_guid[16];
};

/* Same bounded, serialized transport callbacks as the native Data source.
 * Every callback returns iosw_block_status. read/write transfer 1..128 sectors;
 * a failed transfer may have changed a prefix. qualify must bind this exact
 * permit callback and prove the SD cache disabled; sync is the real CMD13
 * readiness barrier. unbind clears that binding only for the matching owner.
 * Context and callbacks outlive the backend. No recovery/retries are allowed. */
struct iosw_raw_apfs_io {
    void *context;
    int (*ready)(void *);
    int (*begin)(void *, uint32_t sectors, uint32_t seconds);
    int (*end)(void *);
    void (*offline)(void *);
    int (*read)(void *, uint64_t lba, uint32_t sectors, uint8_t *private_bytes);
    int (*qualify)(void *, int (*permit)(void *, uint64_t, uint32_t), void *owner);
    void (*unbind)(void *, void *owner);
    int (*write)(void *, uint64_t lba, uint32_t sectors, const uint8_t *private_bytes);
    int (*sync)(void *);
};

/* Caller-owned fixed storage: zero before first open; do not modify fields.
 * One sleepable controller owner serializes open/read/write/sync/info/close,
 * including every other backend using the same SD host. No internal heap/lock. */
struct iosw_raw_apfs {
    uint64_t magic;
    struct iosw_raw_apfs_io io;
    uint64_t first_sector, partition_sectors, container_sectors;
    uint32_t block_size, online, bound;
    uint8_t container_uuid[16];
};

/* Pure standard-format check, also usable by an installer before confirmation.
 * Supply at least the NXSB's declared 4..64 KiB block. No I/O or mutation.
 * Checks bootstrap metadata/geometry only, not complete filesystem consistency. */
int iosw_raw_apfs_validate_nxsb(const void *block, uint32_t available_bytes, uint64_t partition_sector_count);

/* Scratch is private, at least SCRATCH_BYTES, and needed only during open.
 * NOT_READY can mean no APFS entry or an unavailable transport. Raw mode treats
 * either as fatal; this API does not choose or authorize a fallback backend.
 * Malformed/ambiguous entries or NXSB return CORRUPT.
 * Validates block-zero NXSB checksum/type/OID and bounded boot/checkpoint
 * geometry, not live checkpoints, volume identities or filesystem consistency.
 * expected_container_uuid is a portable content identity, never a card/GPT ID.
 * It must match the validated NXSB before qualification enables ANY write.
 * Writes are enabled only after validation and successful cache qualification.
 * A failed open is unusable; close it before disposing of its fixed storage. */
int iosw_raw_apfs_open(struct iosw_raw_apfs *, const struct iosw_raw_apfs_io *, uint64_t capacity_sectors,
                       const struct iosw_raw_apfs_partition *, uint32_t count,
                       const uint8_t expected_container_uuid[16], void *private_scratch, uint32_t scratch_bytes);
int iosw_raw_apfs_get_info(struct iosw_raw_apfs *, struct iosw_block_info *);
/* Exact, nonzero, 512-aligned <=64 KiB request. Read output and write input
 * MUST be private staging. Copy read bytes to clients only after OK; stage the
 * COMPLETE client write before calling. Any uncertain I/O offlines the shared
 * source; writes may have changed a prefix, so never retry the request. */
int iosw_raw_apfs_read(struct iosw_raw_apfs *, uint64_t offset, void *, uint32_t length);
int iosw_raw_apfs_write(struct iosw_raw_apfs *, uint64_t offset, const void *, uint32_t length);
int iosw_raw_apfs_sync(struct iosw_raw_apfs *);
/* Quiesce the shared owner and drain clients before closing. */
void iosw_raw_apfs_close(struct iosw_raw_apfs *);
#ifdef __cplusplus
}
#endif
#endif
