// Structural and execution oracles for SSA joins, backedges, and destruction.
#include "ir/ssa.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace arm64emu;
static unsigned checks;
static void check(bool ok, const char* message) {
    ++checks;
    if (!ok) { fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
using State = std::array<uint64_t, 64>;
// Independent executor of the input/output non-SSA IR, not SSA's optimizer.
// Branch choices are supplied separately, using guest register x0.
static State execute(const std::vector<SSASourceBlock>& graph, State state,
                     size_t loop_header = SIZE_MAX, uint64_t limit = 0) {
    std::array<uint64_t, 4096> values{};
    size_t b = 0;
    for (unsigned steps = 0; steps < 10000; ++steps) {
        for (const auto& i : graph[b].ir.insts) {
            switch (i.op) {
                case IROp::IMM: values[i.dest] = i.imm_value(); break;
                case IROp::LOAD_REG: values[i.dest] = state[i.src1 + (i.is_fp_load() ? 32 : 0)]; break;
                case IROp::STORE_REG: state[i.dest + (i.is_fp_store() ? 32 : 0)] = values[i.src1]; break;
                case IROp::MOV: values[i.dest] = values[i.src1]; break;
                case IROp::ADD: values[i.dest] = values[i.src1] + values[i.src2]; break;
                case IROp::SUB: values[i.dest] = values[i.src1] - values[i.src2]; break;
                case IROp::XOR: values[i.dest] = values[i.src1] ^ values[i.src2]; break;
                case IROp::ZEXT: values[i.dest] = values[i.src1] & ((1ULL << i.zext_bits()) - 1); break;
                default: check(false, "unexpected oracle instruction");
            }
        }
        const auto& edges = graph[b].successors;
        if (edges.empty()) return state;
        check(edges.size() <= 2, "oracle CFG degree");
        size_t edge = edges.size() == 1 ? 0 : (b == loop_header ? state[0] >= limit : (state[0] & 1));
        b = edges[edge];
    }
    check(false, "oracle terminates");
    return state;
}
static SSAFunction build(const std::vector<SSASourceBlock>& graph) {
    SSAFunction f;
    std::string error;
    check(build_ssa(graph, f, &error), error.c_str());
    check(validate_ssa(f, &error), error.c_str());
    return f;
}
static size_t phis(const SSAFunction& f) {
    size_t n = 0;
    for (const auto& b : f.blocks) n += b.phis.size();
    return n;
}
static std::vector<SSASourceBlock> lower(SSAFunction& f) {
    std::string error;
    optimize_ssa(f);
    check(validate_ssa(f, &error), error.c_str());
    std::vector<SSASourceBlock> out;
    check(lower_ssa(f, out, &error), error.c_str());
    return out;
}
int main() {
    // Diamond, including reassignment of a scalar temporary on both arms.
    // The incoming x0 differs from x1, exposing wrong phi predecessor choices.
    std::vector<SSASourceBlock> diamond(4);
    diamond[0].ir.insts = {IRInst::make_load_reg(33, 1, false)};
    diamond[0].successors = {1, 2};
    diamond[1].ir.insts = {IRInst::make_imm(34, 11), IRInst::make(IROp::ADD, 35, 33, 34), IRInst::make_store_reg(2, 35, false)};
    diamond[1].successors = {3};
    diamond[2].ir.insts = {IRInst::make_imm(34, 77), IRInst::make(IROp::XOR, 35, 33, 34), IRInst::make_store_reg(2, 35, false)};
    diamond[2].successors = {3};
    diamond[3].ir.insts = {IRInst::make_load_reg(36, 2, false), IRInst::make(IROp::ADD, 37, 36, 35), IRInst::make_store_reg(3, 37, false)};
    auto f = build(diamond);
    check(phis(f) >= 2, "diamond creates real phis");
    auto lowered = lower(f);
    check(phis(f) > 0, "different incoming values retain phi");
    for (uint64_t n = 0; n < 256; ++n) {
        State s{}; s[0] = n; s[1] = n * 0x123456789ULL;
        check(execute(diamond, s) == execute(lowered, s), "diamond lowering execution");
    }
    // Deliberately corrupt a phi edge and a normal use: verifier rejects both.
    auto bad = f;
    for (auto& b : bad.blocks) if (!b.phis.empty()) { b.phis[0].incoming.pop_back(); break; }
    check(!validate_ssa(bad), "reject missing phi predecessor");
    bad = f;
    bad.inputs.push_back(bad.inputs.front());
    check(!validate_ssa(bad), "reject duplicate definitions");
    bad = f;
    SSAValue invalid_result = static_cast<SSAValue>(bad.variables.size());
    bad.variables.push_back(300);
    bad.blocks[0].instructions.push_back({IRInst::make(IROp::MOV, 300, 0), invalid_result,
        {bad.blocks[3].instructions.back().inputs[0], 0, 0}, 1, false});
    check(!validate_ssa(bad), "reject non-dominating definition");

    // Loop carries two values and swaps them at the backedge; phi destruction
    // must perform parallel copies. The exit is a critical header edge too.
    std::vector<SSASourceBlock> loop(4);
    loop[0].ir.insts = {IRInst::make_imm(33, 0), IRInst::make_store_reg(0, 33, false), IRInst::make_imm(34, 1)};
    loop[0].successors = {1};
    loop[1].ir.insts = {IRInst::make_load_reg(35, 0, false), IRInst::make_load_reg(36, 1, false), IRInst::make_load_reg(37, 2, false)};
    loop[1].successors = {2, 3};
    loop[2].ir.insts = {IRInst::make(IROp::ADD, 38, 35, 34), IRInst::make_store_reg(0, 38, false), IRInst::make_store_reg(1, 37, false), IRInst::make_store_reg(2, 36, false)};
    loop[2].successors = {1};
    loop[3].ir.insts = {IRInst::make_load_reg(39, 1, false), IRInst::make_load_reg(40, 2, false), IRInst::make(IROp::SUB, 41, 39, 40), IRInst::make_store_reg(3, 41, false)};
    f = build(loop);
    lowered = lower(f);
    check(phis(f) >= 3, "loop retains induction and swap phis");
    check(lowered.size() > loop.size(), "phi edges split");
    for (uint64_t count = 0; count < 65; ++count) {
        State s{}; s[1] = 0x1020304050607080; s[2] = 0xfedcba9876543210;
        check(execute(loop, s, 1, count) == execute(lowered, s, 1, count), "loop parallel-copy execution");
    }
    // Region adapter lowers architectural loop phis through retained guest
    // state commits. It must preserve both zero-iteration and carried cases.
    std::string state_error;
    std::vector<SSASourceBlock> state_lowered;
    check(lower_ssa(f, state_lowered, &state_error, SSAPhiLowering::ArchitecturalState), state_error.c_str());
    check(state_lowered.size() == loop.size(), "state phi lowering needs no synthetic edges");
    for (uint64_t count = 0; count < 65; ++count) {
        State s{}; s[1] = 0xaabbccdd; s[2] = 0x12345678;
        check(execute(loop, s, 1, count) == execute(state_lowered, s, 1, count), "state phi loop execution");
    }
    // Critical edge: entry has two successors and join two predecessors.
    auto critical = diamond;
    critical[0].ir.insts.push_back(IRInst::make_imm(35, 99));
    critical[0].ir.insts.push_back(IRInst::make_store_reg(2, 35, false));
    critical[0].successors = {1, 3};
    critical.erase(critical.begin() + 2);
    critical[0].successors = {1, 2}; critical[1].successors = {2};
    f = build(critical); lowered = lower(f);
    check(lowered[0].successors[1] >= critical.size(), "critical edge is split");
    for (uint64_t n = 0; n < 64; ++n) {
        State s{}; s[0] = n; s[1] = n + 0xabcdef;
        check(execute(critical, s) == execute(lowered, s), "critical edge execution");
    }
    // Same numerical slot in two architectural namespaces, overwritten temp,
    // repeated expressions, width-sensitive keys, and a live store checkpoint.
    IRBlock straight;
    straight.insts = {IRInst::make_load_reg(33, 0, false), IRInst::make_load_reg(34, 0, true),
        IRInst::make(IROp::ADD, 35, 33, 34), IRInst::make_store_reg(1, 35, false),
        IRInst::make(IROp::ADD, 36, 33, 34), IRInst::make_store_reg(1, 36, true),
        IRInst::make_imm(33, 0xff), IRInst::make(IROp::ADD, 37, 33, 34),
        IRInst::make_store_reg(2, 37, false), IRInst::make_zext(38, 34, 8, 0),
        IRInst::make_zext(39, 34, 32, 0), IRInst::make_store_reg(3, 38, false),
        IRInst::make_store_reg(4, 39, false)};
    IRBlock before = straight;
    check(optimize_ir_ssa(straight, true), "JIT scalar SSA adapter");
    check(straight.insts.size() < before.insts.size(), "CSE removes duplicate expression");
    for (uint64_t n = 0; n < 256; ++n) {
        State s{}; s[0] = n + 0x2222; s[32] = n * 0x123456789ULL;
        check(execute({{before, {}}}, s) == execute({{straight, {}}}, s), "SSA scalar adapter execution");
    }
    // Constants stay single-use for the backend's immediate folding; inputs
    // load at their first use, but never after a store to their own slot.
    IRBlock sunk;
    sunk.insts = {IRInst::make_load_reg(33, 5, false), IRInst::make_imm(34, 7),
        IRInst::make_store_reg(5, 34, false), IRInst::make_load_reg(35, 6, false),
        IRInst::make_imm(36, 7), IRInst::make(IROp::ADD, 37, 35, 36),
        IRInst::make_store_reg(7, 37, false), IRInst::make(IROp::ADD, 38, 33, 34),
        IRInst::make_store_reg(8, 38, false)};
    IRBlock sunk_before = sunk;
    check(optimize_ir_ssa(sunk, true), "input sinking adapter");
    int imms = 0, load5 = -1, load6 = -1, store5 = -1, first_imm = -1;
    for (size_t k = 0; k < sunk.insts.size(); ++k) {
        const auto& i = sunk.insts[k];
        if (i.op == IROp::IMM) { ++imms; if (first_imm < 0) first_imm = static_cast<int>(k); }
        if (i.op == IROp::LOAD_REG && i.src1 == 5) load5 = static_cast<int>(k);
        if (i.op == IROp::LOAD_REG && i.src1 == 6) load6 = static_cast<int>(k);
        if (i.op == IROp::STORE_REG && i.dest == 5) store5 = static_cast<int>(k);
    }
    // IMM 7 reaches the second ADD both directly and through x5's store.
    check(imms == 3, "constants are not value-numbered; shared src2 is rematerialized");
    bool folded = false;
    for (size_t k = 1; k < sunk.insts.size(); ++k) {
        const auto& i = sunk.insts[k];
        const auto& p = sunk.insts[k - 1];
        folded |= i.op == IROp::ADD && p.op == IROp::IMM && p.dest == i.src2 && p.imm_value() == 7;
    }
    check(folded, "shared constant copy precedes its folding consumer");
    bool adjacent = false;
    for (size_t k = 1; load6 >= 0 && k < sunk.insts.size(); ++k)
        adjacent |= sunk.insts[k].op == IROp::ADD && sunk.insts[k].src1 == sunk.insts[load6].dest &&
                    sunk.insts[k - 1].op == IROp::IMM && sunk.insts[k - 1].dest == sunk.insts[k].src2;
    check(adjacent, "sunk input load keeps IMM -> consumer adjacency");
    check(load5 >= 0 && store5 >= 0 && load5 < store5, "input load stays before its slot's store");
    check(load6 > first_imm, "input load sinks to its first use");
    for (uint64_t n = 0; n < 256; ++n) {
        State s{}; s[5] = n * 3 + 1; s[6] = n << 20;
        check(execute({{sunk_before, {}}}, s) == execute({{sunk, {}}}, s), "sunk input execution");
    }
    auto invalid = before;
    invalid.insts[0] = IRInst::make(IROp::MOV, 33, 200);
    SSAFunction sentinel;
    check(!build_ssa({{invalid, {}}}, sentinel), "reject undefined temporary");
    invalid = before; invalid.insts.push_back(IRInst::make(IROp::CALL_INTERP));
    check(!optimize_ir_ssa(invalid), "helper effects remain conservative");
    check(invalid.insts.size() == before.insts.size() + 1, "fallback is transactional");
    printf("IR_SSA_OK: %u checks\n", checks);
}
