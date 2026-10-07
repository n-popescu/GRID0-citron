// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/arm/arm_interface.h"
#include "core/arm/hypervisor/hypervisor_vm.h"
#include <atomic>
namespace Core {
class System;
struct HypervisorProcess;
class ArmHypervisor final : public ArmInterface {
public:
    static std::shared_ptr<HypervisorProcess> Create(System& system, Kernel::KProcess& process);
    explicit ArmHypervisor(std::shared_ptr<HypervisorProcess> process);
    void Initialize() override;
    Architecture GetArchitecture() const override { return Architecture::AArch64; }
    HaltReason RunThread(Kernel::KThread*) override;
    HaltReason StepThread(Kernel::KThread*) override;
    void GetContext(Kernel::Svc::ThreadContext&) const override;
    void SetContext(const Kernel::Svc::ThreadContext&) override;
    void SetTpidrroEl0(u64 value) override { context.tpidrro = value; }
    void GetSvcArguments(std::span<uint64_t, 8>) const override;
    void SetSvcArguments(std::span<const uint64_t, 8>) override;
    u32 GetSvcNumber() const override { return svc; }
    void SignalInterrupt(Kernel::KThread*) override;
    void ClearInstructionCache() override;
    void InvalidateCacheRange(u64, size_t) override;
    const Kernel::DebugWatchpoint* HaltedWatchpoint() const override { return nullptr; }
    void RewindBreakpointInstruction() override {}

private:
    std::shared_ptr<HypervisorProcess> process;
    Hypervisor::Context context;
    std::atomic<bool> interrupted{};
    std::atomic<uint64_t> cpu_id{UINT64_MAX};
    u32 svc{};
};
} // namespace Core
