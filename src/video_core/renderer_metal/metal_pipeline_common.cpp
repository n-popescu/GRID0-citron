// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include "common/alignment.h"
#include "common/bit_cast.h"
#include "common/div_ceil.h"
#include "common/logging.h"
#include "video_core/renderer_metal/metal_pipeline_common.h"

namespace NativeMetal {

ArgumentBuffer& ArgumentPool::Acquire() {
    if (!buffers.empty()) {
        next %= buffers.size();
        if (!buffers[next]->Busy()) {
            ArgumentBuffer& result = *buffers[next];
            next = (next + 1) % buffers.size();
            return result;
        }
    }
    // The oldest clone is still in use, so all are: insert a new clone as the newest
    // entry, keeping the ring in acquisition order.
    const size_t at = buffers.empty() ? 0 : next;
    buffers.insert(buffers.begin() + static_cast<std::ptrdiff_t>(at),
                   std::make_unique<ArgumentBuffer>(metal->CloneArguments(prototype)));
    next = (at + 1) % buffers.size();
    return *buffers[at];
}

StageProgram::StageProgram(Runtime& metal, Shader::Info info_, ShaderSource source_)
    : info{std::move(info_)}, source{std::move(source_)} {
    function = metal.CompileMSL(source.msl, source.entry, source.stage);
    for (u32 set = 0; set < 2; ++set) {
        if (source.argument_sets[set])
            arguments[set] = ArgumentPool{metal, metal.CreateArguments(function, source, set)};
    }
    // Mirror the SPIR-V emitter's resource-set numbering (split descriptor sets, unified
    // binding): storage buffers take `count` bindings each; texture buffers, image buffers,
    // textures and images take one binding per descriptor with arrays inside.
    using Type = ResourceSlot::Type;
    for (const auto& desc : info.storage_buffers_descriptors) {
        for (u32 i = 0; i < desc.count; ++i) {
            ResourceSlot slot;
            slot.type = Type::StorageBuffer;
            slot.first = static_cast<u32>(resource_slots.size());
            slot.is_written = desc.is_written;
            resource_slots.push_back(slot);
        }
    }
    u32 view = 0;
    for (const auto& desc : info.texture_buffer_descriptors) {
        ResourceSlot slot;
        slot.type = Type::TextureBuffer;
        slot.first = view;
        slot.texture_type = Shader::TextureType::Buffer;
        resource_slots.push_back(slot);
        view += desc.count;
    }
    for (const auto& desc : info.image_buffer_descriptors) {
        ResourceSlot slot;
        slot.type = Type::ImageBuffer;
        slot.first = view;
        slot.texture_type = Shader::TextureType::Buffer;
        slot.image_format = desc.format;
        slot.is_integer = desc.is_integer;
        slot.is_written = desc.is_written;
        resource_slots.push_back(slot);
        view += desc.count;
    }
    u32 sampler = 0;
    for (const auto& desc : info.texture_descriptors) {
        ResourceSlot slot;
        slot.type = Type::Texture;
        slot.first = view;
        slot.sampler_first = sampler;
        slot.texture_type = desc.type;
        resource_slots.push_back(slot);
        view += desc.count;
        sampler += desc.count;
    }
    for (const auto& desc : info.image_descriptors) {
        ResourceSlot slot;
        slot.type = Type::Image;
        slot.first = view;
        slot.texture_type = desc.type;
        slot.image_format = desc.format;
        slot.is_integer = desc.is_integer;
        slot.is_written = desc.is_written;
        resource_slots.push_back(slot);
        view += desc.count;
    }
}

const Buffer& ResourceBinder::NullBuffer() {
    if (!null_buffer_created) {
        constexpr size_t size = 64 * 1024;
        null_buffer = metal.CreateBuffer(size);
        const std::vector<std::byte> zeros(size);
        null_buffer.Write(0, zeros);
        null_buffer_created = true;
    }
    return null_buffer;
}

VertexBinding ResourceBinder::RepackVertices(Scheduler& scheduler, const VertexBinding& binding,
                                             u32 src_stride, u32 dst_stride) {
    if (binding.offset > std::numeric_limits<u32>::max() ||
        binding.size > std::numeric_limits<u32>::max())
        throw std::out_of_range("Metal vertex repack range exceeds 32 bits");
    // Stride 0 (every vertex reads one element): copy that element once.
    const u32 element = src_stride != 0 ? src_stride
                                        : static_cast<u32>(std::min<u64>(binding.size, 256));
    const u32 count = src_stride != 0
                          ? static_cast<u32>(Common::DivCeil<u64>(binding.size, src_stride))
                          : 1;
    const u32 out_stride = src_stride != 0 ? dst_stride : Common::AlignUp(element, 4u);
    VertexBinding result;
    result.stride = src_stride != 0 ? dst_stride : 0;
    if (count == 0 || element == 0)
        return result;
    const size_t out_size = static_cast<size_t>(count) * out_stride;
    const RepackKey key{binding.buffer.AllocationId(), binding.offset, binding.size,
                        src_stride, dst_stride};
    const u64 version = binding.buffer.ContentVersion();
    const u64 stamp = ++repack_clock;
    auto found = repacked_vertices.find(key);
    if (found != repacked_vertices.end() && found->second.version == version) {
        found->second.last_use = stamp;
        ++stats.repack_hits;
        return found->second.converted;
    }
    ++stats.repack_misses;
    stats.repack_bytes += out_size;
    if (out_size <= RepackBudget) {
        if (found == repacked_vertices.end()) {
            while (!repacked_vertices.empty() &&
                   (repack_bytes + out_size > RepackBudget ||
                    repacked_vertices.size() >= RepackEntries)) {
                const auto oldest = std::min_element(repacked_vertices.begin(), repacked_vertices.end(),
                    [](const auto& a, const auto& b) {
                        return a.second.last_use < b.second.last_use;
                    });
                repack_bytes -= oldest->second.converted.buffer.Size();
                repacked_vertices.erase(oldest);
            }
            result.buffer = metal.CreateBuffer(out_size);
            result.offset = 0;
            result.size = out_size;
            found = repacked_vertices.emplace(key, RepackEntry{result}).first;
            repack_bytes += out_size;
        }
        result = found->second.converted;
        found->second.version = version;
        found->second.last_use = stamp;
    } else {
        // Oversized one-off conversions do not evict all useful scene geometry.
        const auto stream = scheduler.Stream(out_size);
        result.buffer = stream.buffer;
        result.offset = stream.offset;
        result.size = out_size;
    }
    // Dedicated cache buffers are GPU-only. Replacing stale contents is ordered after
    // prior draws on the same queue, so it needs neither CPU access nor a GPU wait.
    try {
        scheduler.Utilities().RepackVertices(scheduler.Record(), binding.buffer, binding.offset,
                                             binding.size, element, count, result.buffer,
                                             result.offset, out_stride);
    } catch (...) {
        if (found != repacked_vertices.end()) {
            repack_bytes -= found->second.converted.buffer.Size();
            repacked_vertices.erase(found);
        }
        throw;
    }
    return result;
}

Texture ResourceBinder::TexelTexture(const TexelBufferBinding& binding, bool storage,
                                     bool integer) {
    const auto null_texture = [&] {
        return texture_runtime.NullStorageTexture(TextureDimension::Buffer, integer);
    };
    if (binding.size == 0 || !binding.buffer.Valid())
        return null_texture();
    PixelFormat format;
    try {
        format = GuestPixelFormat(binding.format);
    } catch (const std::exception& error) {
        LOG_WARNING(Render, "Metal texel buffer: {}", error.what());
        return null_texture();
    }
    const u32 bytes = BytesPerBlock(format);
    if (BlockWidth(format) != 1 || bytes == 0)
        return null_texture();
    const u32 elements = static_cast<u32>(binding.size / bytes);
    if (elements == 0)
        return null_texture();
    const auto usage = storage ? TextureUsage::Sample | TextureUsage::Storage : TextureUsage::Sample;
    const size_t alignment = metal.TextureBufferAlignment(format);
    if (binding.offset % alignment == 0) {
        const TexelKey key{binding.buffer.Identity(), binding.offset, elements, format, storage};
        if (const auto it = texel_textures.find(key); it != texel_textures.end())
            return it->second;
        if (texel_textures.size() >= 4096)
            texel_textures.clear();
        auto texture =
            metal.CreateBufferTexture(binding.buffer, binding.offset, elements, format, usage);
        texel_textures.emplace(key, texture);
        return texture;
    }
    // Metal needs aligned texel-buffer offsets; guest buffers have none. Sampled data is
    // copied to a fresh allocation (before any render pass of this draw begins).
    if (storage) {
        LOG_WARNING(Render, "Metal: misaligned image buffer (offset {}); writes are dropped",
                    binding.offset);
    }
    if (binding.offset % 4 != 0) {
        LOG_WARNING(Render, "Metal: texel buffer offset {} is not 4-byte aligned; unbound",
                    binding.offset);
        return null_texture();
    }
    const size_t available = binding.buffer.Size() - binding.offset;
    const size_t copy_size =
        Common::AlignDown(std::min<size_t>(Common::AlignUp(binding.size, 4), available), 4);
    if (copy_size == 0)
        return null_texture();
    const u32 copied_elements = static_cast<u32>(copy_size / bytes);
    if (copied_elements == 0)
        return null_texture();
    const auto copy = texture_runtime.scheduler.Stream(copy_size, std::max<size_t>(alignment, 256));
    texture_runtime.scheduler.Record().CopyBuffer(binding.buffer, binding.offset, copy.buffer,
                                                  copy.offset, copy_size);
    return metal.CreateBufferTexture(copy.buffer, copy.offset, copied_elements, format, usage);
}

const Texture& ResourceBinder::SampledTexture(VideoCommon::ImageViewId id,
                                              Shader::TextureType type) {
    const Texture& texture = texture_cache.GetImageView(id).Handle(type);
    if (texture.Valid())
        return texture;
    return texture_cache.GetImageView(VideoCommon::NULL_IMAGE_VIEW_ID).Handle(type);
}

const Texture& ResourceBinder::StorageTexture(VideoCommon::ImageViewId id,
                                              const ResourceSlot& slot) {
    auto& view = texture_cache.GetImageView(id);
    const Texture& texture = view.StorageView(slot.texture_type, slot.image_format,
                                              slot.is_integer);
    if (texture.Valid())
        return texture;
    const bool integer = slot.is_integer || (slot.image_format != Shader::ImageFormat::Typeless &&
                                             slot.image_format != Shader::ImageFormat::R32_SFLOAT &&
                                             slot.image_format != Shader::ImageFormat::R32G32_SFLOAT &&
                                             slot.image_format !=
                                                 Shader::ImageFormat::R32G32B32A32_SFLOAT);
    TextureDimension dimension = TextureDimension::D2;
    switch (slot.texture_type) {
    case Shader::TextureType::Color1D:
        dimension = TextureDimension::D1;
        break;
    case Shader::TextureType::ColorArray1D:
        dimension = TextureDimension::D1Array;
        break;
    case Shader::TextureType::ColorArray2D:
        dimension = TextureDimension::D2Array;
        break;
    case Shader::TextureType::Color3D:
        dimension = TextureDimension::D3;
        break;
    default:
        break;
    }
    return texture_runtime.NullStorageTexture(dimension, integer);
}

StageArguments ResourceBinder::Fill(StageProgram& program, const StageResources& resources) {
    StageArguments result{};
    for (auto& recorder : recorders)
        recorder.Reset();
    std::array<std::vector<u32>, 2> size_tables;
    for (const auto& binding : program.source.bindings) {
        if (binding.kind == ResourceKind::PushConstant || binding.kind == ResourceKind::BufferSizes)
            continue;
        if (binding.set >= 2 || !program.arguments[binding.set].Valid())
            continue;
        ArgumentRecorder* const arguments = &recorders[binding.set];
        if (binding.set == 0) {
            // Uniform buffers: binding numbers follow the enabled constant buffers in order,
            // as the buffer cache's binding indices do.
            for (u32 i = 0; i < binding.count; ++i) {
                const u32 index = binding.binding + i;
                const auto& uniforms = resources.buffers->uniforms;
                if (index < uniforms.size() && uniforms[index].size > 0 &&
                    uniforms[index].buffer.Valid() &&
                    uniforms[index].offset % 4 == 0) {
                    arguments->SetBuffer(binding.buffer + i, uniforms[index].buffer,
                                         uniforms[index].offset);
                } else {
                    arguments->SetBuffer(binding.buffer + i, NullBuffer());
                }
            }
            continue;
        }
        const auto& slots = program.resource_slots;
        if (binding.binding >= slots.size()) {
            throw std::runtime_error("Metal resource binding outside the guest layout");
        }
        const ResourceSlot& slot = slots[binding.binding];
        using Type = ResourceSlot::Type;
        const bool consistent = [&] {
            switch (slot.type) {
            case Type::StorageBuffer:
                return binding.kind == ResourceKind::StorageBuffer;
            case Type::TextureBuffer:
                return binding.kind == ResourceKind::SeparateImage;
            case Type::ImageBuffer:
            case Type::Image:
                return binding.kind == ResourceKind::StorageImage;
            case Type::Texture:
                return binding.kind == ResourceKind::SampledImage ||
                       binding.kind == ResourceKind::SeparateImage;
            case Type::None:
                return false;
            }
            return false;
        }();
        if (!consistent) {
            throw std::runtime_error("Metal resource layout mismatch at set 1 binding " +
                                     std::to_string(binding.binding));
        }
        switch (slot.type) {
        case Type::StorageBuffer: {
            const auto& storage = resources.buffers->storage;
            for (u32 i = 0; i < binding.count; ++i) {
                const u32 index = slot.first + i;
                u32 size = 0;
                if (index < storage.size() && storage[index].size > 0 &&
                    storage[index].buffer.Valid() &&
                    storage[index].offset % 4 == 0) {
                    arguments->SetBuffer(binding.buffer + i, storage[index].buffer,
                                         storage[index].offset, slot.is_written);
                    size = static_cast<u32>(storage[index].size);
                } else {
                    arguments->SetBuffer(binding.buffer + i, NullBuffer());
                }
                auto& table = size_tables[binding.set];
                if (table.size() <= binding.buffer + i)
                    table.resize(binding.buffer + i + 1);
                table[binding.buffer + i] = size;
            }
            break;
        }
        case Type::TextureBuffer:
        case Type::ImageBuffer: {
            const bool storage = slot.type == Type::ImageBuffer;
            for (u32 i = 0; i < binding.count; ++i) {
                const u32 index = slot.first + i;
                Texture texture;
                if (index < resources.texels.size())
                    texture = TexelTexture(resources.texels[index], storage, slot.is_integer);
                else
                    texture = texture_runtime.NullStorageTexture(TextureDimension::Buffer,
                                                                 slot.is_integer);
                arguments->SetTexture(binding.texture + i, texture, storage && slot.is_written);
            }
            break;
        }
        case Type::Texture:
            for (u32 i = 0; i < binding.count; ++i) {
                const auto id = slot.first + i < resources.views.size()
                                    ? resources.views[slot.first + i].id
                                    : VideoCommon::NULL_IMAGE_VIEW_ID;
                const auto sampler_id = slot.sampler_first + i < resources.samplers.size()
                                            ? resources.samplers[slot.sampler_first + i]
                                            : VideoCommon::NULL_SAMPLER_ID;
                const Texture& texture = SampledTexture(id, slot.texture_type);
                if (!texture.Valid())
                    throw std::runtime_error("Metal has no placeholder for this texture type");
                arguments->SetTexture(binding.texture + i, texture);
                if (binding.sampler != InvalidBinding) {
                    arguments->SetSampler(binding.sampler + i,
                                          texture_cache.GetSampler(sampler_id).Handle());
                }
            }
            break;
        case Type::Image:
            for (u32 i = 0; i < binding.count; ++i) {
                const auto id = slot.first + i < resources.views.size()
                                    ? resources.views[slot.first + i].id
                                    : VideoCommon::NULL_IMAGE_VIEW_ID;
                arguments->SetTexture(binding.texture + i, StorageTexture(id, slot), slot.is_written);
            }
            break;
        case Type::None:
            throw std::runtime_error("Metal resource binding has no guest resource");
        }
    }
    for (u32 set = 0; set < 2; ++set) {
        if (!program.arguments[set].Valid())
            continue;
        const u32 member = program.source.buffer_size_member[set];
        if (member != InvalidBinding) {
            auto& table = size_tables[set];
            if (table.empty())
                table.resize(1);
            recorders[set].SetSizeTable(member, table);
        }
        Commit(program, set, recorders[set], result);
    }
    return result;
}

void ResourceBinder::Commit(StageProgram& program, u32 set, ArgumentRecorder& recorder,
                            StageArguments& result) {
    // Argument buffers are only read by the GPU, so one whose members match this draw's
    // can be bound again as is, even while earlier submissions still use it.
    if (program.last_arguments[set] && program.last_ops[set] == recorder.ops) {
        result[set] = program.last_arguments[set];
        ++stats.argument_reuses;
        return;
    }
    ArgumentBuffer& arguments = program.arguments[set].Acquire();
    Buffer sizes_buffer;
    auto& writes = argument_writes;
    writes.clear();
    size_t buffer_index = 0, texture_index = 0, sampler_index = 0;
    for (size_t i = 0; i < recorder.ops.size(); ++i) {
        const ArgumentOp& op = recorder.ops[i];
        ArgumentWrite write;
        write.member = op.member;
        write.is_written = op.is_written;
        switch (op.kind) {
        case ArgumentOp::Kind::Buffer:
            write.kind = ArgumentWrite::Kind::Buffer;
            write.buffer = &recorder.buffers[buffer_index++];
            write.offset = op.offset;
            break;
        case ArgumentOp::Kind::Texture:
            write.kind = ArgumentWrite::Kind::Texture;
            write.texture = &recorder.textures[texture_index++];
            break;
        case ArgumentOp::Kind::Sampler:
            write.kind = ArgumentWrite::Kind::Sampler;
            write.sampler = &recorder.samplers[sampler_index++];
            break;
        case ArgumentOp::Kind::SizeTable: {
            const size_t count = static_cast<size_t>(op.offset);
            std::vector<u32> sizes(count);
            for (size_t j = 0; j < count; ++j)
                sizes[j] = static_cast<u32>(recorder.ops[i + 1 + j].offset);
            // Commit can reuse these arguments long after this command buffer completes.
            // Stream chunks recycle independently, so they cannot hold cached bounds tables.
            sizes_buffer = arguments.UploadSizeTable(metal, sizes);
            write.kind = ArgumentWrite::Kind::Buffer;
            write.buffer = &sizes_buffer;
            write.offset = 0;
            i += count;
            break;
        }
        case ArgumentOp::Kind::SizeValue:
            continue;
        }
        writes.push_back(write);
    }
    arguments.Encode(writes);
    program.last_ops[set] = recorder.ops;
    program.last_arguments[set] = &arguments;
    result[set] = &arguments;
    ++stats.argument_writes;
}

void ResourceBinder::Bind(Commands& commands, Stage stage, const StageArguments& arguments) {
    for (const ArgumentBuffer* buffer : arguments) {
        if (buffer)
            commands.SetArguments(stage, *buffer);
    }
}

void PushConstants::SetUnscaled() {
    words.fill(0);
    words[6] = Common::BitCast<u32>(1.0f); // down_factor at byte offset 24
}

void PushConstants::SetRenderArea(float width, float height) {
    words[0] = Common::BitCast<u32>(width);
    words[1] = Common::BitCast<u32>(height);
    words[2] = 0;
    words[3] = 0;
}

} // namespace NativeMetal
