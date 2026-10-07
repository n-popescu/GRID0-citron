// SPDX-License-Identifier: GPL-2.0-or-later
// Uses the production VM implementation; no game files or keys required.
#include "core/arm/hypervisor/hypervisor_vm.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <mach/mach_time.h>
#include <sys/mman.h>
#include <thread>
using namespace Core::Hypervisor;
int main() {
    void* ram = mmap(nullptr, 16384, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (ram == MAP_FAILED)
        return 1;
    try {
        auto vm = std::make_shared<Vm>(ram, 16384);
        AddressSpace space{vm};
        // mov x0,#42; str x0,[x1]; ldr x2,[x1]; mrs x3,tpidr_el0; svc #7
        constexpr uint32_t code[]{0xd2800540, 0xf9000020, 0xf9400022, 0xd53bd043, 0xd40000e1};
        std::memcpy(ram, code, sizeof(code));
        constexpr uint64_t text = 0x100000000, data = text + 0x2000;
        space.Map(text, 0, 5);
        space.Map(data, 0x2000, 3);
        Cpu cpu{vm};
        Context context{};
        context.pc = text;
        context.regs[1] = data;
        context.tpidr = 0x11223344;
        context.regs[18] = 0xfeedabcd;
        context.vectors[8] = {0x12345678, 0xabcdef12};
        const auto exit = cpu.Run(context, space);
        cpu.SyncVectors(context);
        if (exit.syndrome != 0x56000007 || context.pc != text + sizeof(code) ||
            context.regs[2] != 42 || context.regs[3] != context.tpidr ||
            context.regs[18] != 0xfeedabcd ||
            context.vectors[8] != std::array<uint64_t, 2>{0x12345678, 0xabcdef12}) {
            std::cerr << "Unexpected native result: syndrome=" << std::hex << exit.syndrome
                      << " pc=" << context.pc << " x2=" << context.regs[2] << '\n';
            return 1;
        }
        space.Invalidate(data, 4096);
        context.pc = text;
        auto fault = cpu.Run(context, space);
        if (fault.kind == Exit::Kind::Interrupted)
            fault = cpu.Run(context, space);
        if ((fault.syndrome >> 26) != 0x24 || fault.address != data) {
            std::cerr << "fault " << std::hex << fault.syndrome << " addr " << fault.address
                      << " pc " << context.pc << "\n";
            return 1;
        }
        // Remap one 4 KB guest page within the same 16 KB host allocation.
        const auto stale_generation = space.Generation();
        space.Invalidate(data, 4096);
        if (space.Map(data, 0x3000, 3, stale_generation)) {
            std::cerr << "Stale mapping survived invalidation\n";
            return 1;
        }
        space.Map(data, 0x3000, 3);
        auto resumed = cpu.Run(context, space);
        if (resumed.kind == Exit::Kind::Interrupted) resumed = cpu.Run(context, space);
        uint64_t value{};
        std::memcpy(&value, static_cast<char*>(ram) + 0x3000, 8);
        if (resumed.syndrome != 0x56000007 || value != 42)
            return 1;
        // WFE can either consume a pending event and fall through, or trap
        // with ELR at the wait hint. Both must reach the following SVC.
        constexpr uint32_t wait_code[]{0xd503205f, 0xd40000e1};
        std::memcpy(static_cast<char*>(ram) + 0x200, wait_code, sizeof(wait_code));
        vm->Flush(true); // new code: invalidate stale instruction cache lines
        context.pc = text + 0x200;
        auto wait_exit = cpu.Run(context, space);
        if ((wait_exit.syndrome >> 26) == 1 && context.pc == text + 0x200) {
            context.pc += 4;
            wait_exit = cpu.Run(context, space);
        }
        if (wait_exit.syndrome != 0x56000007 || context.pc != text + 0x208) {
            std::cerr << "WFE did not resume at the following SVC: kind="
                      << static_cast<int>(wait_exit.kind) << " syndrome=" << std::hex
                      << wait_exit.syndrome << " pc=" << context.pc << '\n';
            return 1;
        }
        std::atomic<bool> failed{};
        // EL0 counter reads are scaled to 19.2 MHz inside the VM without exiting, on the
        // same timeline as the host clock (mach_absolute_time).
        // mrs x4,cntvct_el0; mrs x5,cntfrq_el0; mrs x6,ctr_el0; svc #7
        constexpr uint32_t counter_code[]{0xd53be044, 0xd53be005, 0xd53b0026, 0xd40000e1};
        std::memcpy(static_cast<char*>(ram) + 0x500, counter_code, sizeof(counter_code));
        vm->Flush(true); // new code: invalidate stale instruction cache lines
        const auto scale = [&](uint64_t ticks) {
            return static_cast<uint64_t>((static_cast<unsigned __int128>(ticks) *
                                          vm->TickFactor()) >> 64);
        };
        context.pc = text + 0x500;
        const auto counter_before = scale(mach_absolute_time());
        auto counter_exit = cpu.Run(context, space);
        while (counter_exit.kind == Exit::Kind::Interrupted)
            counter_exit = cpu.Run(context, space);
        const auto counter_after = scale(mach_absolute_time());
        if (counter_exit.syndrome != 0x56000007 || context.regs[5] != 19200000 ||
            context.regs[6] != 0x8444c004 || context.regs[4] < counter_before ||
            context.regs[4] > counter_after) {
            std::cerr << "Native counter emulation failed: x4=" << context.regs[4] << " window=["
                      << counter_before << ", " << counter_after << "] x5=" << context.regs[5]
                      << " offset=" << cpu.CounterOffset() << '\n';
            return 1;
        }
        // Invalidate must not return while another vCPU can still use the old translation.
        // ldr x2,[x1]; b .-4
        constexpr uint32_t reader[]{0xf9400022, 0x17ffffff};
        std::memcpy(static_cast<char*>(ram) + 0x600, reader, sizeof(reader));
        vm->Flush(true); // new code: invalidate stale instruction cache lines
        std::atomic<bool> reading{};
        std::atomic<bool> invalidated{};
        std::thread shootdown{[&] {
            try {
                Cpu core{vm};
                Context state{};
                state.pc = text + 0x600;
                state.regs[1] = data;
                Exit result{};
                reading = true;
                do {
                    result = core.Run(state, space);
                } while (result.kind == Exit::Kind::Interrupted);
                if (!invalidated || (result.syndrome >> 26) != 0x24 || result.address != data)
                    failed = true;
            } catch (...) {
                failed = true;
            }
        }};
        while (!reading)
            std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        invalidated = true;
        space.Invalidate(data, 4096);
        shootdown.join();
        if (failed) {
            std::cerr << "Stale translation survived Invalidate\n";
            return 1;
        }
        space.Map(data, 0x3000, 3);
        // Two vCPUs increment shared memory using native ARM exclusive accesses.
        constexpr uint32_t atomic_code[]{0xc85f7c20, 0x91000400, 0xc8027c20, 0x35ffffa2,
                                         0xd1000463, 0xb5ffff63, 0xd40000e1};
        std::memcpy(static_cast<char*>(ram) + 0x100, atomic_code, sizeof(atomic_code));
        vm->Flush(true); // new code: invalidate stale instruction cache lines
        value = 0;
        std::memcpy(static_cast<char*>(ram) + 0x3000, &value, 8);
        auto worker = [&] {
            try {
                Cpu core{vm};
                Context state{};
                state.pc = text + 0x100;
                state.regs[1] = data;
                state.regs[3] = 10000;
                const auto result = core.Run(state, space);
                if (result.syndrome != 0x56000007)
                    failed = true;
            } catch (...) {
                failed = true;
            }
        };
        std::thread first{worker}, second{worker};
        first.join();
        second.join();
        std::memcpy(&value, static_cast<char*>(ram) + 0x3000, 8);
        if (failed || value != 20000) {
            std::cerr << "Native shared exclusives failed: " << value << '\n';
            return 1;
        }
        // Repeated scheduler interrupts must preserve guest PCs and deliver every SVC.
        // The guest increments x19 after each call; the host counts delivered exceptions.
        constexpr uint32_t calls[]{0xd40000e1, 0x91000673, 0x17fffffe};
        std::memcpy(static_cast<char*>(ram) + 0x400, calls, sizeof(calls));
        vm->Flush(true); // new code: invalidate stale instruction cache lines
        std::promise<void> calls_ready;
        std::thread calling{[&] {
            try {
                Cpu core{vm};
                Context state{};
                state.pc = text + 0x400;
                state.regs[0] = 0x84000000; // PSCI_VERSION must remain a guest SVC argument.
                calls_ready.set_value();
                unsigned delivered = 0;
                for (unsigned i = 0; delivered < 5000 && i < 100000; ++i) {
                    const auto result = core.Run(state, space);
                    if (result.kind == Exit::Kind::Exception && state.regs[19] != delivered) {
                        std::cerr << "Lost SVC during interruption: delivered=" << delivered
                                  << " guest_completed=" << state.regs[19] << '\n';
                        failed = true;
                        break;
                    }
                    if (result.kind == Exit::Kind::Exception) {
                        ++delivered;
                    }
                    if (state.pc < text || state.pc >= text + 4096 ||
                        (result.kind == Exit::Kind::Exception && result.syndrome != 0x56000007)) {
                        failed = true;
                        break;
                    }
                }
                if (delivered != 5000) {
                    failed = true;
                }
            } catch (...) {
                failed = true;
            }
        }};
        calls_ready.get_future().wait();
        for (unsigned i = 0; i < 10000; ++i) {
            vm->Interrupt();
            std::this_thread::sleep_for(std::chrono::microseconds(1));
        }
        calling.join();
        if (failed) {
            std::cerr << "Interrupted SVC state restoration failed\n";
            return 1;
        }
        constexpr uint32_t spin = 0x14000000;
        std::memcpy(static_cast<char*>(ram) + 0x300, &spin, 4);
        vm->Flush(true);
        std::promise<void> ready;
        std::thread spinning{[&] {
            try {
                Cpu core{vm};
                Context state{};
                state.pc = text + 0x300;
                ready.set_value();
                if (core.Run(state, space).kind != Exit::Kind::Interrupted)
                    failed = true;
            } catch (...) {
                failed = true;
            }
        }};
        ready.get_future().wait();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        vm->Interrupt();
        spinning.join();
        if (failed)
            return 1;
        std::cout << "Native ARM64, SVC, TLS/SIMD/x18, 4 KB remapping, EL1 counter/CTR/WFE "
                     "traps, TLB shootdown, multicore exclusives and interruption passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    munmap(ram, 16384);
}
