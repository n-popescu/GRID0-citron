// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <atomic>
#include <stdexcept>
#include "common/alignment.h"
#include "common/div_ceil.h"
#include "common/logging.h"
#include "video_core/renderer_metal/metal_texture_cache.h"
#include "video_core/surface.h"
#include "video_core/texture_cache/util.h"

namespace NativeMetal {
namespace {
using VideoCommon::ImageViewType;
using Tegra::Texture::TextureFilter;
using Tegra::Texture::TextureMipmapFilter;
using Tegra::Texture::WrapMode;

TextureDimension ViewDimension(Shader::TextureType type) {
    switch (type) {
    case Shader::TextureType::Color1D:
        return TextureDimension::D1;
    case Shader::TextureType::ColorArray1D:
        return TextureDimension::D1Array;
    case Shader::TextureType::Color2D:
    case Shader::TextureType::Color2DRect:
        return TextureDimension::D2;
    case Shader::TextureType::ColorArray2D:
        return TextureDimension::D2Array;
    case Shader::TextureType::Color3D:
        return TextureDimension::D3;
    case Shader::TextureType::ColorCube:
        return TextureDimension::Cube;
    case Shader::TextureType::ColorArrayCube:
        return TextureDimension::CubeArray;
    case Shader::TextureType::Buffer:
        return TextureDimension::Buffer;
    }
    throw std::invalid_argument("Invalid shader texture type");
}

// Texture view types Metal allows for a given allocation type.
bool Compatible(TextureDimension source, TextureDimension view) {
    switch (source) {
    case TextureDimension::D1:
    case TextureDimension::D1Array:
        return view == TextureDimension::D1 || view == TextureDimension::D1Array;
    case TextureDimension::D2:
        return view == TextureDimension::D2 || view == TextureDimension::D2Array;
    case TextureDimension::D2Array:
    case TextureDimension::Cube:
    case TextureDimension::CubeArray:
        return view == TextureDimension::D2 || view == TextureDimension::D2Array ||
               view == TextureDimension::Cube || view == TextureDimension::CubeArray;
    case TextureDimension::D3:
        return view == TextureDimension::D3;
    case TextureDimension::Buffer:
        return false;
    }
    return false;
}

std::array<u8, 4> ConvertSwizzle(const VideoCommon::ImageViewInfo& info) {
    if (info.IsRenderTarget())
        return {2, 3, 4, 5}; // render-target view infos carry a sentinel, not a swizzle
    std::array<u8, 4> result{};
    const auto sources = info.Swizzle();
    for (size_t i = 0; i < 4; ++i) {
        switch (sources[i]) {
        case Tegra::Texture::SwizzleSource::Zero:
            result[i] = 0;
            break;
        case Tegra::Texture::SwizzleSource::OneInt:
        case Tegra::Texture::SwizzleSource::OneFloat:
            result[i] = 1;
            break;
        case Tegra::Texture::SwizzleSource::R:
        case Tegra::Texture::SwizzleSource::G:
        case Tegra::Texture::SwizzleSource::B:
        case Tegra::Texture::SwizzleSource::A:
            result[i] = static_cast<u8>(sources[i]); // 2..5 match the runtime's encoding
            break;
        default:
            result[i] = static_cast<u8>(2 + i); // identity for unknown sources
        }
    }
    return result;
}

TextureDesc LevelDesc(const Texture& texture, u32 level, TextureDimension dimension,
                      PixelFormat format, TextureUsage usage) {
    TextureDesc desc;
    desc.width = std::max(1u, texture.Width() >> level);
    desc.height = std::max(1u, texture.Height() >> level);
    desc.depth = std::max(1u, texture.Depth() >> level);
    desc.levels = 1;
    desc.layers = 1;
    desc.dimension = dimension;
    desc.format = format;
    desc.usage = usage;
    return desc;
}

TextureDimension SingleSliceDimension(TextureDimension dimension) {
    switch (dimension) {
    case TextureDimension::D1:
    case TextureDimension::D1Array:
        return TextureDimension::D1;
    case TextureDimension::D3:
        return TextureDimension::D3;
    default:
        return TextureDimension::D2;
    }
}

// Copies a region; a destination with a different (same-sized) format is written
// through a reinterpreting view so Metal's blit sees matching formats.
void CopyRegion(Runtime& metal, Scheduler& scheduler, Commands& commands, const Texture& src,
                const TextureRegion& region,
                const Texture& dst, u32 level, u32 slice, u32 x, u32 y, u32 z) {
    if (!src.Valid() || !dst.Valid())
        return; // an image whose allocation failed
    if (src.Format() == dst.Format()) {
        commands.CopyTexture(src, region, dst, level, slice, x, y, z);
        return;
    }
    const auto src_format = src.Format();
    const auto dst_format = dst.Format();
    const bool depth = IsDepthFormat(src_format) || HasStencil(src_format) ||
                       IsDepthFormat(dst_format) || HasStencil(dst_format);
    if (depth) {
        LOG_WARNING(Render, "Metal: copy between depth/stencil and other formats ({} -> {}) skipped",
                    static_cast<u32>(src_format), static_cast<u32>(dst_format));
        return;
    }
    if (!CanViewAs(dst_format, src_format)) {
        // Metal cannot view compressed data through uncompressed formats (or vice versa).
        // Copy block data through a buffer instead; extents are in source texels and the
        // destination region covers the same blocks (Vulkan size-compatible copy rules).
        const u32 bytes = BytesPerBlock(src_format);
        if (bytes != BytesPerBlock(dst_format)) {
            LOG_WARNING(Render, "Metal: copy between formats {} and {} of different block sizes "
                                "skipped",
                        static_cast<u32>(src_format), static_cast<u32>(dst_format));
            return;
        }
        // Compressed source extents must cover whole blocks or reach the level edge.
        TextureRegion source = region;
        const u32 src_bw = BlockWidth(src_format);
        const u32 src_bh = BlockHeight(src_format);
        const u32 src_width = std::max(1u, src.Width() >> region.level);
        const u32 src_height = std::max(1u, src.Height() >> region.level);
        if (source.x >= src_width || source.y >= src_height)
            return;
        source.width = std::min(Common::AlignUp(source.width, src_bw), src_width - source.x);
        source.height = std::min(Common::AlignUp(source.height, src_bh), src_height - source.y);
        const u32 blocks_x = Common::DivCeil(source.width, src_bw);
        const u32 blocks_y = Common::DivCeil(source.height, src_bh);
        // Metal buffer transfers here use 256-byte aligned rows (see ValidateTransfer).
        const size_t row = Common::AlignUp(static_cast<size_t>(blocks_x) * bytes, size_t{256});
        const size_t image = row * blocks_y;
        const auto staging = scheduler.Stream(image * source.depth);
        commands.CopyTextureToBuffer(src, source, staging.buffer, staging.offset, row, image);
        const u32 dst_width = std::max(1u, dst.Width() >> level);
        const u32 dst_height = std::max(1u, dst.Height() >> level);
        TextureRegion target;
        target.level = level;
        target.slice = slice;
        target.x = x;
        target.y = y;
        target.z = z;
        target.width = std::min(blocks_x * BlockWidth(dst_format), dst_width - std::min(x, dst_width));
        target.height =
            std::min(blocks_y * BlockHeight(dst_format), dst_height - std::min(y, dst_height));
        target.depth = source.depth;
        if (target.width == 0 || target.height == 0)
            return;
        commands.CopyBufferToTexture(staging.buffer, staging.offset, row, image, dst, target);
        return;
    }
    const auto dimension = SingleSliceDimension(dst.Dimension());
    const auto view = metal.CreateTextureView(
        dst, LevelDesc(dst, level, dimension, src.Format(), TextureUsage::Sample), level,
        dimension == TextureDimension::D3 ? 0 : slice);
    commands.CopyTexture(src, region, view, 0, 0, x, y, z);
}

// Texture-cache operations must never throw: the shared cache would be left half-updated
// (dangling image/view ids). Unsupported work is logged (rate-limited) and dropped instead.
void Unsupported(const char* what) {
    static std::atomic<u32> count{};
    const u32 n = count.fetch_add(1, std::memory_order_relaxed);
    if (n < 64 || (n & (n - 1)) == 0)
        LOG_WARNING(Render, "Metal texture cache: {} is not implemented; skipped (#{})", what,
                    n + 1);
}

void ReportFailure(const char* what, const std::exception& e) {
    static std::atomic<u32> count{};
    const u32 n = count.fetch_add(1, std::memory_order_relaxed);
    if (n < 64 || (n & (n - 1)) == 0)
        LOG_ERROR(Render, "Metal texture cache: {} failed: {} (#{})", what, e.what(), n + 1);
}

// The view format to use for an image: the requested one when Metal can reinterpret,
// otherwise the image's own format (e.g. a depth image sampled through a color view).
PixelFormat ViewFormat(const Texture& image, PixelFormat wanted) {
    return CanViewAs(image.Format(), wanted) ? wanted : image.Format();
}

SamplerAddress Wrap(WrapMode mode, TextureFilter filter) {
    switch (mode) {
    case WrapMode::Wrap:
        return SamplerAddress::Repeat;
    case WrapMode::Mirror:
        return SamplerAddress::MirrorRepeat;
    case WrapMode::ClampToEdge:
        return SamplerAddress::ClampToEdge;
    case WrapMode::Border:
        return SamplerAddress::ClampToBorder;
    case WrapMode::Clamp:
        // GL_CLAMP: blends with the border when filtering linearly.
        return filter == TextureFilter::Linear ? SamplerAddress::ClampToBorder
                                               : SamplerAddress::ClampToEdge;
    case WrapMode::MirrorOnceClampToEdge:
    case WrapMode::MirrorOnceBorder:
    case WrapMode::MirrorOnceClampOGL:
        return SamplerAddress::MirrorClampToEdge;
    }
    return SamplerAddress::Repeat;
}

BorderColor NearestBorder(const std::array<float, 4>& color) {
    if (color[3] < 0.5f)
        return BorderColor::TransparentBlack;
    return (color[0] + color[1] + color[2]) / 3.0f >= 0.5f ? BorderColor::OpaqueWhite
                                                           : BorderColor::OpaqueBlack;
}
} // namespace

namespace {
constexpr std::string_view BLIT_MSL = R"(#include <metal_stdlib>
using namespace metal;
struct BlitParams {
    float4 src_rect; // u0, v0, u1, v1 (normalized; reversed for mirrored blits)
};
struct BlitVaryings {
    float4 position [[position]];
    float2 uv;
};
vertex BlitVaryings citrosis_blit_vs(uint vid [[vertex_id]], constant BlitParams& p [[buffer(0)]]) {
    const float2 t = float2(float((vid << 1) & 2u), float(vid & 2u));
    BlitVaryings out;
    out.position = float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);
    out.uv = mix(p.src_rect.xy, p.src_rect.zw, t);
    return out;
}
static uint2 texel(float2 uv, uint width, uint height) {
    const float2 size = float2(float(width), float(height));
    return uint2(clamp(uv * size, float2(0.0), size - 1.0));
}
fragment float4 citrosis_blit_color(BlitVaryings in [[stage_in]],
                                    texture2d<float> src [[texture(0)]],
                                    sampler s [[sampler(0)]]) {
    return src.sample(s, in.uv, level(0.0));
}
fragment uint4 citrosis_blit_uint(BlitVaryings in [[stage_in]], texture2d<uint> src [[texture(0)]]) {
    return src.read(texel(in.uv, src.get_width(), src.get_height()));
}
fragment int4 citrosis_blit_sint(BlitVaryings in [[stage_in]], texture2d<int> src [[texture(0)]]) {
    return src.read(texel(in.uv, src.get_width(), src.get_height()));
}
fragment float4 citrosis_blit_depth_to_color(BlitVaryings in [[stage_in]],
                                             depth2d<float> src [[texture(0)]],
                                             sampler s [[sampler(0)]]) {
    return float4(src.sample(s, in.uv, level(0.0)), 0.0, 0.0, 1.0);
}
struct DepthOut {
    float depth [[depth(any)]];
};
fragment DepthOut citrosis_blit_depth(BlitVaryings in [[stage_in]], depth2d<float> src [[texture(0)]],
                                      sampler s [[sampler(0)]]) {
    return {src.sample(s, in.uv, level(0.0))};
}
fragment DepthOut citrosis_blit_color_to_depth(BlitVaryings in [[stage_in]],
                                               texture2d<float> src [[texture(0)]],
                                               sampler s [[sampler(0)]]) {
    return {src.sample(s, in.uv, level(0.0)).r};
}
)";
constexpr std::array<const char*, 7> BLIT_ENTRIES{
    "citrosis_blit_vs",    "citrosis_blit_color", "citrosis_blit_uint",
    "citrosis_blit_sint",  "citrosis_blit_depth_to_color", "citrosis_blit_depth",
    "citrosis_blit_color_to_depth"};

enum class FormatClass { Float, Uint, Sint, Depth, Other };
FormatClass ClassOf(PixelFormat format) {
    if (HasStencil(format) && !IsDepthFormat(format))
        return FormatClass::Other; // stencil-only
    if (IsDepthFormat(format))
        return FormatClass::Depth;
    if (BlockWidth(format) != 1 || BlockHeight(format) != 1)
        return FormatClass::Other; // compressed: not renderable
    switch (format) {
    case PixelFormat::R8Uint:
    case PixelFormat::RG8Uint:
    case PixelFormat::RGBA8Uint:
    case PixelFormat::R16Uint:
    case PixelFormat::RG16Uint:
    case PixelFormat::RGBA16Uint:
    case PixelFormat::R32Uint:
    case PixelFormat::RG32Uint:
    case PixelFormat::RGBA32Uint:
    case PixelFormat::RGB10A2Uint:
        return FormatClass::Uint;
    case PixelFormat::R8Sint:
    case PixelFormat::RG8Sint:
    case PixelFormat::RGBA8Sint:
    case PixelFormat::R16Sint:
    case PixelFormat::RG16Sint:
    case PixelFormat::RGBA16Sint:
    case PixelFormat::R32Sint:
    case PixelFormat::RG32Sint:
    case PixelFormat::RGBA32Sint:
        return FormatClass::Sint;
    default:
        return FormatClass::Float;
    }
}
} // namespace

const Function& TextureCacheRuntime::BlitFunction(u32 index) {
    if (!blit_function_built[index]) {
        blit_function_built[index] = true;
        blit_functions[index] = metal.CompileMSL(BLIT_MSL, BLIT_ENTRIES[index],
                                                 index == 0 ? Stage::Vertex : Stage::Fragment);
    }
    return blit_functions[index];
}

bool TextureCacheRuntime::DrawBlit(const Texture& dst, const Texture& src,
                                   const VideoCommon::Region2D& dst_region,
                                   const VideoCommon::Region2D& src_region, bool linear) {
    if (!dst.Valid() || !src.Valid())
        return true; // nothing to draw into or from
    const FormatClass dst_class = ClassOf(dst.Format());
    const FormatClass src_class = ClassOf(src.Format());
    u32 fragment;
    if (dst_class == FormatClass::Depth) {
        if (src_class == FormatClass::Depth)
            fragment = 5;
        else if (src_class == FormatClass::Float)
            fragment = 6;
        else
            return false;
    } else if (dst_class == FormatClass::Float) {
        if (src_class == FormatClass::Float)
            fragment = 1;
        else if (src_class == FormatClass::Depth)
            fragment = 4;
        else
            return false;
    } else if (dst_class == FormatClass::Uint && src_class == FormatClass::Uint) {
        fragment = 2;
    } else if (dst_class == FormatClass::Sint && src_class == FormatClass::Sint) {
        fragment = 3;
    } else {
        return false;
    }
    if ((static_cast<u32>(dst.Usage()) & static_cast<u32>(TextureUsage::RenderTarget)) == 0)
        return false;
    RenderPipelineDesc desc;
    desc.vertex = BlitFunction(0);
    desc.fragment = BlitFunction(fragment);
    const bool depth = dst_class == FormatClass::Depth;
    if (depth) {
        desc.color_count = 0;
        desc.depth = dst.Format();
    } else {
        desc.colors[0] = dst.Format();
        desc.color_count = 1;
    }
    const auto pipeline = metal.CreateRenderPipeline(desc);
    // Destination rectangle (mirroring is expressed by reversed source coordinates).
    const s32 dx0 = std::min(dst_region.start.x, dst_region.end.x);
    const s32 dy0 = std::min(dst_region.start.y, dst_region.end.y);
    const s32 dx1 = std::max(dst_region.start.x, dst_region.end.x);
    const s32 dy1 = std::max(dst_region.start.y, dst_region.end.y);
    const bool flip_x = (dst_region.end.x < dst_region.start.x) !=
                        (src_region.end.x < src_region.start.x);
    const bool flip_y = (dst_region.end.y < dst_region.start.y) !=
                        (src_region.end.y < src_region.start.y);
    const float sw = static_cast<float>(src.Width());
    const float sh = static_cast<float>(src.Height());
    float u0 = std::min(src_region.start.x, src_region.end.x) / sw;
    float u1 = std::max(src_region.start.x, src_region.end.x) / sw;
    float v0 = std::min(src_region.start.y, src_region.end.y) / sh;
    float v1 = std::max(src_region.start.y, src_region.end.y) / sh;
    if (flip_x)
        std::swap(u0, u1);
    if (flip_y)
        std::swap(v0, v1);
    const s32 width = static_cast<s32>(dst.Width());
    const s32 height = static_cast<s32>(dst.Height());
    if (dx1 <= 0 || dy1 <= 0 || dx0 >= width || dy0 >= height || dx1 <= dx0 || dy1 <= dy0)
        return true;
    auto& commands = scheduler.Record();
    if (depth) {
        commands.BeginRender({}, &dst);
    } else {
        const std::array attachment{ColorAttachment{dst, false, {}}};
        commands.BeginRender(attachment);
    }
    commands.SetRenderPipeline(pipeline);
    if (depth) {
        if (!blit_depth_state.Valid()) {
            DepthStencilDesc state;
            state.depth_compare = CompareFunction::Always;
            state.depth_write = true;
            blit_depth_state = metal.CreateDepthStencilState(state);
        }
        commands.SetDepthStencilState(blit_depth_state);
    }
    commands.SetViewport(dx0, dy0, dx1 - dx0, dy1 - dy0);
    const u32 sx = static_cast<u32>(std::max(dx0, 0));
    const u32 sy = static_cast<u32>(std::max(dy0, 0));
    commands.SetScissor(sx, sy, static_cast<u32>(std::min(dx1, width)) - sx,
                        static_cast<u32>(std::min(dy1, height)) - sy);
    const std::array<float, 4> rect{u0, v0, u1, v1};
    commands.SetBytes(Stage::Vertex, 0, std::as_bytes(std::span{rect}));
    commands.SetTexture(Stage::Fragment, 0, src);
    auto& sampler = blit_samplers[linear ? 1 : 0];
    if (!sampler.Identity())
        sampler = metal.CreateSampler(linear);
    commands.SetSampler(Stage::Fragment, 0, sampler);
    commands.Draw(Primitive::Triangles, 0, 3);
    return true;
}

StagingBuffer TextureCacheRuntime::UploadStagingBuffer(size_t size) {
    const auto stream = scheduler.Stream(size);
    return {stream.buffer, stream.offset, {reinterpret_cast<u8*>(stream.mapped.data()), size}};
}

StagingBuffer TextureCacheRuntime::DownloadStagingBuffer(size_t size, bool) {
    // Read by the CPU after the GPU finishes (possibly deferred): a dedicated allocation.
    auto buffer = metal.CreateBuffer(std::max<size_t>(size, 4));
    const auto mapping = buffer.MappedBytes();
    return {buffer, 0, {reinterpret_cast<u8*>(mapping.data()), size}};
}

void TextureCacheRuntime::BlitImage(CacheFramebuffer*, CacheImageView& dst, CacheImageView& src,
                                    const VideoCommon::Region2D& dst_region,
                                    const VideoCommon::Region2D& src_region,
                                    Tegra::Engines::Fermi2D::Filter filter,
                                    Tegra::Engines::Fermi2D::Operation operation) {
    using Operation = Tegra::Engines::Fermi2D::Operation;
    if (operation != Operation::SrcCopy && operation != Operation::SrcCopyAnd)
        return Unsupported("blending/ROP 2D blits");
    if (src.IsBuffer() || dst.IsBuffer())
        return Unsupported("2D blits involving buffer images");
    const s32 width = src_region.end.x - src_region.start.x;
    const s32 height = src_region.end.y - src_region.start.y;
    const bool plain = width > 0 && height > 0 && src_region.start.x >= 0 &&
                       src_region.start.y >= 0 && dst_region.start.x >= 0 &&
                       dst_region.start.y >= 0 && dst_region.end.x - dst_region.start.x == width &&
                       dst_region.end.y - dst_region.start.y == height &&
                       VideoCore::Surface::BytesPerBlock(dst.format) ==
                           VideoCore::Surface::BytesPerBlock(src.format);
    try {
        if (plain) {
            // Unscaled, same-sized formats: a blit-encoder copy.
            const TextureRegion region{static_cast<u32>(src.range.base.level),
                                       static_cast<u32>(src.range.base.layer),
                                       static_cast<u32>(src_region.start.x),
                                       static_cast<u32>(src_region.start.y),
                                       0,
                                       static_cast<u32>(width),
                                       static_cast<u32>(height),
                                       1};
            CopyRegion(metal, scheduler, scheduler.Record(), src.ImageTexture(), region,
                       dst.ImageTexture(), static_cast<u32>(dst.range.base.level),
                       static_cast<u32>(dst.range.base.layer),
                       static_cast<u32>(dst_region.start.x),
                       static_cast<u32>(dst_region.start.y), 0);
            return;
        }
        // Scaled, mirrored or format-converting blits: draw the source into the target.
        if (!DrawBlit(dst.RenderTarget(), src.Handle(Shader::TextureType::Color2D), dst_region,
                      src_region, filter == Tegra::Engines::Fermi2D::Filter::Bilinear))
            Unsupported("2D blits between these format classes");
    } catch (const std::exception& e) {
        ReportFailure("2D blit", e);
    }
}

void TextureCacheRuntime::CopyImage(CacheImage& dst, CacheImage& src,
                                    std::span<const VideoCommon::ImageCopy> copies) {
    if (VideoCore::Surface::BytesPerBlock(dst.info.format) !=
        VideoCore::Surface::BytesPerBlock(src.info.format))
        return Unsupported("image copies between formats of different sizes");
    if (!dst.Handle().Valid() || !src.Handle().Valid())
        return;
    auto& commands = scheduler.Record();
    for (const auto& copy : copies) {
        const auto& from = copy.src_subresource;
        const auto& to = copy.dst_subresource;
        if (from.num_layers != to.num_layers || from.num_layers <= 0 || from.base_level < 0 ||
            to.base_level < 0 || from.base_layer < 0 || to.base_layer < 0 ||
            copy.src_offset.x < 0 || copy.src_offset.y < 0 || copy.src_offset.z < 0 ||
            copy.dst_offset.x < 0 || copy.dst_offset.y < 0 || copy.dst_offset.z < 0)
            continue; // malformed guest copy
        if (!copy.extent.width || !copy.extent.height || !copy.extent.depth)
            continue;
        for (s32 layer = 0; layer < from.num_layers; ++layer) {
            const TextureRegion region{static_cast<u32>(from.base_level),
                                       static_cast<u32>(from.base_layer + layer),
                                       static_cast<u32>(copy.src_offset.x),
                                       static_cast<u32>(copy.src_offset.y),
                                       static_cast<u32>(copy.src_offset.z),
                                       copy.extent.width,
                                       copy.extent.height,
                                       copy.extent.depth};
            try {
                CopyRegion(metal, scheduler, commands, src.Handle(), region, dst.Handle(),
                           static_cast<u32>(to.base_level),
                           static_cast<u32>(to.base_layer + layer),
                           static_cast<u32>(copy.dst_offset.x),
                           static_cast<u32>(copy.dst_offset.y),
                           static_cast<u32>(copy.dst_offset.z));
            } catch (const std::exception& e) {
                ReportFailure("image copy", e);
            }
        }
    }
}

void TextureCacheRuntime::CopyImageMSAA(CacheImage& dst, CacheImage& src,
                                        std::span<const VideoCommon::ImageCopy> copies) {
    // Guest multisampled images are allocated single-sampled, so these are plain copies.
    CopyImage(dst, src, copies);
}

bool TextureCacheRuntime::ShouldReinterpret(CacheImage& dst, CacheImage& src) {
    // Same-sized formats are copied through reinterpreting views; anything else needs
    // a conversion pass that does not exist yet (ReinterpretImage throws).
    return VideoCore::Surface::BytesPerBlock(dst.info.format) !=
           VideoCore::Surface::BytesPerBlock(src.info.format);
}

void TextureCacheRuntime::ReinterpretImage(CacheImage&, CacheImage&,
                                           std::span<const VideoCommon::ImageCopy>) {
    Unsupported("image reinterpretation between differently sized formats");
}

void TextureCacheRuntime::ConvertImage(CacheFramebuffer*, CacheImageView& dst,
                                       CacheImageView& src) {
    // Format conversion through a draw (e.g. depth <-> color copies).
    try {
        const Texture& target = dst.RenderTarget();
        const Texture& source = src.Handle(Shader::TextureType::Color2D);
        if (!target.Valid() || !source.Valid())
            return;
        VideoCommon::Region2D dst_region{{0, 0},
                                         {static_cast<s32>(target.Width()),
                                          static_cast<s32>(target.Height())}};
        VideoCommon::Region2D src_region{{0, 0},
                                         {static_cast<s32>(source.Width()),
                                          static_cast<s32>(source.Height())}};
        if (!DrawBlit(target, source, dst_region, src_region, false))
            Unsupported("render-pass image conversion between these format classes");
    } catch (const std::exception& e) {
        ReportFailure("image conversion", e);
    }
}

void TextureCacheRuntime::AccelerateImageUpload(CacheImage&, const StagingBuffer&,
                                                std::span<const VideoCommon::SwizzleParameters>) {
    Unsupported("accelerated image upload");
}

const Texture& TextureCacheRuntime::NullTexture(TextureDimension dimension) {
    if (dimension == TextureDimension::Buffer)
        throw std::invalid_argument("Null Metal buffer textures come from the buffer cache");
    auto& texture = null_textures[static_cast<size_t>(dimension)];
    if (!texture.Valid()) {
        TextureDesc desc;
        desc.dimension = dimension;
        desc.format = PixelFormat::RGBA8;
        desc.usage = TextureUsage::Sample | TextureUsage::Storage;
        if (dimension == TextureDimension::Cube || dimension == TextureDimension::CubeArray)
            desc.layers = 6;
        texture = metal.CreateTexture(desc);
    }
    return texture;
}

const Texture& TextureCacheRuntime::NullStorageTexture(TextureDimension dimension, bool integer) {
    auto& texture = null_storage_textures[integer ? 1 : 0][static_cast<size_t>(dimension)];
    if (texture.Valid())
        return texture;
    const auto format = integer ? PixelFormat::R32Uint : PixelFormat::RGBA32Float;
    if (dimension == TextureDimension::Buffer) {
        constexpr size_t size = 4096;
        if (!null_texel_buffer_created) {
            null_texel_buffer = metal.CreateBuffer(size);
            const std::vector<std::byte> zeros(size);
            null_texel_buffer.Write(0, zeros);
            null_texel_buffer_created = true;
        }
        texture = metal.CreateBufferTexture(null_texel_buffer, 0,
                                            static_cast<u32>(size / BytesPerBlock(format)), format,
                                            TextureUsage::Sample | TextureUsage::Storage);
        return texture;
    }
    TextureDesc desc;
    desc.dimension = dimension;
    desc.format = format;
    desc.usage = TextureUsage::Sample | TextureUsage::Storage;
    if (dimension == TextureDimension::Cube || dimension == TextureDimension::CubeArray)
        desc.layers = 6;
    texture = metal.CreateTexture(desc);
    return texture;
}

CacheImageView::CacheImageView(TextureCacheRuntime& runtime_,
                               const VideoCommon::ImageViewInfo& info, VideoCommon::ImageId id,
                               CacheImage& image)
    : ImageViewBase{info, image.info, id, image.gpu_addr}, runtime{&runtime_},
      image_texture{image.Handle()}, swizzle{ConvertSwizzle(info)} {}

CacheImageView::CacheImageView(TextureCacheRuntime& runtime_,
                               const VideoCommon::ImageViewInfo& info, VideoCommon::ImageId id,
                               CacheImage& image, const Common::SlotVector<CacheImage>&)
    : CacheImageView{runtime_, info, id, image} {}

CacheImageView::CacheImageView(TextureCacheRuntime& runtime_, const VideoCommon::ImageInfo& info,
                               const VideoCommon::ImageViewInfo& view_info, GPUVAddr addr)
    : ImageViewBase{info, view_info, addr}, runtime{&runtime_},
      buffer_size{VideoCommon::CalculateGuestSizeInBytes(info)} {}

CacheImageView::CacheImageView(TextureCacheRuntime& runtime_,
                               const VideoCommon::NullImageViewParams& params)
    : ImageViewBase{params}, runtime{&runtime_}, null_view{true} {}

const Texture& CacheImageView::Handle(Shader::TextureType texture_type) const {
    const auto index = static_cast<size_t>(texture_type);
    if (view_attempted.at(index))
        return views[index];
    view_attempted[index] = true;
    const auto dimension = ViewDimension(texture_type);
    if (dimension == TextureDimension::Buffer)
        return views[index]; // texel buffers are bound by the buffer cache
    if (null_view) {
        views[index] = runtime->NullTexture(dimension);
        return views[index];
    }
    if (!image_texture.Valid() || !Compatible(image_texture.Dimension(), dimension))
        return views[index]; // empty: the shader asked for an incompatible view type
    const auto level = static_cast<u32>(range.base.level);
    const auto slice = static_cast<u32>(range.base.layer);
    u32 layers = 1;
    switch (dimension) {
    case TextureDimension::D1Array:
    case TextureDimension::D2Array:
        layers = static_cast<u32>(range.extent.layers);
        break;
    case TextureDimension::Cube:
        layers = 6;
        break;
    case TextureDimension::CubeArray:
        layers = static_cast<u32>(range.extent.layers) / 6 * 6;
        break;
    default:
        break;
    }
    if (layers == 0 || slice + layers > image_texture.Layers())
        return views[index];
    if (level >= image_texture.Levels())
        return views[index];
    auto desc = LevelDesc(image_texture, level, dimension,
                          ViewFormat(image_texture, GuestPixelFormat(format)),
                          TextureUsage::Sample);
    desc.levels = std::max(1u, std::min(static_cast<u32>(range.extent.levels),
                                        image_texture.Levels() - level));
    desc.layers = layers;
    try {
        views[index] = runtime->metal.CreateTextureView(image_texture, desc, level, slice, swizzle);
    } catch (const std::exception& e) {
        ReportFailure("sampled view creation", e);
    }
    return views[index];
}

const Texture& CacheImageView::RenderTarget() const {
    if (render_target.Valid() || null_view || !image_texture.Valid())
        return render_target;
    const auto level = static_cast<u32>(range.base.level);
    const auto slice = static_cast<u32>(range.base.layer);
    const auto layers = static_cast<u32>(range.extent.layers);
    auto dimension = SingleSliceDimension(image_texture.Dimension());
    if (layers > 1 && dimension == TextureDimension::D2)
        dimension = TextureDimension::D2Array;
    if (level >= image_texture.Levels() ||
        (static_cast<u32>(image_texture.Usage()) & static_cast<u32>(TextureUsage::RenderTarget)) ==
            0)
        return render_target;
    auto desc = LevelDesc(image_texture, level, dimension,
                          ViewFormat(image_texture, GuestPixelFormat(format)),
                          TextureUsage::RenderTarget);
    if (dimension == TextureDimension::D2Array)
        desc.layers = std::min(layers, image_texture.Layers() - std::min(slice,
                                                                          image_texture.Layers()));
    if (desc.layers == 0)
        return render_target;
    // Attachments must not be swizzled; use the identity mapping.
    try {
        render_target = runtime->metal.CreateTextureView(
            image_texture, desc, level, dimension == TextureDimension::D3 ? 0 : slice);
    } catch (const std::exception& e) {
        ReportFailure("render-target view creation", e);
    }
    return render_target;
}

namespace {
PixelFormat StorageFormat(Shader::ImageFormat format) {
    switch (format) {
    case Shader::ImageFormat::R8_UINT:
        return PixelFormat::R8Uint;
    case Shader::ImageFormat::R8_SINT:
        return PixelFormat::R8Sint;
    case Shader::ImageFormat::R16_UINT:
        return PixelFormat::R16Uint;
    case Shader::ImageFormat::R16_SINT:
        return PixelFormat::R16Sint;
    case Shader::ImageFormat::R32_UINT:
        return PixelFormat::R32Uint;
    case Shader::ImageFormat::R32_SINT:
        return PixelFormat::R32Sint;
    case Shader::ImageFormat::R32_SFLOAT:
        return PixelFormat::R32Float;
    case Shader::ImageFormat::R32G32_UINT:
        return PixelFormat::RG32Uint;
    case Shader::ImageFormat::R32G32_SINT:
        return PixelFormat::RG32Sint;
    case Shader::ImageFormat::R32G32_SFLOAT:
        return PixelFormat::RG32Float;
    case Shader::ImageFormat::R32G32B32A32_UINT:
        return PixelFormat::RGBA32Uint;
    case Shader::ImageFormat::R32G32B32A32_SINT:
        return PixelFormat::RGBA32Sint;
    case Shader::ImageFormat::R32G32B32A32_SFLOAT:
        return PixelFormat::RGBA32Float;
    default:
        return PixelFormat::Invalid;
    }
}

// Raw unsigned format with the same texel size, for integer access to typeless images.
PixelFormat RawUintFormat(u32 bytes) {
    switch (bytes) {
    case 1:
        return PixelFormat::R8Uint;
    case 2:
        return PixelFormat::R16Uint;
    case 4:
        return PixelFormat::R32Uint;
    case 8:
        return PixelFormat::RG32Uint;
    case 16:
        return PixelFormat::RGBA32Uint;
    default:
        return PixelFormat::Invalid;
    }
}

bool IsIntegerFormat(PixelFormat format) {
    switch (format) {
    case PixelFormat::R32Uint:
    case PixelFormat::RGBA8Uint:
    case PixelFormat::RGBA8Sint:
    case PixelFormat::R8Uint:
    case PixelFormat::R8Sint:
    case PixelFormat::RG8Uint:
    case PixelFormat::RG8Sint:
    case PixelFormat::R16Uint:
    case PixelFormat::R16Sint:
    case PixelFormat::RG16Uint:
    case PixelFormat::RG16Sint:
    case PixelFormat::RGBA16Uint:
    case PixelFormat::RGBA16Sint:
    case PixelFormat::R32Sint:
    case PixelFormat::RG32Uint:
    case PixelFormat::RG32Sint:
    case PixelFormat::RGBA32Uint:
    case PixelFormat::RGBA32Sint:
    case PixelFormat::RGB10A2Uint:
        return true;
    default:
        return false;
    }
}
} // namespace

const Texture& CacheImageView::StorageView(Shader::TextureType texture_type,
                                           Shader::ImageFormat image_format,
                                           bool is_integer) const {
    const auto own_format = GuestPixelFormat(format);
    const bool raw_uint = image_format == Shader::ImageFormat::Typeless && is_integer &&
                          !IsIntegerFormat(own_format);
    const size_t format_index = raw_uint ? NUM_STORAGE_FORMATS - 1
                                         : static_cast<size_t>(image_format);
    const size_t index =
        static_cast<size_t>(texture_type) * NUM_STORAGE_FORMATS + format_index;
    if (storage_attempted.at(index))
        return storage_views[index];
    storage_attempted[index] = true;
    if (null_view || !image_texture.Valid())
        return storage_views[index];
    if ((static_cast<u32>(image_texture.Usage()) & static_cast<u32>(TextureUsage::Storage)) == 0)
        return storage_views[index];
    const auto dimension = ViewDimension(texture_type);
    if (dimension == TextureDimension::Buffer || dimension == TextureDimension::Cube ||
        dimension == TextureDimension::CubeArray ||
        !Compatible(image_texture.Dimension(), dimension))
        return storage_views[index];
    PixelFormat view_format = own_format;
    if (raw_uint)
        view_format = RawUintFormat(BytesPerBlock(own_format));
    else if (image_format != Shader::ImageFormat::Typeless)
        view_format = StorageFormat(image_format);
    if (view_format == PixelFormat::Invalid || BlockWidth(own_format) != 1 ||
        BytesPerBlock(view_format) != BytesPerBlock(own_format) ||
        !CanViewAs(image_texture.Format(), view_format)) {
        LOG_DEBUG(Render, "Metal: storage view format {} is incompatible with image format {}",
                  static_cast<u32>(image_format), static_cast<u32>(own_format));
        return storage_views[index];
    }
    const auto level = static_cast<u32>(range.base.level);
    const auto slice = static_cast<u32>(range.base.layer);
    auto desc = LevelDesc(image_texture, level, dimension, view_format, TextureUsage::Storage);
    if (dimension == TextureDimension::D1Array || dimension == TextureDimension::D2Array)
        desc.layers = std::min(static_cast<u32>(range.extent.layers),
                               image_texture.Layers() - slice);
    try {
        storage_views[index] = runtime->metal.CreateTextureView(
            image_texture, desc, level, dimension == TextureDimension::D3 ? 0 : slice);
    } catch (const std::exception& e) {
        ReportFailure("storage view creation", e);
    }
    return storage_views[index];
}

CacheSampler::CacheSampler(TextureCacheRuntime& runtime, const Tegra::Texture::TSCEntry& tsc) {
    SamplerDesc desc;
    desc.mag_filter = tsc.mag_filter == TextureFilter::Linear ? SamplerFilter::Linear
                                                              : SamplerFilter::Nearest;
    desc.min_filter = tsc.min_filter == TextureFilter::Linear ? SamplerFilter::Linear
                                                              : SamplerFilter::Nearest;
    switch (tsc.mipmap_filter) {
    case TextureMipmapFilter::Nearest:
        desc.mip_filter = SamplerMipFilter::Nearest;
        break;
    case TextureMipmapFilter::Linear:
        desc.mip_filter = SamplerMipFilter::Linear;
        break;
    default:
        desc.mip_filter = SamplerMipFilter::None;
        break;
    }
    desc.address = {Wrap(tsc.wrap_u, tsc.mag_filter), Wrap(tsc.wrap_v, tsc.mag_filter),
                    Wrap(tsc.wrap_p, tsc.mag_filter)};
    // Matches the Vulkan backend: no-mip samplers sample level 0 only.
    const bool no_mips = tsc.mipmap_filter == TextureMipmapFilter::None;
    desc.lod_min = no_mips ? 0.0f : std::max(0.0f, tsc.MinLod());
    desc.lod_max = no_mips ? 0.25f : std::max(desc.lod_min, tsc.MaxLod());
    desc.max_anisotropy = static_cast<u32>(std::clamp(tsc.MaxAnisotropy(), 1.0f, 16.0f));
    desc.compare = tsc.depth_compare_enabled != 0;
    desc.compare_function = static_cast<CompareFunction>(static_cast<u32>(tsc.depth_compare_func.Value()));
    // Shadow maps sample opaque white outside their bounds (not shadowed), as on Vulkan.
    desc.border = desc.compare ? BorderColor::OpaqueWhite : NearestBorder(tsc.BorderColor());
    desc.normalized_coordinates = true;
    if (tsc.LodBias() != 0.0f) {
        LOG_DEBUG(Render, "Metal samplers have no LOD bias; ignoring bias {}", tsc.LodBias());
    }
    if (tsc.reduction_filter != Tegra::Texture::SamplerReduction::WeightedAverage) {
        LOG_WARNING(Render, "Metal min/max sampler reduction is not implemented");
    }
    sampler = runtime.metal.CreateSampler(desc);
}

CacheFramebuffer::CacheFramebuffer(TextureCacheRuntime&,
                                   std::span<CacheImageView*, VideoCommon::NUM_RT> color_buffers,
                                   CacheImageView* depth_buffer,
                                   const VideoCommon::RenderTargets& key)
    : width{key.size.width}, height{key.size.height} {
    for (size_t index = 0; index < VideoCommon::NUM_RT; ++index) {
        if (!color_buffers[index])
            continue;
        colors[index] = color_buffers[index]->RenderTarget();
        if (colors[index].Valid())
            num_colors = static_cast<u32>(index + 1);
    }
    if (depth_buffer)
        depth = depth_buffer->RenderTarget();
}
} // namespace NativeMetal
