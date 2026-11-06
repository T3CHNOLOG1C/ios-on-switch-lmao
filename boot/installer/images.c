// SPDX-License-Identifier: GPL-2.0-only
#include "images.h"

#include <stddef.h>

static int has_size(switch_file_size size, void *context, const char *path, uint64_t want)
{
    uint64_t bytes;
    return !size(context, path, &bytes) && bytes == want;
}

enum switch_image_state switch_image_examine(const struct switch_image *image, const char *folder,
                                             switch_file_size size, void *context, const char **missing)
{
    *missing = NULL;
    if (!image) {
        return SWITCH_IMAGE_UNREADABLE;
    }
    if (!switch_image_in(image, folder)) {
        return SWITCH_IMAGE_ELSEWHERE;
    }
    for (uint32_t i = 0; i < image->part_count; i++) {
        if (!has_size(size, context, image->parts[i].path, image->parts[i].bytes)) {
            *missing = image->parts[i].path;
            return SWITCH_IMAGE_MISSING;
        }
    }
    for (uint32_t i = 0; i < image->boot_count; i++) {
        if (!has_size(size, context, image->boot[i].path, image->boot[i].bytes)) {
            *missing = image->boot[i].path;
            return SWITCH_IMAGE_MISSING;
        }
    }
    return SWITCH_IMAGE_READY;
}

uint32_t switch_menu_first(const uint8_t *ready, uint32_t count)
{
    uint32_t i = 0;
    while (i < count && !ready[i]) {
        i++;
    }
    return i;
}

enum switch_menu switch_menu_press(const uint8_t *ready, uint32_t count, uint32_t *current, uint32_t keys)
{
    const uint32_t both = SWITCH_KEY_VOL_UP | SWITCH_KEY_VOL_DOWN;
    if ((keys & both) == both) {
        return SWITCH_MENU_CANCELLED;
    }
    if (*current >= count || !ready[*current]) {
        *current = switch_menu_first(ready, count);
        return SWITCH_MENU_MOVED;
    }
    if (keys & SWITCH_KEY_POWER) {
        return SWITCH_MENU_CHOSEN;
    }
    if (keys & both) {
        uint32_t step = keys & SWITCH_KEY_VOL_UP ? count - 1 : 1;
        uint32_t next = *current;
        do {
            next = (next + step) % count;
        } while (!ready[next]);
        *current = next;
    }
    return SWITCH_MENU_MOVED;
}

static char *digits(char *out, uint32_t value)
{
    char reversed[10];
    uint32_t n = 0;
    do {
        reversed[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (n) {
        *out++ = reversed[--n];
    }
    return out;
}

void switch_size_text(uint64_t bytes, char *out)
{
    uint64_t mib = bytes >> 20;
    if (mib < 1024) {
        out = digits(out, (uint32_t)mib);
        *out++ = ' ';
        *out++ = 'M';
    } else {
        uint64_t tenths = (bytes * 10 + (UINT64_C(1) << 29)) >> 30;
        out = digits(out, (uint32_t)(tenths / 10));
        *out++ = '.';
        *out++ = (char)('0' + tenths % 10);
        *out++ = ' ';
        *out++ = 'G';
    }
    *out++ = 'B';
    *out = 0;
}
