// SPDX-License-Identifier: GPL-2.0-only
#include "catalog.h"

#include <bdk.h>
#include <libs/fatfs/ff.h>
#include <string.h>

#define MANIFEST_NAME "manifest.bin"

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

static struct switch_image *load(const char *folder)
{
    char path[SWITCH_IMAGE_PATH_BYTES + sizeof(MANIFEST_NAME)];
    strcpy(path, folder);
    strcat(path, "/" MANIFEST_NAME);
    FIL file;
    if (f_open(&file, path, FA_READ) != FR_OK) {
        return NULL;
    }
    uint32_t size = (uint32_t)f_size(&file);
    uint8_t *raw = f_size(&file) <= SWITCH_IMAGE_MAX_BYTES ? malloc(size) : NULL;
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

static void insert_sorted(struct catalog_entry *entries, uint32_t count, const char *folder)
{
    uint32_t at = count;
    while (at && strcmp(entries[at - 1].folder, folder) > 0) {
        entries[at] = entries[at - 1];
        at--;
    }
    memset(&entries[at], 0, sizeof(entries[at]));
    strcpy(entries[at].folder, folder);
}

uint32_t catalog_scan(struct catalog_entry *entries)
{
    DIR dir;
    FILINFO info;
    uint32_t count = 0;
    if (f_opendir(&dir, SWITCH_IMAGES_DIR) != FR_OK) {
        return 0;
    }
    while (count < SWITCH_IMAGES_MAX && f_readdir(&dir, &info) == FR_OK && info.fname[0]) {
        uint32_t n = strlen(info.fname);
        if (!(info.fattrib & AM_DIR) || info.fname[0] == '.' ||
            sizeof(SWITCH_IMAGES_DIR) + n + 1 > SWITCH_IMAGE_PATH_BYTES) {
            continue;
        }
        char folder[SWITCH_IMAGE_PATH_BYTES];
        strcpy(folder, SWITCH_IMAGES_DIR "/");
        strcat(folder, info.fname);
        insert_sorted(entries, count++, folder);
    }
    f_closedir(&dir);
    for (uint32_t i = 0; i < count; i++) {
        struct catalog_entry *e = &entries[i];
        e->image = load(e->folder);
        e->state = switch_image_examine(e->image, e->folder, file_size, NULL, &e->missing);
    }
    return count;
}

int catalog_legacy(void)
{
    uint64_t bytes;
    return !file_size(NULL, SWITCH_IMAGES_DIR "/" MANIFEST_NAME, &bytes);
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
