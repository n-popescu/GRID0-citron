// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <memory>
#include <string>
#include "video_core/renderer_base.h"
#include "video_core/renderer_metal/metal_rasterizer.h"

namespace NativeMetal {

/// Native Metal renderer: guest rendering through RasterizerMetal and presentation into the
/// frontend's CAMetalLayer. No Vulkan or MoltenVK objects are involved.
class RendererMetal final : public VideoCore::RendererBase {
public:
    RendererMetal(Core::Frontend::EmuWindow& emu_window,
                  Tegra::MaxwellDeviceMemoryManager& device_memory, Tegra::GPU& gpu,
                  std::unique_ptr<Core::Frontend::GraphicsContext> context);
    ~RendererMetal() override;

    void Composite(std::span<const Tegra::FramebufferConfig> framebuffers) override;
    std::vector<u8> GetAppletCaptureBuffer() override;
    VideoCore::RasterizerInterface* ReadRasterizer() override {
        return &rasterizer;
    }
    std::string GetDeviceVendor() const override;

private:
    /// Texture holding the guest framebuffer: the texture cache's image when it owns it,
    /// otherwise a CPU-deswizzled upload of guest memory.
    std::optional<Texture> FramebufferTexture(const Tegra::FramebufferConfig& framebuffer);

    Tegra::MaxwellDeviceMemoryManager& device_memory;
    Tegra::GPU& gpu;
    Runtime metal;
    RasterizerMetal rasterizer;
    Texture raw_texture;
    Buffer raw_staging;
    std::array<Submission, 2> presents;
    size_t present_index{};
    bool used_raw_framebuffer{};
};

} // namespace NativeMetal
