// boot/storage/firmware_export/export.c (iosw_fw_capture: the RAM reader that exports the pinned U-Boot's SD card
// state) against a synthetic RAM image built here around the pinned bl33.bin: the exact 256-byte record, capacity
// edges, a global-data pointer never dereferenced when invalid, every instruction fingerprint, bad global fields, heap
// mismatches, class, device, card-link, state and capacity faults, a failed read at every address it reads, and a
// snapshot that changes between its two reads. Nothing outside the fixture is ever read; failure leaves the output
// untouched.
#include <CommonCrypto/CommonDigest.h>
#include <gtest/gtest.h>
#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "bazel/testing/source_text.h"

extern "C" {
#include "../export.h"
#include "../signatures.h"
}

namespace {
constexpr uint64_t BASE = 0xf1000000, LOW = BASE - 0x402000, GD = LOW - 0x1000, END = BASE + 0x68000;
constexpr uint64_t UC0 = LOW + 0x1000, UC = LOW + 0x1100, BD0 = LOW + 0x2000, BD = LOW + 0x2200;
constexpr uint64_t DESC0 = LOW + 0x3000, DESC = LOW + 0x3200, PARENT = LOW + 0x4000, PLAT = LOW + 0x5000;
constexpr uint64_t MMC = PLAT + 0x20, PRIVATE = LOW + 0x6000, UPRIVATE = LOW + 0x7000;
constexpr uint64_t CAPACITY = 1023871549440ull / 512;

using Bytes = std::vector<uint8_t>;

const std::string &Bl33()
{
    static const std::string bytes = switch_ios::test::SourceText("../../../../bazel/pinned/firmware/bl33.bin");
    return bytes;
}

class FirmwareExport : public testing::Test {
protected:
    Bytes mem = Bytes(END - GD, 0);
    std::vector<std::pair<uint64_t, uint32_t>> accesses;
    std::optional<uint64_t> reject_at;
    std::function<void()> change_at_second;
    unsigned gd_reads = 0;
    iosw_fw_reader reader{this, Copy};

    void P16(uint64_t p, uint16_t v) { Put(p, v, 2); }
    void P32(uint64_t p, uint32_t v) { Put(p, v, 4); }
    void P64(uint64_t p, uint64_t v) { Put(p, v, 8); }
    void Put(uint64_t p, uint64_t v, unsigned width)
    {
        for (unsigned i = 0; i < width; ++i) mem[p - GD + i] = (uint8_t)(v >> (8 * i));
    }
    Bytes Get(uint64_t p, size_t n) const
    {
        return Bytes(mem.begin() + (long)(p - GD), mem.begin() + (long)(p - GD + n));
    }
    void Set(uint64_t p, const Bytes &b) { std::copy(b.begin(), b.end(), mem.begin() + (long)(p - GD)); }

    void SetUp() override
    {
        ASSERT_EQ(Bl33().size(), 390884u);
        std::memcpy(&mem[BASE - GD], Bl33().data(), Bl33().size());
        P64(GD + 8, 0x201), P64(GD + 0x58, BASE), P64(GD + 0x80, BASE - 0x88110000), P64(GD + 0x90, LOW + 0x8000);
        P64(BASE + 0x66cb0, LOW), P64(BASE + 0x66ca8, BASE);
        List(GD + 0xa0, {UC0 + 0x20, UC + 0x20});
        P64(UC0 + 8, BASE + 0x52470), P64(UC + 8, BASE + 0x523f0);
        List(UC + 0x10, {BD0 + 0x60, BD + 0x60});
        for (auto [dev, desc, num] : {std::tuple<uint64_t, uint64_t, uint32_t>{BD0, DESC0, 0}, {BD, DESC, 1}}) {
            P64(dev, BASE + 0x51c10), P64(dev + 0x20, desc), P64(dev + 0x38, PARENT);
            P64(dev + 0x48, UC), P32(dev + 0x90, 1);
            P32(desc, 6), P32(desc + 4, num), P64(desc + 0x80, dev);
        }
        P64(PARENT, BASE + 0x52048), P64(PARENT + 0x10, PLAT), P64(PARENT + 0x40, PRIVATE);
        P64(PARENT + 0x50, UPRIVATE), P32(PARENT + 0x90, 1);
        P64(PRIVATE, 0x700b0000), P64(UPRIVATE, MMC);
        P64(MMC, PLAT), P32(MMC + 8, 0x80030000), P64(MMC + 0x10, PARENT);
        P32(MMC + 0x18, 1), P32(MMC + 0x1c, 1), P32(MMC + 0x20, 4), P32(MMC + 0x24, 48000000);
        P32(MMC + 0x2c, 0xc0ff8000), P32(MMC + 0x40, 0x40000000);
        for (unsigned i = 0; i < 16; ++i) mem[MMC + 0x50 - GD + i] = (uint8_t)(i + 1);
        P16(MMC + 0x60, 0xabcd), P32(MMC + 0x6c, 512), P32(MMC + 0x70, 512), P64(MMC + 0xe0, PARENT);
        Capacity(CAPACITY);
    }

    void Capacity(uint64_t sectors)
    {
        const uint64_t csize = (sectors >> 10) - 1;
        P32(MMC + 0x44, (uint32_t)((9u << 16) | (csize >> 16)));
        P32(MMC + 0x48, (uint32_t)((csize & 0xffff) << 16));
        P64(MMC + 0x88, sectors * 512), P64(MMC + 0x90, sectors * 512);
        P64(DESC + 0x10, sectors), P64(DESC + 0x18, 512), P32(DESC + 0x20, 9);
    }

    // A circular list: head, then nodes, each pointing at the next and back at the previous.
    void List(uint64_t head, std::initializer_list<uint64_t> nodes)
    {
        std::vector<uint64_t> all{head};
        all.insert(all.end(), nodes);
        for (size_t i = 0; i < all.size(); ++i) {
            P64(all[i], all[(i + 1) % all.size()]);
            P64(all[i] + 8, all[(i + all.size() - 1) % all.size()]);
        }
    }

    static int Copy(void *context, uint64_t p, uint8_t *out, uint32_t n)
    {
        auto *self = static_cast<FirmwareExport *>(context);
        self->accesses.push_back({p, n});
        if (p == GD) {
            ++self->gd_reads;
            if (self->gd_reads == 2 && self->change_at_second) self->change_at_second();
        }
        if (self->reject_at && p <= *self->reject_at && *self->reject_at < p + n) return 1;
        if (!(GD <= p && p <= END - n)) {
            ADD_FAILURE() << "out of fixture read " << std::hex << p;
            return 1;
        }
        std::memcpy(out, &self->mem[p - GD], n);
        return 0;
    }

    Bytes RunCapture(int expected = 0, uint64_t gd = GD)
    {
        Bytes out(256, 0xa5);
        EXPECT_EQ(iosw_fw_capture(&reader, gd, out.data()), expected);
        if (expected) EXPECT_EQ(out, Bytes(256, 0xa5)) << "failure wrote the record";
        return out;
    }

    // Each (address, value): set it (`width` bytes), expect `code`, restore it.
    void Faults(std::initializer_list<std::pair<uint64_t, uint64_t>> faults, int code, unsigned width = 8)
    {
        for (auto [p, v] : faults) {
            SCOPED_TRACE(testing::Message() << std::hex << "0x" << p << " = 0x" << v);
            const Bytes old = Get(p, width);
            Put(p, v, width);
            RunCapture(code);
            Set(p, old);
        }
    }
};

void Le(Bytes &b, size_t at, uint64_t v, unsigned width)
{
    for (unsigned i = 0; i < width; ++i) b[at + i] = (uint8_t)(v >> (8 * i));
}
}  // namespace

TEST_F(FirmwareExport, ExactSuccessRecord)
{
    const Bytes out = RunCapture();
    Bytes expected(256, 0);
    const uint32_t words[8] = {0x4d435349, 1, 256, 1, 0x700b0000, 1, 512, 1};
    for (unsigned i = 0; i < 8; ++i) Le(expected, 4 * i, words[i], 4);
    Le(expected, 32, CAPACITY, 8);
    Le(expected, 40, CAPACITY * 512, 8);
    const uint32_t tail[4] = {0xc0ff8000, 0xabcd, 0x80030000, 0};
    for (unsigned i = 0; i < 4; ++i) Le(expected, 48 + 4 * i, tail[i], 4);
    const Bytes card = Get(MMC + 0x40, 0x20);
    std::copy(card.begin(), card.end(), expected.begin() + 0x40);
    const uint64_t links[4] = {MMC, DESC, GD, BASE - 0x88110000};
    for (unsigned i = 0; i < 4; ++i) Le(expected, 0x60 + 8 * i, links[i], 8);
    uint8_t digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(Bl33().data(), (CC_LONG)Bl33().size(), digest);
    std::memcpy(&expected[0x80], digest, 32);
    EXPECT_EQ(std::memcmp(digest, iosw_fw_sha256, 32), 0) << "signatures.h names the pinned bl33.bin";
    Le(expected, 0xf0, (uint32_t)crc32(0, expected.data(), 0xf0), 4);
    EXPECT_EQ(out, expected);
    EXPECT_EQ(gd_reads, 2u);
    for (auto [p, n] : accesses)
        EXPECT_TRUE(0x80000000 <= p && p < 0x100000000 && p + n <= 0x100000000) << std::hex << p << " + " << n;
}

TEST_F(FirmwareExport, CapacityEdges)
{
    for (uint64_t cap : {1024ull, 0x100000000ull}) {
        SCOPED_TRACE(testing::Message() << cap << " sectors");
        Capacity(cap);
        RunCapture();
    }
}

TEST_F(FirmwareExport, InvalidGlobalDataNeverDereferenced)
{
    for (uint64_t gd : {0ull, 0x700b0000ull, 0x7fffffffull, GD + 1, 0x100000000ull, 0xfffffff8ull}) {
        SCOPED_TRACE(testing::Message() << std::hex << "gd 0x" << gd);
        accesses.clear();
        RunCapture(2, gd);
        EXPECT_TRUE(accesses.empty());
    }
}

TEST_F(FirmwareExport, EveryInstructionFingerprint)
{
    const size_t count = sizeof(iosw_fw_words) / sizeof(iosw_fw_words[0]);
    EXPECT_GT(count, 100u);
    for (size_t i = 0; i < count; ++i) {
        const uint64_t p = BASE + iosw_fw_words[i].offset;
        SCOPED_TRACE(testing::Message() << std::hex << "fingerprint at +0x" << iosw_fw_words[i].offset);
        // The fingerprint is the pinned file's own word there.
        uint32_t stored;
        std::memcpy(&stored, &mem[p - GD], 4);
        EXPECT_EQ(stored, iosw_fw_words[i].word);
        P32(p, iosw_fw_words[i].word ^ 1);
        RunCapture(3);
        P32(p, iosw_fw_words[i].word);
    }
}

TEST_F(FirmwareExport, BadGlobalFields)
{
    Faults({{GD + 8, 1},
            {GD + 0x90, 0},
            {GD + 0x58, BASE + 8},
            {GD + 0x80, 0},
            {GD + 0x58, 0x70000000},
            {GD + 0x58, 0xffff0000}},
           2);
}

TEST_F(FirmwareExport, HeapMismatch)
{
    Faults({{BASE + 0x66cb0, LOW + 8}, {BASE + 0x66ca8, LOW + 8}}, 4);
}

TEST_F(FirmwareExport, ClassFaults)
{
    Faults({{GD + 0xa0, 0x700b0000},
            {UC + 8, BASE + 0x52470},
            {UC + 0x28, 0},
            {UC + 0x20, UC + 0x20},
            {GD + 0xa8, UC0 + 0x20}},
           5);
}

TEST_F(FirmwareExport, DeviceFaults)
{
    Faults({{UC + 0x10, 0x700b0000},
            {BD + 0x20, 0x700b0000},
            {BD + 0x68, 0},
            {BD + 0x48, UC0},
            {BD, BASE + 0x52048},
            {DESC + 0x80, BD0},
            {UC + 0x18, BD0 + 0x60}},
           6);
    P32(DESC0 + 4, 1);
    RunCapture(6);
}

TEST_F(FirmwareExport, CardLinks)
{
    Faults({{BD + 0x38, 0x700b0000},
            {PARENT, BASE + 0x51c10},
            {PARENT + 0x90, 0},
            {PARENT + 0x10, 0x700b0000},
            {PARENT + 0x40, 0x700b0000},
            {PRIVATE, 0x700b0600},
            {UPRIVATE, MMC + 8},
            {MMC, 0},
            {MMC + 0x10, 0},
            {MMC + 0xe0, 0}},
           7);
}

TEST_F(FirmwareExport, StateRefusals)
{
    Faults({{MMC + 0x18, 0},
            {MMC + 0x1c, 0},
            {MMC + 8, 0x30000},
            {MMC + 0x2c, 0x80ff8000},
            {MMC + 0x2c, 0x40ff8000},
            {MMC + 0x60, 0},
            {MMC + 0x40, 0},
            {MMC + 0x40, 0x80000000},
            {MMC + 0x20, 8},
            {MMC + 0x24, 0},
            {MMC + 0x6c, 1024},
            {MMC + 0x70, 1024},
            {MMC + 0xdc, 1},
            {MMC + 0xd8, 1},
            {MMC + 0xd8, 256},
            {DESC + 0x18, 1024},
            {DESC + 0x20, 10}},
           8, 4);
}

TEST_F(FirmwareExport, CapacityMismatches)
{
    Faults({{MMC + 0x44, 0}, {MMC + 0x88, 0}, {MMC + 0x90, 0}, {DESC + 0x10, CAPACITY - 1}}, 9);
}

TEST_F(FirmwareExport, EveryReadCanFailWithoutOutput)
{
    RunCapture();
    std::set<uint64_t> points;
    for (auto [p, n] : accesses) points.insert(p);
    for (uint64_t p : points) {
        SCOPED_TRACE(testing::Message() << std::hex << "failing the read at 0x" << p);
        reject_at = p;
        Bytes out(256, 0xa5);
        EXPECT_NE(iosw_fw_capture(&reader, GD, out.data()), 0);
        EXPECT_EQ(out, Bytes(256, 0xa5));
    }
    reject_at.reset();
}

TEST_F(FirmwareExport, ChangedValidSnapshot)
{
    change_at_second = [this] { P16(MMC + 0x60, 0xabce); };
    RunCapture(10);
}

TEST_F(FirmwareExport, ChangedInvalidSnapshot)
{
    change_at_second = [this] { P32(MMC + 0x1c, 0); };
    RunCapture(8);
}
