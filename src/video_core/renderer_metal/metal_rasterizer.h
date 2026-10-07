// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <chrono>
#include <shared_mutex>
#include "video_core/control/channel_state_cache.h"
#include "video_core/engines/maxwell_dma.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_metal/metal_buffer_cache.h"
#include "video_core/renderer_metal/metal_fence_manager.h"
#include "video_core/renderer_metal/metal_query_cache.h"
#include "video_core/renderer_metal/metal_pipeline_cache.h"
#include "video_core/renderer_metal/metal_texture_cache.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"

namespace Tegra {
class GPU;
}

namespace NativeMetal {

class AccelerateDMA final : public Tegra::Engines::AccelerateDMAInterface {
public:
    AccelerateDMA(BufferCache& buffer_cache_, TextureCache& texture_cache_)
        : buffer_cache{buffer_cache_}, texture_cache{texture_cache_} {}
    bool BufferCopy(GPUVAddr src_address, GPUVAddr dst_address, u64 amount) override;
    bool BufferClear(GPUVAddr src_address, u64 amount, u32 value) override;
    bool ImageToBuffer(const Tegra::DMA::ImageCopy& copy_info, const Tegra::DMA::ImageOperand& src,
                       const Tegra::DMA::BufferOperand& dst) override;
    bool BufferToImage(const Tegra::DMA::ImageCopy& copy_info, const Tegra::DMA::BufferOperand& src,
                       const Tegra::DMA::ImageOperand& dst) override;

private:
    template <bool IS_IMAGE_UPLOAD>
    bool DmaBufferImageCopy(const Tegra::DMA::ImageCopy& copy_info,
                            const Tegra::DMA::BufferOperand& buffer_operand,
                            const Tegra::DMA::ImageOperand& image_operand);
    BufferCache& buffer_cache;
    TextureCache& texture_cache;
};

/// Guest rasterizer on the native Metal runtime: shared buffer/texture/shader caches, guest
/// draws (direct and indirect) and compute dispatches through native Metal pipelines,
/// clears, DMA, 2D copies, fences, syncpoints and semaphore reports. Unsupported guest
/// features (geometry/tessellation, transform feedback, host query counters) are logged
/// and skipped rather than aborting emulation.
class RasterizerMetal final : public VideoCore::RasterizerInterface,
                              protected VideoCommon::ChannelSetupCaches<VideoCommon::ChannelInfo> {
public:
    RasterizerMetal(Tegra::GPU& gpu_, Tegra::MaxwellDeviceMemoryManager& device_memory_,
                    Runtime& metal_);
    ~RasterizerMetal() override;

    void Shutdown() override;
    void Draw(bool is_indexed, u32 instance_count) override;
    void DrawIndirect() override;
    void DrawTexture() override;
    void Clear(u32 layer_count) override;
    void DispatchCompute() override;
    void ResetCounter(VideoCommon::QueryType type) override;
    void Query(GPUVAddr gpu_addr, VideoCommon::QueryType type,
               VideoCommon::QueryPropertiesFlags flags, u32 payload, u32 subreport) override;
    void BindGraphicsUniformBuffer(size_t stage, u32 index, GPUVAddr gpu_addr, u32 size) override;
    void DisableGraphicsUniformBuffer(size_t stage, u32 index) override;
    void FlushAll() override;
    void FlushRegion(DAddr addr, u64 size,
                     VideoCommon::CacheType which = VideoCommon::CacheType::All) override;
    bool MustFlushRegion(DAddr addr, u64 size,
                         VideoCommon::CacheType which = VideoCommon::CacheType::All) override;
    VideoCore::RasterizerDownloadArea GetFlushArea(DAddr addr, u64 size) override;
    void InvalidateRegion(DAddr addr, u64 size,
                          VideoCommon::CacheType which = VideoCommon::CacheType::All) override;
    void InnerInvalidation(std::span<const std::pair<DAddr, std::size_t>> sequences) override;
    void OnCacheInvalidation(DAddr addr, u64 size) override;
    bool OnCPUWrite(DAddr addr, u64 size) override;
    void InvalidateGPUCache() override;
    void UnmapMemory(DAddr addr, u64 size) override;
    void ModifyGPUMemory(size_t as_id, GPUVAddr addr, u64 size) override;
    void SignalFence(std::function<void()>&& func) override;
    void SyncOperation(std::function<void()>&& func) override;
    void SignalSyncPoint(u32 value) override;
    void SignalReference() override;
    void ReleaseFences(bool force = true) override;
    void FlushAndInvalidateRegion(
        DAddr addr, u64 size, VideoCommon::CacheType which = VideoCommon::CacheType::All) override;
    void WaitForIdle() override;
    void FragmentBarrier() override;
    void TiledCacheBarrier() override;
    void FlushCommands() override;
    void TickFrame() override;
    bool AccelerateSurfaceCopy(const Tegra::Engines::Fermi2D::Surface& src,
                               const Tegra::Engines::Fermi2D::Surface& dst,
                               const Tegra::Engines::Fermi2D::Config& copy_config) override;
    Tegra::Engines::AccelerateDMAInterface& AccessAccelerateDMA() override;
    void AccelerateInlineToMemory(GPUVAddr address, size_t copy_size,
                                  std::span<const u8> memory) override;
    void LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                           const VideoCore::DiskResourceLoadCallback& callback) override;
    void InitializeChannel(Tegra::Control::ChannelState& channel) override;
    void BindChannel(Tegra::Control::ChannelState& channel) override;
    void ReleaseChannel(s32 channel_id) override;

    /// Presentation time spent waiting for the GPU, folded into the performance log.
    void AddPresentWait(u64 nanoseconds, bool raw_framebuffer) {
        stat_present_wait_ns += nanoseconds;
        stat_raw_framebuffers += raw_framebuffer ? 1 : 0;
    }

    /// Texture to present for a guest framebuffer, if the texture cache owns it.
    std::optional<Texture> AccelerateDisplay(const Tegra::FramebufferConfig& config,
                                             DAddr framebuffer_addr, u32 pixel_stride);

private:
    template <typename PrePass, typename DrawFunc>
    void PrepareDraw(bool is_indexed, PrePass&& pre_pass, DrawFunc&& draw);
    /// Begins (or keeps) the render pass for the framebuffer. Null if it has no area.
    Commands* BeginPass(const CacheFramebuffer& framebuffer);
    /// Sets viewports, scissors and fixed-function dynamic state. False skips the draw.
    bool UpdateDynamicState(Commands& commands, const CacheFramebuffer& framebuffer);
    ScissorRect GuestScissor(size_t index, const CacheFramebuffer& framebuffer) const;
    void FlushWork();
    // Clears that load actions cannot express (scissored, masked or partial stencil).
    void DrawClearColor(const Texture& target, const std::array<double, 4>& value, u8 write_mask,
                        bool integer, bool signed_integer, const ScissorRect& scissor);
    void DrawClearDepthStencil(const Texture& target, bool depth, double depth_value,
                               bool stencil, u32 stencil_value, u32 stencil_mask,
                               const ScissorRect& scissor);
    const Function& ClearFunction(Stage stage, u32 kind);
    std::vector<Texture> LayerTargets(const Texture& target, u32 base_layer, u32 layer_count);
    bool TryClearInPass(const CacheFramebuffer& framebuffer, bool scissored, u32 base_layer,
                        u32 layers, bool use_color, bool use_depth, bool use_stencil);
    void WritePayload(GPUVAddr gpu_addr, std::span<const u8> bytes);

    Tegra::GPU& gpu;
    Tegra::MaxwellDeviceMemoryManager& device_memory;
    Runtime& metal;
    Scheduler scheduler;
    BufferCacheRuntime buffer_cache_runtime;
    BufferCache buffer_cache;
    TextureCacheRuntime texture_cache_runtime;
    TextureCache texture_cache;
    AccelerateDMA accelerate_dma;
    ResourceBinder binder;
    PipelineCache pipeline_cache;
    Vulkan::StateTracker state_tracker;
    QueryCache query_cache;
    FenceManager fence_manager; // last: its release thread uses the caches above
    u64 current_pass_id{};
    const CacheFramebuffer* current_framebuffer{};
    u32 draw_counter{};
    std::array<Function, 4> clear_functions; // vertex, float, uint, sint fragment
    std::array<bool, 4> clear_function_built{};
    std::shared_mutex shutdown_mutex;
    std::atomic<bool> is_shutting_down{};

    // Performance log ("Metal perf:" once per second, from TickFrame).
    void ReportPerformance();
    u64 stat_frames{};
    u64 stat_draws{};
    u64 stat_draws_not_ready{};
    u64 stat_draw_errors{};
    u64 stat_dispatches{};
    u64 stat_repacks{};
    u64 stat_present_wait_ns{};
    u64 stat_raw_framebuffers{};
    std::chrono::steady_clock::time_point stat_start{std::chrono::steady_clock::now()};
};

} // namespace NativeMetal
