// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cstring>
#include <limits>
#include <numeric>
#include <stdexcept>
#include "video_core/renderer_metal/metal_buffer_cache.h"

namespace NativeMetal {
namespace {
void Range(const Buffer& buffer, u64 offset, u64 size) {
    if (offset > buffer.Size() || size > buffer.Size() - offset)
        throw std::out_of_range("Metal guest buffer range exceeds allocation");
}
} // namespace
CacheBuffer::CacheBuffer(BufferCacheRuntime& runtime, VideoCommon::NullBufferParams params)
    : BufferBase{params}, handle{runtime.runtime.CreateBuffer(65536)}, usage{65536} {}
CacheBuffer::CacheBuffer(BufferCacheRuntime& runtime, VAddr address, u64 size)
    : BufferBase{address, size}, handle{runtime.runtime.CreateBuffer(size)}, usage{size} {}
void BufferCacheRuntime::TickFrame(Common::SlotVector<CacheBuffer>& buffers) {
    for (auto entry : buffers)
        entry.second->ResetUsageTracking();
}
StagingBuffer BufferCacheRuntime::UploadStagingBuffer(size_t size) {
    auto stream = scheduler.Stream(size);
    return {stream.buffer, stream.offset,
            {reinterpret_cast<u8*>(stream.mapped.data()), size}};
}
StagingBuffer BufferCacheRuntime::DownloadStagingBuffer(size_t size, bool) {
    // Downloads are read after the GPU finishes (possibly much later when deferred), so
    // they get their own allocation rather than recycled stream memory.
    auto buffer = runtime.CreateBuffer(std::max<size_t>(size, 4));
    const auto mapping = buffer.MappedBytes();
    return {buffer, 0, {reinterpret_cast<u8*>(mapping.data()), size}};
}
void BufferCacheRuntime::CopyBuffer(const Buffer& dst, const Buffer& src,
                                    std::span<const VideoCommon::BufferCopy> copies, bool, bool) {
    for (const auto& copy : copies) {
        Range(src, copy.src_offset, copy.size);
        Range(dst, copy.dst_offset, copy.size);
        if (copy.size == 0)
            continue;
        if (dst == src && copy.src_offset < copy.dst_offset + copy.size &&
            copy.dst_offset < copy.src_offset + copy.size) {
            const auto temporary = scheduler.Stream(copy.size);
            scheduler.Record().CopyBuffer(src, copy.src_offset, temporary.buffer, temporary.offset,
                                          copy.size);
            scheduler.Record().CopyBuffer(temporary.buffer, temporary.offset, dst,
                                          copy.dst_offset, copy.size);
        } else
            scheduler.Record().CopyBuffer(src, copy.src_offset, dst, copy.dst_offset, copy.size);
    }
}
void BufferCacheRuntime::ClearBuffer(const Buffer& dst, u32 offset, size_t size, u32 value) {
    Range(dst, offset, size);
    if ((offset | size) & 3)
        throw std::invalid_argument("Metal guest fill must be 32-bit aligned");
    if (!size)
        return;
    const size_t chunk = std::min<size_t>(size, 1024 * 1024);
    auto staging = UploadStagingBuffer(chunk);
    for (size_t pos = 0; pos < chunk; pos += 4)
        std::memcpy(staging.mapped_span.data() + pos, &value, 4);
    for (size_t pos = 0; pos < size; pos += chunk)
        scheduler.Record().CopyBuffer(staging.buffer, staging.offset, dst, offset + pos,
                                      std::min(chunk, size - pos));
}
void BufferCacheRuntime::Bind(std::vector<BufferBinding>& bindings, u32 index, const Buffer& buffer,
                              u32 offset, u32 size, bool written) {
    Range(buffer, offset, size);
    if (index >= 64)
        throw std::out_of_range("Metal guest buffer binding exceeds stage limit");
    if (bindings.size() <= index)
        bindings.resize(index + 1);
    bindings[index] = {buffer, offset, size, written};
}
std::span<u8> BufferCacheRuntime::BindMappedUniformBuffer(size_t stage, u32 binding, u32 size) {
    auto staging = UploadStagingBuffer(size);
    BindUniformBuffer(stage, binding, staging.buffer, static_cast<u32>(staging.offset), size);
    return staging.mapped_span;
}
void BufferCacheRuntime::BindUniformBuffer(size_t stage, u32 binding, const Buffer& buffer,
                                           u32 offset, u32 size) {
    Bind(graphics.at(stage).uniforms, binding, buffer, offset, size, false);
}
void BufferCacheRuntime::BindStorageBuffer(size_t stage, u32 binding, const Buffer& buffer,
                                           u32 offset, u32 size, bool written) {
    Bind(graphics.at(stage).storage, binding, buffer, offset, size, written);
}
void BufferCacheRuntime::BindComputeUniformBuffer(u32 binding, const Buffer& buffer, u32 offset,
                                                  u32 size) {
    Bind(compute.uniforms, binding, buffer, offset, size, false);
}
void BufferCacheRuntime::BindComputeStorageBuffer(u32 binding, const Buffer& buffer, u32 offset,
                                                  u32 size, bool written) {
    Bind(compute.storage, binding, buffer, offset, size, written);
}
void BufferCacheRuntime::BindVertexBuffers(VideoCommon::HostBindings<CacheBuffer>& bindings) {
    if (bindings.max_index > vertices.size() || bindings.min_index > bindings.max_index ||
        bindings.buffers.size() != bindings.max_index - bindings.min_index ||
        bindings.offsets.size() != bindings.buffers.size() ||
        bindings.sizes.size() != bindings.buffers.size() ||
        bindings.strides.size() != bindings.buffers.size())
        throw std::invalid_argument("Invalid Metal guest vertex binding range");
    for (size_t i = 0; i < bindings.buffers.size(); ++i) {
        if (!bindings.buffers[i])
            throw std::invalid_argument("Missing Metal guest vertex buffer");
        const auto& buffer = bindings.buffers[i]->Handle();
        Range(buffer, bindings.offsets[i], bindings.sizes[i]);
        if (bindings.strides[i] > 2048)
            throw std::invalid_argument("Metal guest vertex stride exceeds 2048");
        vertices[bindings.min_index + i] = {{buffer, bindings.offsets[i], bindings.sizes[i], false},
                                            static_cast<u32>(bindings.strides[i])};
    }
}
void BufferCacheRuntime::BindTransformFeedbackBuffers(
    VideoCommon::HostBindings<CacheBuffer>& bindings) {
    if (!bindings.buffers.empty())
        throw std::runtime_error("Metal transform feedback is not implemented");
}
void BufferCacheRuntime::BindTextureBuffer(const CacheBuffer& buffer, u32 offset, u32 size,
                                           VideoCore::Surface::PixelFormat format) {
    Range(buffer.Handle(), offset, size);
    texels.push_back({{buffer.Handle(), offset, size, false}, format, false});
}
void BufferCacheRuntime::BindImageBuffer(const CacheBuffer& buffer, u32 offset, u32 size,
                                         VideoCore::Surface::PixelFormat format) {
    Range(buffer.Handle(), offset, size);
    texels.push_back({{buffer.Handle(), offset, size, true}, format, true});
}
void BufferCacheRuntime::ResetDescriptorBindings() {
    // Keep vector capacity: this runs for every draw.
    for (auto& stage : graphics) {
        stage.uniforms.clear();
        stage.storage.clear();
    }
    compute.uniforms.clear();
    compute.storage.clear();
    texels.clear();
}
void BufferCacheRuntime::BindQuadIndexBuffer(Maxwell::PrimitiveTopology topology, u32 first,
                                             u32 count) {
    if (count && first > std::numeric_limits<u32>::max() - (count - 1))
        throw std::overflow_error("Metal generated vertex index overflows");
    Kernels::IndexMode mode;
    Primitive primitive = Primitive::Triangles;
    switch (topology) {
    case Maxwell::PrimitiveTopology::Quads:
        mode = Kernels::IndexMode::Quads;
        break;
    case Maxwell::PrimitiveTopology::QuadStrip:
        mode = Kernels::IndexMode::QuadStrip;
        break;
    case Maxwell::PrimitiveTopology::TriangleFan:
    case Maxwell::PrimitiveTopology::Polygon:
        mode = Kernels::IndexMode::Fan;
        break;
    case Maxwell::PrimitiveTopology::LineLoop:
        mode = Kernels::IndexMode::LineLoop;
        primitive = Primitive::Lines;
        break;
    default:
        throw std::runtime_error("Metal guest primitive conversion is not implemented");
    }
    // Generated straight into stream memory: no GPU work, no temporary vectors.
    const u32 out_count = Kernels::ConvertedCount(mode, count);
    const auto stream = scheduler.Stream(std::max<size_t>(static_cast<size_t>(out_count) * 4, 4));
    u32* const out = reinterpret_cast<u32*>(stream.mapped.data());
    for (u32 i = 0; i < out_count; ++i) {
        u32 source = i;
        switch (mode) {
        case Kernels::IndexMode::Quads: {
            static constexpr u32 map[6] = {0, 1, 2, 0, 2, 3};
            source = i / 6 * 4 + map[i % 6];
            break;
        }
        case Kernels::IndexMode::QuadStrip: {
            static constexpr u32 map[6] = {0, 1, 3, 0, 3, 2};
            source = i / 6 * 2 + map[i % 6];
            break;
        }
        case Kernels::IndexMode::Fan:
            source = i % 3 == 0 ? 0 : i / 3 + i % 3;
            break;
        case Kernels::IndexMode::LineLoop:
            source = i % 2 == 0 ? i / 2 : (i / 2 + 1) % count;
            break;
        case Kernels::IndexMode::Widen:
            break;
        }
        out[i] = first + source;
    }
    indices = {{stream.buffer, stream.offset, static_cast<u64>(out_count) * 4, false},
               primitive,
               true,
               out_count};
}
void BufferCacheRuntime::BindIndexBuffer(Maxwell::PrimitiveTopology topology,
                                         Maxwell::IndexFormat format, u32 first, u32 count,
                                         const Buffer& buffer, u32 offset, u32 size) {
    const size_t bytes = format == Maxwell::IndexFormat::UnsignedByte    ? 1
                         : format == Maxwell::IndexFormat::UnsignedShort ? 2
                         : format == Maxwell::IndexFormat::UnsignedInt   ? 4
                                                                         : 0;
    if (!bytes)
        throw std::invalid_argument("Invalid Metal guest index format");
    if (static_cast<u64>(first) * bytes > size ||
        static_cast<u64>(count) * bytes > size - static_cast<u64>(first) * bytes)
        throw std::out_of_range("Metal guest indices exceed bound range");
    const u64 start = static_cast<u64>(offset) + static_cast<u64>(first) * bytes;
    Range(buffer, start, static_cast<u64>(count) * bytes);
    Primitive primitive;
    switch (topology) {
    case Maxwell::PrimitiveTopology::Points:
        primitive = Primitive::Points;
        break;
    case Maxwell::PrimitiveTopology::Lines:
        primitive = Primitive::Lines;
        break;
    case Maxwell::PrimitiveTopology::LineStrip:
        primitive = Primitive::LineStrip;
        break;
    case Maxwell::PrimitiveTopology::Triangles:
        primitive = Primitive::Triangles;
        break;
    case Maxwell::PrimitiveTopology::TriangleStrip:
        primitive = Primitive::TriangleStrip;
        break;
    default:
        primitive = Primitive::Triangles;
        break;
    }
    const bool convert = topology != Maxwell::PrimitiveTopology::Points &&
                         topology != Maxwell::PrimitiveTopology::Lines &&
                         topology != Maxwell::PrimitiveTopology::LineStrip &&
                         topology != Maxwell::PrimitiveTopology::Triangles &&
                         topology != Maxwell::PrimitiveTopology::TriangleStrip;
    // Metal index buffer offsets must be 4-byte aligned; others go through the widen kernel.
    if (!convert && bytes != 1 && start % 4 == 0) {
        indices = {
            {buffer, start, static_cast<u64>(count) * bytes, false}, primitive, bytes == 4, count};
        return;
    }
    // Converted on the GPU: the indices may have been written by earlier GPU work, and
    // reading them back would stall the GPU thread on every such draw.
    Kernels::IndexMode mode = Kernels::IndexMode::Widen;
    Primitive converted = primitive;
    switch (topology) {
    case Maxwell::PrimitiveTopology::Quads:
        mode = Kernels::IndexMode::Quads;
        converted = Primitive::Triangles;
        break;
    case Maxwell::PrimitiveTopology::QuadStrip:
        mode = Kernels::IndexMode::QuadStrip;
        converted = Primitive::Triangles;
        break;
    case Maxwell::PrimitiveTopology::TriangleFan:
    case Maxwell::PrimitiveTopology::Polygon:
        mode = Kernels::IndexMode::Fan;
        converted = Primitive::Triangles;
        break;
    case Maxwell::PrimitiveTopology::LineLoop:
        mode = Kernels::IndexMode::LineLoop;
        converted = Primitive::Lines;
        break;
    default:
        if (convert)
            throw std::runtime_error("Metal guest primitive conversion is not implemented");
        break;
    }
    const u32 out_count = Kernels::ConvertedCount(mode, count);
    if (out_count == 0) {
        indices = {{}, converted, true, 0};
        return;
    }
    const size_t output_size = static_cast<size_t>(out_count) * 4;
    const IndexKey key{buffer.AllocationId(), start, count, static_cast<u32>(bytes), mode};
    const u64 version = buffer.ContentVersion();
    const u64 stamp = ++index_clock;
    auto found = converted_indices.find(key);
    if (found != converted_indices.end() && found->second.version == version) {
        found->second.last_use = stamp;
        ++index_stats.hits;
        indices = {{found->second.converted, 0, output_size, false}, converted, true, out_count};
        return;
    }
    ++index_stats.misses;
    index_stats.bytes += output_size;
    Buffer output;
    size_t output_offset = 0;
    if (output_size <= IndexBudget) {
        if (found == converted_indices.end()) {
            while (!converted_indices.empty() &&
                   (index_bytes + output_size > IndexBudget ||
                    converted_indices.size() >= IndexEntries)) {
                const auto oldest = std::min_element(converted_indices.begin(), converted_indices.end(),
                    [](const auto& a, const auto& b) {
                        return a.second.last_use < b.second.last_use;
                    });
                index_bytes -= oldest->second.converted.Size();
                converted_indices.erase(oldest);
            }
            found = converted_indices.emplace(key, IndexEntry{runtime.CreateBuffer(output_size)}).first;
            index_bytes += output_size;
        }
        output = found->second.converted;
        found->second.version = version;
        found->second.last_use = stamp;
    } else {
        const auto stream = scheduler.Stream(output_size);
        output = stream.buffer;
        output_offset = stream.offset;
    }
    try {
        scheduler.Utilities().ConvertIndices(scheduler.Record(), buffer, start,
                                             static_cast<u32>(bytes), count, mode, output,
                                             output_offset);
    } catch (...) {
        if (found != converted_indices.end()) {
            index_bytes -= found->second.converted.Size();
            converted_indices.erase(found);
        }
        throw;
    }
    indices = {{output, output_offset, output_size, false}, converted, true, out_count};
}
} // namespace NativeMetal
