// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <deque>
#include <mutex>
#include <vector>
#include "common/common_types.h"
#include "video_core/renderer_metal/metal_buffer_cache.h"

namespace Tegra {
class MemoryManager;
}

namespace NativeMetal {

/// Guest occlusion queries (ZPassPixelCount64) on Metal visibility results.
///
/// Counted draws accumulate into 8-byte slots of a per-recording visibility buffer (one slot
/// per render pass and counter epoch). A report sums the slots counted since the last reset;
/// the sum is written to guest memory on the fence release thread once the GPU finished
/// (VideoCommon::FenceManager's Commit/PopAsyncFlushes protocol), like the Vulkan query cache.
class QueryCache {
public:
    QueryCache(Runtime& metal_, Scheduler& scheduler_, BufferCache& buffer_cache_)
        : metal{metal_}, scheduler{scheduler_}, buffer_cache{buffer_cache_} {}

    // Fence manager protocol (Commit/HasUncommitted: GPU thread; Pop: release thread).
    void CommitAsyncFlushes();
    bool HasUncommittedFlushes() const;
    bool ShouldWaitAsyncFlushes() const;
    void PopAsyncFlushes();

    /// Counter reset (ZPassPixelCount).
    void Reset();
    /// Report of the counter value into guest memory (8 bytes + timestamp if long).
    void Report(Tegra::MemoryManager& memory, GPUVAddr address, bool long_query, u64 ticks);

    /// Whether render passes should carry a visibility buffer (queries were used).
    bool Active() const {
        return active;
    }
    void Activate() {
        active = true;
    }
    /// Attaches this recording's visibility buffer to the next render pass.
    void PreparePass(Commands& commands);
    /// Records that the pass with this id was begun with the visibility buffer.
    void NotePass(u64 pass_id) {
        visibility_pass = pass_id;
    }
    bool PassHasVisibility(u64 pass_id) const {
        return visibility_pass == pass_id;
    }
    /// Sets counting state for the next draw of the active pass.
    void UpdateDraw(Commands& commands, bool counting);

private:
    struct Slot {
        Buffer buffer;
        size_t offset;
    };
    struct PendingReport {
        Tegra::MemoryManager* memory;
        GPUVAddr address;
        bool long_query;
        u64 ticks;
        std::vector<Slot> slots;
    };
    static constexpr size_t VISIBILITY_BYTES = 64 * 1024;

    Runtime& metal;
    Scheduler& scheduler;
    BufferCache& buffer_cache;
    bool active{};

    Buffer visibility;
    u64 visibility_recording = ~0ULL;
    size_t next_slot{};
    u64 visibility_pass{};

    std::vector<Slot> segments; // slots counted since the last reset
    u64 epoch{1};
    u64 slot_pass{};
    u64 slot_epoch{};
    size_t slot_offset{};
    u64 enabled_pass{};
    size_t enabled_offset{};

    std::vector<PendingReport> uncommitted;
    mutable std::mutex mutex; // committed is shared with the release thread
    std::deque<std::vector<PendingReport>> committed;
};

} // namespace NativeMetal
