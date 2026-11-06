#ifndef SWITCH_LOADER_BOOT_ARGS_H
#define SWITCH_LOADER_BOOT_ARGS_H
#include <stdint.h>

/* switchroot/ios/bootargs.txt as U-Boot leaves it (boot/u-boot-script.txt): the page is zeroed, then the file, at most
 * SWITCH_BOOT_ARGS_BYTES - 1 bytes, is read to its start when it exists, so the text always ends in a zero byte. */
#define SWITCH_BOOT_ARGS_PA    UINT64_C(0x8fe00000)
#define SWITCH_BOOT_ARGS_BYTES 0x1000u

/* arm64 boot_args (pexpert/arm64/boot.h): Video.v_display and CommandLine[1024]. */
#define SWITCH_BOOT_ARGS_VIDEO_DISPLAY 0x30u
#define SWITCH_BOOT_ARGS_COMMAND_LINE  0x6cu
#define SWITCH_COMMAND_LINE_BYTES      1024u

enum switch_boot_args_result {
    SWITCH_BOOT_ARGS_OK = 0,
    SWITCH_BOOT_ARGS_TOO_LONG = 1,
    SWITCH_BOOT_ARGS_NOT_ASCII = 2,
};

/* Appends the text's words to the zero-terminated command line, one space apart. A word starting with '#' starts a
 * comment that runs to the end of its line; a leading UTF-8 byte order mark is skipped. *verbose is set when one of
 * the words is -v. On failure the line is left as it was. */
int switch_boot_args_append(volatile char *line, const volatile char *text, uint32_t text_bytes, int *verbose);
#endif
