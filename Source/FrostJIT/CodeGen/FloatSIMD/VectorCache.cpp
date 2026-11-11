// VectorCache.cpp — XMM vector register cache.
#include <cstdlib>
#include <algorithm>
#include <vector>
#include <stdexcept>
//
// XMM0-2 are instruction scratch. Tier-1 keeps a stable hot loop core
// in XMM3-15 and allocates the remaining slots by next use under pressure.
// Unsupported operations synchronize locally. Retained loop edges reconcile
// demand values with the entry layout, including parallel-copy cycles.
// Precise exits and NaN recovery publish the actual layout at that point.
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include "FrostJIT/CodeGen/Native/NativeSIMD.hpp"
namespace arm64emu {

// ── Cache enablement pre-scan ──────────────────────────────────────────
// Ops that either have a cached fast path for pinned XMM operands
// (including unary and indexed FP) or are GPR-only
// ops that neither read cpu.v_lo/v_hi nor clobber XMM3-15. Everything
// else is a synchronization boundary unless vec_inst_effects describes it.
// (If a whitelisted GPR op does fall through to emit_call_interp, the
// guard inside emit_call_interp writes back + reloads the pinned XMM
// regs around the host call, so the cache stays correct.)
static bool vec_cache_compatible_op(IROp op) {
    switch (op) {
        case IROp::SIMD_FP_FMA:
        case IROp::SIMD_FP_ARITH:
        case IROp::SIMD_FP_UNARY:
        case IROp::SIMD_LOGICAL:
        case IROp::SIMD_DUP:
        case IROp::SIMD_MOVI:     // constant broadcast — cached fast path
        case IROp::SIMD_ORRIMM:   // dest read-modify-write — cached fast path
        case IROp::SIMD_ST16:     // 16-byte guest store — cached fast path
        case IROp::SIMD_LD16:     // 16-byte guest load — cached fast path
        case IROp::NOP: case IROp::CLEAR_EXCLUSIVE: case IROp::IMM: case IROp::MOV:
        case IROp::LOAD_REG: case IROp::STORE_REG:
        case IROp::LOAD_MEM: case IROp::STORE_MEM:
        case IROp::ADD: case IROp::SUB: case IROp::MUL:
        case IROp::AND: case IROp::OR: case IROp::XOR:
        case IROp::SHL: case IROp::SHR: case IROp::SAR: case IROp::ROR:
        case IROp::NOT: case IROp::NEG: case IROp::SEXT: case IROp::ZEXT:
        case IROp::CLZ: case IROp::CLS: case IROp::RBIT:
        case IROp::REV16: case IROp::REV32: case IROp::REV64:
        case IROp::ADDS: case IROp::SUBS: case IROp::ADCS: case IROp::SBCS:
        case IROp::TST: case IROp::TST_ZERO:
        case IROp::BRCOND_ZERO: case IROp::BRCOND_BIT:
        case IROp::CSEL: case IROp::CSINC: case IROp::CSINV: case IROp::CSNEG:
        case IROp::CCMP:
        case IROp::BFM: case IROp::UBFM: case IROp::SBFM: case IROp::EXTR:
        case IROp::BR: case IROp::BRCOND: case IROp::BRCOND_FALLTHRU:
        case IROp::BRCOND_SKIP:
        case IROp::UDIV: case IROp::SDIV:
        case IROp::SMADDL: case IROp::UMADDL: case IROp::SMULH: case IROp::UMULH:
        case IROp::SMSUBL: case IROp::UMSUBL:
        case IROp::MRS: case IROp::MSR:
            return true;
        default:
            return false;
    }
}

// Vector operands/effects used both by admission and Tier-1's allocator.
// Unknown operations are synchronization boundaries; no guessed effects.
bool FrostJIT::vec_inst_operands(const IRInst& inst, uint32_t& reads, uint32_t& writes) const {
    reads = writes = 0;
    auto read = [&](unsigned v) { reads |= uint32_t(1) << (v & 31); };
    auto write = [&](unsigned v) { writes |= uint32_t(1) << (v & 31); };
    switch (inst.op) {
        case IROp::SIMD_NATIVE_EXTRA: {
            const uint32_t w = inst.native_simd_opcode();
            if (!native_simd_cache_compatible(w)) return false;
            const auto kind = classify_native_simd(w);
            write(w & 31);
            if (kind != NativeSimdKind::ImmediateFp) read((w >> 5) & 31);
            if (kind == NativeSimdKind::Extract || kind == NativeSimdKind::CompareFp ||
                kind == NativeSimdKind::Select || kind == NativeSimdKind::MultiplyAdd ||
                kind == NativeSimdKind::WidenThree || kind == NativeSimdKind::HalvingAdd ||
                kind == NativeSimdKind::TestBits || kind == NativeSimdKind::AbsDiffAcc)
                read((w >> 16) & 31);
            if (kind == NativeSimdKind::Select || kind == NativeSimdKind::MultiplyAdd ||
                kind == NativeSimdKind::AbsDiffAcc)
                read(w & 31);
            // MLAL/MLSL and ADALP accumulate into Vd.
            if ((kind == NativeSimdKind::WidenThree && (((w >> 12) & 15) == 0x8 || ((w >> 12) & 15) == 0xA)) ||
                (kind == NativeSimdKind::PairLong && ((w >> 14) & 1)))
                read(w & 31);
            if ((w & (1u << 30)) && (kind == NativeSimdKind::NarrowShift ||
                kind == NativeSimdKind::NarrowFp ||
                (kind == NativeSimdKind::HalfFp && !(w & 0x1000)))) read(w & 31);
            return true;
        }
        case IROp::SIMD_CVTF:
            if (inst.cvtf_params().subop > 1) return false;
            read(inst.src1); write(inst.dest); return true;
        case IROp::SIMD_USRA:
        case IROp::SIMD_SSRA:
        case IROp::SIMD_URSRA:
        case IROp::SIMD_SRSRA:
        case IROp::SIMD_SLI:
        case IROp::SIMD_SRI:
            read(inst.dest);
            [[fallthrough]];
        case IROp::SIMD_SHL:
        case IROp::SIMD_USHR:
        case IROp::SIMD_SSHR:
            read(inst.src1); write(inst.dest); return true;
        case IROp::SIMD_FP_FMA:
            read(inst.dest);
            [[fallthrough]];
        case IROp::SIMD_ARITH:
        case IROp::SIMD_CMP:
        case IROp::SIMD_FP_ARITH:
        case IROp::SIMD_LOGICAL:
            read(inst.src1); read(inst.src2); write(inst.dest); return true;
        case IROp::SIMD_FP_UNARY:
            read(inst.src1); write(inst.dest); return true;
        case IROp::SIMD_DUP:
        case IROp::SIMD_MOVI:
            write(inst.dest); return true;
        case IROp::SIMD_ORRIMM:
            read(inst.dest); write(inst.dest); return true;
        case IROp::SIMD_ST16: {
            const auto p = inst.st16_params();
            for (unsigned i = 0; i < (p.count ? p.count : 1); ++i)
                read(p.broadcast ? inst.src2 : inst.src2 + i);
            return true;
        }
        case IROp::SIMD_LD16: {
            const auto p = inst.ld16_params();
            for (unsigned i = 0; i < (p.count ? p.count : 1); ++i) write(inst.dest + i);
            return true;
        }
        case IROp::LOAD_REG:
            return !inst.is_fp_load();
        case IROp::STORE_REG:
            return !inst.is_fp_store();
        default:
            return vec_cache_compatible_op(inst.op);
    }
}

bool FrostJIT::vec_inst_effects(const IRInst& inst, VectorEffects& effects) const {
    effects = {};
    if (vec_inst_operands(inst, effects.reads, effects.writes)) return true;
    auto read = [&](unsigned v) { effects.reads |= uint32_t(1) << (v & 31); };
    auto write = [&](unsigned v) { effects.writes |= uint32_t(1) << (v & 31); };
    if (inst.op == IROp::SIMD_NATIVE_EXTRA) {
        const uint32_t word = inst.native_simd_opcode();
        const auto kind = classify_native_simd(word);
        // These specialized bodies only access the stated vector operands
        // and FP control/status; they neither access guest memory nor change
        // control flow. Other helpers retain the unknown-effects boundary.
        if (kind != NativeSimdKind::Select && kind != NativeSimdKind::RoundFp &&
            kind != NativeSimdKind::VariableShift) return false;
        read((word >> 5) & 31);
        if (kind != NativeSimdKind::RoundFp) read((word >> 16) & 31);
        if (kind == NativeSimdKind::Select) read(word & 31);
        write(word & 31);
        effects.memory_operands = true;
        // Inline variable shifts borrow XMM0-5. A C++ helper has the full
        // SysV XMM clobber set, irrespective of the compiler used to build it.
        effects.xmm_clobbers = native_simd_inlinable(word) ? 0x003f : 0xffff;
        return true;
    }
    if (inst.op == IROp::SIMD_STRUCT_MEM) {
        // LD2-LD4 / ST2-ST4 read or write v_lo/v_hi directly and use only XMM0-2 and XMM12-15
        // (compile_simd_structure_), which the allocator fills last.
        const NativeStructureParams params = inst.native_structure_params();
        for (unsigned i = 0; i < params.count; ++i) {
            if (params.load) write(params.first + i); else read(params.first + i);
        }
        effects.memory_operands = true;
        effects.xmm_clobbers = 0xF007;
        return true;
    }
    effects.scalar = true;
    switch (inst.op) {
        case IROp::FP_BINOP: case IROp::FP_CSEL:
            read(inst.src2); [[fallthrough]];
        case IROp::FP_UNOP: case IROp::FP_MOV:
        case IROp::FRINT: case IROp::FCVT_S2D: case IROp::FCVT_D2S:
            read(inst.src1); write(inst.dest); break;
        case IROp::FMADD: case IROp::FMSUB: case IROp::FNMADD: case IROp::FNMSUB:
            read(inst.src1); read(inst.src2); read(inst.fp_fused_params().acc);
            write(inst.dest); break;
        case IROp::FP_MOVI: case IROp::FP_I2F:
            write(inst.dest); break;
        case IROp::FP_F2I: case IROp::FMOV_F2G:
            read(inst.src1); break;
        case IROp::FP_CMP:
            read(inst.src1);
            if (!inst.fp_cmp_params().with_zero) read(inst.src2);
            break;
        case IROp::FP_F2I_FIXED:
            read(inst.src1);
            if (inst.fp_f2i_fixed_params().fp_reg) write(inst.dest);
            break;
        case IROp::FP_I2F_FIXED:
            if (inst.fp_i2f_fixed_params().fp_reg) read(inst.src1);
            write(inst.dest); break;
        case IROp::FMOV_G2F:
            write(inst.dest); break;
        default:
            return false;
    }
    // Scalar cold steps currently rejoin inline after the commit. Keep their
    // original destinations resident until they have committed; unlike
    // vector guards they do not yet have a deferred pre-commit snapshot.
    if (inst.op != IROp::FP_MOV && inst.op != IROp::FP_MOVI && inst.op != IROp::FMOV_G2F)
        effects.recovery_reads = effects.writes;
    return true;
}

bool FrostJIT::vec_cache_may_enable(const IRBlock& block) {
    static const bool no_vec_cache = getenv("BIFROST_NO_VEC_CACHE") != nullptr;
    if (no_vec_cache || !has_fma3()) return false;
    uint32_t used = 0, written = 0;
    unsigned uses[32] = {};
    std::vector<uint32_t> operand_sets;
    std::vector<VectorEffects> effects_by_op;
    bool boundary = false, skip = false, has_vector = false;
    for (size_t i = 0; i < block.insts.size(); ++i) {
        const auto& inst = block.insts[i];
        VectorEffects effects;
        if (!vec_inst_effects(inst, effects)) {
            boundary = true;
            effects_by_op.push_back({UINT32_MAX, UINT32_MAX, 0, false});
            continue;
        }
        effects_by_op.push_back(effects);
        skip |= inst.op == IROp::BRCOND_SKIP;
        written |= effects.writes;
        uint32_t touched = effects.reads | effects.writes | effects.recovery_reads;
        has_vector |= touched && !effects.scalar;
        used |= touched;
        if (touched) operand_sets.push_back(touched);
        while (touched) {
            const unsigned v = __builtin_ctz(touched);
            touched &= touched - 1;
            ++uses[v];
            vec_uses_[v].push_back(static_cast<uint16_t>(i));
        }
    }
    const int count = __builtin_popcount(used);
    if (!count || !has_vector) return false;
    constexpr int capacity = VEC_XMM_END - VEC_XMM_START + 1;
    // Leaf-inlined conditional skips need one fixed layout at the join.
    // Retain their existing complete-layout contract until vector CFG
    // merge copies are represented explicitly.
    if (skip && (boundary || count > capacity)) return false;
    vec_allocator_active_ = !skip;
    vec_cache_active_ = true;
    // Preserve the complete layout when it fits. Under pressure reserve
    // only the demand slots this block's simultaneous operands require.
    // A common broadcast source can leave twelve loop-carried pins; a
    // four-register load may require four demand slots instead.
    vec_fixed_count_ = count <= capacity ? count : capacity - 1;
    std::vector<int> ranked;
    for (int v = 0; v < 32; ++v) if (used & (uint32_t(1) << v)) ranked.push_back(v);
    if (!skip)
        std::sort(ranked.begin(), ranked.end(), [&](int a, int b) {
            return uses[a] != uses[b] ? uses[a] > uses[b] : a < b;
        });
    if (count > capacity) {
        for (; vec_fixed_count_ >= 0; --vec_fixed_count_) {
            uint32_t fixed = 0;
            for (int i = 0; i < vec_fixed_count_; ++i) fixed |= uint32_t(1) << ranked[i];
            bool fits = true;
            for (uint32_t operands : operand_sets)
                if (__builtin_popcount(operands & ~fixed) > capacity - vec_fixed_count_) {
                    fits = false; break;
                }
            if (fits) break;
        }
    }
    if (vec_allocator_active_) {
        // Compare only three layouts: the maximum legal core, half of it,
        // and demand-only. Estimate read/overwrite traffic with protected
        // operands and next use. Helpers and loop tails are conservatively
        // charged full publication; entry-layout copies can cost less.
        // This is a bounded static heuristic, not a runtime layout search.
        const auto& last = block.insts.back();
        const bool loop = (last.op == IROp::BRCOND || last.op == IROp::BRCOND_ZERO ||
                           last.op == IROp::BRCOND_BIT || last.op == IROp::BRCOND_FALLTHRU) &&
                          last.branch_target() == block.start_pc;
        auto estimate = [&](int fixed_count) {
            bool mapped[32] = {}, valid[32] = {}, dirty[32] = {}, fixed[32] = {};
            unsigned cost = 2 * fixed_count;
            for (int i = 0; i < fixed_count; ++i)
                mapped[ranked[i]] = valid[ranked[i]] = fixed[ranked[i]] = true;
            for (unsigned trip = 0; trip < (loop ? 2u : 1u); ++trip) {
                for (size_t i = 0; i < effects_by_op.size(); ++i) {
                    const auto& e = effects_by_op[i];
                    if (e.reads == UINT32_MAX || e.memory_operands) {
                        for (int v = 0; v < 32; ++v) {
                            if (valid[v] && (dirty[v] || (loop && fixed[v] && (written & (1u << v))))) cost += 2;
                            valid[v] = dirty[v] = false; mapped[v] = fixed[v];
                        }
                        continue;
                    }
                    const uint32_t reads = e.reads | e.recovery_reads;
                    const uint32_t operands = reads | e.writes;
                    uint32_t pending = operands;
                    while (pending) {
                        const int v = __builtin_ctz(pending); pending &= pending - 1;
                        if (!mapped[v]) {
                            int resident = 0;
                            for (bool m : mapped) resident += m;
                            if (resident == capacity) {
                                int victim = -1, farthest = -1;
                                for (int old = 0; old < 32; ++old) {
                                    if (!mapped[old] || fixed[old] || (operands & (1u << old))) continue;
                                    const auto& u = vec_uses_[old];
                                    const auto next = std::upper_bound(u.begin(), u.end(), i);
                                    const int distance = next == u.end() ? INT32_MAX : *next;
                                    if (distance > farthest || (distance == farthest && victim >= 0 &&
                                        dirty[victim] && !dirty[old])) { victim = old; farthest = distance; }
                                }
                                if (victim < 0) return UINT32_MAX;
                                if (dirty[victim]) cost += 2;
                                mapped[victim] = valid[victim] = dirty[victim] = false;
                            }
                            mapped[v] = true;
                        }
                        if ((reads & (1u << v)) && !valid[v]) { cost += 2; valid[v] = true; }
                    }
                    for (int v = 0; v < 32; ++v)
                        if (e.writes & (1u << v)) valid[v] = dirty[v] = true;
                }
                if (loop) for (int v = 0; v < 32; ++v) {
                    if (!fixed[v]) {
                        if (dirty[v]) cost += 2;
                        mapped[v] = valid[v] = dirty[v] = false;
                    } else if (!valid[v]) { cost += 2; valid[v] = true; }
                }
            }
            for (int v = 0; v < 32; ++v) if (dirty[v]) cost += 2;
            return cost;
        };
        int best = vec_fixed_count_;
        unsigned best_cost = estimate(best);
        for (int candidate : {vec_fixed_count_ / 2, 0}) {
            const unsigned cost = estimate(candidate);
            if (cost < best_cost) { best = candidate; best_cost = cost; }
        }
        vec_fixed_count_ = best;
    }
    for (int v = 0; v < 32; ++v) vec_written_this_block_[v] = (written >> v) & 1;
    for (int i = 0; i < vec_fixed_count_; ++i) {
        const int v = ranked[i], xmm = VEC_XMM_START + i;
        vec_cache_[v] = xmm;
        vec_xmm_owner_[xmm] = v;
        vec_pinned_[vec_pinned_count_++] = v;
    }
    // Seed demand slots with values read before their first definition.
    // Their layout is a loop-entry contract, not another set of pins: body
    // allocation may evict them. Backedges reconcile this layout explicitly.
    if (vec_allocator_active_) {
        uint32_t defined = 0;
        int next_xmm = VEC_XMM_START + vec_fixed_count_;
        for (const auto& effects : effects_by_op) {
            if (effects.reads == UINT32_MAX || effects.memory_operands) break;
            uint32_t live_in = (effects.reads | effects.recovery_reads) & ~defined;
            while (live_in && next_xmm <= VEC_XMM_END) {
                const int v = __builtin_ctz(live_in); live_in &= live_in - 1;
                if (vec_cache_[v] >= 0) continue;
                vec_cache_[v] = next_xmm;
                vec_xmm_owner_[next_xmm++] = v;
                vec_pinned_[vec_pinned_count_++] = v;
            }
            defined |= effects.writes;
            if (next_xmm > VEC_XMM_END) break;
        }
    }
    for (int v = 0; v < 32; ++v) vec_entry_cache_[v] = vec_cache_[v];
    return true;
}

void FrostJIT::vec_writeback_one(int v) {
    const int xmm = vec_cache_[v];
    store_lo(xmm, V_LO_OFF + v * 8);
    if (!fp_cache_active_) store_hi(xmm, V_HI_OFF + v * 8);
    ++vec_spills_;
}

void FrostJIT::vec_prepare_operands(uint32_t reads, uint32_t writes) {
    const uint32_t operands = reads | writes;
    uint32_t pending = operands;
    while (pending) {
        const int v = __builtin_ctz(pending);
        pending &= pending - 1;
        if (vec_cache_[v] >= 0) {
            if (!vec_valid_[v] && (reads & (uint32_t(1) << v)))
                vec_emit_load_lo_hi(vec_cache_[v], v);
            continue;
        }
        int chosen = -1;
        int farthest = -1;
        for (int xmm = VEC_XMM_START + vec_fixed_count_; xmm <= VEC_XMM_END; ++xmm) {
            const int old = vec_xmm_owner_[xmm];
            if (old < 0) { chosen = xmm; break; }
            if (operands & (uint32_t(1) << old)) continue;
            const auto& uses = vec_uses_[old];
            const auto next = std::upper_bound(uses.begin(), uses.end(), cur_op_index_);
            const int distance = next == uses.end() ? 0x7fffffff : *next;
            if (distance > farthest || (distance == farthest && chosen >= 0 &&
                vec_dirty_[vec_xmm_owner_[chosen]] && !vec_dirty_[old])) {
                chosen = xmm; farthest = distance;
            }
        }
        // The pre-scan reserved enough slots for this complete operand set.
        if (chosen < 0) throw std::logic_error("vector allocator exhausted protected operands");
        const int old = vec_xmm_owner_[chosen];
        if (old >= 0) {
            if (vec_dirty_[old]) vec_writeback_one(old);
            vec_cache_[old] = -1;
            vec_dirty_[old] = false;
            ++vec_evictions_;
        }
        vec_cache_[v] = chosen;
        vec_xmm_owner_[chosen] = v;
        vec_pinned_[chosen - VEC_XMM_START] = v;
        vec_pinned_count_ = std::max(vec_pinned_count_, chosen - VEC_XMM_START + 1);
        vec_valid_[v] = false;
        if (reads & (uint32_t(1) << v)) vec_emit_load_lo_hi(chosen, v);
        vec_dirty_[v] = false;
    }
}

void FrostJIT::vec_drop_dynamic() {
    for (int i = vec_fixed_count_; i < vec_pinned_count_; ++i) {
        const int v = vec_pinned_[i];
        vec_xmm_owner_[vec_cache_[v]] = -1;
        vec_cache_[v] = -1;
        vec_dirty_[v] = false;
        vec_valid_[v] = false;
    }
    vec_pinned_count_ = vec_fixed_count_;
}

void FrostJIT::vec_writeback_dynamic() {
    if (!vec_allocator_active_) return;
    for (int i = vec_fixed_count_; i < vec_pinned_count_; ++i) {
        const int v = vec_pinned_[i];
        if (vec_dirty_[v] && vec_entry_cache_[v] < 0) vec_writeback_one(v);
    }
    struct Copy { int target, source, v; };
    std::vector<Copy> copies;
    for (int v = 0; v < 32; ++v) {
        const int target = vec_entry_cache_[v];
        if (target < VEC_XMM_START + vec_fixed_count_) continue;
        const int source = vec_valid_[v] ? vec_cache_[v] : -1;
        if (source != target) copies.push_back({target, source, v});
    }
    // Parallel copies preserve all sources, including cycles. XMM0 is
    // instruction scratch and is not part of any entry or recovery layout.
    while (!copies.empty()) {
        auto ready = std::find_if(copies.begin(), copies.end(), [&](const Copy& copy) {
            return std::none_of(copies.begin(), copies.end(), [&](const Copy& other) {
                return other.source == copy.target;
            });
        });
        if (ready == copies.end()) {
            const int source = copies.front().source;
            movdqa(0, source);
            for (auto& copy : copies) if (copy.source == source) copy.source = 0;
            continue;
        }
        if (ready->source < 0) {
            const bool valid = vec_valid_[ready->v];
            vec_emit_load_lo_hi(ready->target, ready->v);
            vec_valid_[ready->v] = valid;
        } else movdqa(ready->target, ready->source);
        copies.erase(ready);
    }
    // The retained loop entry expects the fixed layout to be valid. A
    // helper followed only by overwrites may have left some pins unloaded.
    // Re-establish that entry contract without changing fall-through state.
    for (int i = 0; i < vec_fixed_count_; ++i) {
        const int v = vec_pinned_[i];
        if (!vec_valid_[v]) {
            vec_emit_load_lo_hi(vec_cache_[v], v);
            vec_valid_[v] = false; // The alternate fall-through did not reload it.
        }
    }
}

// ── Scalar-FP cache (lo-only) ─────────────────────────────────────────
// Ops allowed in an fp-cache block: every op either has a cache-aware
// scalar-FP fast path (pinned XMM operand handling), reads/writes only
// GPRs (the vec-cache GPR whitelist), or is a call with a writeback+reload
// guard (BL_CALL/BLR_CALL — the callee may clobber ANY XMM and ANY
// cpu.v_lo entry, so the guard in FrostJIT.cpp flushes + reloads around it).
// SIMD ops, SVC, CALL_INTERP, atomics, and the FMOV hi-half ops (which
// read/write v_hi, never pinned in the lo-only cache) all disqualify.
static bool fp_cache_compatible_op(IROp op) {
    switch (op) {
        // Scalar FP ops with cache-aware fast paths:
        case IROp::FP_BINOP:
        case IROp::FP_CSEL:
        case IROp::FP_UNOP:
        case IROp::FP_MOV:
        case IROp::FP_CMP:
        case IROp::FP_MOVI:
        case IROp::FP_F2I:
        case IROp::FP_I2F:
        case IROp::FP_F2I_FIXED:
        case IROp::FP_I2F_FIXED:
        case IROp::FCVT_S2D:
        case IROp::FCVT_D2S:
        case IROp::FRINT:
        case IROp::FMADD: case IROp::FMSUB:
        case IROp::FNMADD: case IROp::FNMSUB:
        case IROp::FMOV_G2F: case IROp::FMOV_F2G:
        // GPR-only (sf=0) and FP (sf=1) register moves:
        case IROp::LOAD_REG: case IROp::STORE_REG:
        // Calls with the writeback+reload guard:
        case IROp::BL_CALL: case IROp::BLR_CALL:
        // GPR-only ops (mirrors vec_cache_compatible_op's integer whitelist):
        case IROp::NOP: case IROp::CLEAR_EXCLUSIVE: case IROp::IMM: case IROp::MOV:
        case IROp::LOAD_MEM: case IROp::STORE_MEM:
        case IROp::ADD: case IROp::SUB: case IROp::MUL:
        case IROp::AND: case IROp::OR: case IROp::XOR:
        case IROp::SHL: case IROp::SHR: case IROp::SAR: case IROp::ROR:
        case IROp::NOT: case IROp::NEG: case IROp::SEXT: case IROp::ZEXT:
        case IROp::CLZ: case IROp::CLS: case IROp::RBIT:
        case IROp::REV16: case IROp::REV32: case IROp::REV64:
        case IROp::ADDS: case IROp::SUBS: case IROp::ADCS: case IROp::SBCS:
        case IROp::TST: case IROp::TST_ZERO:
        case IROp::BRCOND_ZERO: case IROp::BRCOND_BIT:
        case IROp::CSEL: case IROp::CSINC: case IROp::CSINV: case IROp::CSNEG:
        case IROp::CCMP:
        case IROp::BFM: case IROp::UBFM: case IROp::SBFM: case IROp::EXTR:
        case IROp::BR: case IROp::BRCOND: case IROp::BRCOND_FALLTHRU:
        case IROp::BRCOND_SKIP:
        case IROp::UDIV: case IROp::SDIV:
        case IROp::SMADDL: case IROp::UMADDL: case IROp::SMULH: case IROp::UMULH:
        case IROp::SMSUBL: case IROp::UMSUBL:
        case IROp::MRS: case IROp::MSR:
            return true;
        default:
            return false;
    }
}

// Record an FP register (raw ARM reg index 0-31) as used by the block.
static inline void fp_touch(uint8_t r, int (&use)[32]) {
    if (r < 32) use[r]++;
}

bool FrostJIT::fp_cache_may_enable(const IRBlock& block) {
    static const bool no_fp_cache_ = (getenv("BIFROST_NO_FP_CACHE") != nullptr);
    if (no_fp_cache_) return false;
    if (!has_fma3()) return false;
    int fp_use[32] = {};
    bool fp_written[32] = {};
    for (const IRInst& inst : block.insts) {
        if (!fp_cache_compatible_op(inst.op)) return false;
        switch (inst.op) {
            case IROp::FP_BINOP:
            case IROp::FP_CSEL:
                fp_touch(inst.dest, fp_use); fp_touch(inst.src1, fp_use); fp_touch(inst.src2, fp_use);
                fp_written[inst.dest] = true;
                break;
            case IROp::FP_UNOP:
            case IROp::FP_MOV:
                fp_touch(inst.dest, fp_use); fp_touch(inst.src1, fp_use);
                fp_written[inst.dest] = true;
                break;
            case IROp::FP_CMP:
                fp_touch(inst.src1, fp_use);
                if (!inst.fp_cmp_params().with_zero) fp_touch(inst.src2, fp_use);  // register form only
                break;
            case IROp::FP_MOVI:
                fp_touch(inst.dest, fp_use);
                fp_written[inst.dest] = true;
                break;
            case IROp::FP_F2I:
                fp_touch(inst.src1, fp_use);
                break;
            case IROp::FP_I2F:
                fp_touch(inst.dest, fp_use);
                fp_written[inst.dest] = true;
                break;
            case IROp::FP_F2I_FIXED:
                fp_touch(inst.src1, fp_use);
                if (inst.fp_f2i_fixed_params().fp_reg) {
                    fp_touch(inst.dest, fp_use);  // fp_dest subop
                    fp_written[inst.dest] = true;
                }
                break;
            case IROp::FP_I2F_FIXED:
                fp_touch(inst.dest, fp_use);
                if (inst.fp_i2f_fixed_params().fp_reg) fp_touch(inst.src1, fp_use);  // fp_src subop
                break;
            case IROp::FCVT_S2D:
            case IROp::FCVT_D2S:
            case IROp::FRINT:
                fp_touch(inst.dest, fp_use); fp_touch(inst.src1, fp_use);
                fp_written[inst.dest] = true;
                break;
            case IROp::FMADD:
            case IROp::FMSUB:
            case IROp::FNMADD:
            case IROp::FNMSUB:
                fp_touch(inst.dest, fp_use); fp_touch(inst.src1, fp_use);
                fp_touch(inst.src2, fp_use); fp_touch(inst.fp_fused_params().acc, fp_use);  // Va (acc)
                fp_written[inst.dest] = true;
                break;
            case IROp::FMOV_G2F:
                fp_touch(inst.dest, fp_use);
                fp_written[inst.dest] = true;
                break;
            case IROp::FMOV_F2G:
                fp_touch(inst.src1, fp_use);
                break;
            case IROp::LOAD_REG:
                if (inst.is_fp_load()) fp_touch(inst.src1, fp_use);  // sf=1: load_fp_reg
                break;
            case IROp::STORE_REG:
                if (inst.is_fp_store()) fp_touch(inst.dest, fp_use);  // sf=1: store_fp_reg
                break;
            default:
                break;  // GPR-only / call — no FP regs
        }
    }
    // Partial pinning: pin the up-to-13 most-used FP regs into XMM3-15.
    // The least-used regs stay memory-authoritative (fp_load_operand /
    // fp_store_operand fall back to v_lo for unpinned operands).
    std::vector<std::pair<int, int>> cand;  // (use_count, vreg)
    for (int v = 0; v < 32; v++) {
        if (fp_use[v]) cand.emplace_back(fp_use[v], v);
    }
    if (cand.empty()) return false;
    std::sort(cand.begin(), cand.end(),
              [](const std::pair<int, int>& a, const std::pair<int, int>& b) {
                  return a.first != b.first ? a.first > b.first : a.second < b.second;
              });
    int limit = std::min<int>(static_cast<int>(cand.size()),
                              VEC_XMM_END - VEC_XMM_START + 1);
    vec_cache_active_ = true;
    fp_cache_active_ = true;
    for (int i = 0; i < 32; i++) vec_written_this_block_[i] = false;
    int xmm = VEC_XMM_START;
    for (int i = 0; i < limit; i++) {
        int v = cand[i].second;
        vec_cache_[v] = xmm;
        vec_xmm_owner_[xmm] = v;
        vec_pinned_[vec_pinned_count_++] = v;
        vec_written_this_block_[v] = fp_written[v];
        xmm++;
    }
    return true;
}

void FrostJIT::vec_cache_reset() {
    vec_cache_active_ = false;
    vec_allocator_active_ = false;
    vec_fixed_count_ = 0;
    vec_evictions_ = vec_reloads_ = vec_spills_ = 0;
    for (auto& uses : vec_uses_) uses.clear();
    fp_cache_active_ = false;
    vec_pinned_count_ = 0;
    for (int i = 0; i < 32; i++) {
        vec_cache_[i] = -1;
        vec_entry_cache_[i] = -1;
        vec_pinned_[i] = -1;
        vec_dirty_[i] = false;
        vec_valid_[i] = false;
        vec_written_this_block_[i] = false;
    }
    for (int i = 0; i < 16; i++) vec_xmm_owner_[i] = -1;
}

// ── Prologue / epilogue ────────────────────────────────────────────────
// movsd xmm, [rbx+lo] + movhpd xmm, [rbx+hi].
void FrostJIT::vec_emit_load_lo_hi(int xmm, int vreg) {
    vec_valid_[vreg] = true;
    ++vec_reloads_;
    bool r = (xmm >= 8);
    emit_rex_if_needed(r, false, false);
    emit_byte(0x0F); emit_byte(0x10);                  // movsd
    emit_modrm_disp(xmm, CPU_REG, V_LO_OFF + vreg * 8);
    emit_rex_if_needed(r, false, false);
    emit_byte(0x0F); emit_byte(0x16);                  // movhpd
    emit_modrm_disp(xmm, CPU_REG, V_HI_OFF + vreg * 8);
}

void FrostJIT::vec_emit_prologue_loads() {
    for (int i = 0; i < vec_pinned_count_; i++) {
        int v = vec_pinned_[i];
        if (fp_cache_active_) {
            // Scalar-FP cache: lo-only. v_hi is never cached and never
            // loaded here (scalar FP ops don't read it; vector ops can't
            // appear in an fp-cache block). MOVSD (F2 0F 10) is 64-bit —
            // a bare 0F 10 (MOVUPS) would load 128 bits and read the NEXT
            // register's lo slot into the XMM's upper half.
            int xmm = vec_cache_[v];
            bool r = (xmm >= 8);
            emit_byte(0xF2);
            emit_rex_if_needed(r, false, false);
            emit_byte(0x0F); emit_byte(0x10);          // movsd
            emit_modrm_disp(xmm, CPU_REG, V_LO_OFF + v * 8);
            ++vec_reloads_;
        } else {
            vec_emit_load_lo_hi(vec_cache_[v], v);
        }
        // Demand entry values can carry a write from the previous loop
        // trip even before their first definition on this trip. An early
        // eviction must publish them before losing their only current copy.
        vec_dirty_[v] = vec_allocator_active_ && i >= vec_fixed_count_ &&
                        vec_written_this_block_[v];
        vec_valid_[v] = true;
    }
}

void FrostJIT::vec_cache_writeback_all(bool clear_flags) {
    if (!vec_cache_active_) return;
    for (int i = 0; i < vec_pinned_count_; i++) {
        int v = vec_pinned_[i];
        if (!vec_dirty_[v]) continue;
        int xmm = vec_cache_[v];
        bool r = (xmm >= 8);
        // MOVSD (F2 0F 11) — 64-bit store. A bare 0F 11 (MOVUPS) would
        // write the XMM's upper half into the NEXT register's lo slot,
        // silently corrupting v_lo[v+1] every time v is written back.
        emit_byte(0xF2);
        emit_rex_if_needed(r, false, false);
        emit_byte(0x0F); emit_byte(0x11);              // movsd
        emit_modrm_disp(xmm, CPU_REG, V_LO_OFF + v * 8);
        if (!fp_cache_active_) {
            emit_rex_if_needed(r, false, false);
            emit_byte(0x0F); emit_byte(0x17);          // movhpd
            emit_modrm_disp(xmm, CPU_REG, V_HI_OFF + v * 8);
        }
        ++vec_spills_;
        if (clear_flags) vec_dirty_[v] = false;
    }
}

void FrostJIT::vec_cache_writeback_all_pinned(bool clear_flags) {
    if (!vec_cache_active_) return;
    for (int i = 0; i < vec_pinned_count_; i++) {
        int v = vec_pinned_[i];
        // Flush a pinned reg iff it is statically dirty AT this point OR
        // written anywhere in the block. The second term covers the
        // loop-carried accumulator: written after this call in the IR, so
        // clean at this codegen point, but dirty at runtime on the next
        // self-loop iteration. It is always safe to flush a clean reg
        // (its XMM value equals its slot).
        if (!vec_valid_[v] || (!vec_dirty_[v] && !vec_written_this_block_[v])) continue;
        int xmm = vec_cache_[v];
        bool r = (xmm >= 8);
        // MOVSD (F2 0F 11) — 64-bit store (see vec_cache_writeback_all).
        emit_byte(0xF2);
        emit_rex_if_needed(r, false, false);
        emit_byte(0x0F); emit_byte(0x11);              // movsd
        emit_modrm_disp(xmm, CPU_REG, V_LO_OFF + v * 8);
        if (!fp_cache_active_) {
            emit_rex_if_needed(r, false, false);
            emit_byte(0x0F); emit_byte(0x17);          // movhpd
            emit_modrm_disp(xmm, CPU_REG, V_HI_OFF + v * 8);
        }
        ++vec_spills_;
        if (clear_flags) vec_dirty_[v] = false;
    }
}

void FrostJIT::vec_cache_mark_dirty(int vreg) {
    if (vreg >= 0 && vreg < 32) vec_dirty_[vreg] = vec_valid_[vreg] = true;
}

// ── VEX 3-byte helpers ────────────────────────────────────────────────
// VEX.NDS.128.XX.0F{38}: C4 [R~ X~ B~ mmmmm] [W vvvv~ L pp] opcode modrm.
// `reg_field`/`rm_field` are the (possibly ≥8) x86 register numbers used in
// the ModRM reg/rm fields; R~/B~ extend them exactly like REX.R/REX.B.
// L=0 (128-bit) always; pp: 0=no prefix, 1=66, 2=F3, 3=F2.
void FrostJIT::emit_vex3(int map, bool w, int vvvv, int pp, int reg_field,
                         int rm_field, bool rm_is_reg, uint8_t opcode) {
    uint8_t b1 = 0;
    b1 |= (reg_field < 8) ? 0x80 : 0x00;  // VEX.R
    b1 |= 0x40;                            // VEX.X = 1
    b1 |= (!rm_is_reg || rm_field < 8) ? 0x20 : 0x00;  // VEX.B
    b1 |= static_cast<uint8_t>(map & 0x1F);
    emit_byte(0xC4); emit_byte(b1);
    // byte2 = [W] [vvvv~] [L] [pp] — L=0 (128-bit), pp in bits 1-0.
    emit_byte(static_cast<uint8_t>((w ? 0x80 : 0) |
                                   (((~vvvv) & 0xF) << 3) |
                                   (pp & 3)));
    emit_byte(opcode);
    emit_byte(static_cast<uint8_t>((rm_is_reg ? 0xC0 : 0) |
                                   ((reg_field & 7) << 3) | (rm_field & 7)));
}

// vfmadd231ps/pd xmmD, xmmS1, xmmS2   (dest = dest + src1*src2)
// vfnmadd231ps/pd xmmD, xmmS1, xmmS2  (dest = dest - src1*src2)
void FrostJIT::emit_vex_fma(int dest, int src1, int src2,
                            bool is_double, bool is_sub) {
    // VEX.128.66.0F38.W[01]: map=2, pp=1 (66), L=0, vvvv=src1.
    emit_vex3(2, is_double, src1, 1, dest, src2, true, is_sub ? 0xBC : 0xB8);
}

// VEX packed FP binop (0F map): vaddps/vsubps/vmulps/vdivps/vminps/vmaxps
// (single, pp=0) or vaddpd/... (double, pp=1). dest = src1 OP src2, with
// VEX NDS semantics so dest may alias src1 or src2.
void FrostJIT::emit_vex_fp_binop(int dest, int src1, int src2,
                                 uint8_t op_byte, bool is_double) {
    emit_vex3(1, false, src1, is_double ? 1 : 0, dest, src2, true, op_byte);
}

// XMM0 is scratch in cached blocks. Build the sign/absolute-value mask in
// every lane, without borrowing a GPR or loading a constant from memory.
void FrostJIT::emit_vec_sign_mask(int esize, bool clear_sign) {
    emit_vex3(1, false, 0, 1, 0, 0, true, 0x76); // vpcmpeqd xmm0,xmm0,xmm0
    emit_byte(0x66); emit_byte(0x0F); emit_byte(esize == 8 ? 0x73 : 0x72);
    emit_byte(clear_sign ? 0xD0 : 0xF0); // psrl[d/q] /2 or psll[d/q] /6
    emit_byte(clear_sign ? 1 : esize * 8 - 1);
}

void FrostJIT::emit_vec_zero_upper(int xmm) {
    // VEX.128.F3.0F 7E: VMOVQ zeroes bits 127:64; vvvv is reserved.
    emit_vex3(1, false, 0, 2, xmm, xmm, true, 0x7E);
}

// ── Scalar-FP-cache helpers ────────────────────────────────────────────
// Load FP operand `vreg` (raw ARM FP reg 0-31) into XMM `xmm`. When the fp
// cache is active and the operand is pinned, this is a reg-reg move (no
// memory access); otherwise it loads cpu.v_lo[vreg] exactly like the
// non-cached codegen. `is_double` selects MOVSD (F2) vs MOVSS (F3).
void FrostJIT::fp_load_operand(int xmm, int vreg, bool is_double) {
    uint8_t prefix = is_double ? 0xF2 : 0xF3;
    int xs = vec_xmm(vreg);
    if (xs >= 0) {
        // movsd/movss xmm, xmm_s  (reg-reg; REX for XMM8-15 source/dest)
        emit_byte(prefix);
        emit_rex_if_needed(xmm >= 8, false, xs >= 8);
        emit_byte(0x0F); emit_byte(0x10);
        emit_byte(modrm(3, xmm & 7, xs & 7));
    } else {
        emit_byte(prefix);
        emit_byte(0x0F); emit_byte(0x10);
        emit_modrm_disp(xmm, CPU_REG, V_LO_OFF + vreg * 8);
    }
}

// A pinned XMM in the vector cache holds all 128 bits; the low-half FP cache
// pins only v_lo (its XMM bits 63:0), and v_hi stays current in memory, since
// that cache admits no op that writes v_hi through a register.
void FrostJIT::fp_load_lane(int xmm, int vreg, int lane, bool is_double) {
    const int bytes = lane * (is_double ? 8 : 4);
    if (bytes == 0) {
        fp_load_operand(xmm, vreg, is_double);
        return;
    }
    const int xs = vec_xmm(vreg);
    if (xs >= 0 && (bytes < 8 || !fp_cache_active_)) {
        // pshufd xmm, xs, imm: the element to lane 0 (the others are unused).
        emit_byte(0x66);
        emit_rex_if_needed(xmm >= 8, false, xs >= 8);
        emit_byte(0x0F); emit_byte(0x70);
        emit_byte(modrm(3, xmm & 7, xs & 7));
        emit_byte(is_double ? 0xEE : static_cast<uint8_t>(lane * 0x55));
        return;
    }
    emit_byte(is_double ? 0xF2 : 0xF3);  // movsd/movss xmm, [element]
    emit_rex_if_needed(xmm >= 8, false, false);
    emit_byte(0x0F); emit_byte(0x10);
    emit_modrm_disp(xmm, CPU_REG, bytes < 8 ? V_LO_OFF + vreg * 8 + bytes
                                            : V_HI_OFF + vreg * 8 + bytes - 8);
}

// Resolve FP operand `vreg` to the XMM holding its value: the pinned XMM
// if cached (no code), else `scratch` after loading v_lo[vreg] into it.
int FrostJIT::fp_resolve_operand(int vreg, int scratch, bool is_double) {
    int xs = vec_xmm(vreg);
    if (xs >= 0) return xs;
    fp_load_operand(scratch, vreg, is_double);
    return scratch;
}

// Store XMM `xmm` into FP operand `vreg`. When pinned, this is a reg-reg
// move into the pinned XMM (marked dirty); otherwise it stores to
// cpu.v_lo[vreg] like the non-cached codegen. The caller is responsible
// for zeroing/copying v_hi via fp_zero_hi / the FP_MOV double path.
void FrostJIT::fp_store_operand(int xmm, int vreg, bool is_double) {
    uint8_t prefix = is_double ? 0xF2 : 0xF3;
    int xd = vec_xmm(vreg);
    if (xd >= 0) {
        // The pinned XMM gets the result with every bit above it zero, as an
        // ARM scalar write leaves the register (interp write_fp_s/write_fp_d):
        // single `xorps xd,xd; movss xd,xmm`, double `movq xd,xmm`. The vector
        // cache keeps v_hi in the XMM, so fp_zero_hi then has nothing to do.
        // Callers pass scratch XMM0-2 and pins are XMM3-15, so xmm != xd.
        if (!is_double) {
            emit_rex_if_needed(xd >= 8, false, xd >= 8);
            emit_byte(0x0F); emit_byte(0x57);
            emit_byte(modrm(3, xd & 7, xd & 7));                     // xorps xd,xd
            emit_byte(0xF3);
            emit_rex_if_needed(xd >= 8, false, xmm >= 8);
            emit_byte(0x0F); emit_byte(0x10);
            emit_byte(modrm(3, xd & 7, xmm & 7));                    // movss xd,xmm
        } else {
            emit_byte(0xF3);
            emit_rex_if_needed(xd >= 8, false, xmm >= 8);
            emit_byte(0x0F); emit_byte(0x7E);
            emit_byte(modrm(3, xd & 7, xmm & 7));                    // movq xd,xmm
        }
        if (!fp_cache_active_) fp_hi_zeroed_vreg_ = vreg;
        vec_cache_mark_dirty(vreg);
    } else {
        emit_byte(prefix);
        emit_byte(0x0F); emit_byte(0x11);
        emit_modrm_disp(xmm, CPU_REG, V_LO_OFF + vreg * 8);
        if (!is_double) {
            // Same canonicalization for the memory path: movss m32 leaves
            // the old high dword in place — zero it explicitly.
            emit_byte(0xC7);
            emit_modrm_disp(0, CPU_REG, V_LO_OFF + vreg * 8 + 4);
            emit_u32(0);
        }
    }
}

// Zero v_hi[vreg]. A vector-cache XMM holds v_hi in bits 127:64, already
// zero when this op's fp_store_operand wrote it; otherwise (the lo-only
// fp cache, or an unpinned register) it is `pxor xmm0, xmm0; movsd [hi],
// xmm0`. Keep that pair: `mov qword [hi], 0` is one instruction fewer but
// made two scalar-FP kernels (cull, collide) about 30% slower in cycles
// (2026-11-10, cause not established). MUST run after the lo result is stored.
void FrostJIT::fp_zero_hi(int vreg) {
    const int pinned = vec_xmm(vreg);
    if (pinned >= 0 && !fp_cache_active_) {
        if (fp_hi_zeroed_vreg_ != vreg) emit_vec_zero_upper(pinned);
        fp_hi_zeroed_vreg_ = -1;
        vec_cache_mark_dirty(vreg);
        return;
    }
    emit_byte(0x66); emit_byte(0x0F); emit_byte(0xEF); emit_byte(0xC0);  // pxor xmm0, xmm0
    emit_byte(0xF2); emit_byte(0x0F); emit_byte(0x11);                    // movsd [hi], xmm0
    emit_modrm_disp(0, CPU_REG, V_HI_OFF + vreg * 8);
}

// movq xmm, gpr:  66 REX.W 0F 6E /r  (reg = xmm, rm = gpr)
void FrostJIT::emit_vmovq_gpr_to_xmm(int xmm, int gpr) {
    emit_byte(0x66);
    emit_byte(rex(true, xmm >= 8, false, gpr >= 8));
    emit_byte(0x0F); emit_byte(0x6E);
    emit_byte(modrm(3, xmm & 7, gpr & 7));
}

// movq gpr, xmm:  66 REX.W 0F 7E /r  (reg = xmm source, rm = gpr dest)
void FrostJIT::emit_vmovq_xmm_to_gpr(int gpr, int xmm) {
    emit_byte(0x66);
    emit_byte(rex(true, xmm >= 8, false, gpr >= 8));
    emit_byte(0x0F); emit_byte(0x7E);
    emit_byte(modrm(3, xmm & 7, gpr & 7));
}

} // namespace arm64emu
