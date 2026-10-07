// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <atomic>
#include <optional>
#include <cstring>
#include <stdexcept>
#include "common/alignment.h"
#include "common/scope_exit.h"
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/control/channel_state.h"
#include "video_core/engines/draw_manager.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/framebuffer_config.h"
#include "video_core/gpu.h"
#include "video_core/host1x/host1x.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_metal/metal_rasterizer.h"
#include "video_core/surface.h"

namespace NativeMetal {
namespace {
[[noreturn]] void Unimplemented(const char* what) {
    throw std::runtime_error(std::string("Metal rasterizer: ") + what + " is not implemented");
}

VideoCore::RasterizerDownloadArea PageArea(DAddr addr, u64 size) {
    return {
        .start_address = Common::AlignDown(addr, Core::DEVICE_PAGESIZE),
        .end_address = Common::AlignUp(addr + size, Core::DEVICE_PAGESIZE),
        .preemtive = true,
    };
}
} // namespace

RasterizerMetal::RasterizerMetal(Tegra::GPU& gpu_,
                                 Tegra::MaxwellDeviceMemoryManager& device_memory_,
                                 Runtime& metal_)
    : gpu{gpu_}, device_memory{device_memory_}, metal{metal_}, scheduler{metal},
      buffer_cache_runtime{metal, scheduler}, buffer_cache{device_memory, buffer_cache_runtime},
      texture_cache_runtime{metal, scheduler}, texture_cache{texture_cache_runtime, device_memory},
      accelerate_dma{buffer_cache, texture_cache},
      binder{metal, texture_cache, texture_cache_runtime},
      pipeline_cache{device_memory,
                     PipelineContext{metal, scheduler, buffer_cache_runtime, buffer_cache,
                                     texture_cache_runtime, texture_cache, binder}},
      query_cache{metal, scheduler, buffer_cache},
      fence_manager{*this, gpu, texture_cache, buffer_cache, query_cache, scheduler} {}

RasterizerMetal::~RasterizerMetal() {
    try {
        Shutdown();
    } catch (const std::exception& error) {
        LOG_ERROR(Render, "Metal rasterizer shutdown failed: {}", error.what());
    }
}

void RasterizerMetal::Shutdown() {
    if (is_shutting_down.exchange(true))
        return;
    std::unique_lock exclusive_guard{shutdown_mutex};
    scheduler.Finish();
}

namespace {
using Maxwell = Tegra::Engines::Maxwell3D::Regs;

bool NeedsGeneratedIndices(Maxwell::PrimitiveTopology topology) {
    switch (topology) {
    case Maxwell::PrimitiveTopology::LineLoop:
    case Maxwell::PrimitiveTopology::TriangleFan:
    case Maxwell::PrimitiveTopology::Polygon:
    case Maxwell::PrimitiveTopology::Quads:
    case Maxwell::PrimitiveTopology::QuadStrip:
        return true;
    default:
        return false;
    }
}

std::optional<Primitive> DirectPrimitive(Maxwell::PrimitiveTopology topology) {
    switch (topology) {
    case Maxwell::PrimitiveTopology::Points:
        return Primitive::Points;
    case Maxwell::PrimitiveTopology::Lines:
        return Primitive::Lines;
    case Maxwell::PrimitiveTopology::LineStrip:
        return Primitive::LineStrip;
    case Maxwell::PrimitiveTopology::Triangles:
        return Primitive::Triangles;
    case Maxwell::PrimitiveTopology::TriangleStrip:
        return Primitive::TriangleStrip;
    default:
        return std::nullopt;
    }
}

Viewport GuestViewport(const Maxwell& regs, size_t index) {
    const auto& src = regs.viewport_transform[index];
    const float x = src.translate_x - src.scale_x;
    const float width = src.scale_x * 2.0f;
    float y = src.translate_y - src.scale_y;
    float height = src.scale_y * 2.0f;
    if (regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft) {
        y += static_cast<f32>(regs.surface_clip.height);
        height = -height;
    }
    if (src.swizzle.y == Maxwell::ViewportSwizzle::NegativeY) {
        y += height;
        height = -height;
    }
    const float reduce_z = regs.depth_mode == Maxwell::DepthMode::MinusOneToOne ? 1.0f : 0.0f;
    Viewport viewport;
    viewport.x = x;
    viewport.y = y;
    viewport.width = width > 0.0f ? width : 1.0f; // Metal rejects non-positive widths
    viewport.height = height != 0.0f ? height : 1.0f;
    viewport.near_depth = std::clamp(src.translate_z - src.scale_z * reduce_z, 0.0f, 1.0f);
    viewport.far_depth = std::clamp(src.translate_z + src.scale_z, 0.0f, 1.0f);
    return viewport;
}

// Errors on the draw path repeat every frame; keep the log readable.
bool ShouldLogError() {
    static std::atomic<u64> count{};
    const u64 n = count.fetch_add(1, std::memory_order_relaxed);
    return n < 32 || n % 4096 == 0;
}

template <typename T>
void LogOnce(const char* what, T&& detail) {
    static std::atomic<bool> logged{};
    if (!logged.exchange(true))
        LOG_WARNING(Render, "Metal: {} ({})", what, detail);
}
} // namespace

void RasterizerMetal::FlushWork() {
    static constexpr u32 DRAWS_TO_DISPATCH = 2048;
    if ((++draw_counter % DRAWS_TO_DISPATCH) == 0)
        scheduler.Flush();
}

Commands* RasterizerMetal::BeginPass(const CacheFramebuffer& framebuffer) {
    auto& commands = scheduler.Record();
    if (commands.RenderPassActive() && commands.RenderPassId() == current_pass_id &&
        current_framebuffer == &framebuffer &&
        (!query_cache.Active() || query_cache.PassHasVisibility(current_pass_id)))
        return &commands;
    query_cache.PreparePass(commands);
    const u32 count = framebuffer.NumColorBuffers();
    std::array<ColorAttachment, VideoCommon::NUM_RT> colors{};
    for (u32 i = 0; i < count; ++i)
        colors[i].texture = framebuffer.Colors()[i];
    const bool any_color = std::any_of(colors.begin(), colors.begin() + count,
                                       [](const ColorAttachment& c) { return c.texture.Valid(); });
    if (!any_color && !framebuffer.HasDepth()) {
        if (framebuffer.Width() == 0 || framebuffer.Height() == 0)
            return nullptr;
        commands.BeginRenderWithoutAttachments(framebuffer.Width(), framebuffer.Height());
    } else {
        commands.BeginRender(std::span(colors.data(), any_color ? count : 0),
                             framebuffer.HasDepth() ? &framebuffer.Depth() : nullptr);
    }
    current_pass_id = commands.RenderPassId();
    current_framebuffer = &framebuffer;
    if (query_cache.Active())
        query_cache.NotePass(current_pass_id);
    return &commands;
}

ScissorRect RasterizerMetal::GuestScissor(size_t index, const CacheFramebuffer& framebuffer) const {
    const auto& regs = maxwell3d->regs;
    const u32 fb_width = std::max(framebuffer.Width(), 1u);
    const u32 fb_height = std::max(framebuffer.Height(), 1u);
    s64 min_x = 0, min_y = 0, max_x = fb_width, max_y = fb_height;
    if (!regs.viewport_scale_offset_enabled) {
        min_x = regs.surface_clip.x;
        min_y = regs.surface_clip.y;
        max_x = min_x + regs.surface_clip.width;
        max_y = min_y + regs.surface_clip.height;
    } else if (regs.scissor_test[index].enable) {
        const auto& src = regs.scissor_test[index];
        const bool lower_left = regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft;
        const s64 clip_height = regs.surface_clip.height;
        min_x = src.min_x;
        max_x = src.max_x;
        min_y = lower_left ? clip_height - src.max_y : src.min_y.Value();
        max_y = lower_left ? clip_height - src.min_y : src.max_y.Value();
    }
    min_x = std::clamp<s64>(min_x, 0, fb_width);
    max_x = std::clamp<s64>(max_x, min_x, fb_width);
    min_y = std::clamp<s64>(min_y, 0, fb_height);
    max_y = std::clamp<s64>(max_y, min_y, fb_height);
    return {static_cast<u32>(min_x), static_cast<u32>(min_y), static_cast<u32>(max_x - min_x),
            static_cast<u32>(max_y - min_y)};
}

bool RasterizerMetal::UpdateDynamicState(Commands& commands, const CacheFramebuffer& framebuffer) {
    const auto& regs = maxwell3d->regs;
    std::array<Viewport, Maxwell::NumViewports> viewports;
    std::array<ScissorRect, Maxwell::NumViewports> scissors;
    for (size_t i = 0; i < viewports.size(); ++i) {
        if (regs.viewport_scale_offset_enabled) {
            viewports[i] = GuestViewport(regs, i);
        } else {
            viewports[i].x = regs.surface_clip.x;
            viewports[i].y = regs.surface_clip.y;
            viewports[i].width = std::max<double>(regs.surface_clip.width, 1.0);
            viewports[i].height = std::max<double>(regs.surface_clip.height, 1.0);
        }
        scissors[i] = GuestScissor(i, framebuffer);
        if (scissors[i].width == 0 || scissors[i].height == 0) {
            if (i == 0)
                return false; // nothing can be rasterized through an empty first scissor
            scissors[i] = {0, 0, 1, 1};
        }
    }
    commands.SetViewports(viewports);
    commands.SetScissors(scissors);

    const auto topology = maxwell3d->draw_manager->GetDrawState().topology;
    const bool is_points_or_lines = topology == Maxwell::PrimitiveTopology::Points ||
                                    topology == Maxwell::PrimitiveTopology::Lines ||
                                    topology == Maxwell::PrimitiveTopology::LineLoop ||
                                    topology == Maxwell::PrimitiveTopology::LineStrip;
    CullMode cull = CullMode::None;
    if (regs.gl_cull_test_enabled) {
        switch (regs.gl_cull_face) {
        case Maxwell::CullFace::Front:
            cull = CullMode::Front;
            break;
        case Maxwell::CullFace::Back:
            cull = CullMode::Back;
            break;
        case Maxwell::CullFace::FrontAndBack:
            if (!is_points_or_lines)
                return false; // every polygon is culled
            break;
        }
    }
    commands.SetCullMode(cull);
    bool clockwise = regs.gl_front_face == Maxwell::FrontFace::ClockWise;
    if (regs.window_origin.flip_y != 0)
        clockwise = !clockwise;
    commands.SetFrontFacing(clockwise ? Winding::Clockwise : Winding::CounterClockwise);

    const bool bias_enabled = topology == Maxwell::PrimitiveTopology::Points
                                  ? regs.polygon_offset_point_enable != 0
                              : is_points_or_lines ? regs.polygon_offset_line_enable != 0
                                                   : regs.polygon_offset_fill_enable != 0;
    // Guest D24 depth is emulated with Depth32Float, where Metal's constant bias unit
    // shrinks with depth. Apply the constant part in D24 UNORM units in the vertex shader
    // (clip-space Z offset, VertexFixupBuffer) and keep the slope part in Metal.
    std::array<float, 4> fixup{};
    if (bias_enabled) {
        const auto zeta = regs.zeta.format;
        const bool is_d24 = regs.zeta_enable != 0 &&
                            (zeta == Tegra::DepthFormat::Z24_UNORM_S8_UINT ||
                             zeta == Tegra::DepthFormat::X8Z24_UNORM ||
                             zeta == Tegra::DepthFormat::S8Z24_UNORM ||
                             zeta == Tegra::DepthFormat::V8Z24_UNORM);
        const float units = regs.depth_bias / 2.0f;
        const float clamp = regs.depth_bias_clamp;
        if (is_d24) {
            float offset = units / 16777216.0f;
            if (clamp > 0.0f)
                offset = std::min(offset, clamp);
            else if (clamp < 0.0f)
                offset = std::max(offset, clamp);
            fixup[0] = offset;
            commands.SetDepthBias(0.0f, regs.slope_scale_depth_bias, clamp);
        } else {
            commands.SetDepthBias(units, regs.slope_scale_depth_bias, clamp);
        }
    } else {
        commands.SetDepthBias(0.0f, 0.0f, 0.0f);
    }
    commands.SetBytes(Stage::Vertex, VertexFixupBuffer, std::as_bytes(std::span{fixup}));
    const auto clip = regs.viewport_clip_control.geometry_clip.Value();
    using GeometryClip = Maxwell::ViewportClipControl::GeometryClip;
    commands.SetDepthClamp(!(clip == GeometryClip::Passthrough || clip == GeometryClip::FrustumXYZ ||
                             clip == GeometryClip::FrustumZ));
    const bool two_side = regs.stencil_two_side_enable != 0;
    commands.SetStencilReference(regs.stencil_front_ref & 0xFF,
                                 (two_side ? regs.stencil_back_ref : regs.stencil_front_ref) & 0xFF);
    commands.SetBlendColor(regs.blend_color.r, regs.blend_color.g, regs.blend_color.b,
                           regs.blend_color.a);
    commands.SetFillLines(regs.polygon_mode_front == Maxwell::PolygonMode::Line);
    return true;
}

template <typename PrePass, typename DrawFunc>
void RasterizerMetal::PrepareDraw(bool is_indexed, PrePass&& pre_pass, DrawFunc&& draw) {
    std::shared_lock shared_guard{shutdown_mutex};
    if (is_shutting_down)
        return;
    SCOPE_EXIT {
        gpu.TickWork();
    };
    FlushWork();
    gpu_memory->FlushCaching();
    ++stat_draws;
    GraphicsPipeline* const pipeline = pipeline_cache.CurrentGraphicsPipeline();
    if (!pipeline) {
        ++stat_draws_not_ready;
        return;
    }
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    try {
        pipeline->SetEngine(maxwell3d, gpu_memory);
        pipeline->Configure(is_indexed);
        const auto& draw_state = maxwell3d->draw_manager->GetDrawState();
        if (!is_indexed && NeedsGeneratedIndices(draw_state.topology) &&
            draw_state.topology != Maxwell::PrimitiveTopology::Quads &&
            draw_state.topology != Maxwell::PrimitiveTopology::QuadStrip) {
            // The buffer cache only generates quad indices; loops, fans and polygons too.
            buffer_cache_runtime.BindQuadIndexBuffer(draw_state.topology,
                                                     draw_state.vertex_buffer.first,
                                                     draw_state.vertex_buffer.count);
        }
        if (!pre_pass())
            return;
        pipeline->PrepareVertexBuffers();
        const CacheFramebuffer* const framebuffer = texture_cache.GetFramebuffer();
        if (!framebuffer)
            return;
        const bool counting = maxwell3d->regs.zpass_pixel_count_enable != 0;
        if (counting)
            query_cache.Activate();
        Commands* const commands = BeginPass(*framebuffer);
        if (!commands || !pipeline->Bind(*commands, *framebuffer) ||
            !UpdateDynamicState(*commands, *framebuffer))
            return;
        if (query_cache.Active())
            query_cache.UpdateDraw(*commands, counting);
        draw(*commands);
    } catch (const std::exception& error) {
        ++stat_draw_errors;
        if (ShouldLogError())
            LOG_ERROR(Render, "Metal draw failed: {}", error.what());
    }
}

void RasterizerMetal::Draw(bool is_indexed, u32 instance_count) {
    PrepareDraw(
        is_indexed, [] { return true; },
        [&](Commands& commands) {
            const auto& draw_state = maxwell3d->draw_manager->GetDrawState();
            const auto topology = draw_state.topology;
            const u32 instances = std::max(instance_count, 1u);
            if (is_indexed || NeedsGeneratedIndices(topology)) {
                const auto& index = buffer_cache_runtime.Index();
                if (index.count == 0 || !index.buffer.Valid())
                    return;
                if (!is_indexed && !DirectPrimitive(topology) && !NeedsGeneratedIndices(topology))
                    return;
                if (is_indexed && !NeedsGeneratedIndices(topology) && !DirectPrimitive(topology)) {
                    LogOnce("unsupported indexed topology", static_cast<u32>(topology));
                    return;
                }
                commands.DrawIndexed(index.primitive, index.buffer, index.index32, index.offset,
                                     index.count, instances,
                                     is_indexed ? static_cast<s32>(draw_state.base_index) : 0,
                                     draw_state.base_instance);
                return;
            }
            const auto primitive = DirectPrimitive(topology);
            if (!primitive) {
                LogOnce("unsupported topology (patches/adjacency)", static_cast<u32>(topology));
                return;
            }
            if (draw_state.vertex_buffer.count == 0)
                return;
            commands.DrawInstanced(*primitive, draw_state.vertex_buffer.first,
                                   draw_state.vertex_buffer.count, instances,
                                   draw_state.base_instance);
        });
}

void RasterizerMetal::DrawIndirect() {
    const auto& params = maxwell3d->draw_manager->GetIndirectParams();
    buffer_cache.SetDrawIndirect(&params);
    size_t draw_count = params.max_draw_counts;
    // Draw-count indirect: Metal has none, so a kernel copies the records and zeroes those
    // past the GPU-side count; all max_draw_counts draws are then issued from the copy.
    StreamAllocation compacted;
    PrepareDraw(
        params.is_indexed,
        [&] {
            if (params.is_byte_count) {
                LogOnce("transform-feedback byte-count draws are not implemented", 0);
                return false;
            }
            const auto topology = maxwell3d->draw_manager->GetDrawState().topology;
            if (!DirectPrimitive(topology)) {
                LogOnce("indirect draws with converted topologies are not implemented",
                        static_cast<u32>(topology));
                return false;
            }
            // 8-bit indices are widened in place (positions unchanged), so firstIndex holds.
            if (params.include_count && draw_count > 0) {
                const auto [count_buffer, count_offset] = buffer_cache.GetDrawIndirectCount();
                const auto [args_buffer, args_offset] = buffer_cache.GetDrawIndirectBuffer();
                const u32 words = params.is_indexed ? 5 : 4;
                const u32 stride = static_cast<u32>(
                    params.stride != 0 ? params.stride : (params.is_indexed ? 20 : 16));
                compacted = scheduler.Stream(draw_count * words * sizeof(u32));
                scheduler.Utilities().CompactIndirect(
                    scheduler.Record(), args_buffer->Handle(), args_offset, stride, words,
                    count_buffer->Handle(), count_offset, static_cast<u32>(draw_count),
                    compacted.buffer, compacted.offset);
            }
            return draw_count > 0;
        },
        [&](Commands& commands) {
            const auto [buffer, offset] = buffer_cache.GetDrawIndirectBuffer();
            const auto primitive =
                *DirectPrimitive(maxwell3d->draw_manager->GetDrawState().topology);
            const bool use_compacted = compacted.buffer.Valid();
            const Buffer& args = use_compacted ? compacted.buffer : buffer->Handle();
            const size_t base = use_compacted ? compacted.offset : offset;
            const size_t stride = use_compacted ? (params.is_indexed ? 20 : 16)
                                  : params.stride != 0 ? params.stride
                                                       : (params.is_indexed ? 20 : 16);
            for (size_t i = 0; i < draw_count; ++i) {
                const size_t at = base + i * stride;
                if (params.is_indexed) {
                    const auto& index = buffer_cache_runtime.Index();
                    commands.DrawIndexedIndirect(primitive, index.buffer, index.index32,
                                                 index.offset, args, at);
                } else {
                    commands.DrawIndirect(primitive, args, at);
                }
            }
        });
    buffer_cache.SetDrawIndirect(nullptr);
}

void RasterizerMetal::DrawTexture() {
    LogOnce("DrawTexture is not implemented; skipped", 0);
}

void RasterizerMetal::DispatchCompute() {
    std::shared_lock shared_guard{shutdown_mutex};
    if (is_shutting_down)
        return;
    FlushWork();
    gpu_memory->FlushCaching();
    ComputePipeline* const pipeline = pipeline_cache.CurrentComputePipeline();
    if (!pipeline)
        return;
    std::scoped_lock lock{texture_cache.mutex, buffer_cache.mutex};
    try {
        pipeline->Configure(*kepler_compute, *gpu_memory);
        const auto& qmd = kepler_compute->launch_description;
        const auto indirect_address = kepler_compute->GetIndirectComputeAddress();
        std::pair<CacheBuffer*, u32> indirect{};
        if (indirect_address) {
            indirect = buffer_cache.ObtainBuffer(*indirect_address, 12,
                                                 VideoCommon::ObtainBufferSynchronize::FullSynchronize,
                                                 VideoCommon::ObtainBufferOperation::DiscardWrite);
        }
        auto& commands = scheduler.Record();
        current_framebuffer = nullptr;
        pipeline->Bind(commands);
        if (indirect.first) {
            commands.DispatchIndirect(indirect.first->Handle(), indirect.second,
                                      pipeline->LocalSize());
            return;
        }
        const std::array<u32, 3> groups{qmd.grid_dim_x, qmd.grid_dim_y, qmd.grid_dim_z};
        if (groups[0] == 0 || groups[1] == 0 || groups[2] == 0)
            return;
        commands.Dispatch(groups, pipeline->LocalSize());
    } catch (const std::exception& error) {
        if (ShouldLogError())
            LOG_ERROR(Render, "Metal compute dispatch failed: {}", error.what());
    }
}

namespace {
constexpr std::string_view CLEAR_MSL = R"(#include <metal_stdlib>
using namespace metal;
struct ClearParams { float4 color; uint4 icolor; float depth; float pad0; float pad1; float pad2; };
struct ClearVertex { float4 position [[position]]; };
vertex ClearVertex citrosis_clear_vs(uint vid [[vertex_id]], constant ClearParams& p [[buffer(0)]]) {
    const float2 uv = float2(float((vid << 1) & 2), float(vid & 2));
    ClearVertex out;
    out.position = float4(uv * 2.0 - 1.0, p.depth, 1.0);
    return out;
}
fragment float4 citrosis_clear_fs(constant ClearParams& p [[buffer(0)]]) { return p.color; }
fragment uint4 citrosis_clear_fs_uint(constant ClearParams& p [[buffer(0)]]) { return p.icolor; }
fragment int4 citrosis_clear_fs_sint(constant ClearParams& p [[buffer(0)]]) {
    return as_type<int4>(p.icolor);
}
)";
struct ClearParams {
    std::array<float, 4> color{};
    std::array<u32, 4> icolor{};
    float depth{};
    std::array<float, 3> pad{};
};
} // namespace

const Function& RasterizerMetal::ClearFunction(Stage stage, u32 kind) {
    static constexpr std::array<std::string_view, 4> entries{
        "citrosis_clear_vs", "citrosis_clear_fs", "citrosis_clear_fs_uint", "citrosis_clear_fs_sint"};
    const u32 index = stage == Stage::Vertex ? 0 : kind;
    if (!clear_function_built[index]) {
        clear_functions[index] = metal.CompileMSL(CLEAR_MSL, entries[index], stage);
        clear_function_built[index] = true;
    }
    return clear_functions[index];
}

void RasterizerMetal::DrawClearColor(const Texture& target, const std::array<double, 4>& value,
                                     u8 write_mask, bool integer, bool signed_integer,
                                     const ScissorRect& scissor) {
    RenderPipelineDesc desc;
    desc.vertex = ClearFunction(Stage::Vertex, 0);
    desc.fragment = ClearFunction(Stage::Fragment, integer ? (signed_integer ? 3 : 2) : 1);
    desc.colors[0] = target.Format();
    desc.color_count = 1;
    desc.use_blend_states = true;
    desc.blend[0].write_mask = write_mask;
    const auto pipeline = metal.CreateRenderPipeline(desc);
    ClearParams params;
    for (size_t i = 0; i < 4; ++i) {
        params.color[i] = static_cast<float>(value[i]);
        params.icolor[i] = signed_integer ? static_cast<u32>(static_cast<s32>(value[i]))
                                          : static_cast<u32>(value[i]);
    }
    auto& commands = scheduler.Record();
    const std::array attachment{ColorAttachment{target, false, {}}};
    commands.BeginRender(attachment);
    commands.SetRenderPipeline(pipeline);
    const auto bytes = std::as_bytes(std::span{&params, 1});
    commands.SetBytes(Stage::Vertex, 0, bytes);
    commands.SetBytes(Stage::Fragment, 0, bytes);
    commands.SetScissor(scissor.x, scissor.y, scissor.width, scissor.height);
    commands.Draw(Primitive::Triangles, 0, 3);
}

void RasterizerMetal::DrawClearDepthStencil(const Texture& target, bool depth, double depth_value,
                                            bool stencil, u32 stencil_value, u32 stencil_mask,
                                            const ScissorRect& scissor) {
    RenderPipelineDesc desc;
    desc.vertex = ClearFunction(Stage::Vertex, 0);
    desc.color_count = 0;
    desc.depth = target.Format();
    const auto pipeline = metal.CreateRenderPipeline(desc);
    DepthStencilDesc state;
    state.depth_compare = CompareFunction::Always;
    state.depth_write = depth;
    if (stencil) {
        state.stencil_enable = true;
        StencilFaceDesc face;
        face.compare = CompareFunction::Always;
        face.fail = face.depth_fail = face.pass = StencilOperation::Replace;
        face.write_mask = stencil_mask & 0xFF;
        state.front = state.back = face;
    }
    ClearParams params;
    params.depth = static_cast<float>(depth_value);
    auto& commands = scheduler.Record();
    commands.BeginRender({}, &target);
    commands.SetRenderPipeline(pipeline);
    commands.SetDepthStencilState(metal.CreateDepthStencilState(state));
    commands.SetStencilReference(stencil_value & 0xFF, stencil_value & 0xFF);
    commands.SetBytes(Stage::Vertex, 0, std::as_bytes(std::span{&params, 1}));
    commands.SetScissor(scissor.x, scissor.y, scissor.width, scissor.height);
    commands.Draw(Primitive::Triangles, 0, 3);
}

void RasterizerMetal::Clear(u32 layer_count) {
    std::shared_lock shared_guard{shutdown_mutex};
    if (is_shutting_down)
        return;
    gpu_memory->FlushCaching();
    auto& regs = maxwell3d->regs;
    const auto& surface = regs.clear_surface;
    const bool use_color = surface.R || surface.G || surface.B || surface.A;
    const bool use_depth = surface.Z;
    const bool use_stencil = surface.S;
    if (!use_color && !use_depth && !use_stencil)
        return;

    std::scoped_lock lock{texture_cache.mutex};
    try {
        texture_cache.UpdateRenderTargets(true);
        const CacheFramebuffer* const framebuffer = texture_cache.GetFramebuffer();
        if (!framebuffer)
            return;
        current_framebuffer = nullptr; // clears begin their own passes
        ScissorRect area{0, 0, framebuffer->Width(), framebuffer->Height()};
        bool scissored = false;
        if (regs.clear_control.use_scissor && regs.scissor_test[0].enable) {
            area = GuestScissor(0, *framebuffer);
            if (area.width == 0 || area.height == 0)
                return; // empty scissor: nothing to clear
            scissored = area.x != 0 || area.y != 0 || area.width != framebuffer->Width() ||
                        area.height != framebuffer->Height();
        }
        const u32 base_layer = surface.layer;
        const u32 layers = std::max(layer_count, 1u);
        if (TryClearInPass(*framebuffer, scissored, base_layer, layers, use_color, use_depth,
                           use_stencil))
            return;
        if (use_color) {
            const u32 rt = surface.RT;
            if (rt < VideoCommon::NUM_RT && framebuffer->Colors()[rt].Valid()) {
                const auto format =
                    VideoCore::Surface::PixelFormatFromRenderTargetFormat(regs.rt[rt].format);
                const bool integer = VideoCore::Surface::IsPixelFormatInteger(format);
                const bool is_signed = VideoCore::Surface::IsPixelFormatSignedInteger(format);
                std::array<double, 4> value{};
                if (!integer) {
                    for (size_t i = 0; i < 4; ++i)
                        value[i] = regs.clear_color[i];
                } else {
                    // Same integer conversion as the Vulkan backend, for identical results.
                    const auto bits = VideoCore::Surface::PixelComponentSizeBitsInteger(format);
                    for (size_t i = 0; i < 4; ++i) {
                        value[i] = is_signed
                                       ? static_cast<double>(static_cast<s32>(
                                             static_cast<f32>(static_cast<s64>(bits - 1) << 1) *
                                             (regs.clear_color[i] - 0.5f)))
                                       : static_cast<double>(static_cast<u32>(
                                             static_cast<f32>(static_cast<u64>(bits) << 1U) *
                                             regs.clear_color[i]));
                    }
                }
                const u8 mask = static_cast<u8>((surface.R ? 1 : 0) | (surface.G ? 2 : 0) |
                                                (surface.B ? 4 : 0) | (surface.A ? 8 : 0));
                for (const auto& target :
                     LayerTargets(framebuffer->Colors()[rt], base_layer, layers)) {
                    if (!scissored && mask == 0xF) {
                        const std::array attachment{ColorAttachment{target, true, value}};
                        scheduler.Record().BeginRender(attachment);
                    } else {
                        DrawClearColor(target, value, mask, integer, is_signed, area);
                    }
                }
            }
        }
        if ((use_depth || use_stencil) && framebuffer->HasDepth()) {
            const auto format = framebuffer->Depth().Format();
            const bool clear_depth = use_depth && IsDepthFormat(format);
            const bool clear_stencil = use_stencil && HasStencil(format);
            if (clear_depth || clear_stencil) {
                const double depth = std::clamp(static_cast<double>(regs.clear_depth), 0.0, 1.0);
                const u32 stencil = static_cast<u32>(regs.clear_stencil) & 0xFF;
                const u32 stencil_mask = regs.stencil_front_mask & 0xFF;
                for (const auto& target : LayerTargets(framebuffer->Depth(), base_layer, layers)) {
                    if (!scissored && (!clear_stencil || stencil_mask == 0xFF)) {
                        scheduler.Record().BeginRender({}, &target, clear_depth, depth,
                                                       clear_stencil, stencil);
                    } else {
                        DrawClearDepthStencil(target, clear_depth, depth, clear_stencil, stencil,
                                              stencil_mask, area);
                    }
                }
            }
        }
    } catch (const std::exception& error) {
        if (ShouldLogError())
            LOG_ERROR(Render, "Metal clear failed: {}", error.what());
    }
}

// Common full clears become the load actions of a render pass over the whole framebuffer,
// which the following draws keep using: no separate clear pass to store and reload.
bool RasterizerMetal::TryClearInPass(const CacheFramebuffer& framebuffer, bool scissored,
                                     u32 base_layer, u32 layers, bool use_color, bool use_depth,
                                     bool use_stencil) {
    if (scissored || base_layer != 0 || layers != 1)
        return false;
    auto& regs = maxwell3d->regs;
    const auto& surface = regs.clear_surface;
    const u32 count = framebuffer.NumColorBuffers();
    std::array<ColorAttachment, VideoCommon::NUM_RT> colors{};
    bool any_color = false;
    for (u32 i = 0; i < count; ++i) {
        colors[i].texture = framebuffer.Colors()[i];
        if (!colors[i].texture.Valid())
            continue;
        if (colors[i].texture.Layers() != 1)
            return false;
        any_color = true;
    }
    if (use_color) {
        const u32 rt = surface.RT;
        if (rt >= count || !colors[rt].texture.Valid())
            return false;
        if (!(surface.R && surface.G && surface.B && surface.A))
            return false; // masked clear: drawn
        const auto format =
            VideoCore::Surface::PixelFormatFromRenderTargetFormat(regs.rt[rt].format);
        if (VideoCore::Surface::IsPixelFormatInteger(format))
            return false; // integer conversion lives in the general path
        colors[rt].clear = true;
        for (size_t c = 0; c < 4; ++c)
            colors[rt].clear_color[c] = regs.clear_color[c];
    }
    const Texture* depth = framebuffer.HasDepth() ? &framebuffer.Depth() : nullptr;
    bool clear_depth = false, clear_stencil = false;
    double depth_value = 0.0;
    u32 stencil_value = 0;
    if (depth) {
        if (depth->Layers() != 1)
            return false;
        clear_depth = use_depth && IsDepthFormat(depth->Format());
        clear_stencil = use_stencil && HasStencil(depth->Format());
        if (clear_stencil && (regs.stencil_front_mask & 0xFF) != 0xFF)
            return false; // partial stencil write mask: drawn
        depth_value = std::clamp(static_cast<double>(regs.clear_depth), 0.0, 1.0);
        stencil_value = static_cast<u32>(regs.clear_stencil) & 0xFF;
    } else if ((use_depth || use_stencil) && !use_color) {
        return true; // nothing to clear
    }
    if (!any_color && !depth)
        return false;
    auto& commands = scheduler.Record();
    query_cache.PreparePass(commands);
    commands.BeginRender(std::span(colors.data(), any_color ? count : 0), depth, clear_depth,
                         depth_value, clear_stencil, stencil_value);
    current_pass_id = commands.RenderPassId();
    current_framebuffer = &framebuffer;
    if (query_cache.Active())
        query_cache.NotePass(current_pass_id);
    return true;
}

std::vector<Texture> RasterizerMetal::LayerTargets(const Texture& target, u32 base_layer,
                                                   u32 layer_count) {
    if (base_layer == 0 && layer_count == 1 && target.Layers() == 1)
        return {target};
    if (target.Dimension() == TextureDimension::D3)
        Unimplemented("clears of 3D render-target slices");
    if (base_layer >= target.Layers() || layer_count > target.Layers() - base_layer)
        throw std::out_of_range("Metal clear layers exceed the render target");
    std::vector<Texture> result;
    result.reserve(layer_count);
    for (u32 layer = 0; layer < layer_count; ++layer) {
        TextureDesc desc;
        desc.width = target.Width();
        desc.height = target.Height();
        desc.dimension = TextureDimension::D2;
        desc.format = target.Format();
        desc.usage = TextureUsage::RenderTarget;
        result.push_back(metal.CreateTextureView(target, desc, 0, base_layer + layer));
    }
    return result;
}

void RasterizerMetal::ResetCounter(VideoCommon::QueryType type) {
    if (type == VideoCommon::QueryType::ZPassPixelCount64) {
        query_cache.Reset();
        return;
    }
    LOG_DEBUG(Render, "Metal: counter reset {} ignored", type);
}

void RasterizerMetal::WritePayload(GPUVAddr gpu_addr, std::span<const u8> bytes) {
    gpu_memory->WriteBlockUnsafe(gpu_addr, bytes.data(), bytes.size());
    if (const auto cpu_addr = gpu_memory->GpuToCpuAddress(gpu_addr)) {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.WriteMemory(*cpu_addr, bytes.size());
    }
}

void RasterizerMetal::Query(GPUVAddr gpu_addr, VideoCommon::QueryType type,
                            VideoCommon::QueryPropertiesFlags flags, u32 payload, u32) {
    if (!gpu_memory)
        return;
    const bool long_query = True(flags & VideoCommon::QueryPropertiesFlags::HasTimeout);
    if (type == VideoCommon::QueryType::ZPassPixelCount64) {
        // Samples passed: resolved from Metal visibility results after the GPU finishes.
        query_cache.Report(*gpu_memory, gpu_addr, long_query, gpu.GetTicks());
        return;
    }
    // Other counters are not emulated: their reports write the payload (as the null
    // backend does).
    if (type != VideoCommon::QueryType::Payload)
        LOG_DEBUG(Render, "Metal: query type {} reported as its payload", type);
    if (long_query) {
        const std::array<u64, 2> report{payload, gpu.GetTicks()};
        WritePayload(gpu_addr, {reinterpret_cast<const u8*>(report.data()), sizeof(report)});
    } else {
        WritePayload(gpu_addr, {reinterpret_cast<const u8*>(&payload), sizeof(payload)});
    }
}

void RasterizerMetal::BindGraphicsUniformBuffer(size_t stage, u32 index, GPUVAddr gpu_addr,
                                                u32 size) {
    std::scoped_lock lock{buffer_cache.mutex};
    buffer_cache.BindGraphicsUniformBuffer(stage, index, gpu_addr, size);
}

void RasterizerMetal::DisableGraphicsUniformBuffer(size_t stage, u32 index) {
    std::scoped_lock lock{buffer_cache.mutex};
    buffer_cache.DisableGraphicsUniformBuffer(stage, index);
}

void RasterizerMetal::FlushAll() {}

void RasterizerMetal::FlushRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    std::shared_lock shared_guard{shutdown_mutex};
    if (is_shutting_down || addr == 0 || size == 0)
        return;
    if (True(which & VideoCommon::CacheType::TextureCache)) {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.DownloadMemory(addr, size);
    }
    if (True(which & VideoCommon::CacheType::BufferCache)) {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.DownloadMemory(addr, size);
    }
}

bool RasterizerMetal::MustFlushRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    std::shared_lock shared_guard{shutdown_mutex};
    if (is_shutting_down)
        return false;
    if (True(which & VideoCommon::CacheType::BufferCache)) {
        std::scoped_lock lock{buffer_cache.mutex};
        if (buffer_cache.IsRegionGpuModified(addr, size))
            return true;
    }
    if (True(which & VideoCommon::CacheType::TextureCache)) {
        std::scoped_lock lock{texture_cache.mutex};
        return texture_cache.IsRegionGpuModified(addr, size);
    }
    return false;
}

VideoCore::RasterizerDownloadArea RasterizerMetal::GetFlushArea(DAddr addr, u64 size) {
    std::shared_lock shared_guard{shutdown_mutex};
    if (!is_shutting_down) {
        std::scoped_lock lock{texture_cache.mutex};
        if (const auto area = texture_cache.GetFlushArea(addr, size))
            return *area;
    }
    return PageArea(addr, size);
}

void RasterizerMetal::InvalidateRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    std::shared_lock shared_guard{shutdown_mutex};
    if (is_shutting_down || addr == 0 || size == 0)
        return;
    if (True(which & VideoCommon::CacheType::TextureCache)) {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.WriteMemory(addr, size);
    }
    if (True(which & VideoCommon::CacheType::BufferCache)) {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.WriteMemory(addr, size);
    }
    if (True(which & VideoCommon::CacheType::ShaderCache))
        pipeline_cache.InvalidateRegion(addr, size);
}

void RasterizerMetal::InnerInvalidation(std::span<const std::pair<DAddr, std::size_t>> sequences) {
    {
        std::scoped_lock lock{texture_cache.mutex};
        for (const auto& [addr, size] : sequences)
            texture_cache.WriteMemory(addr, size);
    }
    {
        std::scoped_lock lock{buffer_cache.mutex};
        for (const auto& [addr, size] : sequences)
            buffer_cache.WriteMemory(addr, size);
    }
    for (const auto& [addr, size] : sequences)
        pipeline_cache.InvalidateRegion(addr, size);
}

bool RasterizerMetal::OnCPUWrite(DAddr addr, u64 size) {
    if (addr == 0 || size == 0)
        return false;
    {
        std::scoped_lock lock{buffer_cache.mutex};
        if (buffer_cache.OnCPUWrite(addr, size))
            return true;
    }
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.WriteMemory(addr, size);
    }
    pipeline_cache.InvalidateRegion(addr, size);
    return false;
}

void RasterizerMetal::OnCacheInvalidation(DAddr addr, u64 size) {
    if (addr == 0 || size == 0)
        return;
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.WriteMemory(addr, size);
    }
    {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.WriteMemory(addr, size);
    }
    pipeline_cache.InvalidateRegion(addr, size);
}

void RasterizerMetal::InvalidateGPUCache() {
    gpu.InvalidateGPUCache();
}

void RasterizerMetal::UnmapMemory(DAddr addr, u64 size) {
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.UnmapMemory(addr, size);
    }
    {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.WriteMemory(addr, size);
    }
    pipeline_cache.OnCacheInvalidation(addr, size);
}

void RasterizerMetal::ModifyGPUMemory(size_t as_id, GPUVAddr addr, u64 size) {
    std::scoped_lock lock{texture_cache.mutex};
    texture_cache.UnmapGPUMemory(as_id, addr, size);
}

// Guest fences go through VideoCommon::FenceManager (as on Vulkan): it commits the
// asynchronous GPU -> guest memory downloads (GPU-written buffers, preemptively downloaded
// render targets, occlusion query results) and writes them on its release thread once the
// GPU has finished, so guest CPU code sees GPU results. Fences with nothing to download
// do not submit.
void RasterizerMetal::SignalFence(std::function<void()>&& func) {
    fence_manager.SignalFence(std::move(func));
}

void RasterizerMetal::SyncOperation(std::function<void()>&& func) {
    fence_manager.SyncOperation(std::move(func));
}

void RasterizerMetal::SignalSyncPoint(u32 value) {
    fence_manager.SignalSyncPoint(value);
}

void RasterizerMetal::SignalReference() {
    fence_manager.SignalReference();
}

void RasterizerMetal::ReleaseFences(bool force) {
    fence_manager.WaitPendingFences(force);
}

void RasterizerMetal::FlushAndInvalidateRegion(DAddr addr, u64 size,
                                               VideoCommon::CacheType which) {
    if (Settings::IsGPULevelExtreme())
        FlushRegion(addr, size, which);
    InvalidateRegion(addr, size, which);
}

// Metal tracks hazards between encoders for the (default hazard-tracked) resources the
// runtime creates, so guest barriers need no extra commands.
void RasterizerMetal::WaitForIdle() {
    // Metal orders the work; the fence manager must still collect GPU-written ranges.
    fence_manager.SignalOrdering();
}
void RasterizerMetal::FragmentBarrier() {}
void RasterizerMetal::TiledCacheBarrier() {}

void RasterizerMetal::FlushCommands() {
    scheduler.Flush();
}

void RasterizerMetal::ReportPerformance() {
    ++stat_frames;
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration<double>(now - stat_start).count();
    if (elapsed < 1.0)
        return;
    const auto sched = scheduler.TakeStats();
    const auto runtime_stats = TakeRuntimeStats();
    const auto binder_stats = binder.TakeStats();
    const auto index_stats = buffer_cache_runtime.TakeIndexStats();
    const double frames = static_cast<double>(std::max<u64>(stat_frames, 1));
    // Warning level so it shows with the default log filter (development diagnostics).
    LOG_WARNING(Render,
             "Metal perf: {:.1f} fps | per frame: {:.0f} draws ({:.0f} not ready, {:.0f} failed), "
             "{:.0f} passes ({:.0f} blit, {:.0f} compute encoders), {:.1f} cmdbufs, {:.1f} GPU waits ({:.2f} ms), GPU {:.2f} ms, "
             "present wait {:.2f} ms | arg writes {:.0f} reuses {:.0f} | stream {:.0f} KB, "
             "new buffers {:.0f}, new textures {:.0f}, stream chunks +{} | raw fb {} | "
             "vertex repack {:.0f} hits {:.0f} misses {:.0f} KB | "
             "index convert {:.0f} hits {:.0f} misses {:.0f} KB | bindings skipped {:.0f}",
             stat_frames / elapsed, stat_draws / frames, stat_draws_not_ready / frames,
             stat_draw_errors / frames, runtime_stats.render_passes / frames,
             runtime_stats.blit_encoders / frames, runtime_stats.compute_encoders / frames,
             runtime_stats.command_buffers / frames, sched.finishes / frames,
             sched.finish_ns / 1e6 / frames, runtime_stats.gpu_nanoseconds / 1e6 / frames,
             stat_present_wait_ns / 1e6 / frames, binder_stats.argument_writes / frames,
             binder_stats.argument_reuses / frames, sched.stream_bytes / 1024.0 / frames,
             runtime_stats.buffers_created / frames, runtime_stats.textures_created / frames,
             sched.stream_chunks, stat_raw_framebuffers, binder_stats.repack_hits / frames,
             binder_stats.repack_misses / frames, binder_stats.repack_bytes / 1024.0 / frames,
             index_stats.hits / frames, index_stats.misses / frames, index_stats.bytes / 1024.0 / frames,
             runtime_stats.bindings_skipped / frames);
    stat_frames = stat_draws = stat_draws_not_ready = stat_draw_errors = 0;
    stat_dispatches = stat_repacks = stat_present_wait_ns = stat_raw_framebuffers = 0;
    stat_start = now;
}

void RasterizerMetal::TickFrame() {
    ReportPerformance();
    fence_manager.TickFrame();
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.TickFrame();
    }
    std::scoped_lock lock{buffer_cache.mutex};
    buffer_cache.TickFrame();
}

bool RasterizerMetal::AccelerateSurfaceCopy(const Tegra::Engines::Fermi2D::Surface& src,
                                            const Tegra::Engines::Fermi2D::Surface& dst,
                                            const Tegra::Engines::Fermi2D::Config& copy_config) {
    std::shared_lock shared_guard{shutdown_mutex};
    if (is_shutting_down)
        return false;
    std::scoped_lock lock{texture_cache.mutex};
    return texture_cache.BlitImage(dst, src, copy_config);
}

Tegra::Engines::AccelerateDMAInterface& RasterizerMetal::AccessAccelerateDMA() {
    return accelerate_dma;
}

void RasterizerMetal::AccelerateInlineToMemory(GPUVAddr address, size_t copy_size,
                                               std::span<const u8> memory) {
    std::shared_lock shared_guard{shutdown_mutex};
    if (is_shutting_down)
        return;
    const auto cpu_addr = gpu_memory->GpuToCpuAddress(address);
    if (!cpu_addr) [[unlikely]] {
        gpu_memory->WriteBlock(address, memory.data(), copy_size);
        return;
    }
    gpu_memory->WriteBlockUnsafe(address, memory.data(), copy_size);
    {
        std::unique_lock<std::recursive_mutex> lock{buffer_cache.mutex};
        if (!buffer_cache.InlineMemory(*cpu_addr, copy_size, memory))
            buffer_cache.WriteMemory(*cpu_addr, copy_size);
    }
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.WriteMemory(*cpu_addr, copy_size);
    }
    pipeline_cache.InvalidateRegion(*cpu_addr, copy_size);
}

std::optional<Texture> RasterizerMetal::AccelerateDisplay(const Tegra::FramebufferConfig& config,
                                                          DAddr framebuffer_addr, u32) {
    if (!framebuffer_addr)
        return std::nullopt;
    std::scoped_lock lock{texture_cache.mutex};
    const auto [image_view, scaled] =
        texture_cache.TryFindFramebufferImageView(config, framebuffer_addr);
    if (!image_view)
        return std::nullopt;
    const auto& texture = image_view->Handle(Shader::TextureType::Color2D);
    if (!texture.Valid())
        return std::nullopt;
    return texture;
}

void RasterizerMetal::LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                                        const VideoCore::DiskResourceLoadCallback& callback) {
    pipeline_cache.LoadDiskResources(title_id, stop_loading, callback);
}

void RasterizerMetal::InitializeChannel(Tegra::Control::ChannelState& channel) {
    CreateChannel(channel);
    {
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        texture_cache.CreateChannel(channel);
        buffer_cache.CreateChannel(channel);
    }
    pipeline_cache.CreateChannel(channel);
    // The register-to-dirty-flag tables are API-independent; the shared caches and the
    // fixed pipeline state rely on them.
    state_tracker.SetupTables(channel);
}

void RasterizerMetal::BindChannel(Tegra::Control::ChannelState& channel) {
    const s32 channel_id = channel.bind_id;
    BindToChannel(channel_id);
    {
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        texture_cache.BindToChannel(channel_id);
        buffer_cache.BindToChannel(channel_id);
    }
    pipeline_cache.BindToChannel(channel_id);
    state_tracker.ChangeChannel(channel);
    state_tracker.InvalidateState();
    current_framebuffer = nullptr;
}

void RasterizerMetal::ReleaseChannel(s32 channel_id) {
    EraseChannel(channel_id);
    {
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        texture_cache.EraseChannel(channel_id);
        buffer_cache.EraseChannel(channel_id);
    }
    pipeline_cache.EraseChannel(channel_id);
}

bool AccelerateDMA::BufferClear(GPUVAddr src_address, u64 amount, u32 value) {
    std::scoped_lock lock{buffer_cache.mutex};
    return buffer_cache.DMAClear(src_address, amount, value);
}

bool AccelerateDMA::BufferCopy(GPUVAddr src_address, GPUVAddr dst_address, u64 amount) {
    std::scoped_lock lock{buffer_cache.mutex};
    return buffer_cache.DMACopy(src_address, dst_address, amount);
}

template <bool IS_IMAGE_UPLOAD>
bool AccelerateDMA::DmaBufferImageCopy(const Tegra::DMA::ImageCopy& copy_info,
                                       const Tegra::DMA::BufferOperand& buffer_operand,
                                       const Tegra::DMA::ImageOperand& image_operand) {
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    const auto image_id = texture_cache.DmaImageId(image_operand, IS_IMAGE_UPLOAD);
    if (image_id == VideoCommon::NULL_IMAGE_ID)
        return false;
    const u32 buffer_size = static_cast<u32>(buffer_operand.pitch * buffer_operand.height);
    static constexpr auto sync_info = VideoCommon::ObtainBufferSynchronize::FullSynchronize;
    const auto post_op = IS_IMAGE_UPLOAD ? VideoCommon::ObtainBufferOperation::DoNothing
                                         : VideoCommon::ObtainBufferOperation::MarkAsWritten;
    const auto [buffer, offset] =
        buffer_cache.ObtainBuffer(buffer_operand.address, buffer_size, sync_info, post_op);
    const auto [image, copy] = texture_cache.DmaBufferImageCopy(
        copy_info, buffer_operand, image_operand, image_id, IS_IMAGE_UPLOAD);
    const std::span copy_span{&copy, 1};
    if constexpr (IS_IMAGE_UPLOAD) {
        texture_cache.PrepareImage(image_id, true, false);
        image->UploadMemory(buffer->Handle(), offset, copy_span);
    } else {
        if (offset % VideoCore::Surface::BytesPerBlock(image->info.format))
            return false;
        texture_cache.DownloadImageIntoBuffer(image, buffer->Handle(), offset, copy_span,
                                              buffer_operand.address, buffer_size);
    }
    return true;
}

bool AccelerateDMA::ImageToBuffer(const Tegra::DMA::ImageCopy& copy_info,
                                  const Tegra::DMA::ImageOperand& image_operand,
                                  const Tegra::DMA::BufferOperand& buffer_operand) {
    return DmaBufferImageCopy<false>(copy_info, buffer_operand, image_operand);
}

bool AccelerateDMA::BufferToImage(const Tegra::DMA::ImageCopy& copy_info,
                                  const Tegra::DMA::BufferOperand& buffer_operand,
                                  const Tegra::DMA::ImageOperand& image_operand) {
    return DmaBufferImageCopy<true>(copy_info, buffer_operand, image_operand);
}

} // namespace NativeMetal
