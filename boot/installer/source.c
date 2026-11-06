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
static int open_part(struct source *s)
{
    if (f_open(&file, s->image->parts[s->part].path, FA_READ) != FR_OK) {
        return 1;
    }
    s->open = 1;
    UINT cluster = file.obj.fs->csize * 512;
    DWORD entries = 2 + 2 * (DWORD)((f_size(&file) + cluster - 1) / cluster);
    link_map = malloc(entries * sizeof(DWORD));
    fast = 0;
    if (link_map) {
        link_map[0] = entries;
        file.cltbl = link_map;
        fast = f_lseek(&file, CREATE_LINKMAP) == FR_OK;
        if (!fast) {
            file.cltbl = NULL;
        }
    }
    return f_lseek(&file, s->offset) != FR_OK;
}

int source_seek(struct source *s, uint64_t at)
{
    close_part(s);
    for (s->part = 0; s->part < s->image->part_count; s->part++) {
        if (at < s->image->parts[s->part].bytes) {
            s->offset = at;
            return 0;
        }
        at -= s->image->parts[s->part].bytes;
    }
    return 1;
}

int source_read(void *context, void *buffer, uint32_t bytes)
{
    struct source *s = context;
    uint8_t *out = buffer;
    while (bytes) {
        if (s->part >= s->image->part_count || (!s->open && open_part(s))) {
            close_part(s);
            return 1;
        }
        uint64_t left = s->image->parts[s->part].bytes - s->offset;
        UINT n = left < bytes ? (UINT)left : bytes;
        UINT got = n;
        FRESULT result = fast ? f_read_fast(&file, out, n) : f_read(&file, out, n, &got);
        if (result != FR_OK || got != n) {
            close_part(s);
            return 1;
        }
        out += n;
        bytes -= n;
        s->offset += n;
        if (s->offset == s->image->parts[s->part].bytes) {
            close_part(s);
            s->part++;
            s->offset = 0;
        }
    }
    return 0;
}

int source_verify(void *context, uint32_t chunk, const void *buffer, uint32_t bytes)
{
    struct source *s = context;
    uint8_t hash[SE_SHA_256_SIZE] __attribute__((aligned(4)));
    if (chunk >= s->image->hash_total || se_sha_hash_256_oneshot(hash, buffer, bytes)) {
        return 1;
    }
    return memcmp(hash, s->image->hashes + (size_t)chunk * SWITCH_IMAGE_HASH_BYTES, SWITCH_IMAGE_HASH_BYTES) != 0;
}

void source_end(struct source *s)
{
    close_part(s);
}
