// SPDX-License-Identifier: MIT
// raw GM20B path: a multisampled color attachment's resolve (MTLStoreActionMultisampleResolve), drawn one pixel a fragment over
// the pass's render area into the resolve texture, by the rules m2v resolves it by on this GPU, so a resolve is the same pixels
// whichever path draws it (metal2vulkan-runtime color_resolve.rs exact, metal2vulkan bind/color_resolve.rs):
//  - Metal's measured rule where it has one: an 8-bit unorm channel's codes summed with half the count, over the count; an 8-bit
//    sRGB channel's code (the nearest to its decoded value) as a 12-bit linear value (resolve_srgb.h), those averaged so and
//    encoded by Metal's thresholds, written as the code through the target's UNORM twin; a half channel at two samples, the
//    exact mean rounded to a half with a tie toward +infinity;
//  - else the device's average, as NVK resolves (vk_meta_resolve_rendering AVERAGE): the samples summed in order, times one
//    over their count.
// One function a target format: the pack keys an own state by its function's name. The sample count comes in buffer 0, as the
// attachment's (the shader does not query the texture). Built without fast math (rawpack.py), so the sums keep their order and
// the bit tests their NaNs.
#include <metal_stdlib>
#include "resolve_srgb.h"
using namespace metal;

struct Out {
    float4 position [[position]];
};

vertex Out resolve_vert(uint vid [[vertex_id]])
{
    Out o;
    o.position = float4((vid & 1) ? 1.0 : -1.0, (vid & 2) ? 1.0 : -1.0, 0.0, 1.0);
    return o;
}

typedef texture2d_ms<float, access::read> Samples;

// The code of a decoded 8-bit sRGB value (the midpoints at or below it), and the code Metal encodes a 12-bit linear value as (the
// thresholds it reaches): each the count of a table's 255 ascending entries x reaches, by eight steps of a binary search. Each
// reads its table by name, so m2v keeps the tables as the program's own constants.
static uint codeOfDecoded(float x)
{
    uint lo = 0;
    for (uint step = 128; step; step >>= 1)
        lo = x >= kSrgbMidpoints[lo + step - 1] ? lo + step : lo;
    return lo;
}

static uint codeOfLinear(uint x)
{
    uint lo = 0;
    for (uint step = 128; step; step >>= 1)
        lo = x >= kSrgbThresholds[lo + step - 1] ? lo + step : lo;
    return lo;
}

static float4 deviceAverage(Samples s, uint n, uint2 at)
{
    float4 sum = s.read(at, 0);
    for (uint k = 1; k < n; k++)
        sum += s.read(at, k);
    return sum * (1.0 / float(n));
}

static float4 unorm8(Samples s, uint n, uint2 at)
{
    uint4 sum = uint4(n / 2);
    for (uint k = 0; k < n; k++)
        sum += uint4(s.read(at, k) * 255.0 + 0.5);
    return float4(sum / n) * as_type<float>(0x3b808081u);  // 1 / 255
}

static float4 srgb8(Samples s, uint n, uint2 at)
{
    uint4 sum = uint4(n / 2);
    for (uint k = 0; k < n; k++) {
        const float4 x = s.read(at, k);
        for (uint c = 0; c < 3; c++)
            sum[c] += kSrgbLinear[codeOfDecoded(x[c])];
        sum.w += uint(x.w * 255.0 + 0.5);
    }
    sum /= n;
    return float4(codeOfLinear(sum.x), codeOfLinear(sum.y), codeOfLinear(sum.z), sum.w) * as_type<float>(0x3b808081u);
}

// Two halves' resolve: a NaN counts 0 and an infinity 65536 of its sign; the mean rounded to the half grid with a tie toward
// +infinity as floor(x / u + 1/2) u, u the grid's step at x (each step exact); 65536 or more an infinity, a zero +0
static float counted(float x)
{
    const uint bits = as_type<uint>(x), m = bits & 0x7fffffffu;
    return as_type<float>(m > 0x7f800000u ? 0u : m == 0x7f800000u ? (bits & 0x80000000u) | 0x47800000u : bits);
}

static float halfMean(float a, float b)
{
    const float x = (counted(a) + counted(b)) * 0.5;
    const uint field = max((as_type<uint>(x) & 0x7fffffffu) >> 23, 127u - 14u) - 10u;
    const float units = floor(x * as_type<float>((254u - field) << 23) + 0.5);
    const uint rounded = as_type<uint>(units * as_type<float>(field << 23));
    const uint rm = rounded & 0x7fffffffu;
    return as_type<float>(rm == 0 ? 0u : rm >= 0x47800000u ? (rounded & 0x80000000u) | 0x7f800000u : rounded);
}

static float4 halfResolve(Samples s, uint n, uint2 at)
{
    if (n != 2)
        return deviceAverage(s, n, at);
    const float4 a = s.read(at, 0), b = s.read(at, 1);
    return float4(halfMean(a.x, b.x), halfMean(a.y, b.y), halfMean(a.z, b.z), halfMean(a.w, b.w));
}

fragment float4 resolve_rgba8_frag(Out in [[stage_in]], Samples s [[texture(0)]], constant uint &n [[buffer(0)]])
{
    return unorm8(s, n, uint2(in.position.xy));
}

fragment float4 resolve_rgba8_srgb_frag(Out in [[stage_in]], Samples s [[texture(0)]], constant uint &n [[buffer(0)]])
{
    return srgb8(s, n, uint2(in.position.xy));
}

fragment float4 resolve_bgra8_frag(Out in [[stage_in]], Samples s [[texture(0)]], constant uint &n [[buffer(0)]])
{
    return unorm8(s, n, uint2(in.position.xy));
}

fragment float4 resolve_bgra8_srgb_frag(Out in [[stage_in]], Samples s [[texture(0)]], constant uint &n [[buffer(0)]])
{
    return srgb8(s, n, uint2(in.position.xy));
}

fragment float4 resolve_rg11b10f_frag(Out in [[stage_in]], Samples s [[texture(0)]], constant uint &n [[buffer(0)]])
{
    return deviceAverage(s, n, uint2(in.position.xy));
}

fragment float4 resolve_rgba16f_frag(Out in [[stage_in]], Samples s [[texture(0)]], constant uint &n [[buffer(0)]])
{
    return halfResolve(s, n, uint2(in.position.xy));
}
