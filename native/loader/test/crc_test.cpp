// native/loader/checksum.S (loader_crc32_update: the loader's IEEE CRC-32 with the ARMv8 CRC32 instructions), the
// routine itself run on this arm64 Mac (Apple's cores implement CRC32), against zlib's crc32 as the independent
// reference: every offset 0..7 and the lengths around each of the routine's paths (bytes to alignment, 8-byte words,
// the tail), in one call and in 113-byte chunks, and no read past the end of the buffer.
//
// The ELF-only directives (.type/.size) are dropped and Mach-O's leading underscore added at build time
// (:checksum_host); the instructions are the file's own. What a host run cannot see: that every wide read is 8-byte
// aligned (the loader runs it with the MMU off, where an unaligned LDR faults); the routine's alignment loop is checked
// by reading it (checksum.S).
#include <gtest/gtest.h>
#include <sys/mman.h>
#include <unistd.h>
#include <zlib.h>

#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

extern "C" uint32_t loader_crc32_update(uint32_t crc, const void *data, uint64_t bytes);

namespace {
struct Sample {
    uint32_t offset, size, expected;
};

std::vector<uint8_t> input()
{
    std::mt19937 rng(7801);
    std::vector<uint8_t> data(65544);
    for (uint8_t &b : data) b = (uint8_t)rng();
    return data;
}

std::vector<Sample> samples(const std::vector<uint8_t> &data)
{
    std::vector<uint32_t> sizes;
    for (uint32_t size = 0; size <= 16; ++size) sizes.push_back(size);
    for (uint32_t size : {31u, 32u, 33u, 255u, 256u, 257u, 4096u, 65535u, 65536u}) sizes.push_back(size);
    std::vector<Sample> cases;
    for (uint32_t offset = 0; offset < 8; ++offset)
        for (uint32_t size : sizes) cases.push_back({offset, size, (uint32_t)crc32(0, data.data() + offset, size)});
    return cases;
}
}  // namespace

TEST(LoaderCrc32, MatchesZlibAtAllAlignmentsAndChunkBoundaries)
{
    const std::vector<uint8_t> data = input();
    const std::vector<Sample> cases = samples(data);
    ASSERT_EQ(cases.size(), 208u);
    for (const Sample &s : cases) {
        SCOPED_TRACE(testing::Message() << "offset " << s.offset << ", " << s.size << " bytes");
        EXPECT_EQ(~loader_crc32_update(UINT32_MAX, data.data() + s.offset, s.size), s.expected) << "one call";
        uint32_t crc = UINT32_MAX;
        for (uint32_t offset = 0; offset < s.size;) {
            uint32_t size = s.size - offset;
            if (size > 113) size = 113;
            crc = loader_crc32_update(crc, data.data() + s.offset + offset, size);
            offset += size;
        }
        EXPECT_EQ(~crc, s.expected) << "113-byte chunks";
    }
}

// Each case's bytes end exactly at an inaccessible page: a word read past the end faults the test.
TEST(LoaderCrc32, NeverReadsPastTheEnd)
{
    const std::vector<uint8_t> data = input();
    const size_t page = (size_t)getpagesize();
    const size_t span = (65536 + 8 + page - 1) / page * page;
    void *map = mmap(nullptr, span + page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    ASSERT_NE(map, MAP_FAILED);
    uint8_t *base = static_cast<uint8_t *>(map);
    ASSERT_EQ(mprotect(base + span, page, PROT_NONE), 0);
    for (const Sample &s : samples(data)) {
        uint8_t *at = base + span - s.size;  // the last byte is the last accessible one
        std::memcpy(at, data.data() + s.offset, s.size);
        EXPECT_EQ(~loader_crc32_update(UINT32_MAX, at, s.size), s.expected)
            << "offset " << s.offset << ", " << s.size << " bytes at the end of a page";
    }
    munmap(map, span + page);
}
