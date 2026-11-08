// SPDX-License-Identifier: MIT
// See raw_emit.h. The rules are NVK's (nvk_cmd_draw.c: blend, the write-mask macro, viewport clip control; nvk_shader.c: the
// fragment program's push) applied to what m2v makes of a Metal descriptor.
#include "raw_emit.h"

#include <cstdio>
#include <iterator>
#include <map>

namespace raw {
namespace {

constexpr uint32_t kEarlyZ = 0x0210;         // SET_API_MANDATED_EARLY_Z
constexpr uint32_t kSingleCtWrite = 0x0f90;  // SET_SINGLE_CT_WRITE_CONTROL
constexpr uint32_t kBlend = 0x1360;          // SET_BLEND(j)
constexpr uint32_t kViewportClip = 0x193c;   // SET_VIEWPORT_CLIP_CONTROL
constexpr uint32_t kCtWrite = 0x1a00;        // SET_CT_WRITE(j)
constexpr uint32_t kBlendPerTarget =
    0x1e00;  // SET_BLEND_PER_TARGET_SEPARATE_FOR_ALPHA(j), then its ops and coefficients
constexpr uint32_t kRegisterCount[2] = {0x204c, 0x214c};  // SET_PIPELINE_REGISTER_COUNT(vertex B, pixel)
constexpr uint32_t kOpAdd = 0x8006, kZero = 0x4000;
constexpr uint32_t kViewportClipBase = 0x181c;  // NVK's, z_clamp_zero_one in bit 0

// MTLBlendOperation -> NV9097 OGL op
constexpr uint32_t kOps[] = {0x8006, 0x800a, 0x800b, 0x8007, 0x8008};
// MTLBlendFactor -> NV9097 OGL coefficient, through m2v's VkBlendFactor
constexpr uint32_t kFactors[] = {0x4000, 0x4001, 0x4300, 0x4301, 0x4302, 0x4303, 0x4306, 0x4307, 0x4304, 0x4305,
                                 0x4308, 0xc001, 0xc002, 0xc003, 0xc004, 0xc900, 0xc901, 0xc902, 0xc903};

// "c<j>=format:writeMask:blending:srcRGB:dstRGB:opRGB:srcAlpha:dstAlpha:opAlpha"
struct Color {
    unsigned format, writeMask, blending, factor[6];  // srcRGB, dstRGB, opRGB, srcAlpha, dstAlpha, opAlpha
};

// m2v holds an extended-range attachment as its raw codes (RGB10A8_2P_XR10, BGRA10_XR, BGR10_XR and _sRGB twins): the program
// writes the components of the format it is stored in, and blends in itself
bool extendedRange(unsigned format)
{
    return format >= 550 && format <= 555;
}

bool identityBlend(const Color &c)
{
    return c.factor[0] == 1 && c.factor[1] == 0 && c.factor[2] == 0 && c.factor[3] == 1 && c.factor[4] == 0 &&
           c.factor[5] == 0;
}

uint32_t bits(const uint8_t *header, unsigned lo, unsigned count)
{
    uint32_t v = 0;
    for (unsigned b = 0; b < count; b++)
        v |= uint32_t(header[(lo + b) / 8] >> ((lo + b) % 8) & 1) << b;
    return v;
}

// SET_CT_WRITE's layout: a component's enable in bits 0, 4, 8, 12 (r, g, b, a)
uint32_t ctWrite(unsigned components)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < 4; i++)
        if (components >> i & 1)
            v |= 1u << (4 * i);
    return v;
}

bool parse(const std::string &key, std::map<unsigned, Color> &colors, unsigned &depthFormat, std::string &why)
{
    depthFormat = 0;
    size_t at = 0;
    while (at < key.size()) {
        size_t end = key.find(' ', at);
        if (end == std::string::npos)
            end = key.size();
        const std::string field = key.substr(at, end - at);
        at = end + 1;
        unsigned j = 0, stencil = 0;
        Color c = {};
        if (field.rfind("ds=", 0) == 0 && sscanf(field.c_str(), "ds=%u/%u", &depthFormat, &stencil) == 2)
            continue;
        if (field[0] != 'c' || field.size() < 3 || field[1] < '0' || field[1] > '7')
            continue;
        if (sscanf(field.c_str(), "c%u=%u:%u:%u:%u:%u:%u:%u:%u:%u", &j, &c.format, &c.writeMask, &c.blending,
                   &c.factor[0], &c.factor[1], &c.factor[2], &c.factor[3], &c.factor[4], &c.factor[5]) != 10 ||
            c.factor[0] >= std::size(kFactors) || c.factor[1] >= std::size(kFactors) ||
            c.factor[3] >= std::size(kFactors) || c.factor[4] >= std::size(kFactors) ||
            c.factor[2] >= std::size(kOps) || c.factor[5] >= std::size(kOps)) {
            why = "a color field it can't read: " + field;
            return false;
        }
        colors[j] = c;
    }
    return true;
}

}  // namespace

bool emitState(const std::string &keyFields, const EmitProgram &vertex, const EmitProgram &fragment,
               std::vector<EmitRegister> &out, std::string &why)
{
    std::map<unsigned, Color> colors;
    unsigned depthFormat = 0;
    if (!parse(keyFields, colors, depthFormat, why))
        return false;
    // the fragment program's output map: 4 components per target, from bit 576 of its header (NAK sph.rs omap_target)
    const uint32_t outputs = fragment.header ? bits(fragment.header, 576, 32) : 0;
    out.clear();
    std::map<unsigned, uint32_t> writes;
    for (const auto &[j, c] : colors) {
        const unsigned written = outputs >> (4 * j) & 0xF;
        // m2v's write mask: Metal's (red in bit 3) where the program writes the target; an extended-range target's the
        // components its stored format has
        unsigned components = 0;
        if (written && c.writeMask)
            for (unsigned i = 0; i < 4; i++)
                components |= (extendedRange(c.format) ? written >> i & 1 : c.writeMask >> (3 - i) & 1) << i;
        writes[j] = ctWrite(components);
        out.push_back({kCtWrite + 4 * j, writes[j], true});
        const bool blends = c.blending && c.writeMask && !identityBlend(c) && !extendedRange(c.format);
        out.push_back({kBlend + 4 * j, blends, writes[j] && !(c.blending && identityBlend(c))});
        if (j)
            out.push_back({kBlendPerTarget + 0x20 * j, 1, true});
        const uint32_t values[6] = {kOps[c.factor[2]], kFactors[c.factor[0]], kFactors[c.factor[1]],
                                    kOps[c.factor[5]], kFactors[c.factor[3]], kFactors[c.factor[4]]};
        const uint32_t off[6] = {kOpAdd, kZero, kZero, kOpAdd, kZero, kZero};
        for (unsigned k = 0; k < 6; k++)
            out.push_back({kBlendPerTarget + 0x20 * j + 4 + 4 * k, blends ? values[k] : off[k], blends && writes[j]});
    }
    // NVK's write-mask macro: one control when every target's mask is the first's
    bool common = true;
    for (const auto &[j, w] : writes)
        common &= w == writes.begin()->second;
    out.push_back({kSingleCtWrite, common, true});
    const bool floatDepth = depthFormat == 252 || depthFormat == 260;  // Depth32Float, Depth32Float_Stencil8
    out.push_back({kViewportClip, kViewportClipBase | !floatDepth, true});
    out.push_back({kEarlyZ, fragment.earlyFragmentTests, true});
    out.push_back({kRegisterCount[0], vertex.gprs, true});
    out.push_back({kRegisterCount[1], fragment.gprs, true});
    return true;
}

}  // namespace raw
