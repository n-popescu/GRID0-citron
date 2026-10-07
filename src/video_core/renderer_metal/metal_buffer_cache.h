// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <unordered_map>
#include "video_core/buffer_cache/buffer_cache_base.h"
#include "video_core/buffer_cache/memory_tracker_base.h"
#include "video_core/buffer_cache/usage_tracker.h"
#include "video_core/renderer_metal/metal_scheduler.h"

namespace NativeMetal {
class BufferCacheRuntime;
class CacheBuffer : public VideoCommon::BufferBase {
public:
    CacheBuffer(BufferCacheRuntime&, VideoCommon::NullBufferParams);
    CacheBuffer(BufferCacheRuntime&, VAddr address, u64 size);
    operator const Buffer&() const {
        return handle;
    }
    const Buffer& Handle() const {
        return handle;
    }
    void MarkUsage(u64 offset, u64 size) {
        if (!size)
            return;
        const auto begin = Common::AlignDown(offset, u64{64});
        usage.Track(begin, Common::AlignUp(offset + size, u64{64}) - begin);
    }
    bool IsRegionUsed(u64 offset, u64 size) const {
        if (!size)
            return false;
        const auto begin = Common::AlignDown(offset, u64{64});
        return usage.IsUsed(begin, Common::AlignUp(offset + size, u64{64}) - begin);
    }
    void ResetUsageTracking() {
        usage.Reset();
    }

private:
    Buffer handle;
    VideoCommon::UsageTracker usage;
};
struct StagingBuffer {
    Buffer buffer;
    size_t offset{};
    std::span<u8> mapped_span;
};
struct BufferBinding {
    Buffer buffer;
    u64 offset{}, size{};
    bool written{};
};
struct VertexBinding : BufferBinding {
    u32 stride{};
};
struct IndexBinding : BufferBinding {
    Primitive primitive{Primitive::Triangles};
    bool index32{};
    u32 count{};
};
struct TexelBufferBinding : BufferBinding {
    VideoCore::Surface::PixelFormat format{};
    bool image{};
};
struct StageBufferBindings {
    std::vector<BufferBinding> uniforms, storage;
};
class BufferCacheRuntime {
    friend CacheBuffer;
    using Maxwell = Tegra::Engines::Maxwell3D::Regs;

public:
    BufferCacheRuntime(Runtime& runtime_, Scheduler& scheduler_)
        : runtime{runtime_}, scheduler{scheduler_} {}
    void TickFrame(Common::SlotVector<CacheBuffer>&);
    void Finish() {
        scheduler.Finish();
    }
    bool CanReportMemoryUsage() const {
        return false;
    }
    u64 GetDeviceLocalMemory() const {
        return 0;
    }
    u64 GetDeviceMemoryUsage() const {
        return 0;
    }
    u32 GetStorageBufferAlignment() const {
        return 16;
    }
    StagingBuffer UploadStagingBuffer(size_t size);
    StagingBuffer DownloadStagingBuffer(size_t size, bool deferred = false);
    void FreeDeferredStagingBuffer(StagingBuffer& buffer) {
        buffer = {};
    }
    bool CanReorderUpload(const CacheBuffer&, std::span<const VideoCommon::BufferCopy>) const {
        return false;
    }
    void PreCopyBarrier() {}
    void PostCopyBarrier() {}
    void CopyBuffer(const Buffer& dst, const Buffer& src,
                    std::span<const VideoCommon::BufferCopy> copies, bool barrier,
                    bool reorder = false);
    void ClearBuffer(const Buffer& dst, u32 offset, size_t size, u32 value);
    void BindIndexBuffer(Maxwell::PrimitiveTopology topology, Maxwell::IndexFormat format,
                         u32 first, u32 count, const Buffer& buffer, u32 offset, u32 size);
    void BindQuadIndexBuffer(Maxwell::PrimitiveTopology topology, u32 first, u32 count);
    void BindVertexBuffers(VideoCommon::HostBindings<CacheBuffer>&);
    void BindTransformFeedbackBuffers(VideoCommon::HostBindings<CacheBuffer>&);
    std::span<u8> BindMappedUniformBuffer(size_t stage, u32 binding, u32 size);
    void BindUniformBuffer(size_t stage, u32 binding, const Buffer&, u32 offset, u32 size);
    void BindStorageBuffer(size_t stage, u32 binding, const Buffer&, u32 offset, u32 size,
                           bool written);
    void BindComputeUniformBuffer(u32 binding, const Buffer&, u32 offset, u32 size);
    void BindComputeStorageBuffer(u32 binding, const Buffer&, u32 offset, u32 size, bool written);
    void BindTextureBuffer(const CacheBuffer&, u32 offset, u32 size,
                           VideoCore::Surface::PixelFormat);
    void BindImageBuffer(const CacheBuffer&, u32 offset, u32 size, VideoCore::Surface::PixelFormat);
    void ResetDescriptorBindings();
    const StageBufferBindings& GraphicsBindings(size_t stage) const {
        return graphics.at(stage);
    }
    const StageBufferBindings& ComputeBindings() const {
        return compute;
    }
    const auto& VertexBindings() const {
        return vertices;
    }
    const IndexBinding& Index() const {
        return indices;
    }
    const auto& TexelBindings() const {
        return texels;
    }

    struct IndexStats {
        u64 hits{}, misses{}, bytes{};
    };
    IndexStats TakeIndexStats() {
        const auto result = index_stats;
        index_stats = {};
        return result;
    }

private:
    struct IndexKey {
        u64 buffer, offset;
        u32 count, width;
        Kernels::IndexMode mode;
        bool operator==(const IndexKey&) const = default;
    };
    struct IndexHash {
        size_t operator()(const IndexKey& key) const noexcept {
            size_t h = std::hash<u64>{}(key.buffer);
            for (const u64 value : {key.offset, u64{key.count}, u64{key.width}, u64(key.mode)})
                h ^= std::hash<u64>{}(value) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            return h;
        }
    };
    struct IndexEntry {
        Buffer converted;
        u64 version{}, last_use{};
    };
    std::unordered_map<IndexKey, IndexEntry, IndexHash> converted_indices;
    size_t index_bytes{};
    u64 index_clock{};
    IndexStats index_stats;
    static constexpr size_t IndexBudget = 32 * 1024 * 1024;
    static constexpr size_t IndexEntries = 512;
    static void Bind(std::vector<BufferBinding>&, u32 binding, const Buffer&, u32 offset, u32 size,
                     bool written);
    Runtime& runtime;
    Scheduler& scheduler;
    std::array<StageBufferBindings, VideoCommon::NUM_STAGES> graphics;
    StageBufferBindings compute;
    std::array<VertexBinding, VideoCommon::NUM_VERTEX_BUFFERS> vertices;
    IndexBinding indices;
    std::vector<TexelBufferBinding> texels;
};
struct BufferCacheParams {
    using Runtime = BufferCacheRuntime;
    using Buffer = CacheBuffer;
    using Async_Buffer = StagingBuffer;
    using MemoryTracker = VideoCommon::MemoryTrackerBase<Tegra::MaxwellDeviceMemoryManager>;
    static constexpr bool IS_OPENGL = false;
    static constexpr bool HAS_PERSISTENT_UNIFORM_BUFFER_BINDINGS = false;
    static constexpr bool HAS_FULL_INDEX_AND_PRIMITIVE_SUPPORT = false;
    static constexpr bool NEEDS_BIND_UNIFORM_INDEX = true;
    static constexpr bool NEEDS_BIND_STORAGE_INDEX = true;
    static constexpr bool USE_MEMORY_MAPS = true;
    static constexpr bool SEPARATE_IMAGE_BUFFER_BINDINGS = true;
    static constexpr bool USE_MEMORY_MAPS_FOR_UPLOADS = true;
};
using BufferCache = VideoCommon::BufferCache<BufferCacheParams>;
} // namespace NativeMetal
