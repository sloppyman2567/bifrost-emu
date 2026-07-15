// ir_translate_fp.cpp — FP/SIMD translation cases for the ARM64 → IR
// translator.
//
// Extracted from ir_translate.cpp to keep that file under 1000 lines.
// translate_to_ir() delegates InstClass::FP_SCALAR, FMOV_VD1/RVD1,
// SIMD_LOGICAL, SIMD_DUP, SIMD_LD1/ST1, and SIMD_DP to translate_fp().
//
// All cases handled here are non-terminating (none of them set
// `block.ends_with_branch` or return true from translate_to_ir), so
// translate_fp() returns `true` (= "handled") to signal that
// translate_to_ir() should itself return `false` (= "block continues").
// Returning `false` from translate_fp() means "InstClass not handled
// here; let the main switch in ir_translate.cpp deal with it".
//
// See ir_translate.cpp for the header comment covering translator-wide
// design rules (vreg mapping, ZEXT-after-32-bit-ops, etc.).
#include "ir/ir.h"        // emit/load_imm/swar helpers + g_alloc
#include "ir/ir.hpp"      // public IR types
#include "core/emulator.h"  // for cond_true() (used by executor only)
namespace arm64emu {
// Returns `true` if `d.cls` was one of the FP/SIMD cases handled here
// (in which case translate_to_ir() returns `false` — none of the
// extracted cases terminate a block). Returns `false` to let the caller
// handle the InstClass itself.
bool translate_fp(IRBlock& block, const DecodedInst& d, uint64_t cur_pc) {
    switch (d.cls) {
        case InstClass::FMOV_VD1: {
            // FMOV Vd.D[1], Rn → v_hi[Vd] = regs[Rn]
            uint16_t val = load_arm_reg(block, d.rn);
            emit(block, IROp::FMOV_G2FHI, d.rd, val, 0, 0, 0, 0, 0, cur_pc);
            return true;
        }
        case InstClass::FMOV_RVD1: {
            // FMOV Rn, Vm.D[1] → regs[Rn] = v_hi[Vm]
            uint16_t v = g_alloc.alloc();
            emit(block, IROp::FMOV_FHI2G, v, d.rn, 0, 0, 0, 0, 0, cur_pc);
            store_arm_reg(block, d.rd, v);
            return true;
        }
        // ── FP_SCALAR — native FP arithmetic ────────────────────────
        // Decode specific FP op from raw bits and emit native IR ops.
        // Falls back to CALL_INTERP for ops we don't handle natively.
        case InstClass::FP_SCALAR: {
            uint32_t op = d.raw;
            uint8_t ftype = (op >> 22) & 3;
            uint8_t rd = op & 0x1F;
            uint8_t rn = (op >> 5) & 0x1F;
            uint8_t rm = (op >> 16) & 0x1F;
            uint8_t opcode = (op >> 12) & 0xF;
            // FMOV (general ↔ FP, 64-bit): may reach here via FP_SCALAR.
            // Bit[18]=1 distinguishes FMOV from SCVTF/UCVTF (bit[18]=0).
            if ((op & 0xFFE0FC00) == 0x9E600000 && (op & (1u << 18))) {
                bool to_fp = (op >> 16) & 1;
                if (to_fp) {
                    uint16_t val = load_arm_reg(block, rn);
                    emit(block, IROp::FMOV_G2F, rd, val, 0, 0, 0, 0, 0, cur_pc);
                } else {
                    uint16_t v = g_alloc.alloc();
                    emit(block, IROp::FMOV_F2G, v, rn, 0, 0, 0, 0, 0, cur_pc);
                    store_arm_reg(block, rd, v);
                }
                return true;
            }
            // FMOV (general ↔ FP, 32-bit): native path.
            // Encoding: 0x1E200000 with bit 16 = to_fp (1) or to_gpr (0).
            // Bit[18]=1 distinguishes FMOV from SCVTF/UCVTF (bit[18]=0),
            // mirroring the 64-bit check above. Without this guard,
            // `scvtf s0, w0` (0x1E220000) and `ucvtf s0, w0` (0x1E230000)
            // match this mask and get misdecoded as a raw GPR↔FP bit copy.
            // FMOV Sn, Wn → v_lo[rd] = (uint32_t)regs[rn]; v_hi[rd] = 0
            // FMOV Wd, Sn → regs[rd] = (uint32_t)v_lo[rn]
            if ((op & 0xFFE0FC00) == 0x1E200000 && (op & (1u << 18))) {
                bool to_fp = (op >> 16) & 1;
                if (to_fp) {
                    // Wn → Sn: mask GPR to 32 bits before storing to v_lo[rd].
                    uint16_t val = load_arm_reg(block, rn);
                    uint16_t mask = load_imm(block, 0xFFFFFFFFULL);
                    uint16_t masked = g_alloc.alloc();
                    emit(block, IROp::AND, masked, val, mask);
                    emit(block, IROp::FMOV_G2F, rd, masked, 0, 0, 0, 0, 0, cur_pc);
                } else {
                    // Sn → Wd: load v_lo[rn] (full 64 bits), mask to 32 bits.
                    uint16_t v = g_alloc.alloc();
                    emit(block, IROp::FMOV_F2G, v, rn, 0, 0, 0, 0, 0, cur_pc);
                    uint16_t mask = load_imm(block, 0xFFFFFFFFULL);
                    uint16_t masked = g_alloc.alloc();
                    emit(block, IROp::AND, masked, v, mask);
                    store_arm_reg(block, rd, masked);
                }
                return true;
            }
            // FMOV (FP↔FP register): native path.
            // Encoding: 0x1E604000 (double) or 0x1E204000 (single).
            // FMOV Dd, Dn → v_lo[rd] = v_lo[rn]; v_hi[rd] = v_hi[rn]
            // FMOV Sd, Sn → v_lo[rd] = v_lo[rn] (low 32 bits); v_hi[rd] = 0
            // We use FMOV_G2F/F2G via a GPR scratch to avoid adding a new IR op.
            // For double: load v_lo[rn] into GPR, store to v_lo[rd]; same for v_hi.
            // For single: load v_lo[rn] into GPR, mask to 32 bits, store to v_lo[rd]; v_hi[rd] = 0.
            if ((op & 0xFFFFFC00) == 0x1E604000) {
                // Double-precision FP register move.
                uint16_t lo = g_alloc.alloc();
                emit(block, IROp::FMOV_F2G, lo, rn, 0, 0, 0, 0, 0, cur_pc);
                emit(block, IROp::FMOV_G2F, rd, lo, 0, 0, 0, 0, 0, cur_pc);
                uint16_t hi = g_alloc.alloc();
                emit(block, IROp::FMOV_FHI2G, hi, rn, 0, 0, 0, 0, 0, cur_pc);
                emit(block, IROp::FMOV_G2FHI, rd, hi, 0, 0, 0, 0, 0, cur_pc);
                return true;
            }
            if ((op & 0xFFFFFC00) == 0x1E204000) {
                // Single-precision FP register move (zeroes upper bits).
                uint16_t lo = g_alloc.alloc();
                emit(block, IROp::FMOV_F2G, lo, rn, 0, 0, 0, 0, 0, cur_pc);
                uint16_t mask = load_imm(block, 0xFFFFFFFFULL);
                uint16_t masked = g_alloc.alloc();
                emit(block, IROp::AND, masked, lo, mask);
                emit(block, IROp::FMOV_G2F, rd, masked, 0, 0, 0, 0, 0, cur_pc);
                return true;
            }
            // FCMP/FCMPE — uses shared fp_decode helper.
            //
            // The #0.0 form vs register form is distinguished by bits[4:0]:
            //   #0.0 form:    bits[4:0] = 0b01000
            //   register form: bits[4:0] = 0b00000, rm in bits[20:16]
            //
            // We pass src2 = rm for the register form (including rm == 0,
            // which means "compare against d0"), and src2 = 0 plus a
            // sentinel bit in the `imm` field (bit 0) for the #0.0 form.
            // The JIT checks `inst.imm & 1` to distinguish the two cases;
            // without this sentinel, FCMP Dn, D0 would be confused with
            // FCMP Dn, #0.0 because both have IR src2 == 0.
            if (fp_decode::is_fcmp(op)) {
                bool with_zero = fp_decode::fcmp_with_zero(op);
                if (with_zero) {
                    emit(block, IROp::FP_CMP, 0, rn, 0, ftype,
                         0, 0, /*imm=*/1, cur_pc);
                } else {
                    emit(block, IROp::FP_CMP, 0, rn, rm, ftype,
                         0, 0, /*imm=*/0, cur_pc);
                }
                return true;
            }
            // FP arithmetic (2-source): bit[21]=1, bits[11:10]=0b10,
            // AND bits[31:24]=0x1E (NOT 0x1F — that's the FMA encoding space).
            //
            // The ARM ARM distinguishes FP 2-source (FMUL/FADD/etc.) from
            // FMA (FMADD/FMSUB/FNMADD/FNMSUB) by bits[31:24]:
            //   0x1E = FP 2-source (FADD/FSUB/FMUL/FDIV/FMAX/FMIN/FNMUL)
            //   0x1F = FMA 3-source (FMADD/FMSUB/FNMADD/FNMSUB)
            //
            // Both have bit[21]=1. The bits[11:10]=0b10 check was meant to
            // distinguish 2-source from FCVTZS/SCVTF (which have
            // bits[11:10]=0b00). But it ALSO matches FMA instructions when
            // Ra's low 2 bits happen to be 0b10 (e.g. Ra=2, 6, 10, ...).
            // This caused FNMADD with Ra=2 to be misdecoded as FP_BINOP
            // (FMUL), silently producing a*b instead of -a*b+c.
            //
            // The fix: also require bits[31:24]=0x1E, excluding the FMA
            // encoding space (0x1F). FMA instructions are handled by the
            // dedicated check further below.
            //
            // The old check only excluded bits[15:10] in {0x04, 0x08, 0x10,
            // 0x14} (FMOV imm / FCMP / FP 1-source / FMOV imm alternate) —
            // but those all have bit[21]=0, so the bit[21]=1 check already
            // excluded them. The real collision was with FCVTZS/SCVTF,
            // which have bit[21]=1 AND bits[15:10]=0x00, matching the old
            // check and causing `fcvtzs w1, d0` (0x1e780001) to be
            // misdecoded as `fmul d1, d0, d24`. The fix is to require
            // bits[11:10]=0b10, which is the architectural encoding for
            // 2-source ops.
            if (((op >> 21) & 1) == 1 && ((op >> 10) & 0x3) == 0b10
                && (op & 0xFF000000) == 0x1E000000) {
                // FADD=0x2, FSUB=0x3, FMUL=0x0, FDIV=0x1, FMAX=0x4, FMIN=0x5,
                // FMAXNM=0x6, FMINNM=0x7, FNMUL=0x8.
                // BUGFIX: 0x6 was labeled FNMUL (actually FMAXNM); 0x7
                // (FMINNM) and 0x8 (FNMUL) were missing.
                if (opcode <= 8 && ftype <= 1) {
                    emit(block, IROp::FP_BINOP, rd, rn, rm, ftype, 0, 0, opcode, cur_pc);
                    return true;
                }
            }
            // FP 1-source: uses shared fp_decode helper.
            // FABS=1, FNEG=2, FSQRT=3. FRINT* (4+) is handled by the
            // dedicated FRINT block further below.
            if (fp_decode::is_fp_1source(op)) {
                uint8_t fp1_opcode = fp_decode::fp_1source_opcode(op);
                if (fp1_opcode >= 1 && fp1_opcode <= 3 && ftype <= 1) {
                    emit(block, IROp::FP_UNOP, rd, rn, 0, ftype,
                         0, 0, fp1_opcode, cur_pc);
                    return true;
                }
                // FRINT* (opcode 0x08-0x0F): handle natively in JIT.
                //
                // to the FRINT IR op — do NOT use load_arm_reg/store_arm_reg.
                // Those helpers allocate vregs (indices >= 33) and emit
                // LOAD_REG/STORE_REG which access cpu.regs[] (GPR array),
                // not cpu.v_lo[] (FP array). The JIT codegen and IR executor
                // both treat inst.dest/inst.src1 as ARM FP reg indices
                // (0-31) and access V_LO_OFF + idx*8 — same convention as
                // FP_BINOP/FP_UNOP. Passing vregs caused out-of-bounds
                // writes to v_lo[33+] and broke floor/ceil/round/trunc
                // under JIT. The masked this by falling back
                // to CALL_INTERP; this is the proper fix.
                //
                // off by one. The actual A64 FRINT opcodes (verified via
                // binutils) are:
                //   0x08=FRINTN, 0x09=FRINTP, 0x0A=FRINTM, 0x0B=FRINTZ,
                //   0x0C=FRINTA, 0x0E=FRINTX, 0x0F=FRINTI  (0x0D unused)
                // wrong. Also, `is_fp_1source` used to reject FRINTA/X/I
                // (bit[17]=1), so they were silently NOP'd — now fixed in
                // decoder.hpp.
                if (fp1_opcode >= 0x08 && fp1_opcode <= 0x0F && ftype <= 1) {
                    uint8_t frint_mode;
                    switch (fp1_opcode) {
                        case 0x08: frint_mode = 0; break;  // FRINTN (nearest)
                        case 0x09: frint_mode = 1; break;  // FRINTP (+inf/ceil)
                        case 0x0A: frint_mode = 2; break;  // FRINTM (-inf/floor)
                        case 0x0B: frint_mode = 3; break;  // FRINTZ (truncate)
                        case 0x0C: frint_mode = 4; break;  // FRINTA (FPCR)
                        case 0x0E: frint_mode = 5; break;  // FRINTX (FPCR+inexact)
                        case 0x0F: frint_mode = 4; break;  // FRINTI (FPCR)
                        default: frint_mode = 0; break;    // 0x0D unused
                    }
                    // Pass rd (dest) and rn (src1) as ARM FP reg indices.
                    // The JIT reads/writes V_LO_OFF + idx*8 directly.
                    emit(block, IROp::FRINT, rd, rn, 0, ftype ? 64 : 32,
                         0, 0, frint_mode, cur_pc);
                    return true;
                }
            }
            // FMOV (scalar, immediate) — uses shared fp_decode helper.
            //
            // MUST be checked BEFORE FCVTZS/SCVTF — the SCVTF mask
            // 0x7F3F0000 also matches FMOV imm (since both have bit[21]=1
            // and similar high bits), causing FMOV imm to be misdecoded
            // as SCVTF (int→FP conversion). This bug caused `fmov d1, #5.0`
            // to be treated as `scvtf d1, x0` (reading garbage from x0),
            // which made every subsequent FP comparison against an
            // immediate-loaded register fail.
            if (fp_decode::is_fmov_imm(op)) {
                uint8_t imm8 = (op >> 13) & 0xFF;
                uint64_t bits = fp_decode::vfp_expand_imm(imm8, ftype);
                emit(block, IROp::FP_MOVI, rd, 0, 0, ftype, 0, 0, bits, cur_pc);
                return true;
            }
            // FCVTZS/FCVTZU: FP→int (toward zero)
            // Encoding: (op & 0x7F3E0000) == 0x1E380000, rmode=3 (toward zero)
            // Mask 0x7F3E0000 excludes bit 16 (U/S selector) so both
            // FCVTZS and FCVTZU match. The previous mask 0x7F3F0000
            // included bit 16, so FCVTZU fell through to CALL_INTERP
            // (interpreter) which has the same mask bug — resulting in
            // a silent NOP for every unsigned float→int conversion.
            // We pass sf (bit 31 of the opcode) via flags_op so the JIT
            // can choose between 32-bit and 64-bit CVTTSD2SI.
            //
            // For 32-bit dest (sf=0), the JIT's CVTTSD2SI produces a
            // 64-bit result. AArch64 32-bit register writes must zero
            // the upper 32 bits — otherwise a subsequent 64-bit read of
            // Xd would see sign-extension instead of zero-extension,
            // breaking code that reuses the register as a 64-bit value.
            // We emit a ZEXT after FP_F2I when sf=0 to enforce this.
            if ((op & 0x7F3E0000) == 0x1E380000) {
                bool is_unsigned = (op >> 16) & 1;
                uint8_t sf = (op >> 31) & 1;
                if (ftype <= 1) {
                    // For 32-bit dest (sf=0), the JIT's CVTTSD2SI produces a
                    // 64-bit result. AArch64 32-bit register writes must zero
                    // the upper 32 bits — otherwise a subsequent 64-bit read
                    // of Xd would see sign-extension instead of zero-extension,
                    // and a cbz/cbnz w0 test on the 32-bit result could see
                    // stale high bits from a previous computation. We emit a
                    // ZEXT after FP_F2I when sf=0 to enforce this.
                    uint16_t tmp = g_alloc.alloc();
                    emit(block, IROp::FP_F2I, tmp, rn, 0, ftype, 0,
                         sf, is_unsigned, cur_pc);
                    if (!sf) {
                        uint16_t z = g_alloc.alloc();
                        emit(block, IROp::ZEXT, z, tmp, 0, 32);
                        store_arm_reg(block, rd, z);
                    } else {
                        store_arm_reg(block, rd, tmp);
                    }
                    return true;
                }
            }
            // FCVTZS/FCVTZU (fixed-point variant): scale FP value by 2^fbits
            // then convert to integer with truncation toward zero, saturating
            // to the destination's signed/unsigned range on overflow. The 6-bit
            // scale at bits[15:10] gives fbits = 64 - scale.
            //
            // Native IR op FP_F2I_FIXED is emitted here; the JIT codegen
            // (src/jit/frostjit.cpp) implements the saturating scaled
            // truncation directly in x86. The codegen was fixed to properly
            // flush XMM0/XMM1's prior contents before loading the operands
            // (the previous "first FCVTZU produces 0" bug was caused by
            // stale XMM state from a prior FP op not being cleared).
            if ((op & 0x7F3E0000) == 0x1E180000) {
                bool is_unsigned = (op >> 16) & 1;
                uint8_t sf = (op >> 31) & 1;
                uint8_t scale = (op >> 10) & 0x3F;
                uint8_t fbits = 64 - scale;
                if (ftype <= 1) {
                    // For 32-bit dest (sf=0), emit ZEXT to zero upper bits.
                    uint16_t tmp = g_alloc.alloc();
                    emit(block, IROp::FP_F2I_FIXED, tmp, rn, 0, ftype, 0,
                         sf, is_unsigned, cur_pc);
                    // Patch immr via the IRInst — the emit() helper doesn't
                    // expose immr directly; set it on the just-pushed inst.
                    block.insts.back().immr = fbits;
                    if (!sf) {
                        uint16_t z = g_alloc.alloc();
                        emit(block, IROp::ZEXT, z, tmp, 0, 32);
                        store_arm_reg(block, rd, z);
                    } else {
                        store_arm_reg(block, rd, tmp);
                    }
                    return true;
                }
                // ftype=3 (half) → fall through to CALL_INTERP.
                emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
                return true;
            }
            // FCVT (float ↔ double conversion).
            // Encoding: 0x1E624000 (D→S) or 0x1E22C000 (S→D).
            // SCVTF mask 0x7F3E0000 also matches FCVT (0x1E22C000 &
            // 0x7F3E0000 == 0x1E220000 == SCVTF mask), causing FCVT to be
            // misidentified as SCVTF (int→FP) and emitted as FP_I2F instead
            // of FCVT_S2D/FCVT_D2S. This was the root cause of all 32-bit
            // FP loads from memory reading as 0.0 under the JIT: the LDR S0
            // loaded the float correctly, but the subsequent FCVT D0,S0 was
            // treated as SCVTF D0,X0 (reading garbage from x0 instead of
            // the float in v0).
            if ((op & 0xFFFFFC00) == 0x1E624000) {
                // FCVT Sd, Dn (double → single)
                emit(block, IROp::FCVT_D2S, rd, rn, 0, 0, 0, 0, 0, cur_pc);
                return true;
            }
            if ((op & 0xFFFFFC00) == 0x1E22C000) {
                // FCVT Dd, Sn (single → double)
                emit(block, IROp::FCVT_S2D, rd, rn, 0, 0, 0, 0, 0, cur_pc);
                return true;
            }
            // SCVTF/UCVTF: int→FP
            // Encoding: (op & 0x7F3E0000) == 0x1E220000
            // Mask 0x7F3E0000 excludes bit 16 so both SCVTF (bit 16=0)
            // and UCVTF (bit 16=1) match. The previous mask 0x7F3F0000
            // included bit 16, so UCVTF (0x1E230000) did NOT match
            // 0x1E220000 and was silently NOP'd.
            // We pass sf (bit 31) via flags_op so the JIT can choose
            // between 32-bit (CVTSI2SS eax) and 64-bit (CVTSI2SS rax)
            // source forms. Without this, `scvtf s0, w0` with w0=-1
            // would convert 0x00000000FFFFFFFF (4294967295) instead of
            // -1, producing 4.29e+09 instead of -1.0f.
            // Checked AFTER FMOV imm (which has a tighter mask and must
            // match first to avoid collision).
            if ((op & 0x7F3E0000) == 0x1E220000) {
                bool is_unsigned = (op >> 16) & 1;
                uint8_t sf = (op >> 31) & 1;
                if (ftype <= 1) {
                    emit(block, IROp::FP_I2F, rd, rn, 0, ftype, 0,
                         sf, is_unsigned, cur_pc);
                    return true;
                }
            }
            // SCVTF/UCVTF (fixed-point variant): convert integer to FP and
            // divide by 2^fbits (fbits = 64 - scale). Native IR op
            // FP_I2F_FIXED is emitted; the JIT codegen reuses the integer-
            // variant FP_I2F codegen then multiplies by 2^-fbits.
            if ((op & 0x7F3E0000) == 0x1E020000) {
                bool is_unsigned = (op >> 16) & 1;
                uint8_t sf = (op >> 31) & 1;
                uint8_t scale = (op >> 10) & 0x3F;
                uint8_t fbits = 64 - scale;
                if (ftype <= 1) {
                    emit(block, IROp::FP_I2F_FIXED, rd, rn, 0, ftype, 0,
                         sf, is_unsigned, cur_pc);
                    block.insts.back().immr = fbits;
                    return true;
                }
                // ftype=3 (half) → fall through to CALL_INTERP.
                emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
                return true;
            }
            // FMADD/FMSUB/FNMADD/FNMSUB (FP fused multiply-add/subtract).
            // Encoding: (op & 0xFF000000) == 0x1F000000.
            //   bit 15 (o1): 0 = ADD-form, 1 = SUB-form
            //   bit 21 (o2): 0 = positive product, 1 = negative product
            // ra = bits[14:10]. Operands: a=Vn, b=Vm, c=Va.
            //
            //   o2=0, o1=0: FMADD  → dest = a*b + c
            //   o2=0, o1=1: FMSUB  → dest = c - a*b  (= -a*b + c)
            //   o2=1, o1=0: FNMADD → dest = -a*b + c  (numerically same as
            //                                        FMSUB but with different
            //                                        IEEE 754 sign rules on
            //                                        NaN/signed-zero inputs)
            //   o2=1, o1=1: FNMSUB → dest = -a*b - c  (= -(a*b + c))
            //
            // which silently dropped FNMADD/FNMSUB (o2=1) — they fell
            // through to the "Unknown FP instruction — NOP" path in the
            // interpreter, returning whatever was already in Vd. This
            // broke any guest program that used FNMADD/FNMSUB (e.g.
            // musl's __muldf3 fallback for long double).
            if ((op & 0xFF000000) == 0x1F000000) {
                uint8_t ra = (op >> 10) & 0x1F;
                bool sub = (op >> 15) & 1;   // o1
                bool neg = (op >> 21) & 1;   // o2
                if (ftype <= 1) {
                    IROp op_e;
                    if      (!neg && !sub) op_e = IROp::FMADD;
                    else if (!neg &&  sub) op_e = IROp::FMSUB;
                    else if ( neg && !sub) op_e = IROp::FNMADD;
                    else                   op_e = IROp::FNMSUB;
                    // Pass FP register indices directly — the JIT reads
                    // operands from V_LO_OFF + idx*8. Do NOT use
                    // load_arm_reg (that loads GPRs, not FP regs).
                    emit(block, op_e,
                         rd, rn, rm, ftype ? 64 : 32, 0, 0,
                         static_cast<uint64_t>(ra), cur_pc);
                    return true;
                }
            }
            // FCVT check was moved above SCVTF.
            // The old FCVT check here is removed — it was unreachable because
            // the SCVTF mask caught FCVT first.
            // FRINT* is now handled earlier (in the is_fp_1source block above).
            // The old FRINT block here used a mask that only matched FRINTN.
            // FCSEL (FP conditional select).
            // Encoding: (op & 0xFF200C00) == 0x1E200C00, cond in bits[15:12].
            // FCSEL Sd/Dd, Sn, Sm, cond → if cond: dest = n else dest = m.
            // We emit a CSEL-like sequence via a GPR scratch + CSEL IR op,
            // since we don't have a native FP_CSEL IR op. But actually we
            // can do this natively by loading both FP values into GPRs and
            // using CSEL.
            if ((op & 0xFF200C00) == 0x1E200C00 && ftype <= 1) {
                uint8_t cond = (op >> 12) & 0xF;
                // Load both FP values into GPR scratch vregs.
                uint16_t val_n = g_alloc.alloc();
                emit(block, IROp::FMOV_F2G, val_n, rn, 0, 0, 0, 0, 0, cur_pc);
                uint16_t val_m = g_alloc.alloc();
                emit(block, IROp::FMOV_F2G, val_m, rm, 0, 0, 0, 0, 0, cur_pc);
                // CSEL between the two GPR values.
                uint16_t selected = g_alloc.alloc();
                emit(block, IROp::CSEL, selected, val_n, val_m, 0, cond, 0, 0, cur_pc);
                // Store the selected value back to v_lo[rd].
                emit(block, IROp::FMOV_G2F, rd, selected, 0, 0, 0, 0, 0, cur_pc);
                // FMOV_G2F zeroes v_hi[rd] per ARM semantics, so no extra
                // zero-store is needed for single-precision (ftype == 0).
                return true;
            }
            // Everything else (rare FP ops) falls back to interpreter.
            emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
            return true;
        }
        // ── SIMD LOGICAL (AND/ORR/EOR/BIC/ORN/EON) — native ────────
        case InstClass::SIMD_LOGICAL: {
            uint32_t op = d.raw;
            uint8_t opcode = (op >> 12) & 0xF;
            // ARM SIMD logical opcodes: AND=3, BIC=0, ORR=1, ORN=2,
            //                           EOR=7, EON=6, BIF=8, BIT=9, BSL=10
            // Map to our SIMD_LOGICAL imm: 0=and,1=orr,2=xor,3=bic,4=orn,5=eon
            uint8_t simd_op;
            switch (opcode) {
                case 0x3: simd_op = 0; break; // AND
                case 0x1: simd_op = 1; break; // ORR
                case 0x7: simd_op = 2; break; // EOR
                case 0x0: simd_op = 3; break; // BIC
                case 0x2: simd_op = 4; break; // ORN
                case 0x6: simd_op = 5; break; // EON
                default:
                    emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
                    return true;
            }
            emit(block, IROp::SIMD_LOGICAL, d.rd, d.rn, d.rm, 0, 0, 0, simd_op, cur_pc);
            return true;
        }
        // ── SIMD DUP — native ──────────────────────────────────────
        case InstClass::SIMD_DUP: {
            // dup Vd.2d, Rn → broadcast Rn to both halves
            uint16_t val = load_arm_reg(block, d.rn);
            emit(block, IROp::SIMD_DUP, d.rd, val, 0, 0, 0, 0, 0, cur_pc);
            return true;
        }
        // ── SIMD LD1/ST1 — native (128-bit load/store) ─────────────
        // Handles multi-register forms: LD1/ST1 {Vt..Vt+n-1} stores n×16
        // bytes (n = d.simd_count, 1..4). Each register's v_lo and v_hi
        // are loaded/stored as two 8-byte memory accesses.
        case InstClass::SIMD_LD1: {
            uint16_t base = load_arm_reg(block, d.rn, true);
            for (uint8_t i = 0; i < d.simd_count; i++) {
                uint8_t reg = (d.rt + i) & 0x1F;
                uint16_t lo = g_alloc.alloc();
                emit(block, IROp::LOAD_MEM, lo, base, 0, 8, 0, 0,
                     static_cast<uint64_t>(i * 16));
                uint16_t hi = g_alloc.alloc();
                emit(block, IROp::LOAD_MEM, hi, base, 0, 8, 0, 0,
                     static_cast<uint64_t>(i * 16 + 8));
                emit(block, IROp::SIMD_LDST, reg, lo, hi, 1, 0, 0, 0, cur_pc);
            }
            return true;
        }
        case InstClass::SIMD_ST1: {
            uint16_t base = load_arm_reg(block, d.rn, true);
            for (uint8_t i = 0; i < d.simd_count; i++) {
                uint8_t reg = (d.rt + i) & 0x1F;
                uint16_t lo = g_alloc.alloc();
                uint16_t hi = g_alloc.alloc();
                emit(block, IROp::SIMD_LDST, reg, lo, hi, 0, 0, 0, 0, cur_pc);
                emit(block, IROp::STORE_MEM, 0, base, lo, 8, 0, 0,
                     static_cast<uint64_t>(i * 16));
                emit(block, IROp::STORE_MEM, 0, base, hi, 8, 0, 0,
                     static_cast<uint64_t>(i * 16 + 8));
            }
            return true;
        }
        // ── SIMD data-processing (integer add/sub/mul/min/max/cmp) ───
        // Common encodings that we can JIT natively via SIMD_ARITH /
        // SIMD_CMP:
        //   ADD (vector): 0x0E208400  (size 0/1/2/3 = 8/16/32/64-bit)
        //   SUB (vector): 0x2E208400
        //   MUL (vector): 0x0E209C00  (size 0/1/2 = 8/16/32-bit; 64-bit
        //                               not in SSE2)
        //   CMGT (signed >):   0x0E203400  (U=0, opcode=0x34)
        //   CMGE (signed >=):  0x0E203C00  (U=0, opcode=0x3C)
        //   CMEQ (==):         0x2E208C00  (U=1, opcode=0x8C)
        //   CMHI (unsigned >): 0x2E203400  (U=1, opcode=0x34)
        //   CMHS (unsigned >=):0x2E203C00  (U=1, opcode=0x3C)
        // These are the most common SIMD arithmetic and compare ops.
        // Other SIMD_DP encodings fall through to CALL_INTERP.
        case InstClass::SIMD_DP: {
            uint32_t op = d.raw;
            bool Q = (op >> 30) & 1;
            uint8_t size = (op >> 22) & 3;
            uint32_t sub3 = op & 0xFF20FC00;
            uint32_t sub3_noq = sub3 & ~(1u << 30);
            int esize = 1 << size;   // 1, 2, 4, 8
            // ── Arithmetic ops (SIMD_ARITH) ──
            uint8_t arith_op = 0xFF;
            if (sub3_noq == 0x0E208400) {
                arith_op = 0;  // ADD
            } else if (sub3_noq == 0x2E208400) {
                arith_op = 1;  // SUB
            } else if (sub3_noq == 0x0E209C00) {
                arith_op = 2;  // MUL
                if (size == 3) {
                    emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
                    return true;
                }
            }
            if (arith_op != 0xFF) {
                (void)Q;
                emit(block, IROp::SIMD_ARITH, d.rd, d.rn, d.rm, 0,
                     static_cast<uint64_t>(esize), 0, arith_op, cur_pc);
                return true;
            }
            // ── Compare ops (SIMD_CMP) ──
            // SIMD_CMP imm: 0=eq, 1=ge_u, 2=gt_u, 3=ge_s, 4=gt_s,
            //               5=hi_u, 6=hs_u
            uint8_t cmp_op = 0xFF;
            if (sub3_noq == 0x2E208C00) {
                // CMEQ (==): U=1, opcode=0x8C
                cmp_op = 0;  // eq
            }
            if (cmp_op != 0xFF) {
                (void)Q;
                emit(block, IROp::SIMD_CMP, d.rd, d.rn, d.rm, 0,
                     static_cast<uint64_t>(esize), 0, cmp_op, cur_pc);
                return true;
            }
            // ── NOT/MVN (vector) — 0x2E205800 ──
            // NOT Vd.<T>, Vn.<T> = bitwise NOT of all lanes.
            // Encoding: 1 Q 0 1 1 1 1 0 size 1 0000 0 1 0 1 1 0 Rn Rd
            // sub3_noq = 0x2E205800
            if (sub3_noq == 0x2E205800) {
                // Use SIMD_LOGICAL with opc=2 (XOR) and src2=src1
                // to compute NOT: a ^ a = 0, then we need ~a.
                // Actually NOT = a XOR all-ones. We emit CALL_INTERP
                // for now since SIMD_LOGICAL doesn't have a NOT mode.
                // But we can use BIC with src2=src1: a & ~a = 0 (wrong).
                // Let's emit a logical NOT via XOR with all-ones.
                // We don't have an all-ones register, so fall to interp.
                emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
                return true;
            }
            // ── NEG (vector) — 0x2E20B800 ──
            // NEG Vd.<T>, Vn.<T> = 0 - Vn (two's complement negate).
            // This is SUB with src1=0. We can emit SIMD_ARITH sub with
            // a zero src1, but our SIMD_ARITH reads from vregs, not XZR.
            // Fall to interp for now.
            if (sub3_noq == 0x2E20B800) {
                emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
                return true;
            }
            // ── Vector shift-by-immediate: SHL, USHR, SSHR ──
            // v1.4.5-alpha: native IR ops (AVX2 256-bit on capable hosts,
            // SSE2 128-bit fallback otherwise). USRA/SSRA/SLI/SRI/SHRN
            // still fall to the interpreter (they need an accumulator or
            // narrowing semantics not yet in the IR).
            // Encoding constants (mask 0xBF00FC00, which strips Q):
            //   SHL  0x0F005400   USHR 0x2F000400   SSHR 0x0F000400
            //   USRA 0x2F001400   SSRA 0x0F001400
            //   SLI  0x2F005400   SRI  0x2F004400   SHRN 0x0F008400
            {
                uint32_t sm = op & 0xBF00FC00;
                // Extract element size from immh (bits[23:20]) per ARM ARM.
                // NOTE: bits[23:20] = (op >> 20) & 0xF, NOT (op >> 19) —
                // the interpreter uses (op >> 20) and we must match.
                uint8_t immh = (op >> 20) & 0xF;
                uint8_t immb = (op >> 16) & 0xF;
                uint8_t esize_bytes = 0;  // 1, 2, 4, or 8
                if      (immh == 0) esize_bytes = 1;
                else if (immh == 1) esize_bytes = 2;
                else if (immh == 2 || immh == 3) esize_bytes = 4;
                else if (immh >= 4) esize_bytes = 8;
                if (esize_bytes != 0) {
                    uint32_t shift_amount = 0;
                    IROp shift_op = IROp::NOP;
                    if (sm == 0x0F005400) {  // SHL
                        // SHL: shift = UInt(immh:immb) - esize*8
                        shift_amount = ((immh << 4) | immb) - esize_bytes * 8;
                        shift_op = IROp::SIMD_SHL;
                    } else if (sm == 0x2F000400) {  // USHR
                        // USHR: shift = (2 * esize*8) - UInt(immh:immb)
                        shift_amount = (2 * esize_bytes * 8) - ((immh << 4) | immb);
                        shift_op = IROp::SIMD_USHR;
                    } else if (sm == 0x0F000400) {  // SSHR
                        // SSHR: shift = (2 * esize*8) - UInt(immh:immb)
                        shift_amount = (2 * esize_bytes * 8) - ((immh << 4) | immb);
                        shift_op = IROp::SIMD_SSHR;
                    }
                    if (shift_op != IROp::NOP) {
                        emit(block, shift_op, d.rd, d.rn, 0,
                             esize_bytes, 0, 0, shift_amount, cur_pc);
                        return true;
                    }
                }
                // USRA/SSRA/SLI/SRI/SHRN still fall to interpreter.
                if (sm == 0x2F001400 || sm == 0x0F001400 ||
                    sm == 0x2F005400 || sm == 0x2F004400 ||
                    sm == 0x0F008400) {
                    emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
                    return true;
                }
            }
            // ── v1.5.0.alpha: ARMv8 Crypto Extensions ───────────────
            // AES: 0x4E284800-0x4E287800 (AESE/AESD/AESMC/AESIMC)
            // SHA1H: 0x5E280800
            // SHA1SU1: 0x5E280000
            // SHA256SU0: 0x5E282000
            // PMULL: 0x4E60E000 (size=11, 64-bit poly mul, low half)
            // PMULL2: 0x4EE0E000 (size=11, 64-bit poly mul, high half)
            //
            // We emit AES_CRYPTO IR ops for AESE/AESD/AESMC/AESIMC and
            // PMULL/PMULL2. The JIT's codegen will use AES-NI /
            // PCLMULQDQ when available, or fall back to CALL_INTERP
            // (which calls the interpreter's software table-driven
            // implementation in interp_crypto.hpp).
            {
                uint32_t aes_masked = op & 0xFFFFFC00;
                if (aes_masked == 0x4E284800) {
                    // AESE/AESD/AESMC/AESIMC — imm = (op>>10)&3
                    uint8_t sub_op = static_cast<uint8_t>((op >> 10) & 3);
                    emit(block, IROp::AES_CRYPTO, d.rd, d.rn, 0, 0,
                         0, 0, sub_op, cur_pc);
                    return true;
                }
                // PMULL (size=11, 64-bit poly mul, low half)
                if ((op & 0xFFE0FC00) == 0x4E60E000) {
                    emit(block, IROp::AES_CRYPTO, d.rd, d.rn, d.rm, 0,
                         0, 0, 4, cur_pc);  // sub_op=4 = PMULL
                    return true;
                }
                // PMULL2 (size=11, 64-bit poly mul, high half)
                if ((op & 0xFFE0FC00) == 0x4EE0E000) {
                    emit(block, IROp::AES_CRYPTO, d.rd, d.rn, d.rm, 0,
                         0, 0, 5, cur_pc);  // sub_op=5 = PMULL2
                    return true;
                }
                // SHA1H, SHA1SU1, SHA256SU0 — 2-operand crypto (mask 0xFFFFFC00).
                // SHA1C/SHA1P/SHA1M, SHA1SU0, SHA256H/H2, SHA256SU1 — 3-operand
                // crypto (mask 0xFFE0FC00).
                // All fall back to CALL_INTERP (the interpreter has full
                // implementations in interp_crypto.hpp; native SHA-NI
                // codegen is a future enhancement).
                // Encoding constants verified against binutils:
                //   SHA1H    = 0x5E280800
                //   SHA1SU1  = 0x5E281800  (was 0x5E280000 — wrong)
                //   SHA256SU0= 0x5E282800  (was 0x5E282000 — wrong)
                //   SHA1C    = 0x5E000000  (mask 0xFFE0FC00)
                //   SHA1P    = 0x5E001000  (mask 0xFFE0FC00)
                //   SHA1M    = 0x5E002000  (mask 0xFFE0FC00)
                //   SHA1SU0  = 0x5E003000  (mask 0xFFE0FC00)
                //   SHA256H  = 0x5E004000  (mask 0xFFE0FC00)
                //   SHA256H2 = 0x5E005000  (mask 0xFFE0FC00)
                //   SHA256SU1= 0x5E006000  (mask 0xFFE0FC00)
                if (aes_masked == 0x5E280800 ||  // SHA1H
                    aes_masked == 0x5E281800 ||  // SHA1SU1
                    aes_masked == 0x5E282800) {  // SHA256SU0
                    emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
                    return true;
                }
                if ((op & 0xFFE0FC00) == 0x5E000000 ||  // SHA1C
                    (op & 0xFFE0FC00) == 0x5E001000 ||  // SHA1P
                    (op & 0xFFE0FC00) == 0x5E002000 ||  // SHA1M
                    (op & 0xFFE0FC00) == 0x5E003000 ||  // SHA1SU0
                    (op & 0xFFE0FC00) == 0x5E004000 ||  // SHA256H
                    (op & 0xFFE0FC00) == 0x5E005000 ||  // SHA256H2
                    (op & 0xFFE0FC00) == 0x5E006000) {  // SHA256SU1
                    emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
                    return true;
                }
            }
            // Unrecognized SIMD_DP — fall back to interpreter.
            emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
            return true;
        }
        // (End of SIMD_DP case — the crypto checks below are BEFORE the
        //  fallthrough, in the sub3_noq checks above. If we reach here,
        //  we already emitted CALL_INTERP.)
        default:
            // Not an FP/SIMD case — let the main translator handle it.
            return false;
    }
}
} // namespace arm64emu
