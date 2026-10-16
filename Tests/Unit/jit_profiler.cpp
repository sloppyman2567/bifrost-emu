// Deterministic checks of the sampler's actual per-thread range predicate.
// No statistical sample thresholds or wall-clock timing assertions.
#include "FrostJIT/Compiler/FrostJIT.hpp"
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
