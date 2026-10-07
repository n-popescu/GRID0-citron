// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <memory>
#include "video_core/fence_manager.h"
#include "video_core/renderer_metal/metal_buffer_cache.h"
#include "video_core/renderer_metal/metal_query_cache.h"
#include "video_core/renderer_metal/metal_texture_cache.h"

namespace NativeMetal {

/// A guest fence: the submission that contains the work recorded before it.
class InnerFence : public VideoCommon::FenceBase {
public:
    InnerFence(Scheduler& scheduler_, bool is_stubbed_)
        : FenceBase{is_stubbed_}, scheduler{scheduler_} {}
    void Queue() {
        if (!is_stubbed)
            submission = scheduler.Flush();
    }
    bool IsSignaled() const {
        return is_stubbed || !submission || submission.IsComplete();
    }
    void Wait() {
        if (!is_stubbed && submission)
            submission.Wait();
    }

private:
    Scheduler& scheduler;
    Submission submission;
};
using Fence = std::shared_ptr<InnerFence>;

struct FenceManagerParams {
    using FenceType = Fence;
    using BufferCacheType = BufferCache;
    using TextureCacheType = TextureCache;
    using QueryCacheType = QueryCache;
    static constexpr bool HAS_ASYNC_CHECK = true;
};

/// VideoCommon fence manager on Metal submissions: guest fences, sync points and the
/// asynchronous GPU -> guest memory downloads (buffer cache, texture cache, queries) that
/// make GPU results visible to guest CPU code.
class FenceManager final : public VideoCommon::FenceManager<FenceManagerParams> {
public:
    FenceManager(VideoCore::RasterizerInterface& rasterizer_, Tegra::GPU& gpu_,
                 TextureCache& texture_cache_, BufferCache& buffer_cache_,
                 QueryCache& query_cache_, Scheduler& scheduler_)
        : VideoCommon::FenceManager<FenceManagerParams>{rasterizer_, gpu_, texture_cache_,
                                                        buffer_cache_, query_cache_},
          scheduler{scheduler_} {}

protected:
    Fence CreateFence(bool is_stubbed) override {
        return std::make_shared<InnerFence>(scheduler, is_stubbed);
    }
    void QueueFence(Fence& fence) override {
        fence->Queue();
    }
    bool IsFenceSignaled(Fence& fence) const override {
        return fence->IsSignaled();
    }
    void WaitFence(Fence& fence) override {
        fence->Wait();
    }

private:
    Scheduler& scheduler;
};

} // namespace NativeMetal
