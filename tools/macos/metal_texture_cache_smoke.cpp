// SPDX-License-Identifier: GPL-3.0-or-later
// GPU checks for the Metal texture-cache adapter (views, samplers, copies, blits and
// framebuffers with unbound color indices). No game data required.
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
#include "video_core/renderer_metal/metal_texture_cache.h"

namespace {
using namespace NativeMetal;
using VideoCommon::BufferImageCopy;
using GuestFormat = VideoCore::Surface::PixelFormat;

void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

VideoCommon::ImageInfo Info2D(GuestFormat format, u32 width, u32 height, s32 levels, s32 layers) {
    VideoCommon::ImageInfo info;
    info.type = VideoCommon::ImageType::e2D;
    info.format = format;
    info.size = {width, height, 1};
    info.resources = {levels, layers};
    info.layer_stride = width * height * 4;
    return info;
}

void Upload(TextureCacheRuntime& runtime, CacheImage& image, const std::vector<u8>& pixels,
            u32 width, u32 height) {
    auto staging = runtime.UploadStagingBuffer(pixels.size());
    std::memcpy(staging.mapped_span.data(), pixels.data(), pixels.size());
    const std::array copy{BufferImageCopy{0, pixels.size(), width, height, {0, 0, 1}, {0, 0, 0},
                                          {width, height, 1}}};
    image.UploadMemory(staging, copy);
}

std::vector<u8> Download(TextureCacheRuntime& runtime, CacheImage& image, u32 width, u32 height) {
    const size_t size = size_t{width} * height * 4;
    auto staging = runtime.DownloadStagingBuffer(size);
    const std::array copy{
        BufferImageCopy{0, size, width, height, {0, 0, 1}, {0, 0, 0}, {width, height, 1}}};
    image.DownloadMemory(staging, copy);
    runtime.Finish();
    std::vector<u8> result(size);
    staging.buffer.Read(0, std::as_writable_bytes(std::span{result}));
    return result;
}
} // namespace

int main() try {
    Runtime metal;
    Scheduler scheduler{metal};
    TextureCacheRuntime runtime{metal, scheduler};

    std::vector<u8> pattern(8 * 8 * 4);
    for (size_t i = 0; i < pattern.size(); ++i)
        pattern[i] = static_cast<u8>(i * 7 + 3);
    CacheImage color{runtime, Info2D(GuestFormat::A8B8G8R8_UNORM, 8, 8, 2, 1), 0x10000, 0x10000};
    Upload(runtime, color, pattern, 8, 8);
    Check(Download(runtime, color, 8, 8) == pattern, "Staging-buffer upload/download mismatch");

    // Same-format copy of a 4x4 block to an offset.
    CacheImage target{runtime, Info2D(GuestFormat::A8B8G8R8_UNORM, 8, 8, 1, 1), 0x20000, 0x20000};
    Upload(runtime, target, std::vector<u8>(pattern.size(), 0), 8, 8);
    const std::array block{VideoCommon::ImageCopy{{0, 0, 1}, {0, 0, 1}, {0, 0, 0}, {4, 4, 0}, {4, 4, 1}}};
    runtime.CopyImage(target, color, block);
    auto copied = Download(runtime, target, 8, 8);
    for (u32 y = 0; y < 8; ++y)
        for (u32 x = 0; x < 8; ++x)
            for (u32 c = 0; c < 4; ++c) {
                const bool inside = x >= 4 && y >= 4;
                const u8 want = inside ? pattern[((y - 4) * 8 + (x - 4)) * 4 + c] : 0;
                Check(copied[(y * 8 + x) * 4 + c] == want, "Offset image copy is wrong");
            }

    // Copy into a same-sized but different format goes through a reinterpreting view.
    CacheImage integer{runtime, Info2D(GuestFormat::R32_UINT, 8, 8, 1, 1), 0x30000, 0x30000};
    const std::array whole{VideoCommon::ImageCopy{{0, 0, 1}, {0, 0, 1}, {0, 0, 0}, {0, 0, 0}, {8, 8, 1}}};
    runtime.CopyImage(integer, color, whole);
    Check(Download(runtime, integer, 8, 8) == pattern, "Reinterpreting copy changed bits");

    // Views: compatible types are created, incompatible ones are empty.
    const VideoCommon::ImageViewInfo view_info{VideoCommon::ImageViewType::e2D,
                                               GuestFormat::A8B8G8R8_UNORM,
                                               {{0, 0}, {2, 1}}};
    CacheImageView view{runtime, view_info, VideoCommon::ImageId{1}, color};
    Check(view.Handle(Shader::TextureType::Color2D).Valid(), "Missing 2D view");
    Check(view.Handle(Shader::TextureType::Color2D).Levels() == 2, "2D view lost mips");
    Check(view.Handle(Shader::TextureType::ColorArray2D).Valid(), "Missing 2D-array view");
    Check(!view.Handle(Shader::TextureType::Color3D).Valid(), "2D image produced a 3D view");
    Check(!view.Handle(Shader::TextureType::ColorCube).Valid(), "1-layer image produced a cube");
    Check(view.RenderTarget().Valid() && view.RenderTarget().Levels() == 1,
          "Render-target view is wrong");

    CacheImage cube{runtime, Info2D(GuestFormat::A8B8G8R8_UNORM, 4, 4, 1, 6), 0x40000, 0x40000};
    const VideoCommon::ImageViewInfo cube_info{VideoCommon::ImageViewType::Cube,
                                               GuestFormat::A8B8G8R8_UNORM, {{0, 0}, {1, 6}}};
    CacheImageView cube_view{runtime, cube_info, VideoCommon::ImageId{2}, cube};
    Check(cube_view.Handle(Shader::TextureType::ColorCube).Valid(), "Missing cube view");
    Check(cube_view.Handle(Shader::TextureType::ColorArray2D).Layers() == 6,
          "Cube image array view lost layers");

    CacheImageView null_view{runtime, VideoCommon::NullImageViewParams{}};
    Check(null_view.Handle(Shader::TextureType::Color2D).Valid() &&
              null_view.Handle(Shader::TextureType::ColorArrayCube).Valid(),
          "Null views must provide placeholder textures");

    // Samplers across wrap/filter/compare/anisotropy combinations.
    Tegra::Texture::TSCEntry tsc{};
    tsc.wrap_u.Assign(Tegra::Texture::WrapMode::Border);
    tsc.wrap_v.Assign(Tegra::Texture::WrapMode::Clamp);
    tsc.wrap_p.Assign(Tegra::Texture::WrapMode::MirrorOnceClampToEdge);
    tsc.mag_filter.Assign(Tegra::Texture::TextureFilter::Linear);
    tsc.min_filter.Assign(Tegra::Texture::TextureFilter::Linear);
    tsc.mipmap_filter.Assign(Tegra::Texture::TextureMipmapFilter::Linear);
    tsc.max_anisotropy.Assign(4);
    tsc.max_lod_clamp.Assign(256 * 4);
    CacheSampler linear{runtime, tsc};
    tsc.depth_compare_enabled.Assign(1);
    tsc.depth_compare_func.Assign(Tegra::Texture::DepthCompareFunc::LessEqual);
    tsc.mipmap_filter.Assign(Tegra::Texture::TextureMipmapFilter::None);
    CacheSampler shadow{runtime, tsc};
    (void)linear;
    (void)shadow;

    // Framebuffer with an unbound color index 0 and a bound index 1, cleared in one pass.
    CacheImage cleared{runtime, Info2D(GuestFormat::A8B8G8R8_UNORM, 8, 8, 1, 1), 0x50000, 0x50000};
    const VideoCommon::ImageViewInfo rt_info{VideoCommon::ImageViewType::e2D,
                                             GuestFormat::A8B8G8R8_UNORM, {{0, 0}, {1, 1}}};
    CacheImageView rt_view{runtime, rt_info, VideoCommon::ImageId{3}, cleared};
    std::array<CacheImageView*, VideoCommon::NUM_RT> buffers{};
    buffers[1] = &rt_view;
    VideoCommon::RenderTargets key;
    key.size = {8, 8};
    CacheFramebuffer framebuffer{runtime, buffers, nullptr, key};
    Check(framebuffer.NumColorBuffers() == 2 && !framebuffer.Colors()[0].Valid() &&
              framebuffer.Colors()[1].Valid(),
          "Framebuffer color mapping is wrong");
    std::array<ColorAttachment, 2> attachments{};
    attachments[1] = {framebuffer.Colors()[1], true, {1.0, 0.0, 0.0, 1.0}};
    scheduler.Record().BeginRender(attachments);
    auto red = Download(runtime, cleared, 8, 8);
    for (size_t i = 0; i < red.size(); i += 4)
        Check(red[i] == 255 && red[i + 1] == 0 && red[i + 2] == 0 && red[i + 3] == 255,
              "Clear through a gapped framebuffer failed");

    // Unscaled 2D blit through views copies a region.
    runtime.BlitImage(&framebuffer, rt_view, view, {{2, 2}, {6, 6}}, {{0, 0}, {4, 4}},
                      Tegra::Engines::Fermi2D::Filter::Point,
                      Tegra::Engines::Fermi2D::Operation::SrcCopy);
    auto blitted = Download(runtime, cleared, 8, 8);
    for (u32 y = 2; y < 6; ++y)
        for (u32 x = 2; x < 6; ++x)
            Check(std::memcmp(&blitted[(y * 8 + x) * 4], &pattern[((y - 2) * 8 + (x - 2)) * 4],
                              4) == 0,
                  "2D blit copied the wrong texels");
    Check(blitted[0] == 255, "2D blit touched texels outside the region");

    bool rejected = false;
    try {
        runtime.BlitImage(&framebuffer, rt_view, view, {{0, 0}, {8, 8}}, {{0, 0}, {4, 4}},
                          Tegra::Engines::Fermi2D::Filter::Bilinear,
                          Tegra::Engines::Fermi2D::Operation::SrcCopy);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    Check(rejected, "Scaled blits must fail explicitly until implemented");
    std::cout << "PASS: Metal texture cache adapter: staging transfers, offset and "
                 "reinterpreting copies, view types, cube/null views, samplers, gapped "
                 "framebuffer clear and unscaled blits\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
