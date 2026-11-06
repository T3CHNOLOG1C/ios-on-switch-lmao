// SPDX-License-Identifier: GPL-2.0-only
// SwitchDPLink.h (link choice, SOR1 transfer units, PLL_D2) and SwitchDPTraining.h against a model sink.
#include <gtest/gtest.h>

#include <cstdint>

#include "../SwitchDPLink.h"
#include "../SwitchDPTraining.h"

namespace {

TEST(Link, Only1080p60sNeedIsTrained)
{
    switch_dp_link link{};
    ASSERT_TRUE(switch_dp_link_for(148500, SWITCH_DP_RATE_HBR2, 2, &link));
    EXPECT_EQ(link.rate, SWITCH_DP_RATE_HBR);
    EXPECT_EQ(link.lanes, 2);
    ASSERT_TRUE(switch_dp_link_for(148500, SWITCH_DP_RATE_HBR2, 4, &link));
    EXPECT_EQ(link.rate, SWITCH_DP_RATE_RBR);
    EXPECT_EQ(link.lanes, 4);
    ASSERT_TRUE(switch_dp_link_for(74250, SWITCH_DP_RATE_HBR2, 2, &link));
    EXPECT_EQ(link.rate, SWITCH_DP_RATE_RBR);
    EXPECT_EQ(link.lanes, 2);
    EXPECT_FALSE(switch_dp_link_for(148500, SWITCH_DP_RATE_RBR, 2, &link));
}

TEST(Link, RateCapAndFallback)
{
    EXPECT_EQ(switch_dp_rate_cap(0x1E), SWITCH_DP_RATE_HBR2);
    EXPECT_EQ(switch_dp_rate_cap(0x0A), SWITCH_DP_RATE_HBR);
    EXPECT_EQ(switch_dp_rate_cap(0x00), SWITCH_DP_RATE_RBR);
    EXPECT_EQ(switch_dp_rate_below(SWITCH_DP_RATE_HBR2), SWITCH_DP_RATE_HBR);
    EXPECT_EQ(switch_dp_rate_below(SWITCH_DP_RATE_HBR), SWITCH_DP_RATE_RBR);
    EXPECT_EQ(switch_dp_rate_below(SWITCH_DP_RATE_RBR), 0);
}

struct TuGolden {
    uint32_t pixel_khz, hactive, htotal;
    uint8_t rate, lanes;
    bool enhanced;
    switch_dp_tu tu;
};

// Values from a separate port of Linux tegra_sor_compute_config.
TEST(TransferUnit, MatchesLinux)
{
    const TuGolden cases[] = {
        {148500, 1920, 2200, SWITCH_DP_RATE_HBR, 2, true, {56, 46, 5, 0, 13, 487, 3423}},
        {148500, 1920, 2200, SWITCH_DP_RATE_RBR, 4, true, {64, 43, 1, 0, 18, 291, 2054}},
        {74250, 1280, 1650, SWITCH_DP_RATE_RBR, 2, true, {64, 43, 1, 0, 18, 783, 2716}},
        {25175, 640, 800, SWITCH_DP_RATE_RBR, 1, false, {37, 17, 4, 0, 14, 972, 3917}},
    };
    for (const TuGolden &c : cases) {
        switch_dp_tu tu{};
        ASSERT_TRUE(switch_dp_tu_config(c.pixel_khz, c.hactive, c.htotal, c.rate, c.lanes, c.enhanced, &tu));
        EXPECT_EQ(tu.tu_size, c.tu.tu_size) << c.pixel_khz;
        EXPECT_EQ(tu.active_count, c.tu.active_count) << c.pixel_khz;
        EXPECT_EQ(tu.active_frac, c.tu.active_frac) << c.pixel_khz;
        EXPECT_EQ(tu.active_polarity, c.tu.active_polarity) << c.pixel_khz;
        EXPECT_EQ(tu.watermark, c.tu.watermark) << c.pixel_khz;
        EXPECT_EQ(tu.hblank_symbols, c.tu.hblank_symbols) << c.pixel_khz;
        EXPECT_EQ(tu.vblank_symbols, c.tu.vblank_symbols) << c.pixel_khz;
    }
}

TEST(TransferUnit, OverfullLinkIsRefused)
{
    switch_dp_tu tu{};
    EXPECT_FALSE(switch_dp_tu_config(148500, 1920, 2200, SWITCH_DP_RATE_RBR, 2, true, &tu));
}

TEST(PllD2, PixelClocksAreExact)
{
    switch_dp_plld2 pll{};
    ASSERT_TRUE(switch_dp_plld2_for(148500, 38400, &pll));
    EXPECT_EQ(pll.out_hz, 297000000u);
    EXPECT_EQ(pll.pdiv, 3u);
    EXPECT_EQ(pll.pdiv_hw, 2u);
    EXPECT_EQ(pll.n, 22u);
    EXPECT_EQ(pll.sdm_din, 5760);
    ASSERT_TRUE(switch_dp_plld2_for(74250, 38400, &pll));
    EXPECT_EQ(pll.out_hz, 148500000u);
    ASSERT_TRUE(switch_dp_plld2_for(27000, 38400, &pll));
    EXPECT_EQ(pll.out_hz, 54000000u);
    ASSERT_TRUE(switch_dp_plld2_for(25175, 38400, &pll));
    EXPECT_NEAR(static_cast<double>(pll.out_hz), 50350000.0, 50350000.0 * 1e-4);
}

TEST(PllD2, VcoStaysInRange)
{
    for (uint32_t khz = 25000; khz <= 165000; khz += 250) {
        switch_dp_plld2 pll{};
        ASSERT_TRUE(switch_dp_plld2_for(khz, 38400, &pll)) << khz;
        const uint64_t vco = pll.out_hz * pll.pdiv;
        EXPECT_GE(vco, 749000000u) << khz;
        EXPECT_LE(vco, 1500000000u) << khz;
        EXPECT_NEAR(static_cast<double>(pll.out_hz), khz * 2000.0, khz * 2000.0 * 1e-4) << khz;
    }
}

// A sink that recovers the clock once every lane swings at least `swing` and equalises once every lane also has
// `emphasis`; it asks for exactly that.
struct Sink {
    uint8_t lanes = 2;
    uint8_t swing = 1, emphasis = 1;
    bool never_locks = false, loses_clock_in_eq = false;
    uint8_t vs[4] = {}, pe[4] = {};
    uint8_t pattern = 0;

    void write(const switch_dp_train_out &out)
    {
        pattern = out.dpcd[0] & 3;
        for (unsigned l = 0; l + 1 < out.dpcd_len; ++l) {
            vs[l] = out.dpcd[1 + l] & 3;
            pe[l] = (out.dpcd[1 + l] >> 3) & 3;
        }
    }

    void status(uint8_t s[6]) const
    {
        for (int i = 0; i < 6; ++i) {
            s[i] = 0;
        }
        for (unsigned l = 0; l < lanes; ++l) {
            uint8_t bits = 0;
            const bool cr = !never_locks && vs[l] >= swing && !(loses_clock_in_eq && pattern >= 2);
            if (cr) {
                bits |= SWITCH_DPCD_CR_DONE;
            }
            if (cr && pattern >= 2 && pe[l] >= emphasis) {
                bits |= SWITCH_DPCD_EQ_DONE | SWITCH_DPCD_SYMBOL_LOCKED;
            }
            s[l / 2] = static_cast<uint8_t>(s[l / 2] | (bits << (4 * (l % 2))));
            const uint8_t request = static_cast<uint8_t>((never_locks ? 3 : swing) | (emphasis << 2));
            s[4 + l / 2] = static_cast<uint8_t>(s[4 + l / 2] | (request << (4 * (l % 2))));
        }
        bool all = true;
        for (unsigned l = 0; l < lanes; ++l) {
            all = all && ((s[l / 2] >> (4 * (l % 2))) & 0x7) == 0x7;
        }
        s[2] = all ? SWITCH_DPCD_INTERLANE_ALIGN_DONE : 0;
    }
};

switch_dp_train_out train(Sink &sink, switch_dp_train &t, bool tps3, unsigned *drives)
{
    switch_dp_train_out out = switch_dp_train_start(&t, sink.lanes, tps3, 0);
    *drives = 0;
    while (out.act == SWITCH_DP_TRAIN_DRIVE && *drives < 50) {
        ++*drives;
        sink.write(out);
        uint8_t status[6];
        sink.status(status);
        out = switch_dp_train_status(&t, status);
    }
    return out;
}

TEST(Training, ConvergesOnTheSinksRequests)
{
    Sink sink;
    switch_dp_train t{};
    unsigned drives = 0;
    const switch_dp_train_out out = train(sink, t, false, &drives);
    EXPECT_EQ(out.act, SWITCH_DP_TRAIN_DONE);
    EXPECT_EQ(out.dpcd[0], 0);
    EXPECT_EQ(out.dpcd_len, 1);
    EXPECT_EQ(t.phase, SWITCH_DP_TRAIN_TRAINED);
    EXPECT_LE(drives, 4u);
}

TEST(Training, PatternsAndLaneSettings)
{
    switch_dp_train t{};
    switch_dp_train_out out = switch_dp_train_start(&t, 2, true, 1);
    EXPECT_EQ(out.pattern, 1);
    EXPECT_EQ(out.dpcd[0], 0x21);
    EXPECT_EQ(out.dpcd_len, 3);
    EXPECT_EQ(out.wait_us, 100u);
    const uint8_t cr_done[6] = {0x11, 0, 0, 0, 0x0E, 0};  // lane 0 asks swing 2 emphasis 3: clamped to 1
    out = switch_dp_train_status(&t, cr_done);
    EXPECT_EQ(out.pattern, 3);
    EXPECT_EQ(out.dpcd[0], 0x23);
    EXPECT_EQ(out.wait_us, 4000u);
    const uint8_t eq_retry[6] = {0x11, 0, 0, 0, 0x0E, 0};
    out = switch_dp_train_status(&t, eq_retry);
    EXPECT_EQ(out.dpcd[1], 2 | (1 << 3) | SWITCH_DPCD_MAX_PRE_EMPHASIS_REACHED);
    switch_dp_train t2{};
    out = switch_dp_train_start(&t2, 1, false, 0);
    const uint8_t done[6] = {0x01, 0, 0, 0, 0, 0};
    out = switch_dp_train_status(&t2, done);
    EXPECT_EQ(out.dpcd[0], 0x22);
    EXPECT_EQ(out.wait_us, 400u);
}

TEST(Training, ClockThatNeverLocksFails)
{
    Sink sink;
    sink.never_locks = true;
    switch_dp_train t{};
    unsigned drives = 0;
    EXPECT_EQ(train(sink, t, false, &drives).act, SWITCH_DP_TRAIN_FAIL);
    EXPECT_NE(t.why, nullptr);
    EXPECT_LE(drives, SWITCH_DP_CR_TRIES_MAX);
}

TEST(Training, ClockLostInEqualisationFails)
{
    Sink sink;
    sink.loses_clock_in_eq = true;
    switch_dp_train t{};
    unsigned drives = 0;
    EXPECT_EQ(train(sink, t, false, &drives).act, SWITCH_DP_TRAIN_FAIL);
}

TEST(Training, EqualisationGivesUpAfterFiveTries)
{
    Sink sink;
    sink.emphasis = 3;
    sink.swing = 1;  // asks for 1 + 3: SOR1 tops out at 1 + 2, so it never equalises
    switch_dp_train t{};
    unsigned drives = 0;
    EXPECT_EQ(train(sink, t, false, &drives).act, SWITCH_DP_TRAIN_FAIL);
    EXPECT_EQ(t.tries, SWITCH_DP_EQ_TRIES_MAX);
}

TEST(Training, LinkHolds)
{
    const uint8_t good[3] = {0x77, 0x77, 0x01};
    EXPECT_TRUE(switch_dp_link_holds(good, 4));
    const uint8_t lost[3] = {0x77, 0x37, 0x01};
    EXPECT_FALSE(switch_dp_link_holds(lost, 4));
    EXPECT_TRUE(switch_dp_link_holds(lost, 2));
    const uint8_t unaligned[3] = {0x77, 0x00, 0x00};
    EXPECT_FALSE(switch_dp_link_holds(unaligned, 2));
}

}  // namespace
