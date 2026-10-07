// SPDX-License-Identifier: GPL-2.0-or-later
// LEGACY (pre-October-4-evening) native backend, kept for A/B diagnosis only.
#include "core/arm/hypervisor/hypervisor_vm.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <sys/mman.h>

namespace Core::Hypervisor {
namespace {
constexpr uint64_t PoolBase = 0x800000000ULL;
constexpr size_t PoolSize = 64 * 1024 * 1024;
constexpr uint64_t KernelBase = 0xffffff8000000000ULL;
constexpr uint64_t VectorAddress = KernelBase + 0x1000;
constexpr uint64_t FlushAddress = KernelBase + 0x2000;
constexpr uint64_t AddressMask = 0x000000fffffff000ULL;
void Check(hv_return_t result, const char* operation) {
    if (result != HV_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(result));
    }
}
uint64_t Read(hv_vcpu_t cpu, hv_reg_t reg) {
    uint64_t value{};
    Check(hv_vcpu_get_reg(cpu, reg, &value), "hv_vcpu_get_reg");
    return value;
}
uint64_t ReadSystem(hv_vcpu_t cpu, hv_sys_reg_t reg) {
    uint64_t value{};
    Check(hv_vcpu_get_sys_reg(cpu, reg, &value), "hv_vcpu_get_sys_reg");
    return value;
}
void WriteSystem(hv_vcpu_t cpu, hv_sys_reg_t reg, uint64_t value) {
    Check(hv_vcpu_set_sys_reg(cpu, reg, value), "hv_vcpu_set_sys_reg");
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
        next_page = 0x4000;
        kernel_root = AllocateTable();
        const auto middle = AllocateTable();
        const auto leaf = AllocateTable();
        Table(kernel_root)[0] = middle | 3;
        Table(middle)[0] = leaf | 3;
        for (unsigned i = 0; i < 4; ++i) {
            Table(leaf)[i] = (PoolBase + i * 0x1000) | 3 | (1ULL << 10) | (3ULL << 8);
        }
        auto* vectors = reinterpret_cast<uint32_t*>(static_cast<char*>(pool) + 0x1000);
        for (unsigned i = 0; i < 0x800 / 4; ++i)
            vectors[i] = 0xd4200000; // brk
        for (unsigned i = 0; i < 16; ++i)
            vectors[i * 32] = 0xd4182ea2; // hvc #0xc175
        // clrex; tlbi vmalle1is; dsb ish; ic iallu; dsb ish; isb; hvc #1
        constexpr uint32_t flush[]{0xd5033f5f, 0xd508831f, 0xd5033b9f, 0xd508751f,
                                   0xd5033b9f, 0xd5033fdf, 0xd4000022};
        std::memcpy(static_cast<char*>(pool) + 0x2000, flush, sizeof(flush));
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
void Vm::RegisterCpu(hv_vcpu_t cpu) {
    std::lock_guard lock{mutex};
    cpus.push_back(cpu);
}
void Vm::UnregisterCpu(hv_vcpu_t cpu) {
    std::lock_guard lock{mutex};
    std::erase(cpus, cpu);
}
void Vm::Interrupt() {
    std::lock_guard lock{mutex};
    if (!cpus.empty())
        hv_vcpus_exit(cpus.data(), static_cast<uint32_t>(cpus.size()));
}
AddressSpace::AddressSpace(std::shared_ptr<Vm> vm_)
    : vm{std::move(vm_)}, root{vm->AllocateTable()} {}
uint64_t* AddressSpace::Entry(uint64_t address) {
    if (address >= (1ULL << 39))
        throw std::runtime_error("Hypervisor virtual address exceeds 39 bits");
    auto* table = vm->Table(root);
    for (const unsigned shift : {30, 21}) {
        auto& entry = table[(address >> shift) & 511];
        if (!entry)
            entry = vm->AllocateTable() | 3;
        table = vm->Table(entry & AddressMask);
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
    std::lock_guard lock{mutex};
    if (expected_generation != UINT64_MAX && generation != expected_generation)
        return false;
    auto* entry = Entry(address);
    uint64_t descriptor = physical | 3 | (1ULL << 10) | (3ULL << 8) | (1ULL << 53);
    descriptor |= (permissions & 2 ? 1ULL : 3ULL) << 6;
    if (!(permissions & 4))
        descriptor |= 1ULL << 54; // UXN
    if (!(permissions & 1))
        descriptor = 0;
    std::atomic_ref<uint64_t>(*entry).store(descriptor, std::memory_order_release);
    mapped[address] = entry;
    return true;
}
void AddressSpace::Invalidate(uint64_t address, uint64_t size) {
    if (!size || size > (1ULL << 39) || address >= (1ULL << 39))
        return;
    const auto first = address & ~4095ULL;
    const auto last = std::min((address + size + 4095) & ~4095ULL, 1ULL << 39);
    bool changed = false;
    {
        std::lock_guard lock{mutex};
        ++generation;
        for (uint64_t page = first; page < last; page += 4096) {
            if (auto it = mapped.find(page); it != mapped.end()) {
                std::atomic_ref<uint64_t>(*it->second).store(0, std::memory_order_release);
                changed = true;
            }
        }
    }
    if (changed)
        vm->Interrupt();
}
Cpu::Cpu(std::shared_ptr<Vm> vm_) : vm{std::move(vm_)} {
    Check(hv_vcpu_create(&cpu, &exit, nullptr), "hv_vcpu_create");
    try {
        WriteSystem(cpu, HV_SYS_REG_MAIR_EL1, 0xff);
        WriteSystem(cpu, HV_SYS_REG_TCR_EL1,
                    25ULL | (1ULL << 8) | (1ULL << 10) | (3ULL << 12) | (25ULL << 16) |
                        (1ULL << 24) | (1ULL << 26) | (3ULL << 28) | (2ULL << 30) | (2ULL << 32));
        WriteSystem(cpu, HV_SYS_REG_TTBR1_EL1, vm->KernelRoot());
        WriteSystem(cpu, HV_SYS_REG_VBAR_EL1, VectorAddress);
        WriteSystem(cpu, HV_SYS_REG_CPACR_EL1, 3ULL << 20);
        WriteSystem(cpu, HV_SYS_REG_CNTKCTL_EL1, 0);
        Check(hv_vcpu_set_vtimer_mask(cpu, true), "mask virtual timer");
        WriteSystem(cpu, HV_SYS_REG_SCTLR_EL1, 0x30d01805);
        vm->RegisterCpu(cpu);
    } catch (...) {
        hv_vcpu_destroy(cpu);
        throw;
    }
}
Cpu::~Cpu() {
    vm->UnregisterCpu(cpu);
    hv_vcpu_destroy(cpu);
}
Exit Cpu::Run(Context& context, AddressSpace& space) {
    WriteSystem(cpu, HV_SYS_REG_TTBR0_EL1, space.Root());
    Check(hv_vcpu_set_reg(cpu, HV_REG_PC, FlushAddress), "set flush PC");
    Check(hv_vcpu_set_reg(cpu, HV_REG_CPSR, 0x3c5), "set EL1");
    Check(hv_vcpu_run(cpu), "flush translations");
    if (exit->reason == HV_EXIT_REASON_CANCELED)
        return {Exit::Kind::Interrupted};
    if (exit->reason != HV_EXIT_REASON_EXCEPTION || exit->exception.syndrome != 0x5a000001) {
        throw std::runtime_error("Hypervisor translation flush failed: " +
                                 std::to_string(exit->exception.syndrome));
    }
    for (unsigned i = 0; i < 31; ++i)
        Check(hv_vcpu_set_reg(cpu, static_cast<hv_reg_t>(HV_REG_X0 + i), context.regs[i]),
              "set GPR");
    for (unsigned i = 0; i < 32; ++i) {
        hv_simd_fp_uchar16_t value;
        std::memcpy(&value, context.vectors[i].data(), 16);
        Check(hv_vcpu_set_simd_fp_reg(cpu, static_cast<hv_simd_fp_reg_t>(HV_SIMD_FP_REG_Q0 + i),
                                      value),
              "set SIMD");
    }
    WriteSystem(cpu, HV_SYS_REG_SP_EL0, context.sp);
    WriteSystem(cpu, HV_SYS_REG_TPIDR_EL0, context.tpidr);
    WriteSystem(cpu, HV_SYS_REG_TPIDRRO_EL0, context.tpidrro);
    Check(hv_vcpu_set_reg(cpu, HV_REG_FPCR, context.fpcr), "set FPCR");
    Check(hv_vcpu_set_reg(cpu, HV_REG_FPSR, context.fpsr), "set FPSR");
    Check(hv_vcpu_set_reg(cpu, HV_REG_PC, context.pc), "set guest PC");
    Check(hv_vcpu_set_reg(cpu, HV_REG_CPSR, (context.pstate & 0xf0000000) | 0x3c0), "set EL0");
    Check(hv_vcpu_run(cpu), "run ARM64 guest");
    for (unsigned i = 0; i < 31; ++i)
        context.regs[i] = Read(cpu, static_cast<hv_reg_t>(HV_REG_X0 + i));
    for (unsigned i = 0; i < 32; ++i) {
        hv_simd_fp_uchar16_t value;
        Check(hv_vcpu_get_simd_fp_reg(cpu, static_cast<hv_simd_fp_reg_t>(HV_SIMD_FP_REG_Q0 + i),
                                      &value),
              "get SIMD");
        std::memcpy(context.vectors[i].data(), &value, 16);
    }
    context.sp = ReadSystem(cpu, HV_SYS_REG_SP_EL0);
    context.tpidr = ReadSystem(cpu, HV_SYS_REG_TPIDR_EL0);
    context.fpcr = static_cast<uint32_t>(Read(cpu, HV_REG_FPCR));
    context.fpsr = static_cast<uint32_t>(Read(cpu, HV_REG_FPSR));
    context.pc = Read(cpu, HV_REG_PC);
    context.pstate = static_cast<uint32_t>(Read(cpu, HV_REG_CPSR));
    if (context.pc >= VectorAddress && context.pc < VectorAddress + 0x800) {
        const auto guest_pc = ReadSystem(cpu, HV_SYS_REG_ELR_EL1);
        context.pc = guest_pc;
        context.pstate = static_cast<uint32_t>(ReadSystem(cpu, HV_SYS_REG_SPSR_EL1));
        return {Exit::Kind::Exception, ReadSystem(cpu, HV_SYS_REG_ESR_EL1),
                ReadSystem(cpu, HV_SYS_REG_FAR_EL1)};
    }
    if (exit->reason == HV_EXIT_REASON_CANCELED)
        return {Exit::Kind::Interrupted};
    if (exit->reason != HV_EXIT_REASON_EXCEPTION)
        throw std::runtime_error("Unexpected Hypervisor exit");
    return {Exit::Kind::Exception, exit->exception.syndrome, exit->exception.virtual_address};
}
} // namespace Core::Hypervisor
