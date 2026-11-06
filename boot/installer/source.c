// SPDX-License-Identifier: GPL-2.0-only
#include "source.h"

#include <bdk.h>
#include <libs/fatfs/ff.h>
#include <string.h>

static FIL file;
static DWORD *link_map;
static int fast;

static void close_part(struct source *s)
{
    if (s->open) {
        f_close(&file);
        s->open = 0;
    }
    free(link_map);
    link_map = NULL;
}

// The part's cluster link map, so f_read_fast reads runs of contiguous clusters in one command. Without memory for
// one, plain f_read (a command per cluster) still works.
static void map_clusters(void)
{
    UINT cluster = file.obj.fs->csize * 512;
    DWORD entries = 2 + 2 * (DWORD)((f_size(&file) + cluster - 1) / cluster);
    link_map = malloc(entries * sizeof(DWORD));
    fast = 0;
    if (link_map) {
        link_map[0] = entries;
        file.cltbl = link_map;
        fast = f_lseek(&file, CREATE_LINKMAP) == FR_OK && f_lseek(&file, 0) == FR_OK;
        if (!fast) {
            file.cltbl = NULL;
            f_lseek(&file, 0);
        }
    }
}

int source_check(const struct switch_image *image)
{
    for (uint32_t i = 0; i < image->part_count; i++) {
        FILINFO info;
        if (f_stat(image->parts[i].path, &info) != FR_OK || info.fsize != image->parts[i].bytes) {
            return (int)i + 1;
        }
    }
    return 0;
}

int source_read(void *context, void *buffer, uint32_t bytes)
{
    struct source *s = context;
    if (!s->open || s->offset == s->image->parts[s->part].bytes) {
        if (s->open) {
            close_part(s);
            s->part++;
        }
        if (s->part >= s->image->part_count || f_open(&file, s->image->parts[s->part].path, FA_READ) != FR_OK) {
            return 1;
        }
        s->open = 1;
        s->offset = 0;
        map_clusters();
    }
    if (bytes > s->image->parts[s->part].bytes - s->offset) {
        return 1;
    }
    UINT got = bytes;
    FRESULT result = fast ? f_read_fast(&file, buffer, bytes) : f_read(&file, buffer, bytes, &got);
    if (result != FR_OK || got != bytes) {
        close_part(s);
        return 1;
    }
    s->offset += bytes;
    if (s->offset == s->image->parts[s->part].bytes && s->part + 1 == s->image->part_count) {
        close_part(s);
        s->part++;
    }
    return 0;
}

int source_verify(void *context, uint32_t chunk, const void *buffer, uint32_t bytes)
{
    struct source *s = context;
    uint8_t hash[SE_SHA_256_SIZE] __attribute__((aligned(4)));
    if (chunk >= s->image->hash_count || se_sha_hash_256_oneshot(hash, buffer, bytes)) {
        return 1;
    }
    return memcmp(hash, s->image->hashes + chunk * SWITCH_IMAGE_HASH_BYTES, SWITCH_IMAGE_HASH_BYTES) != 0;
}

void source_end(struct source *s)
{
    close_part(s);
}
