// jit_codegen_vec_cache.cpp — XMM vector register cache.
#include <cstdlib>
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

void FrostJIT::vec_cache_reset() {
    vec_cache_active_ = false;
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
        vec_emit_load_lo_hi(vec_cache_[v], v);
        vec_dirty_[v] = false;
    }
}

void FrostJIT::vec_cache_writeback_all() {
    if (!vec_cache_active_) return;
    for (int i = 0; i < vec_pinned_count_; i++) {
        int v = vec_pinned_[i];
        if (!vec_dirty_[v]) continue;
        int xmm = vec_cache_[v];
        bool r = (xmm >= 8);
        emit_byte(rex(false, r, false, false));
        emit_byte(0x0F); emit_byte(0x11);              // movsd
        emit_modrm_disp(xmm, CPU_REG, V_LO_OFF + v * 8);
        emit_byte(rex(false, r, false, false));
        emit_byte(0x0F); emit_byte(0x17);              // movhpd
        emit_modrm_disp(xmm, CPU_REG, V_HI_OFF + v * 8);
        vec_dirty_[v] = false;
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

} // namespace arm64emu
