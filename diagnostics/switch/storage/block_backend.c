#include "block_backend_source.h"
#include <string.h>
#include "verified_root.h"

#define IOSW_BLOCK_BACKEND_MAGIC UINT64_C(0x494f53574241434b)
#define IOSW_BLOCK_INDEX_SECTORS 131072u
#define IOSW_BLOCK_INDEX_SECONDS 120u
#define IOSW_BLOCK_READ_SECONDS  5u

struct iosw_block_backend {
    uint64_t magic;
    struct iosw_block_allocator allocator;
    struct iosw_block_source source;
    struct iosw_split_indexed_disk disks[IOSW_BLOCK_DISK_COUNT];
    uint32_t *maps[IOSW_BLOCK_DISK_COUNT];
    size_t map_bytes[IOSW_BLOCK_DISK_COUNT];
    struct iosw_block_info info[IOSW_BLOCK_DISK_COUNT];
    unsigned online;
};

static int healthy(struct iosw_block_backend *backend)
{
    if (!backend || backend->magic != IOSW_BLOCK_BACKEND_MAGIC || !backend->online) return 0;
    if (backend->source.ready(backend->source.context) != IOSW_BLOCK_OK) {
        backend->online = 0;
        return 0;
    }
    return 1;
}

void iosw_block_destroy(struct iosw_block_backend *backend)
{
    if (!backend || backend->magic != IOSW_BLOCK_BACKEND_MAGIC) return;
    struct iosw_block_allocator allocator = backend->allocator;
    backend->online = 0;
    backend->magic = 0;
    for (unsigned i = 0; i < IOSW_BLOCK_DISK_COUNT; i++)
        if (backend->maps[i]) allocator.release(allocator.context, backend->maps[i], backend->map_bytes[i]);
    allocator.release(allocator.context, backend, sizeof(*backend));
}

int iosw_block_create(const struct iosw_block_allocator *allocator, struct iosw_block_backend **out)
{
    if (!out) return IOSW_BLOCK_INVALID;
    *out = NULL;
    if (!allocator || !allocator->allocate || !allocator->release) return IOSW_BLOCK_INVALID;
    struct iosw_block_source source = {0};
    int status = switch_files_block_source(&source);
    if (status != IOSW_BLOCK_OK) return status;
    if (!source.volume || !source.volume->mounted || !source.ready || !source.begin || !source.end)
        return IOSW_BLOCK_CORRUPT;
    if (source.ready(source.context) != IOSW_BLOCK_OK) return IOSW_BLOCK_NOT_READY;

    uint32_t entries[IOSW_BLOCK_DISK_COUNT];
    size_t total = 0;
    for (unsigned i = 0; i < IOSW_BLOCK_DISK_COUNT; i++) {
        const struct iosw_block_source_disk *disk = &source.disks[i];
        unsigned label_bytes = 0;
        if (!disk->label) return IOSW_BLOCK_CORRUPT;
        while (label_bytes < sizeof(((struct iosw_block_info *)0)->label) && disk->label[label_bytes]) label_bytes++;
        if (!label_bytes || label_bytes == sizeof(((struct iosw_block_info *)0)->label)) return IOSW_BLOCK_CORRUPT;
        int error = iosw_split_index_count(source.volume, disk->parts, disk->part_count, disk->size_bytes, &entries[i]);
        if (error || !entries[i] || entries[i] > (IOSW_BLOCK_MAX_INDEX_BYTES - total) / sizeof(uint32_t))
            return IOSW_BLOCK_CORRUPT;
        total += (size_t)entries[i] * sizeof(uint32_t);
    }
    struct iosw_block_backend *backend = allocator->allocate(allocator->context, sizeof(*backend));
    if (!backend) return IOSW_BLOCK_NOMEM;
    memset(backend, 0, sizeof(*backend));
    backend->magic = IOSW_BLOCK_BACKEND_MAGIC;
    backend->allocator = *allocator;
    backend->source = source;
    for (unsigned i = 0; i < IOSW_BLOCK_DISK_COUNT; i++) {
        backend->map_bytes[i] = (size_t)entries[i] * sizeof(uint32_t);
        backend->maps[i] = allocator->allocate(allocator->context, backend->map_bytes[i]);
        if (!backend->maps[i]) {
            iosw_block_destroy(backend);
            return IOSW_BLOCK_NOMEM;
        }
        backend->info[i].size_bytes = source.disks[i].size_bytes;
        size_t n = 0;
        do {
            backend->info[i].label[n] = source.disks[i].label[n];
        } while (source.disks[i].label[n++]);
    }
    status = source.begin(source.context, IOSW_BLOCK_INDEX_SECTORS, IOSW_BLOCK_INDEX_SECONDS);
    if (status != IOSW_BLOCK_OK) {
        iosw_block_destroy(backend);
        return status;
    }
    for (unsigned i = 0; i < IOSW_BLOCK_DISK_COUNT; i++) {
        const struct iosw_block_source_disk *disk = &source.disks[i];
        int error = iosw_split_open_indexed(&backend->disks[i], source.volume, disk->parts, disk->part_count,
                                            disk->size_bytes, backend->maps[i], entries[i]);
        if (error) {
            status = error == IOSW_FAT_IO ? IOSW_BLOCK_IO : IOSW_BLOCK_CORRUPT;
            break;
        }
    }
    int end_status = source.end(source.context);
    if (end_status != IOSW_BLOCK_OK) status = end_status;
    if (status != IOSW_BLOCK_OK || source.ready(source.context) != IOSW_BLOCK_OK) {
        iosw_block_destroy(backend);
        return status != IOSW_BLOCK_OK ? status : IOSW_BLOCK_NOT_READY;
    }
    backend->online = 1;
    *out = backend;
    return IOSW_BLOCK_OK;
}

int iosw_block_get_info(struct iosw_block_backend *backend, unsigned index, struct iosw_block_info *info)
{
    if (!info || index >= IOSW_BLOCK_DISK_COUNT) return IOSW_BLOCK_INVALID;
    memset(info, 0, sizeof(*info));
    if (!healthy(backend)) return IOSW_BLOCK_NOT_READY;
    *info = backend->info[index];
    return IOSW_BLOCK_OK;
}

int iosw_block_read(struct iosw_block_backend *backend, unsigned index, uint64_t offset, void *destination,
                    uint32_t length)
{
    if (!destination || index >= IOSW_BLOCK_DISK_COUNT || !length || length > IOSW_BLOCK_MAX_TRANSFER_BYTES ||
        ((offset | length) & 511u))
        return IOSW_BLOCK_INVALID;
    if (!healthy(backend)) return IOSW_BLOCK_NOT_READY;
    uint64_t size = backend->info[index].size_bytes;
    if (offset > size || length > size - offset) return IOSW_BLOCK_INVALID;
    int status =
        backend->source.begin(backend->source.context, length / IOSW_BLOCK_SECTOR_BYTES, IOSW_BLOCK_READ_SECONDS);
    if (status != IOSW_BLOCK_OK) {
        backend->online = 0;
        return status;
    }
    int error = iosw_split_read_indexed(&backend->disks[index], offset, destination, length);
    int end_status = backend->source.end(backend->source.context);
    status = end_status != IOSW_BLOCK_OK ? end_status
             : error == IOSW_FAT_IO      ? IOSW_BLOCK_IO
             : error                     ? IOSW_BLOCK_CORRUPT
                                         : IOSW_BLOCK_OK;
    if (status != IOSW_BLOCK_OK) backend->online = 0;
    return status;
}

int iosw_block_read_verified(struct iosw_block_backend *backend, unsigned index, uint64_t offset, void *scratch,
                             uint32_t capacity, uint32_t length, const struct vr_view *view,
                             const struct vr_crypto *crypto)
{
    if (!scratch || index >= IOSW_BLOCK_DISK_COUNT || !length || length > IOSW_BLOCK_MAX_TRANSFER_BYTES ||
        ((offset | length) & 511u) || capacity < IOSW_BLOCK_VERIFIED_SCRATCH_BYTES)
        return IOSW_BLOCK_INVALID;
    if (!healthy(backend)) return IOSW_BLOCK_NOT_READY;
    uint64_t size = backend->info[index].size_bytes;
    if (offset > size || length > size - offset) return IOSW_BLOCK_INVALID;
    if (!view || !view->hashes || !crypto || !crypto->sha256 || view->image_bytes != size ||
        view->block_count != size / VR_BLOCK_BYTES + (size % VR_BLOCK_BYTES != 0)) {
        backend->online = 0;
        return IOSW_BLOCK_INTEGRITY;
    }
    uint64_t first = offset & ~((uint64_t)VR_BLOCK_BYTES - 1);
    uint64_t end = offset + length;
    uint64_t padding = (VR_BLOCK_BYTES - (end & (VR_BLOCK_BYTES - 1))) & (VR_BLOCK_BYTES - 1);
    if (padding > size - end) padding = size - end;
    uint64_t span64 = end + padding - first;
    if (!span64 || span64 > capacity || span64 > IOSW_BLOCK_VERIFIED_SCRATCH_BYTES || (span64 & 511u))
        return IOSW_BLOCK_INVALID;
    uint32_t span = (uint32_t)span64;
    int status =
        backend->source.begin(backend->source.context, span / IOSW_BLOCK_SECTOR_BYTES, IOSW_BLOCK_READ_SECONDS);
    if (status != IOSW_BLOCK_OK) {
        backend->online = 0;
        return status;
    }
    int error = iosw_split_read_indexed(&backend->disks[index], first, scratch, span);
    status = error == IOSW_FAT_IO ? IOSW_BLOCK_IO : error ? IOSW_BLOCK_CORRUPT : IOSW_BLOCK_OK;
    if (status == IOSW_BLOCK_OK) {
        for (uint32_t at = 0; at < span; at += VR_BLOCK_BYTES) {
            uint32_t n = span - at < VR_BLOCK_BYTES ? span - at : VR_BLOCK_BYTES;
            if (vr_verify_block(view, first / VR_BLOCK_BYTES + at / VR_BLOCK_BYTES, (uint8_t *)scratch + at, n,
                                crypto) != VR_OK) {
                status = IOSW_BLOCK_INTEGRITY;
                break;
            }
        }
    }
    /* Includes CPU hash time in the same absolute deadline, not a new budget. */
    int end_status = backend->source.end(backend->source.context);
    if (end_status != IOSW_BLOCK_OK) status = end_status;
    if (status != IOSW_BLOCK_OK)
        backend->online = 0;
    else
        memmove(scratch, (uint8_t *)scratch + (offset - first), length);
    return status;
}

int iosw_block_excludes_clusters(struct iosw_block_backend *backend, const struct iosw_fat_volume *volume,
                                 const uint32_t *clusters, uint32_t count)
{
    if (!healthy(backend) || volume != backend->source.volume || !clusters || !count) return IOSW_BLOCK_NOT_READY;
    for (uint32_t n = 0; n < count; n++)
        if (clusters[n] < 2 || clusters[n] - 2 >= volume->cluster_count || (n && clusters[n - 1] >= clusters[n]))
            return IOSW_BLOCK_CORRUPT;
    for (unsigned d = 0; d < IOSW_BLOCK_DISK_COUNT; d++) {
        uint32_t entries = (uint32_t)(backend->map_bytes[d] / sizeof(uint32_t));
        for (uint32_t n = 0; n < entries; n++) {
            uint32_t key = backend->maps[d][n], lo = 0, hi = count;
            while (lo < hi) {
                uint32_t mid = lo + (hi - lo) / 2;
                if (clusters[mid] < key)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            if (lo < count && clusters[lo] == key) return IOSW_BLOCK_CORRUPT;
        }
    }
    return IOSW_BLOCK_OK;
}

/* The caller holds the controller owner and an active metadata-read budget. */
int iosw_block_excludes_directories(struct iosw_block_backend *backend, struct iosw_fat_volume *volume,
                                    const uint32_t *clusters, uint32_t count)
{
    if (!healthy(backend) || volume != backend->source.volume || !clusters || !count) return IOSW_BLOCK_NOT_READY;
    for (unsigned d = 0; d < IOSW_BLOCK_DISK_COUNT; d++) {
        const struct iosw_block_source_disk *disk = &backend->source.disks[d];
        for (unsigned p = 0; p < disk->part_count; p++) {
            int error = iosw_fat_path_directories_exclude(volume, disk->parts[p].path, clusters, count);
            if (error) return error == IOSW_FAT_IO ? IOSW_BLOCK_IO : IOSW_BLOCK_CORRUPT;
        }
    }
    return IOSW_BLOCK_OK;
}
