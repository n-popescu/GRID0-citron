// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <unordered_map>
#include "common/thread_worker.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/value.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/object_pool.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_metal/metal_pipeline.h"
#include "video_core/shader_cache.h"

namespace NativeMetal {

/// Guest shader and pipeline cache for the native Metal backend: shared Maxwell frontend,
/// SPIR-V emission with the Metal profile, SPIRV-Cross to MSL, then native Metal objects.
///
/// Pipelines are recorded in a per-title transferable cache (`metal.bin`, the same guest
/// environment format the Vulkan backend uses, with Metal keys) and rebuilt in parallel at
/// boot. With asynchronous shaders enabled, MSL compilation of new pipelines runs on worker
/// threads and draws using them are skipped until they are ready.
/// Geometry and tessellation stages are not emulated yet: such pipelines are skipped.
class PipelineCache : public VideoCommon::ShaderCache {
public:
    PipelineCache(Tegra::MaxwellDeviceMemoryManager& device_memory_, PipelineContext context);
    ~PipelineCache();

    GraphicsPipeline* CurrentGraphicsPipeline();
    ComputePipeline* CurrentComputePipeline();

    void LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                           const VideoCore::DiskResourceLoadCallback& callback);

private:
    struct ShaderPools {
        void ReleaseContents() {
            flow_block.ReleaseContents();
            block.ReleaseContents();
            inst.ReleaseContents();
        }
        Shader::ObjectPool<Shader::IR::Inst> inst{8192};
        Shader::ObjectPool<Shader::IR::Block> block{32};
        Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_block{32};
    };
    /// A stage translated to MSL but not yet compiled into a Metal function.
    struct TranslatedStage {
        Shader::Info info;
        ShaderSource source;
    };
    using TranslatedStages = std::array<std::optional<TranslatedStage>, GraphicsPipeline::NUM_STAGES>;

    struct GraphicsEntry {
        std::atomic<bool> ready{};
        std::unique_ptr<GraphicsPipeline> pipeline;
    };

    /// Maxwell → MSL for every stage of a graphics pipeline. Reads guest code through the
    /// environments, so runtime calls must happen on the GPU thread.
    std::optional<TranslatedStages> TranslateGraphics(ShaderPools& pools,
                                                      const GraphicsPipelineKey& key,
                                                      std::span<Shader::Environment* const> envs);
    /// Compiles translated stages into Metal functions (thread-safe).
    std::unique_ptr<GraphicsPipeline> BuildGraphics(const GraphicsPipelineKey& key,
                                                    TranslatedStages stages);
    std::unique_ptr<ComputePipeline> BuildCompute(ShaderPools& pools, const ComputePipelineKey& key,
                                                  Shader::Environment& env);
    void CreateGraphicsPipeline(GraphicsEntry& entry);

    PipelineContext ctx;
    Shader::HostTranslateInfo host_info;
    Vulkan::DynamicFeatures dynamic_features{};
    bool use_asynchronous_shaders{};

    GraphicsPipelineKey graphics_key{};
    GraphicsEntry* current_entry{};
    GraphicsPipelineKey current_key{};
    std::mutex cache_mutex; // graphics_cache/compute_cache structure (disk loading inserts)
    std::unordered_map<GraphicsPipelineKey, std::unique_ptr<GraphicsEntry>> graphics_cache;
    std::unordered_map<ComputePipelineKey, std::unique_ptr<ComputePipeline>> compute_cache;
    ShaderPools main_pools;

    std::filesystem::path pipeline_cache_filename;
    Common::ThreadWorker workers;
    Common::ThreadWorker serialization_thread;
};

} // namespace NativeMetal
