// SPDX-License-Identifier: GPL-3.0-or-later
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include "common/div_ceil.h"
#include "video_core/renderer_metal/metal_kernels.h"

namespace NativeMetal {
namespace {
// Note: `vertex`, `fragment` and `kernel` are MSL keywords; avoid them as identifiers.
constexpr std::string_view KERNELS_MSL = R"(#include <metal_stdlib>
using namespace metal;

static uint load_index(device const uchar* src, uint byte_offset, uint bytes, uint i) {
    const uint at = byte_offset + i * bytes;
    uint value = src[at];
    if (bytes > 1)
        value |= uint(src[at + 1]) << 8;
    if (bytes > 2) {
        value |= uint(src[at + 2]) << 16;
        value |= uint(src[at + 3]) << 24;
    }
    return value;
}

struct IndexParams {
    uint src_offset;
    uint index_bytes;
    uint count;
    uint mode;
    uint out_count;
};

kernel void citrosis_convert_indices(device const uchar* src [[buffer(0)]],
                                     device uint* dst [[buffer(1)]],
                                     constant IndexParams& p [[buffer(2)]],
                                     uint gid [[thread_position_in_grid]]) {
    if (gid >= p.out_count)
        return;
    uint source_index = gid;
    switch (p.mode) {
    case 1: { // quads -> triangles
        const uint quad = gid / 6;
        const uint corner = gid % 6;
        const uint map[6] = {0, 1, 2, 0, 2, 3};
        source_index = quad * 4 + map[corner];
        break;
    }
    case 2: { // quad strip -> triangles
        const uint quad = gid / 6;
        const uint corner = gid % 6;
        const uint map[6] = {0, 1, 3, 0, 3, 2};
        source_index = quad * 2 + map[corner];
        break;
    }
    case 3: { // fan / polygon -> triangles
        const uint tri = gid / 3;
        const uint corner = gid % 3;
        source_index = corner == 0 ? 0 : tri + corner;
        break;
    }
    case 4: { // line loop -> lines
        const uint line = gid / 2;
        source_index = (gid % 2) == 0 ? line : (line + 1) % p.count;
        break;
    }
    default:
        break;
    }
    dst[gid] = load_index(src, p.src_offset, p.index_bytes, source_index);
}

struct RepackParams {
    uint src_offset;
    uint src_size;
    uint element;
    uint dst_stride;
    uint count;
};

kernel void citrosis_repack_vertices(device const uchar* src [[buffer(0)]],
                                     device uchar* dst [[buffer(1)]],
                                     constant RepackParams& p [[buffer(2)]],
                                     uint gid [[thread_position_in_grid]]) {
    const uint words = (p.element + 3) / 4;
    const uint item = gid / words;
    const uint byte_index = (gid % words) * 4;
    if (item >= p.count)
        return;
    uint value = 0;
    for (uint lane = 0; lane < 4; ++lane) {
        const uint relative = item * p.element + byte_index + lane;
        if (byte_index + lane < p.element && relative < p.src_size)
            value |= uint(src[p.src_offset + relative]) << (lane * 8);
    }
    // Destination stride and offset are aligned; initialize the final padding too.
    ((device uint*)(dst + item * p.dst_stride))[byte_index / 4] = value;
}

struct IndirectParams {
    uint args_offset;
    uint stride;
    uint words;
    uint count_offset;
    uint max_draws;
};

kernel void citrosis_compact_indirect(device const uchar* args [[buffer(0)]],
                                      device const uchar* count_data [[buffer(1)]],
                                      device uint* dst [[buffer(2)]],
                                      constant IndirectParams& p [[buffer(3)]],
                                      uint gid [[thread_position_in_grid]]) {
    const uint draw = gid / p.words;
    const uint word = gid % p.words;
    if (draw >= p.max_draws)
        return;
    const uint draw_count = load_index(count_data, p.count_offset, 4, 0);
    uint value = 0;
    if (draw < draw_count)
        value = load_index(args, p.args_offset + draw * p.stride, 4, word);
    dst[gid] = value;
}
)";

u32 Narrow(size_t value) {
    if (value > std::numeric_limits<u32>::max())
        throw std::out_of_range("Metal kernel parameter exceeds 32 bits");
    return static_cast<u32>(value);
}

void Dispatch1D(Commands& commands, u64 threads) {
    constexpr u32 local = 64;
    if (threads == 0)
        return;
    commands.Dispatch({Narrow(Common::DivCeil<u64>(threads, local)), 1, 1}, {local, 1, 1});
}
} // namespace

const Pipeline& Kernels::Get(Pipeline& pipeline, bool& attempted, const char* entry) {
    if (!attempted) {
        attempted = true;
        pipeline = runtime.CreateComputePipeline(
            runtime.CompileMSL(KERNELS_MSL, entry, Stage::Compute));
    }
    if (!pipeline.Valid())
        throw std::runtime_error(std::string("Metal utility kernel unavailable: ") + entry);
    return pipeline;
}

u32 Kernels::ConvertedCount(IndexMode mode, u32 count) {
    switch (mode) {
    case IndexMode::Widen:
        return count;
    case IndexMode::Quads:
        return count / 4 * 6;
    case IndexMode::QuadStrip:
        return count >= 4 ? (count - 2) / 2 * 6 : 0;
    case IndexMode::Fan:
        return count >= 3 ? (count - 2) * 3 : 0;
    case IndexMode::LineLoop:
        return count > 1 ? count * 2 : 0;
    }
    return 0;
}

void Kernels::ConvertIndices(Commands& commands, const Buffer& src, size_t src_offset,
                             u32 index_bytes, u32 count, IndexMode mode, const Buffer& dst,
                             size_t dst_offset) {
    const u32 out_count = ConvertedCount(mode, count);
    if (out_count == 0)
        return;
    struct {
        u32 src_offset, index_bytes, count, mode, out_count;
    } params{Narrow(src_offset), index_bytes, count, static_cast<u32>(mode), out_count};
    commands.BeginCompute(Get(index_pipeline, index_attempted, "citrosis_convert_indices"));
    commands.SetBuffer(Stage::Compute, 0, src, 0);
    commands.SetBuffer(Stage::Compute, 1, dst, dst_offset);
    commands.SetBytes(Stage::Compute, 2, std::as_bytes(std::span{&params, 1}));
    dst.MarkWritten();
    Dispatch1D(commands, out_count);
}

void Kernels::RepackVertices(Commands& commands, const Buffer& src, size_t src_offset,
                             size_t src_size, u32 element, u32 count, const Buffer& dst,
                             size_t dst_offset, u32 dst_stride) {
    if (element == 0 || count == 0)
        return;
    struct {
        u32 src_offset, src_size, element, dst_stride, count;
    } params{Narrow(src_offset), Narrow(src_size), element, dst_stride, count};
    commands.BeginCompute(Get(repack_pipeline, repack_attempted, "citrosis_repack_vertices"));
    commands.SetBuffer(Stage::Compute, 0, src, 0);
    commands.SetBuffer(Stage::Compute, 1, dst, dst_offset);
    commands.SetBytes(Stage::Compute, 2, std::as_bytes(std::span{&params, 1}));
    dst.MarkWritten();
    Dispatch1D(commands, static_cast<u64>(count) * Common::DivCeil(element, 4u));
}

void Kernels::CompactIndirect(Commands& commands, const Buffer& args, size_t args_offset,
                              u32 stride, u32 words, const Buffer& count_buffer,
                              size_t count_offset, u32 max_draws, const Buffer& dst,
                              size_t dst_offset) {
    if (max_draws == 0 || words == 0)
        return;
    struct {
        u32 args_offset, stride, words, count_offset, max_draws;
    } params{Narrow(args_offset), stride, words, Narrow(count_offset), max_draws};
    commands.BeginCompute(Get(indirect_pipeline, indirect_attempted, "citrosis_compact_indirect"));
    commands.SetBuffer(Stage::Compute, 0, args, 0);
    commands.SetBuffer(Stage::Compute, 1, count_buffer, 0);
    commands.SetBuffer(Stage::Compute, 2, dst, dst_offset);
    commands.SetBytes(Stage::Compute, 3, std::as_bytes(std::span{&params, 1}));
    dst.MarkWritten();
    Dispatch1D(commands, static_cast<u64>(max_draws) * words);
}

} // namespace NativeMetal
