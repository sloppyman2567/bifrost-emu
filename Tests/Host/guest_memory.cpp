// Guest memory policy (System/Core/GuestMemoryView.cpp): the cap, the ordering invariants, odd hosts.
#include "System/Core/GuestMemoryView.hpp"
#include <cstdio>
using namespace arm64emu;
static int failed;
#define CHECK(cond, what) do { if (!(cond)) { std::printf("FAIL: %s\n", what); failed = 1; } } while (0)
static constexpr uint64_t G = 1ull << 30;
static void invariants(const GuestMemoryInfo& g, const char* tag) {
    CHECK(g.total > 0, tag);
    CHECK(g.available <= g.total && g.free <= g.available, tag);
    CHECK(g.buffers + g.cached + g.free <= g.total, tag);
    CHECK(g.shmem <= g.total, tag);
}
int main() {
    HostMemoryInfo big{32 * G, 20 * G, 28 * G, 1 * G, 6 * G, 2 * G};
    GuestMemoryInfo g = apply_guest_memory_policy(big, 8 * G);
    CHECK(g.total == 8 * G, "total capped to 8 GiB");
    CHECK(g.available == 8 * G && g.free == 8 * G, "plenty free on the host: the whole capped total is available");
    invariants(g, "big host");
    g = apply_guest_memory_policy(big, 0);
    CHECK(g.total == 32 * G, "cap 0 means the host total");
    HostMemoryInfo small{6 * G, 1 * G, 2 * G, 100 << 20, 500 << 20, 50 << 20};
    g = apply_guest_memory_policy(small, 8 * G);
    CHECK(g.total == 6 * G, "a host below the cap keeps its own total");
    CHECK(g.available == 2 * G && g.free == 1 * G, "pressure on the host shows in available and free");
    invariants(g, "small host");
    HostMemoryInfo pressed{14 * G, 100 << 20, 300 << 20, 0, 0, 0};
    g = apply_guest_memory_policy(pressed, 8 * G);
    CHECK(g.total == 8 * G && g.available == (300ull << 20), "low host availability is reported even under the cap");
    invariants(g, "pressed host");
    HostMemoryInfo odd{1 * G, 4 * G, 5 * G, 3 * G, 3 * G, 3 * G};  // inconsistent figures
    g = apply_guest_memory_policy(odd, 8 * G);
    invariants(g, "inconsistent host");
    g = apply_guest_memory_policy(HostMemoryInfo{}, 8 * G);
    CHECK(g.total == 8 * G, "a host that reports nothing still gives a usable total");
    invariants(g, "empty host");
    GuestMemoryInfo live = guest_memory_info();
    invariants(live, "live host");
    if (failed) return 1;
    std::puts("guest_memory: ALL PASS");
    return 0;
}
