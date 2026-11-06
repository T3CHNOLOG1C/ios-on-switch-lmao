#include "block_backend_source.h"
#include <string.h>

#define SWITCH_BLOCK_BACKEND_MAGIC UINT64_C(0x535743484241434b)
#define SWITCH_BLOCK_INDEX_SECTORS 131072u
#define SWITCH_BLOCK_INDEX_SECONDS 120u
#define SWITCH_BLOCK_READ_SECONDS  5u

struct switch_block_backend {
    uint64_t magic;
    struct switch_block_allocator allocator;
    struct switch_block_source source;
    struct switch_split_indexed_disk disks[SWITCH_BLOCK_DISK_COUNT];
    uint32_t *maps[SWITCH_BLOCK_DISK_COUNT];
    size_t map_bytes[SWITCH_BLOCK_DISK_COUNT];
    struct switch_block_info info[SWITCH_BLOCK_DISK_COUNT];
    unsigned online;
};

static int healthy(struct switch_block_backend *backend)
{
    if (!backend || backend->magic != SWITCH_BLOCK_BACKEND_MAGIC || !backend->online)
        return 0;
    if (backend->source.ready(backend->source.context) != SWITCH_BLOCK_OK) {
        backend->online = 0;
        return 0;
    }
    return 1;
}

void switch_block_destroy(struct switch_block_backend *backend)
{
    if (!backend || backend->magic != SWITCH_BLOCK_BACKEND_MAGIC)
        return;
    struct switch_block_allocator allocator = backend->allocator;
    backend->online = 0;
    backend->magic = 0;
    for (unsigned i = 0; i < SWITCH_BLOCK_DISK_COUNT; i++)
        if (backend->maps[i])
            allocator.release(allocator.context, backend->maps[i], backend->map_bytes[i]);
    allocator.release(allocator.context, backend, sizeof(*backend));
}

int switch_block_create(const struct switch_block_allocator *allocator, struct switch_block_backend **out)
{
    if (!out)
        return SWITCH_BLOCK_INVALID;
    *out = NULL;
    if (!allocator || !allocator->allocate || !allocator->release)
        return SWITCH_BLOCK_INVALID;
    struct switch_block_source source = {0};
    int status = switch_files_block_source(&source);
    if (status != SWITCH_BLOCK_OK)
        return status;
    if (!source.volume || !source.volume->mounted || !source.ready || !source.begin || !source.end)
        return SWITCH_BLOCK_CORRUPT;
    if (source.ready(source.context) != SWITCH_BLOCK_OK)
        return SWITCH_BLOCK_NOT_READY;

    uint32_t entries[SWITCH_BLOCK_DISK_COUNT];
    size_t total = 0;
    for (unsigned i = 0; i < SWITCH_BLOCK_DISK_COUNT; i++) {
        const struct switch_block_source_disk *disk = &source.disks[i];
        unsigned label_bytes = 0;
        if (!disk->label)
            return SWITCH_BLOCK_CORRUPT;
        while (label_bytes < sizeof(((struct switch_block_info *)0)->label) && disk->label[label_bytes])
            label_bytes++;
        if (!label_bytes || label_bytes == sizeof(((struct switch_block_info *)0)->label))
            return SWITCH_BLOCK_CORRUPT;
        int error =
            switch_split_index_count(source.volume, disk->parts, disk->part_count, disk->size_bytes, &entries[i]);
        if (error || !entries[i] || entries[i] > (SWITCH_BLOCK_MAX_INDEX_BYTES - total) / sizeof(uint32_t))
            return SWITCH_BLOCK_CORRUPT;
        total += (size_t)entries[i] * sizeof(uint32_t);
    }
    struct switch_block_backend *backend = allocator->allocate(allocator->context, sizeof(*backend));
    if (!backend)
        return SWITCH_BLOCK_NOMEM;
    memset(backend, 0, sizeof(*backend));
    backend->magic = SWITCH_BLOCK_BACKEND_MAGIC;
    backend->allocator = *allocator;
    backend->source = source;
    for (unsigned i = 0; i < SWITCH_BLOCK_DISK_COUNT; i++) {
        backend->map_bytes[i] = (size_t)entries[i] * sizeof(uint32_t);
        backend->maps[i] = allocator->allocate(allocator->context, backend->map_bytes[i]);
        if (!backend->maps[i]) {
            switch_block_destroy(backend);
            return SWITCH_BLOCK_NOMEM;
        }
        backend->info[i].size_bytes = source.disks[i].size_bytes;
        size_t n = 0;
        do {
            backend->info[i].label[n] = source.disks[i].label[n];
        } while (source.disks[i].label[n++]);
    }
    status = source.begin(source.context, SWITCH_BLOCK_INDEX_SECTORS, SWITCH_BLOCK_INDEX_SECONDS);
    if (status != SWITCH_BLOCK_OK) {
        switch_block_destroy(backend);
        return status;
    }
    for (unsigned i = 0; i < SWITCH_BLOCK_DISK_COUNT; i++) {
        const struct switch_block_source_disk *disk = &source.disks[i];
        int error = switch_split_open_indexed(&backend->disks[i], source.volume, disk->parts, disk->part_count,
                                              disk->size_bytes, backend->maps[i], entries[i]);
        if (error) {
            status = error == SWITCH_FAT_IO ? SWITCH_BLOCK_IO : SWITCH_BLOCK_CORRUPT;
            break;
        }
    }
    int end_status = source.end(source.context);
    if (end_status != SWITCH_BLOCK_OK)
        status = end_status;
    if (status != SWITCH_BLOCK_OK || source.ready(source.context) != SWITCH_BLOCK_OK) {
        switch_block_destroy(backend);
        return status != SWITCH_BLOCK_OK ? status : SWITCH_BLOCK_NOT_READY;
    }
    backend->online = 1;
    *out = backend;
    return SWITCH_BLOCK_OK;
}

int switch_block_get_info(struct switch_block_backend *backend, unsigned index, struct switch_block_info *info)
{
    if (!info || index >= SWITCH_BLOCK_DISK_COUNT)
        return SWITCH_BLOCK_INVALID;
    memset(info, 0, sizeof(*info));
    if (!healthy(backend))
        return SWITCH_BLOCK_NOT_READY;
    *info = backend->info[index];
    return SWITCH_BLOCK_OK;
}

int switch_block_read(struct switch_block_backend *backend, unsigned index, uint64_t offset, void *destination,
                      uint32_t length)
{
    if (!destination || index >= SWITCH_BLOCK_DISK_COUNT || !length || length > SWITCH_BLOCK_MAX_TRANSFER_BYTES ||
        ((offset | length) & 511u))
        return SWITCH_BLOCK_INVALID;
    if (!healthy(backend))
        return SWITCH_BLOCK_NOT_READY;
    uint64_t size = backend->info[index].size_bytes;
    if (offset > size || length > size - offset)
        return SWITCH_BLOCK_INVALID;
    int status =
        backend->source.begin(backend->source.context, length / SWITCH_BLOCK_SECTOR_BYTES, SWITCH_BLOCK_READ_SECONDS);
    if (status != SWITCH_BLOCK_OK) {
        backend->online = 0;
        return status;
    }
    int error = switch_split_read_indexed(&backend->disks[index], offset, destination, length);
    int end_status = backend->source.end(backend->source.context);
    status = end_status != SWITCH_BLOCK_OK ? end_status
             : error == SWITCH_FAT_IO      ? SWITCH_BLOCK_IO
             : error                       ? SWITCH_BLOCK_CORRUPT
                                           : SWITCH_BLOCK_OK;
    if (status != SWITCH_BLOCK_OK)
        backend->online = 0;
    return status;
}
