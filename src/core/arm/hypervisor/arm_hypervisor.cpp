// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/arm/hypervisor/arm_hypervisor.h"
#include "common/logging.h"
#include "common/page_table.h"
#include "core/arm/nce/interpreter_visitor.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_process.h"
#include "core/memory.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace Core {
struct HypervisorProcess {
    HypervisorProcess(System& system_, Kernel::KProcess& process_,
                      std::shared_ptr<Hypervisor::Vm> vm)
        : system{system_}, process{process_},
          space{std::make_shared<Hypervisor::AddressSpace>(std::move(vm))} {}
    System& system;
    Kernel::KProcess& process;
    std::shared_ptr<Hypervisor::AddressSpace> space;
    std::atomic<bool> failed{};
};
namespace {
void StopFailedBackend(HypervisorProcess& process, const Hypervisor::Context& context,
                       const std::exception& error) {
    if (process.failed.exchange(true, std::memory_order_acq_rel))
        return;
    LOG_CRITICAL(Core_ARM, "Apple Hypervisor stopped: {}; guest pc={:#x}, lr={:#x}, sp={:#x}",
                 error.what(), context.pc, context.regs[30], context.sp);
    Common::Log::Flush();
    process.system.SetStatus(SystemResultStatus::ErrorUnknown, error.what());
    // The native frontend wakes its session thread; shutdown and vCPU destruction run there,
    // after the physical cores have left execution. Never join a CPU from its own guest fiber.
    process.system.Exit();
}
std::mutex vm_mutex;
std::weak_ptr<Hypervisor::Vm> current_vm;
// Hypervisor vCPUs belong to host threads, rather than guest processes/interfaces.
thread_local std::unique_ptr<Hypervisor::Cpu> native_cpu;

// Opt-in exit statistics (CITROSIS_NATIVE_STATS=1), logged every five seconds.
struct NativeStats {
    std::atomic<u64> runs, kicks, svcs, faults, cached_faults, host_sysregs, interpreted;
    std::atomic<s64> last_report_ns;
};
NativeStats native_stats;
const bool native_stats_enabled = [] {
    const char* value = std::getenv("CITROSIS_NATIVE_STATS");
    return value && value[0] == '1';
}();
void Count(std::atomic<u64>& counter) {
    if (native_stats_enabled)
        counter.fetch_add(1, std::memory_order_relaxed);
}
void MaybeReportStats(System& system) {
    if (!native_stats_enabled)
        return;
    const s64 now = system.CoreTiming().GetGlobalTimeNs().count();
    s64 last = native_stats.last_report_ns.load(std::memory_order_relaxed);
    if (now - last < 5'000'000'000 ||
        !native_stats.last_report_ns.compare_exchange_strong(last, now))
        return;
    const double seconds = last ? static_cast<double>(now - last) / 1e9 : 1.0;
    const auto rate = [&](std::atomic<u64>& counter) {
        return static_cast<u64>(static_cast<double>(counter.exchange(0)) / seconds);
    };
    const u64 runs = rate(native_stats.runs), kicks = rate(native_stats.kicks),
              svcs = rate(native_stats.svcs), faults = rate(native_stats.faults),
              cached = rate(native_stats.cached_faults),
              sysregs = rate(native_stats.host_sysregs);
    LOG_WARNING(Core_ARM,
                "Native exits/s: total={} kicks={} svc={} faults={} gpu_cached_faults={} "
                "host_sysreg={} interpreted_gpu_accesses={}",
                runs, kicks, svcs, faults, cached, sysregs, rate(native_stats.interpreted));
}
} // namespace
std::shared_ptr<HypervisorProcess> ArmHypervisor::Create(System& system,
                                                         Kernel::KProcess& process) {
    std::lock_guard lock{vm_mutex};
    auto vm = current_vm.lock();
    if (!vm) {
        auto& buffer = system.DeviceMemory().buffer;
        vm = std::make_shared<Hypervisor::Vm>(buffer.BackingBasePointer(), buffer.backing_size);
        current_vm = vm;
    }
    auto state = std::make_shared<HypervisorProcess>(system, process, std::move(vm));
    auto& table = process.GetPageTable().GetBasePageTable().GetImpl();
    // Retain the validated cache handoff until the optimized ownership path
    // has equivalent GPU/CPU coherency coverage.
    table.notify_uncache = true;
    if (const char* compat = std::getenv("CITROSIS_NATIVE_COMPAT"); compat && *compat)
        LOG_WARNING(Core_ARM, "Native compatibility switches: {}", compat);
    table.on_memory_change = [weak = std::weak_ptr{state->space}](u64 addr, u64 size) {
        if (auto space = weak.lock())
            space->Invalidate(addr, size);
    };
    LOG_WARNING(Core_ARM, "Apple Hypervisor native ARM64 backend selected (experimental)");
    return state;
}
ArmHypervisor::ArmHypervisor(std::shared_ptr<HypervisorProcess> state)
    : ArmInterface{true}, process{std::move(state)} {}
void ArmHypervisor::Initialize() try {
    if (process->failed.load(std::memory_order_acquire))
        return;
    if (!native_cpu) {
        native_cpu = std::make_unique<Hypervisor::Cpu>(process->space->GetVm());
        if (native_cpu->CounterOffset() != 0) {
            LOG_WARNING(Core_ARM, "Native guest counter needed an offset of {} ticks",
                        native_cpu->CounterOffset());
        }
    }
    cpu_id.store(native_cpu->Id(), std::memory_order_release);
} catch (const std::exception& error) {
    StopFailedBackend(*process, context, error);
}
HaltReason ArmHypervisor::RunThread(Kernel::KThread*) try {
    auto& memory = process->process.GetMemory();
    auto& page_table = process->process.GetPageTable();
    auto& table = page_table.GetBasePageTable().GetImpl();
    u64 last_fault_pc = UINT64_MAX;
    u64 last_fault_address = UINT64_MAX;
    u64 last_fault_generation = UINT64_MAX;
    while (true) {
        if (process->failed.load(std::memory_order_acquire))
            return HaltReason::PrefetchAbort;
        if (interrupted.exchange(false))
            return HaltReason::BreakLoop;
        const auto exit = native_cpu->Run(context, *process->space);
        Count(native_stats.runs);
        // Kicks also come from TLB shootdowns; only a pending SignalInterrupt (checked at
        // the top of the loop) returns to the scheduler.
        if (exit.kind == Hypervisor::Exit::Kind::Interrupted) {
            Count(native_stats.kicks);
            MaybeReportStats(process->system);
            if (Hypervisor::GetCompat().kicks)
                return HaltReason::BreakLoop;
            continue;
        }
        const u64 ec = exit.syndrome >> 26;
        if (ec == 0x01) { // Wait hints are normally skipped in EL1; treat as a no-op.
            context.pc += 4;
            continue;
        }
        if (ec == 0x15) {
            Count(native_stats.svcs);
            svc = exit.syndrome & 0xffff;
            MaybeReportStats(process->system);
            return HaltReason::SupervisorCall;
        }
        // Guest counter accesses trap to EL1, preserving the Switch's 19.2 MHz clock.
        if (ec == 0x18) {
            Count(native_stats.host_sysregs);
            const auto instruction = memory.Read32(context.pc);
            const auto opcode = instruction & ~31U;
            const auto rt = instruction & 31;
            u64 value{};
            if (opcode == 0xd53be000)
                value = 19200000; // CNTFRQ_EL0
            else if (opcode == 0xd53be020 || opcode == 0xd53be040)
                value = process->system.CoreTiming().GetClockTicks();
            else if (opcode == 0xd53b00e0)
                value = 4; // DCZID_EL0: 64-byte blocks
            else if (opcode == 0xd53b0020)
                value = 0x8444c004; // CTR_EL0: Cortex-A57 cache geometry
            else if ((instruction & 0xffffffe0) == 0xd50b7420) { // DC ZVA
                const auto address = (rt == 31 ? 0 : context.regs[rt]) & ~63ULL;
                memory.ZeroBlock(address, 64);
            } else if (opcode == 0xd50b7a20 || opcode == 0xd50b7b20 || opcode == 0xd50b7e20 ||
                       opcode == 0xd50b7520) {
                // Cache maintenance: coherent backing memory and the entry cache flush suffice.
            } else {
                LOG_CRITICAL(Core_ARM,
                             "Native unsupported system instruction: pc={:#x} instruction={:#x}",
                             context.pc, instruction);
                return HaltReason::PrefetchAbort;
            }
            if (rt < 31 && (instruction & 0xfff00000) == 0xd5300000)
                context.regs[rt] = value;
            context.pc += 4;
            continue;
        }
        if (ec == 0x20 || ec == 0x24) {
            Count(native_stats.faults);
            const u64 addr = exit.address & ~4095ULL;
            if (addr >= (1ULL << page_table.GetAddressSpaceWidth())) {
                LOG_CRITICAL(Core_ARM,
                             "Native guest fault outside address space: pc={:#x} address={:#x}",
                             context.pc, exit.address);
                return HaltReason::PrefetchAbort;
            }
            const auto generation = process->space->Generation();
            const auto type = table.entries[addr >> 12].pointer.Type();
            Kernel::KMemoryInfo info{};
            Kernel::Svc::PageInfo page{};
            const auto result = page_table.QueryInfo(&info, &page, addr);
            const auto permissions = static_cast<unsigned>(info.GetSvcMemoryInfo().permission);
            const auto required = ec == 0x20 ? 4U : (exit.syndrome & (1U << 6) ? 2U : 1U);
            if (result.IsSuccess() && (permissions & required) &&
                type == Common::PageType::Memory) {
                if (last_fault_pc == context.pc && last_fault_address == exit.address &&
                    last_fault_generation == generation) {
                    LOG_CRITICAL(Core_ARM,
                                 "Native mapping did not resolve fault: pc={:#x} address={:#x} "
                                 "syndrome={:#x}",
                                 context.pc, exit.address, exit.syndrome);
                    return HaltReason::PrefetchAbort;
                }
                last_fault_pc = context.pc;
                last_fault_address = exit.address;
                last_fault_generation = generation;
                const auto& device_memory = process->system.DeviceMemory();
                const auto map_page = [&](u64 target) {
                    const auto* pointer = memory.GetPointer(target);
                    return process->space->Map(target, device_memory.GetRawPhysicalAddr(pointer),
                                               permissions, generation);
                };
                if (!map_page(addr))
                    continue; // memory changed under us; fault again with fresh state
                if (Hypervisor::GetCompat().around)
                    continue;
                // Fault-around: map the rest of the surrounding 64 KB within the same
                // Horizon block (uniform permissions), avoiding one exit per 4 KB page.
                const u64 block_start = info.GetAddress();
                const u64 block_end = block_start + info.GetSize();
                const u64 window_start = std::max<u64>(block_start, addr & ~u64{0xffff});
                const u64 window_end = std::min<u64>(block_end, (addr | u64{0xffff}) + 1);
                for (u64 other = window_start; other < window_end; other += 4096) {
                    if (other != addr &&
                        table.entries[other >> 12].pointer.Type() == Common::PageType::Memory &&
                        !map_page(other)) {
                        break;
                    }
                }
                continue;
            }
            if (ec == 0x24 && result.IsSuccess() && (permissions & required) &&
                type == Common::PageType::RasterizerCachedMemory) {
                Count(native_stats.cached_faults);
                // Download GPU-produced bytes before invalidation marks the whole page
                // CPU-owned. The Linux NCE path uses the same cache invalidation;
                // guest 4 KB mappings let macOS retry the access natively too.
                std::array<u8, 4096> preserved;
                memory.ReadBlock(addr, preserved.data(), preserved.size());
                const bool accessible = memory.InvalidateNCE(addr, 4096);
                if (accessible && table.entries[addr >> 12].pointer.Type() == Common::PageType::Memory) {
                    // Re-query permissions and cache state before publishing a mapping.
                    continue;
                }
                native_cpu->SyncVectors(context);
                std::array<u128, 32> vectors;
                std::memcpy(vectors.data(), context.vectors.data(), sizeof(vectors));
                InterpreterVisitor visitor{memory, context.regs, vectors, context.sp, context.pc};
                const auto instruction = memory.Read32(context.pc);
                const auto executed =
                    Dynarmic::A64::Decode<VisitorBase, bool>(visitor, instruction);
                if (executed && *executed) {
                    Count(native_stats.interpreted);
                    std::memcpy(context.vectors.data(), vectors.data(), sizeof(vectors));
                    context.vectors_dirty = true;
                    context.pc += 4;
                    continue;
                }
            }
            LOG_CRITICAL(Core_ARM,
                         "Native guest fault: pc={:#x} address={:#x} syndrome={:#x} type={} "
                         "permissions={} instruction={:#x}",
                         context.pc, exit.address, exit.syndrome, static_cast<unsigned>(type),
                         permissions, memory.Read32(context.pc));
            return HaltReason::PrefetchAbort;
        }
        LOG_CRITICAL(Core_ARM, "Unhandled native exception: pc={:#x} syndrome={:#x}", context.pc,
                     exit.syndrome);
        return HaltReason::PrefetchAbort;
    }
} catch (const std::exception& error) {
    StopFailedBackend(*process, context, error);
    return HaltReason::PrefetchAbort;
}
HaltReason ArmHypervisor::StepThread(Kernel::KThread*) {
    // Debugger sessions select Dynarmic before loading guest code.
    LOG_CRITICAL(Core_ARM, "Single stepping is unavailable with the Apple native backend");
    return HaltReason::PrefetchAbort;
}
void ArmHypervisor::GetContext(Kernel::Svc::ThreadContext& ctx) const {
    // SIMD state stays in the vCPU between runs; fetch it if this thread's vCPU holds it.
    if (native_cpu && !process->failed.load(std::memory_order_acquire)) {
        try {
            native_cpu->SyncVectors(const_cast<Hypervisor::Context&>(context));
        } catch (const std::exception& error) {
            StopFailedBackend(*process, context, error);
        }
    }
    std::copy_n(context.regs.begin(), 29, ctx.r.begin());
    ctx.fp = context.regs[29];
    ctx.lr = context.regs[30];
    ctx.pc = context.pc;
    ctx.sp = context.sp;
    ctx.pstate = context.pstate;
    ctx.fpcr = context.fpcr;
    ctx.fpsr = context.fpsr;
    ctx.tpidr = context.tpidr;
    std::memcpy(ctx.v.data(), context.vectors.data(), sizeof(ctx.v));
}
void ArmHypervisor::SetContext(const Kernel::Svc::ThreadContext& ctx) {
    std::copy_n(ctx.r.begin(), 29, context.regs.begin());
    context.regs[29] = ctx.fp;
    context.regs[30] = ctx.lr;
    context.pc = ctx.pc;
    context.sp = ctx.sp;
    context.pstate = ctx.pstate;
    context.fpcr = ctx.fpcr;
    context.fpsr = ctx.fpsr;
    context.tpidr = ctx.tpidr;
    std::memcpy(context.vectors.data(), ctx.v.data(), sizeof(ctx.v));
    context.vectors_dirty = true;
}
void ArmHypervisor::GetSvcArguments(std::span<uint64_t, 8> args) const {
    std::copy_n(context.regs.begin(), 8, args.begin());
}
void ArmHypervisor::SetSvcArguments(std::span<const uint64_t, 8> args) {
    std::copy(args.begin(), args.end(), context.regs.begin());
}
void ArmHypervisor::SignalInterrupt(Kernel::KThread*) {
    interrupted.store(true);
    auto id = cpu_id.load(std::memory_order_acquire);
    if (id != UINT64_MAX)
        hv_vcpus_exit(&id, 1);
}
void ArmHypervisor::ClearInstructionCache() { process->space->GetVm()->Flush(true); }
void ArmHypervisor::InvalidateCacheRange(u64 addr, size_t size) {
    if (Hypervisor::GetCompat().unmap)
        process->space->Invalidate(addr, size);
    // Translations are unchanged; every vCPU invalidates its instruction cache before
    // running guest code again.
    process->space->GetVm()->Flush(true);
}
} // namespace Core
