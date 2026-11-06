// SPDX-License-Identifier: GPL-2.0-only
#include "catalog.h"

#include <bdk.h>
#include <libs/fatfs/ff.h>
#include <string.h>

#define LEGACY_MANIFEST SWITCH_IMAGES_DIR "/install/manifest.bin"

static int file_size(void *context, const char *path, uint64_t *bytes)
{
    (void)context;
    FILINFO info;
    if (f_stat(path, &info) != FR_OK || (info.fattrib & AM_DIR)) {
        return 1;
    }
    *bytes = info.fsize;
    return 0;
}

// The manifest from the head of the image's first part.
static struct switch_image *load(const char *path)
{
    FIL file;
    if (f_open(&file, path, FA_READ) != FR_OK) {
        return NULL;
    }
    UINT size = f_size(&file) < SWITCH_IMAGE_BOOT_OFFSET ? (UINT)f_size(&file) : SWITCH_IMAGE_BOOT_OFFSET;
    uint8_t *raw = malloc(size);
    struct switch_image_extent *extents = malloc(SWITCH_IMAGE_MAX_EXTENTS * sizeof(*extents));
    struct switch_image *image = malloc(sizeof(*image));
    UINT got = 0;
    int error = !raw || !extents || !image || f_read(&file, raw, size, &got) != FR_OK || got != size ||
                switch_image_decode(raw, size, image, extents, SWITCH_IMAGE_MAX_EXTENTS);
    f_close(&file);
    if (error) {
        free(raw);
        free(extents);
        free(image);
        return NULL;
    }
    return image;
}

static void insert_sorted(struct catalog_entry *entries, uint32_t count, const char *name)
{
    uint32_t at = count;
    while (at && strcmp(entries[at - 1].name, name) > 0) {
        entries[at] = entries[at - 1];
        at--;
    }
    memset(&entries[at], 0, sizeof(entries[at]));
    strcpy(entries[at].path, SWITCH_IMAGES_DIR "/");
    strcat(entries[at].path, name);
    strcpy(entries[at].name, name);
    entries[at].name[strlen(name) - 4] = 0;
}

uint32_t catalog_scan(struct catalog_entry *entries)
{
    DIR dir;
    FILINFO info;
    uint32_t count = 0;
    FRESULT found = f_findfirst(&dir, &info, SWITCH_IMAGES_DIR, SWITCH_IMAGES_FIRST);
    while (found == FR_OK && info.fname[0] && count < SWITCH_IMAGES_MAX) {
        if (!(info.fattrib & AM_DIR) && sizeof(SWITCH_IMAGES_DIR) + strlen(info.fname) < SWITCH_IMAGE_PATH_BYTES) {
            insert_sorted(entries, count++, info.fname);
        }
        found = f_findnext(&dir, &info);
    }
    f_closedir(&dir);
    for (uint32_t i = 0; i < count; i++) {
        struct catalog_entry *e = &entries[i];
        e->image = load(e->path);
        e->state = switch_image_examine(e->image, e->path, file_size, NULL, &e->missing);
    }
    return count;
}

int catalog_legacy(void)
{
    uint64_t bytes = 0;
    return !file_size(NULL, LEGACY_MANIFEST, &bytes);
}

int catalog_read(void *context, const char *path, uint64_t at, void *buffer, uint32_t bytes)
{
    (void)context;
    FIL file;
    UINT got = 0;
    if (f_open(&file, path, FA_READ) != FR_OK) {
        return 1;
    }
    int error = f_lseek(&file, at) != FR_OK || f_read(&file, buffer, bytes, &got) != FR_OK || got != bytes;
    f_close(&file);
    return error;
}

int catalog_write(void *context, const char *path, uint64_t at, const void *buffer, uint32_t bytes)
{
    (void)context;
    FIL file;
    UINT put = 0;
    if (f_open(&file, path, at ? FA_WRITE | FA_OPEN_EXISTING : FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        return 1;
    }
    int error = f_lseek(&file, at) != FR_OK || f_write(&file, buffer, bytes, &put) != FR_OK || put != bytes;
    return f_close(&file) != FR_OK || error;
}
