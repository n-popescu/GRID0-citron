// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "video_core/renderer_metal/metal_buffer_cache.h"
#include "video_core/texture_cache/image_base.h"

namespace NativeMetal {
// Native format for a guest format. Guest formats without a Metal equivalent map to a
// placeholder (see GuestFormatSupported) instead of failing.
PixelFormat GuestPixelFormat(VideoCore::Surface::PixelFormat format);
// False when GuestPixelFormat returns a placeholder whose memory layout differs from the
// guest's; such images are allocated but their contents are not transferred.
bool GuestFormatSupported(VideoCore::Surface::PixelFormat format);
class ImageRuntime {
public:
    ImageRuntime(Runtime& metal_, Scheduler& scheduler_) : metal{metal_}, scheduler{scheduler_} {}
    Runtime& metal;
    Scheduler& scheduler;
};
// Uses the shared guest image layout/alias metadata. The shared texture-cache
// controller will own these images once its view/sampler/framebuffer adapter is wired.
class CacheImage : public VideoCommon::ImageBase {
public:
    CacheImage(ImageRuntime&, const VideoCommon::ImageInfo&, GPUVAddr gpu_addr, VAddr cpu_addr);
    explicit CacheImage(VideoCommon::NullImageParams params) : ImageBase{params} {}
    const Texture& Handle() const {
        return texture;
    }
    void UploadMemory(const Buffer&, size_t offset,
                      std::span<const VideoCommon::BufferImageCopy> copies);
    void DownloadMemory(const Buffer&, size_t offset,
                        std::span<const VideoCommon::BufferImageCopy> copies);
    // Staging buffers are fresh allocations no GPU work references, so packing them on the
    // CPU needs no wait; guest (buffer cache) buffers may have pending GPU writes.
    void UploadMemory(const StagingBuffer& map,
                      std::span<const VideoCommon::BufferImageCopy> copies) {
        SafeTransfer(map.buffer, map.offset, copies, true, false, false);
    }
    void DownloadMemory(const StagingBuffer& map,
                        std::span<const VideoCommon::BufferImageCopy> copies) {
        SafeTransfer(map.buffer, map.offset, copies, false, false, false);
    }
    /// Downloads into several buffers at once (texture cache DMA-to-buffer with a deferred
    /// staging copy); completion is tracked by the fence manager.
    void DownloadMemory(std::span<Buffer> buffers, std::span<size_t> offsets,
                        std::span<const VideoCommon::BufferImageCopy> copies) {
        for (size_t i = 0; i < buffers.size() && i < offsets.size(); ++i)
            SafeTransfer(buffers[i], offsets[i], copies, false, true, false);
    }
    bool ScaleUp(bool = false) {
        return false;
    }
    bool ScaleDown(bool = false) {
        return false;
    }
    bool IsRescaled() const {
        return false;
    }

private:
    // synchronize: the buffer may have pending GPU work (CPU packing waits for it first).
    // complete: downloads have finished (and are CPU-visible) when this returns.
    void SafeTransfer(const Buffer&, size_t offset, std::span<const VideoCommon::BufferImageCopy>,
                      bool upload, bool synchronize, bool complete);
    void Transfer(const Buffer&, size_t offset, std::span<const VideoCommon::BufferImageCopy>,
                  bool upload, bool synchronize, bool complete);
    ImageRuntime* runtime{};
    Texture texture;
};
} // namespace NativeMetal
