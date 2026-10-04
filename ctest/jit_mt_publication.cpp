// Host-side invariants that a guest result oracle cannot observe:
// published instruction bytes remain immutable, and chains have bounded leases.
#include "core/emulator.h"
#include "jit/frostjit.hpp"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>
using namespace arm64emu;
int main() {
    auto e = std::make_unique<Emulator>();
    e->mem().map_range(0x10000, 8192);
    e->mem().mprotect_guest(0x10000, 8192, 7);
    const uint32_t code[] = {0xd2800001,0xd2800002,0xeb00005f,0x540000a0,
        0x8b020021,0x91000c21,0x91000442,0x17fffffb,0xaa0103e0,0xd65f03c0};
    constexpr uint64_t entry = 0x10ff0, head = entry + 8, body = 0x11000;
    constexpr uint64_t stop = 0x20000;
    e->mem().write(entry, code, sizeof(code));
    e->enable_jit();
    auto* j = e->jit();
    if (!j) return 2;
    j->enter_multithreaded();
    for (uint64_t pc : {entry, head, body, body + 16}) j->lookup_or_translate(*e, pc);
    const size_t prefix = j->code_buf_used();
    std::vector<uint8_t> before(j->code_buf(), j->code_buf() + prefix);
    std::atomic<unsigned> failures{0};
    auto run = [&] {
        CPU cpu;
        cpu.pc = entry; cpu.regs[0] = 20000;
        cpu.regs[30] = stop; cpu.jit_stop_pc = stop;
        unsigned dispatches = 0;
        while (cpu.pc != stop && ++dispatches < 10000)
            cpu.pc = j->run_block(cpu, *e);
        if (cpu.pc != stop || cpu.regs[0] != 20000ull * 19999 / 2 + 3 * 20000)
            ++failures;
    };
    std::vector<std::thread> threads;
    for (unsigned k = 0; k < 8; ++k) threads.emplace_back(run);
    for (auto& t : threads) t.join();
    if (!j->tier2_regions.load() || std::memcmp(before.data(), j->code_buf(), prefix)) ++failures;
    j->invalidate_range(body, 24); // must revoke a region whose head is on another page
    if (std::memcmp(before.data(), j->code_buf(), prefix)) ++failures;
    run();
    if (std::memcmp(before.data(), j->code_buf(), prefix)) ++failures;
    CPU bounded;
    bounded.pc = entry; bounded.regs[0] = 1000000000;
    bounded.regs[30] = stop; bounded.jit_stop_pc = stop;
    bounded.pc = j->run_block(bounded, *e);
    if (!bounded.regs[2] || bounded.regs[2] > 1024 || bounded.pc == stop) ++failures;
    j->invalidate_all();
    if (std::memcmp(before.data(), j->code_buf(), prefix)) ++failures;
    if (failures) { std::fprintf(stderr, "MT publication: %u failures\n", failures.load()); return 1; }
    std::puts("jit_mt_publication: immutable bytes and bounded leases PASS");
}
