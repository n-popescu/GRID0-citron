// SPDX-License-Identifier: GPL-3.0-or-later
#include <chrono>
#include <cstring>
#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"
#include "video_core/capture.h"
#include "video_core/framebuffer_config.h"
#include "video_core/gpu.h"
#include "video_core/renderer_metal/renderer_metal.h"
#include "video_core/textures/decoders.h"

namespace NativeMetal {
namespace {
struct GuestFramebufferFormat {
    PixelFormat format;
    u32 bytes_per_pixel;
};

std::optional<GuestFramebufferFormat> FramebufferFormat(Service::android::PixelFormat format) {
    using F = Service::android::PixelFormat;
    switch (format) {
    case F::Rgba8888:
    case F::Rgbx8888:
        return GuestFramebufferFormat{PixelFormat::RGBA8, 4};
    case F::Bgra8888:
        return GuestFramebufferFormat{PixelFormat::BGRA8, 4};
    case F::Rgb565:
        return GuestFramebufferFormat{PixelFormat::B5G6R5, 2};
    default:
        return std::nullopt;
    }
}
} // namespace

RendererMetal::RendererMetal(Core::Frontend::EmuWindow& emu_window,
                             Tegra::MaxwellDeviceMemoryManager& device_memory_, Tegra::GPU& gpu_,
                             std::unique_ptr<Core::Frontend::GraphicsContext> context_)
    : RendererBase(emu_window, std::move(context_)), device_memory{device_memory_}, gpu{gpu_},
      rasterizer{gpu_, device_memory_, metal} {
    LOG_INFO(Render, "Native Metal renderer on {}", metal.DeviceName());
    if (!render_window.GetWindowInfo().render_surface) {
        LOG_WARNING(Render, "Metal: the frontend window has no CAMetalLayer; frames are not "
                            "presented");
    }
}

RendererMetal::~RendererMetal() {
    try {
        rasterizer.Shutdown();
        for (const auto& present : presents)
            if (present)
                present.Wait();
    } catch (const std::exception& error) {
        LOG_ERROR(Render, "Metal renderer shutdown: {}", error.what());
    }
}

std::string RendererMetal::GetDeviceVendor() const {
    return "Apple (" + metal.DeviceName() + ", native Metal)";
}

std::vector<u8> RendererMetal::GetAppletCaptureBuffer() {
    return std::vector<u8>(VideoCore::Capture::TiledSize);
}

std::optional<Texture> RendererMetal::FramebufferTexture(const Tegra::FramebufferConfig& framebuffer) {
    const DAddr address = framebuffer.address + framebuffer.offset;
    used_raw_framebuffer = false;
    if (auto texture = rasterizer.AccelerateDisplay(framebuffer, address, framebuffer.stride))
        return texture;
    used_raw_framebuffer = true;
    const auto format = FramebufferFormat(framebuffer.pixel_format);
    if (!format || framebuffer.width == 0 || framebuffer.height == 0)
        return std::nullopt;
    const u8* const host_ptr = device_memory.GetPointer<u8>(address);
    if (!host_ptr)
        return std::nullopt;
    // Same deswizzle as the Vulkan backend's raw-image path.
    constexpr u32 block_height_log2 = 4;
    const size_t linear_size =
        static_cast<size_t>(framebuffer.width) * framebuffer.height * format->bytes_per_pixel;
    const size_t tiled_size = Tegra::Texture::CalculateSize(true, format->bytes_per_pixel,
                                                            framebuffer.stride, framebuffer.height,
                                                            1, block_height_log2, 0);
    if (!raw_texture.Valid() || raw_texture.Width() != framebuffer.width ||
        raw_texture.Height() != framebuffer.height || raw_texture.Format() != format->format) {
        raw_texture = metal.CreateTexture(framebuffer.width, framebuffer.height, format->format,
                                          TextureUsage::Sample);
    }
    // A fresh staging buffer per frame: the previous one may still be read by the GPU.
    raw_staging = metal.CreateBuffer(linear_size);
    std::vector<u8> linear(linear_size);
    Tegra::Texture::UnswizzleTexture(linear, std::span(host_ptr, tiled_size),
                                     format->bytes_per_pixel, framebuffer.width,
                                     framebuffer.height, 1, block_height_log2, 0);
    raw_staging.Write(0, std::as_bytes(std::span{linear}));
    auto commands = metal.BeginCommands("Citrosis raw framebuffer upload");
    commands.CopyBufferToTexture(raw_staging, 0,
                                 static_cast<size_t>(framebuffer.width) * format->bytes_per_pixel,
                                 raw_texture);
    commands.Submit();
    return raw_texture;
}

void RendererMetal::Composite(std::span<const Tegra::FramebufferConfig> framebuffers) {
    if (framebuffers.empty())
        return;
    SCOPE_EXIT {
        render_window.OnFrameDisplayed();
    };
    try {
        // Guest work recorded so far must be submitted before presentation reads it.
        rasterizer.FlushCommands();
        if (renderer_settings.screenshot_requested.exchange(false)) {
            LOG_WARNING(Render, "Metal: screenshots are not implemented yet");
            if (renderer_settings.screenshot_complete_callback)
                renderer_settings.screenshot_complete_callback(false);
        }
        void* const layer = render_window.GetWindowInfo().render_surface;
        if (layer && render_window.IsShown()) {
            // Only the bottom layer is presented; overlay composition comes later.
            const auto& framebuffer = framebuffers.front();
            if (const auto texture = FramebufferTexture(framebuffer)) {
                const auto& layout = render_window.GetFramebufferLayout();
                const auto crop =
                    Tegra::NormalizeCrop(framebuffer, texture->Width(), texture->Height());
                Runtime::PresentDesc desc;
                desc.drawable_width = layout.width;
                desc.drawable_height = layout.height;
                desc.dst_x = layout.screen.left;
                desc.dst_y = layout.screen.top;
                desc.dst_width = layout.screen.GetWidth();
                desc.dst_height = layout.screen.GetHeight();
                desc.u0 = crop.left;
                desc.v0 = crop.top;
                desc.u1 = crop.right;
                desc.v1 = crop.bottom;
                desc.linear =
                    Settings::values.scaling_filter.GetValue() != Settings::ScalingFilter::NearestNeighbor;
                // Bound the CPU to two frames ahead of the GPU.
                auto& slot = presents[present_index];
                const auto wait_start = std::chrono::steady_clock::now();
                if (slot)
                    slot.Wait();
                rasterizer.AddPresentWait(
                    static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - wait_start)
                                         .count()),
                    used_raw_framebuffer);
                slot = metal.Present(layer, *texture, desc);
                present_index = (present_index + 1) % presents.size();
            }
        }
    } catch (const std::exception& error) {
        LOG_ERROR(Render, "Metal presentation failed: {}", error.what());
    }
    ++m_current_frame;
    gpu.RendererFrameEndNotify();
    rasterizer.TickFrame();
}

} // namespace NativeMetal
