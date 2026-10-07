// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/arm/hypervisor/hypervisor_vm.h"
#include "core/arm/hypervisor/hypervisor_el1.inc"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mach/mach_time.h>
#include <stdexcept>
#include <string>
#include <cstdlib>
#include <sys/mman.h>

namespace Core::Hypervisor {
const Compat& GetCompat() {
    static const Compat compat = [] {
        Compat result;
        const char* env = std::getenv("CITROSIS_NATIVE_COMPAT");
        const std::string value = env ? env : "";
        const auto has = [&](const char* name) {
            if (value == "all")
                return true;
            const std::string token{name};
            size_t start = 0;
            while (start <= value.size()) {
                const auto end = std::min(value.find(',', start), value.size());
                if (value.compare(start, end - start, token) == 0)
                    return true;
                start = end + 1;
            }
            return false;
        };
        result.sctlr = has("sctlr");
        result.clrex = has("clrex");
        result.tlb = has("tlb");
        result.icache = has("icache");
        result.simd = has("simd");
        result.around = has("around");
        result.uncache = has("uncache");
        result.kicks = has("kicks");
        result.el1 = has("el1");
        result.regs = has("regs");
        result.unmap = has("unmap");
        return result;
    }();
    return compat;
}
namespace {
constexpr uint64_t PoolBase = 0x800000000ULL;
constexpr size_t PoolSize = 64 * 1024 * 1024;
constexpr uint64_t KernelBase = 0xffffff8000000000ULL;
constexpr uint64_t VectorAddress = KernelBase + 0x1000;
constexpr uint64_t StubAddress = KernelBase + 0x2000;
constexpr uint64_t StubClrex = StubAddress + 0x00;
constexpr uint64_t StubTlb = StubAddress + 0x10;
constexpr uint64_t StubTlbIc = StubAddress + 0x30;
constexpr uint64_t StubCounter = StubAddress + 0x50;
constexpr uint64_t FrameAddress = KernelBase + 0x3000;
constexpr uint64_t FrameSize = 64;
constexpr unsigned MaxCpus = 64;
constexpr uint64_t AddressMask = 0x000000fffffff000ULL;
constexpr uint64_t Uxn = 1ULL << 54;
constexpr uint64_t El1Cpsr = 0x3c5; // EL1h, DAIF masked
// HVC #imm from EL1: EC 0x16, IL set.
constexpr uint64_t DispatchSyndrome = 0x5a000000 | 0xc175;
constexpr uint64_t ProbeSyndrome = 0x5a000000 | 0xc176;
// M, C, I, SA0, nAA... baseline plus DZE (EL0 DC ZVA) and UCI (EL0 cache maintenance).
// UCT stays clear so CTR_EL0 reads trap and report Switch geometry; nTWE/nTWI clear so
// wait hints are skipped by the EL1 handler without leaving the VM.
constexpr uint64_t Sctlr = 0x30d01805ULL | (1ULL << 14) | (1ULL << 26);

constexpr uint64_t El0Cpsr(uint64_t pstate) {
    return (pstate & 0xf0000000ULL) | 0x3c0; // EL0t, NZCV kept, DAIF masked
}
void Check(hv_return_t result, const char* operation) {
    if (result != HV_SUCCESS) {
        char code[32];
        std::snprintf(code, sizeof(code), "0x%08x", static_cast<unsigned>(result));
        throw std::runtime_error(std::string(operation) + " failed: " + code);
    }
}
uint64_t Read(hv_vcpu_t cpu, hv_reg_t reg) {
    uint64_t value{};
    Check(hv_vcpu_get_reg(cpu, reg, &value), "hv_vcpu_get_reg");
    return value;
}
void Write(hv_vcpu_t cpu, hv_reg_t reg, uint64_t value) {
    Check(hv_vcpu_set_reg(cpu, reg, value), "hv_vcpu_set_reg");
}
uint64_t ReadSystem(hv_vcpu_t cpu, hv_sys_reg_t reg) {
    uint64_t value{};
    Check(hv_vcpu_get_sys_reg(cpu, reg, &value), "hv_vcpu_get_sys_reg");
    return value;
}
void WriteSystem(hv_vcpu_t cpu, hv_sys_reg_t reg, uint64_t value) {
    Check(hv_vcpu_set_sys_reg(cpu, reg, value), "hv_vcpu_set_sys_reg");
}
void StoreEntry(uint64_t& entry, uint64_t value) {
    std::atomic_ref<uint64_t>(entry).store(value, std::memory_order_release);
}
uint64_t LoadEntry(uint64_t& entry) {
    return std::atomic_ref<uint64_t>(entry).load(std::memory_order_acquire);
}
uint64_t HostCounterFrequency() {
    uint64_t frequency{};
    asm volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    return frequency;
}
std::string Hex(uint64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%#llx", static_cast<unsigned long long>(value));
    return buffer;
}
} // namespace

Vm::Vm(void* backing, size_t size) : backing_size{size} {
    Check(hv_vm_create(nullptr), "hv_vm_create (requires com.apple.security.hypervisor)");
    try {
        Check(hv_vm_map(backing, 0, size, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC),
              "map DRAM");
        pool = mmap(nullptr, PoolSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (pool == MAP_FAILED) {
            pool = nullptr;
            throw std::bad_alloc{};
        }
        Check(
            hv_vm_map(pool, PoolBase, PoolSize, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC),
            "map tables");
        // Kernel pages: 1 vectors+helpers, 2 entry stubs, 3 per-vCPU EL1 frames.
        next_page = 0x4000;
        kernel_root = AllocateTable();
        const auto middle = AllocateTable();
        const auto leaf = AllocateTable();
        Table(kernel_root)[0] = middle | 3;
        Table(middle)[0] = leaf | 3;
        for (unsigned i = 0; i < 4; ++i) {
            // EL1-only (AP=00), never executable from EL0.
            Table(leaf)[i] = (PoolBase + i * 0x1000) | 3 | (1ULL << 10) | (3ULL << 8) | Uxn;
        }
        std::memcpy(static_cast<char*>(pool) + 0x1000, kEl1VectorPage, sizeof(kEl1VectorPage));
        std::memcpy(static_cast<char*>(pool) + 0x2000, kEl1StubPage, sizeof(kEl1StubPage));
        if (GetCompat().el1) {
            // Lower-EL synchronous vector: hand every exception straight to the host.
            const uint32_t dispatch = 0xd4182ea2; // hvc #0xc175
            std::memcpy(static_cast<char*>(pool) + 0x1400, &dispatch, sizeof(dispatch));
        }
        // Natively read counters are scaled to the Switch's 19.2 MHz in EL1 with UMULH.
        // This equals Common::Arm64::NativeClock::GetCNTPCT on the same counter value.
        const uint64_t host_frequency = HostCounterFrequency();
        if (host_frequency <= 19200000) {
            throw std::runtime_error("Unsupported host counter frequency " +
                                     std::to_string(host_frequency));
        }
        tick_factor = static_cast<uint64_t>((static_cast<unsigned __int128>(19200000) << 64) /
                                            host_frequency);
    } catch (...) {
        hv_vm_destroy();
        if (pool)
            munmap(pool, PoolSize);
        throw;
    }
}
Vm::~Vm() {
    hv_vm_destroy();
    if (pool)
        munmap(pool, PoolSize);
}
uint64_t Vm::AllocateTable() {
    std::lock_guard lock{mutex};
    if (next_page == PoolSize)
        throw std::runtime_error("Hypervisor page-table pool exhausted");
    const auto address = PoolBase + next_page;
    next_page += 0x1000;
    return address;
}
uint64_t* Vm::Table(uint64_t physical) {
    return reinterpret_cast<uint64_t*>(static_cast<char*>(pool) + (physical - PoolBase));
}
uint64_t* Vm::Frame(unsigned slot) {
    return reinterpret_cast<uint64_t*>(static_cast<char*>(pool) + 0x3000 + slot * FrameSize);
}
unsigned Vm::RegisterCpu(Cpu* cpu) {
    std::lock_guard lock{mutex};
    for (unsigned slot = 0; slot < MaxCpus; ++slot) {
        if (!(used_slots & (1ULL << slot))) {
            used_slots |= 1ULL << slot;
            cpus.push_back(cpu);
            return slot;
        }
    }
    throw std::runtime_error("Too many Hypervisor vCPUs");
}
void Vm::UnregisterCpu(Cpu* cpu) {
    std::lock_guard lock{mutex};
    std::erase(cpus, cpu);
    if (cpu->slot < MaxCpus)
        used_slots &= ~(1ULL << cpu->slot);
}
void Vm::Interrupt() {
    std::lock_guard lock{mutex};
    std::vector<hv_vcpu_t> ids;
    ids.reserve(cpus.size());
    for (auto* cpu : cpus)
        ids.push_back(cpu->cpu);
    if (!ids.empty())
        hv_vcpus_exit(ids.data(), static_cast<uint32_t>(ids.size()));
}
void Vm::Flush(bool instruction_cache) {
    uint64_t target;
    {
        std::lock_guard lock{flush_mutex};
        target = epoch.load(std::memory_order_relaxed) + 1;
        if (instruction_cache)
            ic_epoch.store(target, std::memory_order_seq_cst);
        epoch.store(target, std::memory_order_seq_cst);
    }
    // Every vCPU flushes its own TLB (and I-cache) before it next executes guest code.
    // Wait for vCPUs currently in the guest, so callers (unmap, GPU cache tracking) can
    // rely on the old translations being unusable once this returns.
    std::lock_guard lock{mutex};
    const auto self = std::this_thread::get_id();
    std::vector<hv_vcpu_t> ids;
    for (auto* cpu : cpus) {
        if (cpu->owner != self && cpu->in_guest.load(std::memory_order_seq_cst) &&
            cpu->acknowledged.load(std::memory_order_seq_cst) < target) {
            ids.push_back(cpu->cpu);
        }
    }
    if (ids.empty())
        return;
    hv_vcpus_exit(ids.data(), static_cast<uint32_t>(ids.size()));
    for (auto* cpu : cpus) {
        if (cpu->owner == self)
            continue;
        for (unsigned spins = 1; cpu->in_guest.load(std::memory_order_seq_cst) &&
                                 cpu->acknowledged.load(std::memory_order_seq_cst) < target;
             ++spins) {
            if ((spins & 1023) == 0) {
                hv_vcpu_t id = cpu->cpu;
                hv_vcpus_exit(&id, 1);
            }
            std::this_thread::yield();
        }
    }
}

AddressSpace::AddressSpace(std::shared_ptr<Vm> vm_)
    : vm{std::move(vm_)}, root{vm->AllocateTable()} {}
uint64_t* AddressSpace::Entry(uint64_t address, bool allocate) {
    if (address >= (1ULL << 39))
        throw std::runtime_error("Hypervisor virtual address exceeds 39 bits");
    auto* table = vm->Table(root);
    for (const unsigned shift : {30, 21}) {
        auto& entry = table[(address >> shift) & 511];
        auto value = LoadEntry(entry);
        if (!value) {
            if (!allocate)
                return nullptr;
            value = vm->AllocateTable() | 3;
            StoreEntry(entry, value);
        }
        table = vm->Table(value & AddressMask);
    }
    return &table[(address >> 12) & 511];
}
uint64_t AddressSpace::Generation() {
    std::lock_guard lock{mutex};
    return generation;
}
bool AddressSpace::Map(uint64_t address, uint64_t physical, unsigned permissions,
                       uint64_t expected_generation) {
    if ((address | physical) & 4095 || physical + 4096 > vm->BackingSize()) {
        throw std::runtime_error("Invalid Hypervisor 4 KB mapping");
    }
    uint64_t descriptor = physical | 3 | (1ULL << 10) | (3ULL << 8) | (1ULL << 53);
    // AP=01 permits EL0 read/write; AP=11 permits EL0 read-only.
    descriptor |= (permissions & 2 ? 1ULL : 3ULL) << 6;
    if (!(permissions & 4))
        descriptor |= Uxn;
    if (!(permissions & 1))
        descriptor = 0;
    bool replaced = false;
    {
        std::lock_guard lock{mutex};
        if (expected_generation != UINT64_MAX && generation != expected_generation)
            return false;
        auto& entry = *Entry(address, true);
        const auto old = LoadEntry(entry);
        if (old == descriptor)
            return true;
        StoreEntry(entry, descriptor);
        if (old & 1) {
            // Changing a live translation needs a TLB flush; adding one does not,
            // because translation faults are never cached.
            ++generation;
            replaced = true;
        }
    }
    if (replaced)
        vm->Flush(false);
    return true;
}
void AddressSpace::Invalidate(uint64_t address, uint64_t size) {
    if (!size || size > (1ULL << 39) || address >= (1ULL << 39))
        return;
    const auto first = address & ~4095ULL;
    const auto last = std::min((address + size + 4095) & ~4095ULL, 1ULL << 39);
    bool changed = false, executable = false;
    {
        std::lock_guard lock{mutex};
        ++generation;
        // Walk the tables instead of probing every page, skipping absent 1 GB/2 MB ranges.
        for (uint64_t page = first; page < last;) {
            auto& upper = vm->Table(root)[(page >> 30) & 511];
            const auto upper_value = LoadEntry(upper);
            if (!upper_value) {
                page = (page | ((1ULL << 30) - 1)) + 1;
                continue;
            }
            auto& middle = vm->Table(upper_value & AddressMask)[(page >> 21) & 511];
            const auto middle_value = LoadEntry(middle);
            const auto next = std::min((page | ((1ULL << 21) - 1)) + 1, last);
            if (middle_value) {
                auto* leaf = vm->Table(middle_value & AddressMask);
                for (; page < next; page += 4096) {
                    auto& entry = leaf[(page >> 12) & 511];
                    const auto value = LoadEntry(entry);
                    if (value) {
                        executable |= !(value & Uxn);
                        StoreEntry(entry, 0);
                        changed = true;
                    }
                }
            }
            page = next;
        }
    }
    if (changed)
        vm->Flush(executable);
}

Cpu::Cpu(std::shared_ptr<Vm> vm_) : vm{std::move(vm_)}, owner{std::this_thread::get_id()} {
    Check(hv_vcpu_create(&cpu, &exit, nullptr), "hv_vcpu_create");
    try {
        WriteSystem(cpu, HV_SYS_REG_MAIR_EL1, 0xff);
        // Two 39-bit VA spaces, 4 KB guest granules, 40-bit IPA, inner-shareable WB tables.
        WriteSystem(cpu, HV_SYS_REG_TCR_EL1,
                    25ULL | (1ULL << 8) | (1ULL << 10) | (3ULL << 12) | (25ULL << 16) |
                        (1ULL << 24) | (1ULL << 26) | (3ULL << 28) | (2ULL << 30) | (2ULL << 32));
        WriteSystem(cpu, HV_SYS_REG_TTBR1_EL1, vm->KernelRoot());
        WriteSystem(cpu, HV_SYS_REG_VBAR_EL1, VectorAddress);
        WriteSystem(cpu, HV_SYS_REG_CPACR_EL1, 3ULL << 20);
        WriteSystem(cpu, HV_SYS_REG_CNTKCTL_EL1, 0); // EL0 counter reads trap to the EL1 handler
        Check(hv_vcpu_set_vtimer_mask(cpu, true), "mask virtual timer");
        WriteSystem(cpu, HV_SYS_REG_SCTLR_EL1, GetCompat().sctlr ? 0x30d01805ULL : Sctlr);
        slot = vm->RegisterCpu(this);
        frame_top = FrameAddress + slot * FrameSize + 32;
        WriteSystem(cpu, HV_SYS_REG_SP_EL1, frame_top);
        auto* frame = vm->Frame(slot);
        std::memset(frame, 0, FrameSize);
        frame[5] = vm->TickFactor();
        Calibrate();
        frame[4] = static_cast<uint64_t>(counter_offset);
    } catch (...) {
        if (slot != UINT32_MAX)
            vm->UnregisterCpu(this);
        hv_vcpu_destroy(cpu);
        throw;
    }
}
Cpu::~Cpu() {
    vm->UnregisterCpu(this);
    hv_vcpu_destroy(cpu);
}
void Cpu::Calibrate() {
    // The guest reads CNTVCT_EL0 at EL1; the host clock uses mach_absolute_time. They are
    // expected to be the same counter. Verify it, and fall back to a measured offset.
    uint64_t best_width = UINT64_MAX;
    int64_t best_offset = 0;
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        Write(cpu, HV_REG_PC, StubCounter);
        Write(cpu, HV_REG_CPSR, El1Cpsr);
        const uint64_t before = mach_absolute_time();
        Check(hv_vcpu_run(cpu), "counter probe");
        const uint64_t after = mach_absolute_time();
        if (exit->reason != HV_EXIT_REASON_EXCEPTION || exit->exception.syndrome != ProbeSyndrome)
            continue; // canceled by a concurrent kick
        const uint64_t guest = Read(cpu, HV_REG_X0);
        if (guest >= before && guest <= after) {
            counter_offset = 0;
            return;
        }
        if (after - before < best_width) {
            best_width = after - before;
            best_offset = static_cast<int64_t>(before + (after - before) / 2 - guest);
        }
    }
    if (best_width == UINT64_MAX)
        throw std::runtime_error("Hypervisor counter probe failed");
    counter_offset = best_offset;
}
void Cpu::UploadAll(const Context& context) {
    for (unsigned i = 0; i < 31; ++i)
        Write(cpu, static_cast<hv_reg_t>(HV_REG_X0 + i), context.regs[i]);
    WriteSystem(cpu, HV_SYS_REG_SP_EL0, context.sp);
    WriteSystem(cpu, HV_SYS_REG_TPIDR_EL0, context.tpidr);
    WriteSystem(cpu, HV_SYS_REG_TPIDRRO_EL0, context.tpidrro);
    Write(cpu, HV_REG_FPCR, context.fpcr);
    Write(cpu, HV_REG_FPSR, context.fpsr);
    shadow.regs = context.regs;
    shadow.sp = context.sp;
    shadow.tpidr = context.tpidr;
    shadow.tpidrro = context.tpidrro;
    shadow.fpcr = context.fpcr;
    shadow.fpsr = context.fpsr;
}
void Cpu::SyncVectors(Context& context) {
    if (resident != &context || context.vectors_dirty)
        return;
    for (unsigned i = 0; i < 32; ++i) {
        hv_simd_fp_uchar16_t value;
        Check(hv_vcpu_get_simd_fp_reg(cpu, static_cast<hv_simd_fp_reg_t>(HV_SIMD_FP_REG_Q0 + i),
                                      &value),
              "get SIMD");
        std::memcpy(context.vectors[i].data(), &value, 16);
    }
}
Exit Cpu::Run(Context& context, AddressSpace& space) {
    // A different guest thread: drop any exclusive reservation of the previous one.
    need_clrex |= resident != &context || context.vectors_dirty;
    if (GetCompat().regs)
        resident = nullptr;
    if (resident != &context) {
        UploadAll(context);
    } else {
        for (unsigned i = 0; i < 31; ++i) {
            if (context.regs[i] != shadow.regs[i]) {
                Write(cpu, static_cast<hv_reg_t>(HV_REG_X0 + i), context.regs[i]);
                shadow.regs[i] = context.regs[i];
            }
        }
        if (context.sp != shadow.sp)
            WriteSystem(cpu, HV_SYS_REG_SP_EL0, shadow.sp = context.sp);
        if (context.tpidr != shadow.tpidr)
            WriteSystem(cpu, HV_SYS_REG_TPIDR_EL0, shadow.tpidr = context.tpidr);
        if (context.tpidrro != shadow.tpidrro)
            WriteSystem(cpu, HV_SYS_REG_TPIDRRO_EL0, shadow.tpidrro = context.tpidrro);
        if (context.fpcr != shadow.fpcr)
            Write(cpu, HV_REG_FPCR, shadow.fpcr = context.fpcr);
        if (context.fpsr != shadow.fpsr)
            Write(cpu, HV_REG_FPSR, shadow.fpsr = context.fpsr);
    }
    if (context.vectors_dirty || resident != &context) {
        for (unsigned i = 0; i < 32; ++i) {
            hv_simd_fp_uchar16_t value;
            std::memcpy(&value, context.vectors[i].data(), 16);
            Check(hv_vcpu_set_simd_fp_reg(
                      cpu, static_cast<hv_simd_fp_reg_t>(HV_SIMD_FP_REG_Q0 + i), value),
                  "set SIMD");
        }
        context.vectors_dirty = false;
    }
    resident = &context;

    bool flush_tlb = false;
    if (space.Root() != last_root) {
        WriteSystem(cpu, HV_SYS_REG_TTBR0_EL1, space.Root());
        last_root = space.Root();
        flush_tlb = true; // no ASIDs are used
    }
    in_guest.store(true, std::memory_order_seq_cst);
    const uint64_t epoch = vm->Epoch();
    const auto& compat = GetCompat();
    bool flush_ic = compat.icache;
    need_clrex |= compat.clrex;
    flush_tlb |= compat.tlb || compat.icache;
    if (epoch != flushed_epoch) {
        flush_tlb = true;
        flush_ic |= vm->InstructionEpoch() > flushed_epoch;
    }
    // The stub runs before any guest instruction, so this vCPU already honours `epoch`.
    acknowledged.store(epoch, std::memory_order_seq_cst);
    const uint64_t guest_cpsr = El0Cpsr(context.pstate);
    const uint64_t stub = flush_tlb ? (flush_ic ? StubTlbIc : StubTlb) : need_clrex ? StubClrex : 0;
    hv_return_t result;
    try {
        if (stub) {
            // One VM entry: the stub flushes, then ERETs to the guest.
            WriteSystem(cpu, HV_SYS_REG_ELR_EL1, context.pc);
            WriteSystem(cpu, HV_SYS_REG_SPSR_EL1, guest_cpsr);
            Write(cpu, HV_REG_PC, stub);
            Write(cpu, HV_REG_CPSR, El1Cpsr);
        } else {
            Write(cpu, HV_REG_PC, context.pc);
            Write(cpu, HV_REG_CPSR, guest_cpsr);
        }
        result = hv_vcpu_run(cpu);
    } catch (...) {
        in_guest.store(false, std::memory_order_release);
        throw;
    }
    in_guest.store(false, std::memory_order_release);
    if (result != HV_SUCCESS) {
        // Capture the entry state and actual vCPU PC before the backend stops. Hypervisor API
        // failures are not ordinary guest exception exits and must not be resumed blindly.
        uint64_t actual_pc{};
        const bool have_pc = hv_vcpu_get_reg(cpu, HV_REG_PC, &actual_pc) == HV_SUCCESS;
        const auto operation = "hv_vcpu_run guest_pc=" + Hex(context.pc) +
                               " vcpu_pc=" + (have_pc ? Hex(actual_pc) : "unavailable") +
                               " cpsr=" + Hex(stub ? El1Cpsr : guest_cpsr) +
                               " stub=" + Hex(stub);
        Check(result, operation.c_str());
    }

    const uint64_t pc = Read(cpu, HV_REG_PC);
    const bool in_stub = pc >= StubAddress && pc < StubAddress + 0x1000;
    if (stub && !in_stub) {
        need_clrex = false;
        if (flush_tlb)
            flushed_epoch = epoch;
    } else if (flush_tlb) {
        last_root = 0; // the flush did not finish; redo it on the next entry
    }
    for (unsigned i = 0; i < 31; ++i)
        context.regs[i] = shadow.regs[i] = Read(cpu, static_cast<hv_reg_t>(HV_REG_X0 + i));
    context.sp = shadow.sp = ReadSystem(cpu, HV_SYS_REG_SP_EL0);
    context.tpidr = shadow.tpidr = ReadSystem(cpu, HV_SYS_REG_TPIDR_EL0);
    context.fpcr = shadow.fpcr = static_cast<uint32_t>(Read(cpu, HV_REG_FPCR));
    context.fpsr = shadow.fpsr = static_cast<uint32_t>(Read(cpu, HV_REG_FPSR));
    if (compat.simd) {
        SyncVectors(context);
        context.vectors_dirty = true; // upload again on the next run
    }

    if (pc < KernelBase) {
        context.pc = pc;
        context.pstate = static_cast<uint32_t>(Read(cpu, HV_REG_CPSR));
        if (exit->reason == HV_EXIT_REASON_EXCEPTION)
            return {Exit::Kind::Exception, exit->exception.syndrome,
                    exit->exception.virtual_address};
        return {Exit::Kind::Interrupted};
    }
    // Stopped in EL1 support code.
    const uint64_t elr = ReadSystem(cpu, HV_SYS_REG_ELR_EL1);
    const uint64_t spsr = ReadSystem(cpu, HV_SYS_REG_SPSR_EL1);
    if (exit->reason == HV_EXIT_REASON_EXCEPTION &&
        exit->exception.syndrome == DispatchSyndrome) {
        const uint64_t esr = ReadSystem(cpu, HV_SYS_REG_ESR_EL1);
        if (elr >= KernelBase) {
            throw std::runtime_error("Exception inside Hypervisor EL1 code: elr=" + Hex(elr) +
                                     " esr=" + Hex(esr));
        }
        context.pc = elr;
        context.pstate = static_cast<uint32_t>(spsr);
        return {Exit::Kind::Exception, esr, ReadSystem(cpu, HV_SYS_REG_FAR_EL1)};
    }
    if (exit->reason != HV_EXIT_REASON_CANCELED) {
        throw std::runtime_error("Unexpected Hypervisor exit in EL1 code: reason=" +
                                 std::to_string(exit->reason) + " pc=" + Hex(pc) +
                                 " syndrome=" + Hex(exit->exception.syndrome));
    }
    // Canceled inside the stub or a trap handler. Recover x0-x2 if the handler
    // saved them before deciding whether to deliver the exception or resume EL0.
    // In-VM counter/wait handlers are restartable; host-dispatched SVCs are not.
    if (pc >= VectorAddress && pc < VectorAddress + 0x1000) {
        const uint64_t sp1 = ReadSystem(cpu, HV_SYS_REG_SP_EL1);
        if (sp1 == frame_top - 32) {
            const auto* saved = vm->Frame(slot);
            context.regs[0] = saved[0];
            context.regs[1] = saved[1];
            if (pc != VectorAddress + 0x404) // str x2 not yet executed: x2 is live
                context.regs[2] = saved[2];
        }
        if (sp1 != frame_top)
            WriteSystem(cpu, HV_SYS_REG_SP_EL1, frame_top);
    }
    context.pc = elr;
    context.pstate = static_cast<uint32_t>(spsr);
    if (pc >= VectorAddress && pc < VectorAddress + 0x1000) {
        const uint64_t esr = ReadSystem(cpu, HV_SYS_REG_ESR_EL1);
        const uint64_t ec = esr >> 26;
        // SVC sets ELR to the following instruction. Returning Interrupted here
        // would skip the system call entirely. Deliver host-handled exceptions
        // even when cancellation arrives before their dispatch HVC executes.
        // The in-VM counter/wait handlers are restartable and may have already
        // advanced ELR, so those retain their interrupted-resume path.
        if (ec != 0x18 && ec != 0x01) {
            return {Exit::Kind::Exception, esr, ReadSystem(cpu, HV_SYS_REG_FAR_EL1)};
        }
    }
    return {Exit::Kind::Interrupted};
}
} // namespace Core::Hypervisor
