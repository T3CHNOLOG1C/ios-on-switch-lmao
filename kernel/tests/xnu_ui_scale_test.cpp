// bsd/vm/switch_ui_scale.h: the scales switch-ui-scale names and the cache text each one's pager sites write.
// Encodings are checked against clang's assembler output and against the bytes the 1.6-only sites shipped.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "bazel/testing/source_text.h"

extern "C" {
#include "switch_ui_scale.h"
}

namespace {
std::vector<uint8_t> EditBytes(switch_ui_scale_edit_kind kind, const switch_ui_scale *scale, size_t size)
{
    std::vector<uint8_t> out(size);
    EXPECT_TRUE(switch_ui_scale_edit_bytes(kind, scale, out.data(), size));
    return out;
}

const switch_ui_scale *Named(const char *name)
{
    const switch_ui_scale *scale = switch_ui_scale_named(name);
    EXPECT_NE(scale, nullptr) << name;
    return scale;
}

// VFPExpandImm for a double: imm8 = a:b:cd:efgh.
double ExpandFmovImm(uint32_t insn)
{
    const unsigned imm8 = (insn >> 13) & 0xff;
    const unsigned b = (imm8 >> 6) & 1, cd = (imm8 >> 4) & 3, efgh = imm8 & 0xf;
    const int n = b ? static_cast<int>(cd) - 3 : static_cast<int>(cd) + 1;
    const double value = (16.0 + efgh) / 16.0 * std::ldexp(1.0, n);
    return (imm8 & 0x80) ? -value : value;
}
}  // namespace

TEST(XnuUiScale, EveryScaleIsItsNameAndAWholePointScreen)
{
    ASSERT_GE(SWITCH_UI_SCALES, 1u);
    EXPECT_TRUE(switch_ui_scale_is_unit(&switch_ui_scales[SWITCH_UI_SCALE_UNIT]));
    for (size_t i = 0; i < SWITCH_UI_SCALES; i++) {
        const switch_ui_scale &s = switch_ui_scales[i];
        SCOPED_TRACE(s.name);
        // the decimal name as a fraction, compared exactly
        const std::string name = s.name;
        const size_t dot = name.find('.');
        const std::string digits = dot == std::string::npos ? name : name.substr(0, dot) + name.substr(dot + 1);
        uint64_t denominator = 1;
        for (size_t d = dot == std::string::npos ? 0 : name.size() - dot - 1; d > 0; d--) {
            denominator *= 10;
        }
        EXPECT_EQ(std::stoull(digits) * s.den, static_cast<uint64_t>(s.num) * denominator);
        float as_float = static_cast<float>(s.num) / static_cast<float>(s.den);
        uint32_t bits = 0;
        std::memcpy(&bits, &as_float, sizeof(bits));
        EXPECT_EQ(s.float_bits, bits);
        EXPECT_GE(s.num, s.den);
        EXPECT_GE(s.den, 1u);
        EXPECT_LE(s.num, 31u);
        EXPECT_EQ(720u * s.den % s.num, 0u);
        EXPECT_EQ(1280u * s.den % s.num, 0u);
        EXPECT_EQ(switch_ui_scale_named(s.name), &s);
    }
}

TEST(XnuUiScale, BootArgValues)
{
    EXPECT_EQ(switch_ui_scale_for_boot_arg(nullptr), &switch_ui_scales[SWITCH_UI_SCALE_DEFAULT]);
    EXPECT_EQ(switch_ui_scale_for_boot_arg("1")->num, 1u);
    EXPECT_EQ(switch_ui_scale_for_boot_arg("1.25")->num, 5u);
    EXPECT_EQ(switch_ui_scale_for_boot_arg("1.25")->den, 4u);
    EXPECT_EQ(switch_ui_scale_for_boot_arg("1.6")->num, 8u);
    EXPECT_EQ(switch_ui_scale_for_boot_arg("1.6")->den, 5u);
    for (const char *bad : {"", "1.60", "1.6x", "2", "1.5", "8/5", "16", "1."}) {
        EXPECT_EQ(switch_ui_scale_named(bad), nullptr) << bad;
        EXPECT_EQ(switch_ui_scale_for_boot_arg(bad), &switch_ui_scales[SWITCH_UI_SCALE_UNIT]) << bad;
    }
}

TEST(XnuUiScale, FmovImmediateMatchesTheAssembler)
{
    // clang -arch arm64: fmov d8, #1.0 / #5.0 / #8.0; fmov d1, #4.0 / #5.0 / #17.0 / #31.0
    EXPECT_EQ(switch_a64_fmov_d_int(8, 1), 0x1e6e1008u);
    EXPECT_EQ(switch_a64_fmov_d_int(8, 5), 0x1e629008u);
    EXPECT_EQ(switch_a64_fmov_d_int(8, 8), 0x1e641008u);
    EXPECT_EQ(switch_a64_fmov_d_int(1, 4), 0x1e621001u);
    EXPECT_EQ(switch_a64_fmov_d_int(1, 5), 0x1e629001u);
    EXPECT_EQ(switch_a64_fmov_d_int(1, 17), 0x1e663001u);
    EXPECT_EQ(switch_a64_fmov_d_int(1, 31), 0x1e67f001u);
    for (unsigned value = 1; value <= 31; value++) {
        const uint32_t insn = switch_a64_fmov_d_int(3, value);
        EXPECT_EQ(insn & ~(0xffu << 13), 0x1e601003u) << value;
        EXPECT_EQ(ExpandFmovImm(insn), static_cast<double>(value)) << value;
    }
}

TEST(XnuUiScale, FmovFractionMatchesTheAssembler)
{
    // clang -arch arm64: fmov d2, #2.5 / #1.25 / #0.125 / #31.0
    uint32_t insn = 0;
    ASSERT_TRUE(switch_a64_fmov_d(2, 5, 2, &insn));
    EXPECT_EQ(insn, 0x1e609002u);
    ASSERT_TRUE(switch_a64_fmov_d(2, 10, 8, &insn));
    EXPECT_EQ(insn, 0x1e6e9002u);
    ASSERT_TRUE(switch_a64_fmov_d(2, 1, 8, &insn));
    EXPECT_EQ(insn, 0x1e681002u);
    ASSERT_TRUE(switch_a64_fmov_d(2, 31, 1, &insn));
    EXPECT_EQ(insn, 0x1e67f002u);
    // every positive immediate, as (16 + efgh) * 2^(n + 3) / 128
    for (unsigned imm8 = 0; imm8 < 0x80; imm8++) {
        const uint32_t expected = 0x1e601002u | (imm8 << 13);
        const unsigned b = (imm8 >> 6) & 1, cd = (imm8 >> 4) & 3, efgh = imm8 & 0xf;
        const unsigned p = b ? cd : cd + 4;
        ASSERT_TRUE(switch_a64_fmov_d(2, (16 + efgh) << p, 128, &insn)) << imm8;
        EXPECT_EQ(insn, expected) << imm8;
        EXPECT_EQ(ExpandFmovImm(insn), (16.0 + efgh) * std::ldexp(1.0, static_cast<int>(p)) / 128.0) << imm8;
    }
    for (const auto &[num, den] : {std::pair{16u, 5u}, {8u, 5u}, {1u, 3u}, {33u, 1u}, {1u, 16u}, {64u, 1u}}) {
        EXPECT_FALSE(switch_a64_fmov_d(2, num, den, &insn)) << num << "/" << den;
    }
}

TEST(XnuUiScale, FrontBoardServicesOverride)
{
    // what the 1.6-only site wrote: fmov d8, #8.0; fmov d1, #5.0; fdiv d8, d8, d1; tbnz w0, #0, +0x8; fmov d8, xzr
    const std::vector<uint8_t> shipped = {0x08, 0x10, 0x64, 0x1e, 0x01, 0x90, 0x62, 0x1e, 0x08, 0x19,
                                          0x61, 0x1e, 0x40, 0x00, 0x00, 0x37, 0xe8, 0x03, 0x67, 0x9e};
    EXPECT_EQ(EditBytes(SWITCH_US_FBS_OVERRIDE, Named("1.6"), 20), shipped);
    const std::vector<uint8_t> five_fourths = {0x08, 0x90, 0x62, 0x1e, 0x01, 0x10, 0x62, 0x1e, 0x08, 0x19,
                                               0x61, 0x1e, 0x40, 0x00, 0x00, 0x37, 0xe8, 0x03, 0x67, 0x9e};
    EXPECT_EQ(EditBytes(SWITCH_US_FBS_OVERRIDE, Named("1.25"), 20), five_fourths);
}

TEST(XnuUiScale, MobileGestaltScale)
{
    // movz w19, #0xcccd; movk w19, #0x3fcc, lsl #16 (the 1.6-only site); movz w19, #0; movk w19, #0x3fa0, lsl #16
    EXPECT_EQ(EditBytes(SWITCH_US_MG_SCALE, Named("1.6"), 8),
              (std::vector<uint8_t>{0xb3, 0x99, 0x99, 0x52, 0x93, 0xf9, 0xa7, 0x72}));
    EXPECT_EQ(EditBytes(SWITCH_US_MG_SCALE, Named("1.25"), 8),
              (std::vector<uint8_t>{0x13, 0x00, 0x80, 0x52, 0x13, 0xf4, 0xa7, 0x72}));
}

TEST(XnuUiScale, ProgressBarIsWholePixels)
{
    for (size_t i = 0; i < SWITCH_UI_SCALES; i++) {
        const switch_ui_scale &s = switch_ui_scales[i];
        if (switch_ui_scale_is_unit(&s)) {
            continue;
        }
        SCOPED_TRACE(s.name);
        float scale = 0;
        std::memcpy(&scale, &s.float_bits, sizeof(scale));
        for (size_t side = 0; side < 2; side++) {
            float points = 0;
            std::memcpy(&points, &s.progress_bar_bits[side], sizeof(points));
            const volatile float pixels = points * scale;
            EXPECT_EQ(pixels, std::floor(pixels)) << side;
            EXPECT_LT(std::fabs(points - (side ? 4.0f : 239.0f)), 0.5f) << side;
        }
    }
    // little-endian floats over ProgressUI's 239.0f, 4.0f
    EXPECT_EQ(EditBytes(SWITCH_US_PROGRESS_BAR, Named("1.6"), 8),
              (std::vector<uint8_t>{0x00, 0xc0, 0x6e, 0x43, 0x00, 0x00, 0x70, 0x40}));
    EXPECT_EQ(EditBytes(SWITCH_US_PROGRESS_BAR, Named("1.25"), 8),
              (std::vector<uint8_t>{0x33, 0x33, 0x6f, 0x43, 0x00, 0x00, 0x80, 0x40}));
}

TEST(XnuUiScale, BatteryFillMatchesTheAssembler)
{
    // clang -arch arm64: fmov d2, #2.5; fmul d1, d12, d2; frintn d1, d1; fdiv d13, d1, d2; fmov d3, #-4.0;
    // fmadd d11, d13, d3, d11; fmadd d8, d13, d3, d8
    EXPECT_EQ(
        EditBytes(SWITCH_US_BATTERY_FILL, Named("1.25"), 28),
        (std::vector<uint8_t>{0x02, 0x90, 0x60, 0x1e, 0x81, 0x09, 0x62, 0x1e, 0x21, 0x40, 0x64, 0x1e, 0x2d, 0x18,
                              0x62, 0x1e, 0x03, 0x10, 0x72, 0x1e, 0xab, 0x2d, 0x43, 0x1f, 0xa8, 0x21, 0x43, 0x1f}));
    uint8_t out[28] = {};
    EXPECT_FALSE(switch_ui_scale_edit_bytes(SWITCH_US_BATTERY_FILL, Named("1.6"), out, sizeof(out)));
    for (uint8_t byte : out) {
        EXPECT_EQ(byte, 0);
    }
}

namespace {
// -[_UIStatusBarBatteryView _fillLayerFrame:cornerRadius:] with both battery edits, at 100% charge: the fill's rect
// in pixels from the body's origin, for a body and line width in points.
struct PixelRect {
    double x, y, width, height;
};

PixelRect BatteryFill(const switch_ui_scale &s, double body_width, double body_height, double line_width)
{
    const std::vector<uint8_t> code = EditBytes(SWITCH_US_BATTERY_FILL, &s, 28);
    auto insn = [&code](size_t i) {
        uint32_t word = 0;
        std::memcpy(&word, code.data() + 4 * i, sizeof(word));
        return word;
    };
    const double d2 = ExpandFmovImm(insn(0));
    const double d13 = std::nearbyint(line_width * d2) / d2;
    const double d3 = ExpandFmovImm(insn(4));
    const double width = std::fma(d13, d3, body_width), height = std::fma(d13, d3, body_height);
    const double origin = d13 + d13;
    const double pixels = static_cast<double>(s.num) / s.den;
    return {origin * pixels, origin * pixels, width * pixels, height * pixels};
}
}  // namespace

TEST(XnuUiScale, BatteryFillIsWholePixelsWithEqualGaps)
{
    const switch_ui_scale &s = *Named("1.25");
    // the status bar's battery: 24 x 12 points, 1-point line, 30 x 15 pixels
    const PixelRect fill = BatteryFill(s, 24, 12, 1);
    EXPECT_NEAR(fill.x, 2, 1e-9);
    EXPECT_NEAR(fill.y, 2, 1e-9);
    EXPECT_NEAR(fill.width, 26, 1e-9);
    EXPECT_NEAR(fill.height, 11, 1e-9);
    EXPECT_NEAR(30 - (fill.x + fill.width), fill.x, 1e-9);
    EXPECT_NEAR(15 - (fill.y + fill.height), fill.y, 1e-9);
    // the other line widths _lineWidthAndInterspaceForIconSize: answers: insets of whole pixels, where Core Animation
    // starts the stock fill (the first pixel whose centre the 2 * line_width * 1.25 inset covers)
    for (double line_width : {1.0, 7.0 / 6.0, 1.25}) {
        const double inset = BatteryFill(s, 24, 12, line_width).x;
        EXPECT_NEAR(inset, std::round(inset), 1e-9) << line_width;
        EXPECT_EQ(std::round(inset), std::ceil(2 * line_width * 1.25 - 0.5)) << line_width;
    }
}

TEST(XnuUiScale, AWrongLengthOrAFixedEditWritesNothing)
{
    uint8_t out[20] = {};
    EXPECT_FALSE(switch_ui_scale_edit_bytes(SWITCH_US_FBS_OVERRIDE, Named("1.6"), out, 16));
    EXPECT_FALSE(switch_ui_scale_edit_bytes(SWITCH_US_MG_SCALE, Named("1.6"), out, 12));
    EXPECT_FALSE(switch_ui_scale_edit_bytes(SWITCH_US_PROGRESS_BAR, Named("1.6"), out, 4));
    EXPECT_FALSE(switch_ui_scale_edit_bytes(SWITCH_US_BATTERY_FILL, Named("1.25"), out, 20));
    EXPECT_FALSE(switch_ui_scale_edit_bytes(SWITCH_US_FIXED, Named("1.6"), out, 12));
    for (uint8_t byte : out) {
        EXPECT_EQ(byte, 0);
    }
}

TEST(XnuUiScaleSource, CoreUiRowBytesAndStockAtScaleOne)
{
    const std::string vm_unix = switch_ios::test::SourceText("../xnu-10063.141.1/bsd/vm/vm_unix.c");
    // both CoreUI sites: ... ucvtf d0, x24; fmul d0, dW, d0; fcvtzu x3, d0 -> mul x3, x0, x24; nop; nop
    const std::string row_bytes = "{ 0x03, 0x7c, 0x18, 0x9b, 0x1f, 0x20, 0x03, 0xd5, 0x1f, 0x20, 0x03, 0xd5 }";
    EXPECT_EQ(switch_ios::test::Count(vm_unix, row_bytes), 2u);
    const std::string pager =
        switch_ios::test::Body(vm_unix, "switch_ui_scale_pager(const struct switch_us_site *site");
    const size_t unit = pager.find("if (switch_ui_scale_is_unit(switch_ui_scale())) {");
    ASSERT_NE(unit, std::string::npos);
    EXPECT_LT(unit, pager.find("vn_rdwr("));
    EXPECT_LT(unit, pager.find("apple_protect_pager_setup("));
    // a site with no bytes at the boot's scale (the battery fill at 1.6) maps no pager either
    const size_t no_bytes = pager.find("!switch_ui_scale_edit_bytes(site->edit[i].kind, switch_ui_scale()");
    ASSERT_NE(no_bytes, std::string::npos);
    EXPECT_LT(no_bytes, pager.find("apple_protect_pager_setup("));
}
