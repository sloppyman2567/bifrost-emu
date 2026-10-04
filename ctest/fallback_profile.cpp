// Concurrent full-inventory snapshots retain every entry, including more
// than twelve opcodes, different PCs, and threads that have already exited.
#include "jit/fallback_profile.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    setenv("BIFROST_FALLBACK_PROFILE", argv[1], 1);
    std::atomic<bool> done{false};
    std::thread reporter([&] {
        while (!done.load()) {
            arm64emu::dump_interpreted_profile();
            std::this_thread::yield();
        }
    });
    std::vector<std::thread> workers;
    for (int tid = 1; tid <= 4; ++tid) workers.emplace_back([tid] {
        for (unsigned n = 0; n < 1000; ++n)
            for (unsigned op = 0; op < 24; ++op)
                for (unsigned pc = 0; pc < 2; ++pc)
                    arm64emu::record_interpreted_instruction(tid, 0x1000 + pc * 4,
                                                             0x10000000 + op, 7);
    });
    for (auto& thread : workers) thread.join();
    done.store(true); reporter.join();
    arm64emu::dump_interpreted_profile();
    std::ifstream file(argv[1]); std::string line; std::getline(file, line);
    unsigned rows = 0; uint64_t total = 0;
    while (std::getline(file, line)) {
        std::istringstream in(line); unsigned tid, cls; long host;
        std::string pc, opcode; uint64_t count;
        if (!(in >> tid >> host >> pc >> opcode >> cls >> count) ||
            tid < 1 || tid > 4 || host <= 0 || cls != 7 || count != 1000) return 1;
        ++rows; total += count;
    }
    if (rows != 192 || total != 192000) return 1;
    std::puts("fallback_profile: ALL PASS (192 complete entries, 192000 counts)");
}
