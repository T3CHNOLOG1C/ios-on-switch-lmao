// SwitchAudio's driver and plug-in use the S32 link as switch_audio_format_test checks it: the driver programs
// exactly SwitchAudioFormat.h's words (no hand-written duplicates), its I2S clock is 47999.878 Hz on PLLA (its own
// PLLA constants through the header's divider and rate helpers), and the ABI and plug-in are S32 throughout.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <iterator>
#include <optional>
#include <regex>
#include <string>
#include <type_traits>

#include "../SwitchAudioAbi.h"
#include "bazel/testing/source_text.h"
#include "switch_audio_format_harness.h"
#include "switch_audio_words.h"

namespace {

using ioswitch::test::SourceText;
using switch_audio_linux::Fraction;
using testing::HasSubstr;
using testing::Not;

// The ABI: version 3 is S32_LE samples; periods stay whole ADMA buffers and the rings DMA-able (16384 frames of
// stereo S32: 128 KiB).
static_assert(SWITCH_AUDIO_VERSION == 3u);
static_assert(std::is_same_v<switch_audio_sample_t, int32_t>);
static_assert(SWITCH_AUDIO_RING_FRAMES * switch_audio_linux::kChannels * sizeof(switch_audio_sample_t) == 128 * 1024);

// A `name = <number>` constant of the driver's source, if it is defined as a literal.
std::optional<int64_t> constant(const std::string &text, const std::string &name)
{
    const std::regex pattern("\\b" + name + "\\s*=\\s*(0x[0-9a-fA-F]+|[0-9]+)");
    std::smatch match;
    if (!std::regex_search(text, match, pattern)) return std::nullopt;
    return std::stoll(match[1].str(), nullptr, 0);
}

TEST(SwitchAudioSource, I2sClockIsASixteenthOfPllaOut0)
{
    const std::string driver = SourceText("../SwitchAudio.cpp");
    const auto m = constant(driver, "kPllaM"), n = constant(driver, "kPllaN"), p = constant(driver, "kPllaP");
    const auto sdm = constant(driver, "kPllaSdm");
    ASSERT_TRUE(m && n && p && sdm);
    EXPECT_EQ(*m, 2);
    EXPECT_EQ(*n, 38);
    EXPECT_EQ(*p, 2);
    EXPECT_EQ(*sdm, 0xfccc);
    const int64_t sdmSigned = *sdm & 0x8000 ? *sdm - 0x10000 : *sdm;
    const int64_t nEff = 8192 * *n + 4096 + sdmSigned;
    const Fraction plla(38400000 * (__int128)nEff, 8192 * (__int128)*m * *p);
    EXPECT_TRUE(plla == Fraction(737278125, 2));  // 368639062.5 Hz (clk-tegra210b01.c table entry)
    const auto out0x2 = constant(driver, "kPllaOut0X2"), perFs = constant(driver, "kPllaOut0PerFs");
    ASSERT_TRUE(out0x2 && perFs);
    EXPECT_EQ(*out0x2, 15);
    EXPECT_EQ(*perFs, 1024);
    EXPECT_FALSE(constant(driver, "kPllaOut0Div").has_value());  // derived: kPllaOut0X2 - 2 (7.1 of 7.5)
    EXPECT_THAT(driver, HasSubstr("kPllaOut0Div = kPllaOut0X2 - 2"));
    const Fraction pllaOut0 = plla * Fraction(2) / Fraction(*out0x2);
    const uint32_t divider = neon_h_i2s_divider((uint32_t)*perFs);
    EXPECT_EQ(divider, 30u);
    const Fraction bclk = pllaOut0 / (Fraction(divider, 2) + Fraction(1));  // 7.1 divider: ratio = value / 2 + 1
    const Fraction fs = bclk / Fraction(neon_h_bclk_fs());
    EXPECT_TRUE(fs == plla / (Fraction(15, 2) * Fraction(32 * 32)));
    EXPECT_EQ(std::round((fs.value() - 47999.878) * 1000), 0.0) << fs.value();
    // (The values fit 64 bits; the arithmetic is exact in 128.)
    EXPECT_EQ(
        neon_h_rate_millihz(38400000, (uint64_t)*m, (uint64_t)*p, (uint64_t)nEff, (uint64_t)*out0x2, (uint64_t)*perFs),
        (uint64_t)(fs * Fraction(1000)).floor());
    for (uint64_t hostHz : {19200000ull, 24000000ull}) {
        const uint64_t q32 = neon_h_ticks_per_frame_q32(hostHz, 38400000, (uint64_t)*m, (uint64_t)*p, (uint64_t)nEff,
                                                        (uint64_t)*out0x2, (uint64_t)*perFs);
        EXPECT_EQ(q32, (uint64_t)(Fraction((__int128)hostHz) / fs * Fraction((__int128)1 << 32)).floor()) << hostHz;
    }
    // The driver programs the rate through these helpers.
    EXPECT_THAT(
        driver,
        HasSubstr("switch_audio_ticks_per_frame_q32(hz, refHz, kPllaM, kPllaP, (uint64_t)kPllaNEff, kPllaOut0X2,"));
}

TEST(SwitchAudioSource, DriverProgramsTheHeaderWords)
{
    const std::string d = SourceText("../SwitchAudio.cpp");
    for (const char *line :
         {"w32(ahub, ADMAIF_TX_CIF, switch_audio_admaif_cif(SWITCH_AUDIO_OUT_CHANNELS));",
          "w32(ahub, ADMAIF_RX_CIF, switch_audio_admaif_cif(SWITCH_AUDIO_IN_CHANNELS));",
          "w32(ahub, I2S_RX_CIF, switch_audio_i2s_rx_cif());", "w32(ahub, I2S_TX_CIF, switch_audio_i2s_tx_cif());",
          "w32(ahub, I2S_CTRL, kI2sCtrl);", "w32(ahub, I2S_TIMING, kI2sTiming);", "w32(car, CLK_SOURCE_I2S0, kI2sDiv);",
          "codecUpdate(I2S1_SDP, SWITCH_AUDIO_RT5640_SDP_MASK, SWITCH_AUDIO_RT5640_SDP);",
          "codecUpdate(ADDA_CLK1, SWITCH_AUDIO_RT5640_ADDA_MASK, SWITCH_AUDIO_RT5640_ADDA);"})
        EXPECT_THAT(d, HasSubstr(line));
    EXPECT_THAT(d, HasSubstr("kI2sCtrl = switch_audio_i2s_ctrl(), kI2sTiming = switch_audio_i2s_timing()"));
    EXPECT_THAT(d, HasSubstr("kI2sDiv = switch_audio_i2s_divider(kPllaOut0PerFs)"));
    for (const char *gone : {"kPack16", "constexpr uint32_t cif(", "sizeof(int16_t)", "int16_t *"})
        EXPECT_THAT(d, Not(HasSubstr(gone)));
    // Nothing else writes the CIF registers.
    const std::regex writes(R"(w32\(ahub, (?:ADMAIF|I2S)_[RT]X_CIF,)");
    EXPECT_EQ(std::distance(std::sregex_iterator(d.begin(), d.end(), writes), std::sregex_iterator()), 4);
}

TEST(SwitchAudioSource, AbiIsS32)
{
    const std::string driver = SourceText("../SwitchAudio.cpp");
    const std::string plugin = SourceText("../hal/SwitchAudioPlugIn.c");
    EXPECT_THAT(driver, HasSubstr("sizeof(switch_audio_sample_t)"));
    EXPECT_THAT(plugin, Not(HasSubstr("int16_t")));
    EXPECT_THAT(plugin,
                HasSubstr("switch_audio_ring_mix(gOut, SWITCH_AUDIO_RING_FRAMES, 2, first, main, frames, gain())"));
    EXPECT_THAT(plugin, HasSubstr("switch_audio_s32_to_float(gIn["));
}

}  // namespace
