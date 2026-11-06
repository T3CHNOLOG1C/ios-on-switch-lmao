// SPDX-License-Identifier: GPL-2.0-only
// install-ios.bin: writes the install image `bazel run //:image` put on the FAT onto the partition it was made for.
#include <bdk.h>
#include <libs/fatfs/ff.h>
#include <string.h>

#include "card.h"
#include "manifest.h"
#include "source.h"
#include "stream.h"
#include "target.h"
#include "ui.h"

#define INSTALL_DIR "switchroot/ios/install"
#define MANIFEST    INSTALL_DIR "/manifest.bin"
#define ZERO_BYTES  (1u << 20)
#define MARGIN      80u

gfx_con_t gfx_con;

void gfx_printf(const char *fmt, ...)
{
    (void)fmt;
}

extern void pivot_stack(u32 stack_top);

static struct switch_image image;
static struct switch_gpt *gpt;
static struct switch_target targets[SWITCH_TARGET_MAX];
static uint32_t line_y, last_progress;

static void screen(const char *title)
{
    ui_clear();
    ui_text(MARGIN, 64, 5, UI_WHITE, title);
    if (image.label[0]) {
        ui_text(MARGIN, 124, 2, UI_GREY, image.label);
    }
    line_y = 200;
}

#define say(color, ...)                                  \
    do {                                                 \
        ui_textf(MARGIN, line_y, 3, color, __VA_ARGS__); \
        line_y += 40;                                    \
    } while (0)

static u8 press(void)
{
    while (btn_read() & (BTN_POWER | BTN_VOL_UP | BTN_VOL_DOWN)) {
        msleep(10);
    }
    return btn_wait();
}

static uint32_t mib(uint64_t sectors)
{
    return (uint32_t)(sectors >> 11);
}

static int load_image(void)
{
    FIL file;
    if (f_open(&file, MANIFEST, FA_READ) != FR_OK) {
        screen("No install image");
        say(UI_WHITE, "This card has no %s.", MANIFEST);
        say(UI_GREY, "On your Mac, run  bazelisk run //:image");
        say(UI_GREY, "and copy its folder onto the card.");
        return 1;
    }
    uint32_t size = (uint32_t)f_size(&file);
    uint8_t *raw = size <= SWITCH_IMAGE_MAX_BYTES ? malloc(size) : NULL;
    struct switch_image_extent *extents = malloc(SWITCH_IMAGE_MAX_EXTENTS * sizeof(*extents));
    UINT got = 0;
    int error = !raw || !extents || f_read(&file, raw, size, &got) != FR_OK || got != size ||
                switch_image_decode(raw, size, &image, extents, SWITCH_IMAGE_MAX_EXTENTS);
    f_close(&file);
    if (error) {
        screen("Damaged install image");
        say(UI_WHITE, "%s cannot be read.", MANIFEST);
        say(UI_GREY, "Copy the folder from your Mac onto the card again.");
        return 1;
    }
    int missing = source_check(&image);
    if (missing) {
        screen("Incomplete install image");
        say(UI_WHITE, "%s is missing or the wrong size.", image.parts[missing - 1].path);
        say(UI_GREY, "Copy the folder from your Mac onto the card again.");
        return 1;
    }
    return 0;
}

static const struct switch_target *choose(int count)
{
    if (count == 1) {
        return &targets[0];
    }
    uint32_t current = 0;
    for (;;) {
        screen("Choose a partition");
        say(UI_GREY, "Which partition becomes iOS?");
        line_y += 20;
        for (int i = 0; i < count; i++) {
            say(i == (int)current ? UI_WHITE : UI_GREY, "%s %s  (%d MB)", i == (int)current ? ">" : " ",
                targets[i].name, mib(targets[i].sectors));
        }
        line_y += 20;
        say(UI_GREY, "VOL+/VOL- move    POWER choose    both: cancel");
        u8 key = press();
        if ((key & (BTN_VOL_UP | BTN_VOL_DOWN)) == (BTN_VOL_UP | BTN_VOL_DOWN)) {
            return NULL;
        }
        if (key & BTN_POWER) {
            return &targets[current];
        }
        current = key & BTN_VOL_UP ? (current + count - 1) % count : (current + 1) % count;
    }
}

static int confirm(const struct switch_target *t)
{
    screen("Install iOS");
    say(UI_WHITE, "Partition %s, %d MB", t->name, mib(t->sectors));
    if (t->installed) {
        say(UI_WHITE, "The iOS on it is replaced; its data is erased.");
    } else {
        say(UI_RED, "Everything on %s is erased.", t->name);
    }
    say(UI_GREY, "Nothing else on the card is touched.");
    say(UI_GREY, "Keep the console charged and the card in.");
    line_y += 40;
    say(UI_WHITE, "VOL+  install        VOL-  cancel");
    for (;;) {
        u8 key = press();
        if (key == BTN_VOL_UP) {
            return 1;
        }
        if (key & BTN_VOL_DOWN) {
            return 0;
        }
    }
}

static int write_sectors(void *context, uint64_t sector, uint32_t count, const void *buffer)
{
    (void)context;
    return card_write(sector, count, buffer);
}

static void progress(void *context, uint64_t done, uint64_t total)
{
    (void)context;
    uint32_t now = get_tmr_ms();
    if (done && done != total && now - last_progress < 250) {
        return;
    }
    last_progress = now;
    ui_progress(360, done, total);
}

// The image's files go, so the FAT gets its space back; writes are permitted only inside the FAT partition.
static int remove_image(void)
{
    uint64_t fat_sectors = 0;
    for (uint32_t i = 0; i < gpt->entry_count; i++) {
        const uint8_t *e = gpt->entries + i * SWITCH_GPT_ENTRY_BYTES;
        uint64_t first, last;
        memcpy(&first, e + 32, 8);
        memcpy(&last, e + 40, 8);
        if (first == sd_fs.volbase) {
            fat_sectors = last - first + 1;
        }
    }
    if (!fat_sectors) {
        return 1;
    }
    card_permit(sd_fs.volbase, fat_sectors);
    int error = 0;
    for (uint32_t i = 0; i < image.part_count; i++) {
        error |= f_unlink(image.parts[i].path) != FR_OK;
    }
    error |= f_unlink(MANIFEST) != FR_OK;
    f_unlink(INSTALL_DIR);
    card_permit_none();
    return error;
}

static void install(const struct switch_target *t)
{
    uint8_t *buffer = malloc(SWITCH_IMAGE_HASH_CHUNK), *zeros = calloc(1, ZERO_BYTES);
    if (!buffer || !zeros) {
        screen("Out of memory");
        return;
    }
    switch_gpt_set(gpt, t->index, switch_gpt_linux, "iOS-setup");
    if (card_write_gpt(gpt)) {
        screen("Install failed");
        say(UI_RED, "The partition table could not be written.");
        say(UI_GREY, "The partition's contents were not touched.");
        return;
    }
    screen("Installing iOS");
    say(UI_WHITE, "Writing %s. This takes a while.", t->name);
    say(UI_GREY, "Keep the console charged and the card in.");
    struct source s = {.image = &image};
    struct switch_install_io io = {&s, source_read, source_verify, write_sectors, progress};
    card_permit(t->first, t->sectors);
    int error = switch_install_stream(&image, &io, t->first, t->sectors, buffer, zeros, ZERO_BYTES);
    card_permit_none();
    source_end(&s);
    if (error) {
        screen("Install failed");
        say(UI_RED, "The image could not be read, or the card written.");
        say(UI_GREY, "%s is now iOS-setup and does not boot.", t->name);
        say(UI_GREY, "Copy the folder from your Mac onto the card again");
        say(UI_GREY, "and run install-ios.bin again.");
        return;
    }
    switch_gpt_set(gpt, t->index, switch_gpt_apfs, "iOS");
    if (card_write_gpt(gpt)) {
        screen("Install failed");
        say(UI_RED, "iOS is written but the partition table is not.");
        say(UI_GREY, "Run install-ios.bin again.");
        return;
    }
    int kept = remove_image();
    screen("iOS is installed");
    say(UI_WHITE, "The first start takes longer: iOS sets itself up");
    say(UI_WHITE, "under the Apple logo. Later starts are quick.");
    say(UI_GREY, "Choose iOS in hekate to start it.");
    if (kept) {
        say(UI_GREY, "Delete %s from the card to free its space.", INSTALL_DIR);
    }
}

static void run(void)
{
    if (sd_mount()) {
        screen("No SD card");
        say(UI_WHITE, "The SD card could not be read.");
        return;
    }
    if (load_image()) {
        return;
    }
    gpt = malloc(sizeof(*gpt));
    if (!gpt || card_read_gpt(gpt)) {
        screen("Unexpected partition table");
        say(UI_WHITE, "The card's GPT is not valid. Nothing was written.");
        return;
    }
    int count = switch_targets(gpt, sd_fs.volbase, targets, SWITCH_TARGET_MAX);
    if (count <= 0) {
        screen("No partition for iOS");
        say(UI_WHITE,
            count < 0 ? "The card has more than one iOS partition." : "The card has no Linux or Android partition.");
        say(UI_GREY, "Make one with hekate's partition manager first.");
        return;
    }
    const struct switch_target *t = choose(count);
    if (!t) {
        screen("Cancelled");
        say(UI_WHITE, "Nothing was written.");
        return;
    }
    if (t->sectors * 512 != image.partition_bytes) {
        screen("Wrong size");
        say(UI_WHITE, "This image is for a %d MB partition;", (uint32_t)(image.partition_bytes >> 20));
        say(UI_WHITE, "%s is %d MB. Nothing was written.", t->name, mib(t->sectors));
        say(UI_GREY, "Run bazelisk run //:image again with this card in the Mac.");
        return;
    }
    if (!confirm(t)) {
        screen("Cancelled");
        say(UI_WHITE, "Nothing was written.");
        return;
    }
    install(t);
}

void ipl_main(void)
{
    hw_init();
    pivot_stack(IPL_LOAD_ADDR);
    heap_init((void *)IPL_HEAP_START);
    card_permit_none();
    display_init();
    ui_init((uint32_t *)display_init_window_a_pitch());
    ui_clear();
    display_backlight_pwm_init();
    display_backlight_brightness(100, 1000);
    bpmp_clk_rate_set(hw_get_chip_id() == GP_HIDREV_MAJOR_T210B01 ? BPMP_CLK_DEFAULT_BOOST : BPMP_CLK_LOWER_BOOST);
    run();
    line_y += 40;
    say(UI_WHITE, "Press POWER to restart.");
    while (!(press() & BTN_POWER)) {
    }
    sd_end();
    power_set_state(REBOOT_BYPASS_FUSES);
    while (1) {
        bpmp_halt();
    }
}
