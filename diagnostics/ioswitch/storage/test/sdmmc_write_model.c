/* Focused host-only fault injection for the actual SD write transport: sdmmc_write_cases() runs every case and is run
 * by sdmmc_write_test.cpp. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../sdmmc_read.h"
#include "bazel/testing/c_check.h"
enum fault {
    NONE,
    POWER_OFF,
    PIN_PROTECT,
    PRE_ERROR,
    CMD_ERROR,
    CMD_TIMEOUT,
    CARD_ERROR,
    BUFFER_ERROR,
    BUFFER_TIMEOUT,
    BUFFER_MISSING,
    EARLY_DONE,
    EARLY_READY_DONE,
    MID_ERROR,
    MID_DONE,
    END_ERROR,
    END_TIMEOUT,
    AUTO_ERROR,
    STOP_ERROR,
    BUSY_WAIT,
    BUSY_TIMEOUT,
    REMOVED,
    STATUS_PROGRAM,
    STATUS_WRONG,
    STATUS_TIMEOUT,
    STOPPED_TIMER,
    LAST_BLOCK_ERROR,
    LAST_BLOCK_TIMEOUT
};
struct fixture {
    uint32_t regs[128], commands[1024], arguments[1024], command_count, current, phase;
    uint32_t size, count, words, blocks, armed, busy_reads, status_commands;
    uint32_t io_count, fifo_writes, write_calls, accepted, permit_count, reads, writes;
    uint32_t status_reads, tick_reads;
    uint32_t fail_command, app_missing;
    uint64_t now, permit_lba;
    uint8_t scr[8], general[512], perf[512], data[65536];
    enum fault fault;
};
static int writing(struct fixture *f)
{
    return f->current == 24 || f->current == 25;
}
static int fault_command(struct fixture *f)
{
    return writing(f) || (f->fail_command && f->fail_command == f->command_count);
}
static uint8_t read8(void *context, uint32_t offset)
{
    struct fixture *f = context;
    f->io_count++;
    f->reads++;
    if (offset == 0x28) return 2;
    if (offset == 0x29) return f->fault == POWER_OFF ? 0 : 11;
    CHECK(offset == 0x2f);
    return 0;
}
static uint16_t read16(void *context, uint32_t offset)
{
    struct fixture *f = context;
    f->io_count++;
    f->reads++;
    if (offset == 0xfe) return 0x0303;
    if (offset == 0x2c) return 7;
    if (offset == 0x3c) return f->fault == AUTO_ERROR ? 4 : 0;
    CHECK(offset == 4 || offset == 6 || offset == 12);
    return (uint16_t)f->regs[offset / 2];
}
static uint32_t read32(void *context, uint32_t offset)
{
    struct fixture *f = context;
    f->io_count++;
    f->reads++;
    if (offset == 0x24) {
        uint32_t present = 0x1fb0000;
        if (f->fault == PIN_PROTECT) present &= ~0x80000u;
        if (f->phase == 2) {
            present |= 0x203;
            if (f->fault != BUFFER_MISSING || !writing(f)) present |= writing(f) ? 0x400 : 0x800;
        }
        if (f->phase == 4 && writing(f)) {
            f->busy_reads++;
            if (f->fault == BUSY_TIMEOUT || (f->fault == BUSY_WAIT && f->busy_reads <= 4))
                present = (present | 2) & ~0x100000u;
            if (f->fault == REMOVED) present &= ~0x10000u;
        }
        return present;
    }
    if (offset == 0x30) {
        f->status_reads++;
        if (writing(f) && f->fault == LAST_BLOCK_ERROR && f->words >= f->count * 128 - 63) return 0x208010;
        if (!f->phase) return f->fault == PRE_ERROR ? 0x8000 : 0;
        if (f->phase == 1) {
            if (fault_command(f)) {
                if (f->fault == CMD_TIMEOUT || f->fault == STOPPED_TIMER) return 0;
                if (f->fault == CMD_ERROR) return 0x20001;
            }
            return 1;
        }
        if (f->phase == 2) {
            if (writing(f)) {
                if (f->fault == MID_ERROR && f->words >= 65) return 0x208010;
                if (f->fault == MID_DONE && f->words >= 65) return 2;
                if (f->blocks == 1) {
                    if (f->fault == BUFFER_ERROR) return 0x208010;
                    if (f->fault == BUFFER_TIMEOUT) return 0;
                    if (f->fault == EARLY_DONE) return 2;
                    if (f->fault == EARLY_READY_DONE) return 0x12;
                }
                return f->armed ? 0 : 0x10;
            }
            return f->armed ? 0 : 0x20;
        }
        if (f->phase == 3) {
            if (writing(f) && f->fault == END_ERROR) return 0x208002;
            if (writing(f) && f->fault == END_TIMEOUT) return 0;
            return 2;
        }
        CHECK(f->phase == 4);
        return 0;
    }
    if (offset == 0x10) {
        if (fault_command(f) && f->fault == CARD_ERROR) return 0x400000;
        if (f->current == 13) {
            if (f->fault == STATUS_WRONG) return 0x700;
            if (f->fault == STATUS_TIMEOUT || (f->fault == STATUS_PROGRAM && f->status_commands < 3)) return 0xe00;
        }
        return f->current == 55 && !f->app_missing ? 0x920 : 0x900;
    }
    if (offset == 0x1c) return f->fault == STOP_ERROR ? 0x400000 : 0x900;
    if (offset == 0x20) {
        CHECK(f->phase == 2 && !writing(f) && f->armed);
        const uint8_t *bytes = f->current == 51                              ? f->scr
                               : f->arguments[f->command_count - 1] == 0x1ff ? f->general
                                                                             : f->perf;
        uint32_t at = f->words++ * 4, value = 0;
        for (unsigned i = 0; i < 4; i++) value |= (uint32_t)bytes[at + i] << (8 * i);
        if (f->words == f->size / 4) f->phase = 3;
        return value;
    }
    CHECK(offset == 8 || offset == 0x34 || offset == 0x38);
    return f->regs[offset / 2];
}
static void write16(void *context, uint32_t offset, uint16_t value)
{
    struct fixture *f = context;
    f->io_count++;
    f->writes++;
    if (offset == 0xe) {
        if (f->phase || f->command_count >= 1024)
            fprintf(stderr, "command invariant phase %u count %u fault %u\n", f->phase, f->command_count, f->fault);
        CHECK(!f->phase && f->command_count < 1024);
        f->current = value >> 8;
        CHECK(f->current == 13 || f->current == 55 || f->current == 51 || f->current == 48 || f->current == 24 ||
              f->current == 25);
        CHECK(value == (f->current << 8 | (f->current == 13 || f->current == 55 ? 0x1a : 0x3a)));
        f->commands[f->command_count] = f->current;
        f->arguments[f->command_count++] = f->regs[8 / 2];
        f->count = f->regs[6 / 2];
        f->size = f->count ? f->regs[4 / 2] : 0;
        if (writing(f)) {
            CHECK(f->accepted && f->permit_lba == f->regs[8 / 2] && f->permit_count == f->count);
            CHECK(f->size == 512 && f->count >= 1 && f->count <= 128);
            CHECK(f->regs[12 / 2] == (f->count > 1 ? 0x26 : 2));
        } else if (f->current == 13 || f->current == 55) {
            CHECK(!f->count && !f->regs[12 / 2]);
            CHECK(f->regs[8 / 2] == 0x12340000);
        } else {
            CHECK(f->count == 1 && f->regs[12 / 2] == 0x12);
            CHECK(f->size == (f->current == 51 ? 8 : 512));
        }
        CHECK(!f->regs[0x38 / 2]);
        CHECK((f->regs[0x34 / 2] & 0x03ff8033) == 0x03ff8033);
        f->phase = 1;
        f->words = f->blocks = f->armed = 0;
        if (f->current == 13) f->status_commands++;
        return;
    }
    CHECK(offset == 4 || offset == 6 || offset == 12);
    f->regs[offset / 2] = value;
}
static void write32(void *context, uint32_t offset, uint32_t value)
{
    struct fixture *f = context;
    f->io_count++;
    f->writes++;
    if (offset == 0x30) {
        if (!f->phase)
            CHECK(value == 0x33);
        else if (f->phase == 1) {
            CHECK(value == 1);
            f->phase = f->size ? 2 : 4;
        } else if (f->phase == 2) {
            CHECK(value == (writing(f) ? 0x10 : 0x20));
            CHECK(!f->armed);
            f->armed = 1;
        } else {
            CHECK(f->phase == 3 && value == 2);
            f->phase = 4;
        }
        return;
    }
    if (offset == 0x20) {
        CHECK(writing(f) && f->phase == 2 && f->armed && f->words < f->count * 128);
        uint32_t at = f->words++ * 4;
        f->fifo_writes++;
        if (f->fault == LAST_BLOCK_TIMEOUT && f->words == f->count * 128 - 63) f->now += 6000000;
        for (unsigned i = 0; i < 4; i++) f->data[at + i] = (uint8_t)(value >> (8 * i));
        if (!(f->words & 127)) {
            f->blocks++;
            f->armed = 0;
            if (f->blocks == f->count) f->phase = 3;
        }
        return;
    }
    CHECK(offset == 8 || offset == 0x34 || offset == 0x38);
    if (offset == 0x38 && value) {
        CHECK(f->phase == 4);
        f->phase = 0;
    }
    f->regs[offset / 2] = value;
}
static uint64_t ticks(void *context)
{
    struct fixture *f = context;
    f->io_count++;
    f->tick_reads++;
    if (f->fault != STOPPED_TIMER) f->now += f->fault == NONE ? 1 : 1000;
    return f->now;
}
static void barrier(void *context)
{
    ((struct fixture *)context)->io_count++;
}
static int permit(void *context, uint64_t lba, uint32_t count)
{
    struct fixture *f = context;
    CHECK(!f->io_count);
    f->write_calls++;
    f->permit_lba = lba;
    f->permit_count = count;
    return f->accepted;
}
static void setup(struct fixture *f, struct iosw_sd_host *h, enum fault fault)
{
    *f = (struct fixture){.fault = fault, .accepted = 1};
    f->regs[4 / 2] = 0x7200;
    f->regs[6 / 2] = 7;
    f->regs[12 / 2] = 0x33;
    f->regs[8 / 2] = 0x4567;
    f->regs[0x34 / 2] = 0x8003;
    f->regs[0x38 / 2] = 0x8002;
    f->scr[0] = 2;
    f->scr[1] = 5;
    *h = (struct iosw_sd_host){.io = {read8, read16, read32, write16, write32, ticks, barrier},
                               .context = f,
                               .frequency = 1000000,
                               .capacity_sectors = UINT64_C(0x100000000),
                               .permitted_first_sector = 100,
                               .permitted_sector_count = 1000,
                               .block_addressing_verified = 1,
                               .rca = 0x1234,
                               .write_cache_off_verified = 1,
                               .permit_write = permit,
                               .write_context = f};
}
static void extensions(struct fixture *f)
{
    f->scr[2] = 0x84;
    f->scr[3] = 4;
    f->general[2] = 64;
    f->general[4] = 1;
    f->general[16] = 2;
    f->general[58] = 1;
    f->general[61] = 4;
    f->general[62] = 4; /* FNO1, page2, offset0. */
    f->perf[4] = 1;
}
static unsigned cases;
static void write_case(uint32_t count, enum fault fault, int expected)
{
    struct fixture f;
    struct iosw_sd_host h;
    setup(&f, &h, fault);
    uint32_t saved[128];
    memcpy(saved, f.regs, sizeof saved);
    uint8_t bytes[65536];
    for (uint32_t i = 0; i < sizeof bytes; i++) bytes[i] = (uint8_t)(i * 7 + i / 512);
    struct iosw_sd_result r;
    int result = iosw_sd_write_sectors(&h, 100, count, bytes, &r);
    if (result != expected)
        fprintf(stderr, "write fault %u got %d expected %d stage %u\n", fault, result, expected, r.stage);
    CHECK(result == expected);
    if (!result) {
        CHECK(!h.blocked && h.write_cache_off_verified && r.words_written == count * 128 && r.stage == 22);
        CHECK(f.commands[0] == (count > 1 ? 25 : 24) && f.commands[1] == 13);
        CHECK(!memcmp(saved, f.regs, sizeof saved));
        CHECK(!memcmp(f.data, bytes, count * 512));
        CHECK(r.card_status == 0x900);
        if (count == 128) {
            CHECK(f.status_reads < 300 && f.tick_reads < 600);
            printf("64KiB write: %u FIFO writes, %u status reads, %u timer samples, %u total I/O callbacks\n",
                   f.fifo_writes, f.status_reads, f.tick_reads, f.io_count);
        }
    } else if (fault == POWER_OFF || fault == PIN_PROTECT || fault == PRE_ERROR) {
        CHECK(!f.command_count && !f.writes && !h.blocked);
    } else {
        CHECK(h.blocked && !h.write_cache_off_verified && !f.regs[0x38 / 2]);
        uint32_t io = f.io_count;
        CHECK(iosw_sd_write_sectors(&h, 100, count, bytes, &r) == IOSW_SD_ALREADY_BLOCKED && f.io_count == io);
        CHECK(iosw_sd_sync(&h, &r) == IOSW_SD_ALREADY_BLOCKED && f.io_count == io);
        if (fault == MID_ERROR || fault == MID_DONE) CHECK(f.fifo_writes == 128);
        if (fault == EARLY_DONE || fault == EARLY_READY_DONE) CHECK(f.fifo_writes == 128);
    }
    cases++;
}
static void qualify_case(unsigned variant, int expected, uint32_t evidence)
{
    struct fixture f;
    struct iosw_sd_host h;
    setup(&f, &h, NONE);
    h.write_cache_off_verified = 0;
    if (variant) extensions(&f);
    if (variant == 2) f.perf[4] = 0;
    if (variant == 3) f.general[16] = 1;
    if (variant == 4) f.perf[260] = 1;
    if (variant == 5) f.perf[0] = 1;
    if (variant == 6) f.perf[262] = 1;
    if (variant == 7) f.general[0] = 1;
    if (variant == 8) f.general[3] = 3;
    if (variant == 9) f.general[4] = 11;
    if (variant == 10) f.general[58] = 2;
    if (variant == 11) f.general[56] = 16; /* Last descriptor cannot loop. */
    if (variant == 12) f.general[60] = 1;  /* Nonzero performance offset. */
    if (variant == 13) f.general[63] = 1;  /* Reserved address bit. */
    if (variant == 14) f.general[62] = 0;  /* FNO zero. */
    if (variant == 15) f.scr[0] = 0x12;
    if (variant == 16) f.scr[1] = 1;
    if (variant == 17) f.scr[2] = 4; /* CMD48 without SD_SPEC3. */
    if (variant == 18) {
        f.scr[3] = 8;
    } /* CMD58 without CMD48. */
    if (variant == 19) {
        f.general[4] = 0;
    }
    if (variant == 20) {
        f.general[2] = 63;
    }
    if (variant >= 21) {
        f.general[2] = 112;
        f.general[4] = 2;
        f.general[56] = 64;
        f.general[64] = variant == 21 ? 2 : 1;
        f.general[106] = 1;
        f.general[109] = 4;
        f.general[110] = 4;
        if (variant == 23) f.general[56] = 32; /* Overlap. */
    }
    uint32_t saved[128];
    memcpy(saved, f.regs, sizeof saved);
    struct iosw_sd_result r;
    int result = iosw_sd_qualify_writes(&h, &r);
    if (result != expected) fprintf(stderr, "qualify variant %u got %d expected %d\n", variant, result, expected);
    CHECK(result == expected && !f.fifo_writes && !f.write_calls);
    for (unsigned i = 0; i < f.command_count; i++)
        CHECK(f.commands[i] != 24 && f.commands[i] != 25 && f.commands[i] != 49);
    if (!result) {
        CHECK(h.write_cache_off_verified && !h.blocked && r.cache_evidence == evidence && r.stage == 12);
        CHECK(!memcmp(saved, f.regs, sizeof saved));
        CHECK(f.commands[0] == 13 && f.commands[1] == 55 && f.commands[2] == 51);
    } else
        CHECK(h.blocked && !h.write_cache_off_verified && !f.regs[0x38 / 2]);
    cases++;
}
unsigned sdmmc_write_cases(void);
unsigned sdmmc_write_cases(void)
{
    write_case(1, NONE, 0);
    write_case(2, NONE, 0);
    write_case(128, NONE, 0);
    write_case(2, POWER_OFF, IOSW_SD_NOT_READY);
    write_case(2, PIN_PROTECT, IOSW_SD_WRITE_PROTECTED);
    write_case(2, PRE_ERROR, IOSW_SD_PREEXISTING_ERROR);
    write_case(2, CMD_ERROR, IOSW_SD_CONTROLLER_ERROR);
    write_case(2, CMD_TIMEOUT, IOSW_SD_TIMEOUT);
    write_case(2, CARD_ERROR, IOSW_SD_CARD_ERROR);
    write_case(2, BUFFER_ERROR, IOSW_SD_CONTROLLER_ERROR);
    write_case(2, BUFFER_TIMEOUT, IOSW_SD_TIMEOUT);
    write_case(2, BUFFER_MISSING, IOSW_SD_BUFFER_STATE);
    write_case(2, EARLY_DONE, IOSW_SD_BUFFER_STATE);
    write_case(2, EARLY_READY_DONE, IOSW_SD_BUFFER_STATE);
    write_case(2, MID_ERROR, IOSW_SD_CONTROLLER_ERROR);
    write_case(2, MID_DONE, IOSW_SD_BUFFER_STATE);
    write_case(2, END_ERROR, IOSW_SD_CONTROLLER_ERROR);
    write_case(2, END_TIMEOUT, IOSW_SD_TIMEOUT);
    write_case(2, AUTO_ERROR, IOSW_SD_CONTROLLER_ERROR);
    write_case(2, STOP_ERROR, IOSW_SD_CARD_ERROR);
    write_case(2, BUSY_WAIT, 0);
    write_case(2, BUSY_TIMEOUT, IOSW_SD_TIMEOUT);
    write_case(2, REMOVED, IOSW_SD_NOT_READY);
    write_case(2, STATUS_PROGRAM, 0);
    write_case(2, STATUS_WRONG, IOSW_SD_CARD_STATE);
    write_case(2, STATUS_TIMEOUT, IOSW_SD_TIMEOUT);
    write_case(2, STOPPED_TIMER, IOSW_SD_TIMEOUT);
    write_case(2, LAST_BLOCK_ERROR, IOSW_SD_CONTROLLER_ERROR);
    write_case(2, LAST_BLOCK_TIMEOUT, IOSW_SD_TIMEOUT);
    qualify_case(0, 0, IOSW_SD_CACHE_NO_CMD48);
    qualify_case(1, 0, IOSW_SD_CACHE_OBSERVED_OFF);
    qualify_case(2, 0, IOSW_SD_CACHE_UNSUPPORTED);
    qualify_case(3, 0, IOSW_SD_CACHE_NO_PERF);
    for (unsigned i = 4; i <= 21; i++) qualify_case(i, IOSW_SD_CACHE_UNQUALIFIED, 0);
    qualify_case(22, 0, IOSW_SD_CACHE_OBSERVED_OFF);
    qualify_case(23, IOSW_SD_CACHE_UNQUALIFIED, 0);
    for (unsigned scenario = 0; scenario < 17; scenario++) {
        struct fixture f;
        struct iosw_sd_host h;
        setup(&f, &h, NONE);
        uint64_t lba = 100;
        uint32_t count = 2;
        uint8_t bytes[1024] = {0};
        struct iosw_sd_result r;
        if (scenario == 0) count = 0;
        if (scenario == 1) count = 129;
        if (scenario == 2) lba = 99;
        if (scenario == 3) lba = 1099;
        if (scenario == 4) lba = UINT64_MAX;
        if (scenario == 5) h.block_addressing_verified = 0;
        if (scenario == 6) h.rca = 0;
        if (scenario == 7) h.rca = 0x10000;
        if (scenario == 8) h.card_write_protected = 1;
        if (scenario == 9) h.write_cache_off_verified = 0;
        if (scenario == 10) h.permit_write = 0;
        if (scenario == 11) f.accepted = 0;
        if (scenario == 12) f.accepted = 2;
        if (scenario == 13) h.capacity_sectors = UINT64_C(0x100000001);
        if (scenario == 14) h.permitted_sector_count = UINT64_MAX;
        if (scenario == 15) h.io.write16 = 0;
        if (scenario == 16) h.frequency = 0;
        CHECK(iosw_sd_write_sectors(&h, lba, count, bytes, &r) != 0 && !f.io_count && !f.command_count && !h.blocked);
        cases++;
    }
    /* Each qualification command failure is fail-stop and cannot qualify cache. */
    for (unsigned command = 1; command <= 6; command++) {
        for (unsigned card_error = 0; card_error < 2; card_error++) {
            struct fixture f;
            struct iosw_sd_host h;
            struct iosw_sd_result r;
            setup(&f, &h, card_error ? CARD_ERROR : CMD_ERROR);
            extensions(&f);
            h.write_cache_off_verified = 0;
            f.fail_command = command;
            CHECK(iosw_sd_qualify_writes(&h, &r) == (card_error ? IOSW_SD_CARD_ERROR : IOSW_SD_CONTROLLER_ERROR));
            CHECK(h.blocked && !h.write_cache_off_verified && !f.fifo_writes && f.command_count == command);
            cases++;
        }
    }
    struct fixture f;
    struct iosw_sd_host h;
    struct iosw_sd_result r;
    setup(&f, &h, NONE);
    f.app_missing = 1;
    h.write_cache_off_verified = 0;
    CHECK(iosw_sd_qualify_writes(&h, &r) == IOSW_SD_CARD_STATE && h.blocked && f.command_count == 2);
    cases++;
    setup(&f, &h, NONE);
    CHECK(iosw_sd_sync(&h, &r) == 0 && r.stage == 31 && f.command_count == 1 && f.commands[0] == 13 && !f.fifo_writes);
    cases++;
    setup(&f, &h, NONE);
    h.write_cache_off_verified = 0;
    CHECK(iosw_sd_sync(&h, &r) == IOSW_SD_CACHE_UNQUALIFIED && !f.io_count);
    cases++;
    setup(&f, &h, NONE);
    h.permitted_first_sector = 0;
    h.permitted_sector_count = h.capacity_sectors;
    uint8_t bytes[65536] = {0};
    CHECK(iosw_sd_write_sectors(&h, UINT64_C(0xffffff80), 128, bytes, &r) == 0 && f.permit_lba == UINT64_C(0xffffff80));
    cases++;
    return cases;
}
