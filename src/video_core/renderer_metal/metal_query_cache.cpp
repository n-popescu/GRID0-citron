// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstring>
#include "video_core/memory_manager.h"
#include "video_core/renderer_metal/metal_query_cache.h"

namespace NativeMetal {

void QueryCache::Reset() {
    Activate();
    segments.clear();
    ++epoch;
}

void QueryCache::Report(Tegra::MemoryManager& memory, GPUVAddr address, bool long_query,
                        u64 ticks) {
    Activate();
    uncommitted.push_back({&memory, address, long_query, ticks, segments});
    // Draws after the report must not add to slots this report reads.
    ++epoch;
}

void QueryCache::PreparePass(Commands& commands) {
    if (!active)
        return;
    if (visibility_recording != scheduler.RecordingSerial() || !visibility.Valid()) {
        visibility = metal.CreateBuffer(VISIBILITY_BYTES);
        const auto bytes = visibility.MappedBytes();
        std::memset(bytes.data(), 0, bytes.size());
        visibility_recording = scheduler.RecordingSerial();
        next_slot = 0;
    }
    commands.SetVisibilityBuffer(visibility);
}

void QueryCache::UpdateDraw(Commands& commands, bool counting) {
    const u64 pass = commands.RenderPassId();
    if (!counting) {
        if (enabled_pass == pass) {
            commands.SetVisibilityResult(false, 0);
            enabled_pass = 0;
        }
        return;
    }
    if (!PassHasVisibility(pass))
        return; // this pass began before queries were active: the draw is not counted
    if (slot_pass != pass || slot_epoch != epoch) {
        if ((next_slot + 1) * 8 > VISIBILITY_BYTES)
            return; // out of slots in this recording (thousands of passes): not counted
        slot_offset = next_slot * 8;
        ++next_slot;
        slot_pass = pass;
        slot_epoch = epoch;
        segments.push_back({visibility, slot_offset});
    }
    if (enabled_pass != pass || enabled_offset != slot_offset) {
        commands.SetVisibilityResult(true, slot_offset);
        enabled_pass = pass;
        enabled_offset = slot_offset;
    }
}

void QueryCache::CommitAsyncFlushes() {
    std::scoped_lock lock{mutex};
    committed.push_back(std::move(uncommitted));
    uncommitted.clear();
}

bool QueryCache::HasUncommittedFlushes() const {
    return !uncommitted.empty();
}

bool QueryCache::ShouldWaitAsyncFlushes() const {
    std::scoped_lock lock{mutex};
    return !committed.empty() && !committed.front().empty();
}

void QueryCache::PopAsyncFlushes() {
    std::vector<PendingReport> reports;
    {
        std::scoped_lock lock{mutex};
        if (committed.empty())
            return;
        reports = std::move(committed.front());
        committed.pop_front();
    }
    // The fence for these reports has completed: the visibility results are final.
    for (const PendingReport& report : reports) {
        u64 value = 0;
        for (const Slot& slot : report.slots) {
            u64 samples;
            std::memcpy(&samples, slot.buffer.UnsafeContents() + slot.offset, sizeof(samples));
            value += samples;
        }
        if (report.long_query) {
            const u64 data[2]{value, report.ticks};
            report.memory->WriteBlockUnsafe(report.address, data, sizeof(data));
        } else {
            const u32 data = static_cast<u32>(value);
            report.memory->WriteBlockUnsafe(report.address, &data, sizeof(data));
        }
        if (const auto cpu_addr = report.memory->GpuToCpuAddress(report.address)) {
            std::scoped_lock lock{buffer_cache.mutex};
            buffer_cache.WriteMemory(*cpu_addr, report.long_query ? 16 : 4);
        }
    }
}

} // namespace NativeMetal
