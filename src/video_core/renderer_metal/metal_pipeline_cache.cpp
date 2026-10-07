// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <fstream>
#include <thread>
#include <boost/container/static_vector.hpp>
#include "common/bit_cast.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/program_header.h"
#include "video_core/engines/draw_manager.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_metal/metal_pipeline_cache.h"
#include "video_core/shader_environment.h"
#include "video_core/surface.h"

namespace NativeMetal {
namespace {
using Maxwell = Tegra::Engines::Maxwell3D::Regs;

Shader::CompareFunction ShaderCompare(Maxwell::ComparisonOp comparison) {
    const auto raw = static_cast<u32>(comparison);
    const u32 index = raw >= 0x200 ? raw - 0x200 : raw - 1;
    static constexpr std::array table{
        Shader::CompareFunction::Never,         Shader::CompareFunction::Less,
        Shader::CompareFunction::Equal,         Shader::CompareFunction::LessThanEqual,
        Shader::CompareFunction::Greater,       Shader::CompareFunction::NotEqual,
        Shader::CompareFunction::GreaterThanEqual, Shader::CompareFunction::Always,
    };
    return index < table.size() ? table[index] : Shader::CompareFunction::Always;
}

Shader::AttributeType CastAttributeType(const Vulkan::FixedPipelineState::VertexAttribute& attr) {
    if (attr.enabled == 0)
        return Shader::AttributeType::Disabled;
    using Type = Maxwell::VertexAttribute::Type;
    switch (attr.Type()) {
    case Type::SNorm:
    case Type::UNorm:
    case Type::Float:
        return Shader::AttributeType::Float;
    case Type::SInt:
        return Shader::AttributeType::SignedInt;
    case Type::UInt:
        return Shader::AttributeType::UnsignedInt;
    case Type::UScaled:
        return Shader::AttributeType::UnsignedScaled;
    case Type::SScaled:
        return Shader::AttributeType::SignedScaled;
    default:
        return Shader::AttributeType::Disabled;
    }
}

Shader::FragmentOutputType FragmentOutputType(u8 encoded_format) {
    const auto format = static_cast<Tegra::RenderTargetFormat>(encoded_format);
    if (format == Tegra::RenderTargetFormat::NONE)
        return Shader::FragmentOutputType::Float;
    const auto pixel_format = VideoCore::Surface::PixelFormatFromRenderTargetFormat(format);
    if (!VideoCore::Surface::IsPixelFormatInteger(pixel_format))
        return Shader::FragmentOutputType::Float;
    return VideoCore::Surface::IsPixelFormatSignedInteger(pixel_format)
               ? Shader::FragmentOutputType::SignedInt
               : Shader::FragmentOutputType::UnsignedInt;
}

Shader::RuntimeInfo MakeRuntimeInfo(const GraphicsPipelineKey& key,
                                    const Shader::IR::Program& program,
                                    const Shader::IR::Program* previous_program) {
    Shader::RuntimeInfo info;
    if (previous_program) {
        info.previous_stage_stores = previous_program->info.stores;
        info.previous_stage_legacy_stores_mapping = previous_program->info.legacy_stores_mapping;
    } else {
        info.previous_stage_stores.mask.set();
    }
    switch (program.stage) {
    case Shader::Stage::VertexB:
        if (key.state.topology == Maxwell::PrimitiveTopology::Points)
            info.fixed_state_point_size = Common::BitCast<float>(key.state.point_size);
        info.convert_depth_mode = key.state.ndc_minus_one_to_one != 0;
        std::ranges::transform(key.state.attributes, info.generic_input_types.begin(),
                               &CastAttributeType);
        break;
    case Shader::Stage::Fragment:
        std::ranges::transform(key.state.color_formats, info.frag_color_types.begin(),
                               &FragmentOutputType);
        info.alpha_test_func =
            ShaderCompare(key.state.UnpackComparisonOp(key.state.alpha_test_func.Value()));
        info.alpha_test_reference = Common::BitCast<float>(key.state.alpha_test_ref);
        break;
    default:
        break;
    }
    switch (key.state.topology) {
    case Maxwell::PrimitiveTopology::Points:
        info.input_topology = Shader::InputTopology::Points;
        break;
    case Maxwell::PrimitiveTopology::Lines:
    case Maxwell::PrimitiveTopology::LineLoop:
    case Maxwell::PrimitiveTopology::LineStrip:
        info.input_topology = Shader::InputTopology::Lines;
        break;
    case Maxwell::PrimitiveTopology::LinesAdjacency:
    case Maxwell::PrimitiveTopology::LineStripAdjacency:
        info.input_topology = Shader::InputTopology::LinesAdjacency;
        break;
    case Maxwell::PrimitiveTopology::TrianglesAdjacency:
    case Maxwell::PrimitiveTopology::TriangleStripAdjacency:
        info.input_topology = Shader::InputTopology::TrianglesAdjacency;
        break;
    default:
        info.input_topology = Shader::InputTopology::Triangles;
        break;
    }
    info.force_early_z = key.state.early_z != 0;
    info.y_negate = key.state.y_negate != 0;
    return info;
}
} // namespace

namespace {
// Metal keys (FixedPipelineState with every dynamic feature off) in the shared transferable
// environment format. Kept in its own file so Vulkan caches are never touched.
constexpr u32 METAL_CACHE_VERSION = 1;

size_t WorkerCount() {
    return std::max<size_t>(std::thread::hardware_concurrency(), 2) - 1;
}
} // namespace

PipelineCache::PipelineCache(Tegra::MaxwellDeviceMemoryManager& device_memory_,
                             PipelineContext context)
    : VideoCommon::ShaderCache{device_memory_}, ctx{context}, host_info{HostTranslateInfo()},
      use_asynchronous_shaders{Settings::values.use_asynchronous_shaders.GetValue()},
      workers(WorkerCount(), "MetalPipelineBuilder"),
      serialization_thread(1, "MetalPipelineSerialization") {}

PipelineCache::~PipelineCache() {
    workers.WaitForRequests();
    serialization_thread.WaitForRequests();
}

GraphicsPipeline* PipelineCache::CurrentGraphicsPipeline() {
    if (!RefreshStages(graphics_key.unique_hashes)) {
        current_entry = nullptr;
        return nullptr;
    }
    graphics_key.state.Refresh(*maxwell3d, dynamic_features);
    if (!current_entry || !(current_key == graphics_key)) {
        std::unique_lock lock{cache_mutex};
        auto [it, is_new] = graphics_cache.try_emplace(graphics_key);
        if (is_new)
            it->second = std::make_unique<GraphicsEntry>();
        GraphicsEntry* const entry = it->second.get();
        lock.unlock();
        current_entry = entry;
        current_key = graphics_key;
        if (is_new)
            CreateGraphicsPipeline(*entry);
    }
    if (!current_entry->ready.load(std::memory_order_acquire))
        return nullptr; // still compiling asynchronously: skip the draw
    return current_entry->pipeline.get();
}

void PipelineCache::CreateGraphicsPipeline(GraphicsEntry& entry) {
    GraphicsEnvironments environments;
    GetGraphicsEnvironments(environments, graphics_key.unique_hashes);
    main_pools.ReleaseContents();
    auto translated = TranslateGraphics(main_pools, graphics_key, environments.Span());
    if (!translated) {
        entry.ready.store(true, std::memory_order_release);
        return;
    }
    if (!pipeline_cache_filename.empty()) {
        serialization_thread.QueueWork(
            [this, key = graphics_key, envs = std::move(environments.envs)] {
                boost::container::static_vector<const VideoCommon::GenericEnvironment*,
                                                Tegra::Engines::Maxwell3D::Regs::MaxShaderProgram>
                    env_ptrs;
                for (size_t index = 0; index < envs.size(); ++index) {
                    if (key.unique_hashes[index] != 0)
                        env_ptrs.push_back(&envs[index]);
                }
                VideoCommon::SerializePipeline(key, env_ptrs, pipeline_cache_filename,
                                               METAL_CACHE_VERSION);
            });
    }
    if (!use_asynchronous_shaders) {
        entry.pipeline = BuildGraphics(graphics_key, std::move(*translated));
        entry.ready.store(true, std::memory_order_release);
        return;
    }
    workers.QueueWork([this, &entry, key = graphics_key, stages = std::move(*translated)]() mutable {
        entry.pipeline = BuildGraphics(key, std::move(stages));
        entry.ready.store(true, std::memory_order_release);
    });
}

ComputePipeline* PipelineCache::CurrentComputePipeline() {
    const VideoCommon::ShaderInfo* const shader = ComputeShader();
    if (!shader)
        return nullptr;
    const auto& qmd = kepler_compute->launch_description;
    const ComputePipelineKey key{
        .unique_hash = shader->unique_hash,
        .shared_memory_size = qmd.shared_alloc,
        .workgroup_size{qmd.block_dim_x, qmd.block_dim_y, qmd.block_dim_z},
    };
    std::unique_lock lock{cache_mutex};
    const auto [it, is_new] = compute_cache.try_emplace(key);
    lock.unlock();
    if (is_new) {
        const GPUVAddr program_base = kepler_compute->regs.code_loc.Address();
        VideoCommon::ComputeEnvironment env{*kepler_compute, *gpu_memory, program_base,
                                            qmd.program_start};
        env.SetCachedSize(shader->size_bytes);
        main_pools.ReleaseContents();
        auto pipeline = BuildCompute(main_pools, key, env);
        if (pipeline && !pipeline_cache_filename.empty()) {
            serialization_thread.QueueWork([this, key, env_ = std::move(env)] {
                VideoCommon::SerializePipeline(
                    key, std::array<const VideoCommon::GenericEnvironment*, 1>{&env_},
                    pipeline_cache_filename, METAL_CACHE_VERSION);
            });
        }
        lock.lock();
        it->second = std::move(pipeline);
        lock.unlock();
    }
    return it->second && it->second->Valid() ? it->second.get() : nullptr;
}

std::optional<PipelineCache::TranslatedStages> PipelineCache::TranslateGraphics(
    ShaderPools& pools, const GraphicsPipelineKey& key,
    std::span<Shader::Environment* const> envs) try {
    // Indices: 0 VertexA, 1 VertexB, 2 TessellationControl, 3 TessellationEval,
    // 4 Geometry, 5 Fragment.
    for (const size_t unsupported : {2, 3, 4}) {
        if (key.unique_hashes[unsupported] != 0) {
            LOG_ERROR(Render, "Metal: pipeline {:016x} uses geometry/tessellation stage {}, "
                              "which is not emulated yet; draws are skipped",
                      key.Hash(), unsupported);
            return std::nullopt;
        }
    }
    std::array<Shader::IR::Program, Maxwell::MaxShaderProgram> programs;
    const bool uses_vertex_a = key.unique_hashes[0] != 0;
    const bool uses_vertex_b = key.unique_hashes[1] != 0;
    size_t env_index = 0;
    for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
        if (key.unique_hashes[index] == 0)
            continue;
        Shader::Environment& env = *envs[env_index++];
        const u32 cfg_offset = static_cast<u32>(env.StartAddress() + sizeof(Shader::ProgramHeader));
        Shader::Maxwell::Flow::CFG cfg(env, pools.flow_block, cfg_offset, index == 0);
        if (!uses_vertex_a || index != 1) {
            programs[index] =
                Shader::Maxwell::TranslateProgram(pools.inst, pools.block, env, cfg, host_info);
        } else {
            auto program_vb =
                Shader::Maxwell::TranslateProgram(pools.inst, pools.block, env, cfg, host_info);
            programs[index] = Shader::Maxwell::MergeDualVertexPrograms(programs[0], program_vb, env);
        }
        if (programs[index].info.requires_layer_emulation) {
            LOG_WARNING(Render, "Metal: shader {:016x} writes the layer without geometry "
                                "passthrough emulation",
                        key.unique_hashes[index]);
        }
    }
    TranslatedStages stages;
    const Shader::IR::Program* previous_stage = nullptr;
    for (size_t index = uses_vertex_a && uses_vertex_b ? 1 : 0; index < Maxwell::MaxShaderProgram;
         ++index) {
        if (key.unique_hashes[index] == 0)
            continue;
        if (index == 0) {
            LOG_ERROR(Render, "Metal: VertexA without VertexB is not supported");
            return std::nullopt;
        }
        Shader::IR::Program& program = programs[index];
        const auto runtime_info = MakeRuntimeInfo(key, program, previous_stage);
        Shader::Maxwell::ConvertLegacyToGeneric(program, runtime_info);
        // Each stage has its own argument buffers, so binding numbers restart per stage.
        Shader::Backend::Bindings bindings;
        auto source = EmitGuestShader(program, runtime_info, bindings);
        if (Settings::values.dump_shaders.GetValue()) {
            LOG_INFO(Render, "Metal shader {:016x}:\n{}", key.unique_hashes[index], source.msl);
        }
        stages[index - 1] = TranslatedStage{program.info, std::move(source)};
        previous_stage = &program;
    }
    return stages;
} catch (const std::exception& error) {
    LOG_ERROR(Render, "Metal: graphics pipeline {:016x} translation failed: {}", key.Hash(),
              error.what());
    return std::nullopt;
}

std::unique_ptr<GraphicsPipeline> PipelineCache::BuildGraphics(const GraphicsPipelineKey& key,
                                                               TranslatedStages translated) try {
    std::array<std::unique_ptr<StageProgram>, GraphicsPipeline::NUM_STAGES> stages;
    for (size_t index = 0; index < stages.size(); ++index) {
        if (!translated[index])
            continue;
        stages[index] = std::make_unique<StageProgram>(ctx.metal, std::move(translated[index]->info),
                                                       std::move(translated[index]->source));
    }
    LOG_DEBUG(Render, "Metal: built graphics pipeline {:016x}", key.Hash());
    return std::make_unique<GraphicsPipeline>(ctx, key, std::move(stages));
} catch (const std::exception& error) {
    LOG_ERROR(Render, "Metal: graphics pipeline {:016x} failed: {}", key.Hash(), error.what());
    return nullptr;
}

std::unique_ptr<ComputePipeline> PipelineCache::BuildCompute(ShaderPools& pools,
                                                             const ComputePipelineKey& key,
                                                             Shader::Environment& env) try {
    Shader::Maxwell::Flow::CFG cfg{env, pools.flow_block, env.StartAddress()};
    auto program = Shader::Maxwell::TranslateProgram(pools.inst, pools.block, env, cfg, host_info);
    Shader::Backend::Bindings bindings;
    auto source = EmitGuestShader(program, {}, bindings);
    auto stage = std::make_unique<StageProgram>(ctx.metal, program.info, std::move(source));
    LOG_DEBUG(Render, "Metal: built compute pipeline {:016x}", key.unique_hash);
    return std::make_unique<ComputePipeline>(ctx, std::move(stage));
} catch (const std::exception& error) {
    LOG_ERROR(Render, "Metal: compute pipeline {:016x} failed: {}", key.unique_hash, error.what());
    return nullptr;
}

void PipelineCache::LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                                      const VideoCore::DiskResourceLoadCallback& callback) {
    if (title_id == 0 || !Settings::values.use_disk_shader_cache.GetValue())
        return;
    const auto shader_dir = Common::FS::GetCitronPath(Common::FS::CitronPath::ShaderDir);
    const auto base_dir = shader_dir / fmt::format("{:016x}", title_id);
    if (!Common::FS::CreateDir(shader_dir) || !Common::FS::CreateDir(base_dir)) {
        LOG_ERROR(Common_Filesystem, "Failed to create the Metal pipeline cache directories");
        return;
    }
    pipeline_cache_filename = base_dir / "metal.bin";

    struct {
        std::mutex mutex;
        size_t total{};
        size_t built{};
        bool has_loaded{};
    } state;
    const auto load_compute = [&](std::ifstream& file, VideoCommon::FileEnvironment env) {
        ComputePipelineKey key;
        file.read(reinterpret_cast<char*>(&key), sizeof(key));
        if (!env.HasValidEntryInstruction())
            return;
        workers.QueueWork([this, key, env_ = std::move(env), &state, &callback]() mutable {
            ShaderPools pools;
            auto pipeline = BuildCompute(pools, key, env_);
            std::scoped_lock lock{state.mutex, cache_mutex};
            if (pipeline)
                compute_cache.emplace(key, std::move(pipeline));
            ++state.built;
            if (state.has_loaded)
                callback(VideoCore::LoadCallbackStage::Build, state.built, state.total);
        });
        ++state.total;
    };
    const auto load_graphics = [&](std::ifstream& file,
                                   std::vector<VideoCommon::FileEnvironment> envs) {
        GraphicsPipelineKey key;
        file.read(reinterpret_cast<char*>(&key), sizeof(key));
        if (!std::ranges::all_of(envs, &VideoCommon::FileEnvironment::HasValidEntryInstruction))
            return;
        workers.QueueWork([this, key, envs_ = std::move(envs), &state, &callback]() mutable {
            ShaderPools pools;
            boost::container::static_vector<Shader::Environment*, 6> env_ptrs;
            for (auto& env : envs_)
                env_ptrs.push_back(&env);
            auto translated = TranslateGraphics(pools, key, std::span(env_ptrs.data(), env_ptrs.size()));
            auto entry = std::make_unique<GraphicsEntry>();
            if (translated)
                entry->pipeline = BuildGraphics(key, std::move(*translated));
            entry->ready.store(true, std::memory_order_release);
            std::scoped_lock lock{state.mutex, cache_mutex};
            graphics_cache.try_emplace(key, std::move(entry));
            ++state.built;
            if (state.has_loaded)
                callback(VideoCore::LoadCallbackStage::Build, state.built, state.total);
        });
        ++state.total;
    };
    VideoCommon::LoadPipelines(stop_loading, pipeline_cache_filename, METAL_CACHE_VERSION,
                               load_compute, load_graphics);
    LOG_INFO(Render, "Metal: {} cached pipelines to build", state.total);
    {
        std::unique_lock lock{state.mutex};
        callback(VideoCore::LoadCallbackStage::Build, 0, state.total);
        state.has_loaded = true;
    }
    workers.WaitForRequests(stop_loading);
}

} // namespace NativeMetal
