// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace NativeMetal {
struct VertexFormatInfo {
    uint32_t format{}; // MTLVertexFormat value, 0 (Invalid) when Metal has no equivalent
    uint32_t size{};   // bytes read per vertex
};
/// Maxwell vertex attribute (raw VertexAttribute::Type and ::Size values) to Metal. Scaled
/// types map to integer formats: the shader converts them (Shader::AttributeType::*Scaled).
VertexFormatInfo MaxwellVertexFormat(uint32_t type, uint32_t size);
} // namespace NativeMetal
