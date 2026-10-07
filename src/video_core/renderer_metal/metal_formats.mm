// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_metal/metal_formats.h"

#import <Metal/Metal.h>

namespace NativeMetal {
namespace {
// Maxwell VertexAttribute::Type (UInt = 4 and UScaled = 5 need no special casing here)
constexpr uint32_t SNorm = 1, UNorm = 2, SInt = 3, SScaled = 6, Float = 7;
// Maxwell VertexAttribute::Size
constexpr uint32_t R32G32B32A32 = 0x01, R32G32B32 = 0x02, R16G16B16A16 = 0x03, R32G32 = 0x04,
                   R16G16B16 = 0x05, R8G8B8A8 = 0x0A, R16G16 = 0x0F, R32 = 0x12, R8G8B8 = 0x13,
                   R8G8 = 0x18, R16 = 0x1B, R8 = 0x1D, A2B10G10R10 = 0x30, B10G11R11 = 0x31,
                   G8R8 = 0x32, X8B8G8R8 = 0x33, A8 = 0x34;

VertexFormatInfo Make(MTLVertexFormat format, uint32_t size) {
    return {static_cast<uint32_t>(format), size};
}
} // namespace

VertexFormatInfo MaxwellVertexFormat(uint32_t type, uint32_t size) {
    const bool is_signed = type == SNorm || type == SInt || type == SScaled;
    const bool normalized = type == SNorm || type == UNorm;
    if (type == Float) {
        switch (size) {
        case R16:
            return Make(MTLVertexFormatHalf, 2);
        case R16G16:
            return Make(MTLVertexFormatHalf2, 4);
        case R16G16B16:
            return Make(MTLVertexFormatHalf3, 6);
        case R16G16B16A16:
            return Make(MTLVertexFormatHalf4, 8);
        case R32:
            return Make(MTLVertexFormatFloat, 4);
        case R32G32:
            return Make(MTLVertexFormatFloat2, 8);
        case R32G32B32:
            return Make(MTLVertexFormatFloat3, 12);
        case R32G32B32A32:
            return Make(MTLVertexFormatFloat4, 16);
        case B10G11R11:
            return Make(MTLVertexFormatFloatRG11B10, 4);
        default:
            return {};
        }
    }
    if (type < SNorm || type > SScaled)
        return {};
    switch (size) {
    case R8:
    case A8:
        if (normalized)
            return Make(is_signed ? MTLVertexFormatCharNormalized : MTLVertexFormatUCharNormalized,
                        1);
        return Make(is_signed ? MTLVertexFormatChar : MTLVertexFormatUChar, 1);
    case R8G8:
    case G8R8:
        if (normalized)
            return Make(is_signed ? MTLVertexFormatChar2Normalized
                                  : MTLVertexFormatUChar2Normalized,
                        2);
        return Make(is_signed ? MTLVertexFormatChar2 : MTLVertexFormatUChar2, 2);
    case R8G8B8:
        if (normalized)
            return Make(is_signed ? MTLVertexFormatChar3Normalized
                                  : MTLVertexFormatUChar3Normalized,
                        3);
        return Make(is_signed ? MTLVertexFormatChar3 : MTLVertexFormatUChar3, 3);
    case R8G8B8A8:
    case X8B8G8R8:
        if (normalized)
            return Make(is_signed ? MTLVertexFormatChar4Normalized
                                  : MTLVertexFormatUChar4Normalized,
                        4);
        return Make(is_signed ? MTLVertexFormatChar4 : MTLVertexFormatUChar4, 4);
    case R16:
        if (normalized)
            return Make(is_signed ? MTLVertexFormatShortNormalized
                                  : MTLVertexFormatUShortNormalized,
                        2);
        return Make(is_signed ? MTLVertexFormatShort : MTLVertexFormatUShort, 2);
    case R16G16:
        if (normalized)
            return Make(is_signed ? MTLVertexFormatShort2Normalized
                                  : MTLVertexFormatUShort2Normalized,
                        4);
        return Make(is_signed ? MTLVertexFormatShort2 : MTLVertexFormatUShort2, 4);
    case R16G16B16:
        if (normalized)
            return Make(is_signed ? MTLVertexFormatShort3Normalized
                                  : MTLVertexFormatUShort3Normalized,
                        6);
        return Make(is_signed ? MTLVertexFormatShort3 : MTLVertexFormatUShort3, 6);
    case R16G16B16A16:
        if (normalized)
            return Make(is_signed ? MTLVertexFormatShort4Normalized
                                  : MTLVertexFormatUShort4Normalized,
                        8);
        return Make(is_signed ? MTLVertexFormatShort4 : MTLVertexFormatUShort4, 8);
    case R32:
        if (normalized)
            return {};
        return Make(is_signed ? MTLVertexFormatInt : MTLVertexFormatUInt, 4);
    case R32G32:
        if (normalized)
            return {};
        return Make(is_signed ? MTLVertexFormatInt2 : MTLVertexFormatUInt2, 8);
    case R32G32B32:
        if (normalized)
            return {};
        return Make(is_signed ? MTLVertexFormatInt3 : MTLVertexFormatUInt3, 12);
    case R32G32B32A32:
        if (normalized)
            return {};
        return Make(is_signed ? MTLVertexFormatInt4 : MTLVertexFormatUInt4, 16);
    case A2B10G10R10:
        if (type == UNorm)
            return Make(MTLVertexFormatUInt1010102Normalized, 4);
        if (type == SNorm)
            return Make(MTLVertexFormatInt1010102Normalized, 4);
        return {}; // integer/scaled 10-10-10-2 has no Metal vertex format
    default:
        return {};
    }
}
} // namespace NativeMetal
