// SPDX-License-Identifier: GPL-2.0-or-later
// LEGACY (pre-October-4-evening) native backend, kept for A/B diagnosis only.
#include "core/arm/hypervisor/arm_hypervisor.h"
#include "common/logging.h"
#include "common/page_table.h"
#include "core/arm/nce/interpreter_visitor.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_process.h"
#include "core/memory.h"
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
};
namespace {
std::mutex vm_mutex;
std::weak_ptr<Hypervisor::Vm> current_vm;
thread_local std::unique_ptr<Hypervisor::Cpu> native_cpu;
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
    table.notify_uncache = true; // legacy behaviour: also unmap when the GPU stops tracking
    table.on_memory_change = [weak = std::weak_ptr{state->space}](u64 addr, u64 size) {
        if (auto space = weak.lock())
            space->Invalidate(addr, size);
    };
    LOG_WARNING(Core_ARM, "LEGACY Apple Hypervisor native backend (A/B diagnosis build)");
    return state;
}
ArmHypervisor::ArmHypervisor(std::shared_ptr<HypervisorProcess> state)
    : ArmInterface{true}, process{std::move(state)} {}
void ArmHypervisor::Initialize() {
    if (!native_cpu)
        native_cpu = std::make_unique<Hypervisor::Cpu>(process->space->GetVm());
    cpu_id.store(native_cpu->Id(), std::memory_order_release);
}
HaltReason ArmHypervisor::RunThread(Kernel::KThread*) {
    auto& memory = process->process.GetMemory();
    auto& page_table = process->process.GetPageTable();
    auto& table = page_table.GetBasePageTable().GetImpl();
    u64 last_fault_pc = UINT64_MAX;
    u64 last_fault_address = UINT64_MAX;
    u64 last_fault_generation = UINT64_MAX;
    while (true) {
        if (interrupted.exchange(false))
            return HaltReason::BreakLoop;
        const auto exit = native_cpu->Run(context, *process->space);
        if (exit.kind == Hypervisor::Exit::Kind::Interrupted)
            return HaltReason::BreakLoop;
        const u64 ec = exit.syndrome >> 26;
        if (ec == 0x01) {
            context.pc += 4;
            return HaltReason::BreakLoop;
        }
        if (ec == 0x15) {
            svc = exit.syndrome & 0xffff;
            return HaltReason::SupervisorCall;
        }
        if (ec == 0x18) {
            const auto instruction = memory.Read32(context.pc);
            const auto opcode = instruction & ~31U;
            const auto rt = instruction & 31;
            u64 value{};
            if (opcode == 0xd53be000)
                value = 19200000;
            else if (opcode == 0xd53be020 || opcode == 0xd53be040)
                value = process->system.CoreTiming().GetClockTicks();
            else if (opcode == 0xd53b00e0)
                value = 4;
            else if (opcode == 0xd53b0020)
                value = 0x8444c004;
            else if ((instruction & 0xffffffe0) == 0xd50b7420) {
                const auto address = (rt == 31 ? 0 : context.regs[rt]) & ~63ULL;
                memory.ZeroBlock(address, 64);
            } else if (opcode == 0xd50b7a20 || opcode == 0xd50b7b20 || opcode == 0xd50b7e20 ||
                       opcode == 0xd50b7520) {
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
                const auto* pointer = memory.GetPointer(addr);
                const auto physical = process->system.DeviceMemory().GetRawPhysicalAddr(pointer);
                process->space->Map(addr, physical, permissions, generation);
                continue;
            }
            if (ec == 0x24 && result.IsSuccess() && (permissions & required) &&
                type == Common::PageType::RasterizerCachedMemory) {
                std::array<u8, 4096> preserved;
                memory.ReadBlock(addr, preserved.data(), preserved.size());
                const bool accessible = memory.InvalidateNCE(addr, preserved.size());
                if (accessible &&
                    table.entries[addr >> 12].pointer.Type() == Common::PageType::Memory) {
                    continue;
                }
                std::array<u128, 32> vectors;
                std::memcpy(vectors.data(), context.vectors.data(), sizeof(vectors));
                InterpreterVisitor visitor{memory, context.regs, vectors, context.sp, context.pc};
                const auto instruction = memory.Read32(context.pc);
                const auto executed =
                    Dynarmic::A64::Decode<VisitorBase, bool>(visitor, instruction);
                if (executed && *executed) {
                    std::memcpy(context.vectors.data(), vectors.data(), sizeof(vectors));
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
}
HaltReason ArmHypervisor::StepThread(Kernel::KThread*) {
    LOG_CRITICAL(Core_ARM, "Single stepping is unavailable with the Apple native backend");
    return HaltReason::PrefetchAbort;
}
void ArmHypervisor::GetContext(Kernel::Svc::ThreadContext& ctx) const {
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
void ArmHypervisor::ClearInstructionCache() { process->space->GetVm()->Interrupt(); }
void ArmHypervisor::InvalidateCacheRange(u64 addr, size_t size) {
    process->space->Invalidate(addr, size);
}
} // namespace Core
