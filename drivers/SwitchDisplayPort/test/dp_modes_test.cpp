// SPDX-License-Identifier: GPL-2.0-only
// SwitchDPModes.h: EDID parsing and the 1080p60 cap on mode choice, over real-shaped EDIDs and random ones.
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <random>
#include <vector>

#include "../SwitchDPLink.h"
#include "../SwitchDPModes.h"

namespace {

using Block = std::array<uint8_t, 128>;

void checksum(Block &block)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < 127; ++i) {
        sum = static_cast<uint8_t>(sum + block[i]);
    }
    block[127] = static_cast<uint8_t>(0x100 - sum);
}

// An 18-byte detailed timing descriptor, digital separate sync with both polarities positive.
std::array<uint8_t, 18> dtd(uint32_t pixel_khz, unsigned ha, unsigned hf, unsigned hs, unsigned hb, unsigned va,
                            unsigned vf, unsigned vs, unsigned vb, bool interlaced = false)
{
    const unsigned hblank = hf + hs + hb, vblank = vf + vs + vb, clock = pixel_khz / 10;
    std::array<uint8_t, 18> d{};
    d[0] = clock & 0xFF;
    d[1] = clock >> 8;
    d[2] = ha & 0xFF;
    d[3] = hblank & 0xFF;
    d[4] = static_cast<uint8_t>(((ha >> 8) << 4) | (hblank >> 8));
    d[5] = va & 0xFF;
    d[6] = vblank & 0xFF;
    d[7] = static_cast<uint8_t>(((va >> 8) << 4) | (vblank >> 8));
    d[8] = hf & 0xFF;
    d[9] = hs & 0xFF;
    d[10] = static_cast<uint8_t>(((vf & 0xF) << 4) | (vs & 0xF));
    d[11] = static_cast<uint8_t>(((hf >> 8) << 6) | ((hs >> 8) << 4) | ((vf >> 4) << 2) | (vs >> 4));
    d[17] = static_cast<uint8_t>(0x1E | (interlaced ? 0x80 : 0));
    return d;
}

Block base(const std::vector<std::array<uint8_t, 18>> &dtds, uint8_t extensions, bool vga = true)
{
    Block b{};
    const uint8_t header[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    std::copy(header, header + 8, b.begin());
    b[18] = 1;
    b[19] = 4;
    b[35] = vga ? 0x20 : 0;
    for (size_t i = 0; i < dtds.size() && i < 4; ++i) {
        std::copy(dtds[i].begin(), dtds[i].end(), b.begin() + 54 + 18 * i);
    }
    b[126] = extensions;
    checksum(b);
    return b;
}

Block cta(const std::vector<uint8_t> &vics, const std::vector<std::array<uint8_t, 18>> &dtds = {})
{
    Block b{};
    b[0] = 0x02;
    b[1] = 0x03;
    b[4] = static_cast<uint8_t>((2 << 5) | vics.size());
    std::copy(vics.begin(), vics.end(), b.begin() + 5);
    b[2] = static_cast<uint8_t>(5 + vics.size());
    for (size_t i = 0; i < dtds.size(); ++i) {
        std::copy(dtds[i].begin(), dtds[i].end(), b.begin() + b[2] + 18 * i);
    }
    checksum(b);
    return b;
}

switch_dp_mode_list parse(const std::vector<Block> &blocks)
{
    std::vector<uint8_t> edid;
    for (const Block &b : blocks) {
        edid.insert(edid.end(), b.begin(), b.end());
    }
    switch_dp_mode_list list{};
    switch_dp_edid_modes(edid.data(), static_cast<unsigned>(blocks.size()), &list);
    return list;
}

const auto k2160p60 = dtd(594000, 3840, 176, 88, 296, 2160, 8, 10, 72);
const auto k1440p144 = dtd(586590, 2560, 48, 32, 80, 1440, 3, 5, 62);
const auto k1440p60 = dtd(241500, 2560, 48, 32, 80, 1440, 3, 5, 33);
const auto k1080p144 = dtd(325080, 1920, 48, 32, 80, 1080, 3, 5, 15);
const auto k1080p75 = dtd(174500, 1920, 48, 32, 80, 1080, 3, 5, 23);
const auto k1080p60 = dtd(148500, 1920, 88, 44, 148, 1080, 4, 5, 36);
const auto k1024p60 = dtd(108000, 1280, 48, 112, 248, 1024, 1, 3, 38);
const auto k1050p60 = dtd(146250, 1680, 104, 176, 280, 1050, 3, 6, 30);
const uint64_t kHbr2Lanes = switch_dp_payload_kbps(SWITCH_DP_RATE_HBR, 2);

const switch_dp_mode &pick(const switch_dp_mode_list &list, uint64_t link = kHbr2Lanes)
{
    const int index = switch_dp_pick_mode(&list, link);
    EXPECT_GE(index, 0);
    return list.modes[index < 0 ? 0 : index];
}

TEST(Edid, StandardDetailedTimingParses)
{
    switch_dp_mode mode{};
    ASSERT_TRUE(switch_dp_edid_dtd(k1080p60.data(), &mode));
    EXPECT_EQ(mode.pixel_khz, 148500u);
    EXPECT_EQ(mode.hactive, 1920);
    EXPECT_EQ(mode.hfront, 88);
    EXPECT_EQ(mode.hsync, 44);
    EXPECT_EQ(mode.hback, 148);
    EXPECT_EQ(mode.vactive, 1080);
    EXPECT_EQ(mode.vfront, 4);
    EXPECT_EQ(mode.vsync, 5);
    EXPECT_EQ(mode.vback, 36);
    EXPECT_TRUE(mode.hsync_high);
    EXPECT_TRUE(mode.vsync_high);
    EXPECT_EQ(switch_dp_mode_refresh_mhz(&mode), 60000u);
}

TEST(Edid, InterlacedAndDescriptorsAreSkipped)
{
    switch_dp_mode mode{};
    const auto interlaced = dtd(74250, 1920, 88, 44, 148, 540, 2, 5, 15, true);
    EXPECT_FALSE(switch_dp_edid_dtd(interlaced.data(), &mode));
    const std::array<uint8_t, 18> name{0, 0, 0, 0xFC, 0, 'T', 'V'};
    EXPECT_FALSE(switch_dp_edid_dtd(name.data(), &mode));
}

TEST(ModeCap, FourKTelevisionGets1080p60)
{
    const auto list = parse({base({k2160p60}, 1), cta({97, 0x80 | 16, 4, 31, 3})});
    const switch_dp_mode &mode = pick(list);
    EXPECT_EQ(mode.hactive, 1920);
    EXPECT_EQ(mode.vactive, 1080);
    EXPECT_EQ(switch_dp_mode_refresh_mhz(&mode), 60000u);
}

TEST(ModeCap, FastMonitorGets1080p60NotItsPreferredMode)
{
    const auto list = parse({base({k1440p144, k1440p60, k1080p144, k1080p60}, 0)});
    EXPECT_TRUE(list.modes[0].preferred);
    const switch_dp_mode &mode = pick(list, switch_dp_payload_kbps(SWITCH_DP_RATE_HBR2, 4));
    EXPECT_EQ(mode.hactive, 1920);
    EXPECT_EQ(switch_dp_mode_refresh_mhz(&mode), 60000u);
}

TEST(ModeCap, AboveSixtyHertzIsNeverDriven)
{
    const auto list = parse({base({k1080p75, k1024p60}, 0)});
    const switch_dp_mode &mode = pick(list);
    EXPECT_EQ(mode.hactive, 1280);
    EXPECT_EQ(mode.vactive, 1024);
}

TEST(ModeCap, NarrowLinkFallsToASmallerMode)
{
    const auto list = parse({base({k1080p60}, 1), cta({16, 4, 3})});
    const switch_dp_mode &mode = pick(list, switch_dp_payload_kbps(SWITCH_DP_RATE_RBR, 2));
    EXPECT_EQ(mode.hactive, 1280);
    EXPECT_EQ(mode.vactive, 720);
}

TEST(ModeCap, FiftyHertz1080pBeats720p60)
{
    const auto list = parse({base({}, 1, false), cta({4, 31})});
    const switch_dp_mode &mode = pick(list);
    EXPECT_EQ(mode.vactive, 1080);
    EXPECT_EQ(switch_dp_mode_refresh_mhz(&mode), 50000u);
}

TEST(ModeCap, SmoothBeatsBigger)
{
    const auto list = parse({base({k1050p60}, 1), cta({34})});
    const switch_dp_mode &mode = pick(list);
    EXPECT_EQ(mode.hactive, 1680);
}

TEST(ModeCap, BrokenEdidLeavesOnly640x480)
{
    Block broken = base({k1080p60}, 0);
    broken[60] ^= 0xFF;
    const auto list = parse({broken});
    ASSERT_EQ(list.count, 1u);
    EXPECT_EQ(list.modes[0].hactive, 640);
    EXPECT_EQ(pick(list).vactive, 480);
}

TEST(ModeCap, BadExtensionIsSkipped)
{
    Block extension = cta({16});
    extension[127] ^= 1;
    const auto list = parse({base({k1024p60}, 1), extension});
    EXPECT_EQ(pick(list).hactive, 1280);
}

TEST(ModeCap, RandomModesNeverExceedTheCap)
{
    std::mt19937 rng(1080);
    for (int round = 0; round < 5000; ++round) {
        switch_dp_mode_list list{};
        list.count = 1 + rng() % SWITCH_DP_MAX_MODES;
        for (unsigned i = 0; i < list.count; ++i) {
            switch_dp_mode &m = list.modes[i];
            m.hactive = static_cast<uint16_t>(320 + rng() % 4000);
            m.vactive = static_cast<uint16_t>(200 + rng() % 2400);
            m.hfront = static_cast<uint16_t>(1 + rng() % 200);
            m.hsync = static_cast<uint16_t>(1 + rng() % 100);
            m.hback = static_cast<uint16_t>(1 + rng() % 300);
            m.vfront = static_cast<uint16_t>(1 + rng() % 20);
            m.vsync = static_cast<uint16_t>(1 + rng() % 10);
            m.vback = static_cast<uint16_t>(1 + rng() % 60);
            const uint32_t refresh_hz = 20 + rng() % 230;
            m.pixel_khz = static_cast<uint32_t>(uint64_t{switch_dp_mode_htotal(&m)} * switch_dp_mode_vtotal(&m) *
                                                refresh_hz / 1000);
            m.preferred = i == 0;
        }
        const uint64_t link = switch_dp_payload_kbps(SWITCH_DP_RATE_HBR2, 4);
        const int index = switch_dp_pick_mode(&list, link);
        if (index < 0) {
            continue;
        }
        const switch_dp_mode &m = list.modes[index];
        ASSERT_LE(m.hactive, 1920);
        ASSERT_LE(m.vactive, 1080);
        ASSERT_LE(switch_dp_mode_refresh_mhz(&m), SWITCH_DP_MAX_REFRESH);
        ASSERT_LT(uint64_t{m.pixel_khz} * 24, link);
        for (unsigned i = 0; i < list.count; ++i) {
            if (switch_dp_mode_allowed(&list.modes[i], link) && switch_dp_mode_is_1080p60(&list.modes[i])) {
                ASSERT_TRUE(switch_dp_mode_is_1080p60(&m));
            }
        }
    }
}

}  // namespace
