// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <vector>
#include <thread>
#include <utility>
#include "common/common_types.h"
#include "video_core/renderer_metal/metal_kernels.h"
#include "video_core/renderer_metal/metal_runtime.h"

namespace NativeMetal {
// All guest work uses one queue. Ending a command buffer before CPU access
// preserves order without treating unsubmitted work as a completed fence.
/// A sub-range of a shared, CPU-mapped stream buffer, valid for the work recorded next.
struct StreamAllocation {
    Buffer buffer;
    size_t offset{};
    std::span<std::byte> mapped;
};

class Scheduler {
public:
    explicit Scheduler(Runtime& runtime_) : runtime{runtime_}, kernels{runtime_} {}
    ~Scheduler();
    Commands& Record();
    Submission Flush();
    void Finish();
    Kernels& Utilities() {
        return kernels;
    }
    bool IsRecording() const {
        return commands.has_value();
    }
    /// Identifies the current recording (changes whenever Record() starts a new one).
    u64 RecordingSerial() const {
        return recording_serial;
    }

    /// Transient GPU-visible memory (uniforms, staging, converted indices...). Chunks are
    /// recycled once the submissions that used them complete, so per-draw data costs a
    /// pointer bump instead of a Metal allocation. Write through `mapped` immediately.
    StreamAllocation Stream(size_t size, size_t alignment = 256);

    struct Stats {
        u64 submissions{};
        u64 finishes{};
        u64 finish_ns{};
        u64 stream_bytes{};
        u64 stream_chunks{};
        u64 dedicated_allocations{};
    };
    /// Counters since the last call (reset on read).
    Stats TakeStats();

private:
    struct Chunk {
        Buffer buffer;
        std::byte* base{};
        size_t size{};
        size_t used{};
        Submission fence;
        bool recording{};
    };
    static constexpr size_t CHUNK_SIZE = 16ULL << 20;

    Runtime& runtime;
    Kernels kernels;
    std::optional<Commands> commands;
    Submission last_submission;
    std::vector<Chunk> chunks;
    size_t current = SIZE_MAX;
    u64 recording_serial{};
    Stats stats;
};

} // namespace NativeMetal
