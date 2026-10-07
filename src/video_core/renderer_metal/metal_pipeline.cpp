// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cmath>
#include <cstring>
#include "common/alignment.h"
#include "common/bit_cast.h"
#include "common/cityhash.h"
#include "common/logging.h"
#include "video_core/engines/draw_manager.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_metal/metal_formats.h"
#include "video_core/renderer_metal/metal_pipeline.h"
#include "video_core/surface.h"
#include "video_core/textures/texture.h"

namespace NativeMetal {
namespace {
using Maxwell = Tegra::Engines::Maxwell3D::Regs;
using Tegra::Texture::TexturePair;

BlendFactor ConvertFactor(Maxwell::Blend::Factor factor) {
    using F = Maxwell::Blend::Factor;
    switch (factor) {
    case F::Zero_D3D:
    case F::Zero_GL:
        return BlendFactor::Zero;
    case F::One_D3D:
    case F::One_GL:
        return BlendFactor::One;
    case F::SourceColor_D3D:
    case F::SourceColor_GL:
        return BlendFactor::SourceColor;
    case F::OneMinusSourceColor_D3D:
    case F::OneMinusSourceColor_GL:
        return BlendFactor::OneMinusSourceColor;
    case F::SourceAlpha_D3D:
    case F::SourceAlpha_GL:
    case F::BothSourceAlpha_D3D:
        return BlendFactor::SourceAlpha;
    case F::OneMinusSourceAlpha_D3D:
    case F::OneMinusSourceAlpha_GL:
    case F::OneMinusBothSourceAlpha_D3D:
        return BlendFactor::OneMinusSourceAlpha;
    case F::DestAlpha_D3D:
    case F::DestAlpha_GL:
        return BlendFactor::DestinationAlpha;
    case F::OneMinusDestAlpha_D3D:
    case F::OneMinusDestAlpha_GL:
        return BlendFactor::OneMinusDestinationAlpha;
    case F::DestColor_D3D:
    case F::DestColor_GL:
        return BlendFactor::DestinationColor;
    case F::OneMinusDestColor_D3D:
    case F::OneMinusDestColor_GL:
        return BlendFactor::OneMinusDestinationColor;
    case F::SourceAlphaSaturate_D3D:
    case F::SourceAlphaSaturate_GL:
        return BlendFactor::SourceAlphaSaturated;
    case F::BlendFactor_D3D:
    case F::ConstantColor_GL:
        return BlendFactor::BlendColor;
    case F::OneMinusBlendFactor_D3D:
    case F::OneMinusConstantColor_GL:
        return BlendFactor::OneMinusBlendColor;
    case F::ConstantAlpha_GL:
        return BlendFactor::BlendAlpha;
    case F::OneMinusConstantAlpha_GL:
        return BlendFactor::OneMinusBlendAlpha;
    case F::Source1Color_D3D:
    case F::Source1Color_GL:
        return BlendFactor::Source1Color;
    case F::OneMinusSource1Color_D3D:
    case F::OneMinusSource1Color_GL:
        return BlendFactor::OneMinusSource1Color;
    case F::Source1Alpha_D3D:
    case F::Source1Alpha_GL:
        return BlendFactor::Source1Alpha;
    case F::OneMinusSource1Alpha_D3D:
    case F::OneMinusSource1Alpha_GL:
        return BlendFactor::OneMinusSource1Alpha;
    }
    LOG_WARNING(Render, "Metal: unknown blend factor {:#x}", static_cast<u32>(factor));
    return BlendFactor::One;
}

BlendOperation ConvertEquation(Maxwell::Blend::Equation equation) {
    using E = Maxwell::Blend::Equation;
    switch (equation) {
    case E::Add_D3D:
    case E::Add_GL:
        return BlendOperation::Add;
    case E::Subtract_D3D:
    case E::Subtract_GL:
        return BlendOperation::Subtract;
    case E::ReverseSubtract_D3D:
    case E::ReverseSubtract_GL:
        return BlendOperation::ReverseSubtract;
    case E::Min_D3D:
    case E::Min_GL:
        return BlendOperation::Min;
    case E::Max_D3D:
    case E::Max_GL:
        return BlendOperation::Max;
    }
    return BlendOperation::Add;
}

CompareFunction ConvertCompare(Maxwell::ComparisonOp op) {
    const auto raw = static_cast<u32>(op);
    // GL values are 0x200 + n, D3D values are 1 + n, in the same order as CompareFunction.
    const u32 index = raw >= 0x200 ? raw - 0x200 : raw - 1;
    return index < 8 ? static_cast<CompareFunction>(index) : CompareFunction::Always;
}

StencilOperation ConvertStencil(Maxwell::StencilOp::Op op) {
    using O = Maxwell::StencilOp::Op;
    switch (op) {
    case O::Keep_D3D:
    case O::Keep_GL:
        return StencilOperation::Keep;
    case O::Zero_D3D:
    case O::Zero_GL:
        return StencilOperation::Zero;
    case O::Replace_D3D:
    case O::Replace_GL:
        return StencilOperation::Replace;
    case O::IncrSaturate_D3D:
    case O::IncrSaturate_GL:
        return StencilOperation::IncrementClamp;
    case O::DecrSaturate_D3D:
    case O::DecrSaturate_GL:
        return StencilOperation::DecrementClamp;
    case O::Invert_D3D:
    case O::Invert_GL:
        return StencilOperation::Invert;
    case O::Incr_D3D:
    case O::Incr_GL:
        return StencilOperation::IncrementWrap;
    case O::Decr_D3D:
    case O::Decr_GL:
        return StencilOperation::DecrementWrap;
    }
    return StencilOperation::Keep;
}

InputTopology ConvertInputTopology(Maxwell::PrimitiveTopology topology) {
    switch (topology) {
    case Maxwell::PrimitiveTopology::Points:
        return InputTopology::Point;
    case Maxwell::PrimitiveTopology::Lines:
    case Maxwell::PrimitiveTopology::LineLoop:
    case Maxwell::PrimitiveTopology::LineStrip:
        return InputTopology::Line;
    default:
        return InputTopology::Triangle;
    }
}

bool IsIntegerColor(u8 encoded) {
    const auto format = static_cast<Tegra::RenderTargetFormat>(encoded);
    if (format == Tegra::RenderTargetFormat::NONE)
        return false;
    return VideoCore::Surface::IsPixelFormatInteger(
        VideoCore::Surface::PixelFormatFromRenderTargetFormat(format));
}

template <typename Desc>
u32 ReadHandleIndex(Tegra::MemoryManager& gpu_memory, GPUVAddr base, GPUVAddr secondary_base,
                    const Desc& desc, u32 index, bool via_header_index,
                    std::pair<u32, u32>& out) {
    const u32 index_offset = index << desc.size_shift;
    const GPUVAddr addr = base + desc.cbuf_offset + index_offset;
    if constexpr (std::is_same_v<Desc, Shader::TextureDescriptor> ||
                  std::is_same_v<Desc, Shader::TextureBufferDescriptor>) {
        if (desc.has_secondary) {
            const GPUVAddr separate = secondary_base + desc.secondary_cbuf_offset + index_offset;
            const u32 lhs = gpu_memory.Read<u32>(addr) << desc.shift_left;
            const u32 rhs = gpu_memory.Read<u32>(separate) << desc.secondary_shift_left;
            out = TexturePair(lhs | rhs, via_header_index);
            return out.first;
        }
    }
    out = TexturePair(gpu_memory.Read<u32>(addr), via_header_index);
    return out.first;
}

u64 FramebufferSignature(const CacheFramebuffer& framebuffer) {
    u64 signature = framebuffer.NumColorBuffers();
    for (u32 i = 0; i < VideoCommon::NUM_RT; ++i) {
        const auto& color = framebuffer.Colors()[i];
        const u64 format = color.Valid() ? static_cast<u64>(color.Format()) : 0;
        signature = signature * 131 + format;
    }
    const u64 depth = framebuffer.HasDepth() ? static_cast<u64>(framebuffer.Depth().Format()) : 0;
    return signature * 131 + depth;
}
} // namespace

size_t GraphicsPipelineKey::Hash() const noexcept {
    return static_cast<size_t>(
        Common::CityHash64(reinterpret_cast<const char*>(this), Size()));
}
bool GraphicsPipelineKey::operator==(const GraphicsPipelineKey& rhs) const noexcept {
    return std::memcmp(&rhs, this, Size()) == 0;
}
size_t ComputePipelineKey::Hash() const noexcept {
    return static_cast<size_t>(
        Common::CityHash64(reinterpret_cast<const char*>(this), sizeof(*this)));
}
bool ComputePipelineKey::operator==(const ComputePipelineKey& rhs) const noexcept {
    return std::memcmp(&rhs, this, sizeof(*this)) == 0;
}

GraphicsPipeline::GraphicsPipeline(PipelineContext context, const GraphicsPipelineKey& key_,
                                   std::array<std::unique_ptr<StageProgram>, NUM_STAGES> stages_)
    : ctx{context}, key{key_}, stages{std::move(stages_)} {
    for (size_t stage = 0; stage < NUM_STAGES; ++stage) {
        if (!stages[stage])
            continue;
        const auto& info = stages[stage]->info;
        enabled_uniform_buffer_masks[stage] = info.constant_buffer_mask;
        std::ranges::copy(info.constant_buffer_used_sizes, uniform_buffer_sizes[stage].begin());
    }
    if (stages[0]) {
        const auto& info = stages[0]->info;
        for (size_t index = 0; index < Maxwell::NumVertexAttributes; ++index) {
            const auto& attribute = key.state.attributes[index];
            if (attribute.enabled != 0 && info.loads.Generic(index))
                uses_vertex_buffer.at(attribute.buffer) = true;
        }
    }
}

void GraphicsPipeline::Configure(bool is_indexed) {
    auto& texture_cache = ctx.texture_cache;
    auto& buffer_cache = ctx.buffer_cache;
    views.clear();
    samplers.clear();
    ctx.buffer_runtime.ResetDescriptorBindings();
    texture_cache.SynchronizeGraphicsDescriptors();
    buffer_cache.SetUniformBuffersState(enabled_uniform_buffer_masks, &uniform_buffer_sizes);

    const auto& regs = maxwell3d->regs;
    const bool via_header_index = regs.sampler_binding == Maxwell::SamplerBinding::ViaHeaderBinding;
    std::array<size_t, NUM_STAGES> view_begin{}, sampler_begin{};
    for (size_t stage = 0; stage < NUM_STAGES; ++stage) {
        view_begin[stage] = views.size();
        sampler_begin[stage] = samplers.size();
        if (!stages[stage])
            continue;
        const Shader::Info& info = stages[stage]->info;
        buffer_cache.UnbindGraphicsStorageBuffers(stage);
        size_t ssbo_index = 0;
        for (const auto& desc : info.storage_buffers_descriptors) {
            buffer_cache.BindGraphicsStorageBuffer(stage, ssbo_index, desc.cbuf_index,
                                                   desc.cbuf_offset, desc.is_written);
            ++ssbo_index;
        }
        const auto& cbufs = maxwell3d->state.shader_stages[stage].const_buffers;
        const auto add_image = [&](const auto& desc, bool blacklist) {
            for (u32 index = 0; index < desc.count; ++index) {
                std::pair<u32, u32> handle;
                ReadHandleIndex(*gpu_memory, cbufs[desc.cbuf_index].address, 0, desc, index,
                                via_header_index, handle);
                views.push_back({.index = handle.first, .blacklist = blacklist, .id = {}});
            }
        };
        for (const auto& desc : info.texture_buffer_descriptors) {
            for (u32 index = 0; index < desc.count; ++index) {
                std::pair<u32, u32> handle;
                ReadHandleIndex(*gpu_memory, cbufs[desc.cbuf_index].address,
                                cbufs[desc.secondary_cbuf_index].address, desc, index,
                                via_header_index, handle);
                views.push_back({.index = handle.first});
            }
        }
        for (const auto& desc : info.image_buffer_descriptors)
            add_image(desc, false);
        for (const auto& desc : info.texture_descriptors) {
            for (u32 index = 0; index < desc.count; ++index) {
                std::pair<u32, u32> handle;
                ReadHandleIndex(*gpu_memory, cbufs[desc.cbuf_index].address,
                                cbufs[desc.secondary_cbuf_index].address, desc, index,
                                via_header_index, handle);
                views.push_back({.index = handle.first});
                samplers.push_back(handle.first == 0
                                       ? VideoCommon::NULL_SAMPLER_ID
                                       : texture_cache.GetGraphicsSamplerId(handle.second));
            }
        }
        for (const auto& desc : info.image_descriptors)
            add_image(desc, desc.is_written);
    }
    texture_cache.FillGraphicsImageViews<true>(std::span(views.data(), views.size()));

    for (size_t stage = 0; stage < NUM_STAGES; ++stage) {
        if (!stages[stage])
            continue;
        const Shader::Info& info = stages[stage]->info;
        buffer_cache.UnbindGraphicsTextureBuffers(stage);
        size_t index = 0;
        size_t view = view_begin[stage];
        const auto add_buffer = [&](const auto& desc, bool is_image, bool is_written) {
            for (u32 i = 0; i < desc.count; ++i) {
                auto& image_view = texture_cache.GetImageView(views[view].id);
                buffer_cache.BindGraphicsTextureBuffer(stage, index, image_view.GpuAddr(),
                                                       image_view.BufferSize(), image_view.format,
                                                       is_written, is_image);
                ++index;
                ++view;
            }
        };
        for (const auto& desc : info.texture_buffer_descriptors)
            add_buffer(desc, false, false);
        for (const auto& desc : info.image_buffer_descriptors)
            add_buffer(desc, true, desc.is_written);
    }

    buffer_cache.UpdateGraphicsBuffers(is_indexed);
    buffer_cache.BindHostGeometryBuffers(is_indexed);

    std::array<std::pair<size_t, size_t>, NUM_STAGES> texel_range{};
    for (size_t stage = 0; stage < NUM_STAGES; ++stage) {
        if (!stages[stage])
            continue;
        const size_t begin = ctx.buffer_runtime.TexelBindings().size();
        buffer_cache.BindHostStageBuffers(stage);
        texel_range[stage] = {begin, ctx.buffer_runtime.TexelBindings().size()};
        // Written storage images are guest-modified.
        size_t view = view_begin[stage] + CountDescriptors(stages[stage]->info.texture_buffer_descriptors) +
                      CountDescriptors(stages[stage]->info.image_buffer_descriptors) +
                      CountDescriptors(stages[stage]->info.texture_descriptors);
        for (const auto& desc : stages[stage]->info.image_descriptors) {
            for (u32 i = 0; i < desc.count; ++i, ++view) {
                if (desc.is_written)
                    texture_cache.MarkModification(texture_cache.GetImageView(views[view].id).image_id);
            }
        }
    }
    texture_cache.UpdateRenderTargets(false);
    texture_cache.CheckFeedbackLoop(views);

    push_constants.SetUnscaled();
    const auto& texels = ctx.buffer_runtime.TexelBindings();
    for (size_t stage = 0; stage < NUM_STAGES; ++stage) {
        arguments[stage] = {};
        if (!stages[stage])
            continue;
        const size_t view_end = stage + 1 < NUM_STAGES ? view_begin[stage + 1] : views.size();
        const size_t sampler_end =
            stage + 1 < NUM_STAGES ? sampler_begin[stage + 1] : samplers.size();
        StageResources resources;
        resources.buffers = &ctx.buffer_runtime.GraphicsBindings(stage);
        resources.texels = std::span(texels).subspan(texel_range[stage].first,
                                                     texel_range[stage].second -
                                                         texel_range[stage].first);
        resources.views = std::span(views).subspan(view_begin[stage], view_end - view_begin[stage]);
        resources.samplers = std::span(samplers).subspan(sampler_begin[stage],
                                                         sampler_end - sampler_begin[stage]);
        arguments[stage] = ctx.binder.Fill(*stages[stage], resources);
        if (stages[stage]->info.uses_render_area) {
            push_constants.SetRenderArea(static_cast<float>(regs.surface_clip.width),
                                         static_cast<float>(regs.surface_clip.height));
        }
    }
}

bool GraphicsPipeline::BuildVertexInput(RenderPipelineDesc& desc) const {
    if (!stages[0])
        return true;
    const auto& info = stages[0]->info;
    std::array<bool, Maxwell::NumVertexArrays> buffers{};
    for (u32 index = 0; index < Maxwell::NumVertexAttributes; ++index) {
        const auto& attribute = key.state.attributes[index];
        if (attribute.enabled == 0 || !info.loads.Generic(index))
            continue;
        const auto format = MaxwellVertexFormat(attribute.type.Value(), attribute.size.Value());
        if (format.format == 0) {
            LOG_ERROR(Render, "Metal: unsupported vertex format type={} size={:#x}",
                      attribute.type.Value(), attribute.size.Value());
            return false;
        }
        const u32 buffer = attribute.buffer.Value();
        if (buffer >= ArgumentBufferBase) {
            LOG_ERROR(Render, "Metal: vertex buffer {} collides with argument slots", buffer);
            return false;
        }
        const u32 stride = key.state.vertex_strides[buffer];
        // Strides that are not multiples of 4 are repacked (PrepareVertexBuffers).
        if (stride != 0 && attribute.offset.Value() + format.size > stride) {
            LOG_ERROR(Render, "Metal: vertex stride {} unsupported (attribute offset {} size {})",
                      stride, attribute.offset.Value(), format.size);
            return false;
        }
        VertexAttribute out;
        out.location = index;
        out.buffer = buffer;
        out.offset = attribute.offset.Value();
        out.raw_format = format.format;
        out.raw_size = format.size;
        desc.attributes.push_back(out);
        buffers[buffer] = true;
    }
    for (u32 buffer = 0; buffer < Maxwell::NumVertexArrays; ++buffer) {
        if (!buffers[buffer])
            continue;
        VertexBufferLayout layout;
        layout.buffer = buffer;
        layout.stride = Common::AlignUp(key.state.vertex_strides[buffer], 4u);
        layout.per_instance = ((key.state.enabled_divisors >> buffer) & 1) != 0;
        layout.step_rate = std::max<u32>(1, key.state.binding_divisors[buffer]);
        desc.vertex_buffers.push_back(layout);
    }
    return true;
}

const Pipeline* GraphicsPipeline::Variant(const CacheFramebuffer& framebuffer) {
    const u64 signature = FramebufferSignature(framebuffer);
    if (const auto it = variants.find(signature); it != variants.end())
        return it->second.Valid() ? &it->second : nullptr;
    auto& slot = variants[signature];
    try {
        RenderPipelineDesc desc;
        if (stages[0])
            desc.vertex = stages[0]->function;
        if (stages[4])
            desc.fragment = stages[4]->function;
        desc.color_count = framebuffer.NumColorBuffers();
        for (u32 i = 0; i < desc.color_count; ++i) {
            const auto& color = framebuffer.Colors()[i];
            desc.colors[i] = color.Valid() ? color.Format() : PixelFormat::Invalid;
        }
        desc.depth = framebuffer.HasDepth() ? framebuffer.Depth().Format() : PixelFormat::Invalid;
        desc.use_blend_states = true;
        for (u32 i = 0; i < desc.color_count; ++i) {
            const auto& attachment = key.state.attachments[i];
            const auto mask = attachment.Mask();
            BlendState& blend = desc.blend[i];
            blend.write_mask = static_cast<u8>((mask[0] ? 1 : 0) | (mask[1] ? 2 : 0) |
                                               (mask[2] ? 4 : 0) | (mask[3] ? 8 : 0));
            blend.enable = attachment.enable != 0 && !IsIntegerColor(key.state.color_formats[i]) &&
                           desc.colors[i] != PixelFormat::Invalid;
            if (blend.enable) {
                blend.src_rgb = ConvertFactor(attachment.SourceRGBFactor());
                blend.dst_rgb = ConvertFactor(attachment.DestRGBFactor());
                blend.src_alpha = ConvertFactor(attachment.SourceAlphaFactor());
                blend.dst_alpha = ConvertFactor(attachment.DestAlphaFactor());
                blend.op_rgb = ConvertEquation(attachment.EquationRGB());
                blend.op_alpha = ConvertEquation(attachment.EquationAlpha());
            }
        }
        desc.alpha_to_coverage = key.state.alpha_to_coverage_enabled != 0;
        desc.rasterization_enabled = key.state.dynamic_state.rasterize_enable != 0;
        desc.input_topology = ConvertInputTopology(key.state.topology);
        if (!BuildVertexInput(desc)) {
            failed = true;
            return nullptr;
        }
        if (key.state.dynamic_state.logic_op_enable != 0) {
            LOG_WARNING(Render, "Metal has no framebuffer logic ops; ignoring");
        }
        slot = ctx.metal.CreateRenderPipeline(desc);
    } catch (const std::exception& error) {
        LOG_ERROR(Render, "Metal graphics pipeline {:016x} failed: {}", key.Hash(), error.what());
        slot = {};
    }
    return slot.Valid() ? &slot : nullptr;
}

DepthStencilDesc GraphicsPipeline::MakeDepthStencil(bool has_depth, bool has_stencil) const {
    const auto& dynamic = key.state.dynamic_state;
    const auto& regs = maxwell3d->regs;
    DepthStencilDesc desc;
    if (has_depth && dynamic.depth_test_enable != 0) {
        desc.depth_compare = ConvertCompare(dynamic.DepthTestFunc());
        desc.depth_write = dynamic.depth_write_enable != 0;
    }
    if (has_stencil && dynamic.stencil_enable != 0) {
        desc.stencil_enable = true;
        const auto face = [](const auto& packed, u32 read_mask, u32 write_mask) {
            StencilFaceDesc out;
            out.compare = ConvertCompare(packed.TestFunc());
            out.fail = ConvertStencil(packed.ActionStencilFail());
            out.depth_fail = ConvertStencil(packed.ActionDepthFail());
            out.pass = ConvertStencil(packed.ActionDepthPass());
            out.read_mask = read_mask & 0xFF;
            out.write_mask = write_mask & 0xFF;
            return out;
        };
        const bool two_side = regs.stencil_two_side_enable != 0;
        desc.front = face(dynamic.front, regs.stencil_front_func_mask, regs.stencil_front_mask);
        desc.back = face(dynamic.back, two_side ? regs.stencil_back_func_mask : regs.stencil_front_func_mask,
                         two_side ? regs.stencil_back_mask : regs.stencil_front_mask);
    }
    return desc;
}

bool GraphicsPipeline::Bind(Commands& commands, const CacheFramebuffer& framebuffer) {
    if (failed)
        return false;
    const Pipeline* const pipeline = Variant(framebuffer);
    if (!pipeline)
        return false;
    commands.SetRenderPipeline(*pipeline);
    const bool has_depth = framebuffer.HasDepth() && IsDepthFormat(framebuffer.Depth().Format());
    const bool has_stencil = framebuffer.HasDepth() && HasStencil(framebuffer.Depth().Format());
    // Depth-stencil states are cached per pipeline: the runtime lookup builds a string key.
    const DepthStencilDesc depth_desc = MakeDepthStencil(has_depth, has_stencil);
    const DepthStencilState* depth_state = nullptr;
    for (const auto& [desc, state] : depth_states) {
        if (desc == depth_desc) {
            depth_state = &state;
            break;
        }
    }
    if (!depth_state) {
        if (depth_states.size() >= 16)
            depth_states.clear();
        depth_states.emplace_back(depth_desc, ctx.metal.CreateDepthStencilState(depth_desc));
        depth_state = &depth_states.back().second;
    }
    commands.SetDepthStencilState(*depth_state);
    if (stages[0]) {
        commands.SetBytes(Stage::Vertex, PushConstantBuffer, push_constants.Bytes());
        ResourceBinder::Bind(commands, Stage::Vertex, arguments[0]);
    }
    if (stages[4]) {
        commands.SetBytes(Stage::Fragment, PushConstantBuffer, push_constants.Bytes());
        ResourceBinder::Bind(commands, Stage::Fragment, arguments[4]);
    }
    const auto& vertices = ctx.buffer_runtime.VertexBindings();
    for (u32 index = 0; index < uses_vertex_buffer.size(); ++index) {
        if (!uses_vertex_buffer[index])
            continue;
        const auto& binding = uses_repacked[index] ? repacked_vertices[index] : vertices[index];
        if (binding.size == 0 || !binding.buffer.Valid()) {
            LOG_DEBUG(Render, "Metal: draw reads unbound vertex buffer {}", index);
            return false;
        }
        if (binding.offset % 4 != 0) {
            LOG_WARNING(Render, "Metal: vertex buffer {} offset {} is not 4-byte aligned", index,
                        binding.offset);
            return false;
        }
        commands.SetBuffer(Stage::Vertex, index, binding.buffer, binding.offset);
    }
    return true;
}

void GraphicsPipeline::PrepareVertexBuffers() {
    uses_repacked.fill(false);
    const auto& vertices = ctx.buffer_runtime.VertexBindings();
    for (u32 index = 0; index < uses_vertex_buffer.size(); ++index) {
        if (!uses_vertex_buffer[index])
            continue;
        const auto& binding = vertices[index];
        if (binding.size == 0 || !binding.buffer.Valid())
            continue;
        const u32 stride = key.state.vertex_strides[index];
        if (stride % 4 == 0 && binding.offset % 4 == 0)
            continue;
        repacked_vertices[index] = ctx.binder.RepackVertices(ctx.scheduler, binding, stride,
                                                             Common::AlignUp(stride, 4u));
        uses_repacked[index] = true;
    }
}

ComputePipeline::ComputePipeline(PipelineContext context, std::unique_ptr<StageProgram> program_)
    : ctx{context}, program{std::move(program_)} {
    std::ranges::copy(program->info.constant_buffer_used_sizes, uniform_buffer_sizes.begin());
    try {
        pipeline = ctx.metal.CreateComputePipeline(program->function);
    } catch (const std::exception& error) {
        LOG_ERROR(Render, "Metal compute pipeline failed: {}", error.what());
    }
}

void ComputePipeline::Configure(Tegra::Engines::KeplerCompute& kepler_compute,
                                Tegra::MemoryManager& gpu_memory) {
    auto& texture_cache = ctx.texture_cache;
    auto& buffer_cache = ctx.buffer_cache;
    const auto& info = program->info;
    ctx.buffer_runtime.ResetDescriptorBindings();
    buffer_cache.SetComputeUniformBufferState(info.constant_buffer_mask, &uniform_buffer_sizes);
    buffer_cache.UnbindComputeStorageBuffers();
    size_t ssbo_index = 0;
    for (const auto& desc : info.storage_buffers_descriptors) {
        buffer_cache.BindComputeStorageBuffer(ssbo_index, desc.cbuf_index, desc.cbuf_offset,
                                              desc.is_written);
        ++ssbo_index;
    }
    texture_cache.SynchronizeComputeDescriptors();
    views.clear();
    samplers.clear();
    const auto& qmd = kepler_compute.launch_description;
    const auto& cbufs = qmd.const_buffer_config;
    const bool via_header_index = qmd.linked_tsc != 0;
    const auto read = [&](const auto& desc, u32 index) {
        std::pair<u32, u32> handle;
        const GPUVAddr secondary = [&]() -> GPUVAddr {
            if constexpr (requires { desc.secondary_cbuf_index; })
                return cbufs[desc.secondary_cbuf_index].Address();
            return 0;
        }();
        ReadHandleIndex(gpu_memory, cbufs[desc.cbuf_index].Address(), secondary, desc, index,
                        via_header_index, handle);
        return handle;
    };
    for (const auto& desc : info.texture_buffer_descriptors)
        for (u32 i = 0; i < desc.count; ++i)
            views.push_back({.index = read(desc, i).first});
    for (const auto& desc : info.image_buffer_descriptors)
        for (u32 i = 0; i < desc.count; ++i)
            views.push_back({.index = read(desc, i).first});
    for (const auto& desc : info.texture_descriptors) {
        for (u32 i = 0; i < desc.count; ++i) {
            const auto handle = read(desc, i);
            views.push_back({.index = handle.first});
            samplers.push_back(handle.first == 0 ? VideoCommon::NULL_SAMPLER_ID
                                                 : texture_cache.GetComputeSamplerId(handle.second));
        }
    }
    for (const auto& desc : info.image_descriptors)
        for (u32 i = 0; i < desc.count; ++i)
            views.push_back({.index = read(desc, i).first, .blacklist = desc.is_written});
    texture_cache.FillComputeImageViews(std::span(views.data(), views.size()));

    buffer_cache.UnbindComputeTextureBuffers();
    size_t index = 0;
    const auto add_buffer = [&](const auto& desc, bool is_image, bool is_written) {
        for (u32 i = 0; i < desc.count; ++i) {
            auto& image_view = texture_cache.GetImageView(views[index].id);
            buffer_cache.BindComputeTextureBuffer(index, image_view.GpuAddr(),
                                                  image_view.BufferSize(), image_view.format,
                                                  is_written, is_image);
            ++index;
        }
    };
    for (const auto& desc : info.texture_buffer_descriptors)
        add_buffer(desc, false, false);
    for (const auto& desc : info.image_buffer_descriptors)
        add_buffer(desc, true, desc.is_written);
    buffer_cache.UpdateComputeBuffers();
    buffer_cache.BindHostComputeBuffers();

    size_t view = CountDescriptors(info.texture_buffer_descriptors) +
                  CountDescriptors(info.image_buffer_descriptors) +
                  CountDescriptors(info.texture_descriptors);
    for (const auto& desc : info.image_descriptors) {
        for (u32 i = 0; i < desc.count; ++i, ++view) {
            if (desc.is_written)
                texture_cache.MarkModification(texture_cache.GetImageView(views[view].id).image_id);
        }
    }
    StageResources resources;
    resources.buffers = &ctx.buffer_runtime.ComputeBindings();
    resources.texels = ctx.buffer_runtime.TexelBindings();
    resources.views = views;
    resources.samplers = samplers;
    arguments = ctx.binder.Fill(*program, resources);
    push_constants.SetUnscaled();
}

void ComputePipeline::Bind(Commands& commands) {
    commands.BeginCompute(pipeline);
    commands.SetBytes(Stage::Compute, PushConstantBuffer, push_constants.Bytes());
    ResourceBinder::Bind(commands, Stage::Compute, arguments);
}

} // namespace NativeMetal
