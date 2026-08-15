// jit_codegen_vec_cache.cpp — XMM vector register cache.
#include <cstdlib>
#include <algorithm>
#include <vector>
//
// Guest vector regs (0-31) are pinned into host XMM3-15 across an entire
// JIT block so that hot SIMD/FMA loops keep their accumulators resident in
// XMM registers instead of bouncing them through cpu.v_lo/v_hi every
// instruction (each cached FMLA/FSUB drops from 8 memory ops to a single
// 128-bit VEX op).
//
// Invariants (see vec_cache_may_enable):
//   - The cache is enabled ONLY for blocks whose vector-touching ops are
//     all cache-aware (SIMD_FP_FMA / SIMD_FP_ARITH); every other op is a
//     GPR-only op that neither reads cpu.v_lo/v_hi nor clobbers XMM3-15.
//     Therefore no flush is ever needed mid-block, and — critically for
//     self-loop blocks — no vector value is ever reloaded from the CPU
//     struct inside the body. Self-loop re-entry jumps to the body start
//     (skipping the prologue), so the pinned XMM regs naturally persist
//     as loop-carried state.
//   - The prologue loads (vec_emit_prologue_loads) run only on cold entry
//     (the dispatcher / chain entry calls the block prologue).
//   - The epilogue writeback (vec_cache_writeback_all) is emitted right
//     before the block returns, so cpu.v_lo/v_hi are up to date for the
//     next block (or the interpreter).
//   - XMM0/XMM1/XMM2 remain scratch (used by the non-cached SIMD codegen
//     and by the cached FABD sign-mask load).
#include "jit/frostjit.hpp"
namespace arm64emu {

// ── Cache enablement pre-scan ──────────────────────────────────────────
// Ops that either have a cached fast path for pinned XMM operands
// (SIMD_FP_FMA / SIMD_FP_ARITH / SIMD_LOGICAL / SIMD_DUP) or are GPR-only
// ops that neither read cpu.v_lo/v_hi nor clobber XMM3-15. Everything
// else (any other FP/SIMD op, CALL_INTERP, SVC, BL_CALL, atomic helpers,
// ...) disqualifies the block — the cache is all-or-nothing per block.
// (If a whitelisted GPR op does fall through to emit_call_interp, the
// guard inside emit_call_interp writes back + reloads the pinned XMM
// regs around the host call, so the cache stays correct.)
static bool vec_cache_compatible_op(IROp op) {
    switch (op) {
        case IROp::SIMD_FP_FMA:
        case IROp::SIMD_FP_ARITH:
        case IROp::SIMD_LOGICAL:
        case IROp::SIMD_DUP:
        case IROp::SIMD_MOVI:     // constant broadcast — cached fast path
        case IROp::SIMD_ORRIMM:   // dest read-modify-write — cached fast path
        case IROp::SIMD_ST16:     // 16-byte guest store — cached fast path
        case IROp::SIMD_LD16:     // 16-byte guest load — cached fast path
        case IROp::NOP: case IROp::IMM: case IROp::MOV:
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

bool FrostJIT::vec_cache_may_enable(const IRBlock& block) {
    static const bool no_vec_cache_ = (getenv("BIFROST_NO_VEC_CACHE") != nullptr);
    if (no_vec_cache_) return false;
    if (!has_fma3()) return false;
    bool  vec_used[32] = {};
    int   used_count = 0;
    for (const IRInst& inst : block.insts) {
        if (!vec_cache_compatible_op(inst.op)) return false;
        if (inst.op == IROp::SIMD_FP_FMA || inst.op == IROp::SIMD_FP_ARITH ||
            inst.op == IROp::SIMD_LOGICAL) {
            if (inst.dest < 32 && !vec_used[inst.dest]) {
                vec_used[inst.dest] = true; used_count++;
            }
            if (inst.src1 < 32 && !vec_used[inst.src1]) {
                vec_used[inst.src1] = true; used_count++;
            }
            if (inst.src2 < 32 && !vec_used[inst.src2]) {
                vec_used[inst.src2] = true; used_count++;
            }
        } else if (inst.op == IROp::SIMD_DUP) {
            // src1 is a GPR vreg (>= 32); only the vector dest is pinned.
            if (inst.dest < 32 && !vec_used[inst.dest]) {
                vec_used[inst.dest] = true; used_count++;
            }
        } else if (inst.op == IROp::SIMD_MOVI) {
            // Constant broadcast: dest is the only vector operand.
            if (inst.dest < 32 && !vec_used[inst.dest]) {
                vec_used[inst.dest] = true; used_count++;
            }
        } else if (inst.op == IROp::SIMD_ORRIMM) {
            // Read-modify-write: dest is both source and dest.
            if (inst.dest < 32 && !vec_used[inst.dest]) {
                vec_used[inst.dest] = true; used_count++;
            }
        } else if (inst.op == IROp::SIMD_ST16) {
            // Guest 16-byte store: src2 is the FIRST vector written to guest
            // memory; flags_op = register count (1..4), regs contiguous.
            // cond=1 (broadcast, `stp q0,q0`) keeps src2 constant across all
            // halves — only that one register needs pinning.
            uint32_t n = inst.flags_op ? inst.flags_op : 1;
            for (uint32_t i = 0; i < n; i++) {
                int v = (inst.cond && i) ? (inst.src2 & 31) : ((inst.src2 + i) & 31);
                if (!vec_used[v]) { vec_used[v] = true; used_count++; }
            }
        } else if (inst.op == IROp::SIMD_LD16) {
            // Guest 16-byte load: dest is the FIRST vector destination;
            // flags_op = register count (1..4), regs contiguous.
            uint32_t n = inst.flags_op ? inst.flags_op : 1;
            for (uint32_t i = 0; i < n; i++) {
                int v = (inst.dest + i) & 31;
                if (!vec_used[v]) { vec_used[v] = true; used_count++; }
            }
        }
    }
    if (used_count == 0) return false;
    if (used_count > VEC_XMM_END - VEC_XMM_START + 1) return false;
    vec_cache_active_ = true;
    int xmm = VEC_XMM_START;
    for (int v = 0; v < 32; v++) {
        if (vec_used[v]) {
            vec_cache_[v] = xmm;
            vec_xmm_owner_[xmm] = v;
            vec_pinned_[vec_pinned_count_++] = v;
            xmm++;
        }
    }
    return true;
}

// ── Scalar-FP cache (lo-only) ─────────────────────────────────────────
// Ops allowed in an fp-cache block: every op either has a cache-aware
// scalar-FP fast path (pinned XMM operand handling), reads/writes only
// GPRs (the vec-cache GPR whitelist), or is a call with a writeback+reload
// guard (BL_CALL/BLR_CALL — the callee may clobber ANY XMM and ANY
// cpu.v_lo entry, so the guard in frostjit.cpp flushes + reloads around it).
// SIMD ops, SVC, CALL_INTERP, atomics, and the FMOV hi-half ops (which
// read/write v_hi, never pinned in the lo-only cache) all disqualify.
static bool fp_cache_compatible_op(IROp op) {
    switch (op) {
        // Scalar FP ops with cache-aware fast paths:
        case IROp::FP_BINOP:
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
        case IROp::NOP: case IROp::IMM: case IROp::MOV:
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
    for (const IRInst& inst : block.insts) {
        if (!fp_cache_compatible_op(inst.op)) return false;
        switch (inst.op) {
            case IROp::FP_BINOP:
                fp_touch(inst.dest, fp_use); fp_touch(inst.src1, fp_use); fp_touch(inst.src2, fp_use);
                break;
            case IROp::FP_UNOP:
            case IROp::FP_MOV:
                fp_touch(inst.dest, fp_use); fp_touch(inst.src1, fp_use);
                break;
            case IROp::FP_CMP:
                fp_touch(inst.src1, fp_use);
                if (!(inst.imm & 1)) fp_touch(inst.src2, fp_use);  // register form only
                break;
            case IROp::FP_MOVI:
                fp_touch(inst.dest, fp_use);
                break;
            case IROp::FP_F2I:
                fp_touch(inst.src1, fp_use);
                break;
            case IROp::FP_I2F:
                fp_touch(inst.dest, fp_use);
                break;
            case IROp::FP_F2I_FIXED:
                fp_touch(inst.src1, fp_use);
                if (inst.imms & 1) fp_touch(inst.dest, fp_use);  // fp_dest subop
                break;
            case IROp::FP_I2F_FIXED:
                fp_touch(inst.dest, fp_use);
                if (inst.imms & 1) fp_touch(inst.src1, fp_use);  // fp_src subop
                break;
            case IROp::FCVT_S2D:
            case IROp::FCVT_D2S:
            case IROp::FRINT:
                fp_touch(inst.dest, fp_use); fp_touch(inst.src1, fp_use);
                break;
            case IROp::FMADD:
            case IROp::FMSUB:
            case IROp::FNMADD:
            case IROp::FNMSUB:
                fp_touch(inst.dest, fp_use); fp_touch(inst.src1, fp_use);
                fp_touch(inst.src2, fp_use); fp_touch(inst.imm, fp_use);  // Va (acc)
                break;
            case IROp::FMOV_G2F:
                fp_touch(inst.dest, fp_use);
                break;
            case IROp::FMOV_F2G:
                fp_touch(inst.src1, fp_use);
                break;
            case IROp::LOAD_REG:
                if (inst.sf) fp_touch(inst.src1, fp_use);  // sf=1: load_fp_reg
                break;
            case IROp::STORE_REG:
                if (inst.sf) fp_touch(inst.dest, fp_use);  // sf=1: store_fp_reg
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
    int xmm = VEC_XMM_START;
    for (int i = 0; i < limit; i++) {
        int v = cand[i].second;
        vec_cache_[v] = xmm;
        vec_xmm_owner_[xmm] = v;
        vec_pinned_[vec_pinned_count_++] = v;
        xmm++;
    }
    return true;
}

void FrostJIT::vec_cache_reset() {
    vec_cache_active_ = false;
    fp_cache_active_ = false;
    vec_pinned_count_ = 0;
    for (int i = 0; i < 32; i++) {
        vec_cache_[i] = -1;
        vec_dirty_[i] = false;
    }
    for (int i = 0; i < 16; i++) vec_xmm_owner_[i] = -1;
}

// ── Prologue / epilogue ────────────────────────────────────────────────
// movsd xmm, [rbx+lo] + movhpd xmm, [rbx+hi].
void FrostJIT::vec_emit_load_lo_hi(int xmm, int vreg) {
    bool r = (xmm >= 8);
    emit_byte(rex(false, r, false, false));
    emit_byte(0x0F); emit_byte(0x10);                  // movsd
    emit_modrm_disp(xmm, CPU_REG, V_LO_OFF + vreg * 8);
    emit_byte(rex(false, r, false, false));
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
            emit_byte(rex(false, r, false, false));
            emit_byte(0x0F); emit_byte(0x10);          // movsd
            emit_modrm_disp(xmm, CPU_REG, V_LO_OFF + v * 8);
        } else {
            vec_emit_load_lo_hi(vec_cache_[v], v);
        }
        vec_dirty_[v] = false;
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
        emit_byte(rex(false, r, false, false));
        emit_byte(0x0F); emit_byte(0x11);              // movsd
        emit_modrm_disp(xmm, CPU_REG, V_LO_OFF + v * 8);
        if (!fp_cache_active_) {
            emit_byte(rex(false, r, false, false));
            emit_byte(0x0F); emit_byte(0x17);          // movhpd
            emit_modrm_disp(xmm, CPU_REG, V_HI_OFF + v * 8);
        }
        if (clear_flags) vec_dirty_[v] = false;
    }
}

void FrostJIT::vec_cache_mark_dirty(int vreg) {
    if (vreg >= 0 && vreg < 32) vec_dirty_[vreg] = true;
}

// ── VEX 3-byte helpers ────────────────────────────────────────────────
// VEX.NDS.128.XX.0F{38}: C4 [R~ X~ B~ mmmmm] [W vvvv~ L pp] opcode modrm.
// `reg_field`/`rm_field` are the (possibly ≥8) x86 register numbers used in
// the ModRM reg/rm fields; R~/B~ extend them exactly like REX.R/REX.B.
// L=0 (128-bit) always; pp: 0=ps/no-prefix, 1=pd/66.
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
        emit_byte(rex(false, xmm >= 8, false, xs >= 8));
        emit_byte(0x0F); emit_byte(0x10);
        emit_byte(modrm(3, xmm & 7, xs & 7));
    } else {
        emit_byte(prefix);
        emit_byte(0x0F); emit_byte(0x10);
        emit_modrm_disp(xmm, CPU_REG, V_LO_OFF + vreg * 8);
    }
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
        // movsd/movss xmm_d, xmm  (reg-reg; REX for XMM8-15).
        // 0F 11 is the STORE form: ModRM.reg = SOURCE, ModRM.rm = DEST,
        // so the REX.R/REX.B extension bits follow the source (xmm) /
        // dest (xd) respectively — NOT xd/xmm.
        emit_byte(prefix);
        emit_byte(rex(false, xmm >= 8, false, xd >= 8));
        emit_byte(0x0F); emit_byte(0x11);
        emit_byte(modrm(3, xmm & 7, xd & 7));
        vec_cache_mark_dirty(vreg);
    } else {
        emit_byte(prefix);
        emit_byte(0x0F); emit_byte(0x11);
        emit_modrm_disp(xmm, CPU_REG, V_LO_OFF + vreg * 8);
    }
}

// Zero v_hi[vreg] via pxor xmm0,xmm0 + movsd [hi],xmm0 — no GPR touched.
// v_hi is never cached in fp-cache blocks, so this is always a memory
// store. MUST run after the lo result is stored.
void FrostJIT::fp_zero_hi(int vreg) {
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
