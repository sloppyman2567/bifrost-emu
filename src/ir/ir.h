// ir/ir.h — internal IR helpers shared by the translator, lowerer, and
// optimizer. NOT a public header — only included from src/ir/*.cpp.
//
// Public IR types live in include/ir/ir.hpp.
#pragma once
#include "ir/ir.hpp"
#include <cstdint>
namespace arm64emu {
// ── Scratch vreg allocator ──────────────────────────────────────────────
// Per-block resetable. The translator allocates a fresh vreg for every
// intermediate value; the optimizer later reuses them.
//
// widened from uint8_t to uint16_t to prevent
// wrap-around. Arch regs use 0-32, scratch starts at 33. With uint8_t,
// large blocks (82+ ARM instructions) exhausted the 256-vreg space and
// wrapped to 0, colliding with arch regs (vreg 31 = SP).
struct VregAlloc {
    uint16_t next = 33;
    uint16_t alloc() { return next++; }
    void reset() { next = 33; }
};
// Per-thread allocator (each JIT thread has its own to avoid locking).
extern thread_local VregAlloc g_alloc;
// Reset the per-block allocator. Called at the start of each block.
void ir_reset_vreg_alloc();
// ── IR emit helpers ─────────────────────────────────────────────────────
// NOTE: the old generic emit() (positional width/cond/flags/imm args) and
// emit_bf() are GONE — every op is constructed through a typed factory
// (IRInst::make_* in ir.hpp) or the generic IRInst::make() for
// parameter-less ops. emit_aux() survives: it only touches public
// dataflow fields (op/dest/src1/src2/aux/arm_pc).
// Emit with auxiliary vreg (for SMADDL/SMSUBL accumulator).
inline void emit_aux(IRBlock& b, IROp op, uint16_t dest,
                     uint16_t src1, uint16_t src2, uint16_t aux_vreg,
                     uint64_t arm_pc) {
    IRInst inst{};
    inst.op = op;
    inst.dest = dest;
    inst.src1 = src1;
    inst.src2 = src2;
    inst.aux = aux_vreg;
    inst.arm_pc = arm_pc;
    b.insts.push_back(inst);
}
// Typed emit for SIMD_TBL (table lookup). Packs SimdTblParams via the
// IRInst factory so emit and codegen cannot disagree on the layout.
inline void emit_tbl(IRBlock& b, uint16_t dest, uint16_t table,
                     uint16_t index, SimdTblParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_tbl(dest, table, index, p, arm_pc));
}
// Typed emit for SBFM/UBFM (bitfield). The opcode selects signed vs
// unsigned (from the translator's cls switch); params carry immr/imms/sf.
inline void emit_bf_typed(IRBlock& b, IROp op, uint16_t dest, uint16_t src,
                          BfParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_bf(op, dest, src, p, arm_pc));
}
// Typed emits for SEXT/ZEXT (sign/zero extend from `bits` width).
// arm_pc defaults to 0, matching the old emit() convention at the
// helper call sites (apply_extend/apply_shift/zext_if_32bit predate
// per-op pcs; codegen never reads arm_pc for these ops).
inline void emit_sext(IRBlock& b, uint16_t dest, uint16_t src, uint8_t bits,
                      uint64_t arm_pc = 0) {
    b.insts.push_back(IRInst::make_sext(dest, src, bits, arm_pc));
}
inline void emit_zext(IRBlock& b, uint16_t dest, uint16_t src, uint8_t bits,
                      uint64_t arm_pc = 0) {
    b.insts.push_back(IRInst::make_zext(dest, src, bits, arm_pc));
}
// Typed emits for cond/select/compare/branch/sysreg ops.
inline void emit_csel(IRBlock& b, uint16_t dest, uint16_t src1,
                      uint16_t src2, CselParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_csel(dest, src1, src2, p, arm_pc));
}
inline void emit_ccmp(IRBlock& b, uint16_t src1, uint16_t src2,
                      CcmpParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_ccmp(src1, src2, p, arm_pc));
}
inline void emit_addsub(IRBlock& b, IROp op, uint16_t dest, uint16_t src1,
                        uint16_t src2, AddSubParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_addsub(op, dest, src1, src2, p, arm_pc));
}
inline void emit_tst(IRBlock& b, uint16_t src1, uint16_t src2,
                     TstParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_tst(src1, src2, p, arm_pc));
}
// Typed emit for GPR SHL/SHR/SAR/ROR. The opcode selects the shift kind;
// params carry the width convention (32 = 32-bit form, else 64-bit).
inline void emit_gpr_shift(IRBlock& b, IROp op, uint16_t dest, uint16_t src1,
                           uint16_t src2, GprShiftParams p,
                           uint64_t arm_pc = 0) {
    b.insts.push_back(IRInst::make_gpr_shift(op, dest, src1, src2, p,
                                             arm_pc));
}
// Typed emits for CLZ/REV64 (bits + rd slot).
inline void emit_clz(IRBlock& b, uint16_t dest, uint16_t src,
                     ClzParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_clz(dest, src, p, arm_pc));
}
inline void emit_rev64(IRBlock& b, uint16_t dest, uint16_t src,
                       ClzParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_rev64(dest, src, p, arm_pc));
}
// Typed emit for the AES/PMULL crypto ops (subop in imm).
inline void emit_aes(IRBlock& b, uint16_t dest, uint16_t src1, uint16_t src2,
                     AesParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_aes(dest, src1, src2, p, arm_pc));
}
// Typed emits for guest memory access, LSE atomics, and the 128/16-byte
// vector load/store helpers.
inline void emit_load_mem(IRBlock& b, uint16_t dest, uint16_t base,
                          MemParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_load_mem(dest, base, p, arm_pc));
}
inline void emit_store_mem(IRBlock& b, uint16_t base, uint16_t value,
                           MemParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_store_mem(base, value, p, arm_pc));
}
inline void emit_atomic(IRBlock& b, uint16_t dest, uint16_t base,
                        uint16_t operand, AtomicParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_atomic(dest, base, operand, p, arm_pc));
}
inline void emit_ldst(IRBlock& b, uint16_t dest, uint16_t lo,
                      uint16_t hi_or_zero, LdStParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_ldst(dest, lo, hi_or_zero, p, arm_pc));
}
inline void emit_ld16(IRBlock& b, uint16_t dest, uint16_t base,
                      Ld16Params p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_ld16(dest, base, p, arm_pc));
}
inline void emit_st16(IRBlock& b, uint16_t base, uint16_t src,
                      St16Params p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_st16(base, src, p, arm_pc));
}
// Typed emit for UDIV/SDIV. The opcode selects unsigned vs signed.
inline void emit_div(IRBlock& b, IROp op, uint16_t dest, uint16_t src1,
                     uint16_t src2, DivParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_div(op, dest, src1, src2, p, arm_pc));
}
inline void emit_brcond(IRBlock& b, BrCondParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_brcond(p, arm_pc));
}
inline void emit_brcond_zero(IRBlock& b, uint16_t src, BrCondZeroParams p,
                             uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_brcond_zero(src, p, arm_pc));
}
inline void emit_brcond_bit(IRBlock& b, uint16_t src, BrCondBitParams p,
                            uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_brcond_bit(src, p, arm_pc));
}
inline void emit_brcond_fallthru(IRBlock& b, uint64_t target,
                                 uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_brcond_fallthru(target, arm_pc));
}
inline void emit_brcond_skip(IRBlock& b, uint8_t cond, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_brcond_skip(cond, arm_pc));
}
inline void emit_bl_call(IRBlock& b, uint64_t target, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_bl_call(target, arm_pc));
}
inline void emit_mrs(IRBlock& b, uint16_t dest, uint64_t sys_idx,
                     uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_mrs(dest, sys_idx, arm_pc));
}
inline void emit_msr(IRBlock& b, uint16_t src, uint64_t sys_idx,
                     uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_msr(src, sys_idx, arm_pc));
}
// Typed emit for SIMD_INS (element insert). Packs SimdInsParams via the
// IRInst factory so emit and codegen cannot disagree on the layout.
// src_vec = source vector (Vn), rmw = read-modify-write dest (= dest).
inline void emit_ins(IRBlock& b, uint16_t dest, uint16_t src_vec,
                     uint16_t rmw, SimdInsParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_ins(dest, src_vec, rmw, p, arm_pc));
}
// Typed emits for the unary vector ops (2REG/CVTF/XTN). All share the
// SimdSubopParams shape (imm=subop, width=esize, flags_op=Q); the factory
// pins the opcode. src2 is always 0 for these ops.
inline void emit_2reg(IRBlock& b, uint16_t dest, uint16_t src,
                      SimdSubopParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_2reg(dest, src, p, arm_pc));
}
inline void emit_cvtf(IRBlock& b, uint16_t dest, uint16_t src,
                      SimdSubopParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_cvtf(dest, src, p, arm_pc));
}
inline void emit_xtn(IRBlock& b, uint16_t dest, uint16_t src,
                     SimdSubopParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_xtn(dest, src, p, arm_pc));
}
// Typed emits for the binary vector ops (PERMUTE/PAIRMIN/ADDP). PERMUTE
// and PAIRMIN share the SimdBinopParams shape (imm=subop, width=esize,
// flags_op=Q, real src2); ADDP only varies in Q (width=1, imm=0 fixed).
inline void emit_permute(IRBlock& b, uint16_t dest, uint16_t src1,
                         uint16_t src2, SimdBinopParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_permute(dest, src1, src2, p, arm_pc));
}
inline void emit_pairmin(IRBlock& b, uint16_t dest, uint16_t src1,
                         uint16_t src2, SimdBinopParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_pairmin(dest, src1, src2, p, arm_pc));
}
inline void emit_addp(IRBlock& b, uint16_t dest, uint16_t src1,
                      uint16_t src2, SimdAddpParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_addp(dest, src1, src2, p, arm_pc));
}
// Typed emits for the Batch-A ops. ARITH/LOGICAL hardcode their constant
// fields (flags_op=0 resp. width/cond/flags_op=0); the rest share the
// SimdSubopParams shape with op-pinned factories.
inline void emit_arith(IRBlock& b, uint16_t dest, uint16_t src1,
                       uint16_t src2, SimdArithParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_arith(dest, src1, src2, p, arm_pc));
}
inline void emit_logical(IRBlock& b, uint16_t dest, uint16_t src1,
                         uint16_t src2, SimdLogicParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_logical(dest, src1, src2, p, arm_pc));
}
// Typed emit for the nine vector shift-by-immediate ops. The opcode
// selects the shift kind (from the translator's subop switch); the
// params carry shift/esize/Q.
inline void emit_simd_shift(IRBlock& b, IROp shift_op, uint16_t dest,
                            uint16_t src, SimdShiftParams p,
                            uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_shift(shift_op, dest, src, p, arm_pc));
}
// Typed emits for UMOV/SMOV/ORRIMM/MOVI/DUP.
inline void emit_umov(IRBlock& b, uint16_t dest, uint16_t src_vec,
                      SimdMovParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_umov(dest, src_vec, p, arm_pc));
}
inline void emit_smov(IRBlock& b, uint16_t dest, uint16_t src_vec,
                      SimdMovParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_smov(dest, src_vec, p, arm_pc));
}
inline void emit_orrimm(IRBlock& b, uint16_t dest, SimdOrrImmParams p,
                        uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_orrimm(dest, p, arm_pc));
}
inline void emit_movi(IRBlock& b, uint16_t dest, SimdMoviParams p,
                      uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_movi(dest, p, arm_pc));
}
inline void emit_dup(IRBlock& b, uint16_t dest, uint16_t src,
                     SimdDupParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_dup(dest, src, p, arm_pc));
}
// Typed emits for the compound-imm ops (SHRN_SAT/MUL_ELEM): the factory
// packs subop | (shift/lane << 8) so emit and codegen share the layout.
inline void emit_shrn_sat(IRBlock& b, uint16_t dest, uint16_t src,
                          SimdShrnSatParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_shrn_sat(dest, src, p, arm_pc));
}
inline void emit_mul_elem(IRBlock& b, uint16_t dest, uint16_t src1,
                          uint16_t src2, SimdMulElemParams p,
                          uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_mul_elem(dest, src1, src2, p, arm_pc));
}
// Typed emits for the scalar FP ops.
inline void emit_fp_binop(IRBlock& b, uint16_t dest, uint16_t src1,
                          uint16_t src2, FpBinopParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_fp_binop(dest, src1, src2, p, arm_pc));
}
inline void emit_fp_unop(IRBlock& b, uint16_t dest, uint16_t src,
                         FpUnopParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_fp_unop(dest, src, p, arm_pc));
}
inline void emit_fp_mov(IRBlock& b, uint16_t dest, uint16_t src,
                        FpMovParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_fp_mov(dest, src, p, arm_pc));
}
inline void emit_fp_cmp(IRBlock& b, uint16_t dest, uint16_t src1,
                        uint16_t src2, FpCmpParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_fp_cmp(dest, src1, src2, p, arm_pc));
}
inline void emit_fp_movi(IRBlock& b, uint16_t dest, FpMoviParams p,
                         uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_fp_movi(dest, p, arm_pc));
}
inline void emit_fp_csel(IRBlock& b, uint16_t dest, uint16_t src1,
                         uint16_t src2, FpCselParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_fp_csel(dest, src1, src2, p, arm_pc));
}
inline void emit_fp_f2i(IRBlock& b, uint16_t dest, uint16_t src,
                        FpF2IParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_fp_f2i(dest, src, p, arm_pc));
}
inline void emit_fp_i2f(IRBlock& b, uint16_t dest, uint16_t src,
                        FpI2FParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_fp_i2f(dest, src, p, arm_pc));
}
inline void emit_fp_f2i_fixed(IRBlock& b, uint16_t dest, uint16_t src,
                              FpFixedParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_fp_f2i_fixed(dest, src, p, arm_pc));
}
inline void emit_fp_i2f_fixed(IRBlock& b, uint16_t dest, uint16_t src,
                              FpFixedParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_fp_i2f_fixed(dest, src, p, arm_pc));
}
inline void emit_fp_frint(IRBlock& b, uint16_t dest, uint16_t src,
                          FpFrintParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_fp_frint(dest, src, p, arm_pc));
}
// Typed emit for the FMADD/FMSUB/FNMADD/FNMSUB family. The opcode selects
// the form (from the translator's o1/o2 switch); params carry bits + acc.
inline void emit_fp_fused(IRBlock& b, IROp fma_op, uint16_t dest,
                          uint16_t src1, uint16_t src2, FpFusedParams p,
                          uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_fp_fused(fma_op, dest, src1, src2, p,
                                            arm_pc));
}
inline void emit_cmp(IRBlock& b, uint16_t dest, uint16_t src1,
                     uint16_t src2, SimdSubopParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_cmp(dest, src1, src2, p, arm_pc));
}
inline void emit_fp_arith(IRBlock& b, uint16_t dest, uint16_t src1,
                          uint16_t src2, SimdSubopParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_fp_arith(dest, src1, src2, p, arm_pc));
}
inline void emit_fp_fma(IRBlock& b, uint16_t dest, uint16_t src1,
                        uint16_t src2, SimdSubopParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_fp_fma(dest, src1, src2, p, arm_pc));
}
inline void emit_sataddsub(IRBlock& b, uint16_t dest, uint16_t src1,
                           uint16_t src2, SimdSubopParams p,
                           uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_sataddsub(dest, src1, src2, p, arm_pc));
}
inline void emit_abdl(IRBlock& b, uint16_t dest, uint16_t src1,
                      uint16_t src2, SimdSubopParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_abdl(dest, src1, src2, p, arm_pc));
}
inline void emit_abd(IRBlock& b, uint16_t dest, uint16_t src1,
                     uint16_t src2, SimdSubopParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_abd(dest, src1, src2, p, arm_pc));
}
inline void emit_addw(IRBlock& b, uint16_t dest, uint16_t src1,
                      uint16_t src2, SimdSubopParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_addw(dest, src1, src2, p, arm_pc));
}
inline void emit_addhn(IRBlock& b, uint16_t dest, uint16_t src1,
                       uint16_t src2, SimdSubopParams p, uint64_t arm_pc) {
    b.insts.push_back(IRInst::make_addhn(dest, src1, src2, p, arm_pc));
}
// Emit an immediate into a fresh vreg.
inline uint16_t load_imm(IRBlock& b, uint64_t val) {
    uint16_t v = g_alloc.alloc();
    b.insts.push_back(IRInst::make_imm(v, val));
    return v;
}
// Read an ARM64 reg into a fresh vreg.
// `is_sp` controls the reg-31 mapping:
//   - For ADD/SUB immediate: reg 31 = SP (is_sp=true)
//   - For load/store data regs: reg 31 = XZR (is_sp=false)
//   - For data processing (logical/shift): reg 31 = XZR (is_sp=false)
inline uint16_t load_arm_reg(IRBlock& b, uint8_t ar, bool is_sp = false) {
    if (ar == 31 && !is_sp) ar = 32;  // XZR
    // XZR (vreg 32) is always 0 — emit IMM 0 instead of LOAD_REG.
    // Eliminates a memory read and lets the constant folder propagate it.
    if (ar == 32) {
        return load_imm(b, 0);
    }
    uint16_t v = g_alloc.alloc();
    b.insts.push_back(IRInst::make_load_reg(v, ar, false));
    return v;
}
// Write a vreg to an ARM64 reg.
// `is_sp` controls reg-31 mapping (same as load_arm_reg).
inline void store_arm_reg(IRBlock& b, uint8_t ar, uint8_t v, bool is_sp = false) {
    if (ar == 31 && !is_sp) return;  // XZR — discard
    b.insts.push_back(IRInst::make_store_reg(ar, v, false));
}
// ── FP register load/store ────────────────────────────────────────────
// cpu.regs[]. The old LOAD_REG/STORE_REG always accessed cpu.regs[],
// so FP loads/stores via LDR/STR wrote to the wrong array. This caused
// all 32-bit FP loads from memory (e.g., `ldr s0, [x1]` for a global
// float variable) to read 0.0 under the JIT.
//
// Fix: use the `sf` field as an `is_fp` flag on LOAD_REG/STORE_REG.
// When is_fp=1, the JIT and IR executor access cpu.v_lo[] instead of
// cpu.regs[]. This is only used for LDR/STR with d.is_vec=true —
// all other FP ops (SCVTF, FCVT, FADD, etc.) access v_lo directly
// via their own codegen.
inline uint16_t load_fp_reg(IRBlock& b, uint8_t ar) {
    if (ar == 31) ar = 32;  // same mapping as GPR
    if (ar == 32) return load_imm(b, 0);
    uint16_t v = g_alloc.alloc();
    b.insts.push_back(IRInst::make_load_reg(v, ar, true));
    return v;
}
inline void store_fp_reg(IRBlock& b, uint8_t ar, uint8_t v) {
    if (ar == 31) return;  // XZR — discard
    b.insts.push_back(IRInst::make_store_reg(ar, v, true));
}
// Zero-extend a value to 32 bits (sf=0) or pass-through (sf=1).
// We always emit the op and keep it: the ZEXT-removal peephole is
// currently DISABLED (ir_optimize.cpp Pass 3) because codegen uses
// 64-bit ops that don't zero-extend.
inline uint16_t zext_if_32bit(IRBlock& b, uint16_t v, bool sf) {
    if (sf) return v;
    uint16_t r = g_alloc.alloc();
    emit_zext(b, r, v, 32);
    return r;
}
// Apply an extend operation (UXTB/SXTB/UXTH/SXTH/UXTW/SXTW/UXTX/SXTX) to
// a vreg, returning a new vreg holding the result. Used by ADD_REG/SUB_REG,
// ADDS_REG/SUBS_REG, and LDR_REG/STR_REG for the extended-register form.
// extend: bits[2:0] = extend type (0=UXTB..7=SXTX).
// shift:  shift amount to apply after extend (0-4 for extended form).
inline uint16_t apply_extend(IRBlock& b, uint16_t v, uint8_t extend, uint8_t shift) {
    switch (extend & 7) {
        case 0: { // UXTB
            uint16_t m = load_imm(b, 0xFF);
            uint16_t r = g_alloc.alloc();
            b.insts.push_back(IRInst::make(IROp::AND, r, v, m));
            v = r;
            break;
        }
        case 1: { // UXTH
            uint16_t m = load_imm(b, 0xFFFF);
            uint16_t r = g_alloc.alloc();
            b.insts.push_back(IRInst::make(IROp::AND, r, v, m));
            v = r;
            break;
        }
        case 2: { // UXTW
            uint16_t m = load_imm(b, 0xFFFFFFFF);
            uint16_t r = g_alloc.alloc();
            b.insts.push_back(IRInst::make(IROp::AND, r, v, m));
            v = r;
            break;
        }
        case 3: break; // UXTX — no extend
        case 4: { // SXTB
            uint16_t s = g_alloc.alloc();
            emit_sext(b, s, v, 8);
            v = s;
            break;
        }
        case 5: { // SXTH
            uint16_t s = g_alloc.alloc();
            emit_sext(b, s, v, 16);
            v = s;
            break;
        }
        case 6: { // SXTW
            uint16_t s = g_alloc.alloc();
            emit_sext(b, s, v, 32);
            v = s;
            break;
        }
        case 7: break; // SXTX — no extend
    }
    if (shift != 0) {
        uint16_t sh = load_imm(b, static_cast<uint64_t>(shift));
        uint16_t shifted = g_alloc.alloc();
        b.insts.push_back(IRInst::make(IROp::SHL, shifted, v, sh));
        v = shifted;
    }
    return v;
}
// Apply a shift-type operation (LSL/LSR/ASR/ROR) to a vreg.
// shift_type: 0=LSL, 1=LSR, 2=ASR, 3=ROR.
// shift:       shift amount.
// sf:          0 = 32-bit, 1 = 64-bit. For 32-bit ASR, the operand is
//              sign-extended from bit 31 before the SAR, because the
//              JIT's SAR uses x86's 64-bit sar which looks at bit 63.
inline uint16_t apply_shift(IRBlock& b, uint16_t v, uint8_t shift_type, uint8_t shift, bool sf = true) {
    if (shift == 0 && shift_type == 0) return v;
    // 32-bit ASR: sign-extend first so the 64-bit SAR sees the sign bit.
    if (shift_type == 2 && !sf) {
        uint16_t sext = g_alloc.alloc();
        emit_sext(b, sext, v, 32);
        v = sext;
    }
    uint16_t sh = load_imm(b, static_cast<uint64_t>(shift));
    uint16_t shifted = g_alloc.alloc();
    IROp shop = (shift_type == 0) ? IROp::SHL
              : (shift_type == 1) ? IROp::SHR
              : (shift_type == 2) ? IROp::SAR
              : IROp::ROR;
    // For 32-bit ROR: set width=32 so the JIT uses a 32-bit rotation
    // (64-bit ROR on a zero-extended 32-bit value loses wrap bits).
    uint8_t width = (shift_type == 3 && !sf) ? 32 : 0;
    emit_gpr_shift(b, shop, shifted, v, sh, GprShiftParams{width});
    return shifted;
}
// ── SWAR lowering helpers (defined in ir_lower.cpp) ─────────────────────
// Decompose RBIT/REV16/REV32 into primitive IR ops (AND/OR/SHL/SHR) so
// the JIT can compile them natively instead of falling back to CALL_INTERP.
uint16_t swar_swap(IRBlock& b, uint16_t v, uint64_t mask, int n);
uint16_t rbit64_ir(IRBlock& b, uint16_t v);
uint16_t rbit32_ir(IRBlock& b, uint16_t v);
uint16_t rev16_64_ir(IRBlock& b, uint16_t v);
uint16_t rev32_64_ir(IRBlock& b, uint16_t v);
// ── Translator dispatch helpers (defined in ir_translate_*.cpp) ─────────
// translate_to_ir() delegates FP/SIMD and memory load/store cases to these
// helpers. Each returns `true` if it handled `d.cls` (in which case
// translate_to_ir returns false — none of the extracted cases terminate a
// block), or `false` to let translate_to_ir's own switch handle the case.
//
// Split out so the main translate_to_ir() function stays under ~1000 lines;
// see ir_translate_fp.cpp and ir_translate_mem.cpp for the bodies.
bool translate_fp(IRBlock& block, const DecodedInst& d, uint64_t cur_pc);
bool translate_mem(IRBlock& block, const DecodedInst& d, uint64_t cur_pc);
} // namespace arm64emu
