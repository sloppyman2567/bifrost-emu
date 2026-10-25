// Emulator::dump_memory_breakdown — where this process's resident memory is (BIFROST_STATS_PERIOD, --verbose).
//
// Classifies /proc/self/smaps by what the mapping is: the guest's own pages (inside the guest window), generated
// code, the host heap, glibc malloc arenas, host thread stacks, other anonymous memory and file mappings. The
// guest figure is what the game really uses; everything else is the emulator's.
#include "System/Core/Emulator.hpp"
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace arm64emu {

void Emulator::dump_memory_breakdown() {
    const uint64_t window_lo = reinterpret_cast<uint64_t>(mem_.direct_window());
    const uint64_t window_hi = window_lo + mem_.window_size();
    std::vector<std::pair<uint64_t, uint64_t>> code;
    jit_.for_each_live([&](FrostJIT* jit) {
        if (jit->code_buf())
            code.emplace_back(reinterpret_cast<uint64_t>(jit->code_buf()),
                              reinterpret_cast<uint64_t>(jit->code_buf()) + jit->code_buf_size());
    });
    FILE* file = std::fopen("/proc/self/smaps", "r");
    if (!file) return;
    uint64_t guest = 0, jit_code = 0, heap = 0, arenas = 0, stacks = 0, anon = 0, files = 0;  // kB
    uint64_t start = 0, end = 0;
    char path[512] = {};
    char line[1024];
    while (std::fgets(line, sizeof(line), file)) {
        unsigned long long lo, hi;
        int consumed = 0;
        if (std::sscanf(line, "%llx-%llx %*4s %*s %*s %*s %n", &lo, &hi, &consumed) >= 2 && consumed > 0) {
            start = lo; end = hi;
            std::snprintf(path, sizeof(path), "%s", line + consumed);
            path[std::strcspn(path, "\n")] = '\0';
            continue;
        }
        unsigned long long kb;
        if (std::sscanf(line, "Rss: %llu kB", &kb) != 1) continue;
        bool in_code = false;
        for (const auto& range : code) in_code |= start >= range.first && start < range.second;
        const uint64_t size = end - start;
        if (start >= window_lo && start < window_hi) guest += kb;
        else if (in_code) jit_code += kb;
        else if (!std::strcmp(path, "[heap]")) heap += kb;
        else if (path[0] == '/') files += kb;
        // glibc: a non-main arena is a 64 MiB-aligned heap of at most 64 MiB; a pthread stack is 8 MiB plus a guard.
        else if (path[0] == '\0' && size <= (64ull << 20) && (start & ((64ull << 20) - 1)) == 0) arenas += kb;
        else if (path[0] == '\0' && size >= (8ull << 20) - (64ull << 10) && size <= (8ull << 20) + (64ull << 10)) stacks += kb;
        else anon += kb;
    }
    std::fclose(file);
    const uint64_t total = guest + jit_code + heap + arenas + stacks + anon + files;
    unsigned threads = 0;
    if (FILE* status = std::fopen("/proc/self/status", "r")) {
        while (std::fgets(line, sizeof(line), status)) if (std::sscanf(line, "Threads: %u", &threads) == 1) break;
        std::fclose(status);
    }
    std::fprintf(stderr,
                 "[mem] rss=%llu MB: guest=%llu jit_code=%llu heap=%llu arenas=%llu thread_stacks=%llu other_anon=%llu "
                 "files=%llu (MB, by mapping; arenas and stacks are recognized by size and alignment) threads=%u\n",
                 (unsigned long long)(total >> 10), (unsigned long long)(guest >> 10), (unsigned long long)(jit_code >> 10),
                 (unsigned long long)(heap >> 10), (unsigned long long)(arenas >> 10), (unsigned long long)(stacks >> 10),
                 (unsigned long long)(anon >> 10), (unsigned long long)(files >> 10), threads);
}

} // namespace arm64emu
