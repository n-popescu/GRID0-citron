// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <memory>
#include <unordered_map>
#include <span>
#include <vector>
#include "shader_recompiler/shader_info.h"
#include "video_core/renderer_metal/metal_buffer_cache.h"
#include "video_core/renderer_metal/metal_shader.h"
#include "video_core/renderer_metal/metal_texture_cache.h"

namespace NativeMetal {

/// Recycles argument buffers of one layout. Writing the members of an argument buffer that
/// the open recording or an in-flight submission still reads would wait (or deadlock), so
/// every draw takes one the GPU is done with and the pool grows to the frames in flight.
class ArgumentPool {
public:
    ArgumentPool() = default;
    ArgumentPool(Runtime& metal_, ArgumentBuffer prototype_)
        : metal{&metal_}, prototype{std::move(prototype_)} {}
    bool Valid() const {
        return prototype.Valid();
    }
    /// The least recently acquired clone if the GPU is done with it, else a new clone.
    /// References stay valid for the pool's lifetime.
    ArgumentBuffer& Acquire();

private:
    Runtime* metal{};
    ArgumentBuffer prototype;
    std::vector<std::unique_ptr<ArgumentBuffer>> buffers; // ring, oldest at `next`
    size_t next{};
};

/// One argument-buffer member write, compared between draws to skip re-encoding.
struct ArgumentOp {
    enum class Kind : u8 { Buffer, Texture, Sampler, SizeTable, SizeValue };
    Kind kind{};
    u32 member{};
    const void* id{};
    u64 offset{};
    bool is_written{};
    bool operator==(const ArgumentOp&) const = default;
};

/// Records a stage set's member writes; ResourceBinder applies them only when they differ
/// from what the previously filled argument buffer of that set already holds.
class ArgumentRecorder {
public:
    void Reset() {
        ops.clear();
        buffers.clear();
        textures.clear();
        samplers.clear();
    }
    void SetBuffer(u32 member, const Buffer& buffer, size_t offset = 0, bool written = false) {
        ops.push_back({ArgumentOp::Kind::Buffer, member, buffer.Identity(), offset, written});
        buffers.push_back(buffer);
    }
    void SetTexture(u32 member, const Texture& texture, bool written = false) {
        ops.push_back({ArgumentOp::Kind::Texture, member, texture.Identity(), 0, written});
        textures.push_back(texture);
    }
    void SetSampler(u32 member, const Sampler& sampler) {
        ops.push_back({ArgumentOp::Kind::Sampler, member, sampler.Identity(), 0});
        samplers.push_back(sampler);
    }
    void SetSizeTable(u32 member, std::span<const u32> sizes) {
        ops.push_back({ArgumentOp::Kind::SizeTable, member, nullptr, sizes.size()});
        for (const u32 size : sizes)
            ops.push_back({ArgumentOp::Kind::SizeValue, 0, nullptr, size});
    }

    std::vector<ArgumentOp> ops;
    std::vector<Buffer> buffers;
    std::vector<Texture> textures;
    std::vector<Sampler> samplers;
};

/// Which guest resource a set-1 (resource set) SPIR-V binding of a stage reads.
struct ResourceSlot {
    enum class Type : u8 { None, StorageBuffer, TextureBuffer, ImageBuffer, Texture, Image };
    Type type{Type::None};
    /// Storage buffers: binding index. Other types: index into the stage's flattened view
    /// list (texture buffers, image buffers, textures, images; one entry per array element).
    u32 first{};
    /// Textures: index of the first sampler of this descriptor in the stage's samplers.
    u32 sampler_first{};
    Shader::TextureType texture_type{};
    Shader::ImageFormat image_format{};
    bool is_integer{};
    bool is_written{};
};

/// A compiled guest stage: its translated program info, Metal function and argument layouts.
struct StageProgram {
    Shader::Info info;
    ShaderSource source;
    Function function;
    std::array<ArgumentPool, 2> arguments;
    std::vector<ResourceSlot> resource_slots; // indexed by set-1 binding number
    /// What the last filled argument buffer of each set holds (see ResourceBinder::Fill).
    std::array<std::vector<ArgumentOp>, 2> last_ops;
    std::array<ArgumentBuffer*, 2> last_arguments{};

    StageProgram(Runtime& metal, Shader::Info info_, ShaderSource source_);
};

/// Guest resources resolved for one stage of one draw or dispatch.
struct StageResources {
    const StageBufferBindings* buffers{};
    std::span<const TexelBufferBinding> texels;
    std::span<const VideoCommon::ImageViewInOut> views;
    std::span<const VideoCommon::SamplerId> samplers;
};

using StageArguments = std::array<ArgumentBuffer*, 2>;

/// Fills a stage's argument buffers from resolved guest resources. Unbound or unrepresentable
/// resources become null placeholders so every argument member is valid.
class ResourceBinder {
public:
    ResourceBinder(Runtime& metal_, TextureCache& texture_cache_,
                   TextureCacheRuntime& texture_runtime_)
        : metal{metal_}, texture_cache{texture_cache_}, texture_runtime{texture_runtime_} {}

    StageArguments Fill(StageProgram& program, const StageResources& resources);
    static void Bind(Commands& commands, Stage stage, const StageArguments& arguments);

    /// Metal fetches vertices only with 4-byte aligned strides and offsets. Copies a guest
    /// vertex buffer into a versioned cached allocation with `dst_stride` (a multiple of 4) on the GPU,
    /// so GPU-written vertex data stays correct. Must be called outside a render pass.
    VertexBinding RepackVertices(Scheduler& scheduler, const VertexBinding& binding,
                                 u32 src_stride, u32 dst_stride);

private:
    const Buffer& NullBuffer();
    Texture TexelTexture(const TexelBufferBinding& binding, bool storage, bool integer);
    const Texture& SampledTexture(VideoCommon::ImageViewId id, Shader::TextureType type);
    const Texture& StorageTexture(VideoCommon::ImageViewId id, const ResourceSlot& slot);

    Runtime& metal;
    TextureCache& texture_cache;
    TextureCacheRuntime& texture_runtime;
    void Commit(StageProgram& program, u32 set, ArgumentRecorder& recorder,
                StageArguments& result);

    Buffer null_buffer;
    bool null_buffer_created{};
    std::array<ArgumentRecorder, 2> recorders;
    std::vector<ArgumentWrite> argument_writes;

    struct RepackKey {
        u64 buffer;
        u64 offset, size;
        u32 source_stride, destination_stride;
        bool operator==(const RepackKey&) const = default;
    };
    struct RepackHash {
        size_t operator()(const RepackKey& key) const noexcept {
            size_t h = std::hash<u64>{}(key.buffer);
            for (const u64 value : {key.offset, key.size, u64{key.source_stride},
                                    u64{key.destination_stride}})
                h ^= std::hash<u64>{}(value) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            return h;
        }
    };
    struct RepackEntry {
        VertexBinding converted;
        u64 version{}, last_use{};
    };
    std::unordered_map<RepackKey, RepackEntry, RepackHash> repacked_vertices;
    size_t repack_bytes{};
    u64 repack_clock{};
    static constexpr size_t RepackBudget = 64 * 1024 * 1024;
    static constexpr size_t RepackEntries = 512;

    struct TexelKey {
        const void* buffer;
        u64 offset;
        u32 elements;
        PixelFormat format;
        bool storage;
        bool operator==(const TexelKey&) const = default;
    };
    struct TexelKeyHash {
        size_t operator()(const TexelKey& key) const noexcept {
            size_t h = std::hash<const void*>{}(key.buffer);
            h ^= std::hash<u64>{}(key.offset) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            h ^= (static_cast<size_t>(key.elements) << 8) ^ static_cast<size_t>(key.format) ^
                 (key.storage ? 1ULL << 63 : 0);
            return h;
        }
    };
    /// Buffer textures by (buffer, range, format): creating one per draw is expensive and
    /// gives argument buffers a new identity every draw. Cleared when it grows too large.
    std::unordered_map<TexelKey, Texture, TexelKeyHash> texel_textures;

public:
    struct Stats {
        u64 argument_writes{};
        u64 argument_reuses{};
        u64 repack_hits{}, repack_misses{}, repack_bytes{};
    };
    Stats TakeStats() {
        const Stats result = stats;
        stats = {};
        return result;
    }

private:
    Stats stats;
};

/// Push-constant block shared by every guest stage (rescaling words and render area).
/// The emitter places both blocks at offset 0; render-area users get those words instead.
struct PushConstants {
    std::array<u32, 8> words{};
    void SetUnscaled();
    void SetRenderArea(float width, float height);
    std::span<const std::byte> Bytes() const {
        return std::as_bytes(std::span{words});
    }
};

/// Number of flattened descriptor entries.
template <typename Descriptors>
u32 CountDescriptors(const Descriptors& descriptors) {
    u32 count = 0;
    for (const auto& desc : descriptors)
        count += desc.count;
    return count;
}

} // namespace NativeMetal
