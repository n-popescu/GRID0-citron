// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <chrono>
#include "common/alignment.h"
#include "common/logging.h"
#include "video_core/renderer_metal/metal_scheduler.h"
namespace NativeMetal {
Scheduler::~Scheduler() {
    // Explicit Finish reports GPU faults. Destruction cancels pending work;
    // committed command buffers retain their own resources until completion.
}
Commands& Scheduler::Record() {
    if (!commands) {
        commands.emplace(runtime.BeginCommands("Citrosis guest Metal commands"));
        ++recording_serial;
    }
    return *commands;
}
Submission Scheduler::Flush() {
    if (commands) {
        last_submission = commands->Submit();
        commands.reset();
        ++stats.submissions;
    }
    for (auto& chunk : chunks) {
        if (chunk.recording) {
            chunk.fence = last_submission;
            chunk.recording = false;
        }
    }
    return last_submission;
}
void Scheduler::Finish() {
    const auto submission = Flush();
    if (!submission)
        return;
    const auto start = std::chrono::steady_clock::now();
    submission.Wait();
    ++stats.finishes;
    stats.finish_ns += static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            std::chrono::steady_clock::now() - start)
                                            .count());
}
StreamAllocation Scheduler::Stream(size_t size, size_t alignment) {
    size = std::max<size_t>(size, 4);
    alignment = std::max<size_t>(alignment, 4);
    stats.stream_bytes += size;
    if (size > CHUNK_SIZE / 2) {
        // Large one-off transfers get their own allocation.
        ++stats.dedicated_allocations;
        StreamAllocation result;
        result.buffer = runtime.CreateBuffer(Common::AlignUp(size, size_t{256}));
        result.mapped = result.buffer.MappedBytes().first(size);
        return result;
    }
    const auto fits = [&](const Chunk& chunk) {
        return Common::AlignUp(chunk.used, alignment) + size <= chunk.size;
    };
    if (current >= chunks.size() || !fits(chunks[current])) {
        current = SIZE_MAX;
        for (size_t i = 0; i < chunks.size(); ++i) {
            auto& chunk = chunks[i];
            if (chunk.recording)
                continue;
            if (chunk.fence && !chunk.fence.IsComplete())
                continue;
            chunk.used = 0;
            chunk.fence = {};
            current = i;
            break;
        }
        if (current == SIZE_MAX) {
            Chunk chunk;
            chunk.buffer = runtime.CreateBuffer(CHUNK_SIZE);
            chunk.base = chunk.buffer.MappedBytes().data();
            chunk.size = CHUNK_SIZE;
            chunks.push_back(std::move(chunk));
            current = chunks.size() - 1;
            ++stats.stream_chunks;
        }
    }
    auto& chunk = chunks[current];
    const size_t offset = Common::AlignUp(chunk.used, alignment);
    chunk.used = offset + size;
    chunk.recording = true;
    chunk.buffer.MarkWritten(); // this allocation exposes writable CPU bytes
    return {chunk.buffer, offset, std::span{chunk.base + offset, size}};
}
Scheduler::Stats Scheduler::TakeStats() {
    const Stats result = stats;
    stats = {};
    return result;
}

} // namespace NativeMetal
