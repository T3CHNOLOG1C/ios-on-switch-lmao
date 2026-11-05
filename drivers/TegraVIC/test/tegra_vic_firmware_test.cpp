// The vendored VIC firmware is the file firmware/SOURCE names (its sha256), and its falcon bin header (v1), OS header
// and FCE header are where TegraVIC.cpp reads them: the FCE microcode the kernel hands over (0x3c0 bytes at 0x3d00).
#include <CommonCrypto/CommonDigest.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::SourceText;
using testing::HasSubstr;

namespace {
constexpr const char *kDigest = "01ab2263d3ca54ce6abd425d0df8bedf3a33a59bfc51f9bec1d67207e74867f9";

std::string sha256(const std::string &bytes)
{
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(bytes.data(), static_cast<CC_LONG>(bytes.size()), digest);
    std::string hex;
    for (const unsigned char byte : digest) {
        char two[3];
        std::snprintf(two, sizeof(two), "%02x", byte);
        hex += two;
    }
    return hex;
}

// The little-endian 32-bit word at `offset` (0 past the end, which the size checks below catch).
uint32_t word(const std::string &blob, size_t offset)
{
    if (offset + 4 > blob.size()) {
        ADD_FAILURE() << "read past the end at " << offset;
        return 0;
    }
    const auto *p = reinterpret_cast<const unsigned char *>(blob.data()) + offset;
    return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24;
}
}  // namespace

TEST(TegraVICFirmware, IsTheFileSourceNames)
{
    const std::string blob = SourceText("../firmware/nvidia/tegra210/vic04_ucode.bin");
    EXPECT_EQ(sha256(blob), kDigest);
    EXPECT_THAT(SourceText("../firmware/SOURCE"), HasSubstr(kDigest));
}

TEST(TegraVICFirmware, HeadersAsTegraVICReadsThem)
{
    const std::string blob = SourceText("../firmware/nvidia/tegra210/vic04_ucode.bin");
    ASSERT_GE(blob.size(), 32u);
    const uint32_t magic = word(blob, 0), version = word(blob, 4), size = word(blob, 8), osHeader = word(blob, 12);
    const uint32_t binSize = word(blob, 20), fceHeader = word(blob, 24), fceData = word(blob, 28);
    EXPECT_EQ(magic, 0x10deu);
    EXPECT_EQ(version, 1u);
    EXPECT_EQ(size, blob.size());
    const uint32_t code = word(blob, osHeader), codeSize = word(blob, osHeader + 4);
    const uint32_t data = word(blob, osHeader + 8), dataSize = word(blob, osHeader + 12);
    EXPECT_NE(codeSize, 0u);
    EXPECT_LE(code + codeSize, binSize);
    EXPECT_LE(data + dataSize, binSize);
    EXPECT_EQ(fceHeader, 0x3c00u);
    EXPECT_EQ(fceData, 0x3d00u);
    const uint32_t fceSize = word(blob, fceHeader + 8);
    EXPECT_EQ(fceSize, 0x3c0u);
    EXPECT_LE(fceData + fceSize, blob.size());
}
