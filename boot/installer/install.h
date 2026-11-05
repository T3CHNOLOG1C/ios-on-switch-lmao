// SPDX-License-Identifier: GPL-2.0-only
#ifndef SWITCH_INSTALL_H
#define SWITCH_INSTALL_H
#include <stdint.h>
#define SWITCH_INSTALL_MAX_PARTS 128u
#define SWITCH_INSTALL_MAX_EXTENTS 8192u
#define SWITCH_INSTALL_PATH_BYTES 240u
#define SWITCH_INSTALL_CHUNK (1024u*1024u)
#define SWITCH_INSTALL_MAX_MANIFEST_BYTES (64u+SWITCH_INSTALL_MAX_PARTS*256u+SWITCH_INSTALL_MAX_EXTENTS*24u)
struct switch_install_part { char path[SWITCH_INSTALL_PATH_BYTES]; uint64_t bytes; };
struct switch_install_extent { uint64_t image_offset, source_offset, bytes; };
struct switch_install_image {
    uint64_t bytes, source_bytes, copy_bytes;
    uint32_t block_size, count, extent_count;
    uint8_t sha256[32];
    struct switch_install_part parts[SWITCH_INSTALL_MAX_PARTS];
    struct switch_install_extent extents[SWITCH_INSTALL_MAX_EXTENTS];
};
/* External manifests only, never an on-disk APFS header. Both versions have
 * 64-byte headers and count 256-byte parts: path[240], LE64 bytes, zero[8].
 * V1: SWCHIMG1, LE32 version=1/count, LE64 logical bytes, LE32 block/zero,
 * legacy SHA256[32] (not verified). It describes one dense image extent.
 * V2: SWCHIMG2, LE32 version=2/count, LE64 logical bytes, LE32 block/extents,
 * LE64 source bytes, zero[24]; append extents*24-byte records after parts:
 * LE64 image offset/source offset/bytes. All are APFS-block aligned. Image
 * and source ranges are independently sorted, disjoint and bounded. Source
 * offsets address the concatenated declared parts, which may contain gaps.
 * Only encoded ranges are copied; omitted target bytes remain untouched.
 * The caller must prove omissions are APFS-free; this decoder cannot do so. */
int switch_install_manifest(const void *, uint32_t, struct switch_install_image *);
int switch_install_nxsb(const struct switch_install_image *, const void *, uint32_t);
struct switch_install_io {
    void *context;
    int (*ready)(void *);
    int (*source)(void *, uint64_t offset, void *, uint32_t);
    int (*transfer)(void *, int write, uint64_t sector, uint32_t count, void *);
    int (*sync)(void *);
    void (*progress)(void *, uint64_t done, uint64_t total, int writing);
};
/* One source read and target write per chunk. Only the first APFS block is
 * validated; card programming completion is required without image hashing
 * or full target readback. Any failure stops before GPT publication. */
int switch_install_stream(const struct switch_install_image *, const struct switch_install_io *,
    uint64_t first_sector, uint64_t partition_sectors, void *source_buffer,
    uint32_t buffer_bytes);
#endif
