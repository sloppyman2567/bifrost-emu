// A 512 GiB realm window must still place a large unhinted mapping when a hinted mapping near its top has pushed the
// high allocation cursor past almost all of it: the allocator takes the first hole in the mapping index instead of
// failing. Chromium's PartitionAlloc asks for 16 GiB this way (Zenless Zone Zero's WebView aborted when it got ENOMEM).
#include "System/Memory/Memory.hpp"
#include <cstdio>
#include <cstdlib>

using namespace arm64emu;

static void check(bool ok, const char* what) {
    if (ok) return;
    std::fprintf(stderr, "memory_high_gap: FAIL %s\n", what);
    std::exit(1);
}

int main() {
    constexpr uint64_t GiB = 1ULL << 30, MiB = 1ULL << 20;
    Memory memory(false, Memory::WINDOW_SIZE_MIN);
    check(memory.window_size() == Memory::WINDOW_SIZE_MIN, "the realm window is 512 GiB");

    // A mapping near the top of the window moves the high cursor to its end, leaving about 4 GiB above it.
    const uint64_t top = memory.window_size() - 4 * GiB - 64 * MiB;
    check(memory.mmap_fixed_noreplace(top, 64 * MiB, true, 0) == top, "place the mapping near the window top");

    // 16 GiB cannot fit above the cursor. Linux would find the empty space below it.
    const uint64_t big = memory.mmap_alloc(16 * GiB, 0, true, 0);
    check(big != 0, "a 16 GiB reservation is placed");
    check(big >= Memory::LOW_REGION_SIZE && big + 16 * GiB <= memory.window_size(), "it lies inside the window");
    check(big + 16 * GiB <= top || big >= top + 64 * MiB, "it does not overlap the hinted mapping");

    // A second one is disjoint from the first, and a read-write one comes back zeroed.
    const uint64_t second = memory.mmap_alloc(16 * GiB, 0, true, 0);
    check(second != 0 && (second + 16 * GiB <= big || second >= big + 16 * GiB), "a second reservation is disjoint");
    const uint64_t data = memory.mmap_alloc(2 * GiB, 0, true, Memory::GUEST_PROT_READ | Memory::GUEST_PROT_WRITE);
    check(data != 0 && (data + 2 * GiB <= big || data >= big + 16 * GiB) &&
          (data + 2 * GiB <= second || data >= second + 16 * GiB), "a 2 GiB mapping is disjoint from both");
    check(memory.load<uint64_t>(data) == 0 && memory.load<uint64_t>(data + 2 * GiB - 8) == 0, "it reads back zero");

    // Freed space is reused, and a request larger than the window still fails cleanly.
    memory.untrack_allocation(big, 16 * GiB);
    const uint64_t again = memory.mmap_alloc(16 * GiB, 0, true, 0);
    check(again != 0, "a freed 16 GiB hole can be taken again");
    check(memory.mmap_alloc(memory.window_size(), 0, true, 0) == 0, "a request the window cannot hold fails");
    std::puts("memory_high_gap: ALL PASS");
    return 0;
}
