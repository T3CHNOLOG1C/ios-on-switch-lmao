// native/storage/partition.c (iosw_gpt_find: GPT qualification before the loader trusts a partition) against
// independent sparse GPT images (GptImage below) and zlib's CRC-32: both headers and arrays, every geometry, count,
// size and CRC field, agreement between the copies, every used entry's bounds, uniqueness and overlap, a callback
// failure at every metadata sector, 64-bit capacities, and output written only on success.
#include <gtest/gtest.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

extern "C" {
#include "../partition.h"
}

namespace {
enum { OK, ARG, IO, MBR, HEADER, HEADER_CRC, GEOMETRY, ENTRIES, ARRAY_CRC, MISMATCH, NOT_FOUND, OVERLAP, DUPLICATE };

using Bytes = std::vector<uint8_t>;
using Guid = std::array<uint8_t, 16>;

// A UUID's mixed-endian on-disk bytes (Python's uuid.UUID(...).bytes_le).
Guid BytesLe(const std::array<uint8_t, 16> &big)
{
    Guid g = big;
    std::swap(g[0], g[3]), std::swap(g[1], g[2]), std::swap(g[4], g[5]), std::swap(g[6], g[7]);
    return g;
}
Guid Uuid(const char *text)
{
    std::array<uint8_t, 16> big{};
    unsigned n = 0;
    for (const char *p = text; *p && n < 32; ++p) {
        if (*p == '-') continue;
        const unsigned digit = (unsigned)(*p <= '9' ? *p - '0' : (*p | 0x20) - 'a' + 10);
        big[n / 2] = (uint8_t)(big[n / 2] << 4 | digit);
        ++n;
    }
    return BytesLe(big);
}
Guid UuidInt(uint64_t value)
{
    std::array<uint8_t, 16> big{};
    for (unsigned i = 0; i < 8; ++i) big[15 - i] = (uint8_t)(value >> (8 * i));
    return BytesLe(big);
}

const Guid TARGET = Uuid("2FBBA66A-A356-00C1-CD9F-1DCBB51C2CA9");
const Guid BASIC = Uuid("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7");
const Guid ESP = Uuid("C12A7328-F81F-11D2-BA4B-00A0C93EC93B");
constexpr uint64_t TARGET_SECTORS = 886121168896ull / 512;

void Put(Bytes &b, size_t at, uint64_t v, unsigned width)
{
    for (unsigned i = 0; i < width; ++i) b[at + i] = (uint8_t)(v >> (8 * i));
}
uint32_t Crc(const uint8_t *p, size_t n)
{
    return (uint32_t)crc32(0, p, (uInt)n);
}

void HeaderCrc(Bytes &data)
{
    Put(data, 16, 0, 4);
    Put(data, 16, Crc(data.data(), 92), 4);
}

struct GptImage {
    uint64_t capacity;
    uint32_t target_index, entry_size, entries, length, blocks;
    std::array<uint64_t, 2> array_lbas;
    uint64_t first_usable, last_usable;
    std::array<Bytes, 2> arrays;
    Bytes mbr = Bytes(512, 0);
    std::array<Bytes, 2> headers;
    std::vector<uint64_t> reads, out_of_bounds;
    std::set<uint64_t> failed;
    std::array<uint8_t, 2> padding{0, 0};

    explicit GptImage(uint32_t entrySize = 128, uint32_t entryCount = 128, uint64_t capacityIn = 2000000000,
                      uint32_t targetIndex = 2)
        : capacity(capacityIn), target_index(targetIndex), entry_size(entrySize), entries(entryCount)
    {
        length = entry_size * entries;
        blocks = (length + 511) / 512;
        const uint64_t reserved_blocks = std::max<uint64_t>(32, blocks);
        array_lbas = {2, capacity - 1 - reserved_blocks};
        first_usable = 2 + reserved_blocks;
        last_usable = array_lbas[1] - 1;
        arrays = {Bytes(length, 0), Bytes(length, 0)};
        mbr[450] = 0xee;
        Put(mbr, 454, 1, 4);
        Put(mbr, 458, std::min<uint64_t>(capacity - 1, 0xFFFFFFFFu), 4);
        mbr[510] = 0x55, mbr[511] = 0xaa;
        const Guid disk = Uuid("45d021ce-459b-41b1-ac7f-a84269d4fb7a");
        for (unsigned i = 0; i < 2; ++i) {
            Bytes data(512, 0);
            std::memcpy(data.data(), "EFI PART", 8);
            Put(data, 8, 0x10000, 4);
            Put(data, 12, 92, 4);
            Put(data, 16, 0, 4);
            Put(data, 20, 0, 4);
            Put(data, 24, i ? capacity - 1 : 1, 8);
            Put(data, 32, i ? 1 : capacity - 1, 8);
            Put(data, 40, first_usable, 8);
            Put(data, 48, last_usable, 8);
            std::memcpy(&data[56], disk.data(), 16);
            Put(data, 72, array_lbas[i], 8);
            Put(data, 80, entries, 4);
            Put(data, 84, entry_size, 4);
            Put(data, 88, 0, 4);
            headers[i] = data;
        }
        SetPartition(0, ESP, UuidInt(100), 1000, 2000);
        SetPartition(target_index, BASIC, TARGET, 32768, 32768 + TARGET_SECTORS - 1);
        FinishArrays();
    }

    void SetPartition(uint32_t index, const Guid &type, const Guid &unique, uint64_t first, uint64_t last)
    {
        Bytes entry(entry_size, 0);
        std::memcpy(&entry[0], type.data(), 16);
        std::memcpy(&entry[16], unique.data(), 16);
        Put(entry, 32, first, 8);
        Put(entry, 40, last, 8);
        Put(entry, 48, 0, 8);
        const char16_t name[] = u"TEST";
        for (unsigned c = 0; c < 4; ++c) Put(entry, 56 + 2 * c, name[c], 2);
        for (Bytes &array : arrays) std::memcpy(&array[(size_t)index * entry_size], entry.data(), entry_size);
    }

    void FinishArrays()
    {
        for (unsigned i = 0; i < 2; ++i) {
            Put(headers[i], 88, Crc(arrays[i].data(), arrays[i].size()), 4);
            HeaderCrc(headers[i]);
        }
    }

    void Field(unsigned index, size_t offset, unsigned width, uint64_t value)
    {
        Put(headers[index], offset, value, width);
        HeaderCrc(headers[index]);
    }

    Bytes Sector(uint64_t lba) const
    {
        if (lba == 0) return mbr;
        if (lba == 1) return headers[0];
        if (lba == capacity - 1) return headers[1];
        for (unsigned i = 0; i < 2; ++i)
            if (array_lbas[i] <= lba && lba < array_lbas[i] + blocks) {
                const size_t start = (size_t)(lba - array_lbas[i]) * 512;
                Bytes chunk(arrays[i].begin() + (long)start,
                            arrays[i].begin() + (long)std::min<size_t>(start + 512, arrays[i].size()));
                chunk.resize(512, padding[i]);
                return chunk;
            }
        ADD_FAILURE() << "unexpected GPT read at " << lba;
        return Bytes(512, 0);
    }

    int Callback(uint64_t lba, uint8_t *output)
    {
        reads.push_back(lba);
        if (lba >= capacity) {
            out_of_bounds.push_back(lba);
            return 1;
        }
        if (failed.count(lba)) {
            std::memset(output, 0xee, 512);
            return 1;
        }
        const Bytes sector = Sector(lba);
        std::memcpy(output, sector.data(), 512);
        return 0;
    }
};

GptImage *gImage;
int Read512(void *, uint64_t lba, uint8_t output[512])
{
    return gImage->Callback(lba, output);
}

class NativePartition : public testing::Test {
protected:
    GptImage image;
    iosw_gpt_workspace workspace{};

    void SetUp() override { gImage = &image; }
    void Reset(GptImage next)
    {
        image = std::move(next);
        gImage = &image;
    }

    // Finds `guid` (nullptr: none given); `expected` OK commits exactly the target slice, anything else nothing.
    iosw_gpt_slice Find(int expected = OK, std::optional<Guid> guid = TARGET, bool noGuid = false,
                        std::optional<uint64_t> capacity = std::nullopt)
    {
        iosw_gpt_slice output;
        std::memset(&output, 0xaa, sizeof(output));
        iosw_gpt_slice before;
        std::memcpy(&before, &output, sizeof(output));
        EXPECT_EQ(iosw_gpt_find(Read512, nullptr, capacity ? *capacity : image.capacity,
                                noGuid ? nullptr : guid->data(), &workspace, &output),
                  expected);
        if (expected) {
            EXPECT_EQ(std::memcmp(&output, &before, sizeof(output)), 0) << "failure committed output";
        } else {
            EXPECT_EQ(output.first_sector, 32768u);
            EXPECT_EQ(output.sector_count, TARGET_SECTORS);
            EXPECT_EQ(output.partition_index, image.target_index);
            EXPECT_EQ(std::memcmp(output.unique_guid, TARGET.data(), 16), 0);
        }
        EXPECT_TRUE(image.out_of_bounds.empty());
        return output;
    }
};
}  // namespace

TEST_F(NativePartition, ValidPrimaryBackupExactGuidAndHybridMbr)
{
    Find();
    EXPECT_EQ(image.reads.size(), 67u);
    ASSERT_GE(image.reads.size(), 3u);
    EXPECT_EQ(std::vector<uint64_t>(image.reads.begin(), image.reads.begin() + 3),
              (std::vector<uint64_t>{0, 1, image.capacity - 1}));
    EXPECT_LT(sizeof(iosw_gpt_workspace), 6000u);
    image.mbr[466] = 0x0c;
    image.mbr[462] = 0x80;  // A hybrid entry is not interpreted by GPT selection.
    Find();
}

TEST_F(NativePartition, LargerEntriesAndPartialLastSectorCrcCoverage)
{
    for (auto [size, count] : {std::pair<uint32_t, uint32_t>{256, 64}, {256, 65}, {512, 32}, {512, 128}}) {
        SCOPED_TRACE(testing::Message() << size << "-byte entries x " << count);
        Reset(GptImage(size, count));
        image.padding = {0x11, 0x99};
        Find();
        EXPECT_EQ(image.reads.size(), 3u + 2u * image.blocks);
        EXPECT_LE(image.reads.size(), 259u);
    }
}

TEST_F(NativePartition, AllBoundedCountsAndSizesWithExactDeclaredCrcBytes)
{
    for (uint32_t size : {128u, 256u, 512u})
        for (uint32_t count = 1; count <= 128; ++count) {
            SCOPED_TRACE(testing::Message() << size << "-byte entries x " << count);
            Reset(GptImage(size, count, 2000000000, 0));
            // These differing trailing bytes are not declared entries and must not be included in CRCs or copy
            // comparisons.
            image.padding = {0x11, 0x99};
            Find();
            EXPECT_EQ(image.reads.size(), 3u + 2u * image.blocks);
            EXPECT_LE(image.reads.size(), 259u);
        }
}

TEST_F(NativePartition, HekateCreatedEntryCountAndFullReservedTable)
{
    // Pinned Hekate formatter declares gpt_idx entries, CRCs only those bytes, writes 128 entries, and places backup
    // array at capacity-33. Eleven is a synthetic example, not an observed card-header count.
    Reset(GptImage(128, 11, 0x7731c000, 0));
    image.last_usable = image.capacity - 0x800 - 1;
    for (unsigned index = 0; index < 2; ++index) image.Field(index, 48, 8, image.last_usable);
    EXPECT_EQ(image.array_lbas, (std::array<uint64_t, 2>{2, image.capacity - 33}));
    Find();
    EXPECT_EQ(image.reads.size(), 9u);
}

TEST_F(NativePartition, ShortTableRetainsReservationIntegrityAndEntryBounds)
{
    const std::vector<std::tuple<const char *, std::function<void(GptImage &)>, int>> cases{
        {"short_primary_reservation", [](GptImage &im) { im.Field(0, 72, 8, 3); }, GEOMETRY},
        {"short_backup_reservation", [](GptImage &im) { im.Field(1, 72, 8, im.capacity - 32); }, GEOMETRY},
        {"bad_header_crc", [](GptImage &im) { im.headers[0][16] ^= 1; }, HEADER_CRC},
        {"different_array_bytes", [](GptImage &im) { im.arrays[1][56] ^= 1; }, MISMATCH},
        {"zero_count", [](GptImage &im) { im.Field(0, 80, 4, 0); }, ENTRIES},
        {"oversized_count", [](GptImage &im) { im.Field(0, 80, 4, 129); }, ENTRIES},
        {"unsupported_entry_size", [](GptImage &im) { im.Field(0, 84, 4, 136); }, ENTRIES},
        {"header_count_mismatch", [](GptImage &im) { im.Field(1, 80, 4, 12); }, MISMATCH},
        {"used_entry_out_of_bounds", [](GptImage &im) { im.SetPartition(0, BASIC, TARGET, 1, 100); }, ENTRIES},
    };
    for (const auto &[name, mutate, expected] : cases) {
        SCOPED_TRACE(name);
        Reset(GptImage(128, 11, 2000000000, 0));
        mutate(image);
        Find(expected);
    }
    Reset(GptImage(128, 11, 2000000000, 0));
    for (Bytes &array : image.arrays) array[56] ^= 1;
    Find(ARRAY_CRC);  // Agreeing copies cannot bypass their CRCs.
}

TEST_F(NativePartition, ZeroUnknownGuidAndWrongTargetType)
{
    Find(ARG, Guid{});
    Find(ARG, std::nullopt, true);
    Find(NOT_FOUND, UuidInt(444));
    image.SetPartition(2, ESP, TARGET, 32768, 32768 + TARGET_SECTORS - 1);
    image.FinishArrays();
    Find(NOT_FOUND);
}

TEST_F(NativePartition, ProtectiveSignatureAndTypeRequired)
{
    image.mbr[510] = 0;
    Find(MBR);
    image.mbr[510] = 0x55;
    image.mbr[450] = 0x0c;
    Find(MBR);
    EXPECT_EQ(image.reads, (std::vector<uint64_t>{0, 0}));
}

TEST_F(NativePartition, HeaderCrcAndFormatBothCopies)
{
    for (unsigned index = 0; index < 2; ++index)
        for (auto [offset, value, expected] : {std::tuple<size_t, uint8_t, int>{0, 0, HEADER},
                                               {8, 2, HEADER},
                                               {12, 91, HEADER},
                                               {20, 1, HEADER},
                                               {92, 1, HEADER},
                                               {16, 0, HEADER_CRC}}) {
            SCOPED_TRACE(testing::Message() << "header " << index << " byte " << offset << " = " << (int)value);
            Reset(GptImage());
            image.headers[index][offset] = value;
            Find(expected);
        }
}

TEST_F(NativePartition, HeaderGeometryAndArraySizeLimits)
{
    struct Case {
        size_t offset;
        unsigned width;
        uint64_t value;
        int expected;
    };
    const std::vector<Case> cases{
        {24, 8, 123, GEOMETRY},        {32, 8, 123, GEOMETRY},        {40, 8, 33, GEOMETRY},
        {40, 8, UINT64_MAX, GEOMETRY}, {48, 8, UINT64_MAX, GEOMETRY}, {48, 8, 32, GEOMETRY},
        {80, 4, 0, ENTRIES},           {80, 4, 129, ENTRIES},         {80, 4, 0xFFFFFFFF, ENTRIES},
        {80, 4, 127, MISMATCH},        {84, 4, 0, ENTRIES},           {84, 4, 136, ENTRIES},
        {84, 4, 1024, ENTRIES},        {84, 4, 0xFFFFFFFF, ENTRIES}};
    for (unsigned index = 0; index < 2; ++index)
        for (const Case &c : cases) {
            SCOPED_TRACE(testing::Message() << "header " << index << " offset " << c.offset << " = " << c.value);
            Reset(GptImage());
            image.Field(index, c.offset, c.width, c.value);
            Find(c.expected);
        }
    const GptImage probe;
    for (auto [index, value] : {std::pair<unsigned, uint64_t>{0, 0},
                                {0, 1},
                                {0, 3},
                                {0, 34},
                                {0, UINT64_MAX},
                                {1, probe.last_usable},
                                {1, probe.capacity - 1},
                                {1, probe.capacity - 2}}) {
        SCOPED_TRACE(testing::Message() << "header " << index << " entry LBA " << value);
        Reset(GptImage());
        image.Field(index, 72, 8, value);
        Find(GEOMETRY);
    }
    Find(ARG, TARGET, false, 67);
}

TEST_F(NativePartition, HeaderPairDisagreementAndZeroDiskGuid)
{
    for (auto [offset, width, value] : {std::tuple<size_t, unsigned, uint64_t>{40, 8, 35},
                                        {48, 8, image.last_usable - 1},
                                        {56, 4, 123},
                                        {88, 4, 123}}) {
        SCOPED_TRACE(testing::Message() << "backup header offset " << offset);
        Reset(GptImage());
        image.Field(1, offset, width, value);
        Find(MISMATCH);
    }
    Reset(GptImage());
    std::memset(&image.headers[0][56], 0, 16);
    HeaderCrc(image.headers[0]);
    Find(GEOMETRY);
}

TEST_F(NativePartition, ArrayByteDisagreementAndIndependentArrayCrcValidation)
{
    image.arrays[1][56] ^= 1;
    Find(MISMATCH);
    Reset(GptImage());
    for (Bytes &array : image.arrays) array[56] ^= 1;  // Both copies agree, but stored CRC is stale.
    Find(ARRAY_CRC);
    EXPECT_EQ(image.reads.size(), 67u);
}

TEST_F(NativePartition, EveryUsedEntryBoundsGuidUniquenessAndNoOverlap)
{
    const uint64_t last_usable = image.last_usable;
    for (auto [first, last, expected] : {std::tuple<uint64_t, uint64_t, int>{0, 100, ENTRIES},
                                         {1000, 999, ENTRIES},
                                         {2000, 2001, OVERLAP},
                                         {32768, 32768, OVERLAP},
                                         {last_usable, last_usable + 1, ENTRIES},
                                         {UINT64_MAX, UINT64_MAX, ENTRIES}}) {
        SCOPED_TRACE(testing::Message() << "entry 127 at " << first << ".." << last);
        Reset(GptImage());
        image.SetPartition(127, ESP, UuidInt(200), first, last);
        image.FinishArrays();
        Find(expected);
    }
    Reset(GptImage());
    image.SetPartition(127, ESP, TARGET, 3000, 4000);
    image.FinishArrays();
    Find(DUPLICATE);
    Reset(GptImage());
    image.SetPartition(127, ESP, Guid{}, 3000, 4000);
    image.FinishArrays();
    Find(ENTRIES);
}

TEST_F(NativePartition, PartitionAdjacencyIsAllowedAndExtendedReservedBytesAreZero)
{
    image.SetPartition(127, ESP, UuidInt(200), 2001, 32767);
    image.FinishArrays();
    Find();
    Reset(GptImage(256, 64));
    for (Bytes &array : image.arrays) array[128] = 1;
    image.FinishArrays();
    Find(ENTRIES);
}

TEST_F(NativePartition, CallbackFailureAtEveryMetadataSectorLeavesOutputUncommitted)
{
    std::vector<uint64_t> lbas{0, 1, image.capacity - 1};
    for (uint64_t base : image.array_lbas)
        for (uint64_t lba = base; lba < base + image.blocks; ++lba) lbas.push_back(lba);
    for (uint64_t lba : lbas) {
        SCOPED_TRACE(testing::Message() << "failing LBA " << lba);
        image.failed = {lba};
        Find(IO);
        ASSERT_FALSE(image.reads.empty());
        EXPECT_EQ(image.reads.back(), lba);
    }
}

TEST_F(NativePartition, CapacityAndBackupLbaAre64Bit)
{
    Reset(GptImage(128, 128, (1ull << 40) + 12345));
    Find();
    EXPECT_NE(std::find(image.reads.begin(), image.reads.end(), image.capacity - 1), image.reads.end());
    Reset(GptImage(128, 128, UINT64_MAX));
    Find();
    EXPECT_NE(std::find(image.reads.begin(), image.reads.end(), UINT64_MAX - 1), image.reads.end());
}
