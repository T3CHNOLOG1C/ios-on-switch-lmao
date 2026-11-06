// SPDX-License-Identifier: MIT
// raw/runtime/raw_target.h on the host. A pass into a chain level writes through SET_COLOR_TARGET_*: the render hardware
// addresses a block-linear target from its base, its WIDTH (rounded up to whole GOBs: the row stride) and its block height,
// one GOB wide. Every texel it writes must be the byte raw/fx's layout has for that texel (fx::texelOffset: the layout the
// texture header samples and the 2D engine writes, which the device's self-test checks on the GPU), or the levels the raw
// path renders would land beside where the next draw samples them.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../raw/runtime/raw_target.h"

namespace {

using raw::ColorTargetDesc;
using raw::fx::MipChain;

// Tegra block-linear addressing of a surface with blocks one GOB (64 B x 8 rows) wide and 1 << blockHeight GOBs tall, rows of
// rowBytes: blocks row by row, GOBs top to bottom in a block, TegraColor order inside a GOB
uint64_t blockLinearOffset(uint32_t rowBytes, uint32_t blockHeight, uint32_t xB, uint32_t y)
{
    const uint32_t rowsPerBlock = 8u << blockHeight, gobsPerRow = rowBytes / 64;
    const uint64_t block = uint64_t(y / rowsPerBlock) * gobsPerRow + xB / 64;
    return block * (512ull << blockHeight) + uint64_t((y % rowsPerBlock) / 8) * 512 + raw::fx::gobOffset(xB, y);
}

struct Size {
    uint32_t w, h, levels;
};

const Size kSizes[] = {{2, 2, 2},       {3, 5, 3},       {776, 24, 10},    {180, 180, 8}, {120, 120, 7},
                       {1280, 720, 11}, {720, 1280, 11}, {1024, 1, 11},    {1, 1024, 11}, {2048, 1536, 12},
                       {97, 3, 7},      {33, 65, 7},     {1366, 1024, 11}, {256, 8, 9},   {8, 256, 9},
                       {300, 200, 9},   {640, 480, 10},  {85, 85, 7},      {17, 9, 5},    {4096, 4096, 13}};

TEST(RawTarget, PitchWordsAsBefore)
{
    ColorTargetDesc d;
    d.va = 0x12345678000ull;
    d.pitch = 5120;
    d.height = 720;
    d.bpp = 4;
    d.rtFormat = 0xcf;
    uint32_t w[raw::kColorTargetWords];
    raw::colorTargetWords(d, w);
    const uint32_t want[raw::kColorTargetWords] = {0x123, 0x45678000, 5120, 720, 0xcf, 0x1000, 1, 0, 0};
    for (uint32_t i = 0; i < raw::kColorTargetWords; i++)
        EXPECT_EQ(w[i], want[i]) << "word " << i;
}

TEST(RawTarget, OnlyLaterLevels)
{
    MipChain c;
    ASSERT_TRUE(raw::fx::mipChainLayout(1280, 720, 11, 4, c));
    ColorTargetDesc d;
    EXPECT_FALSE(raw::chainLevelDesc(c, 0x100000, 0, 0xcf, d));
    EXPECT_FALSE(raw::chainLevelDesc(c, 0x100000, 11, 0xcf, d));
    EXPECT_FALSE(raw::chainLevelDesc(c, 0x100000, 1, 0, d));
    EXPECT_TRUE(raw::chainLevelDesc(c, 0x100000, 10, 0xcf, d));
}

// NVK's words for the level, and the hardware's addressing of them equal to raw/fx's layout of the level, texel by texel
TEST(RawTarget, ChainLevelAddressesTheLayout)
{
    const uint64_t chainVa = 0x2000000000ull;  // a chain's base (fx::kChainAlign aligned)
    unsigned checked = 0;
    for (uint32_t bpp : {4u, 8u})
        for (const Size &s : kSizes) {
            MipChain c;
            ASSERT_TRUE(raw::fx::mipChainLayout(s.w, s.h, s.levels, bpp, c)) << s.w << "x" << s.h;
            for (uint32_t l = 1; l < c.levels; l++) {
                const raw::fx::MipLevel &lv = c.level[l];
                ColorTargetDesc d;
                ASSERT_TRUE(raw::chainLevelDesc(c, chainVa, l, 0xcf, d));
                uint32_t w[raw::kColorTargetWords];
                raw::colorTargetWords(d, w);
                const uint64_t va = uint64_t(w[0]) << 32 | w[1];
                EXPECT_EQ(va, chainVa + lv.offset);
                EXPECT_EQ(va % 512, 0u) << "a level starts on a GOB";
                EXPECT_EQ(w[2] * bpp, lv.rowBytes) << "WIDTH is the row stride in texels";
                EXPECT_EQ(lv.rowBytes % 64, 0u);
                EXPECT_GE(lv.rowBytes, lv.width * bpp);
                EXPECT_EQ(w[3], lv.height);
                EXPECT_EQ(w[4], 0xcfu);
                EXPECT_EQ(w[5], uint32_t(lv.yLog2) << 4) << "ONE_GOB wide, the level's block height, BLOCKLINEAR";
                EXPECT_EQ(w[6], 1u);
                EXPECT_EQ(uint64_t(w[7]) * 4, c.bytes);
                EXPECT_EQ(w[8], 0u);
                // the rows the target spans stay inside the level and the chain
                const uint64_t end = lv.offset + uint64_t(lv.rowBytes) * lv.rows;
                EXPECT_LE(end, c.bytes);
                if (l + 1 < c.levels)
                    EXPECT_LE(end, c.level[l + 1].offset);
                const uint32_t gobsPerRow = (w[2] * bpp + 63) / 64;
                const uint32_t stepX = lv.width > 64 ? 7 : 1, stepY = lv.height > 64 ? 5 : 1;
                for (uint32_t y = 0; y < lv.height; y += stepY)
                    for (uint32_t x = 0; x < lv.width; x += stepX)
                        for (uint32_t b = 0; b < bpp; b += bpp - 1) {
                            const uint32_t xB = x * bpp + b;
                            const uint64_t hw = (va - chainVa) + blockLinearOffset(gobsPerRow * 64, lv.yLog2, xB, y);
                            const uint64_t fx = raw::fx::texelOffset(c, l, xB, y);
                            if (hw != fx) {
                                ADD_FAILURE()
                                    << s.w << "x" << s.h << " bpp " << bpp << " level " << l << " texel (" << x << ","
                                    << y << ") byte " << b << ": target " << hw << ", layout " << fx;
                                return;
                            }
                            checked++;
                        }
            }
        }
    std::printf("%u texel bytes addressed alike\n", checked);
}

// A Shared mipmapped texture's CPU access to a later level: every texel byte written lands where fx::texelOffset has it (the
// layout the texture header samples), nothing outside the region is touched, and reading the region back gives the bytes
TEST(RawTarget, CpuLevelCopyFollowsTheLayout)
{
    unsigned checked = 0;
    for (bool fermi : {false, true})
        for (uint32_t bpp : {2u, 4u, 8u})
            for (const Size &s : kSizes) {
                if (uint64_t(s.w) * s.h > 1u << 21)
                    continue;
                MipChain c;
                ASSERT_TRUE(raw::fx::mipChainLayout(s.w, s.h, s.levels, bpp, c));
                std::vector<uint8_t> chain(c.bytes, 0xee);
                for (uint32_t l = 1; l < c.levels; l++) {
                    const raw::fx::MipLevel &lv = c.level[l];
                    const uint32_t x = lv.width / 3, y = lv.height / 4, w = lv.width - x, h = lv.height - y;
                    const size_t row = size_t(w) * bpp + 5;
                    std::vector<uint8_t> src(row * h), back(row * h, 0);
                    for (size_t i = 0; i < src.size(); i++)
                        src[i] = uint8_t(i * 131 + l * 7 + 1);
                    raw::fx::writeLevel(c, chain.data(), l, x, y, w, h, src.data(), row, fermi);
                    for (uint32_t r = 0; r < h; r++)
                        for (uint32_t b = 0; b < w * bpp; b++) {
                            const uint64_t at = raw::fx::texelOffset(c, l, x * bpp + b, y + r, fermi);
                            if (chain[at] != src[r * row + b]) {
                                ADD_FAILURE() << s.w << "x" << s.h << " level " << l << " (" << b << "," << r << ")";
                                return;
                            }
                            checked++;
                        }
                    raw::fx::readLevel(c, chain.data(), l, x, y, w, h, back.data(), row, fermi);
                    for (uint32_t r = 0; r < h; r++)
                        ASSERT_EQ(0, memcmp(back.data() + r * row, src.data() + r * row, size_t(w) * bpp));
                }
                size_t untouched = 0;
                for (uint8_t b : chain)
                    untouched += b == 0xee;
                size_t written = 0;
                for (uint32_t l = 1; l < c.levels; l++) {
                    const raw::fx::MipLevel &lv = c.level[l];
                    written += size_t(lv.width - lv.width / 3) * (lv.height - lv.height / 4) * bpp;
                }
                EXPECT_GE(untouched, c.bytes - written) << "bytes outside the regions were written";
            }
    std::printf("%u texel bytes copied by the CPU where the layout has them\n", checked);
}

}  // namespace
