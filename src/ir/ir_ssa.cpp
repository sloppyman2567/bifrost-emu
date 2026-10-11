#include "ir/ssa.hpp"
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>

namespace arm64emu {
namespace {
struct Shape { uint8_t uses; bool result, effect; };
// An explicit contract: never infer operands from nonzero fields, because FP
// indices, architectural slots and parameters also live in those fields.
bool shape(const IRInst& i, Shape& s) {
    switch (i.op) {
        case IROp::NOP: s = {0, false, false}; break;
        case IROp::IMM: s = {0, true, false}; break;
        case IROp::LOAD_REG: s = {0, true, false}; break;
        case IROp::STORE_REG: s = {1, false, true}; break;
        case IROp::MOV: case IROp::NOT: case IROp::NEG:
        case IROp::SEXT: case IROp::ZEXT: case IROp::CLZ: case IROp::REV64:
        case IROp::UBFM: case IROp::SBFM:
            s = {1, true, false}; break;
        case IROp::ADD: case IROp::SUB: case IROp::MUL:
        case IROp::AND: case IROp::OR: case IROp::XOR:
        case IROp::SHL: case IROp::SHR: case IROp::SAR: case IROp::ROR:
        case IROp::CSEL: case IROp::EXTR:
        case IROp::UDIV: case IROp::SDIV: case IROp::SMULH: case IROp::UMULH:
            // CSEL reads implicit NZCV and therefore cannot be CSE'd/DCE'd
            // as ordinary arithmetic by this pass.
            s = {3, true, i.op == IROp::CSEL}; break;
        case IROp::SMADDL: case IROp::UMADDL:
        case IROp::SMSUBL: case IROp::UMSUBL:
            s = {7, true, false}; break;
        case IROp::ADDS: case IROp::SUBS: case IROp::ADCS: case IROp::SBCS:
            s = {3, i.dest != 0, true}; break;
        case IROp::TST: case IROp::CCMP: s = {3, false, true}; break;
        case IROp::TST_ZERO: s = {1, false, true}; break;
        case IROp::LOAD_MEM: s = {1, true, true}; break;
        case IROp::STORE_MEM: s = {3, false, true}; break;
        case IROp::BR: case IROp::BRCOND_ZERO: case IROp::BRCOND_BIT:
        case IROp::MSR: s = {1, false, true}; break;
        case IROp::MRS: s = {0, true, true}; break;
        case IROp::BRCOND: case IROp::BRCOND_FALLTHRU:
        case IROp::MEMORY_BARRIER: s = {0, false, true}; break;
        // Helpers, calls, atomics, and direct FP/SIMD architectural accesses
        // require explicit state effects before they can enter this graph.
        default: return false;
    }
    return !s.result || i.dest >= 33;
}

bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}
using Dominators = std::vector<std::vector<bool>>;
bool dominators(const SSAFunction& f, Dominators& dom) {
    size_t n = f.blocks.size();
    if (!n || !f.blocks[0].predecessors.empty()) return false;
    std::vector<bool> seen(n);
    std::vector<size_t> work{0};
    seen[0] = true;
    for (size_t k = 0; k < work.size(); ++k)
        for (size_t s : f.blocks[work[k]].successors) {
            if (s >= n) return false;
            if (!seen[s]) { seen[s] = true; work.push_back(s); }
        }
    if (work.size() != n) return false;
    dom.assign(n, std::vector<bool>(n, true));
    std::fill(dom[0].begin(), dom[0].end(), false);
    dom[0][0] = true;
    bool changed;
    do {
        changed = false;
        for (size_t b = 1; b < n; ++b) {
            if (f.blocks[b].predecessors.empty()) return false;
            std::vector<bool> next(n, true);
            for (size_t p : f.blocks[b].predecessors) {
                if (p >= n) return false;
                for (size_t d = 0; d < n; ++d) next[d] = next[d] && dom[p][d];
            }
            next[b] = true;
            if (dom[b] != next) { dom[b] = std::move(next); changed = true; }
        }
    } while (changed);
    return true;
}

class Builder {
public:
    SSAFunction f;
    std::string error;
    explicit Builder(size_t n) : definitions(n), incomplete(n) { f.blocks.resize(n); }
    SSAValue value(SSAVariable variable) {
        f.variables.push_back(variable);
        return static_cast<SSAValue>(f.variables.size() - 1);
    }
    SSAValue read(size_t b, SSAVariable variable) {
        auto it = definitions[b].find(variable);
        if (it != definitions[b].end()) return it->second;
        if (b == 0) {
            if (variable < SSA_GPR || variable >= SSA_FP + 32) {
                error = "undefined scalar temporary";
                return 0;
            }
            SSAValue v = value(variable);
            f.inputs.emplace_back(v, variable);
            definitions[b][variable] = v;
            return v;
        }
        // Install before recursing: a backedge can read this very phi.
        SSAValue v = value(variable);
        definitions[b][variable] = v;
        f.blocks[b].phis.push_back({v, variable, {}});
        if (!sealed) incomplete[b].push_back(v);
        else fill(b, v, variable);
        return v;
    }
    void fill(size_t b, SSAValue v, SSAVariable variable) {
        std::vector<std::pair<size_t, SSAValue>> incoming;
        for (size_t p : f.blocks[b].predecessors)
            incoming.emplace_back(p, read(p, variable));
        // Recursive read may grow the phi vector, so never retain a reference.
        for (auto& phi : f.blocks[b].phis)
            if (phi.result == v) { phi.incoming = std::move(incoming); break; }
    }
    bool append(size_t b, const IRInst& ir) {
        Shape s;
        if (!shape(ir, s)) { error = "unsupported SSA operand/effect contract"; return false; }
        SSAInstruction node{ir, 0, {}, s.uses, s.effect};
        uint16_t sources[] = {ir.src1, ir.src2, ir.aux};
        for (unsigned k = 0; k < 3; ++k) if (s.uses & (1u << k)) {
            // Slot 32 is the backend's synthetic XZR, not FP slot 0.
            if (sources[k] == 32) { error = "unlowered XZR operand"; return false; }
            node.inputs[k] = read(b, sources[k] < 32 ? SSA_GPR + sources[k] : sources[k]);
        }
        if (ir.op == IROp::LOAD_REG) {
            if (ir.src1 >= 32) { error = "invalid architectural load slot"; return false; }
            node.ir = IRInst::make(IROp::MOV, ir.dest, 0, 0, 0, ir.arm_pc);
            node.input_mask = 1;
            node.inputs[0] = read(b, (ir.is_fp_load() ? SSA_FP : SSA_GPR) + ir.src1);
        }
        if (s.result) {
            node.result = value(ir.dest);
            definitions[b][ir.dest] = node.result;
        }
        if (ir.op == IROp::STORE_REG) {
            if (ir.dest >= 32) { error = "invalid architectural store slot"; return false; }
            definitions[b][(ir.is_fp_store() ? SSA_FP : SSA_GPR) + ir.dest] = node.inputs[0];
        }
        f.blocks[b].instructions.push_back(node);
        return error.empty();
    }
    void seal() {
        sealed = true;
        for (size_t b = 1; b < f.blocks.size(); ++b)
            for (SSAValue v : incomplete[b]) fill(b, v, f.variables[v]);
    }
private:
    bool sealed = false;
    std::vector<std::unordered_map<SSAVariable, SSAValue>> definitions;
    std::vector<std::vector<SSAValue>> incomplete;
};

// Only operations without implicit state inputs may be value-numbered. Typed
// readers form semantic keys; arm_pc and original destination are not values.
// IMM is excluded: the backend folds a constant into its single following
// consumer as an x86 immediate, which a shared constant register defeats.
bool cse_parameter(const IRInst& i, uint64_t& parameter) {
    parameter = 0;
    switch (i.op) {
        case IROp::MOV: case IROp::ADD: case IROp::SUB: case IROp::MUL:
        case IROp::AND: case IROp::OR: case IROp::XOR:
        case IROp::NOT: case IROp::NEG: case IROp::SMULH: case IROp::UMULH:
        case IROp::SMADDL: case IROp::UMADDL: case IROp::SMSUBL: case IROp::UMSUBL:
            return true;
        case IROp::SHL: case IROp::SHR: case IROp::SAR: case IROp::ROR:
            parameter = i.gpr_shift_params().width; return true;
        case IROp::SEXT: parameter = i.sext_bits(); return true;
        case IROp::ZEXT: parameter = i.zext_bits(); return true;
        case IROp::CLZ: parameter = i.clz_params().bits; return true;
        case IROp::REV64: parameter = i.rev64_params().bits; return true;
        case IROp::UDIV: case IROp::SDIV: parameter = i.div_bits(); return true;
        default: return false;
    }
}
} // namespace

bool build_ssa(const std::vector<SSASourceBlock>& source, SSAFunction& result,
               std::string* error) {
    if (source.empty() || source.size() > 512) return fail(error, "invalid SSA graph size");
    Builder b(source.size());
    for (size_t p = 0; p < source.size(); ++p) {
        b.f.blocks[p].successors = source[p].successors;
        std::set<size_t> unique;
        for (size_t s : source[p].successors) {
            if (s >= source.size() || !unique.insert(s).second)
                return fail(error, "invalid/duplicate CFG successor");
            b.f.blocks[s].predecessors.push_back(p);
        }
    }
    Dominators dom;
    if (!dominators(b.f, dom)) return fail(error, "unreachable block or invalid entry");
    for (size_t i = 0; i < source.size(); ++i)
        for (const auto& ir : source[i].ir.insts)
            if (!b.append(i, ir)) { if (error) *error = b.error; return false; }
    b.seal();
    if (!b.error.empty()) { if (error) *error = b.error; return false; }
    if (!validate_ssa(b.f, error)) return false;
    result = std::move(b.f);
    return true;
}

bool validate_ssa(const SSAFunction& f, std::string* error) {
    Dominators dom;
    if (!dominators(f, dom)) return fail(error, "invalid SSA CFG");
    size_t n = f.variables.size();
    struct Definition { int block = -1; int index = -1; };
    std::vector<Definition> defs(n);
    auto define = [&](SSAValue v, size_t b, int index) {
        if (!v || v >= n || defs[v].block != -1) return false;
        defs[v] = {static_cast<int>(b), index};
        return true;
    };
    for (const auto& input : f.inputs)
        if (input.second < SSA_GPR || input.second >= SSA_FP + 32 ||
            !define(input.first, 0, -2) || f.variables[input.first] != input.second) return fail(error, "invalid/duplicate SSA input");
    for (size_t b = 0; b < f.blocks.size(); ++b) {
        const auto& block = f.blocks[b];
        if (std::set<size_t>(block.successors.begin(), block.successors.end()).size() != block.successors.size())
            return fail(error, "duplicate SSA CFG edge");
        std::set<size_t> expected;
        for (size_t p = 0; p < f.blocks.size(); ++p)
            if (std::find(f.blocks[p].successors.begin(), f.blocks[p].successors.end(), b)
                != f.blocks[p].successors.end()) expected.insert(p);
        if (std::set<size_t>(block.predecessors.begin(), block.predecessors.end()) != expected ||
            block.predecessors.size() != expected.size()) return fail(error, "inconsistent SSA CFG edges");
        for (const auto& phi : block.phis) {
            if (!define(phi.result, b, -1) || f.variables[phi.result] != phi.variable) return fail(error, "multiple SSA definitions");
            std::set<size_t> incoming;
            for (auto edge : phi.incoming) incoming.insert(edge.first);
            if (incoming != expected || incoming.size() != phi.incoming.size())
                return fail(error, "phi must have one input per predecessor");
        }
        for (size_t j = 0; j < block.instructions.size(); ++j) {
            const auto& inst = block.instructions[j];
            Shape s;
            if (!shape(inst.ir, s) || inst.input_mask != s.uses || inst.effect != s.effect ||
                (inst.result != 0) != s.result)
                return fail(error, "invalid SSA instruction contract");
            if (inst.result && !define(inst.result, b, static_cast<int>(j)))
                return fail(error, "multiple SSA definitions");
        }
    }
    auto use = [&](SSAValue v, size_t b, int index) {
        if (!v || v >= n || defs[v].block < 0) return false;
        auto d = defs[v];
        return dom[b][d.block] && (d.block != static_cast<int>(b) || d.index < index);
    };
    for (size_t b = 0; b < f.blocks.size(); ++b) {
        for (const auto& phi : f.blocks[b].phis)
            for (auto edge : phi.incoming)
                if (!use(edge.second, edge.first, static_cast<int>(f.blocks[edge.first].instructions.size())))
                    return fail(error, "phi input does not dominate predecessor edge");
        for (size_t j = 0; j < f.blocks[b].instructions.size(); ++j) {
            const auto& inst = f.blocks[b].instructions[j];
            for (unsigned k = 0; k < 3; ++k)
                if ((inst.input_mask & (1u << k)) && !use(inst.inputs[k], b, static_cast<int>(j)))
                    return fail(error, "SSA definition does not dominate use");
        }
    }
    return true;
}

void optimize_ssa(SSAFunction& f) {
    std::vector<SSAValue> alias(f.variables.size());
    for (SSAValue v = 0; v < alias.size(); ++v) alias[v] = v;
    std::function<SSAValue(SSAValue)> resolve = [&](SSAValue v) -> SSAValue {
        if (alias[v] != v) alias[v] = resolve(alias[v]);
        return alias[v];
    };
    // Propagating copies and trivial phis to a fixed point handles loop phis
    // with self inputs, and chains of newly trivial joins.
    bool changed;
    do {
        changed = false;
        for (auto& block : f.blocks) {
            for (auto& phi : block.phis) {
                if (resolve(phi.result) != phi.result) continue;
                SSAValue unique = 0; bool different = false;
                for (auto& edge : phi.incoming) {
                    SSAValue v = resolve(edge.second);
                    if (v == phi.result) continue;
                    if (unique && unique != v) different = true;
                    unique = v;
                }
                if (!different && unique) { alias[phi.result] = unique; changed = true; }
            }
            for (auto& inst : block.instructions)
                if (inst.ir.op == IROp::MOV && resolve(inst.result) == inst.result) {
                    alias[inst.result] = resolve(inst.inputs[0]); changed = true;
                }
        }
    } while (changed);
    Dominators dom;
    dominators(f, dom);
    // Dominator-scoped value numbering: an expression in a sibling branch
    // cannot supply a value at a join. No memory load is value-numbered.
    using Key = std::array<uint64_t, 5>;
    std::vector<std::vector<size_t>> children(f.blocks.size());
    for (size_t b = 1; b < f.blocks.size(); ++b) {
        size_t parent = 0, depth = 0;
        for (size_t d = 0; d < f.blocks.size(); ++d) if (d != b && dom[b][d]) {
            size_t dd = std::count(dom[d].begin(), dom[d].end(), true);
            if (dd > depth) { depth = dd; parent = d; }
        }
        children[parent].push_back(b);
    }
    std::function<void(size_t, std::map<Key, SSAValue>)> number =
        [&](size_t b, std::map<Key, SSAValue> available) {
            for (auto& inst : f.blocks[b].instructions) {
                for (unsigned k = 0; k < 3; ++k) if (inst.input_mask & (1u << k))
                    inst.inputs[k] = resolve(inst.inputs[k]);
                uint64_t parameter;
                if (inst.result && !inst.effect && resolve(inst.result) == inst.result &&
                    cse_parameter(inst.ir, parameter)) {
                    Key key{static_cast<uint64_t>(inst.ir.op), parameter,
                            inst.inputs[0], inst.inputs[1], inst.inputs[2]};
                    auto found = available.find(key);
                    if (found != available.end()) alias[inst.result] = resolve(found->second);
                    else available.emplace(key, inst.result);
                }
            }
            for (size_t child : children[b]) number(child, available);
        };
    number(0, {});
    for (auto& block : f.blocks) {
        for (auto& phi : block.phis) for (auto& edge : phi.incoming) edge.second = resolve(edge.second);
        for (auto& inst : block.instructions)
            for (unsigned k = 0; k < 3; ++k) if (inst.input_mask & (1u << k))
                inst.inputs[k] = resolve(inst.inputs[k]);
    }
    // Mark from effects through def-use edges. A phi's incoming uses are on
    // edges, but all are dependencies of its live result (including backedges).
    std::vector<std::vector<SSAValue>> dependencies(f.variables.size());
    std::vector<bool> live(f.variables.size());
    std::vector<SSAValue> work;
    for (const auto& block : f.blocks) {
        for (const auto& phi : block.phis) if (resolve(phi.result) == phi.result)
            for (auto edge : phi.incoming) dependencies[phi.result].push_back(edge.second);
        for (const auto& inst : block.instructions) {
            if (inst.result && resolve(inst.result) == inst.result)
                for (unsigned k = 0; k < 3; ++k) if (inst.input_mask & (1u << k))
                    dependencies[inst.result].push_back(inst.inputs[k]);
            if (inst.effect) {
                if (inst.result) work.push_back(inst.result);
                for (unsigned k = 0; k < 3; ++k) if (inst.input_mask & (1u << k))
                    work.push_back(inst.inputs[k]);
            }
        }
    }
    while (!work.empty()) {
        SSAValue v = work.back(); work.pop_back();
        if (live[v]) continue;
        live[v] = true;
        for (SSAValue input : dependencies[v]) work.push_back(input);
    }
    for (auto& block : f.blocks) {
        block.phis.erase(std::remove_if(block.phis.begin(), block.phis.end(),
            [&](const SSAPhi& p) { return resolve(p.result) != p.result || !live[p.result]; }), block.phis.end());
        block.instructions.erase(std::remove_if(block.instructions.begin(), block.instructions.end(),
            [&](const SSAInstruction& i) { return !i.effect && (!i.result || resolve(i.result) != i.result || !live[i.result]); }), block.instructions.end());
    }
    f.inputs.erase(std::remove_if(f.inputs.begin(), f.inputs.end(),
        [&](const auto& input) { return !live[input.first]; }), f.inputs.end());
}

bool lower_ssa(const SSAFunction& f, std::vector<SSASourceBlock>& result,
               std::string* error, SSAPhiLowering phis) {
    if (!validate_ssa(f, error)) return false;
    std::vector<SSASourceBlock> out(f.blocks.size());
    std::vector<uint16_t> homes(f.variables.size());
    unsigned next = 33;
    auto home = [&](SSAValue v) -> uint16_t {
        if (!homes[v] && next < 4096) homes[v] = static_cast<uint16_t>(next++);
        return homes[v];
    };
    std::set<SSAValue> body_inputs;
    if (phis == SSAPhiLowering::ArchitecturalState)
        for (const auto& block : f.blocks) for (const auto& node : block.instructions)
            for (unsigned k = 0; k < 3; ++k) if (node.input_mask & (1u << k))
                body_inputs.insert(node.inputs[k]);
    // A shared constant (e.g. forwarded through a STORE_REG/LOAD_REG pair)
    // is rematerialized right before each consumer that can fold it as src2,
    // under the backend's own fold rules, so its immediate form survives.
    std::unordered_map<SSAValue, uint64_t> constants;
    std::unordered_map<SSAValue, unsigned> constant_uses;
    for (const auto& block : f.blocks)
        for (const auto& node : block.instructions)
            if (node.ir.op == IROp::IMM && node.result) constants[node.result] = node.ir.imm_value();
    for (const auto& block : f.blocks) {
        for (const auto& phi : block.phis)
            for (auto edge : phi.incoming) if (constants.count(edge.second)) ++constant_uses[edge.second];
        for (const auto& node : block.instructions)
            for (unsigned k = 0; k < 3; ++k)
                if ((node.input_mask & (1u << k)) && constants.count(node.inputs[k]))
                    ++constant_uses[node.inputs[k]];
    }
    auto rematerialize = [&](const SSAInstruction& node) -> bool {
        if (!(node.input_mask & 2) || node.inputs[0] == node.inputs[1]) return false;
        auto c = constants.find(node.inputs[1]);
        if (c == constants.end() || constant_uses[c->first] < 2) return false;
        switch (node.ir.op) {
            case IROp::SHL: case IROp::SHR: case IROp::SAR: case IROp::ROR: return true;
            case IROp::ADD: case IROp::SUB: case IROp::AND:
            case IROp::OR:  case IROp::XOR: case IROp::MUL: {
                const int64_t v = static_cast<int64_t>(c->second);
                return static_cast<int64_t>(static_cast<int32_t>(v)) == v;
            }
            default: return false;
        }
    };
    // Read each input just before its first use in the entry block, keeping
    // short live ranges for the backend's ten host registers. A load never
    // moves past a store to its own slot. Inputs used only by later blocks
    // or phi edges stay at the entry's start.
    const auto& entry = f.blocks[0].instructions;
    std::vector<std::vector<IRInst>> input_loads(entry.size() + 1);
    for (auto input : f.inputs) {
        if (phis == SSAPhiLowering::ArchitecturalState && !body_inputs.count(input.first)) continue;
        bool fp = input.second >= SSA_FP;
        auto slot = static_cast<uint16_t>(input.second - (fp ? SSA_FP : SSA_GPR));
        size_t at = 0;
        bool used = false;
        for (size_t j = 0; j < entry.size() && !used; ++j) {
            const auto& node = entry[j];
            for (unsigned k = 0; k < 3; ++k)
                used |= (node.input_mask & (1u << k)) && node.inputs[k] == input.first;
            used |= node.ir.op == IROp::STORE_REG &&
                (node.ir.is_fp_store() ? SSA_FP : SSA_GPR) + node.ir.dest == input.second;
            if (used) at = j;
        }
        // Keep IMM -> consumer adjacency: the backend skips a constant's
        // mov only when its single consumer immediately follows it.
        while (at > 0 && entry[at - 1].ir.op == IROp::IMM) --at;
        input_loads[at].push_back(IRInst::make_load_reg(home(input.first), slot, fp));
    }
    for (size_t b = 0; b < f.blocks.size(); ++b) {
        out[b].successors = f.blocks[b].successors;
        if (phis == SSAPhiLowering::ArchitecturalState) for (const auto& phi : f.blocks[b].phis) {
            if (phi.variable < SSA_GPR || phi.variable >= SSA_FP + 32)
                return fail(error, "temporary phi requires edge copies");
            bool fp = phi.variable >= SSA_FP;
            auto slot = static_cast<uint16_t>(phi.variable - (fp ? SSA_FP : SSA_GPR));
            out[b].ir.insts.push_back(IRInst::make_load_reg(home(phi.result), slot, fp));
        }
        for (size_t j = 0; j < f.blocks[b].instructions.size(); ++j) {
            if (b == 0)
                out[0].ir.insts.insert(out[0].ir.insts.end(),
                                       input_loads[j].begin(), input_loads[j].end());
            const auto& node = f.blocks[b].instructions[j];
            IRInst i = node.ir;
            if (node.result) i.dest = home(node.result);
            if (node.input_mask & 1) i.src1 = home(node.inputs[0]);
            if (node.input_mask & 2) i.src2 = home(node.inputs[1]);
            if (rematerialize(node)) {
                if (next >= 4096) return fail(error, "SSA lowering exceeds vreg budget");
                i.src2 = static_cast<uint16_t>(next++);
                out[b].ir.insts.push_back(IRInst::make_imm(i.src2, constants[node.inputs[1]]));
            }
            if (node.input_mask & 4) i.aux = home(node.inputs[2]);
            out[b].ir.insts.push_back(i);
        }
        if (b == 0) // empty entry (Tier-2 preheader): every input loads here
            out[0].ir.insts.insert(out[0].ir.insts.end(),
                                   input_loads[entry.size()].begin(), input_loads[entry.size()].end());
    }
    if (phis == SSAPhiLowering::EdgeCopies) for (size_t b = 0; b < f.blocks.size(); ++b) {
        for (size_t e = 0; e < f.blocks[b].successors.size(); ++e) {
            size_t target = f.blocks[b].successors[e];
            if (f.blocks[target].phis.empty()) continue;
            SSASourceBlock edge;
            std::vector<std::pair<uint16_t, uint16_t>> copies;
            // Save all sources before assigning any destination: parallel
            // semantics also for cyclic phi copies on loop backedges.
            for (const auto& phi : f.blocks[target].phis) {
                SSAValue input = 0;
                for (auto pair : phi.incoming) if (pair.first == b) input = pair.second;
                uint16_t from = home(input), to = home(phi.result);
                if (from == to) continue;
                if (next >= 4096) return fail(error, "SSA lowering exceeds vreg budget");
                uint16_t temp = static_cast<uint16_t>(next++);
                edge.ir.insts.push_back(IRInst::make(IROp::MOV, temp, from));
                copies.emplace_back(to, temp);
            }
            for (auto copy : copies) edge.ir.insts.push_back(IRInst::make(IROp::MOV, copy.first, copy.second));
            edge.successors = {target};
            out[b].successors[e] = out.size();
            out.push_back(std::move(edge));
        }
    }
    if (next >= 4096) return fail(error, "SSA lowering exceeds vreg budget");
    result = std::move(out);
    return true;
}

bool optimize_ir_ssa(IRBlock& block, bool check) {
    for (const auto& i : block.insts) {
        Shape s;
        // Existing flattened branch codegen retains compile-time mappings
        // across skips. It cannot yet consume edge-copy CFGs safely.
        if (i.op == IROp::BRCOND_SKIP || !shape(i, s)) return false;
    }
    SSAFunction f;
    std::string error;
    if (!build_ssa({SSASourceBlock{block, {}}}, f, &error)) {
        if (check) fprintf(stderr, "[SSA] build @0x%llx: %s\n", (unsigned long long)block.start_pc, error.c_str());
        return false;
    }
    optimize_ssa(f);
    std::vector<SSASourceBlock> lowered;
    if (!lower_ssa(f, lowered, &error)) {
        if (check) fprintf(stderr, "[SSA] lower @0x%llx: %s\n", (unsigned long long)block.start_pc, error.c_str());
        return false;
    }
    block.insts = std::move(lowered[0].ir.insts);
    return true;
}
} // namespace arm64emu
