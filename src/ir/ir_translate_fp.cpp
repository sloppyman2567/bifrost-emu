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
#include "opgen_simd.hpp" // generated SIMD_DP decode table (tools/opgen)
#include "opgen_fpfixed.hpp" // generated fixed-point convert table (tools/opgen)
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
            // Bit[17]=1 additionally excludes FCVTAS (0x9E640020, bit17=0),
            // which would otherwise be misdecoded as a raw GPR↔FP bit copy.
            if ((op & 0xFFE0FC00) == 0x9E600000 && (op & (1u << 18))
                && (op & (1u << 17))) {
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
            // Bit[17]=1 additionally excludes FCVTAS (0x1E240000, bit17=0),
            // which would otherwise be misdecoded as a raw GPR↔FP bit copy.
            if ((op & 0xFFE0FC00) == 0x1E200000 && (op & (1u << 18))
                && (op & (1u << 17))) {
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
            // Single FP_MOV IR op (codegen = vmovsd/vmovss between the v_lo/
            // v_hi slots). The old 4-op FMOV_F2G→(IMM/AND)→FMOV_G2F(→FHI2G/
            // G2FHI) round-trip through a GPR scratch cost ~4 memory accesses
            // per fmov — noise3/grad3 emit 67 fmovs per body, so this was a
            // large chunk of the worldgen cost.
            if ((op & 0xFFFFFC00) == 0x1E604000) {
                emit(block, IROp::FP_MOV, rd, rn, 0, ftype, 0, 0, 0, cur_pc);
                return true;
            }
            if ((op & 0xFFFFFC00) == 0x1E204000) {
                emit(block, IROp::FP_MOV, rd, rn, 0, ftype, 0, 0, 0, cur_pc);
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
            // SIMD scalar/vector 2-source FP (0x7E group): FABD.
            // Encoding: bits[31:24]=0x7E, bits[15:12]=0xD (FABD opcode),
            // bits[11:10]=0b01. bit22: 0=single, 1=double.
            // We handle FABD (single/double) natively as FP_BINOP with
            // opcode 0xD. Without this, musl's fabsf(got-want) (which the
            // compiler lowers to `fabd s_, s0, s1`) returns the first
            // operand unchanged, breaking float comparisons.
            if ((op & 0xFF00FC00) == 0x7E00D400) {
                bool is_double = (op >> 22) & 1;
                uint8_t ft = is_double ? 1 : 0;  // IR ftype: 0=S, 1=D
                emit(block, IROp::FP_BINOP, rd, rn, rm, ft, 0, 0, 0xD, cur_pc);
                return true;
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
            // FCVT{N,P,M,Z,A}{S,U}: FP→int with explicit rounding mode.
            // Encoding (integer variant, bit 21 = 1):
            //   0x1E200000 = FCVTNS, 0x1E280000 = FCVTPS,
            //   0x1E300000 = FCVTMS, 0x1E240000 = FCVTAS,
            //   0x1E380000 = FCVTZS/FCVTZU.
            // rmode = bits[20:19]: 00=N(nearest even), 01=P(+inf), 10=M(-inf),
            // 11=Z(toward zero); bit[18]=A (nearest, ties away); bit[16]=U.
            // Mask 0x7F220000 leaves rmode, A and U free so every variant
            // matches (the old mask 0x7F3E0000 == 0x1E380000 only matched
            // FCVTZS/FCVTZU — fcvtms/fcvtps/fcvtns fell back to CALL_INTERP,
            // and floor() compiles to FCVTMS, so Minecraft-style chunk/mesh
            // math ran half in the interpreter). bits[15:10]==0 excludes
            // FCSEL (0x1E200C00) and FCVT D↔S (0x1E6240C0).
            if ((op & 0x7F220000) == 0x1E200000 && ((op >> 10) & 0x3F) == 0) {
                bool is_away = (op >> 18) & 1;
                uint8_t rmode = (op >> 19) & 3;
                bool is_unsigned = (op >> 16) & 1;
                uint8_t sf = (op >> 31) & 1;
                if (ftype <= 1) {
                    // A (ties-away) and unsigned non-Z rounding aren't
                    // native in the JIT codegen yet — keep them correct via
                    // the interpreter rather than mis-rounding.
                    if (is_away || (is_unsigned && rmode != 3)) {
                        emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
                        return true;
                    }
                    // For 32-bit dest (sf=0), the JIT's CVTTSD2SI produces a
                    // 64-bit result. AArch64 32-bit register writes must zero
                    // the upper 32 bits — otherwise a subsequent 64-bit read
                    // of Xd would see sign-extension instead of zero-extension,
                    // and a cbz/cbnz w0 test on the 32-bit result could see
                    // stale high bits from a previous computation. We emit a
                    // ZEXT after FP_F2I when sf=0 to enforce this.
                    uint16_t tmp = g_alloc.alloc();
                    // Rounding mode rides in `cond`: (is_away<<2) | rmode.
                    emit(block, IROp::FP_F2I, tmp, rn, 0, ftype,
                         (is_away << 2) | rmode, sf, is_unsigned, cur_pc);
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
            // ── Fixed-point int↔FP conversions (SCVTF/UCVTF/FCVTZS/FCVTZU #fbits) ──
            // Classified by the generated table (tools/opgen/fp_fixconv.txt →
            // include/opgen_fpfixed.hpp) so interp, IR translator and the JIT
            // gate share one mask set. Subops:
            //   0 = SCVTF/UCVTF int→FP, GPR source (FP_I2F_FIXED, src1 = GPR vreg)
            //   1 = FCVTZS/FCVTZU FP→int, GPR dest (FP_F2I_FIXED, src1 = FP reg)
            //   2 = SCVTF/UCVTF int→FP, FP source+dest (FP_I2F_FIXED, FP-src flag)
            //   3 = FCVTZS/FCVTZU FP→int, FP source+dest (FP_F2I_FIXED, FP-dest flag)
            //
            // For subops 2/3 the source integer lives in an FP register
            // (e.g. GCC's `scvtf s0, s0, #1` in (float)x hit tests) so the
            // JIT codegen must read/write v_lo[] instead of a GPR; that is
            // signalled via inst.imms bit 0.
            {
                fpfixed::Op fc = fpfixed::classify(op);
                if (fc.family == fpfixed::Family::FIXCONV) {
                    if (ftype <= 1) {
                        bool is_unsigned;
                        uint8_t w, sf, fbits;
                        if (fc.subop <= 1) {
                            is_unsigned = (op >> 16) & 1;
                            sf = (op >> 31) & 1;
                            fbits = 64 - ((op >> 10) & 0x3F);
                            w = ftype;
                        } else {
                            is_unsigned = (op >> 29) & 1;
                            sf = (op >> 22) & 1;   // size bit = 64-bit int width
                            fbits = 64 - ((op >> 16) & 0x3F);
                            w = (op >> 22) & 1;
                        }
                        if (fc.subop == 0) {
                            emit(block, IROp::FP_I2F_FIXED, rd, rn, 0, w, 0,
                                 sf, is_unsigned, cur_pc);
                            block.insts.back().immr = fbits;
                            return true;
                        }
                        if (fc.subop == 1) {
                            uint16_t tmp = g_alloc.alloc();
                            emit(block, IROp::FP_F2I_FIXED, tmp, rn, 0, w, 0,
                                 sf, is_unsigned, cur_pc);
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
                        if (fc.subop == 2) {
                            emit(block, IROp::FP_I2F_FIXED, rd, rn, 0, w, 0,
                                 sf, is_unsigned, cur_pc);
                            block.insts.back().immr = fbits;
                            block.insts.back().imms = 1;  // FP register source
                            return true;
                        }
                        // subop 3
                        emit(block, IROp::FP_F2I_FIXED, rd, rn, 0, w, 0,
                             sf, is_unsigned, cur_pc);
                        block.insts.back().immr = fbits;
                        block.insts.back().imms = 1;  // FP register dest
                        return true;
                    }
                    // ftype=3 (half) → fall through to CALL_INTERP.
                    emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
                    return true;
                }
            }
            // ── SIMD scalar int↔FP conversions, FP source/dest (0x5E200800) ──
            // "Advanced SIMD scalar two-register miscellaneous" group
            // (bits[31:24]=0x5E, Q=bit30=1). The integer lives in an FP
            // register (Dn/Sn) instead of a GPR — GCC/clang emit these for
            // `(float)int_var` when the int is already in an FP register
            // (e.g. the voxel game's chunk math does `scvtf sN, sN` on
            // every coordinate). Group mask mirrors interp_fp.cpp:
            //   (op & 0xDF3E0C00) == 0x5E200800
            //   opcode = bits[16:12]: 0x1D = SCVTF/UCVTF (int→FP),
            //                          0x1B = FCVTZS/FCVTZU (FP→int)
            //   size = bit 22 (also = 64-bit int width), U = bit 29.
            // Reuse FP_I2F_FIXED/FP_F2I_FIXED with imms=1 (FP source/dest)
            // and immr=0. In the fixed-point table immr = fbits is always
            // 1..64, so the JIT treats immr==0 as "plain integer form" and
            // skips the 2^fbits scale entirely — the conversion is exact.
            if ((op & 0xDF3E0C00) == 0x5E200800) {
                uint8_t gopc = (op >> 12) & 0x1F;
                bool is_unsigned = (op >> 29) & 1;
                bool is_double = (op >> 22) & 1;
                if (gopc == 0x1D) {  // SCVTF/UCVTF: int bits in FP reg → FP
                    emit(block, IROp::FP_I2F_FIXED, rd, rn, 0, is_double, 0,
                         is_double, is_unsigned, cur_pc);
                    block.insts.back().immr = 0;   // fbits = 0 (integer form)
                    block.insts.back().imms = 1;   // FP register source
                    return true;
                }
                if (gopc == 0x1B) {  // FCVTZS/FCVTZU: FP → int bits in FP reg
                    emit(block, IROp::FP_F2I_FIXED, rd, rn, 0, is_double, 0,
                         is_double, is_unsigned, cur_pc);
                    block.insts.back().immr = 0;   // fbits = 0 (integer form)
                    block.insts.back().imms = 1;   // FP register dest
                    return true;
                }
                // Other opcodes (FRINTN, FABS, …) live in the 0x1E… group,
                // not here — fall through to CALL_INTERP if ever seen.
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
            // Encoding: (op & 0x7F3EFC00) == 0x1E220000
            // Mask 0x7F3EFC00 excludes bit 16 so both SCVTF (bit 16=0)
            // and UCVTF (bit 16=1) match, and requires bits[15:10]==0.
            // The 0xFC00 bits are essential: FCSEL (0x1E220C01, bits[15:10]
            // = 0b0011) matched the old loose mask 0x7F3E0000 and was
            // emitted as FP_I2F (int→FP), converting the hash GPR into an
            // FP register and breaking grad3's `fcsel s1, s0, s2, eq`.
            // The previous mask 0x7F3F0000
            // included bit 16, so UCVTF (0x1E230000) did NOT match
            // 0x1E220000 and was silently NOP'd.
            // We pass sf (bit 31) via flags_op so the JIT can choose
            // between 32-bit (CVTSI2SS eax) and 64-bit (CVTSI2SS rax)
            // source forms. Without this, `scvtf s0, w0` with w0=-1
            // would convert 0x00000000FFFFFFFF (4294967295) instead of
            // -1, producing 4.29e+09 instead of -1.0f.
            // Checked AFTER FMOV imm (which has a tighter mask and must
            // match first to avoid collision).
            if ((op & 0x7F3EFC00) == 0x1E220000) {
                bool is_unsigned = (op >> 16) & 1;
                uint8_t sf = (op >> 31) & 1;
                if (ftype <= 1) {
                    emit(block, IROp::FP_I2F, rd, rn, 0, ftype, 0,
                         sf, is_unsigned, cur_pc);
                    return true;
                }
            }
            // FMADD/FMSUB/FNMADD/FNMSUB (FP fused multiply-add/subtract).
            // Encoding: (op & 0xFF000000) == 0x1F000000.
            //   bit 15 (o1): 0 = ADD-form, 1 = SUB-form
            //   bit 21 (o2): 0 = positive product, 1 = negative product
            // ra = bits[14:10]. Operands: a=Vn, b=Vm, c=Va.
            //
            //   o2=0, o1=0: FMADD  → dest = a*b + c      (c + a*b)
            //   o2=0, o1=1: FMSUB  → dest = c - a*b      (c - a*b)
            //   o2=1, o1=0: FNMADD → dest = -c - a*b     (= -(a*b + c))
            //   o2=1, o1=1: FNMSUB → dest = a*b - c
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
            // Native FP_CSEL → VBLENDVPS/VPD in the JIT.
            if ((op & 0xFF200C00) == 0x1E200C00 && ftype <= 1) {
                uint8_t cond = (op >> 12) & 0xF;
                emit(block, IROp::FP_CSEL, rd, rn, rm, ftype, cond, 0, 0, cur_pc);
                return true;
            }
            // Scalar 64-bit shift-by-immediate (SHL/USHR/SSHR Dd, Dn, #imm).
            // The 64-bit-element shift encodings live in the FP space
            // (bits[28:24] = 11111), so the decoder routes them to FP_SCALAR
            // and they used to CALL_INTERP on every execution (the game's
            // `ushr dN, dM, #32` was the top remaining SIMD/FP fallback).
            // Reuse the SIMD shift IR ops with a single 64-bit lane
            // (esize=8, q=0): v_lo[rd] = shift(v_lo[rn]); v_hi[rd] = 0.
            //   SHL  (op & 0xFF00FC00) == 0x5F005400: shift = immh:immb - 64
            //   USHR (op & 0xFF00FC00) == 0x7F000400: shift = 128 - immh:immb
            //   SSHR (op & 0xFF00FC00) == 0x5F000400: shift = 128 - immh:immb
            // shift == 64 (all bits shifted out) falls back to the
            // interpreter, which clears (SHL/USHR) / sign-fills (SSHR) the
            // lane exactly — the SSE2 imm-shift opcode masks the count to 6
            // bits and would silently no-op.
            {
                uint8_t immh = (op >> 20) & 0xF;
                uint8_t immb = (op >> 16) & 0xF;
                int imm = (immh << 4) | immb;
                IROp sshift_op = IROp::SIMD_SHL;
                int sshift = 0;
                if ((op & 0xFF00FC00) == 0x5F005400) {
                    sshift_op = IROp::SIMD_SHL;
                    sshift = imm - 64;
                    if (sshift < 0) sshift = 0;
                } else if ((op & 0xFF00FC00) == 0x7F000400) {
                    sshift_op = IROp::SIMD_USHR;
                    sshift = 128 - imm;
                    if (sshift < 0) sshift = 0;
                } else if ((op & 0xFF00FC00) == 0x5F000400) {
                    sshift_op = IROp::SIMD_SSHR;
                    sshift = 128 - imm;
                    if (sshift < 0) sshift = 0;
                } else {
                    sshift = 64;  // not a scalar shift -> fall through
                }
                if (sshift < 64) {
                    emit(block, sshift_op, rd, rn, 0, 8, 0, 0, sshift, cur_pc);
                    return true;
                }
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
            emit(block, IROp::SIMD_DUP, d.rd, val, 0, 8, 0, 1, 0, cur_pc);
            return true;
        }
        // ── SIMD LD1/ST1 — native (128-bit load/store) ─────────────
        // Handles multi-register forms: LD1/ST1 {Vt..Vt+n-1} stores n×16
        // bytes (n = d.simd_count, 1..4). Each 128-bit register is moved
        // with ONE 16-byte memory access (SIMD_LD16/SIMD_ST16 — single
        // bounds-check + movupd in the JIT). Q=0 (.2s/.2d/.8b/.4h, 64-bit)
        // transfers only v_lo (8 bytes) and zeroes v_hi.
        case InstClass::SIMD_LD1: {
            // LD2/LD3/LD4 (de-interleaved multi-structure) are not native:
            // the LD16 fast path assumes registers are stored consecutively.
            // Fall back to the interpreter, which de-interleaves element-wise.
            if (d.simd_struct >= 2 || d.is_single_struct) {
                emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
                return true;
            }
            uint16_t base = load_arm_reg(block, d.rn, true);
            bool is_64bit = (d.Q == 0);
            if (is_64bit) {
                for (uint8_t i = 0; i < d.simd_count; i++) {
                    uint8_t reg = (d.rt + i) & 0x1F;
                    uint16_t lo = g_alloc.alloc();
                    emit(block, IROp::LOAD_MEM, lo, base, 0, 8, 0, 0,
                         static_cast<uint64_t>(i * 8));
                    // 64-bit form: v_hi = 0 (src2 = a real zero vreg via
                    // load_imm — literal 0 is guest X0 (vreg 0), not zero).
                    uint16_t zero = load_imm(block, 0);
                    emit(block, IROp::SIMD_LDST, reg, lo, zero, 1, 0, 0, 0, cur_pc);
                }
            } else {
                // 128-bit form: ONE 16-byte load per reg, single
                // bounds-check across the whole count (flags_op = count).
                emit(block, IROp::SIMD_LD16, d.rt, base, 0, 0, 0, d.simd_count,
                     static_cast<uint64_t>(0), cur_pc);
            }
            // Post-index writeback: Xn += nregs*vec_bytes (Rm==0b11111),
            // += 0 (Rm==0b11110), or += Xm.
            if (d.post_indexed) {
                uint16_t offv = 0;
                if (d.rm == 31) {
                    offv = load_imm(block,
                        (uint64_t)d.simd_count * (is_64bit ? 8u : 16u));
                } else if (d.rm == 30) {
                    offv = load_imm(block, 0);
                } else {
                    offv = load_arm_reg(block, d.rm);
                }
                uint16_t nb = g_alloc.alloc();
                emit(block, IROp::ADD, nb, base, offv);
                store_arm_reg(block, d.rn, nb, true);
            }
            return true;
        }
        case InstClass::SIMD_ST1: {
            // LD2/LD3/LD4 (de-interleaved multi-structure) are not native:
            // the ST16 fast path assumes registers are stored consecutively.
            // Fall back to the interpreter, which de-interleaves element-wise.
            if (d.simd_struct >= 2 || d.is_single_struct) {
                emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
                return true;
            }
            uint16_t base = load_arm_reg(block, d.rn, true);
            bool is_64bit = (d.Q == 0);
            if (is_64bit) {
                for (uint8_t i = 0; i < d.simd_count; i++) {
                    uint8_t reg = (d.rt + i) & 0x1F;
                    uint16_t lo = g_alloc.alloc();
                    // src2 = fresh scratch for the unused v_hi half — literal
                    // 0 is guest X0 (vreg 0) and set_vreg_reg would clobber it.
                    uint16_t hi_scratch = g_alloc.alloc();
                    emit(block, IROp::SIMD_LDST, reg, lo, hi_scratch, 0, 0, 0, 0, cur_pc);
                    emit(block, IROp::STORE_MEM, 0, base, lo, 8, 0, 0,
                         static_cast<uint64_t>(i * 8));
                }
            } else {
                // 128-bit form: ONE 16-byte store per reg, single
                // bounds-check across the whole count (flags_op = count).
                emit(block, IROp::SIMD_ST16, 0, base, d.rt, 0, 0, d.simd_count,
                     static_cast<uint64_t>(0), cur_pc);
            }
            // Post-index writeback: Xn += nregs*vec_bytes (Rm==0b11111),
            // += 0 (Rm==0b11110), or += Xm.
            if (d.post_indexed) {
                uint16_t offv = 0;
                if (d.rm == 31) {
                    offv = load_imm(block,
                        (uint64_t)d.simd_count * (is_64bit ? 8u : 16u));
                } else if (d.rm == 30) {
                    offv = load_imm(block, 0);
                } else {
                    offv = load_arm_reg(block, d.rm);
                }
                uint16_t nb = g_alloc.alloc();
                emit(block, IROp::ADD, nb, base, offv);
                store_arm_reg(block, d.rn, nb, true);
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
            int esize = 1 << size;
            // Native-family dispatch from the generated decode table
            // (arm64emu::simd::classify, tools/opgen/simd_dp.txt). The family
            // + per-op sub-code are the single source of truth on the JIT
            // side. The interpreter's SIMD_DP switch stays an independent
            // implementation, so interp-vs-JIT divergence is still
            // detectable. Anything UNKNOWN falls through to the interpreter.
            auto ct = simd::classify(op);
            switch (ct.family) {
            case simd::Family::INT_ARITH:
                // arith_op = ct.subop (0..6). MUL 64-bit (size==3) is
                // excluded by the table's guard and lands on CALL_INTERP.
                emit(block, IROp::SIMD_ARITH, d.rd, d.rn, d.rm,
                     static_cast<uint8_t>(esize), 0, 0, ct.subop, cur_pc);
                return true;
            case simd::Family::INT_CMP:
                // subop = ct.subop (0=CMEQ, 1=CMGT, 2=CMGE, 3=CMHI, 4=CMHS).
                // flags_op threads Q so the JIT zeroes v_hi for Q=0 (.8b etc.)
                // exactly like the interpreter's SIMD_DP compare block.
                emit(block, IROp::SIMD_CMP, d.rd, d.rn, d.rm,
                     static_cast<uint8_t>(esize), 0, static_cast<uint8_t>(Q),
                     ct.subop, cur_pc);
                return true;
            case simd::Family::LOGIC:
                // Native only for Q=1 (table enforces the guard). The IR
                // SIMD_LOGICAL computes both 64-bit halves, so Q=0 (.8b)
                // stays on the interpreter, which clears v_hi.
                emit(block, IROp::SIMD_LOGICAL, d.rd, d.rn, d.rm,
                     0, 0, 0, ct.subop, cur_pc);
                return true;
            case simd::Family::DUP:
                // GPR->vector broadcast, native for all element sizes
                // (table guard: imm5 = esize in {1,2,4,8}). width = esize,
                // flags_op = Q (1.5.2-alpha).
                {
                    uint16_t val = load_arm_reg(block, d.rn);
                    uint8_t esize = static_cast<uint8_t>((op >> 16) & 0x1F);
                    uint8_t q = static_cast<uint8_t>((op >> 30) & 1);
                    emit(block, IROp::SIMD_DUP, d.rd, val, 0, esize, 0, q, 0, cur_pc);
                    return true;
                }
            case simd::Family::UMOV: {
                // UMOV (vector element -> GPR). imm5 = esize | (index <<
                // log2(esize)); the low set bit gives the element size and
                // the remaining bits the lane index (interp reference:
                // interp_fp.cpp case 0x0E003C00). Q=0 writes Wd (zero-
                // extended 32-bit), Q=1 writes Xd. rd==31 (XZR) discards.
                uint8_t imm5 = static_cast<uint8_t>((op >> 16) & 0x1F);
                // Bounded loop mirroring interp_fp.cpp's UMOV decode: imm5
                // always has a set bit for valid encodings, but an imm5==0
                // encoding (UNDEFINED per ARM, still admitted by the guard)
                // must not hang the translator — the interp degrades to
                // esize=1, index=0, so match it.
                int esize_log2 = 0;
                for (int b = 0; b < 5; b++) {
                    if (imm5 & (1u << b)) { esize_log2 = b; break; }
                }
                uint8_t esize = static_cast<uint8_t>(1u << esize_log2);
                uint8_t index = static_cast<uint8_t>(imm5 >> (esize_log2 + 1));
                uint8_t qbit = static_cast<uint8_t>((op >> 30) & 1);
                if (d.rd != 31)
                    emit(block, IROp::SIMD_UMOV, d.rd, d.rn, 0, esize, 0,
                         qbit, index, cur_pc);
                return true;
            }
            case simd::Family::MODIMM: {
                // AdvSIMD modified immediate (MOVI/MVNI/ORR/BIC + MSL).
                // Mirror of the interpreter's block in interp_fp.cpp
                // (cmode = bits[15:12], imm8 = bits[18:16]:bits[9:5],
                // op = bit29, Q = bit30). MOVI/MVNI write a broadcast
                // constant; ORR/BIC read-modify-write the DESTINATION.
                // cmode=0xF (FMOV) never reaches here (guard rejects it)
                // and stays on the interpreter.
                uint8_t cmode = (op >> 12) & 0xF;
                uint8_t imm8 = static_cast<uint8_t>(
                    (((op >> 16) & 0x7) << 5) | ((op >> 5) & 0x1F));
                bool opbit = (op >> 29) & 1;
                bool q = (op >> 30) & 1;
                uint64_t pattern = 0;
                bool orr_bic = false;   // ORR/BIC read Vd as their source
                if ((cmode & 0x9) == 0x0) {
                    // 0xx0: MOVI/MVNI 32-bit LSL #((cmode>>1)&3)*8
                    uint32_t imm = static_cast<uint32_t>(imm8)
                                   << (((cmode >> 1) & 3) * 8);
                    if (opbit) imm = ~imm;
                    pattern = (uint64_t)imm | ((uint64_t)imm << 32);
                } else if ((cmode & 0x9) == 0x1) {
                    // 0xx1: ORR/BIC 32-bit LSL (read Vd)
                    uint32_t imm = static_cast<uint32_t>(imm8)
                                   << (((cmode >> 1) & 3) * 8);
                    pattern = (uint64_t)imm | ((uint64_t)imm << 32);
                    orr_bic = true;
                } else if ((cmode & 0xD) == 0x8) {
                    // 10x0: MOVI/MVNI 16-bit LSL #((cmode&2)?8:0)
                    uint64_t imm = static_cast<uint16_t>(
                        static_cast<uint16_t>(imm8) << ((cmode & 2) ? 8 : 0));
                    if (opbit) imm = static_cast<uint16_t>(~imm) & 0xFFFFu;
                    pattern = imm | (imm << 16) | (imm << 32) | (imm << 48);
                } else if ((cmode & 0xD) == 0x9) {
                    // 10x1: ORR/BIC 16-bit LSL (read Vd)
                    uint64_t imm = static_cast<uint16_t>(
                        static_cast<uint16_t>(imm8) << ((cmode & 2) ? 8 : 0));
                    pattern = imm | (imm << 16) | (imm << 32) | (imm << 48);
                    orr_bic = true;
                } else if ((cmode & 0xE) == 0xC) {
                    // 110x: MOVI/MVNI 32-bit MSL #((cmode&1)+1)*8
                    int shift = ((cmode & 1) + 1) * 8;
                    uint32_t ones = (shift == 16) ? 0xFFFFu : 0xFFu;
                    uint32_t imm = (static_cast<uint32_t>(imm8) << shift) | ones;
                    if (opbit) imm = ~imm;
                    pattern = (uint64_t)imm | ((uint64_t)imm << 32);
                } else if (cmode == 0xE && !opbit) {
                    // MOVI 8-bit: replicate imm8 across all bytes
                    for (int i = 0; i < 8; i++)
                        pattern |= static_cast<uint64_t>(imm8) << (i * 8);
                } else if (cmode == 0xE && opbit) {
                    // MOVI 64-bit: each imm8 bit selects 0x00/0xFF byte
                    for (int i = 0; i < 8; i++)
                        if (imm8 & (1u << i))
                            pattern |= 0xFFULL << (i * 8);
                } else {
                    break;  // cmode=0xF FMOV → interpreter (matches guard)
                }
                if (orr_bic)
                    emit(block, IROp::SIMD_ORRIMM, d.rd, 0, 0, 0, opbit, q,
                         pattern, cur_pc);
                else
                    emit(block, IROp::SIMD_MOVI, d.rd, 0, 0, 0, 0, q,
                         pattern, cur_pc);
                return true;
            }
            case simd::Family::FP: {
                // Vector FP 2-source / FMA. bit22: 0=single, 1=double.
                bool is_double = (op >> 22) & 1;
                uint64_t fesize = is_double ? 8 : 4;
                if (ct.subop == 0xE || ct.subop == 0xF) {
                    // FMLA/FMLS: accumulate into dest (3-source).
                    emit(block, IROp::SIMD_FP_FMA, d.rd, d.rn, d.rm,
                         static_cast<uint8_t>(fesize), 0, Q, ct.subop - 0xE, cur_pc);
                } else {
                    emit(block, IROp::SIMD_FP_ARITH, d.rd, d.rn, d.rm,
                         static_cast<uint8_t>(fesize), 0, Q, ct.subop, cur_pc);
                }
                return true;
            }
            case simd::Family::SHIFT: {
                // Vector shift-by-immediate. immh/immb encode element size
                // and shift amount (matches the interpreter's decoding).
                uint8_t immh = (op >> 20) & 0xF;
                uint8_t immb = (op >> 16) & 0xF;
                uint8_t esize_bytes = 0;
                if      (immh == 0) esize_bytes = 1;
                else if (immh == 1) esize_bytes = 2;
                else if (immh == 2 || immh == 3) esize_bytes = 4;
                else if (immh >= 4) esize_bytes = 8;
                if (esize_bytes == 0) break;  // invalid immh -> interpreter
                uint8_t qbit = (op >> 30) & 1;
                // subop 0/5 are left-shift (SHL/SLI); the rest right-shift.
                bool left = (ct.subop == 0 || ct.subop == 5);
                uint32_t imm = (uint32_t)((immh << 4) | immb);
                uint32_t shift_amount = left
                    ? imm - (uint32_t)esize_bytes * 8
                    : (uint32_t)esize_bytes * 8 * 2 - imm;
                IROp shift_op;
                switch (ct.subop) {
                    case 0: shift_op = IROp::SIMD_SHL;  break;
                    case 1: shift_op = IROp::SIMD_USHR; break;
                    case 2: shift_op = IROp::SIMD_SSHR; break;
                    case 3: shift_op = IROp::SIMD_USRA; break;
                    case 4: shift_op = IROp::SIMD_SSRA; break;
                    case 5: shift_op = IROp::SIMD_SLI;  break;
                    case 6: shift_op = IROp::SIMD_SRI;  break;
                    case 7: shift_op = IROp::SIMD_URSRA; break;
                    default: shift_op = IROp::SIMD_SRSRA; break;  // subop 8
                }
                emit(block, shift_op, d.rd, d.rn, 0,
                     esize_bytes, 0, qbit, shift_amount, cur_pc);
                return true;
            }
            case simd::Family::CRYPTO:
                // subop 0 = AESE, which covers AESD/AESMC/AESIMC via
                // bits[11:10]; subop 4 = PMULL, 5 = PMULL2.
                if (ct.subop == 0) {
                    uint8_t sub_op = static_cast<uint8_t>((op >> 10) & 3);
                    emit(block, IROp::AES_CRYPTO, d.rd, d.rn, 0, 0, 0, 0, sub_op, cur_pc);
                } else {
                    emit(block, IROp::AES_CRYPTO, d.rd, d.rn, d.rm, 0, 0, 0, ct.subop, cur_pc);
                }
                return true;
            default:
                break;
            }
            // Remaining SIMD_DP encodings fall back to the interpreter.
            // This includes the SHA-1/SHA-256 family (software impl in
            // interp_crypto.hpp; native SHA-NI codegen is future work).
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
