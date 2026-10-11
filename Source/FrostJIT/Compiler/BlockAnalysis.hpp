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
LeafScanResult leaf_scan(Emulator& emu, uint64_t target);
} // namespace jit_analysis
} // namespace arm64emu
