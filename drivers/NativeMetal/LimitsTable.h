// SPDX-License-Identifier: MIT
// The base class's limits table (-[_MTLDevice limits], the words after isa in 21H461). Metal's accessors read their field from
// it (-[_MTLDevice maxTextureWidth2D] is limits+0x84), and frameworks read it directly, past the device's methods:
// PhotosUICore's texture converter takes its largest image from 0x84/0x88 (-[PXGMetalTextureConverter
// createTextureFromCGImage:transform:alpha:options:], PXGCreateMetalTextureFromBytesInCGImage); with them 0 every image is
// "too large", is redrawn to fit 0x0 and PXGCreateContext asserts. CoreRE reads minBufferNoCopyAlignmentBytes at 0x7c. The
// offsets are those accessors' loads (the 61 integer fields; maxLineWidth and maxPointSize are floats, not filled).
#pragma once
#include <cstdint>
#include <cstring>

struct LimitField {
    const char *selector;
    uint16_t offset;
};

inline constexpr LimitField kLimitFields[] = {{"maxFramebufferStorageBits", 0x0},
                                              {"linearTextureArrayAlignmentBytes", 0x4},
                                              {"linearTextureArrayAlignmentSlice", 0x8},
                                              {"maxTileBuffers", 0xc},
                                              {"maxTileTextures", 0x10},
                                              {"maxTileSamplers", 0x14},
                                              {"maxTileInlineDataSize", 0x18},
                                              {"minTilePixels", 0x1c},
                                              {"maxColorAttachments", 0x20},
                                              {"maxVertexAttributes", 0x24},
                                              {"maxVertexBuffers", 0x28},
                                              {"maxVertexTextures", 0x2c},
                                              {"maxVertexSamplers", 0x30},
                                              {"maxVertexInlineDataSize", 0x34},
                                              {"maxInterpolants", 0x38},
                                              {"maxFragmentBuffers", 0x3c},
                                              {"maxFragmentTextures", 0x40},
                                              {"maxFragmentSamplers", 0x44},
                                              {"maxFragmentInlineDataSize", 0x48},
                                              {"maxComputeBuffers", 0x4c},
                                              {"maxComputeTextures", 0x50},
                                              {"maxComputeSamplers", 0x54},
                                              {"maxComputeInlineDataSize", 0x58},
                                              {"maxComputeLocalMemorySizes", 0x5c},
                                              {"maxTotalComputeThreadsPerThreadgroup", 0x60},
                                              {"maxComputeThreadgroupMemory", 0x64},
                                              {"maxVisibilityQueryOffset", 0x70},
                                              {"minConstantBufferAlignmentBytes", 0x78},
                                              {"minBufferNoCopyAlignmentBytes", 0x7c},
                                              {"maxTextureWidth1D", 0x80},
                                              {"maxTextureWidth2D", 0x84},
                                              {"maxTextureHeight2D", 0x88},
                                              {"maxTextureWidth3D", 0x8c},
                                              {"maxTextureHeight3D", 0x90},
                                              {"maxTextureDepth3D", 0x94},
                                              {"maxTextureDimensionCube", 0x98},
                                              {"maxTextureLayers", 0x9c},
                                              {"linearTextureAlignmentBytes", 0xa0},
                                              {"iosurfaceTextureAlignmentBytes", 0xa4},
                                              {"iosurfaceReadOnlyTextureAlignmentBytes", 0xa8},
                                              {"deviceLinearTextureAlignmentBytes", 0xac},
                                              {"deviceLinearReadOnlyTextureAlignmentBytes", 0xb0},
                                              {"maxFunctionConstantIndices", 0xb4},
                                              {"maxComputeThreadgroupMemoryAlignmentBytes", 0xb8},
                                              {"maxInterpolatedComponents", 0xbc},
                                              {"maxTessellationFactor", 0xc0},
                                              {"maxIndirectBuffers", 0xc4},
                                              {"maxIndirectTextures", 0xc8},
                                              {"maxIndirectSamplers", 0xcc},
                                              {"maxIndirectSamplersPerDevice", 0xd0},
                                              {"maxFenceInstances", 0xd4},
                                              {"maxViewportCount", 0xd8},
                                              {"maxCustomSamplePositions", 0xdc},
                                              {"maxVertexAmplificationFactor", 0xe0},
                                              {"maxVertexAmplificationCount", 0xe4},
                                              {"maxTextureBufferWidth", 0xe8},
                                              {"maxComputeAttributes", 0xec},
                                              {"maxIOCommandsInFlight", 0xf0},
                                              {"maxPredicatedNestingDepth", 0xf4},
                                              {"maxAccelerationStructureLevels", 0xf8},
                                              {"maxConstantBufferArguments", 0xfc}};

enum class LimitFill : uint8_t {
    Filled,
    Overflow,  // the answer does not fit the table's 32 bits
    NotRead,   // the base class's accessor does not read the field there: it is left as it was
};

// Writes each field the device answers into `table`: answer(field, value) is false where it has no answer of its own (the field
// stays the base class's). Every write is read back through readBack(field), the base class's own accessor; a field it does not
// read there (21H461 answers minConstantBufferAlignmentBytes from elsewhere) is restored, and the fill goes on with the next
// field, each checked alike. report(field, how) hears every field answered.
template <class Answer, class ReadBack, class Report>
void fillLimitsTable(uint8_t *table, Answer answer, ReadBack readBack, Report report)
{
    for (const LimitField &field : kLimitFields) {
        uint64_t value = 0;
        if (!answer(field, value))
            continue;
        if (value > UINT32_MAX) {
            report(field, LimitFill::Overflow);
            continue;
        }
        uint32_t was;
        memcpy(&was, table + field.offset, 4);
        const uint32_t word = uint32_t(value);
        memcpy(table + field.offset, &word, 4);
        if (readBack(field) != value) {
            memcpy(table + field.offset, &was, 4);
            report(field, LimitFill::NotRead);
            continue;
        }
        report(field, LimitFill::Filled);
    }
}

#ifdef __OBJC__
#include <objc/runtime.h>

// fillLimitsTable with the answers of `device`'s own class: each field's method where it is not the base class's and answers an
// integer, read back through the base class's.
template <class Report> void fillLimitsFromMethods(id device, uint8_t *table, Report report)
{
    Class cls = object_getClass(device), base = class_getSuperclass(cls);
    auto answer = [&](const LimitField &field, uint64_t &value) {
        SEL sel = sel_registerName(field.selector);
        Method own = class_getInstanceMethod(cls, sel), original = class_getInstanceMethod(base, sel);
        if (!own || !original || method_getImplementation(own) == method_getImplementation(original))
            return false;
        char type[8] = {};
        method_getReturnType(own, type, sizeof type);
        if (strcmp(type, "Q") && strcmp(type, "L") && strcmp(type, "I"))
            return false;
        value = ((NSUInteger (*)(id, SEL))method_getImplementation(own))(device, sel);
        return true;
    };
    auto readBack = [&](const LimitField &field) -> uint64_t {
        SEL sel = sel_registerName(field.selector);
        return ((NSUInteger (*)(id, SEL))method_getImplementation(class_getInstanceMethod(base, sel)))(device, sel);
    };
    fillLimitsTable(table, answer, readBack, report);
}
#endif
