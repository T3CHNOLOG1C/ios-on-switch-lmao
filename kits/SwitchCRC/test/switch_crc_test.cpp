// switch_crc32 against zlib's crc32 at every alignment and short length, on random buffers, and chained.
#include "kits/SwitchCRC/SwitchCRC.h"

#include <gtest/gtest.h>
#include <zlib.h>

#include <random>
#include <vector>

namespace {
uint32_t zlib(uint32_t crc, const uint8_t *data, size_t length)
{
    return static_cast<uint32_t>(crc32(crc, data, static_cast<uInt>(length)));
}

TEST(SwitchCRC, CheckValue)
{
    EXPECT_EQ(switch_crc32(0, "123456789", 9), 0xcbf43926u);
    EXPECT_EQ(switch_crc32(0, nullptr, 0), 0u);
}

TEST(SwitchCRC, MatchesZlibAtEveryAlignmentAndLength)
{
    std::mt19937 random(7);
    std::vector<uint8_t> bytes(4096 + 16);
    for (auto &b : bytes)
        b = static_cast<uint8_t>(random());
    for (size_t offset = 0; offset < 16; ++offset)
        for (size_t length = 0; length < 300; ++length)
            ASSERT_EQ(switch_crc32(0, bytes.data() + offset, length), zlib(0, bytes.data() + offset, length))
                << offset << "+" << length;
    EXPECT_EQ(switch_crc32(0, bytes.data() + 3, 4096), zlib(0, bytes.data() + 3, 4096));
}

TEST(SwitchCRC, ChainsLikeZlib)
{
    std::mt19937 random(9);
    std::vector<uint8_t> bytes(1000);
    for (auto &b : bytes)
        b = static_cast<uint8_t>(random());
    for (size_t split = 0; split <= bytes.size(); split += 37) {
        const uint32_t first = switch_crc32(0, bytes.data(), split);
        ASSERT_EQ(switch_crc32(first, bytes.data() + split, bytes.size() - split), zlib(0, bytes.data(), bytes.size()));
    }
    EXPECT_EQ(switch_crc32(0x12345678, bytes.data(), 77), zlib(0x12345678, bytes.data(), 77));
}
}  // namespace
