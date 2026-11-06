// SPDX-License-Identifier: GPL-2.0-only
// install-ios.bin: writes one of the install images `bazel run //:image` put on the FAT onto the partition it was
// made for, and makes that image's boot files the ones hekate's iOS entry starts.
#include <bdk.h>
#include <libs/fatfs/ff.h>
#include <string.h>

#include "bootfiles.h"
#include "card.h"
#include "catalog.h"
#include "images.h"
#include "manifest.h"
#include "source.h"
#include "stream.h"
#include "target.h"
#include "ui.h"

_Static_assert(SWITCH_KEY_POWER == BTN_POWER && SWITCH_KEY_VOL_DOWN == BTN_VOL_DOWN && SWITCH_KEY_VOL_UP == BTN_VOL_UP,
               "images.h's keys are the BDK's buttons");

#define ZERO_BYTES (1u << 20)
#define MARGIN     80u
#define DETAIL_X   (MARGIN + 48u)
#define BUTTONS    (BTN_POWER | BTN_VOL_UP | BTN_VOL_DOWN)

gfx_con_t gfx_con;

void gfx_printf(const char *fmt, ...)
{
    (void)fmt;
}

extern void pivot_stack(u32 stack_top);

static struct switch_gpt *gpt;
static struct switch_target targets[SWITCH_TARGET_MAX];
static struct catalog_entry images[SWITCH_IMAGES_MAX];
static const struct switch_image *image;
static uint64_t fat_sectors;
static uint32_t line_y, last_progress;

static void screen(const char *title)
{
    ui_clear();
    ui_text(MARGIN, 64, 5, UI_WHITE, title);
    if (image) {
        ui_text(MARGIN, 124, 2, UI_GREY, image->label);
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
    while (btn_read() & BUTTONS) {
        msleep(10);
    }
    return btn_wait();
}

static const char *size_text(uint64_t bytes, char *out)
{
    switch_size_text(bytes, out);
    return out;
}

static const char *base_name(const char *path)
{
    return strrchr(path, '/') + 1;
}

static void image_row(const struct catalog_entry *e, int selected)
{
    char a[SWITCH_SIZE_TEXT], b[SWITCH_SIZE_TEXT];
    const char *name = e->image ? e->image->label : base_name(e->folder);
    ui_textf(MARGIN, line_y, 3, selected ? UI_WHITE : UI_GREY, "%s %s", selected ? ">" : " ", name);
    line_y += 32;
    if (e->state == SWITCH_IMAGE_READY) {
        ui_textf(DETAIL_X, line_y, 2, UI_GREY, "%s image for a %s partition", size_text(e->image->source_bytes, a),
                 size_text(e->image->partition_bytes, b));
    } else if (e->state == SWITCH_IMAGE_MISSING) {
        ui_textf(DETAIL_X, line_y, 2, UI_RED, "Incomplete: %s is missing or the wrong size", base_name(e->missing));
    } else if (e->state == SWITCH_IMAGE_ELSEWHERE) {
        ui_textf(DETAIL_X, line_y, 2, UI_RED, "Its manifest names files outside %s/", base_name(e->folder));
    } else {
        ui_textf(DETAIL_X, line_y, 2, UI_RED, "Unreadable: build it again with this project's //:image");
    }
    line_y += 36;
}

static void image_rows(uint32_t count, uint32_t current, int legacy)
{
    for (uint32_t i = 0; i < count; i++) {
        image_row(&images[i], i == current);
    }
    if (legacy) {
        ui_text(MARGIN, line_y, 2, UI_GREY, SWITCH_IMAGES_DIR "/manifest.bin: older layout, build it again");
        line_y += 36;
    }
}

static void ready_rows(uint8_t *ready, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        ready[i] = images[i].state == SWITCH_IMAGE_READY;
    }
}

static const struct switch_image *choose_image(uint32_t count, int legacy)
{
    uint8_t ready[SWITCH_IMAGES_MAX];
    ready_rows(ready, count);
    uint32_t current = switch_menu_first(ready, count);
    for (;;) {
        screen("Choose an image");
        image_rows(count, current, legacy);
        line_y = 656;
        say(UI_GREY, "VOL+/VOL- move    POWER choose    both: cancel");
        enum switch_menu answer = switch_menu_press(ready, count, &current, press());
        if (answer == SWITCH_MENU_CHOSEN) {
            return images[current].image;
        }
        if (answer == SWITCH_MENU_CANCELLED) {
            return NULL;
        }
    }
}

static const struct switch_target *choose_target(int count)
{
    uint8_t ready[SWITCH_TARGET_MAX];
    memset(ready, 1, sizeof(ready));
    uint32_t current = 0;
    if (count == 1) {
        return &targets[0];
    }
    for (;;) {
        char size[SWITCH_SIZE_TEXT];
        screen("Choose a partition");
        say(UI_GREY, "Which partition becomes iOS?");
        line_y += 20;
        for (int i = 0; i < count; i++) {
            say(i == (int)current ? UI_WHITE : UI_GREY, "%s %s  (%s)", i == (int)current ? ">" : " ", targets[i].name,
                size_text(targets[i].sectors * 512, size));
        }
        line_y += 20;
        say(UI_GREY, "VOL+/VOL- move    POWER choose    both: back");
        enum switch_menu answer = switch_menu_press(ready, (uint32_t)count, &current, press());
        if (answer == SWITCH_MENU_CHOSEN) {
            return &targets[current];
        }
        if (answer == SWITCH_MENU_CANCELLED) {
            return NULL;
        }
    }
}

static int confirm(const struct switch_target *t)
{
    char size[SWITCH_SIZE_TEXT];
    screen("Install iOS");
    say(UI_WHITE, "Partition %s, %s", t->name, size_text(t->sectors * 512, size));
    if (t->installed) {
        say(UI_RED, "This replaces the iPad partition and");
        say(UI_RED, "everything on it: apps, data and settings.");
    } else {
        say(UI_RED, "Everything on %s is erased.", t->name);
    }
    say(UI_WHITE, "A new container and Data volume are written.");
    say(UI_GREY, "Nothing else on the card is touched.");
    say(UI_GREY, "Keep the console charged and the card in.");
    line_y += 40;
    // A second press right after the one that chose the image would install without the warning having been read.
    msleep(1000);
    say(UI_WHITE, "POWER  install        VOL+/VOL-  back");
    for (;;) {
        u8 key = press();
        if (key == BTN_POWER) {
            return 1;
        }
        if (key & (BTN_VOL_UP | BTN_VOL_DOWN)) {
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

// The FAT partition's size, from the GPT entry hekate's MBR copy starts it at.
static uint64_t find_fat(void)
{
    for (uint32_t i = 0; i < gpt->entry_count; i++) {
        const uint8_t *e = gpt->entries + i * SWITCH_GPT_ENTRY_BYTES;
        uint64_t first, last;
        memcpy(&first, e + 32, 8);
        memcpy(&last, e + 40, 8);
        if (first == sd_fs.volbase) {
            return last - first + 1;
        }
    }
    return 0;
}

static int install_boot_files(struct switch_boot_io *io, uint8_t *buffer)
{
    card_permit(sd_fs.volbase, fat_sectors);
    int failed = switch_boot_install(image, io, buffer);
    card_permit_none();
    return failed;
}

static void install(const struct switch_target *t)
{
    uint8_t *buffer = malloc(SWITCH_IMAGE_HASH_CHUNK), *zeros = calloc(1, ZERO_BYTES);
    if (!buffer || !zeros) {
        screen("Out of memory");
        return;
    }
    struct source s = {.image = image};
    struct switch_boot_io boot_io = {&s, catalog_read, catalog_write, source_verify};
    screen("Checking the image");
    int bad = switch_boot_check(image, &boot_io, buffer);
    if (bad) {
        screen("Damaged install image");
        say(UI_WHITE, "%s does not match its manifest.", base_name(image->boot[bad - 1].path));
        say(UI_GREY, "Nothing was written. Copy the folder from");
        say(UI_GREY, "your Mac onto the card again.");
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
    int error = install_boot_files(&boot_io, buffer);
    if (!error) {
        struct switch_install_io io = {&s, source_read, source_verify, write_sectors, progress};
        card_permit(t->first, t->sectors);
        error = switch_install_stream(image, &io, t->first, t->sectors, buffer, zeros, ZERO_BYTES);
        card_permit_none();
        source_end(&s);
    }
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
    screen("iOS is installed");
    say(UI_WHITE, "The first start takes longer: iOS sets itself up");
    say(UI_WHITE, "under the Apple logo. Later starts are quick.");
    say(UI_GREY, "Choose iOS in hekate to start it.");
    say(UI_GREY, "The image stays on the card, ready for a swap.");
}

static int fits(const struct switch_target *t)
{
    char want[SWITCH_SIZE_TEXT], have[SWITCH_SIZE_TEXT];
    if (t->sectors * 512 == image->partition_bytes) {
        return 1;
    }
    screen("Wrong size");
    say(UI_WHITE, "This image is for a %s partition;", size_text(image->partition_bytes, want));
    say(UI_WHITE, "%s is %s. Nothing was written.", t->name, size_text(t->sectors * 512, have));
    say(UI_GREY, "Run bazelisk run //:image again with this card in");
    say(UI_GREY, "the Mac. Press any button to go back.");
    press();
    return 0;
}

static int read_card(int *target_count)
{
    gpt = malloc(sizeof(*gpt));
    if (!gpt || card_read_gpt(gpt) || !(fat_sectors = find_fat())) {
        screen("Unexpected partition table");
        say(UI_WHITE, "The card's GPT is not valid. Nothing was written.");
        return 1;
    }
    *target_count = switch_targets(gpt, sd_fs.volbase, targets, SWITCH_TARGET_MAX);
    if (*target_count <= 0) {
        screen("No partition for iOS");
        say(UI_WHITE, *target_count < 0 ? "The card has more than one iOS partition."
                                        : "The card has no Linux or Android partition.");
        say(UI_GREY, "Make one with hekate's partition manager first.");
        return 1;
    }
    return 0;
}

static int nothing_to_install(uint32_t count, int legacy)
{
    uint8_t ready[SWITCH_IMAGES_MAX];
    ready_rows(ready, count);
    if (switch_menu_first(ready, count) < count) {
        return 0;
    }
    screen(count ? "No complete install image" : "No install image");
    image_rows(count, count, legacy);
    if (!count && !legacy) {
        say(UI_WHITE, "This card has nothing in %s/.", SWITCH_IMAGES_DIR);
    }
    say(UI_GREY, "On your Mac, run  bazelisk run //:image");
    say(UI_GREY, "and copy its folder onto the card.");
    return 1;
}

static void run(void)
{
    int target_count;
    if (sd_mount()) {
        screen("No SD card");
        say(UI_WHITE, "The SD card could not be read.");
        return;
    }
    uint32_t count = catalog_scan(images);
    int legacy = catalog_legacy();
    if (nothing_to_install(count, legacy) || read_card(&target_count)) {
        return;
    }
    for (;;) {
        image = choose_image(count, legacy);
        if (!image) {
            screen("Cancelled");
            say(UI_WHITE, "Nothing was written.");
            return;
        }
        const struct switch_target *t = choose_target(target_count);
        if (t && fits(t) && confirm(t)) {
            install(t);
            return;
        }
        image = NULL;
    }
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
