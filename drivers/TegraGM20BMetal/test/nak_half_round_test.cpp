// SPDX-License-Identifier: MIT
// third_party/mesa/src/nouveau/compiler/nak_nir_algebraic.py: metal2vulkan's half rounding on a device without Float16
// (fquantize2f16, and where that gave zero the float rounded to the half denormal grid, 2^-24) is the round trip through
// f16 with round-to-nearest-even keeping denormals, for every f32 input, so NAK may emit it as two conversions; and two
// halves added or multiplied in f32 and rounded back to half are the half sum or product (this Mac's f16 arithmetic,
// denormals kept), for every pair, so NAK may emit SM53's HADD2 and HMUL2 for them.
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

uint16_t halfBits(_Float16 x)
{
    uint16_t u;
    memcpy(&u, &x, 2);
    return u;
}

_Float16 asHalf(uint16_t u)
{
    _Float16 x;
    memcpy(&x, &u, 2);
    return x;
}

// The CPU's own f16 instructions, so the compiler cannot evaluate the half arithmetic in float
_Float16 nativeAdd(_Float16 a, _Float16 b)
{
    _Float16 r;
    __asm__("fadd %h0, %h1, %h2" : "=w"(r) : "w"(a), "w"(b));
    return r;
}

_Float16 nativeMul(_Float16 a, _Float16 b)
{
    _Float16 r;
    __asm__("fmul %h0, %h1, %h2" : "=w"(r) : "w"(a), "w"(b));
    return r;
}

_Float16 narrow(float f)
{
    _Float16 r;
    __asm__("fcvt %h0, %s1" : "=w"(r) : "w"(f));
    return r;
}

float widen(_Float16 a)
{
    float r;
    __asm__("fcvt %s0, %h1" : "=w"(r) : "w"(a));
    return r;
}

bool halfNan(uint16_t u)
{
    return (u & 0x7c00) == 0x7c00 && (u & 0x3ff);
}

TEST(NakHalfRound, HalfSumsAndProductsInFloatRoundToTheHalfOnes)
{
    uint64_t sums = 0, products = 0;
    for (uint32_t i = 0; i < 65536; i++)
        for (uint32_t j = 0; j < 65536; j++) {
            const _Float16 a = asHalf(uint16_t(i)), b = asHalf(uint16_t(j));
            const float x = widen(a), y = widen(b);
            uint16_t viaFloat = halfBits(narrow(x + y)), native = halfBits(nativeAdd(a, b));
            sums += viaFloat != native && !(halfNan(viaFloat) && halfNan(native));
            viaFloat = halfBits(narrow(x * y));
            native = halfBits(nativeMul(a, b));
            products += viaFloat != native && !(halfNan(viaFloat) && halfNan(native));
        }
    EXPECT_EQ(sums, 0u);
    EXPECT_EQ(products, 0u);
}

}  // namespace
