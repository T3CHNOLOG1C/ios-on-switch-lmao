// The vendored NVJPG firmware and NVIDIA headers are the files their SOURCE files name: the firmware's sha256 and its
// falcon bin header (v1) and OS header as TegraNVJPG.cpp reads them (no FCE section, unlike VIC's), its licence the same
// file as TegraNVDEC's, and each header's sha256 the one nvidia/SOURCE abbreviates.
#include <CommonCrypto/CommonDigest.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <string>

#include "bazel/testing/source_text.h"

using ioswitch::test::SourceText;
using testing::EndsWith;
using testing::HasSubstr;
using testing::StartsWith;

namespace {
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

TEST(TegraNVJPGVendored, Firmware)
{
    const std::string blob = SourceText("../firmware/nvidia/tegra210/nvjpg.bin");
    const std::string digest = sha256(blob);
    EXPECT_EQ(digest, "85bec091379f6bfd5873e9b657396065024e39b11e72c6f7f86e49a30aa830fe");
    EXPECT_THAT(SourceText("../firmware/SOURCE"), HasSubstr(digest));
    ASSERT_GE(blob.size(), 28u);
    const uint32_t osHeader = word(blob, 12), binData = word(blob, 16), binSize = word(blob, 20);
    EXPECT_EQ(word(blob, 0), 0x10deu);  // magic
    EXPECT_EQ(word(blob, 4), 1u);       // version
    EXPECT_EQ(word(blob, 8), blob.size());
    EXPECT_EQ(osHeader, 0x100u);
    EXPECT_EQ(binData, 0x200u);
    EXPECT_EQ(word(blob, 24), 0xa5a5a5a5u);  // no FCE section: nothing to hand over, unlike VIC
    EXPECT_EQ(word(blob, osHeader), 0u);     // code
    EXPECT_EQ(word(blob, osHeader + 4), 0x300u);
    EXPECT_EQ(word(blob, osHeader + 8), 0x3d00u);  // data
    EXPECT_EQ(word(blob, osHeader + 12), 0x200u);
    EXPECT_EQ(word(blob, osHeader + 16), 13u);  // apps
    EXPECT_LE(binData + binSize, blob.size());
    EXPECT_EQ(SourceText("../firmware/LICENCE.nvidia"), SourceText("../../TegraNVDEC/firmware/LICENCE.nvidia"));
}

TEST(TegraNVJPGVendored, NvidiaHeaders)
{
    const std::string source = SourceText("../nvidia/SOURCE");
    struct Header {
        const char *name, *prefix, *suffix;
    };
    for (const Header &h : {Header{"nvjpg_drv.h", "b719a992", "4278"}, Header{"cle7d0.h", "144aa394", "8776"}}) {
        const std::string digest = sha256(SourceText(std::string("../nvidia/") + h.name));
        EXPECT_THAT(digest, StartsWith(h.prefix)) << h.name;
        EXPECT_THAT(digest, EndsWith(h.suffix)) << h.name;
        EXPECT_THAT(source, HasSubstr(std::string(h.prefix) + "..." + h.suffix)) << h.name;
    }
}
