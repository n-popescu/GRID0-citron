// Synthetic ARM64 cache regression: no game files or keys required.
#include <array>
#include <cstring>
#include <iostream>
#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/A64/config.h>

struct Environment final : Dynarmic::A64::UserCallbacks {
    alignas(16) std::array<unsigned char, 4096> memory{};
    std::size_t code_reads{};
    std::uint64_t ticks = 4;
    bool failed = false;
    std::optional<std::uint32_t> MemoryReadCode(std::uint64_t address) override {
        ++code_reads;
        constexpr std::array<std::uint32_t, 4> program{0xd2800540, 0xf9000020, 0xf9400022, 0x14000000};
        if (address < program.size() * 4) return program[address / 4];
        return std::nullopt;
    }
    template<class T> T Read(std::uint64_t address) {
        T value{};
        if (address < 0x1000 || address - 0x1000 + sizeof(T) > memory.size()) {
            failed = true; return value;
        }
        std::memcpy(&value, memory.data() + address - 0x1000, sizeof(T));
        return value;
    }
    template<class T> void Write(std::uint64_t address, T value) {
        if (address < 0x1000 || address - 0x1000 + sizeof(T) > memory.size()) { failed = true; return; }
        std::memcpy(memory.data() + address - 0x1000, &value, sizeof(T));
    }
    std::uint8_t MemoryRead8(std::uint64_t a) override { return Read<std::uint8_t>(a); }
    std::uint16_t MemoryRead16(std::uint64_t a) override { return Read<std::uint16_t>(a); }
    std::uint32_t MemoryRead32(std::uint64_t a) override { return Read<std::uint32_t>(a); }
    std::uint64_t MemoryRead64(std::uint64_t a) override { return Read<std::uint64_t>(a); }
    Dynarmic::A64::Vector MemoryRead128(std::uint64_t a) override { return Read<Dynarmic::A64::Vector>(a); }
    void MemoryWrite8(std::uint64_t a, std::uint8_t v) override { Write(a,v); }
    void MemoryWrite16(std::uint64_t a, std::uint16_t v) override { Write(a,v); }
    void MemoryWrite32(std::uint64_t a, std::uint32_t v) override { Write(a,v); }
    void MemoryWrite64(std::uint64_t a, std::uint64_t v) override { Write(a,v); }
    void MemoryWrite128(std::uint64_t a, Dynarmic::A64::Vector v) override { Write(a,v); }
    void CallSVC(std::uint32_t) override { failed = true; }
    void ExceptionRaised(std::uint64_t, Dynarmic::A64::Exception) override { failed = true; }
    void AddTicks(std::uint64_t value) override { ticks = value < ticks ? ticks - value : 0; }
    std::uint64_t GetTicksRemaining() override { return ticks; }
    std::uint64_t GetCNTPCT() override { return 0; }
};
int main() {
    for (bool use_page_table : {false, true}) {
        Environment env;
        struct PageEntry { std::uintptr_t pointer{}; std::uint64_t padding[3]{}; };
        std::array<PageEntry, 4096> pages{};
        pages[1].pointer = (reinterpret_cast<std::uintptr_t>(env.memory.data()) - 0x1000) | 1;
        Dynarmic::A64::UserConfig config;
        config.callbacks = &env;
        config.code_cache_size = 16 * 1024 * 1024;
        if (use_page_table) {
            config.page_table = reinterpret_cast<void**>(pages.data());
            config.page_table_log2_stride = 5;
            config.page_table_pointer_mask_bits = 2;
            config.absolute_offset_page_table = true;
            config.page_table_address_space_bits = 24;
        }
        Dynarmic::A64::Jit jit(config);
        jit.SetPC(0);
        jit.SetRegister(1, 0x1000);
        jit.Run();
        if (env.failed || jit.GetRegister(0) != 42 || jit.GetRegister(2) != 42 || env.Read<std::uint64_t>(0x1000) != 42) return 1;
        const auto initial_reads = env.code_reads;
        env.ticks = 4;
        jit.SetPC(0);
        jit.Run();
        if (env.code_reads != initial_reads) {
            std::cerr << "Compiled blocks were not retained in the JIT cache\n";
            return 1;
        }
        jit.InvalidateCacheRange(0, 16);
        env.ticks = 4;
        jit.SetPC(0);
        jit.Run();
        if (env.code_reads <= initial_reads || env.failed || jit.GetRegister(2) != 42) {
            std::cerr << "Invalidation did not recompile the guest block correctly\n";
            return 1;
        }
        std::cout << (use_page_table ? "page-table" : "callback") << " JIT execution passed\n";
    }
}
