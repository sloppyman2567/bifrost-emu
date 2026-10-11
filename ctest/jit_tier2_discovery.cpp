// Region discovery must follow direct jumps and instruction-cap boundaries,
// preserve loop-carried state at early exits, and invalidate non-head pages.
#include "core/emulator.h"
#include "jit/frostjit.hpp"
#include "tier2_test_access.hpp"
#include <cstdio>
#include <memory>
#include <vector>
#include <string>
#include <chrono>
using namespace arm64emu;
namespace arm64emu { extern "C" uint64_t jit_call_helper(CPU*, Emulator*, uint64_t); }
int main(int argc, char** argv) {
    bool mt = false, benchmark = false;
    for (int k = 1; k < argc; ++k) {
        mt |= std::string(argv[k]) == "mt";
        benchmark |= std::string(argv[k]) == "benchmark";
    }
    constexpr uint64_t head = 0x10000, body = 0x11000, exit = 0x12000, stop = 0x20000;
    auto e = std::make_unique<Emulator>();
    e->mem().map_range(head, 0x3000);
    e->mem().mprotect_guest(head, 0x3000, 7);
    const uint32_t entry[] = {0xb4010000, 0x140003ff}; // cbz x0,exit; b body
    std::vector<uint32_t> code(40, 0x91000421); // add x1,x1,#1
    code.push_back(0xd1000400); // sub x0,x0,#1
    const uint64_t branch_pc = body + code.size() * 4;
    code.push_back(0x14000000 | (static_cast<uint32_t>((head - branch_pc) / 4) & 0x3ffffff));
    const uint32_t tail[] = {0xaa0103e0, 0xd65f03c0}; // mov x0,x1; ret
    e->mem().write(head, entry, sizeof(entry));
    e->mem().write(body, code.data(), code.size() * 4);
    e->mem().write(exit, tail, sizeof(tail));
    e->enable_jit();
    auto* j = e->jit();
    if (!j) return 2;
    if (mt) j->enter_multithreaded();
    // Collection is quiescent: no CPU is executing or mutating the table.
    const auto trace = j->collect_tier2_trace(*e, head);
    if (!benchmark && (!trace.ok || trace.blocks.size() != 4 || trace.total_insts != 44 ||
        trace.blocks[2].pc != body || trace.blocks[3].pc != body + 128)) {
        std::fprintf(stderr, "bad trace: ok=%d blocks=%zu instructions=%llu\n",
            trace.ok, trace.blocks.size(), (unsigned long long)trace.total_insts);
        return 1;
    }
    auto run = [&](uint64_t n, uint64_t per_iteration) {
        CPU cpu;
        cpu.pc = head; cpu.regs[0] = n; cpu.regs[1] = 7;
        cpu.regs[30] = stop; cpu.jit_stop_pc = stop;
        unsigned dispatches = 0;
        while (cpu.pc != stop && ++dispatches < 100000)
            cpu.pc = j->run_block(cpu, *e);
        if (cpu.pc != stop || cpu.regs[0] != n * per_iteration + 7) {
            std::fprintf(stderr, "bad result: n=%llu got=%llu pc=%llx\n",
                (unsigned long long)n, (unsigned long long)cpu.regs[0],
                (unsigned long long)cpu.pc);
            return false;
        }
        return true;
    };
    if (!run(2000, 40) || !run(0, 40) || !run(1, 40)) return 1;
    if (benchmark) {
        const auto begin = std::chrono::steady_clock::now();
        if (!run(2000000, 40)) return 1;
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - begin).count();
        std::printf("two million iterations: %.3f ms; regions=%llu\n", ms,
            (unsigned long long)j->tier2_regions.load());
        return 0;
    }
    if (j->tier2_regions.load() != 1) {
        std::fputs("expected one region, without rotated duplicates\n", stderr); return 1;
    }
    j->invalidate_range(body, 4);
    if (j->lookup_only(head)) {
        std::fputs("non-head invalidation retained region\n", stderr); return 1;
    }
    const uint32_t add_two = 0x91000821;
    e->mem().write(body, &add_two, 4);
    if (!run(2000, 41) || !run(0, 41)) return 1;
    j->invalidate_all();
    if (!run(2000, 41)) return 1;
    // A cycle to a non-head block must stop collection, never hang or
    // silently turn that cycle into a head back-edge.
    const uint32_t self_jump = 0x14000000;
    e->mem().write(body, &self_jump, 4);
    const auto cycle = j->collect_tier2_trace(*e, head);
    if (!cycle.stop_reason || std::string(cycle.stop_reason) != "revisit" ||
        j->compile_tier2_region(*e, cycle)) {
        std::fputs("non-head cycle was not rejected\n", stderr); return 1;
    }
    // Train the taken path independently for each conditional family.
    // The cold path must remain callable after the hot path was linearized.
    for (uint32_t branch : {0xb5000102u, 0x37000102u, 0x54000101u}) {
        j->invalidate_all();
        std::vector<uint32_t> adaptive(13, 0xd503201f); // nop
        adaptive[0] = 0xf100005f; // cmp x2,#0
        adaptive[1] = branch;     // cbnz/tbnz/b.ne x2, head+36
        adaptive[2] = 0x91019021; // cold: add x1,x1,#100
        adaptive[3] = 0x14000007; // b head+40
        adaptive[9] = 0x91000421; // hot: add x1,x1,#1
        adaptive[10] = 0xd1000400; // sub x0,x0,#1
        adaptive[11] = 0xb5fffea0; // cbnz x0,head
        adaptive[12] = 0xd65f03c0; // ret
        e->mem().write(head, adaptive.data(), adaptive.size()*4);
        auto adaptive_run = [&](uint64_t bit, uint64_t iterations) {
            CPU c; c.pc=head; c.regs[0]=iterations; c.regs[2]=bit;
            c.regs[30]=stop; c.jit_stop_pc=stop;
            unsigned dispatches=0;
            while (c.pc != stop && ++dispatches < 10000) c.pc=j->run_block(c,*e);
            return c.pc==stop && c.regs[1]==iterations*(bit ? 1 : 100);
        };
        if (!adaptive_run(1,20)) {
            std::fputs("adaptive branch result mismatch\n",stderr); return 1;
        }
        auto hot = j->collect_tier2_trace(*e,head);
        if (!hot.ok || !hot.fully_profiled || !hot.blocks[0].follows_taken ||
            hot.blocks[1].pc != head+36 || hot.blocks[0].side_exits[0].first != head+8) {
            std::fprintf(stderr,"taken successor was not selected: ok=%d profile=%d taken=%d stop=%s blocks=%zu\n",hot.ok,hot.fully_profiled,hot.blocks[0].follows_taken,hot.stop_reason,hot.blocks.size()); return 1;
        }
        const auto regions_before = j->tier2_regions.load();
        if (!adaptive_run(1,2000) || !adaptive_run(0,7) ||
            j->tier2_regions.load() <= regions_before) {
            std::fputs("adaptive promoted exit mismatch\n",stderr); return 1;
        }
        Tier2TestAccess::keep_candidates(*j);
        const auto retired_before = j->tier2_phase_retirements.load();
        if (!adaptive_run(0,4000)) {
            std::fputs("phase change result mismatch\n",stderr); return 1;
        }
        auto changed = j->collect_tier2_trace(*e,head);
        if (j->tier2_phase_retirements.load() <= retired_before ||
            !changed.fully_profiled || changed.blocks[0].follows_taken) {
            std::fprintf(stderr,"phase did not retrain: retired=%llu before=%llu profile=%d taken=%d\n",
                (unsigned long long)j->tier2_phase_retirements.load(),
                (unsigned long long)retired_before,changed.fully_profiled,changed.blocks[0].follows_taken);
            return 1;
        }
        // Repeated phase flips must keep exact results and eventually stop
        // speculative churn instead of compiling a new region indefinitely.
        for (unsigned phase=0; phase<4; ++phase)
            if (!adaptive_run(phase & 1,4000)) {
                std::fputs("oscillating phase result mismatch\n",stderr); return 1;
            }
        const auto settled = j->tier2_regions.load();
        if (!adaptive_run(0,4000) || !adaptive_run(1,4000) ||
            j->tier2_regions.load() != settled) {
            std::fputs("phase recompilation did not settle\n",stderr); return 1;
        }
        // Profiles from the old mapping must not steer rewritten code.
        j->invalidate_range(head,adaptive.size()*4);
        auto fresh=j->collect_tier2_trace(*e,head);
        if (fresh.fully_profiled || fresh.blocks[0].follows_taken) {
            std::fputs("stale branch profile survived invalidation\n",stderr); return 1;
        }
    }
    // Evenly alternating branches give roughly 50% probability of
    // finishing a whole candidate iteration. Hotness alone must not promote.
    j->invalidate_all();
    const uint32_t weak[] = {0xb5000082, 0x91000421, 0x14000003, 0xd503201f,
        0x91000821, 0xca030042, 0xd1000400, 0xb5ffff20, 0xd65f03c0};
    e->mem().write(head,weak,sizeof(weak));
    CPU weak_cpu; weak_cpu.pc=head; weak_cpu.regs[0]=2000; weak_cpu.regs[3]=1;
    weak_cpu.regs[30]=stop; weak_cpu.jit_stop_pc=stop;
    unsigned weak_dispatches=0;
    const auto rejections_before=j->tier2_profit_rejections.load();
    while (weak_cpu.pc!=stop && ++weak_dispatches<10000)
        weak_cpu.pc=j->run_block(weak_cpu,*e);
    if (weak_cpu.pc!=stop || weak_cpu.regs[1]!=3000 ||
        j->tier2_profit_rejections.load()<=rejections_before) {
        std::fprintf(stderr,"weak path profitability check failed: pc=%llx sum=%llu rejects=%llu before=%llu\n",(unsigned long long)weak_cpu.pc,(unsigned long long)weak_cpu.regs[1],(unsigned long long)j->tier2_profit_rejections.load(),(unsigned long long)rejections_before); return 1;
    }
    // More loop-carried GPRs than host pins. SSA head phis must see each
    // preceding iteration's writes, including deferred non-pinned stores.
    j->invalidate_all();
    std::vector<uint32_t> carried;
    for (unsigned a = 1; a <= 12; ++a)
        carried.push_back(0x91000000u | (a << 10) | (a << 5) | a);
    carried.push_back(0xd1000400); // sub x0,x0,#1
    const int64_t back = -static_cast<int64_t>(carried.size());
    carried.push_back(0xb5000000u | ((static_cast<uint32_t>(back) & 0x7ffff) << 5));
    carried.push_back(0xd65f03c0);
    e->mem().write(head, carried.data(), carried.size() * 4);
    const auto carried_trace = j->collect_tier2_trace(*e, head);
    auto carried_fn = j->compile_tier2_region(*e, carried_trace);
    if (!carried_fn) { std::fputs("carried SSA region did not compile\n", stderr); return 1; }
    for (uint64_t count : {1ULL, 2ULL, 63ULL, 2000ULL}) {
        CPU c; c.pc = head; c.regs[0] = count; c.regs[30] = stop; c.jit_stop_pc = stop;
        for (unsigned a = 1; a <= 12; ++a) c.regs[a] = a * 0x1000;
        unsigned entries = 0;
        const uint64_t after_loop = head + (carried.size() - 1) * 4;
        while (c.pc == head && ++entries <= count + 1) c.pc = carried_fn(&c, e.get());
        if (c.pc != after_loop || c.regs[0]) {
            std::fputs("carried SSA loop exit mismatch\n", stderr); return 1;
        }
        for (unsigned a = 1; a <= 12; ++a)
            if (c.regs[a] != a * (0x1000 + count)) {
                std::fprintf(stderr, "carried SSA x%u count=%llu got=%llu\n", a,
                    (unsigned long long)count, (unsigned long long)c.regs[a]); return 1;
            }
    }
    // A fault in nested call dispatch must become a guest signal before
    // it can unwind through a generated host frame.
    auto fault_emu=std::make_unique<Emulator>();
    CPU fault; fault.running=true; fault.regs[30]=stop;
    jit_call_helper(&fault,fault_emu.get(),0xdead0000);
    if (fault.running || fault.exit_code!=139) {
        std::fputs("call fallback did not deliver guest SIGSEGV\n",stderr); return 1;
    }
    std::puts("jit_tier2_discovery: direct edges, capped bodies, exits and invalidation PASS");
}
