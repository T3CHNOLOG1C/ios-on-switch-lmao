// SPDX-License-Identifier: MIT
// third_party/mesa/src/nouveau/compiler/nak_nir_algebraic.py: metal2vulkan's half rounding on a device without Float16
// (fquantize2f16, and where that gave zero the float rounded to the half denormal grid, 2^-24) is the round trip through
// f16 with round-to-nearest-even keeping denormals, for every f32 input, so NAK may emit it as two conversions.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace {

float asFloat(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}

uint32_t bitsOf(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

// NIR's fquantize2f16 (nir_opcodes.py)
float quantize(float x)
{
    return std::fabs(x) < 0x1p-14f ? std::copysign(0.0f, x) : float(_Float16(x));
}

float metal2vulkan(float x)
{
    const float q = quantize(x);
    return q == 0.0f ? std::nearbyint(x * 0x1p24f) * 0x1p-24f : q;
}

float roundTrip(float x)
{
    return float(_Float16(x));
}

TEST(NakHalfRound, Metal2vulkansHalfRoundingIsTheF16RoundTripForEveryFloat)
{
    uint64_t differ = 0;
    for (uint64_t i = 0; i <= 0xffffffffull; i++) {
        const float x = asFloat(uint32_t(i)), a = metal2vulkan(x), b = roundTrip(x);
        if (!(std::isnan(a) && std::isnan(b)) && bitsOf(a) != bitsOf(b))
            differ++;
    }
    EXPECT_EQ(differ, 0u);
}

}  // namespace
