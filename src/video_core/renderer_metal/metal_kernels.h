// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "common/common_types.h"
#include "video_core/renderer_metal/metal_runtime.h"

namespace NativeMetal {

/// Small compute kernels that keep guest data on the GPU where Metal lacks a feature
/// (8-bit indices, quads/fans/loops, unaligned vertex strides, draw-count indirect), so
/// the GPU thread never waits for the GPU to read guest data back. Each call records a
/// dispatch, which ends any open render pass: call these before the draw's pass begins.
class Kernels {
public:
    explicit Kernels(Runtime& runtime_) : runtime{runtime_} {}

    enum class IndexMode : u32 { Widen = 0, Quads = 1, QuadStrip = 2, Fan = 3, LineLoop = 4 };

    /// Output index count for `count` guest indices in `mode`.
    static u32 ConvertedCount(IndexMode mode, u32 count);

    /// Reads `count` guest indices of `index_bytes` (1, 2 or 4) bytes at `src_offset` and
    /// writes ConvertedCount(mode, count) 32-bit indices at `dst_offset`.
    void ConvertIndices(Commands& commands, const Buffer& src, size_t src_offset,
                        u32 index_bytes, u32 count, IndexMode mode, const Buffer& dst,
                        size_t dst_offset);

    /// Copies `count` elements of `element` bytes (source stride `element`) into
    /// `dst_stride`-byte slots; bytes past `src_size` read as zero.
    void RepackVertices(Commands& commands, const Buffer& src, size_t src_offset,
                        size_t src_size, u32 element, u32 count, const Buffer& dst,
                        size_t dst_offset, u32 dst_stride);

    /// Copies `max_draws` indirect records of `words` u32 (guest stride `stride` bytes)
    /// into tightly packed records, zeroing those at or past the GPU-side draw count.
    void CompactIndirect(Commands& commands, const Buffer& args, size_t args_offset, u32 stride,
                         u32 words, const Buffer& count_buffer, size_t count_offset,
                         u32 max_draws, const Buffer& dst, size_t dst_offset);

private:
    const Pipeline& Get(Pipeline& pipeline, bool& attempted, const char* entry);

    Runtime& runtime;
    Pipeline index_pipeline, repack_pipeline, indirect_pipeline;
    bool index_attempted{}, repack_attempted{}, indirect_attempted{};
};

} // namespace NativeMetal
