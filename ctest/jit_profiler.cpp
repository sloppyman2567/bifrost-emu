// Deterministic checks of the sampler's actual per-thread range predicate.
// No statistical sample thresholds or wall-clock timing assertions.
#include "jit/frostjit.hpp"
#include <atomic>
#include <cstdio>
#include <memory>
#include <thread>
#include <sys/time.h>

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
    struct itimerval stop{};
    setitimer(ITIMER_PROF, &stop, nullptr);
    if (failures) {
        fprintf(stderr, "jit_profiler: %u failures\n", failures.load());
        return 1;
    }
    puts("jit_profiler: ALL PASS");
    return 0;
}
