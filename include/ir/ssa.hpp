#pragma once

#include "ir/ir.hpp"
#include <array>
#include <string>
#include <utility>

namespace arm64emu {

// Value 0 is absent. Architectural slots and scalar temporaries occupy
// different variable namespaces; SSA values themselves have no register identity.
using SSAValue = uint32_t;
using SSAVariable = uint32_t;
constexpr SSAVariable SSA_GPR = 0x10000;
constexpr SSAVariable SSA_FP = SSA_GPR + 32;

struct SSASourceBlock {
    IRBlock ir;
    std::vector<size_t> successors;
};
struct SSAInstruction {
    IRInst ir{};
    SSAValue result = 0;
    std::array<SSAValue, 3> inputs{}; // src1, src2, aux, according to input_mask
    uint8_t input_mask = 0;
    bool effect = false;
};
struct SSAPhi {
    SSAValue result = 0;
    SSAVariable variable = 0;
    std::vector<std::pair<size_t, SSAValue>> incoming;
};
struct SSABlock {
    std::vector<size_t> predecessors, successors;
    std::vector<SSAPhi> phis;
    std::vector<SSAInstruction> instructions;
};
struct SSAFunction {
    std::vector<SSABlock> blocks;
    std::vector<std::pair<SSAValue, SSAVariable>> inputs;
    // Index by value, including tombstoned definitions after optimization.
    std::vector<SSAVariable> variables{0};
};

// Sealed-block construction, including loop backedges and lazy, pruned phis.
// Unsupported op contracts and undefined temporary reads fail transactionally.
// Block 0 is the sole entry and must have no predecessors; all blocks reachable.
bool build_ssa(const std::vector<SSASourceBlock>& source, SSAFunction& result,
               std::string* error = nullptr);
bool validate_ssa(const SSAFunction& function, std::string* error = nullptr);
void optimize_ssa(SSAFunction& function);
// Destroy SSA using parallel copies in split edges (including critical edges).
// Returned successor indices, rather than guest branch addresses, describe CFG.
enum class SSAPhiLowering { EdgeCopies, ArchitecturalState };
// ArchitecturalState requires retained STORE_REG commits on every edge and
// lowers architectural phis to reads of that canonical state at the join.
// It declines temporary phis. This lets the existing region backend consume
// true loop SSA without inventing scratch homes across its backedge.
bool lower_ssa(const SSAFunction& function, std::vector<SSASourceBlock>& result,
               std::string* error = nullptr,
               SSAPhiLowering phis = SSAPhiLowering::EdgeCopies);
// Current backend adapter: scalar, straight-line blocks. Guest state stores,
// trapping accesses and flags keep their original order. Unsupported blocks
// retain the existing pipeline; whole-region SSA codegen is a separate step.
bool optimize_ir_ssa(IRBlock& block, bool check = false);

} // namespace arm64emu
