// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <Hypervisor/Hypervisor.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace Core::Hypervisor {
/// Diagnostic switches that restore earlier, slower behaviour, from the environment
/// variable CITROSIS_NATIVE_COMPAT (comma separated names, or "all"):
///   sctlr   EL0 DC ZVA / cache maintenance trap again (no DZE/UCI)
///   clrex   clear the exclusive monitor on every entry
///   tlb     flush the TLB on every entry      icache  flush the I-cache on every entry
///   simd    transfer SIMD state on every run  around  no 64 KB fault-around
///   uncache unmap pages when the GPU stops tracking them
///   kicks   every vCPU kick returns to the scheduler
///   el1     EL0 counter/CTR/wait traps go to the host instead of the EL1 handler
///   regs    upload every register on every run
///   unmap   kernel cache-range invalidation also unmaps the range
struct Compat {
    bool sctlr{}, clrex{}, tlb{}, icache{}, simd{}, around{}, uncache{}, kicks{}, el1{},
        regs{}, unmap{};
};
const Compat& GetCompat();
struct Context {
    std::array<uint64_t, 31> regs{};
    std::array<std::array<uint64_t, 2>, 32> vectors{};
    uint64_t pc{}, sp{}, tpidr{}, tpidrro{};
    uint32_t pstate{}, fpcr{}, fpsr{};
    // SIMD registers stay resident in the vCPU between runs. Set this after writing
    // `vectors` from the host; call Cpu::SyncVectors before reading them.
    bool vectors_dirty{true};
};
struct Exit {
    enum class Kind { Exception, Interrupted } kind;
    uint64_t syndrome{}, address{};
};
class Cpu;
class Vm : public std::enable_shared_from_this<Vm> {
public:
    Vm(void* backing, size_t size);
    ~Vm();
    Vm(const Vm&) = delete;
    Vm& operator=(const Vm&) = delete;
    uint64_t AllocateTable();
    uint64_t* Table(uint64_t physical);
    /// Kicks every vCPU out of the guest (scheduler use). Does not wait.
    void Interrupt();
    /// Requests a TLB (and optionally instruction cache) flush on every vCPU and waits
    /// until no other vCPU can still run guest code with stale translations.
    void Flush(bool instruction_cache);
    uint64_t KernelRoot() const { return kernel_root; }
    size_t BackingSize() const { return backing_size; }
    uint64_t Epoch() const { return epoch.load(std::memory_order_seq_cst); }
    uint64_t InstructionEpoch() const { return ic_epoch.load(std::memory_order_seq_cst); }
    uint64_t TickFactor() const { return tick_factor; }

private:
    friend class Cpu;
    unsigned RegisterCpu(Cpu* cpu);
    void UnregisterCpu(Cpu* cpu);
    uint64_t* Frame(unsigned slot);
    void* pool{};
    size_t backing_size{}, next_page{};
    uint64_t kernel_root{}, tick_factor{};
    std::mutex mutex;       // cpus, slots, table pool
    std::mutex flush_mutex; // epoch updates
    std::vector<Cpu*> cpus;
    uint64_t used_slots{};
    std::atomic<uint64_t> epoch{1}, ic_epoch{1};
};
class AddressSpace {
public:
    explicit AddressSpace(std::shared_ptr<Vm> vm);
    bool Map(uint64_t virtual_address, uint64_t physical, unsigned permissions,
             uint64_t expected_generation = UINT64_MAX);
    uint64_t Generation();
    /// Removes mappings in the range; returns once no vCPU can still use them.
    void Invalidate(uint64_t address, uint64_t size);
    uint64_t Root() const { return root; }
    std::shared_ptr<Vm> GetVm() const { return vm; }

private:
    uint64_t* Entry(uint64_t address, bool allocate);
    std::shared_ptr<Vm> vm;
    uint64_t root{}, generation{};
    std::mutex mutex;
};
class Cpu {
public:
    explicit Cpu(std::shared_ptr<Vm> vm);
    ~Cpu();
    Cpu(const Cpu&) = delete;
    Cpu& operator=(const Cpu&) = delete;
    Exit Run(Context& context, AddressSpace& space);
    /// Copies resident SIMD state into `context` if this vCPU holds it. Owner thread only.
    void SyncVectors(Context& context);
    hv_vcpu_t Id() const { return cpu; }
    /// Offset added to the guest CNTVCT_EL0 before 19.2 MHz scaling (0 when aligned).
    int64_t CounterOffset() const { return counter_offset; }

private:
    friend class Vm;
    void Calibrate();
    void UploadAll(const Context& context);
    std::shared_ptr<Vm> vm;
    hv_vcpu_t cpu{};
    hv_vcpu_exit_t* exit{};
    std::thread::id owner;
    unsigned slot{UINT32_MAX};
    uint64_t frame_top{}; // SP_EL1 on exception entry (kernel VA)
    int64_t counter_offset{};
    std::atomic<bool> in_guest{};
    std::atomic<uint64_t> acknowledged{};
    // What the vCPU currently holds, so only differences are uploaded.
    const Context* resident{};
    Context shadow{};
    uint64_t last_root{}, flushed_epoch{};
    bool need_clrex{true};
};
} // namespace Core::Hypervisor
