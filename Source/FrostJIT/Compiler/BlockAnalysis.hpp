#pragma once
#include <cstdint>
namespace arm64emu {
class Emulator;
struct DecodedInst;
namespace jit_analysis {
struct LeafScanResult {
    int size = 0;            // non-RET instructions before the RET (0 = no)
    bool has_branch = false;
    uint64_t branch_pc = 0;  // pc of the forward branch
    uint64_t branch_target = 0;
};
bool instr_will_call_interp(const DecodedInst& instruction);
// Longest instruction run nzcv_dead_in() examines.
constexpr int kNzcvScanWords = 6;
// True when NZCV is dead on entry to the instruction stream `words`: only
// flag-neutral ALU instructions that cannot fault, branch or read flags run
// before one that rewrites all four flags (ADDS/SUBS/ANDS/FCMP). `used` is
// the number of words the verdict depends on. A false result only means the
// flags are not provably dead.
bool nzcv_dead_in(const uint32_t* words, int count, int& used);
LeafScanResult leaf_scan(Emulator& emu, uint64_t target);
} // namespace jit_analysis
} // namespace arm64emu
