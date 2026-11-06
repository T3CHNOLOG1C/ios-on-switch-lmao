// drivers/NativeMobileDisplay/BootLogoCheck.h on the host: the first client frame against the boot framebuffer, with
// the boot framebuffer's 2880-byte rows and the compositor's padded ones.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "../BootLogoCheck.h"

namespace {
constexpr uint32_t kWidth = 720, kHeight = 1280, kBootStride = 720, kFrameStride = 736;

struct Frames {
    std::vector<uint32_t> boot = std::vector<uint32_t>(kBootStride * kHeight, 0xff000000U);
    std::vector<uint32_t> frame = std::vector<uint32_t>(kFrameStride * kHeight, 0xff000000U);

    void logo(uint32_t column, uint32_t row, uint32_t grey)
    {
        boot[row * kBootStride + column] = 0xff000000U | grey * 0x010101U;
        frame[row * kFrameStride + column] = 0xff000000U | grey * 0x010101U;
    }

    BootLogoDifference compare(bool blank = false) const
    {
        return boot_logo_compare(boot.data(), kBootStride * 4, blank ? nullptr : frame.data(), kFrameStride * 4, kWidth,
                                 kHeight);
    }
};
}  // namespace

TEST(BootLogoCheck, SamePanelMatches)
{
    Frames f;
    for (uint32_t row = 490; row < 790; ++row)
        for (uint32_t column = 210; column < 510; ++column)
            f.logo(column, row, (row * 7 + column) & 255);
    EXPECT_EQ(f.compare().pixels, 0u);
}

TEST(BootLogoCheck, AlphaAndRowPaddingAreIgnored)
{
    Frames f;
    f.frame[100 * kFrameStride + 5] = 0x00000000U;
    for (uint32_t row = 0; row < kHeight; ++row)
        f.frame[row * kFrameStride + kWidth] = 0xffffffffU;
    EXPECT_EQ(f.compare().pixels, 0u);
}

TEST(BootLogoCheck, ReportsTheFirstDifferenceInScanOrder)
{
    Frames f;
    f.logo(300, 600, 200);
    f.frame[600 * kFrameStride + 300] = 0xffc7c8c8U;
    f.frame[1279 * kFrameStride + 719] = 0xff010101U;
    const BootLogoDifference d = f.compare();
    EXPECT_EQ(d.pixels, 2u);
    EXPECT_EQ(d.column, 300u);
    EXPECT_EQ(d.row, 600u);
    EXPECT_EQ(d.boot, 0xc8c8c8u);
    EXPECT_EQ(d.frame, 0xc7c8c8u);
}

TEST(BootLogoCheck, BlankSwapClearsTheLogo)
{
    Frames f;
    f.logo(210, 490, 255);
    f.logo(509, 789, 17);
    const BootLogoDifference d = f.compare(true);
    EXPECT_EQ(d.pixels, 2u);
    EXPECT_EQ(d.column, 210u);
    EXPECT_EQ(d.row, 490u);
    EXPECT_EQ(d.frame, 0u);
}
