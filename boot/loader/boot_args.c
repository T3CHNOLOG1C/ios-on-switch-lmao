#include "boot_args.h"

static int is_space(uint8_t c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

int switch_boot_args_append(volatile char *line, const volatile char *text, uint32_t text_bytes, int *verbose)
{
    uint32_t start = 0;
    while (start < SWITCH_COMMAND_LINE_BYTES && line[start]) {
        ++start;
    }
    if (start == SWITCH_COMMAND_LINE_BYTES) {
        return SWITCH_BOOT_ARGS_TOO_LONG;
    }
    uint32_t length = start, at = 0;
    if (text_bytes >= 3 && (uint8_t)text[0] == 0xef && (uint8_t)text[1] == 0xbb && (uint8_t)text[2] == 0xbf) {
        at = 3;
    }
    int result = SWITCH_BOOT_ARGS_OK, saw_verbose = 0;
    uint8_t c;
    while (result == SWITCH_BOOT_ARGS_OK && at < text_bytes && (c = (uint8_t)text[at]) != 0) {
        if (is_space(c)) {
            ++at;
            continue;
        }
        if (c == '#') {
            while (at < text_bytes && text[at] && text[at] != '\n') {
                ++at;
            }
            continue;
        }
        const uint32_t first = at;
        if (length) {
            line[length++] = ' ';
        }
        while (at < text_bytes && (c = (uint8_t)text[at]) != 0 && !is_space(c)) {
            if (c < 0x21 || c > 0x7e) {
                result = SWITCH_BOOT_ARGS_NOT_ASCII;
                break;
            }
            if (length + 1 >= SWITCH_COMMAND_LINE_BYTES) {
                result = SWITCH_BOOT_ARGS_TOO_LONG;
                break;
            }
            line[length++] = (char)c;
            ++at;
        }
        if (at - first == 2 && text[first] == '-' && text[first + 1] == 'v') {
            saw_verbose = 1;
        }
    }
    if (result != SWITCH_BOOT_ARGS_OK) {
        while (length > start) {
            line[--length] = 0;
        }
        return result;
    }
    line[length] = 0;
    *verbose = saw_verbose;
    return SWITCH_BOOT_ARGS_OK;
}
