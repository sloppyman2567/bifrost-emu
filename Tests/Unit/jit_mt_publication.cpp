// Host-side invariants that a guest result oracle cannot observe:
// published instruction bytes remain immutable, and chains have bounded leases.
#include "System/Core/Emulator.hpp"
#include "System/Core/DebugFlags.hpp"
#include "System/Core/Signals.hpp"
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>
using namespace arm64emu;
static bool check_signal_return(bool mt) {
    auto e = std::make_unique<Emulator>();
    constexpr uint64_t handler = 0x50100, resume = handler + 64;
    constexpr uint64_t stack = 0x60000, top = stack + 16384, stop = 0x80000;
    if (!e->mem().map_range(handler & ~4095ULL, 4096) || !e->mem().map_range(stack, 16384))
        return false;
    const uint32_t ret = 0xd65f03c0;
    const uint32_t continuation[] = {0x91000673, ret}; // add x19,x19,#1; ret
    e->mem().write(handler, &ret, sizeof(ret));
    e->mem().write(resume, continuation, sizeof(continuation));
    if (!e->mem().mprotect_guest(handler & ~4095ULL, 4096, 5)) return false;
    const uint64_t action[] = {handler, SA_SIGINFO_EMU, 0, 0};
    e->mem().write(stack, action, sizeof(action));
    if (e->signals().install(e->mem(), BIFROST_SIGUSR1, stack, 0)) return false;
    e->enable_jit();
    auto* j = e->jit();
    if (mt) j->enter_multithreaded();
    // Warm the continuation too: sigreturn must leave through the SVC's
    // cold exit even when both its trampoline and restored PC have IBL hits.
    CPU warm;
    warm.pc = resume; warm.regs[30] = stop;
    if (j->run_block(warm, *e) != stop || warm.regs[19] != 1) return false;
    for (unsigned repeat = 0; repeat < 3; ++repeat) {
        CPU cpu;
        cpu.pc = resume; cpu.sp = top; cpu.regs[30] = stop;
        cpu.regs[19] = 41; cpu.pstate = 0xa0000000;
        if (!deliver_signal(*e, cpu, e->signals(), BIFROST_SIGUSR1)) return false;
        const uint64_t next = j->run_block(cpu, *e);
        const bool chained = repeat && FrostJIT::chain_skip_enabled() && !dbg().no_chain;
        if (next != (chained ? resume : TRAMPOLINE_ADDR)) return false;
        if (!chained && j->run_block(cpu, *e) != resume) return false;
        if (!cpu.sig_frames.empty() || cpu.sp != top || cpu.regs[19] != 41 ||
            cpu.regs[30] != stop || cpu.sigmask || (cpu.pstate & 0xf0000000) != 0xa0000000)
            return false;
    }
    // Executable host stop addresses and nested helper return boundaries
    // must still exit before running a warmed target's first instruction.
    for (unsigned boundary = 0; boundary < 2; ++boundary) {
        CPU cpu;
        cpu.pc = handler; cpu.regs[30] = resume; cpu.regs[19] = 41;
        cpu.jit_stop_pc = boundary == 0 ? resume : 0;
        cpu.jit_call_depth = boundary == 1 ? 1 : 0;
        if (j->run_block(cpu, *e) != resume || cpu.regs[19] != 41) return false;
    }
    std::printf("jit_mt_publication: %s warmed signal return and host boundaries PASS\n",
                mt ? "MT" : "ST");
    return true;
}
static bool check_selfloops() {
    auto e = std::make_unique<Emulator>();
    constexpr uint64_t head = 0x30000, stop = 0x40000;
    e->mem().map_range(head, 4096);
    e->mem().mprotect_guest(head, 4096, 7);
    e->enable_jit();
    auto* j = e->jit();
    j->enter_multithreaded();
    const std::vector<std::vector<uint32_t>> loops = {
        {0x8b020021,0x91000442,0xf1000400,0x54ffffa1,0xd65f03c0}, // SUBS/B.NE
        {0x8b020021,0x91000442,0xd1000400,0xb5ffffa0,0xd65f03c0}, // CBNZ
        {0x8b020021,0x91000442,0x91000400,0x3647ffa0,0xd65f03c0}, // TBZ bit 8
        {0x4e61d400,0x8b020021,0x91000442,0xd1000400,0xb5ffff80,0xd65f03c0}, // FADD/CBNZ
        {0xba020021,0x91000442,0xf1000400,0x54ffffa1,0xd65f03c0}, // loop-carried C
    };
    for (unsigned kind = 0; kind < loops.size(); ++kind) {
        j->invalidate_all();
        e->mem().write(head, loops[kind].data(), loops[kind].size() * 4);
        auto initialize = [&](CPU& cpu) {
            cpu.pc = head; cpu.regs[0] = kind == 2 ? 0 : 5000;
            cpu.regs[1] = 7; cpu.regs[30] = stop; cpu.jit_stop_pc = stop;
            cpu.pstate = 0xa0000000;
            const double one = 1.0, three = 3.0;
            std::memcpy(&cpu.v_lo[1], &one, 8);
            std::memcpy(&cpu.v_hi[1], &three, 8);
        };
        std::atomic<unsigned> failures{0};
        auto run = [&] {
            CPU cpu; initialize(cpu);
            unsigned dispatches = 0;
            while (cpu.pc != stop && ++dispatches < 10000)
                cpu.pc = j->run_block(cpu, *e);
            const uint64_t n = kind == 2 ? 256 : 5000;
            const uint32_t nzcv = kind == 0 || kind == 4 ? 0x60000000 : 0xa0000000;
            double lo, hi;
            std::memcpy(&lo, &cpu.v_lo[0], 8); std::memcpy(&hi, &cpu.v_hi[0], 8);
            if (cpu.pc != stop || cpu.regs[1] != n * (n - 1) / 2 + 7 + (kind == 4 ? n : 0) ||
                cpu.regs[2] != n || (cpu.pstate & 0xf0000000) != nzcv ||
                (kind == 3 && (lo != double(n) || hi != 3.0 * n))) ++failures;
        };
        run(); // compile before the instruction-byte snapshot
        const size_t prefix = j->code_buf_used();
        std::vector<uint8_t> before(j->code_buf(), j->code_buf() + prefix);
        std::atomic<bool> start{false};
        std::vector<std::thread> workers;
        for (unsigned k = 0; k < 8; ++k) workers.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for (unsigned repeat = 0; repeat < 32; ++repeat) run();
        });
        start.store(true, std::memory_order_release);
        for (unsigned repeat = 0; repeat < 100; ++repeat) j->invalidate_range(head, 4);
        for (auto& t : workers) t.join();
        if (std::memcmp(before.data(), j->code_buf(), prefix)) ++failures;
        // Expiry and callback sentinels must commit all carried state,
        // including the final comparison flags and dirty vector accumulators.
        auto fn = j->lookup_or_translate(*e, head);
        for (unsigned boundary = 0; boundary < 3; ++boundary) {
            CPU cpu; initialize(cpu);
            cpu.jit_chain_budget = boundary == 0 ? 256 : boundary == 1 ? 0 : 256;
            if (boundary == 2) cpu.jit_stop_pc = head;
            cpu.pc = fn(&cpu, e.get());
            const uint64_t n = cpu.regs[2];
            const uint32_t nzcv = kind == 0 || kind == 4 ? 0x20000000 : 0xa0000000;
            const uint64_t next_pc = kind == 2 && n == 256 ? head + 16 : head;
            double lo, hi;
            std::memcpy(&lo, &cpu.v_lo[0], 8); std::memcpy(&hi, &cpu.v_hi[0], 8);
            if (!n || n > 256 || (boundary && n != 1) || cpu.pc != next_pc ||
                cpu.regs[1] != n * (n - 1) / 2 + 7 + (kind == 4 ? n : 0) ||
                cpu.regs[0] != (kind == 2 ? n : 5000 - n) ||
                (cpu.pstate & 0xf0000000) != nzcv ||
                (kind == 3 && (lo != double(n) || hi != 3.0 * n))) ++failures;
        }
        j->invalidate_range(head, 4);
        // Retranslation must not revive a retired loop's body cell.
        j->lookup_or_translate(*e, head);
        CPU retired; initialize(retired); retired.jit_chain_budget = 256;
        retired.pc = fn(&retired, e.get());
        if (retired.pc != head || retired.regs[2] != 1) ++failures;
        // A synchronized code rewrite must execute the new arithmetic,
        // rather than reconnecting a body edge to retired instructions.
        const uint32_t add_three = 0x91000c21;
        e->mem().write(head + (kind == 3 ? 4 : 0), &add_three, 4);
        j->invalidate_range(head, 4);
        CPU rewritten; initialize(rewritten);
        unsigned dispatches = 0;
        while (rewritten.pc != stop && ++dispatches < 10000)
            rewritten.pc = j->run_block(rewritten, *e);
        const uint64_t count = kind == 2 ? 256 : 5000;
        if (rewritten.pc != stop || rewritten.regs[1] != 3 * count + 7) ++failures;
        if (failures) {
            std::fprintf(stderr, "MT self-loop kind=%u: %u failures\n", kind, failures.load());
            return false;
        }
    }
    std::puts("jit_mt_publication: self-loop arithmetic, flags, vectors and revocation PASS");
    return true;
}
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
    if (!check_signal_return(false) || !check_signal_return(true) || !check_selfloops()) return 1;
    std::puts("jit_mt_publication: immutable bytes and bounded leases PASS");
}
