// Host-side invariants that a guest result oracle cannot observe:
// published instruction bytes remain immutable, and native loops leave on a preemption epoch.
#include "System/Core/Emulator.hpp"
#include "System/Core/DebugFlags.hpp"
#include "System/Core/Signals.hpp"
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>
using namespace arm64emu;
static bool check_entry_order_loops(bool mt) {
    constexpr uint64_t head = 0x90000, stop = 0xa0000;
    // Warm the interior entry first, as CoreMark's list walk does. Every
    // direct conditional family must still form one five-instruction head.
    const uint32_t branches[] = {0x54ffff81, 0xb5ffff80, 0xb4ffff80,
                                 0x3647ff80, 0x3747ff80, 0x17fffffc};
    for (unsigned kind = 0; kind < sizeof(branches) / sizeof(*branches); ++kind) {
        auto e = std::make_unique<Emulator>();
        if (!e->mem().map_range(head, 4096)) return false;
        uint32_t code[] = {0xd503201f, 0x8b020021, 0x91000442,
                           0xf1000400, branches[kind], 0xd65f03c0};
        if (kind == 3) code[3] = 0x91000400; // TBZ: increment to bit 8
        e->mem().write(head, code, sizeof(code));
        if (!e->mem().mprotect_guest(head, 4096, 5)) return false;
        e->enable_jit();
        auto* j = e->jit();
        if (mt) j->enter_multithreaded();
        if (!j->lookup_or_translate(*e, head + 4)) return false;
        int count = 0;
        auto fn = j->lookup_call_target(*e, head, count);
        if (!fn || count != 5) {
            std::fprintf(stderr, "entry-order loop mt=%d kind=%u: count=%d, expected 5\n",
                         mt, kind, count);
            return false;
        }
        // All absorbed words remain dependencies: unchanged protection
        // retains the head; rewriting an interior word retires it.
        j->invalidate_range(head + 4, 4, CodeInvalidation::Protect);
        if (j->lookup_only(head) != fn) return false;
        auto run = [&](uint64_t want) {
            CPU cpu;
            cpu.pc = head; cpu.regs[0] = kind == 2 ? 1 : kind == 3 ? 0 : kind == 4 ? 511 : 5;
            cpu.regs[30] = stop; cpu.jit_stop_pc = stop;
            unsigned dispatches = 0;
            while (cpu.pc != stop && ++dispatches < 10000)
                cpu.pc = j->run_block(cpu, *e);
            return cpu.pc == stop && cpu.regs[1] == want &&
                   cpu.regs[2] == (kind == 2 ? 2 : kind == 3 || kind == 4 ? 256 : 5);
        };
        const uint64_t n = kind == 2 ? 2 : kind == 3 || kind == 4 ? 256 : 5;
        if (kind != 5 && !run(n * (n - 1) / 2)) return false;
        const uint32_t add_three = 0x91000c21;
        if (!e->mem().mprotect_guest(head, 4096, 7)) return false;
        e->mem().write(head + 4, &add_three, 4);
        if (!e->mem().mprotect_guest(head, 4096, 5)) return false;
        j->invalidate_range(head + 4, 4, CodeInvalidation::CacheMaintenance);
        if (j->lookup_only(head)) return false;
        if (kind != 5 && !run(3 * n)) return false;
    }
    for (bool oversized : {false, true}) {
        auto e = std::make_unique<Emulator>();
        if (!e->mem().map_range(head, 4096)) return false;
        std::vector<uint32_t> code(oversized ? 33 : 5, 0xd503201f);
        // A tail that loops to itself, or cannot fit with the prefix, must
        // retain the ordinary split at its known entry.
        code.back() = oversized ? 0x17ffffe0 : 0x17fffffd;
        e->mem().write(head, code.data(), code.size() * 4);
        if (!e->mem().mprotect_guest(head, 4096, 5)) return false;
        e->enable_jit();
        auto* j = e->jit();
        if (mt) j->enter_multithreaded();
        j->lookup_or_translate(*e, head + 4);
        int count = 0;
        if (!j->lookup_call_target(*e, head, count) || count != 1) return false;
    }
    std::printf("jit_mt_publication: %s entry-order loop counts, results and dependencies PASS\n",
                mt ? "MT" : "ST");
    return true;
}
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
        // A block that reaches a syscall leaves it pending, and the next run_block runs it.
        const auto dispatch = [&] {
            uint64_t pc = j->run_block(cpu, *e);
            while (cpu.jit_svc_pc) pc = j->run_block(cpu, *e);
            return pc;
        };
        const uint64_t next = dispatch();
        const bool chained = repeat && FrostJIT::chain_skip_enabled() && !dbg().no_chain;
        if (next != (chained ? resume : TRAMPOLINE_ADDR)) return false;
        if (!chained && dispatch() != resume) return false;
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
                (kind == 3 && (lo != double(n) || hi != 3.0 * n))) {
                std::fprintf(stderr, "selfloop kind %u run: pc=0x%llx x1=%llu x2=%llu flags=0x%x\n", kind,
                             (unsigned long long)cpu.pc, (unsigned long long)cpu.regs[1],
                             (unsigned long long)cpu.regs[2], cpu.pstate >> 28);
                ++failures;
            }
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
        if (std::memcmp(before.data(), j->code_buf(), prefix)) { std::fprintf(stderr, "selfloop kind %u: published bytes changed\n", kind); ++failures; }
        // Expiry and callback sentinels must commit all carried state,
        // including the final comparison flags and dirty vector accumulators.
        // 0: fresh lease, runs to the end. 1: stale epoch, leaves at the first
        // back-edge. 2: callback sentinel at the head. 3: a ticker advances the
        // epoch while the loop runs, so it leaves at an arbitrary iteration.
        auto fn = j->lookup_or_translate(*e, head);
        const uint64_t total = kind == 2 ? 256 : 5000;
        unsigned mid_loop_exits = 0;
        for (unsigned boundary = 0; boundary < 4 + 200; ++boundary) {
            CPU cpu; initialize(cpu);
            jit_grant_lease(cpu);
            const unsigned mode = boundary < 3 ? boundary : 3;
            if (mode == 1) cpu.jit_epoch_seen = g_jit_preempt_epoch.load() - 1;
            if (mode == 2) cpu.jit_stop_pc = head;
            std::atomic<bool> done{false};
            std::thread ticker;
            if (mode == 3) ticker = std::thread([&] {
                while (!done.load(std::memory_order_relaxed)) {
                    g_jit_preempt_epoch.fetch_add(1, std::memory_order_release);
                    std::this_thread::yield();
                }
            });
            cpu.pc = fn(&cpu, e.get());
            done.store(true, std::memory_order_relaxed);
            if (ticker.joinable()) ticker.join();
            const uint64_t n = cpu.regs[2];
            const bool finished = n == total;
            if (!finished) ++mid_loop_exits;
            // Kind 4's loop head reads the carried C and must see it committed. Kind 0's head
            // (ADD, ADD, SUBS) overwrites all four flags before anything can read them, so its
            // back-edge exits skip the store and leave the entry flags in place.
            const uint32_t nzcv = finished ? (kind == 0 || kind == 4 ? 0x60000000 : 0xa0000000)
                                           : (kind == 4 ? 0x20000000 : 0xa0000000);
            // A finished loop leaves through its exit edge, a forward edge that carries no poll:
            // the chain ends at the return block or runs on through it to the host stop.
            const bool pc_ok = finished ? cpu.pc == stop || cpu.pc == head + 16 : cpu.pc == head;
            double lo, hi;
            std::memcpy(&lo, &cpu.v_lo[0], 8); std::memcpy(&hi, &cpu.v_hi[0], 8);
            if (!n || n > total || (mode == 0 && !finished) || ((mode == 1 || mode == 2) && n != 1) ||
                !pc_ok ||
                cpu.regs[1] != n * (n - 1) / 2 + 7 + (kind == 4 ? n : 0) ||
                cpu.regs[0] != (kind == 2 ? n : 5000 - n) ||
                (cpu.pstate & 0xf0000000) != nzcv ||
                (kind == 3 && (lo != double(n) || hi != 3.0 * n))) {
                std::fprintf(stderr, "selfloop kind %u mode %u: n=%llu pc=0x%llx flags=0x%x\n", kind, mode,
                             (unsigned long long)n, (unsigned long long)cpu.pc, cpu.pstate >> 28);
                ++failures;
            }
        }
        // The loops need no lease to terminate and the ticker must be able to interrupt them.
        if (!mid_loop_exits) { std::fprintf(stderr, "selfloop kind %u: no epoch exit observed\n", kind); ++failures; }
        // A thread already inside the loop holds a lease from before the invalidation. Retiring
        // the code advances the epoch, and retranslation must not revive the retired body.
        CPU retired; initialize(retired); jit_grant_lease(retired);
        j->invalidate_range(head, 4);
        j->lookup_or_translate(*e, head);
        retired.pc = fn(&retired, e.get());
        if (retired.pc != head || retired.regs[2] != 1) {
            std::fprintf(stderr, "selfloop kind %u retired: pc=0x%llx n=%llu\n", kind, (unsigned long long)retired.pc, (unsigned long long)retired.regs[2]);
            ++failures;
        }
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
// A static edge to a higher block start cannot recur, so it carries no poll: with a stale epoch, a
// chain of forward branches still runs end to end.
static bool check_forward_chains() {
    auto e = std::make_unique<Emulator>();
    constexpr uint64_t base = 0x50000, stop = 0x60000;
    constexpr unsigned blocks = 300;
    e->mem().map_range(base, 8192);
    e->mem().mprotect_guest(base, 8192, 7);
    std::vector<uint32_t> code;
    for (unsigned i = 0; i < blocks; ++i) { code.push_back(0x91000400); code.push_back(0x14000001); } // add x0,x0,#1; b .+4
    code.push_back(0xd65f03c0);                                                                       // ret
    e->mem().write(base, code.data(), code.size() * 4);
    e->enable_jit();
    auto* j = e->jit();
    j->enter_multithreaded();
    for (unsigned pass = 0; pass < blocks + 4; ++pass) {  // translate and link one more block per pass
        CPU cpu; cpu.pc = base; cpu.regs[30] = stop; cpu.jit_stop_pc = stop;
        for (unsigned steps = 0; cpu.pc != stop && steps < 10000; ++steps) cpu.pc = j->run_block(cpu, *e);
    }
    auto fn = j->lookup_or_translate(*e, base);
    CPU cpu; cpu.pc = base; cpu.regs[30] = stop; cpu.jit_stop_pc = stop;
    cpu.jit_epoch_seen = g_jit_preempt_epoch.load() - 1;
    cpu.pc = fn(&cpu, e.get());
    if (cpu.regs[0] != blocks) {
        std::fprintf(stderr, "forward chain: %llu of %u blocks ran with a stale epoch\n",
                     (unsigned long long)cpu.regs[0], blocks);
        return false;
    }
    std::puts("jit_mt_publication: forward edges chain without a poll PASS");
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
    bounded.pc = entry; bounded.regs[0] = 1000000000000ull;
    bounded.regs[30] = stop; bounded.jit_stop_pc = stop;
    const auto started = std::chrono::steady_clock::now();
    bounded.pc = j->run_block(bounded, *e);
    // Nothing decrements a lease: the ticker's epoch is what returns this loop to the dispatcher.
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    if (!bounded.regs[2] || bounded.pc == stop || ms > 500) {
        std::fprintf(stderr, "bounded run: %llu iterations, %lld ms\n", (unsigned long long)bounded.regs[2], (long long)ms);
        ++failures;
    }
    j->invalidate_all();
    if (std::memcmp(before.data(), j->code_buf(), prefix)) ++failures;
    if (failures) { std::fprintf(stderr, "MT publication: %u failures\n", failures.load()); return 1; }
    if (!check_signal_return(false) || !check_signal_return(true) || !check_selfloops() ||
        !check_entry_order_loops(false) || !check_entry_order_loops(true) || !check_forward_chains()) {
        std::fputs("jit_mt_publication: FAILED after the checks that passed above\n", stderr);
        return 1;
    }
    std::puts("jit_mt_publication: immutable bytes and epoch exits PASS");
}
