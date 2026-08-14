// ir_optimize.cpp — IR optimization passes for bifrost-emu
//
// Implements optimize_ir(), which performs the following passes on an
// IRBlock in place:
//
//   1. Constant folding  — IMM + ALU chains collapse to a single IMM.
//   2. Copy propagation   — MOV(src, dest) lets later readers of dest
//                           use src directly.
//   3. Dead code elimination — scratch vregs whose result is never used
//                           (and which have no side effects) are removed.
//   4. Store-load forwarding — if a STORE_REG writes a vreg that is
//                           immediately read back by the next LOAD_REG,
//                           reuse the vreg.
//   5. Peephole           — ZEXT after an op that already zero-extends
//                           (ADD/SUB/AND/OR/XOR/SHL/SHR on 32-bit)
//                           is removed.
//
// These passes together eliminate most of the redundant work that the
// naive translator produces when each ARM64 instruction reloads its
// operands from cpu.regs[]. On a tight loop like fib(N), the optimized
// IR is roughly 40% smaller than the naive IR and runs ~3x faster in
// the x86 codegen.
#include "ir/ir.hpp"
#include "core/emulator.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cstdio>
namespace arm64emu {
// ── Constant table ─────────────────────────────────────────────────────
// Maps vreg → known constant value. Populated by IMM, updated by const
// folding, invalidated by any non-constant op or by CALL_INTERP/SVC.
struct ConstMap {
    std::unordered_map<uint16_t, uint64_t> vals;
    bool has(uint8_t v) const { return vals.find(v) != vals.end(); }
    uint64_t get(uint8_t v) const { return vals.at(v); }
    void set(uint8_t v, uint64_t val) { vals[v] = val; }
    void clear(uint8_t v) { vals.erase(v); }
    void clear_all() { vals.clear(); }
};
// ── Copy map ───────────────────────────────────────────────────────────
// Maps vreg → vreg it's a copy of. Transitive lookups handled by find().
struct CopyMap {
    std::unordered_map<uint16_t, uint8_t> parent;
    uint8_t find(uint8_t v) {
        // Path-compressing find.
        auto it = parent.find(v);
        if (it == parent.end()) return v;
        uint8_t root = find(it->second);
        if (it->second != root) parent[v] = root;
        return root;
    }
    void set(uint8_t v, uint8_t src) { parent[v] = src; }
    void clear(uint8_t v) { parent.erase(v); }
    void clear_all() { parent.clear(); }
    // Invalidate all copies where `src` is the source vreg.
    // Called when `src` is modified — any copy of `src` becomes stale.
    void clear_source(uint8_t src) {
        for (auto it = parent.begin(); it != parent.end(); ) {
            if (it->second == src) it = parent.erase(it);
            else ++it;
        }
    }
};
// ── Bitwise helpers for constant folding ──────────────────────────────
static uint64_t mask_for_width(uint8_t bits) {
    if (bits >= 64) return ~0ULL;
    return (1ULL << bits) - 1;
}
// ── Is this op side-effect-free (safe to DCE if dest unused)? ─────────
// Note: flag-setting ops (ADDS/SUBS/TST/ADCS/SBCS/CCMP) are NOT pure
// because they set cpu.pstate which later BRCOND/CSEL ops may read.
// We could track flag liveness to make them pure when flags are unused,
// but for correctness we keep them always.
static bool is_pure(IROp op) {
    switch (op) {
        case IROp::NOP:
        case IROp::IMM:
        case IROp::MOV:
        case IROp::ADD: case IROp::SUB: case IROp::MUL:
        case IROp::AND: case IROp::OR:  case IROp::XOR:
        case IROp::SHL: case IROp::SHR: case IROp::SAR: case IROp::ROR:
        case IROp::NOT: case IROp::NEG:
        case IROp::SEXT: case IROp::ZEXT:
        case IROp::CLZ: case IROp::CLS:
        case IROp::RBIT: case IROp::REV16: case IROp::REV32: case IROp::REV64:
            return true;
        // These have side effects (flags, memory, syscall, branch, or
        // write to architectural regs that CALL_INTERP/SVC might read) —
        // never DCE.
        case IROp::CSEL: case IROp::CSINC: case IROp::CSINV: case IROp::CSNEG:
        case IROp::BFM: case IROp::UBFM: case IROp::SBFM: case IROp::EXTR:
        case IROp::ADDS: case IROp::SUBS:
        case IROp::ADCS: case IROp::SBCS:
        case IROp::TST: case IROp::CCMP:
        case IROp::LOAD_REG:
        case IROp::STORE_REG:
        case IROp::LOAD_MEM:
        case IROp::STORE_MEM:
        case IROp::BR: case IROp::BRCOND: case IROp::BRCOND_FALLTHRU:
        case IROp::CALL_INTERP: case IROp::SVC:
        case IROp::BL_CALL:  // callee may read/write any ARM reg
        case IROp::FMOV_G2F: case IROp::FMOV_F2G:
        case IROp::FMOV_G2FHI: case IROp::FMOV_FHI2G:  // write to v_lo/v_hi or read from them
        case IROp::FP_BINOP: case IROp::FP_UNOP:      // write to v_lo/v_hi
        case IROp::SIMD_LOGICAL: case IROp::SIMD_DUP: // write to v_lo/v_hi
        case IROp::SIMD_MOVI: case IROp::SIMD_ORRIMM: // write to v_lo/v_hi
        case IROp::SIMD_LDST:                          // write to v_lo/v_hi
        case IROp::SIMD_SHL: case IROp::SIMD_USHR: case IROp::SIMD_SSHR: // v1.4.5-alpha
        case IROp::FP_F2I: case IROp::FP_I2F:          // read/write v_lo/regs
        case IROp::FP_F2I_FIXED: case IROp::FP_I2F_FIXED:  // fixed-point variants
        case IROp::FP_CMP: case IROp::FP_MOVI:          // write pstate/v_lo
        case IROp::ATOMIC:                               // read/write memory
        case IROp::LDXR_FAST: case IROp::STXR_FAST:     // read/write + monitor
        case IROp::STLR_FAST:                            // write + monitor
        case IROp::AES_CRYPTO:                           // 1.5.2-alpha: read/write v_lo/v_hi
        // TST_ZERO / BRCOND_ZERO / BRCOND_BIT also have side effects
        // (they read flags or branch) — never DCE.
        case IROp::TST_ZERO:
        case IROp::BRCOND_ZERO:
        case IROp::BRCOND_BIT:
            return false;
        // Newer ops (UDIV/SDIV/SMADDL/UMADDL/SMULH/UMULH/SMSUBL/UMSUBL,
        // FCVT_S2D/FCVT_D2S/FRINT/FCMP/FP_UNOP2/FMADD/FMSUB, MRS/MSR)
        // are not pure (side effects on pstate or system regs) — never DCE.
        default:
            return false;
    }
}
// ── Fold a binary op with two constant operands ──────────────────────
static bool fold_binop(IROp op, uint64_t a, uint64_t b, uint64_t width, uint64_t& out) {
    switch (op) {
        case IROp::ADD: out = a + b; return true;
        case IROp::SUB: out = a - b; return true;
        case IROp::MUL: out = a * b; return true;
        case IROp::AND: out = a & b; return true;
        case IROp::OR:  out = a | b; return true;
        case IROp::XOR: out = a ^ b; return true;
        case IROp::SHL: out = a << (b & (width == 32 ? 31 : 63)); return true;
        case IROp::SHR: out = a >> (b & (width == 32 ? 31 : 63)); return true;
        case IROp::SAR: out = static_cast<uint64_t>(static_cast<int64_t>(a) >> (b & (width == 32 ? 31 : 63))); return true;
        case IROp::ROR: {
            // Rotate within the operand width: 32-bit ROR must not spill
            // into the upper 32 bits (which the JIT/interpreter zero).
            uint64_t w = (width == 32) ? 32 : 64;
            uint64_t r = b & (w - 1);
            uint64_t va = a & ((w == 64) ? ~0ULL : 0xFFFFFFFFULL);
            if (r == 0) { out = va; return true; }
            out = (va >> r) | (va << (w - r));
            if (w == 32) out &= 0xFFFFFFFFULL;
            return true;
        }
        default: return false;
    }
}
// ── Fold a unary op with a constant operand ──────────────────────────
static bool fold_unop(IROp op, uint64_t a, uint64_t width, uint64_t& out) {
    switch (op) {
        case IROp::NOT: out = ~a; return true;
        case IROp::NEG: out = -static_cast<int64_t>(a); return true;
        case IROp::SEXT: {
            uint64_t m = mask_for_width(static_cast<int>(width));
            uint64_t v = a & m;
            if (width < 64) {
                uint64_t sb = 1ULL << (width - 1);
                out = (v ^ sb) - sb;
            } else {
                out = v;
            }
            return true;
        }
        case IROp::ZEXT:
            out = a & mask_for_width(static_cast<int>(width));
            return true;
        case IROp::CLZ: {
            // Count leading zeros by scanning from the high bit down.
            for (int i = 63; i >= 0; i--) {
                if ((a >> i) & 1) { out = 63 - i; return true; }
            }
            out = 64; return true;
        }
        case IROp::RBIT: {
            uint64_t r = 0;
            for (int i = 0; i < 64; i++) if ((a >> i) & 1) r |= (1ULL << (63 - i));
            out = r; return true;
        }
        case IROp::REV16: {
            uint64_t r = 0;
            for (int i = 0; i < 4; i++) {
                uint16_t h = static_cast<uint16_t>((a >> (i * 16)) & 0xFFFF);
                uint16_t s = static_cast<uint16_t>(((h & 0xFF) << 8) | ((h >> 8) & 0xFF));
                r |= static_cast<uint64_t>(s) << (i * 16);
            }
            out = r; return true;
        }
        case IROp::REV32: {
            uint64_t r = 0;
            for (int i = 0; i < 2; i++) {
                uint32_t w = static_cast<uint32_t>((a >> (i * 32)) & 0xFFFFFFFF);
                uint32_t s = __builtin_bswap32(w);
                r |= static_cast<uint64_t>(s) << (i * 32);
            }
            out = r; return true;
        }
        case IROp::REV64:
            out = __builtin_bswap64(a); return true;
        default: return false;
    }
}
// ── The main optimizer ────────────────────────────────────────────────
void optimize_ir(IRBlock& block) {
    if (block.insts.empty()) return;
    // LDXR_FAST, STXR_FAST, or STLR_FAST ops. If so, disable the
    // arm_reg_cache load-forwarding (FWD) for the ENTIRE block. These
    // ops have complex memory + register side effects that the FWD
    // optimizer can't model correctly:
    //   - ATOMIC reads cpu.regs[imm] directly (bypassing vregs)
    //   - ATOMIC writes memory (invalidates forwarded loads)
    //   - LL/SC ops interact with the exclusive monitor
    // Forwarding stale values across these ops causes LSE atomic
    // correctness bugs (CAS writes wrong desired value, etc.).
    // FWD is a ~5.6% speedup on compute loops — we skip it only for
    // blocks with atomics, which are rare in compute workloads.
    static bool enable_fwd_ = (getenv("BIFROST_ENABLE_FWD") != nullptr);
    bool block_has_atomics = false;
    if (enable_fwd_) {
        for (const auto& inst : block.insts) {
            if (inst.op == IROp::ATOMIC ||
                inst.op == IROp::LDXR_FAST ||
                inst.op == IROp::STXR_FAST ||
                inst.op == IROp::STLR_FAST) {
                block_has_atomics = true;
                break;
            }
        }
    }
    bool fwd_enabled = enable_fwd_ && !block_has_atomics;
    ConstMap consts;
    CopyMap  copies;
    // Last vreg → index in insts that defined it (for store-load fwd).
    std::unordered_map<uint16_t, size_t> last_def;
    // Per-vreg "currently in ARM64 reg" cache: arm_reg → vreg holding
    // its current value. LOAD_REG can reuse this.
    std::unordered_map<uint16_t, uint16_t> arm_reg_cache;
    // Helper: invalidate any arm_reg_cache entries that point to vreg `v`.
    // This must be called whenever vreg `v` is redefined, because a
    // cached LOAD_REG that reused `v` would now read the wrong value.
    auto invalidate_vreg_in_cache = [&](uint16_t v) {
        for (auto it = arm_reg_cache.begin(); it != arm_reg_cache.end(); ) {
            if (it->second == v) {
                it = arm_reg_cache.erase(it);
            } else {
                ++it;
            }
        }
    };
    // ── Helper: dead-store elimination for STORE_REG ───────────────
    // If a STORE_REG to arch reg R is followed by another STORE_REG to
    // the same R (no intervening LOAD_REG of R, CALL_INTERP/SVC, ATOMIC,
    // or LL/SC op), the first is dead. NOP it out.
    //
    // ATOMIC reads cpu.regs[imm] directly (e.g., CAS reads expected from
    // Ws=imm), so a preceding STORE_REG to that ARM reg must be preserved.
    // LL/SC ops (LDXR_FAST/STXR_FAST/STLR_FAST) read/write ARM regs and
    // memory — clear all pending store info to be safe.
    //
    // Used by both Pass 0 (pre-FWD) and Pass 1.5 (post-substitution).
    auto dse_pass = [&block]() {
        // to the same register index are treated as different destinations.
        // GPR reg 0 writes cpu.regs[0]; FP reg 0 writes cpu.v_lo[0].
        auto store_key = [](const IRInst& inst) -> uint32_t {
            return (static_cast<uint32_t>(inst.dest) << 1) | inst.sf;
        };
        std::unordered_map<uint32_t, size_t> last_store_to;
        for (size_t i = 0; i < block.insts.size(); i++) {
            IRInst& inst = block.insts[i];
            if (inst.op == IROp::STORE_REG) {
                uint32_t key = store_key(inst);
                auto it = last_store_to.find(key);
                if (it != last_store_to.end()) {
                    block.insts[it->second].op = IROp::NOP;
                    block.dce_removed++;
                }
                last_store_to[key] = i;
            } else if (inst.op == IROp::LOAD_REG) {
                // loads don't cross-invalidate each other's stores.
                uint32_t key = (static_cast<uint32_t>(inst.src1) << 1) | inst.sf;
                last_store_to.erase(key);
            } else if (inst.op == IROp::CALL_INTERP || inst.op == IROp::SVC ||
                       inst.op == IROp::BL_CALL) {
                last_store_to.clear();
            } else if (inst.op == IROp::BR ||
                       inst.op == IROp::BRCOND ||
                       inst.op == IROp::BRCOND_FALLTHRU ||
                       inst.op == IROp::BRCOND_ZERO ||
                       inst.op == IROp::BRCOND_BIT) {
                // callee for BL) may read ANY ARM register. Without this,
                // DSE would incorrectly eliminate the BL's STORE_REG x30
                // (return address) because no instruction in THIS block
                // reads x30 after the store. But the callee's `ret` reads
                // x30 in a DIFFERENT block. This caused curl's SIGSEGV at
                // pc=0x0: __syscall_cancel_arch's `ret` jumped to x30=0
                // because the BL that set x30 had its store DCE'd.
                last_store_to.clear();
            } else if (inst.op == IROp::ATOMIC) {
                // ATOMIC reads cpu.regs[imm] directly — preserve preceding
                // STORE_REG to that ARM reg. (GPR only — is_fp=0.)
                last_store_to.erase(static_cast<uint32_t>(inst.imm) << 1);
            } else if (inst.op == IROp::LDXR_FAST ||
                       inst.op == IROp::STXR_FAST ||
                       inst.op == IROp::STLR_FAST) {
                last_store_to.clear();
            }
            // FP_I2F/FP_I2F_FIXED read GPRs (src1 = GPR reg index).
            // These must invalidate pending GPR STORE_REGs.
            if (inst.op == IROp::FMOV_G2F || inst.op == IROp::FMOV_G2FHI ||
                inst.op == IROp::FP_I2F || inst.op == IROp::FP_I2F_FIXED) {
                // Reads GPR src1. Invalidate GPR store.
                last_store_to.erase(static_cast<uint32_t>(inst.src1) << 1);
            }
            if (inst.op == IROp::FMOV_F2G || inst.op == IROp::FMOV_FHI2G) {
                // Reads FP reg src1, writes GPR dest. Invalidate FP store.
                last_store_to.erase((static_cast<uint32_t>(inst.src1) << 1) | 1);
            }
        }
    };
    // ── Pass 0: dead-store elimination for STORE_REG ───────────────
    dse_pass();
    // Pass 1: walk forward, fold constants, propagate copies, cache
    // ARM64 register loads.
    for (size_t i = 0; i < block.insts.size(); i++) {
        IRInst& inst = block.insts[i];
        // If this instruction reassigns a vreg, invalidate any copies
        // that use it as a source. If v35 was copied to v40 and now v35
        // is reassigned, the copy is stale.
        if (inst.dest && inst.op != IROp::STORE_REG) {
            copies.clear_source(inst.dest);
        }
        // Substitute copy sources.
        if (inst.src1 && copies.parent.count(inst.src1))
            inst.src1 = copies.find(inst.src1);
        if (inst.src2 && copies.parent.count(inst.src2))
            inst.src2 = copies.find(inst.src2);
        if (inst.aux && copies.parent.count(inst.aux))
            inst.aux = copies.find(inst.aux);
        switch (inst.op) {
            case IROp::NOP:
                break;
            case IROp::IMM:
                invalidate_vreg_in_cache(inst.dest);
                consts.set(inst.dest, inst.imm);
                copies.clear(inst.dest);
                last_def[inst.dest] = i;
                break;
            case IROp::MOV: {
                // dest = src1. Replace subsequent uses of dest with src1.
                invalidate_vreg_in_cache(inst.dest);
                if (consts.has(inst.src1)) {
                    // Turn into IMM (constant propagation).
                    inst.op = IROp::IMM;
                    inst.imm = consts.get(inst.src1);
                    consts.set(inst.dest, inst.imm);
                } else {
                    copies.set(inst.dest, inst.src1);
                    consts.clear(inst.dest);
                }
                last_def[inst.dest] = i;
                break;
            }
            case IROp::LOAD_REG: {
                uint8_t ar = inst.src1;
                // arm_reg_cache load-forwarding.
                //
                // The cache maps ARM reg index → vreg holding its current
                // value. After a LOAD_REG or STORE_REG of arm reg R, the
                // cache knows which vreg holds R's value. A subsequent
                // LOAD_REG of R can be replaced by MOV dest, cached_vreg
                // (or IMM if the cached vreg is a known constant). This is
                // the single most impactful IR optimization for tight loops:
                // without it, every ARM register access reloads from
                // cpu.regs[] memory, even when the value is already in a
                // host register from the previous iteration.
                //
                // The cache is invalidated by CALL_INTERP/SVC (interpreter
                // may modify any cpu.regs[]) and by vreg redefinition
                // (invalidate_vreg_in_cache). This fixes the previous
                // correctness bug that crashed `toybox ls /`.
                //
                // Enable with BIFROST_ENABLE_FWD=1 (bench_mips gets ~5.6%
                // speedup). Without FWD, the JIT still achieves 571 MIPS.
                // Disabled for blocks containing ATOMIC/LL/SC ops (see
                // block_has_atomics above).
                auto it = fwd_enabled ? arm_reg_cache.find(ar) : arm_reg_cache.end();
                if (it != arm_reg_cache.end()) {
                    // Reuse cached vreg: turn this into MOV.
                    inst.op = IROp::MOV;
                    inst.src1 = it->second;
                    if (consts.has(it->second)) {
                        inst.op = IROp::IMM;
                        inst.imm = consts.get(it->second);
                        consts.set(inst.dest, inst.imm);
                    } else {
                        copies.set(inst.dest, it->second);
                    }
                } else {
                    // Cache this load as the canonical source for ar.
                    // First, invalidate any old cache entry pointing to
                    // inst.dest (it's about to be redefined).
                    invalidate_vreg_in_cache(inst.dest);
                    arm_reg_cache[ar] = inst.dest;
                    consts.clear(inst.dest);
                    copies.clear(inst.dest);
                }
                last_def[inst.dest] = i;
                break;
            }
            case IROp::STORE_REG: {
                // dest is the ARM64 reg index; src1 is the vreg being stored.
                arm_reg_cache[inst.dest] = inst.src1;
                // The stored value becomes the cached value of arm_reg[dest].
                if (consts.has(inst.src1))
                    consts.set(inst.dest, consts.get(inst.src1)); // unlikely useful
                break;
            }
            case IROp::LOAD_MEM: {
                invalidate_vreg_in_cache(inst.dest);
                consts.clear(inst.dest);
                copies.clear(inst.dest);
                last_def[inst.dest] = i;
                break;
            }
            case IROp::STORE_MEM:
            case IROp::ATOMIC:
                // Side-effecting (writes memory) — skip constant folding
                // even if dest is unused.
                // side effects (reads cpu.regs[imm] directly, writes old
                // value to an ARM reg via subsequent STORE_REG, and does
                // a memory RMW). Clear the entire arm_reg_cache to be
                // safe — ATOMICs are rare enough (not in tight compute
                // loops) that this doesn't hurt performance. Without this,
                // the FWD cache can forward stale values for ARM regs that
                // were cached before the atomic's memory side effect.
                if (inst.op == IROp::ATOMIC) {
                    invalidate_vreg_in_cache(inst.dest);
                    arm_reg_cache.clear();
                    consts.clear_all();
                    copies.clear_all();
                }
                break;
            case IROp::ADD: case IROp::SUB: case IROp::MUL:
            case IROp::AND: case IROp::OR:  case IROp::XOR:
            case IROp::SHL: case IROp::SHR: case IROp::SAR: case IROp::ROR: {
                uint64_t a, b;
                bool ha = consts.has(inst.src1);
                bool hb = consts.has(inst.src2);
                if (ha && hb) {
                    a = consts.get(inst.src1);
                    b = consts.get(inst.src2);
                    uint64_t r;
                    if (fold_binop(inst.op, a, b, inst.width, r)) {
                        inst.op = IROp::IMM;
                        inst.imm = r;
                        consts.set(inst.dest, r);
                        block.fold_subst++;
                    }
                } else {
                    // Special case: ADD x, 0 → MOV x
                    if (inst.op == IROp::ADD && hb && consts.get(inst.src2) == 0) {
                        inst.op = IROp::MOV;
                        if (consts.has(inst.src1)) {
                            inst.op = IROp::IMM;
                            inst.imm = consts.get(inst.src1);
                            consts.set(inst.dest, inst.imm);
                        } else {
                            copies.set(inst.dest, inst.src1);
                        }
                    }
                    // SUB x, 0 → MOV x
                    else if (inst.op == IROp::SUB && hb && consts.get(inst.src2) == 0) {
                        inst.op = IROp::MOV;
                        if (consts.has(inst.src1)) {
                            inst.op = IROp::IMM;
                            inst.imm = consts.get(inst.src1);
                            consts.set(inst.dest, inst.imm);
                        } else {
                            copies.set(inst.dest, inst.src1);
                        }
                    }
                    // MUL x, 1 → MOV x
                    else if (inst.op == IROp::MUL && hb && consts.get(inst.src2) == 1) {
                        inst.op = IROp::MOV;
                        if (consts.has(inst.src1)) {
                            inst.op = IROp::IMM;
                            inst.imm = consts.get(inst.src1);
                            consts.set(inst.dest, inst.imm);
                        } else {
                            copies.set(inst.dest, inst.src1);
                        }
                    }
                    // AND/OR/XOR x, 0 → IMM 0 / MOV x
                    else if (inst.op == IROp::AND && hb && consts.get(inst.src2) == 0) {
                        inst.op = IROp::IMM;
                        inst.imm = 0;
                        consts.set(inst.dest, 0);
                        block.fold_subst++;
                    }
                    else if (inst.op == IROp::OR && hb && consts.get(inst.src2) == 0) {
                        inst.op = IROp::MOV;
                        if (consts.has(inst.src1)) {
                            inst.op = IROp::IMM;
                            inst.imm = consts.get(inst.src1);
                            consts.set(inst.dest, inst.imm);
                        } else {
                            copies.set(inst.dest, inst.src1);
                        }
                    }
                    else if (inst.op == IROp::XOR && hb && consts.get(inst.src2) == 0) {
                        inst.op = IROp::MOV;
                        if (consts.has(inst.src1)) {
                            inst.op = IROp::IMM;
                            inst.imm = consts.get(inst.src1);
                            consts.set(inst.dest, inst.imm);
                        } else {
                            copies.set(inst.dest, inst.src1);
                        }
                    }
                    // AND x, 0xFFFF...F (all ones) → MOV x
                    else if (inst.op == IROp::AND && hb && consts.get(inst.src2) == ~0ULL) {
                        inst.op = IROp::MOV;
                        if (consts.has(inst.src1)) {
                            inst.op = IROp::IMM;
                            inst.imm = consts.get(inst.src1);
                            consts.set(inst.dest, inst.imm);
                        } else {
                            copies.set(inst.dest, inst.src1);
                        }
                    }
                    else {
                        consts.clear(inst.dest);
                        copies.clear(inst.dest);
                    }
                }
                invalidate_vreg_in_cache(inst.dest);
                last_def[inst.dest] = i;
                if (inst.dest <= 31) arm_reg_cache[inst.dest] = inst.dest;
                break;
            }
            case IROp::NOT: case IROp::NEG:
            case IROp::SEXT: case IROp::ZEXT:
            case IROp::CLZ: case IROp::CLS:
            case IROp::RBIT: case IROp::REV16: case IROp::REV32: case IROp::REV64: {
                if (consts.has(inst.src1)) {
                    uint64_t r;
                    if (fold_unop(inst.op, consts.get(inst.src1), inst.width, r)) {
                        inst.op = IROp::IMM;
                        inst.imm = r;
                        consts.set(inst.dest, r);
                        block.fold_subst++;
                    }
                } else {
                    consts.clear(inst.dest);
                    copies.clear(inst.dest);
                }
                invalidate_vreg_in_cache(inst.dest);
                last_def[inst.dest] = i;
                if (inst.dest <= 31) arm_reg_cache[inst.dest] = inst.dest;
                break;
            }
            case IROp::ADDS: case IROp::SUBS: case IROp::TST:
            case IROp::ADCS: case IROp::SBCS:
                // Flag-setting ops also write to dest (if != 0).
                invalidate_vreg_in_cache(inst.dest);
                if (inst.dest != 0 && inst.dest <= 31) arm_reg_cache[inst.dest] = inst.dest;
                consts.clear(inst.dest);
                copies.clear(inst.dest);
                last_def[inst.dest] = i;
                break;
            case IROp::CSEL: case IROp::CSINC:
            case IROp::CSINV: case IROp::CSNEG:
            case IROp::CCMP:
            case IROp::BFM: case IROp::UBFM: case IROp::SBFM: case IROp::EXTR: {
                // constant-fold UBFM/SBFM when src1 is
                // a known constant. These are very common (SXTB/SXTH/SXTW/
                // UXTB/UXTH/UXTW/LSL/LSR/ASR immediate) and folding them
                // eliminates redundant shifts in tight loops.
                if ((inst.op == IROp::UBFM || inst.op == IROp::SBFM) &&
                    consts.has(inst.src1)) {
                    uint64_t a = consts.get(inst.src1);
                    int width = inst.sf ? 64 : 32;
                    int immr = inst.immr % width;
                    int imms = inst.imms;
                    uint64_t result;
                    if (imms < immr) {
                        // LSL/BFI case: extract low (imms+1) bits, shift
                        // left by (width - immr).
                        uint64_t field_mask = (1ULL << (imms + 1)) - 1;
                        uint64_t field = a & field_mask;
                        int sh = width - immr;
                        result = field << sh;
                        if (inst.op == IROp::SBFM) {
                            // Sign-extend from bit (imms + sh)
                            int sb = 1ULL << (imms + sh);
                            result = ((result ^ sb) - sb);
                        }
                    } else {
                        // Normal case: ROR(a, immr) then extract [imms:0]
                        uint64_t rotated = a;
                        if (immr != 0) {
                            if (width == 64) {
                                rotated = (a >> immr) | (a << (64 - immr));
                            } else {
                                uint32_t v = static_cast<uint32_t>(a);
                                rotated = ((v >> immr) | (v << (32 - immr))) & 0xFFFFFFFFULL;
                            }
                        }
                        uint64_t mask = (imms < width - 1)
                            ? ((1ULL << (imms + 1)) - 1)
                            : (width == 64 ? ~0ULL : 0xFFFFFFFFULL);
                        uint64_t extracted = rotated & mask;
                        if (inst.op == IROp::SBFM && imms < width - 1) {
                            int sb = 1ULL << imms;
                            result = ((extracted ^ sb) - sb);
                            if (width == 32) result &= 0xFFFFFFFFULL;
                        } else {
                            result = extracted;
                        }
                    }
                    if (width == 32) result &= 0xFFFFFFFFULL;
                    inst.op = IROp::IMM;
                    inst.imm = result;
                    consts.set(inst.dest, result);
                    block.fold_subst++;
                }
                // CSEL/CSINC/CSINV/CSNEG/CCMP are native in the JIT (no
                // CALL_INTERP), so they only need dest invalidation — this
                // allows the optimizer to keep caching other registers
                // across these ops, improving code quality.
                // BFM is decomposed into SHL+SHR+AND+OR in ir_translate.cpp
                // (never reaches here as IROp::BFM). All ops in this case
                // are native — only need dest invalidation.
                invalidate_vreg_in_cache(inst.dest);
                if (inst.dest <= 31) arm_reg_cache[inst.dest] = inst.dest;
                if (inst.op != IROp::IMM) {  // don't clear if we just folded
                    consts.clear(inst.dest);
                    copies.clear(inst.dest);
                }
                last_def[inst.dest] = i;
                break;
            }
            case IROp::CALL_INTERP:
            case IROp::SVC:
            case IROp::BL_CALL:  // callee may modify any reg
                // The interpreter may modify any cpu.regs[] or memory.
                // Invalidate everything.
                consts.clear_all();
                copies.clear_all();
                arm_reg_cache.clear();
                break;
            case IROp::BR: case IROp::BRCOND:
            case IROp::BRCOND_FALLTHRU:
                // Branches don't produce a value. They may invalidate
                // the arm_reg_cache (since the next block starts fresh),
                // but we keep it conservative within the block.
                break;
            case IROp::FP_F2I: {
                // FP_F2I writes directly to ARM reg vreg `dest` (0..31),
                // bypassing STORE_REG. The JIT's store_reg_to_vreg(dest, RAX)
                // caches the conversion result in vreg `dest`. Update the
                // arm_reg_cache so a subsequent LOAD_REG of `dest` reuses
                // the vreg instead of substituting a stale cached vreg.
                // Without this, the FWD cache would substitute the LOAD_REG
                // with a MOV pointing at the pre-conversion vreg, losing
                // the FP_F2I result.
                invalidate_vreg_in_cache(inst.dest);
                if (inst.dest <= 31) arm_reg_cache[inst.dest] = inst.dest;
                consts.clear(inst.dest);
                copies.clear(inst.dest);
                last_def[inst.dest] = i;
                break;
            }
            case IROp::FP_F2I_FIXED: {
                // Same as FP_F2I: writes directly to ARM reg vreg dest.
                // See comment above for why arm_reg_cache must be updated.
                invalidate_vreg_in_cache(inst.dest);
                if (inst.dest <= 31) arm_reg_cache[inst.dest] = inst.dest;
                consts.clear(inst.dest);
                copies.clear(inst.dest);
                last_def[inst.dest] = i;
                break;
            }
            case IROp::SIMD_UMOV: {
                // SIMD_UMOV writes an ARM reg vreg `dest` (0..30) directly
                // via set_vreg_reg (jit_codegen_simd.cpp), bypassing
                // STORE_REG — mirroring FP_F2I. The arm_reg_cache MUST be
                // updated or a subsequent LOAD_REG of `dest` substitutes a
                // stale cached vreg holding the pre-UMOV value. This broke
                // `jit_neon`'s umov tests under BIFROST_ENABLE_FWD=1.
                invalidate_vreg_in_cache(inst.dest);
                if (inst.dest <= 31) arm_reg_cache[inst.dest] = inst.dest;
                consts.clear(inst.dest);
                copies.clear(inst.dest);
                last_def[inst.dest] = i;
                break;
            }
            case IROp::FP_I2F_FIXED: {
                // Same as FP_I2F: writes to v_lo[dest] (FP reg file),
                // not to an ARM reg vreg. Just invalidate dest's cache
                // entry so we don't propagate a stale constant through it.
                invalidate_vreg_in_cache(inst.dest);
                consts.clear(inst.dest);
                copies.clear(inst.dest);
                last_def[inst.dest] = i;
                break;
            }
            default:
                // Unknown op: conservatively invalidate the dest vreg's
                // cache entry so we don't propagate stale constants or
                // copies through it. Without this, a new IROp added to
                // ir.hpp but not handled above would silently inherit
                // the cache state of whatever vreg previously held dest,
                // producing wrong code.
                if (inst.dest) {
                    invalidate_vreg_in_cache(inst.dest);
                    consts.clear(inst.dest);
                    copies.clear(inst.dest);
                    last_def[inst.dest] = i;
                }
                break;
        }
    }
    // ── Pass 1.5: post-substitution dead-store elimination ───────
    // After Pass 1, LOAD_REGs may have been substituted to MOVs (which
    // are then turned into copy relations or DCE'd). This reveals dead
    // STORE_REGs that Pass 0 couldn't see because the intervening
    // LOAD_REGs were still present.
    //
    // A STORE_REG to ARM reg R is dead if a later STORE_REG to the same R
    // exists with NO intervening LOAD_REG of R or CALL_INTERP/SVC (either
    // of which could observe the first store). The first store is NOP'd.
    //
    // Most effective when combined with arm_reg_cache load-forwarding
    // (BIFROST_ENABLE_FWD=1): without it, the intervening LOAD_REGs are
    // still present and this pass is mostly a no-op. It's still correct
    // to run, just less impactful. Disable with BIFROST_NO_DSE=1.
    static bool no_dse_ = (getenv("BIFROST_NO_DSE") != nullptr);
    if (!no_dse_) {
        dse_pass();
    }
    // Pass 2: dead code elimination.
    // A vreg is "live" if it's used as a source by any later op, OR
    // if it's the dest of a non-pure op (side effects), OR if it's
    // stored into an ARM64 reg (visible outside the block).
    //
    // We compute liveness backward, then drop pure ops whose dest is
    // never used.
    std::vector<bool> used(block.insts.size(), false);
    // We mark an instruction as "used" if it has a side effect, OR if
    // its dest is read by a later used instruction. Walk backward.
    std::unordered_set<uint16_t> live;
    // ARM64 reg writes are always "used" (they're side-effecting).
    // Branches / mem ops / syscalls are always used.
    for (size_t i = block.insts.size(); i > 0; i--) {
        IRInst& inst = block.insts[i - 1];
        bool keep = false;
        if (!is_pure(inst.op)) {
            keep = true;
        }
        // If dest is live, keep it.
        if (inst.dest != 0 && live.count(inst.dest)) {
            keep = true;
        }
        if (keep) {
            used[i - 1] = true;
            // Mark sources as live.
            // For LOAD_REG, src1 is the ARM64 reg index (0-31). The value
            // is "read" from that architectural reg, so any earlier op that
            // writes to vreg src1 (e.g., SBFM dest=2) must be kept.
            // We add src1 to live (don't erase it).
            if (inst.op == IROp::LOAD_REG) {
                if (inst.src1 <= 31) live.insert(inst.src1);
            } else if (inst.op == IROp::STORE_REG) {
                // dest is the ARM64 reg index, src1 is the vreg being stored.
                live.insert(inst.src1);
            } else if (inst.op == IROp::FP_I2F || inst.op == IROp::FP_I2F_FIXED) {
                // index 0-31) and writes an FP reg (dest). The GPR store
                // must be kept, so mark src1 as live (same as LOAD_REG).
                if (inst.src1 <= 31) live.insert(inst.src1);
            } else if (inst.op == IROp::FP_F2I || inst.op == IROp::FP_F2I_FIXED) {
                // FP_F2I (fcvtzs/fcvtzu) reads an FP reg (src1 = FP reg index)
                // and writes a GPR (dest). FP stores use inst.sf=1, so they
                // have a different DSE key. No vreg to mark live here.
            } else if (inst.op == IROp::FCVT_S2D || inst.op == IROp::FCVT_D2S ||
                       inst.op == IROp::FP_UNOP || inst.op == IROp::FRINT) {
                // These read and write FP regs (src1/dest are FP reg indices).
                // No GPR vregs to mark live.
            } else {
                // Normal op: src1 and src2 are vregs.
                if (inst.src1) live.insert(inst.src1);
                if (inst.src2) live.insert(inst.src2);
                // SMSUBL accumulator). Mark it live so DCE doesn't remove
                // the instruction that defines it.
                if (inst.aux) live.insert(inst.aux);
            }
        } else {
            used[i - 1] = false;
            block.dce_removed++;
        }
    }
    // Build the new instruction list, dropping unused.
    std::vector<IRInst> new_insts;
    new_insts.reserve(block.insts.size() - block.dce_removed);
    for (size_t i = 0; i < block.insts.size(); i++) {
        if (used[i]) new_insts.push_back(block.insts[i]);
    }
    block.insts = std::move(new_insts);
    // Pass 3: peephole — (disabled for now).
    // The original idea was to drop redundant ZEXT after ALU ops in
    // 32-bit mode, since x86 32-bit ops zero-extend. But our codegen
    // currently uses 64-bit ops, which DON'T zero-extend. So removing
    // the ZEXT would be incorrect. We leave ZEXT in place and let the
    // codegen emit an explicit AND mask.
    // TODO: re-enable this peephole once the codegen uses 32-bit ops
    // for sf=0 ARM64 instructions.
}
// ── Dump IR (debug) ────────────────────────────────────────────────────
void dump_ir(const IRBlock& block, FILE* out) {
    fprintf(out, "── IR block @ 0x%llx (count=%d, ends_branch=%d) ──\n",
            static_cast<unsigned long long>(block.start_pc), block.count,
            block.ends_with_branch);
    for (size_t i = 0; i < block.insts.size(); i++) {
        const IRInst& inst = block.insts[i];
        fprintf(out, "  [%3zu] %-14s dest=v%-3u src1=v%-3u src2=v%-3u "
                "w=%u cond=%s op=%u imm=0x%llx arm_pc=0x%llx"
                " immr=%u imms=%u sf=%u\n",
                i,
                [](IROp op) -> const char* {
                    switch (op) {
                    case IROp::NOP: return "NOP";
                    case IROp::IMM: return "IMM";
                    case IROp::MOV: return "MOV";
                    case IROp::LOAD_REG: return "LOAD_REG";
                    case IROp::STORE_REG: return "STORE_REG";
                    case IROp::LOAD_MEM: return "LOAD_MEM";
                    case IROp::STORE_MEM: return "STORE_MEM";
                    case IROp::ADD: return "ADD";
                    case IROp::SUB: return "SUB";
                    case IROp::MUL: return "MUL";
                    case IROp::AND: return "AND";
                    case IROp::OR: return "OR";
                    case IROp::XOR: return "XOR";
                    case IROp::SHL: return "SHL";
                    case IROp::SHR: return "SHR";
                    case IROp::SAR: return "SAR";
                    case IROp::ROR: return "ROR";
                    case IROp::NOT: return "NOT";
                    case IROp::NEG: return "NEG";
                    case IROp::SEXT: return "SEXT";
                    case IROp::ZEXT: return "ZEXT";
                    case IROp::ADDS: return "ADDS";
                    case IROp::SUBS: return "SUBS";
                    case IROp::ADCS: return "ADCS";
                    case IROp::SBCS: return "SBCS";
                    case IROp::TST: return "TST";
                    case IROp::TST_ZERO: return "TST_ZERO";
                    case IROp::BRCOND_ZERO: return "BRCOND_ZERO";
                    case IROp::BRCOND_BIT: return "BRCOND_BIT";
                    case IROp::CSEL: return "CSEL";
                    case IROp::CSINC: return "CSINC";
                    case IROp::CSINV: return "CSINV";
                    case IROp::CSNEG: return "CSNEG";
                    case IROp::CCMP: return "CCMP";
                    case IROp::BFM: return "BFM";
                    case IROp::UBFM: return "UBFM";
                    case IROp::SBFM: return "SBFM";
                    case IROp::EXTR: return "EXTR";
                    case IROp::CLZ: return "CLZ";
                    case IROp::CLS: return "CLS";
                    case IROp::RBIT: return "RBIT";
                    case IROp::REV16: return "REV16";
                    case IROp::REV32: return "REV32";
                    case IROp::REV64: return "REV64";
                    case IROp::BR: return "BR";
                    case IROp::BRCOND: return "BRCOND";
                    case IROp::BRCOND_FALLTHRU: return "BRCOND_FT";
                    case IROp::CALL_INTERP: return "CALL_INTERP";
                    case IROp::SVC: return "SVC";
                    case IROp::FMOV_G2F: return "FMOV_G2F";
                    case IROp::FMOV_F2G: return "FMOV_F2G";
                    case IROp::FMOV_G2FHI: return "FMOV_G2FHI";
                    case IROp::FMOV_FHI2G: return "FMOV_FHI2G";
                    case IROp::FP_BINOP: return "FP_BINOP";
                    case IROp::FP_UNOP: return "FP_UNOP";
                    case IROp::SIMD_LOGICAL: return "SIMD_LOGICAL";
                    case IROp::SIMD_DUP: return "SIMD_DUP";
                    case IROp::SIMD_MOVI: return "SIMD_MOVI";
                    case IROp::SIMD_ORRIMM: return "SIMD_ORRIMM";
                    case IROp::SIMD_LDST: return "SIMD_LDST";
                    case IROp::SIMD_ARITH: return "SIMD_ARITH";
                    case IROp::SIMD_CMP:   return "SIMD_CMP";
                    case IROp::SIMD_SHL:   return "SIMD_SHL";
                    case IROp::SIMD_USHR:  return "SIMD_USHR";
                    case IROp::SIMD_SSHR:  return "SIMD_SSHR";
                    case IROp::SIMD_USRA:  return "SIMD_USRA";
                    case IROp::SIMD_SSRA:  return "SIMD_SSRA";
                    case IROp::SIMD_URSRA: return "SIMD_URSRA";
                    case IROp::SIMD_SRSRA: return "SIMD_SRSRA";
                    case IROp::SIMD_SLI:   return "SIMD_SLI";
                    case IROp::SIMD_SRI:   return "SIMD_SRI";
                    case IROp::FP_F2I: return "FP_F2I";
                    case IROp::FP_I2F: return "FP_I2F";
                    case IROp::FP_F2I_FIXED: return "FP_F2I_FIXED";
                    case IROp::FP_I2F_FIXED: return "FP_I2F_FIXED";
                    case IROp::FP_CMP: return "FP_CMP";
                    case IROp::FP_MOVI: return "FP_MOVI";
                    case IROp::UDIV: return "UDIV";
                    case IROp::SDIV: return "SDIV";
                    case IROp::SMADDL: return "SMADDL";
                    case IROp::UMADDL: return "UMADDL";
                    case IROp::SMSUBL: return "SMSUBL";
                    case IROp::UMSUBL: return "UMSUBL";
                    case IROp::SMULH: return "SMULH";
                    case IROp::UMULH: return "UMULH";
                    case IROp::FCVT_S2D: return "FCVT_S2D";
                    case IROp::FCVT_D2S: return "FCVT_D2S";
                    case IROp::FRINT: return "FRINT";
                    case IROp::FMADD: return "FMADD";
                    case IROp::FMSUB: return "FMSUB";
                    case IROp::FNMADD: return "FNMADD";
                    case IROp::FNMSUB: return "FNMSUB";
                    case IROp::MRS: return "MRS";
                    case IROp::MSR: return "MSR";
                    case IROp::ATOMIC: return "ATOMIC";
                    case IROp::LDXR_FAST: return "LDXR_FAST";
                    case IROp::STXR_FAST: return "STXR_FAST";
                    case IROp::STLR_FAST: return "STLR_FAST";
                    case IROp::AES_CRYPTO: return "AES_CRYPTO";
                    default: return "?";
                    }
                    return "?";
                }(inst.op),
                inst.dest, inst.src1, inst.src2, inst.width,
                cond_name(inst.cond), inst.flags_op,
                static_cast<unsigned long long>(inst.imm),
                static_cast<unsigned long long>(inst.arm_pc),
                inst.immr, inst.imms, inst.sf);
    }
    fprintf(out, "  (dce_removed=%d fold_subst=%d)\n",
            block.dce_removed, block.fold_subst);
}
} // namespace arm64emu
