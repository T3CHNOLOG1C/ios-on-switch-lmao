// SPDX-License-Identifier: MIT
// raw GM20B path (r8-rawca): a two-plane 4:2:0 YCbCr surface's texture as Metal reads it, drawn into the image the raw path
// samples in its place, one texel a fragment. The arithmetic is m2v's (metal2vulkan bind/ycbcr_convert.rs, its kernel and
// `ycbcr_read`), which reproduces an Apple M4's measured reads exactly:
//  - chroma: the four nearest chroma texels weighted 9/3/3/1 (column x/2 and the next for an odd x, the one before for an even x,
//    each clamped to the plane, rows alike), the sum S rounded to a code, C = (S + 8) >> 4;
//  - converted (YCBCR8_420_2P, 500): with Cb' = 4 Cb - 512, Cr' = 4 Cr - 512 and the luma term L (video range 9576 (4 Y - 64),
//    full range 256 floor((32768 Y + 127) / 255)), each channel v = clamp((L + kb Cb' + kr Cr' + 2048) >> 12, 0, 2048) of its
//    (kb, kr), read as v / 2048 (an RGBA16Float image holds it exactly), alpha 1;
//  - sRGB (YCBCR8_420_2P_sRGB, 520): q = clamp((T + 512) >> 10, 0, 8192) of the same sum T through the knots of the sRGB curve,
//    read as step / 8192 (RGBA32Float), alpha 1;
//  - codes (RGB8_420_2P, 540): red Cr, green Y, blue Cb of the filtered codes, each code / 255 (RGBA8Unorm), alpha 1.
// Texture 0 is the luma plane (R8Unorm, a code a texel), texture 1 the chroma plane (RG8Unorm, Cb then Cr a texel), buffer 0 the
// shape; the vertex function draws the image's rectangle as a 4-vertex strip.
#include <metal_stdlib>
using namespace metal;

struct Shape {
    uint width, height;  // the texture's, in luma texels
    int full;            // full range (420f), else video range (420v)
    int k[6];            // red's kb, kr, then green's and blue's (m2v's `coefficients` of the range and matrix)
};

struct Out {
    float4 position [[position]];
};

vertex Out ycbcr_vert(uint vid [[vertex_id]])
{
    Out o;
    o.position = float4((vid & 1) ? 1.0 : -1.0, (vid & 2) ? 1.0 : -1.0, 0.0, 1.0);
    return o;
}

static uint code8(float v)
{
    return uint(rint(v * 255.0f));
}

// the two chroma texels of luma column (or row) p of a plane n across, and the first one's weight (the second's is 4 less it)
static uint3 taps(uint p, uint n)
{
    const uint c = p >> 1, odd = p & 1, a = c + odd, last = n - 1;
    const uint second = a > last ? last : a;
    const uint first = a == 0 ? 0 : a - 1;
    return uint3(odd ? c : first, second, odd ? 3 : 1);
}

// the texel's luma code and its filtered chroma codes (Cb, Cr)
static uint3 codes(texture2d<float, access::read> luma, texture2d<float, access::read> chroma, constant Shape &s, uint2 at)
{
    const uint y = code8(luma.read(at).r);
    const uint across = (s.width + 1) >> 1, down = (s.height + 1) >> 1;
    const uint3 tx = taps(at.x, across), ty = taps(at.y, down);
    const uint wx[2] = {tx.z, 4 - tx.z}, wy[2] = {ty.z, 4 - ty.z};
    const uint cx[2] = {tx.x, tx.y}, cy[2] = {ty.x, ty.y};
    uint cb = 0, cr = 0;
    for (uint j = 0; j < 2; j++)
        for (uint i = 0; i < 2; i++) {
            const float2 pair = chroma.read(uint2(cx[i], cy[j])).rg;
            const uint w = wx[i] * wy[j];
            cb += w * code8(pair.x);
            cr += w * code8(pair.y);
        }
    return uint3(y, (cb + 8) >> 4, (cr + 8) >> 4);
}

// T = L + kb Cb' + kr Cr' for each channel
static int3 sums(uint3 c, constant Shape &s)
{
    const int luma = s.full ? 256 * int((c.x * 32768 + 127) / 255) : 9576 * (int(c.x << 2) - 64);
    const int cb = int(c.y << 2) - 512, cr = int(c.z << 2) - 512;
    return int3(luma + s.k[0] * cb + s.k[1] * cr, luma + s.k[2] * cb + s.k[3] * cr, luma + s.k[4] * cb + s.k[5] * cr);
}

fragment float4 ycbcr_converted_frag(Out in [[stage_in]], texture2d<float, access::read> luma [[texture(0)]],
                                     texture2d<float, access::read> chroma [[texture(1)]],
                                     constant Shape &s [[buffer(0)]])
{
    const int3 t = sums(codes(luma, chroma, s, uint2(in.position.xy)), s);
    const int3 v = clamp((t + 2048) >> 12, 0, 2048);
    return float4(float3(v) / 2048.0f, 1.0f);
}

constant ushort kSrgbKnots[513] = {
#include "srgb_knots.h"
};

static float srgbStep(int t)
{
    const uint q = uint(clamp((t + 512) >> 10, 0, 8192));
    const uint i = min(q >> 4, 511u), f = q - 16 * i;
    const uint a = kSrgbKnots[i], b = kSrgbKnots[i + 1];
    const uint n = 16 * a + (b - a) * f;
    return float((n + 7 + ((n >> 4) & 1)) >> 4) / 8192.0f;
}

fragment float4 ycbcr_srgb_frag(Out in [[stage_in]], texture2d<float, access::read> luma [[texture(0)]],
                                texture2d<float, access::read> chroma [[texture(1)]], constant Shape &s [[buffer(0)]])
{
    const int3 t = sums(codes(luma, chroma, s, uint2(in.position.xy)), s);
    return float4(srgbStep(t.x), srgbStep(t.y), srgbStep(t.z), 1.0f);
}

fragment float4 ycbcr_codes_frag(Out in [[stage_in]], texture2d<float, access::read> luma [[texture(0)]],
                                 texture2d<float, access::read> chroma [[texture(1)]], constant Shape &s [[buffer(0)]])
{
    const uint3 c = codes(luma, chroma, s, uint2(in.position.xy));
    return float4(float(c.z) / 255.0f, float(c.x) / 255.0f, float(c.y) / 255.0f, 1.0f);
}
