/* boot/storage/raw_apfs.c (the raw APFS block backend) against a fixture of its block I/O: every NXSB block size,
 * reads and writes inside the partition only, refusals before any I/O, corrupt superblocks, and fail-stop on every
 * injected error. raw_apfs_cases() runs every scenario and is run by raw_apfs_test.cpp. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../raw_apfs.h"
#include "bazel/testing/c_check.h"

static const uint8_t expected_uuid[16] = {0x31};
static const uint8_t apfs_type[16] = {0xef, 0x57, 0x34, 0x7c, 0,    0,    0xaa, 0x11,
                                      0xaa, 0x11, 0,    0x30, 0x65, 0x43, 0xec, 0xac};
struct fixture {
    struct switch_raw_apfs backend;
    struct switch_raw_apfs_io io;
    struct switch_raw_apfs_partition parts[3];
    uint8_t nxsb[65536], scratch[65536], written[65536];
    uint64_t first, sectors, last_lba;
    uint32_t block_size, last_count, budget, used;
    unsigned active, offline, begins, ends, reads, writes, syncs, qualifies, unbinds;
    int ready_error, begin_error, end_error, read_error_at, write_error, sync_error, qualify_error;
    int (*permit)(void *, uint64_t, uint32_t);
    void *owner;
};
static void put32(uint8_t *p, uint32_t n)
{
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(n >> (8 * i));
}
static void put64(uint8_t *p, uint64_t n)
{
    for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(n >> (8 * i));
}
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
/* Independent weighted-sum formulation, rather than production's recurrence. */
static void seal(struct fixture *f)
{
    uint64_t a = 0, b = 0, mod = 0xffffffff;
    uint32_t words = (f->block_size - 8) / 4;
    for (uint32_t i = 0; i < words; i++) {
        uint32_t v = get32(f->nxsb + 8 + i * 4);
        a += v;
        b += (uint64_t)(words - i) * v;
    }
    a %= mod;
    b %= mod;
    uint64_t x = mod - (a + b) % mod, y = mod - (a + x) % mod;
    put64(f->nxsb, x | (y << 32));
}
static int ready(void *p)
{
    struct fixture *f = p;
    return f->offline ? SWITCH_BLOCK_NOT_READY : f->ready_error;
}
static int begin(void *p, uint32_t n, uint32_t seconds)
{
    struct fixture *f = p;
    f->begins++;
    if (f->begin_error) return f->begin_error;
    CHECK(!f->active && n && n <= 129 && (seconds == 5 || seconds == 30));
    f->active = 1;
    f->budget = n;
    f->used = 0;
    return 0;
}
static int end(void *p)
{
    struct fixture *f = p;
    CHECK(f->active);
    f->active = 0;
    f->ends++;
    return f->end_error;
}
static void offline(void *p)
{
    ((struct fixture *)p)->offline = 1;
}
static int read_sectors(void *p, uint64_t lba, uint32_t n, uint8_t *out)
{
    struct fixture *f = p;
    CHECK(f->active && n && n <= 128 && n <= f->budget - f->used && lba >= f->first && lba - f->first < f->sectors &&
          n <= f->sectors - (lba - f->first));
    f->reads++;
    f->used += n;
    f->last_lba = lba;
    f->last_count = n;
    if (f->read_error_at == (int)f->reads) {
        memset(out, 0xee, 512);
        return SWITCH_BLOCK_IO;
    }
    if (lba == f->first && n * 512 <= f->block_size)
        memcpy(out, f->nxsb, n * 512);
    else
        for (uint32_t i = 0; i < n * 512; i++) out[i] = (uint8_t)(lba + i);
    return 0;
}
static int qualify(void *p, int (*permit)(void *, uint64_t, uint32_t), void *owner)
{
    struct fixture *f = p;
    CHECK(!f->active && !f->owner);
    f->qualifies++;
    f->permit = permit;
    f->owner = owner;
    CHECK(permit(owner, f->first, 1) == 1 && permit(owner, f->first - 1, 2) == 0 && permit(owner, UINT64_MAX, 1) == 0);
    return f->qualify_error;
}
static void unbind(void *p, void *owner)
{
    struct fixture *f = p;
    f->unbinds++;
    if (f->owner == owner) {
        f->owner = 0;
        f->permit = 0;
    }
}
static int write_sectors(void *p, uint64_t lba, uint32_t n, const uint8_t *in)
{
    struct fixture *f = p;
    CHECK(f->active && f->permit && f->permit(f->owner, lba, n) == 1 && n <= f->budget - f->used);
    f->used += n;
    f->writes++;
    f->last_lba = lba;
    f->last_count = n;
    memcpy(f->written, in, n * 512);
    return f->write_error;
}
static int sync_card(void *p)
{
    struct fixture *f = p;
    CHECK(f->active);
    f->syncs++;
    return f->sync_error;
}
static void setup(struct fixture *f, uint32_t block_size)
{
    memset(f, 0, sizeof(*f));
    f->first = 2048;
    f->sectors = 65536;
    f->block_size = block_size;
    f->parts[0] = (struct switch_raw_apfs_partition){.first_sector = f->first, .sector_count = f->sectors};
    memcpy(f->parts[0].type_guid, apfs_type, 16);
    f->io = (struct switch_raw_apfs_io){f,       ready,  begin,         end,      offline, read_sectors,
                                      qualify, unbind, write_sectors, sync_card};
    put64(f->nxsb + 8, 1);
    put64(f->nxsb + 16, 5);
    put32(f->nxsb + 24, 0x80000001);
    memcpy(f->nxsb + 32, "NXSB", 4);
    put32(f->nxsb + 36, block_size);
    put64(f->nxsb + 40, 4096);
    f->nxsb[72] = 0x31;
    put32(f->nxsb + 104, 8);
    put32(f->nxsb + 108, 8);
    put64(f->nxsb + 112, 1);
    put64(f->nxsb + 120, 9);
    put64(f->nxsb + 160, 17);
    put32(f->nxsb + 180, 100);
    seal(f);
}
static int open_count(struct fixture *f, unsigned n)
{
    return switch_raw_apfs_open(&f->backend, &f->io, 100000, f->parts, n, expected_uuid, f->scratch, sizeof(f->scratch));
}
static void close_backend(struct fixture *f)
{
    switch_raw_apfs_close(&f->backend);
    CHECK(!f->active && !f->owner);
}
static unsigned cases;
unsigned raw_apfs_cases(void);
unsigned raw_apfs_cases(void)
{
    struct fixture f;
    uint8_t bytes[65536], client[65536];
    struct switch_block_info info;
    for (unsigned size = 4096; size <= 65536; size *= 2) {
        setup(&f, size);
        if (size > 8192) put64(f.nxsb + 40, 256);
        seal(&f);
        CHECK(!switch_raw_apfs_validate_nxsb(f.nxsb, size, f.sectors));
        CHECK(!open_count(&f, 1) && f.qualifies == 1 && f.reads == 2 && !f.offline);
        CHECK(!switch_raw_apfs_get_info(&f.backend, &info) && !strcmp(info.label, "APFS") &&
              info.size_bytes == f.sectors * 512);
        CHECK(f.permit(f.owner, f.first + f.sectors - 1, 1) == 1 && f.permit(f.owner, f.first + f.sectors - 1, 2) == 0);
        close_backend(&f);
        CHECK(f.unbinds == 1);
        cases++;
    }
    setup(&f, 4096);
    CHECK(!open_count(&f, 1));
    memset(bytes, 0xa5, sizeof(bytes));
    CHECK(!switch_raw_apfs_write(&f.backend, 512, bytes, sizeof(bytes)) && f.writes == 1 && f.last_lba == f.first + 1 &&
          f.last_count == 128 && !memcmp(f.written, bytes, sizeof(bytes)));
    CHECK(!switch_raw_apfs_read(&f.backend, 65536, bytes, sizeof(bytes)) && f.last_lba == f.first + 128 &&
          f.last_count == 128);
    for (unsigned i = 0; i < sizeof(bytes); i++) CHECK(bytes[i] == (uint8_t)(f.first + 128 + i));
    CHECK(!switch_raw_apfs_sync(&f.backend) && f.syncs == 1);
    cases += 3;
    uint64_t total = f.sectors * 512;
    CHECK(!switch_raw_apfs_write(&f.backend, total - 512, bytes, 512) && f.last_lba == f.first + f.sectors - 1);
    cases++;
    for (unsigned k = 0; k < 7; k++) {
        uint64_t offset = k == 0 ? 1 : k == 1 ? UINT64_MAX : k == 2 ? total : k == 3 ? total - 512 : 0;
        uint32_t n = k == 3 ? 1024 : k == 4 ? 65537 : k == 5 ? 0 : 512;
        unsigned before = f.begins;
        CHECK(switch_raw_apfs_write(&f.backend, offset, k == 6 ? 0 : bytes, n) == SWITCH_BLOCK_INVALID &&
              f.begins == before && !f.offline);
        cases++;
    }
    close_backend(&f);
    for (unsigned k = 0; k < 8; k++) {
        setup(&f, 4096);
        if (k == 0) f.parts[0].first_sector = 0;
        if (k == 1) f.parts[0].sector_count = 0;
        if (k == 2) f.parts[0].first_sector = 100000;
        if (k == 3) f.parts[0].sector_count = UINT64_MAX;
        if (k == 4) memset(f.parts[0].type_guid, 0, 16);
        if (k == 5) f.parts[0].sector_count = 7;
        if (k >= 6) {
            f.parts[1] = f.parts[0];
            if (k == 7) {
                f.parts[1].first_sector = 80000;
                f.parts[1].sector_count = 100;
            }
        }
        CHECK(open_count(&f, k >= 6 ? 2 : 1) == SWITCH_BLOCK_CORRUPT && !f.qualifies && !f.writes && f.offline);
        close_backend(&f);
        cases++;
    }
    setup(&f, 4096);
    {
        uint8_t wrong[16] = {0x32};
        CHECK(switch_raw_apfs_open(&f.backend, &f.io, 100000, f.parts, 1, wrong, f.scratch, sizeof(f.scratch)) ==
                  SWITCH_BLOCK_CORRUPT &&
              f.reads == 2 && !f.qualifies && !f.writes && f.offline);
        close_backend(&f);
        cases++;
    }
    setup(&f, 4096);
    f.parts[0].type_guid[0] ^= 1;
    CHECK(open_count(&f, 1) == SWITCH_BLOCK_NOT_READY && !f.begins && !f.offline);
    close_backend(&f);
    cases++;
    setup(&f, 4096);
    CHECK(open_count(&f, 0) == SWITCH_BLOCK_NOT_READY && !f.begins && !f.offline);
    close_backend(&f);
    cases++;
    setup(&f, 4096);
    f.first = UINT64_C(0xffff0000);
    f.parts[0].first_sector = f.first;
    CHECK(!switch_raw_apfs_open(&f.backend, &f.io, UINT64_C(0x100000000), f.parts, 1, expected_uuid, f.scratch,
                              sizeof(f.scratch)));
    CHECK(!switch_raw_apfs_write(&f.backend, f.sectors * 512 - 512, bytes, 512) && f.last_lba == UINT64_C(0xffffffff));
    close_backend(&f);
    cases++;
    setup(&f, 4096);
    put32(f.nxsb + 104, 0x80000008);
    put64(f.nxsb + 112, 4095);
    put32(f.nxsb + 128, UINT32_MAX);
    put32(f.nxsb + 136, UINT32_MAX);
    seal(&f);
    CHECK(!open_count(&f, 1));
    close_backend(&f);
    cases++;
    setup(&f, 8192);
    CHECK(switch_raw_apfs_validate_nxsb(f.nxsb, 4096, f.sectors) == SWITCH_BLOCK_CORRUPT);
    CHECK(switch_raw_apfs_validate_nxsb(0, 4096, f.sectors) == SWITCH_BLOCK_INVALID);
    CHECK(switch_raw_apfs_validate_nxsb(f.nxsb, 512, f.sectors) == SWITCH_BLOCK_INVALID);
    CHECK(switch_raw_apfs_validate_nxsb(f.nxsb, 8192, 0) == SWITCH_BLOCK_INVALID);
    cases += 4;
    for (unsigned k = 0; k < 19; k++) {
        setup(&f, 4096);
        if (k == 0) f.nxsb[100] ^= 1;
        if (k == 1) put64(f.nxsb + 8, 2);
        if (k == 2) put64(f.nxsb + 16, 0);
        if (k == 3) put32(f.nxsb + 24, 2);
        if (k == 4) put32(f.nxsb + 24, 0xa0000001);
        if (k == 5) put32(f.nxsb + 28, 1);
        if (k == 6) f.nxsb[32] = 0;
        if (k == 7) put32(f.nxsb + 36, 3072);
        if (k == 8) put32(f.nxsb + 36, 131072);
        if (k == 9) put64(f.nxsb + 40, 0);
        if (k == 10) put64(f.nxsb + 40, UINT64_MAX);
        if (k == 11) f.nxsb[72] = 0;
        if (k == 12) put64(f.nxsb + 64, 0x100);
        if (k == 13) f.nxsb[1280] = 1;
        if (k == 14) put64(f.nxsb + 112, 4096);
        if (k == 15) put64(f.nxsb + 120, 2);
        if (k == 16) put64(f.nxsb + 160, 4096);
        if (k == 17) put32(f.nxsb + 180, 101);
        if (k == 18) put32(f.nxsb + 108, 0);
        if (k) seal(&f);
        CHECK(open_count(&f, 1) == SWITCH_BLOCK_CORRUPT && !f.qualifies && !f.writes && f.offline);
        close_backend(&f);
        cases++;
    }
    for (unsigned k = 0; k < 6; k++) {
        setup(&f, 4096);
        if (k == 0) f.ready_error = SWITCH_BLOCK_NOT_READY;
        if (k == 1) f.begin_error = SWITCH_BLOCK_TIMEOUT;
        if (k == 2) f.read_error_at = 1;
        if (k == 3) f.read_error_at = 2;
        if (k == 4) f.end_error = SWITCH_BLOCK_TIMEOUT;
        if (k == 5) f.qualify_error = SWITCH_BLOCK_IO;
        CHECK(open_count(&f, 1) != 0 && f.offline && !f.writes && !f.owner);
        unsigned reads = f.reads;
        CHECK(switch_raw_apfs_read(&f.backend, 0, bytes, 512) == SWITCH_BLOCK_NOT_READY && f.reads == reads);
        close_backend(&f);
        cases++;
    }
    for (unsigned k = 0; k < 5; k++) {
        setup(&f, 4096);
        CHECK(!open_count(&f, 1));
        int result;
        if (k == 0) {
            f.read_error_at = f.reads + 1;
            memset(client, 0x5a, sizeof(client));
            result = switch_raw_apfs_read(&f.backend, 0, bytes, 512);
            if (!result) memcpy(client, bytes, 512);
            for (unsigned i = 0; i < sizeof(client); i++) CHECK(client[i] == 0x5a);
        } else if (k == 1) {
            f.write_error = SWITCH_BLOCK_IO;
            result = switch_raw_apfs_write(&f.backend, 0, bytes, 512);
            CHECK(f.writes == 1);
        } else if (k == 2) {
            f.sync_error = SWITCH_BLOCK_TIMEOUT;
            result = switch_raw_apfs_sync(&f.backend);
        } else if (k == 3) {
            f.end_error = SWITCH_BLOCK_TIMEOUT;
            result = switch_raw_apfs_write(&f.backend, 0, bytes, 512);
        } else {
            f.begin_error = SWITCH_BLOCK_TIMEOUT;
            result = switch_raw_apfs_write(&f.backend, 0, bytes, 512);
            CHECK(!f.writes);
        }
        CHECK(result && f.offline && !f.owner);
        unsigned writes = f.writes;
        CHECK(switch_raw_apfs_write(&f.backend, 0, bytes, 512) == SWITCH_BLOCK_NOT_READY && f.writes == writes);
        CHECK(switch_raw_apfs_sync(&f.backend) == SWITCH_BLOCK_NOT_READY);
        close_backend(&f);
        cases++;
    }
    return cases;
}
