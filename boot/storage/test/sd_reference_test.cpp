// The fixed, same-boot sector-reference page (boot/storage/sd_reference.c) checked independently on the host: the
// U-Boot byte contract (header words, a big-endian CRC-32 of sector 0, the loader's commit word, zero padding) built
// here with zlib's crc32, every field and reserved byte, no partial output, no write to the page, no read outside it.
#include <gtest/gtest.h>
#include <sys/mman.h>
#include <unistd.h>
#include <zlib.h>

#include <array>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

extern "C" {
#include "../sd_reference.h"
}

namespace {
constexpr uint32_t PAGE_BYTES = 4096, DATA_OFFSET = 512, DATA_BYTES = 512;
constexpr std::array<uint32_t, 10> HEADER{0x52445349, 1, 0x400, 0x200, 0x200, 0x700B0000, 0, 1, 1, 0};

using Bytes = std::vector<uint8_t>;

uint32_t Crc(const uint8_t *data, size_t size)
{
    return (uint32_t)crc32(0, data, (uInt)size);
}

void PutLe(Bytes &page, size_t at, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i) page[at + i] = (uint8_t)(v >> (8 * i));
}
void PutBe(Bytes &page, size_t at, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i) page[at + i] = (uint8_t)(v >> (24 - 8 * i));
}

Bytes Counting()
{
    Bytes sector(DATA_BYTES);
    for (size_t i = 0; i < sector.size(); ++i) sector[i] = (uint8_t)i;
    return sector;
}

// The page as U-Boot writes it (deliberately following the byte contract, independent of the C).
Bytes MakePage(const Bytes &sector = Counting(), uint32_t commit = 0)
{
    Bytes page(PAGE_BYTES);
    for (size_t i = 0; i < HEADER.size(); ++i) PutLe(page, 4 * i, HEADER[i]);
    PutBe(page, 0x28, Crc(sector.data(), sector.size()));
    PutLe(page, 0x2C, commit);
    std::memcpy(page.data() + DATA_OFFSET, sector.data(), DATA_BYTES);
    return page;
}

struct Guarded {
    uint8_t before[16];
    iosw_sd_reference result;
    uint8_t after[16];
};

// The validator over a copy of `page` (exactly its size: ASan sees any read past it), into guarded output.
void Invoke(const Bytes &page, uint32_t expectedCommit = 0, uint32_t length = PAGE_BYTES, int expectedError = 0)
{
    auto raw = std::make_unique<uint8_t[]>(page.size());
    std::memcpy(raw.get(), page.data(), page.size());
    Guarded output;
    std::memset(&output, 0xA5, sizeof(output));
    Guarded before;
    std::memcpy(&before, &output, sizeof(output));
    const int error = iosw_sd_reference_validate(raw.get(), length, expectedCommit, &output.result);
    EXPECT_EQ(error, expectedError);
    EXPECT_EQ(std::memcmp(raw.get(), page.data(), page.size()), 0) << "validator modified input";
    for (unsigned i = 0; i < 16; ++i) {
        EXPECT_EQ(output.before[i], 0xA5);
        EXPECT_EQ(output.after[i], 0xA5);
    }
    if (error) {
        EXPECT_EQ(std::memcmp(&output, &before, sizeof(output)), 0) << "failure committed output";
    } else {
        EXPECT_EQ(std::memcmp(output.result.sector, page.data() + DATA_OFFSET, DATA_BYTES), 0);
        EXPECT_EQ(output.result.crc32, Crc(page.data() + DATA_OFFSET, DATA_BYTES));
    }
}
}  // namespace

TEST(SdReference, ValidLoaderAndKernelCommitsAndSectorPatterns)
{
    std::mt19937 rng(0x49534452);
    Bytes random(DATA_BYTES);
    for (uint8_t &b : random) b = (uint8_t)rng();
    const std::vector<Bytes> patterns{Bytes(DATA_BYTES, 0), Bytes(DATA_BYTES, 0xFF), Counting(), random};
    for (uint32_t commit : {0u, 1u})
        for (const Bytes &sector : patterns) {
            SCOPED_TRACE(testing::Message() << "commit " << commit << ", crc " << Crc(sector.data(), sector.size()));
            Invoke(MakePage(sector, commit), commit);
        }
}

TEST(SdReference, CrcAgainstZlibKnownVectorsAndNonWordLengths)
{
    std::mt19937 rng(0x53444352);
    std::vector<Bytes> vectors{Bytes(), Bytes{'1', '2', '3', '4', '5', '6', '7', '8', '9'}, Bytes(512, 0),
                               Bytes(512, 0xFF)};
    for (size_t length : {1, 2, 3, 4, 7, 8, 15, 16, 31, 32, 63, 64, 127, 128, 255, 256, 511, 512, 513, 1023, 4096}) {
        Bytes value(length);
        for (uint8_t &b : value) b = (uint8_t)rng();
        vectors.push_back(value);
    }
    for (const Bytes &value : vectors) {
        auto raw = std::make_unique<uint8_t[]>(value.size() + 1);  // a C string's room
        if (!value.empty()) std::memcpy(raw.get(), value.data(), value.size());
        EXPECT_EQ(iosw_sd_crc32(raw.get(), (uint32_t)value.size()), Crc(value.data(), value.size()))
            << value.size() << " bytes";
    }
    EXPECT_EQ(iosw_sd_crc32(nullptr, 0), 0u);
}

// A one-byte input is enough: every wrong size must fail before a header read (ASan reports any read past it).
TEST(SdReference, RejectsWrongPageSizeBeforeReadingAnyPageBytes)
{
    for (uint32_t length : {0u, 1u, 511u, 512u, 1024u, 4095u, 4097u, 0xFFFFFFFFu}) Invoke(Bytes{0xEE}, 0, length, 1);
}

TEST(SdReference, RejectsNullArgumentsAndInvalidExpectedCommit)
{
    Guarded output;
    std::memset(&output, 0xA5, sizeof(output));
    Guarded before;
    std::memcpy(&before, &output, sizeof(output));
    EXPECT_EQ(iosw_sd_reference_validate(nullptr, 4096, 0, &output.result), 1);
    EXPECT_EQ(std::memcmp(&output, &before, sizeof(output)), 0);
    const Bytes page = MakePage();
    EXPECT_EQ(iosw_sd_reference_validate(page.data(), 4096, 0, nullptr), 1);
    for (uint32_t commit : {2u, 0xFFFFFFFFu}) Invoke(Bytes{0xEE}, commit, PAGE_BYTES, 1);
}

TEST(SdReference, EveryHeaderWordAndEachHighBitIsFixed)
{
    for (size_t word = 0; word < 10; ++word)
        for (unsigned bit : {0u, 7u, 15u, 23u, 31u}) {
            SCOPED_TRACE(testing::Message() << "word " << word << ", bit " << bit);
            Bytes page = MakePage();
            PutLe(page, word * 4, HEADER[word] ^ (1u << bit));
            Invoke(page, 0, PAGE_BYTES, 2);
        }
    // These specifically distinguish whole-device sector0 from partition data or a guessed SDHC/eMMC descriptor.
    for (auto [offset, value] : {std::pair<size_t, uint32_t>{0x14, 0x700B0600},
                                 {0x18, 1},
                                 {0x1C, 2},
                                 {0x20, 0},
                                 {0x24, 1},
                                 {0x0C, 0x1000},
                                 {0x10, 0xFFFFFFFF}}) {
        SCOPED_TRACE(testing::Message() << "offset " << offset);
        Bytes page = MakePage();
        PutLe(page, offset, value);
        Invoke(page, 0, PAGE_BYTES, 2);
    }
}

TEST(SdReference, CommitMustExactlyMatchTheCallersPhase)
{
    Invoke(MakePage(Counting(), 0), 1, PAGE_BYTES, 2);
    Invoke(MakePage(Counting(), 1), 0, PAGE_BYTES, 2);
    for (uint32_t commit : {2u, 0xFFFFFFFFu}) Invoke(MakePage(Counting(), commit), 0, PAGE_BYTES, 2);
}

TEST(SdReference, EveryReservedByteMustBeZero)
{
    Bytes page = MakePage();
    std::vector<size_t> offsets;
    for (size_t at = 0x30; at < 0x200; ++at) offsets.push_back(at);
    for (size_t at = 0x400; at < 0x1000; ++at) offsets.push_back(at);
    for (size_t offset : offsets) {
        SCOPED_TRACE(testing::Message() << "offset 0x" << std::hex << offset);
        page[offset] = 1;
        Invoke(page, 0, PAGE_BYTES, 3);
        page[offset] = 0;
    }
}

TEST(SdReference, EverySectorByteIsCoveredByCrcWithoutPartialOutput)
{
    Bytes page = MakePage();
    for (size_t offset = DATA_OFFSET; offset < DATA_OFFSET + DATA_BYTES; ++offset) {
        SCOPED_TRACE(testing::Message() << "offset 0x" << std::hex << offset);
        page[offset] ^= 1;
        Invoke(page, 0, PAGE_BYTES, 4);
        page[offset] ^= 1;
    }
}

TEST(SdReference, CrcIsBigEndianAndAllFourBytesAreChecked)
{
    for (size_t offset = 0x28; offset < 0x2C; ++offset) {
        Bytes page = MakePage();
        page[offset] ^= 1;
        Invoke(page, 0, PAGE_BYTES, 4);
    }
    Bytes page = MakePage();
    const uint32_t crc = Crc(page.data() + 0x200, 0x200);
    Bytes little(4), big(4);
    PutLe(little, 0, crc);
    PutBe(big, 0, crc);
    EXPECT_NE(little, big);
    PutLe(page, 0x28, crc);
    Invoke(page, 0, PAGE_BYTES, 4);
}

TEST(SdReference, UnalignedInputPage)
{
    // The serialized contract uses byte reads, so no aligned host deref is needed. Actual firmware page address
    // remains fixed and aligned.
    const Bytes page = MakePage();
    auto raw = std::make_unique<uint8_t[]>(page.size() + 3);  // 'X' + page + 'Y' + a C string's NUL
    raw[0] = 'X';
    std::memcpy(raw.get() + 1, page.data(), page.size());
    raw[page.size() + 1] = 'Y';
    raw[page.size() + 2] = 0;
    iosw_sd_reference out;
    EXPECT_EQ(iosw_sd_reference_validate(raw.get() + 1, 4096, 0, &out), 0);
    EXPECT_EQ(raw[0], 'X');
    EXPECT_EQ(raw[page.size() + 1], 'Y');
}

// Inaccessible pages around the input catch reads outside its exact 4096-byte extent; mprotect also proves the input
// is not written. The page sits at both ends of the accessible granule in turn.
TEST(SdReference, GuardedReadOnlyPage)
{
    const size_t granule = (size_t)sysconf(_SC_PAGESIZE);
    ASSERT_GE(granule, IOSW_SD_REFERENCE_BYTES);
    uint8_t *allocation = static_cast<uint8_t *>(mmap(nullptr, granule * 3, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0));
    ASSERT_NE(allocation, MAP_FAILED);
    uint8_t *middle = allocation + granule;
    ASSERT_EQ(mprotect(middle, granule, PROT_READ | PROT_WRITE), 0);
    for (unsigned end = 0; end < 2; ++end) {
        uint8_t *page = middle + (end ? granule - 4096 : 0);
        std::memset(page, 0, 4096);
        const uint32_t header[10] = {0x52445349, 1, 0x400, 0x200, 0x200, 0x700b0000, 0, 1, 1, 0};
        std::memcpy(page, header, sizeof header);
        // zlib.crc32(bytes(512)) = 0xb2aa7578; firmware serializes big endian.
        page[0x28] = 0xb2;
        page[0x29] = 0xaa;
        page[0x2a] = 0x75;
        page[0x2b] = 0x78;
        ASSERT_EQ(mprotect(middle, granule, PROT_READ), 0);
        iosw_sd_reference out;
        EXPECT_EQ(iosw_sd_reference_validate(page, 4096, 0, &out), IOSW_SD_REF_OK);
        EXPECT_EQ(out.crc32, 0xb2aa7578u);
        ASSERT_EQ(mprotect(middle, granule, PROT_READ | PROT_WRITE), 0);
    }
    EXPECT_EQ(munmap(allocation, granule * 3), 0);
}
