// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <span>
#include "shader_recompiler/shader_info.h"
#include "video_core/engines/fermi_2d.h"
#include "video_core/renderer_metal/metal_image.h"
#include "video_core/texture_cache/image_view_base.h"
#include "video_core/texture_cache/render_targets.h"
#include "video_core/texture_cache/texture_cache_base.h"
#include "video_core/textures/texture.h"

namespace NativeMetal {
class CacheImageView;
class CacheFramebuffer;

// Adapter between VideoCommon::TextureCache and the native Metal runtime. Operations
// that are not implemented yet throw instead of silently dropping guest work.
class TextureCacheRuntime : public ImageRuntime {
public:
    TextureCacheRuntime(Runtime& metal_, Scheduler& scheduler_) : ImageRuntime{metal_, scheduler_} {}

    void Finish() {
        scheduler.Finish();
    }
    StagingBuffer UploadStagingBuffer(size_t size);
    StagingBuffer DownloadStagingBuffer(size_t size, bool deferred = false);
    void FreeDeferredStagingBuffer(StagingBuffer& buffer) {
        buffer = {};
    }
    void TickFrame() {}
    u64 GetDeviceLocalMemory() const {
        return 0;
    }
    u64 GetDeviceMemoryUsage() const {
        return 0;
    }
    bool CanReportMemoryUsage() const {
        return false;
    }

    void BlitImage(CacheFramebuffer* dst_framebuffer, CacheImageView& dst, CacheImageView& src,
                   const VideoCommon::Region2D& dst_region, const VideoCommon::Region2D& src_region,
                   Tegra::Engines::Fermi2D::Filter filter,
                   Tegra::Engines::Fermi2D::Operation operation);
    void CopyImage(CacheImage& dst, CacheImage& src, std::span<const VideoCommon::ImageCopy> copies);
    void CopyImageMSAA(CacheImage& dst, CacheImage& src,
                       std::span<const VideoCommon::ImageCopy> copies);
    bool ShouldReinterpret(CacheImage& dst, CacheImage& src);
    void ReinterpretImage(CacheImage& dst, CacheImage& src,
                          std::span<const VideoCommon::ImageCopy> copies);
    void ConvertImage(CacheFramebuffer* dst, CacheImageView& dst_view, CacheImageView& src_view);
    bool CanAccelerateImageUpload(CacheImage&) const noexcept {
        return false;
    }
    bool CanUploadMSAA() const noexcept {
        return false;
    }
    void AccelerateImageUpload(CacheImage&, const StagingBuffer&,
                               std::span<const VideoCommon::SwizzleParameters>);
    void InsertUploadMemoryBarrier() {}
    void TransitionImageLayout(CacheImage&) {}
    bool HasBrokenTextureViewFormats() const noexcept {
        return false;
    }
    bool HasNativeBgr() const noexcept {
        return true;
    }
    void BarrierFeedbackLoop() {}

    /// Lazily created placeholder textures for null descriptors, one per view dimension.
    const Texture& NullTexture(TextureDimension dimension);
    /// Placeholder storage textures (and texel buffers, for TextureDimension::Buffer) whose
    /// component class matches integer (R32Uint) or float (RGBA32Float) shader access.
    const Texture& NullStorageTexture(TextureDimension dimension, bool integer);

private:
    /// Draws `src_region` of `src` (sampled) into `dst_region` of `dst` (render target),
    /// scaling, mirroring and converting formats. Returns false if the formats cannot be
    /// converted by a shader (e.g. integer to float).
    bool DrawBlit(const Texture& dst, const Texture& src, const VideoCommon::Region2D& dst_region,
                  const VideoCommon::Region2D& src_region, bool linear);
    const Function& BlitFunction(u32 index);

    std::array<Function, 8> blit_functions;
    std::array<bool, 8> blit_function_built{};
    std::array<Sampler, 2> blit_samplers;
    DepthStencilState blit_depth_state;
    std::array<Texture, 8> null_textures;
    std::array<std::array<Texture, 8>, 2> null_storage_textures;
    Buffer null_texel_buffer;
    bool null_texel_buffer_created{};
};

class CacheImageView : public VideoCommon::ImageViewBase {
public:
    CacheImageView(TextureCacheRuntime&, const VideoCommon::ImageViewInfo&, VideoCommon::ImageId,
                   CacheImage&);
    CacheImageView(TextureCacheRuntime&, const VideoCommon::ImageViewInfo&, VideoCommon::ImageId,
                   CacheImage&, const Common::SlotVector<CacheImage>&);
    /// Buffer (texel) views: the data itself is bound through the buffer cache.
    CacheImageView(TextureCacheRuntime&, const VideoCommon::ImageInfo&,
                   const VideoCommon::ImageViewInfo&, GPUVAddr);
    CacheImageView(TextureCacheRuntime&, const VideoCommon::NullImageViewParams&);

    /// Sampled view for a shader texture type. Empty if the image cannot be viewed that way.
    const Texture& Handle(Shader::TextureType texture_type) const;
    /// Unswizzled single-level view for use as a color or depth attachment.
    const Texture& RenderTarget() const;
    /// Unswizzled single-level storage view, reinterpreted as `image_format` (or as a raw
    /// unsigned format when the shader reads a typeless image as integers). Empty if the
    /// image cannot be used as storage in that format.
    const Texture& StorageView(Shader::TextureType texture_type, Shader::ImageFormat image_format,
                               bool is_integer) const;
    const Texture& ImageTexture() const {
        return image_texture;
    }
    bool IsRescaled() const noexcept {
        return false;
    }
    GPUVAddr GpuAddr() const noexcept {
        return gpu_addr;
    }
    u32 BufferSize() const noexcept {
        return buffer_size;
    }

private:
    TextureCacheRuntime* runtime{};
    Texture image_texture;
    std::array<u8, 4> swizzle{2, 3, 4, 5};
    bool null_view{};
    u32 buffer_size{};
    mutable std::array<Texture, Shader::NUM_TEXTURE_TYPES> views;
    mutable std::array<bool, Shader::NUM_TEXTURE_TYPES> view_attempted{};
    mutable Texture render_target;
    // Index: texture type * (ImageFormat count + 1) + format (the extra slot is typeless-as-uint).
    static constexpr size_t NUM_STORAGE_FORMATS = 15;
    mutable std::array<Texture, Shader::NUM_TEXTURE_TYPES * NUM_STORAGE_FORMATS> storage_views;
    mutable std::array<bool, Shader::NUM_TEXTURE_TYPES * NUM_STORAGE_FORMATS> storage_attempted{};
};

class CacheImageAlloc : public VideoCommon::ImageAllocBase {};

class CacheSampler {
public:
    CacheSampler(TextureCacheRuntime&, const Tegra::Texture::TSCEntry&);
    const Sampler& Handle() const noexcept {
        return sampler;
    }

private:
    Sampler sampler;
};

class CacheFramebuffer {
public:
    CacheFramebuffer(TextureCacheRuntime&, std::span<CacheImageView*, VideoCommon::NUM_RT> colors,
                     CacheImageView* depth, const VideoCommon::RenderTargets& key);
    /// Color attachments by render-target index; unbound indices are empty handles.
    const std::array<Texture, VideoCommon::NUM_RT>& Colors() const noexcept {
        return colors;
    }
    u32 NumColorBuffers() const noexcept {
        return num_colors;
    }
    const Texture& Depth() const noexcept {
        return depth;
    }
    bool HasDepth() const noexcept {
        return depth.Valid();
    }
    u32 Width() const noexcept {
        return width;
    }
    u32 Height() const noexcept {
        return height;
    }
    bool IsRescaled() const noexcept {
        return false;
    }

private:
    std::array<Texture, VideoCommon::NUM_RT> colors;
    Texture depth;
    u32 num_colors{}, width{}, height{};
};

struct TextureCacheParams {
    static constexpr bool ENABLE_VALIDATION = true;
    static constexpr bool FRAMEBUFFER_BLITS = false;
    static constexpr bool HAS_EMULATED_COPIES = false;
    static constexpr bool HAS_DEVICE_MEMORY_INFO = false;
    static constexpr bool IMPLEMENTS_ASYNC_DOWNLOADS = true;

    using Runtime = NativeMetal::TextureCacheRuntime;
    using Image = NativeMetal::CacheImage;
    using ImageAlloc = NativeMetal::CacheImageAlloc;
    using ImageView = NativeMetal::CacheImageView;
    using Sampler = NativeMetal::CacheSampler;
    using Framebuffer = NativeMetal::CacheFramebuffer;
    using AsyncBuffer = NativeMetal::StagingBuffer;
    using BufferType = NativeMetal::Buffer;
};

using TextureCache = VideoCommon::TextureCache<TextureCacheParams>;
} // namespace NativeMetal
