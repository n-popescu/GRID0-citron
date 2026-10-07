// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <memory>
#include <unordered_map>
#include <vector>
#include "video_core/engines/maxwell_3d.h"
#include "video_core/renderer_metal/metal_pipeline_common.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"

namespace Tegra {
class MemoryManager;
namespace Engines {
class KeplerCompute;
}
} // namespace Tegra

namespace NativeMetal {

// The Vulkan backend's FixedPipelineState is API-independent; with every dynamic-state
// feature off it captures all fixed-function state a Metal pipeline needs.
struct GraphicsPipelineKey {
    std::array<u64, 6> unique_hashes;
    Vulkan::FixedPipelineState state;

    size_t Hash() const noexcept;
    bool operator==(const GraphicsPipelineKey& rhs) const noexcept;
    size_t Size() const noexcept {
        return sizeof(unique_hashes) + state.Size();
    }
};
static_assert(std::has_unique_object_representations_v<GraphicsPipelineKey>);
static_assert(std::is_trivially_copyable_v<GraphicsPipelineKey>);

struct ComputePipelineKey {
    u64 unique_hash;
    u32 shared_memory_size;
    std::array<u32, 3> workgroup_size;

    size_t Hash() const noexcept;
    bool operator==(const ComputePipelineKey& rhs) const noexcept;
};
static_assert(std::has_unique_object_representations_v<ComputePipelineKey>);

} // namespace NativeMetal

template <>
struct std::hash<NativeMetal::GraphicsPipelineKey> {
    size_t operator()(const NativeMetal::GraphicsPipelineKey& key) const noexcept {
        return key.Hash();
    }
};
template <>
struct std::hash<NativeMetal::ComputePipelineKey> {
    size_t operator()(const NativeMetal::ComputePipelineKey& key) const noexcept {
        return key.Hash();
    }
};

namespace NativeMetal {

struct PipelineContext {
    Runtime& metal;
    Scheduler& scheduler;
    BufferCacheRuntime& buffer_runtime;
    BufferCache& buffer_cache;
    TextureCacheRuntime& texture_runtime;
    TextureCache& texture_cache;
    ResourceBinder& binder;
};

class GraphicsPipeline {
public:
    static constexpr size_t NUM_STAGES = Tegra::Engines::Maxwell3D::Regs::MaxShaderStage;
    using Maxwell = Tegra::Engines::Maxwell3D::Regs;

    GraphicsPipeline(PipelineContext context, const GraphicsPipelineKey& key,
                     std::array<std::unique_ptr<StageProgram>, NUM_STAGES> stages);

    void SetEngine(Tegra::Engines::Maxwell3D* maxwell3d_, Tegra::MemoryManager* gpu_memory_) {
        maxwell3d = maxwell3d_;
        gpu_memory = gpu_memory_;
    }

    /// Resolves guest resources and fills argument buffers. May record transfers, so it must
    /// run before the draw's render pass begins.
    void Configure(bool is_indexed);

    /// Repacks vertex buffers whose stride or offset Metal cannot fetch (not 4-byte
    /// aligned). Records compute work, so it must run after Configure and before the pass.
    void PrepareVertexBuffers();

    /// Binds the pipeline variant for the framebuffer, vertex buffers and arguments inside
    /// the active render pass. Returns false if the draw cannot be expressed in Metal.
    bool Bind(Commands& commands, const CacheFramebuffer& framebuffer);

    const GraphicsPipelineKey& Key() const noexcept {
        return key;
    }

private:
    const Pipeline* Variant(const CacheFramebuffer& framebuffer);
    bool BuildVertexInput(RenderPipelineDesc& desc) const;
    DepthStencilDesc MakeDepthStencil(bool has_depth, bool has_stencil) const;

    PipelineContext ctx;
    GraphicsPipelineKey key;
    std::array<std::unique_ptr<StageProgram>, NUM_STAGES> stages;
    Tegra::Engines::Maxwell3D* maxwell3d{};
    Tegra::MemoryManager* gpu_memory{};

    std::array<u32, NUM_STAGES> enabled_uniform_buffer_masks{};
    VideoCommon::UniformBufferSizes uniform_buffer_sizes{};

    std::vector<VideoCommon::ImageViewInOut> views;
    std::vector<VideoCommon::SamplerId> samplers;
    std::array<StageArguments, NUM_STAGES> arguments{};
    PushConstants push_constants;
    std::array<bool, Maxwell::NumVertexArrays> uses_vertex_buffer{};
    std::array<VertexBinding, Maxwell::NumVertexArrays> repacked_vertices{};
    std::array<bool, Maxwell::NumVertexArrays> uses_repacked{};

    std::unordered_map<u64, Pipeline> variants;
    std::vector<std::pair<DepthStencilDesc, DepthStencilState>> depth_states;
    bool failed{};
};

class ComputePipeline {
public:
    ComputePipeline(PipelineContext context, std::unique_ptr<StageProgram> program);

    void Configure(Tegra::Engines::KeplerCompute& kepler_compute, Tegra::MemoryManager& gpu_memory);
    /// Begins the compute encoder and binds the configured resources.
    void Bind(Commands& commands);
    std::array<u32, 3> LocalSize() const noexcept {
        return program->source.workgroup_size;
    }
    bool Valid() const noexcept {
        return pipeline.Valid();
    }

private:
    PipelineContext ctx;
    std::unique_ptr<StageProgram> program;
    Pipeline pipeline;
    VideoCommon::ComputeUniformBufferSizes uniform_buffer_sizes{};
    std::vector<VideoCommon::ImageViewInOut> views;
    std::vector<VideoCommon::SamplerId> samplers;
    StageArguments arguments{};
    PushConstants push_constants;
};

} // namespace NativeMetal
