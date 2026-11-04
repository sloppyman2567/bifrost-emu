// Deterministic checks of the sampler's actual per-thread range predicate.
// No statistical sample thresholds or wall-clock timing assertions.
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include "FrostJIT/Runtime/SampleProfile.hpp"
#include "System/Core/Emulator.hpp"
#include <atomic>
#include <cstdio>
#include <memory>
#include <thread>
#include <sys/time.h>
#include <fstream>
#include <sstream>
#include <cstdlib>

namespace arm64emu { bool bifrost_prof_contains_ip(uint64_t); }
using namespace arm64emu;

int main() {
    auto a = std::make_unique<FrostJIT>();
    auto b = std::make_unique<FrostJIT>();
    if (!a->code_buf() || !b->code_buf()) return 2;
    const auto pa = reinterpret_cast<uint64_t>(a->code_buf());
    const auto pb = reinterpret_cast<uint64_t>(b->code_buf());
    std::atomic<unsigned> failures{0};
    auto check = [&](bool ok) { if (!ok) ++failures; };
    a->sync_dispatch_cache();
    check(bifrost_prof_contains_ip(pa));
    check(!bifrost_prof_contains_ip(pb));
    check(bifrost_prof_contains_ip(pa + a->code_buf_size() - 1));
    check(!bifrost_prof_contains_ip(pa - 1));
    check(!bifrost_prof_contains_ip(pa + a->code_buf_size()));
    // A worker must register independently even after the main installed
    // the process timer. Registration covers later logical cache growth.
    auto worker = [&] {
        check(!bifrost_prof_contains_ip(pa));
        a->sync_dispatch_cache();
        check(bifrost_prof_contains_ip(pa));
        check(bifrost_prof_contains_ip(pa + 128 * 1024 * 1024));
        b->sync_dispatch_cache();
        check(bifrost_prof_contains_ip(pb));
        check(!bifrost_prof_contains_ip(pa));
        a->sync_dispatch_cache();
        check(bifrost_prof_contains_ip(pa));
        check(!bifrost_prof_contains_ip(pb));
    };
    std::thread t1(worker), t2(worker);
    t1.join(); t2.join();
    // A worker's registration must not overwrite the main thread's range.
    check(bifrost_prof_contains_ip(pa));
    check(!bifrost_prof_contains_ip(pb));
    // The sample report puts each pc into the part of its block, across threads, and keeps what threads leave behind.
    // The process timer is stopped first so that no real sample can land among the synthetic ones.
    {
        struct itimerval off{};
        setitimer(ITIMER_PROF, &off, nullptr);
        const uint64_t base = pa + 0x1000, other = pa + 0x100;
        // entry [0x00,0x10) body [0x10,0x40) exit [0x40,0x50) cold [0x50,0x60); a lower block registered later (unsorted)
        prof_note_block({base, base + 0x10, base + 0x40, base + 0x50, base + 0x60, 0x1234});
        prof_note_block({other, other + 0x10, other + 0x20, other + 0x30, other + 0x40, 0x5678});
        a->sync_dispatch_cache();  // this thread gets a ring
        for (uint64_t rip : {base + 0x00, base + 0x0f, base + 0x10, base + 0x3f, base + 0x40, base + 0x4f, base + 0x50,
                             base + 0x5f, base + 0x60, pa + 0x5000})
            prof_record_sample(rip);
        std::thread worker([&] {
            b->sync_dispatch_cache();
            prof_record_sample(other + 0x18);  // body of the other block
            prof_record_sample(other + 0x18);
        });
        worker.join();  // its ring is resolved as the thread ends
        ProfReport report = prof_collect(8);
        check(report.lost == 0);
        // base: entry 2 (0x00,0x0f), body 2 (0x10,0x3f), exit 2 (0x40,0x4f), cold 2 (0x50,0x5f); 0x60 is the block's end
        // and 0x5000 lies in no block: both are shared. other: body 2.
        check(report.total.entry == 2 && report.total.body == 4 && report.total.exit == 2 && report.total.cold == 2 &&
              report.total.shared == 2);
        check(report.total.total() == 12);
        check(report.hottest.size() == 2 && report.hottest[0].guest_pc == 0x1234 &&
              report.hottest[0].counts.total() == 8 && report.hottest[1].guest_pc == 0x5678 &&
              report.hottest[1].counts.body == 2);
        unsigned exited = 0, live = 0;
        for (const ProfThreadReport& t : report.threads) {
            if (t.counts.total() == 2 && t.exited) ++exited;
            if (t.counts.total() == 10 && !t.exited) ++live;
        }
        check(exited == 1 && live == 1);
        // A code buffer that goes away takes its blocks with it: the same pc is then shared.
        prof_release_code(a->code_buf(), a->code_buf_size());
        prof_record_sample(base + 0x20);
        report = prof_collect(8);
        check(report.total.body == 4 && report.total.shared == 3);  // what was seen before stays in the totals
        check(report.hottest.size() == 2 && report.hottest[0].counts.total() == 8);
        // Lost samples are counted and not folded into a bucket: a ring holds fewer than this.
        for (unsigned i = 0; i < 40000; ++i) prof_record_sample(pa + 0x5000);
        report = prof_collect(0);
        check(report.lost >= 40000 - 16384 && report.total.shared == 3 + 16384);
    }
    // The optional allocation profile must report real IR provenance and
    // executable ranges, including operations constructed by shared helpers.
    if (const char* path = std::getenv("BIFROST_REGALLOC_PROFILE")) {
        constexpr uint64_t head = 0x40000, stop = head + 4096;
        auto emu = std::make_unique<Emulator>();
        emu->mem().map_range(head, 4096); emu->mem().mprotect_guest(head, 4096, 7);
        const uint32_t code[] = {0xd28000a0, 0x91000c00, 0xd65f03c0}; // mov 5; add 3; ret
        emu->mem().write(head, code, sizeof(code)); emu->enable_jit();
        auto fn = emu->jit()->lookup_or_translate(*emu, head);
        check(fn != nullptr);
        if (fn) {
            CPU cpu; cpu.pc = head; cpu.regs[30] = stop;
            check(fn(&cpu, emu.get()) == stop && cpu.regs[0] == 8);
        }
        // With BIFROST_PROF the translator registers the block and its operations: sampling every byte of it puts the body's
        // samples into operation kinds that account for all of them (mov and add are ALU, the result is a guest-state
        // move; the indirect branch of ret is in the exit section, not in the body).
        {
            const auto before = prof_collect(0);
            const auto start = reinterpret_cast<uint64_t>(reinterpret_cast<const void*>(fn));
            for (uint64_t offset = 0; offset < 1024; ++offset) prof_record_sample(start + offset);
            const auto after = prof_collect(0);
            uint64_t kinds = 0, body = after.total.body - before.total.body;
            for (int i = 0; i < PROF_OP_KINDS; ++i) kinds += after.body_op[i] - before.body_op[i];
            check(body > 0 && kinds == body);
            check(after.body_op[PROF_ALU] > before.body_op[PROF_ALU]);
            check(after.body_op[PROF_STATE] > before.body_op[PROF_STATE]);
            check(after.body_op[PROF_MEMORY] == before.body_op[PROF_MEMORY]);
        }
        std::ifstream profile(path);
        std::string line; std::getline(profile, line);
        check(line.find("block_pc\tguest_pc\thost_begin\thost_end") == 0);
        unsigned rows = 0;
        while (std::getline(profile, line)) {
            std::istringstream fields(line);
            std::string block, pc, begin, end, kind;
            if (!(fields >> block >> pc >> begin >> end >> kind)) { check(false); continue; }
            check(std::stoull(begin, nullptr, 0) < std::stoull(end, nullptr, 0));
            if (kind == "tier1-ir") {
                const auto guest = std::stoull(pc, nullptr, 0);
                check(guest >= head && guest < head + sizeof(code) && !(guest & 3));
                ++rows;
            }
        }
        check(rows > 0);
    }
    struct itimerval stop{};
    setitimer(ITIMER_PROF, &stop, nullptr);
    if (failures) {
        fprintf(stderr, "jit_profiler: %u failures\n", failures.load());
        return 1;
    }
    puts("jit_profiler: ALL PASS");
    return 0;
}
