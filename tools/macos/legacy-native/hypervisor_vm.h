// SPDX-License-Identifier: GPL-2.0-or-later
// LEGACY (pre-October-4-evening) native backend, kept for A/B diagnosis only.
#pragma once
#include <Hypervisor/Hypervisor.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace Core::Hypervisor {
struct Context {
    std::array<uint64_t, 31> regs{};
    std::array<std::array<uint64_t, 2>, 32> vectors{};
    uint64_t pc{}, sp{}, tpidr{}, tpidrro{};
    uint32_t pstate{}, fpcr{}, fpsr{};
};
struct Exit {
    enum class Kind { Exception, Interrupted } kind;
    uint64_t syndrome{}, address{};
};
class Vm : public std::enable_shared_from_this<Vm> {
public:
    Vm(void* backing, size_t size);
    ~Vm();
    Vm(const Vm&) = delete;
    Vm& operator=(const Vm&) = delete;
    uint64_t AllocateTable();
    uint64_t* Table(uint64_t physical);
    void Interrupt();
    void RegisterCpu(hv_vcpu_t cpu);
    void UnregisterCpu(hv_vcpu_t cpu);
    uint64_t KernelRoot() const { return kernel_root; }
    size_t BackingSize() const { return backing_size; }

private:
    void* pool{};
    size_t backing_size{}, next_page{};
    uint64_t kernel_root{};
    std::mutex mutex;
    std::vector<hv_vcpu_t> cpus;
};
class AddressSpace {
public:
    explicit AddressSpace(std::shared_ptr<Vm> vm);
    bool Map(uint64_t virtual_address, uint64_t physical, unsigned permissions,
             uint64_t expected_generation = UINT64_MAX);
    uint64_t Generation();
    void Invalidate(uint64_t address, uint64_t size);
    uint64_t Root() const { return root; }
    std::shared_ptr<Vm> GetVm() const { return vm; }

private:
    uint64_t* Entry(uint64_t address);
    std::shared_ptr<Vm> vm;
    uint64_t root{}, generation{};
    std::mutex mutex;
    std::unordered_map<uint64_t, uint64_t*> mapped;
};
class Cpu {
public:
    explicit Cpu(std::shared_ptr<Vm> vm);
    ~Cpu();
    Exit Run(Context& context, AddressSpace& space);
    hv_vcpu_t Id() const { return cpu; }

private:
    std::shared_ptr<Vm> vm;
    hv_vcpu_t cpu{};
    hv_vcpu_exit_t* exit{};
};
} // namespace Core::Hypervisor
