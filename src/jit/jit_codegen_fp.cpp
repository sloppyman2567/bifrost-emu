// jit/jit_codegen_fp.cpp — FrostJIT FP/SIMD IR-op codegen dispatcher.
//
// v1.4.5-alpha: split out of frostjit.cpp. This file held the
// FP_* and SIMD_* case bodies of the IR-op switch, extracted into a
// separate method (compile_ir_inst_fp_) for readability. The main switch
// in frostjit.cpp dispatches to this method for FP/SIMD ops.
//
// v1.4.5-alpha: split further into two sub-files:
//   - jit_codegen_fparith.cpp (compile_ir_fparith): FMOV_*, FP_BINOP,
//     FP_UNOP, FP_CMP, FP_MOVI, FP_F2I, FP_I2F, FP_F2I_FIXED,
//     FP_I2F_FIXED, FCVT_S2D, FCVT_D2S
//   - jit_codegen_simd.cpp    (compile_ir_simd):    SIMD_LOGICAL,
//     SIMD_ARITH, SIMD_CMP, SIMD_DUP, SIMD_LDST, SIMD_SHL/USHR/SSHR
//
// This file now holds only the residual cases (FRINT, FMADD family) plus
// the dispatcher logic that calls the two sub-dispatchers before the
// residual switch.
//
// No behavior change — pure file split. The method is a member of
// FrostJIT (declared in include/jit/frostjit.hpp) so it has full access
// to the JIT's emit_*, alloc_*, flush_*, etc. helpers.
#include "jit/frostjit.hpp"
#include "core/emulator.h"
#include "ir/ir.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
namespace arm64emu {
// ── FrostJIT::compile_ir_inst_fp_ ──────────────────────────────────────
// Handles all FP_* and SIMD_* IR ops. Returns true if the op was
// handled (caller returns the bool as the "ends_block" flag), false if
// not (caller falls through to the default: case, which calls
// emit_call_interp).
//
// Dispatch flow:
//   1. compile_ir_fparith() handles scalar FP arithmetic/conversion/move.
//   2. compile_ir_simd() handles SIMD/NEON ops.
//   3. The residual switch below handles FRINT and the FMADD family
//      (FMADD/FMSUB/FNMADD/FNMSUB).
//   4. default: clears fp_handled_ so the caller (compile_ir_inst) falls
//      through to the integer switch.
bool FrostJIT::compile_ir_inst_fp_(const IRInst& inst) {
    // Set fp_handled_ = true optimistically; the default: case clears
    // it. The caller (compile_ir_inst) checks fp_handled_ after the
    // call to decide whether to fall through to the integer switch.
    fp_handled_ = true;
    // v1.4.5-alpha: FP arithmetic/conversion/move ops and
    // SIMD/NEON ops are dispatched to compile_ir_fparith() and
    // compile_ir_simd() (defined in jit_codegen_fparith.cpp and
    // jit_codegen_simd.cpp) before the residual switch below. Both
    // helpers return true if the op was handled (in which case it does
    // NOT end the block — FP ops never do — so we return false here).
    // The residual switch handles FRINT and the FMADD family.
    if (compile_ir_fparith(inst)) return false;
    if (compile_ir_simd(inst))    return false;
    switch (inst.op) {
        // ── FRINT: FP round to integer ───────────────────────────────
        //
        //
        // The fell back to CALL_INTERP because the IR
        // translator was passing VREG indices (>= 33) as inst.dest/
        // inst.src1, causing out-of-bounds writes to v_lo[33+]. The
        // translator now passes ARM FP reg indices (0-31) directly
        // (same convention as FP_BINOP/FP_UNOP), so V_LO_OFF + idx*8
        // is correct. This makes floor/ceil/trunc/round execute
        // natively under JIT instead of falling back to interpreter.
        case IROp::FRINT: {
            // roundsd/roundss are SSE4.1 instructions. Fall back to
            // CALL_INTERP on hosts without SSE4.1 to avoid SIGILL.
            if (!has_sse41()) {
                emit_call_interp(inst.arm_pc, false);
                return false;
            }
            // Validate FP register indices 
            // fix, these are now always 0-31, but guard against future
            // regressions).
            check_fp_reg_index(inst.dest, "FRINT dest");
            check_fp_reg_index(inst.src1, "FRINT src1");
            // FRINT only clobbers RAX (zero store to v_hi[dest]).
            clobber_flags();
            flush_invalidate_host_regs(1u << RAX);
            bool is_double = (inst.width == 64);
            uint8_t prefix = is_double ? 0xF2 : 0xF3;
            // Load FP value into XMM0 (fp_load_operand: reg-reg move when
            // the fp cache pins src1, else memory load).
            fp_load_operand(0, inst.src1, is_double);
            // x86 rounding mode mapping (SSE4.1 roundsd/roundss imm8):
            //   0 = round-to-nearest (even)
            //   1 = round-down (-inf)
            //   2 = round-up (+inf)
            //   3 = round-toward-zero (truncate)
            //   4 = use current MXCSR rounding mode
            //
            // ARM FRINT mode → x86 mode:
            //   0 (FRINTN) → 0 (nearest)
            //   1 (FRINTP) → 2 (+inf / ceil)
            //   2 (FRINTM) → 1 (-inf / floor)
            //   3 (FRINTZ) → 3 (truncate)
            //   4 (FRINTA) → 4 (MXCSR, default = nearest)
            //   5 (FRINTX) → 4 (uses FPCR rounding mode)
            //   4 (FRINTI) → 4 (uses FPCR rounding mode)
            uint8_t x86_mode;
            switch (inst.imm & 0x7) {
                case 0: x86_mode = 0; break;  // N → nearest
                case 1: x86_mode = 2; break;  // P → +inf (ceil)
                case 2: x86_mode = 1; break;  // M → -inf (floor)
                case 3: x86_mode = 3; break;  // Z → truncate
                default: x86_mode = 4; break; // I/X/A → current MXCSR
            }
            // roundsd xmm0, xmm0, imm8:  66 0F 3A 0B C0 imm8
            // roundss xmm0, xmm0, imm8:  66 0F 3A 0A C0 imm8
            emit_byte(0x66); emit_byte(0x0F); emit_byte(0x3A);
            emit_byte(is_double ? 0x0B : 0x0A);
            emit_byte(0xC0);  // xmm0, xmm0
            emit_byte(x86_mode);
            // Store result (fp_store_operand: pinned dest → reg-reg + dirty, else
            // memory) and zero v_hi.
            fp_store_operand(0, inst.dest, is_double);
            fp_zero_hi(inst.dest);
            return false;
        }
        // ── FMADD / FMSUB / FNMADD / FNMSUB: FP fused multiply-add family
        //
        // ARM FMA semantics (per ARM ARM):
        //   FMADD:  Vd = Va + Vn*Vm       = c + a*b
        //   FMSUB:  Vd = Va - Vn*Vm       = c - a*b
        //   FNMADD: Vd = -Va - Vn*Vm      = -c - a*b  (= -(a*b + c))
        //   FNMSUB: Vd = -Va + Vn*Vm      = a*b - c
        //
        // ── FMA3 path (when host CPU supports FMA3 + AVX) ───────────
        //
        // We use the 231 form: vfmXXX231sd xmm0, xmm1, xmm2/m64
        //   xmm0 = src1 * src2 OP xmm0   (xmm0 is both acc input and dest)
        //
        //   FMADD  → vfmadd231ss/sd   (xmm0 = +Vn*Vm + Va)
        //   FMSUB  → vfnmadd231ss/sd  (xmm0 = -Vn*Vm + Va = Va - Vn*Vm)
        //   FNMADD → vfnmadd231ss/sd  (same as FMSUB — single instruction,
        //                               single-rounded, IEEE 754-correct)
        //   FNMSUB → vfnmsub231ss/sd  (xmm0 = -Vn*Vm - Va)
        //
        // VEX 3-byte encoding (FMA3 uses 0F38 escape map):
        //   C4 [R~ X~ B~ mmmmm] [W vvvv~ L pp] [opcode] [modrm]
        //
        //   byte1 = 0x02  (R=X=B=1 inverted=0, mmmmm=00010 for 0F38)
        //   byte2 = W<<7 | (~vvvv)<<3 | L<<2 | pp
        //     W=1 for sd (double), W=0 for ss (single)
        //     vvvv = NDS register (xmm1, index 1, inverted = 0b1110)
        //     L=0 (128-bit XMM, not 256-bit YMM)
        //     pp = 11 (F2 prefix, sd) or 10 (F3 prefix, ss)
        //
        // Opcodes (per Intel SDM Vol 2A, FMA3 instruction table):
        //   vfmadd231ss/sd:  0x99
        //   vfmsub231ss/sd:  0x9B   (not used by ARM FMA mapping)
        //   vfnmadd231ss/sd: 0xBD
        //   vfnmsub231ss/sd: 0xBF
        //
        // ── Decomposed path (no FMA3) ────────────────────────────────
        //
        // We decompose into separate mulsd + addsd/subsd. This is
        // double-rounded (NOT IEEE 754-correct for edge cases — a
        // known limitation), but matches the interpreter's decomposition
        // path so JIT/interpreter agree.
        //
        // The clobber list (RAX, RCX, RDX) matches the existing FP
        // codegen convention — FP ops only touch XMM0/XMM1/XMM2 plus
        // those three GPRs (RAX for the zero store at the end).
        case IROp::FMADD:
        case IROp::FMSUB:
        case IROp::FNMADD:
        case IROp::FNMSUB: {
            clobber_flags();
            flush_invalidate_host_regs((1u << RAX) | (1u << RCX) | (1u << RDX));
            // Width encoding: 64 = double (D), 32 = single (S).
            // The IR translator emits `ftype ? 64 : 32` for FMADD/FRINT,
            // which is inconsistent with FP_BINOP (uses ftype 0/1 directly).
            // We use `width == 64` to handle this correctly. Using
            // `width != 0` (as the old code did) treats BOTH 32 and 64 as
            // double — silently breaking all single-precision FMA.
            bool is_double = (inst.width == 64);
            uint8_t prefix = is_double ? 0xF2 : 0xF3;
            int32_t off1 = V_LO_OFF + static_cast<int>(inst.src1) * 8;  // Vn
            int32_t off2 = V_LO_OFF + static_cast<int>(inst.src2) * 8;  // Vm
            int32_t off_acc = V_LO_OFF + static_cast<int>(inst.imm) * 8; // Va
            if (has_fma3()) {
                // ── FMA3 native codegen (fp-cache aware) ──
                // 231 form: dest = op(Vn*Vm, dest), where dest starts as Va
                // (the accumulator). When the fp cache pins the operands,
                // the pinned XMMs are used in place (no memory); unpinned
                // operands load into the scratch XMMs 1/2 (XMM0 is the
                // unpinned-dest result register and is never pinned).
                uint8_t opcode;
                switch (inst.op) {
                    case IROp::FMADD:  opcode = 0xB9; break;  // vfmadd231   (acc + prod)
                    case IROp::FMSUB:  opcode = 0xBD; break;  // vfnmadd231  (acc - prod)
                    case IROp::FNMADD: opcode = 0xBF; break;  // vfnmsub231  (-acc - prod)
                    case IROp::FNMSUB: opcode = 0xBB; break;  // vfmsub231   (prod - acc)
                    default: return false;  // unreachable
                }
                int xd = vec_xmm(inst.dest);
                int xacc = vec_xmm(inst.imm);
                int dst_xmm;
                bool copy_acc;
                if (xd >= 0) {
                    dst_xmm = xd;
                    copy_acc = (xacc != xd);
                } else {
                    dst_xmm = 0;  // compute into XMM0 (unpinned dest)
                    copy_acc = true;
                }
                // Resolve Vn/Vm BEFORE the acc copy: the acc copy overwrites
                // the dest XMM, so any source pinned to that SAME XMM (dest
                // aliases src1/src2 while acc is elsewhere — e.g. GCC's
                // `fmsub d0, d0, d1, d2`) would have its value destroyed
                // before the FMA reads it. Reload such a source from v_lo
                // into its scratch instead of using the pinned XMM.
                int xs1 = vec_xmm(inst.src1);
                int xs2 = vec_xmm(inst.src2);
                if (copy_acc && xd >= 0) {
                    if (xs1 == xd) { fp_load_operand(1, inst.src1, is_double); xs1 = 1; }
                    if (xs2 == xd) { fp_load_operand(2, inst.src2, is_double); xs2 = 2; }
                }
                if (xs1 < 0) { fp_load_operand(1, inst.src1, is_double); xs1 = 1; }
                if (xs2 < 0) { fp_load_operand(2, inst.src2, is_double); xs2 = 2; }
                // Copy Va (acc) into the dest XMM, LAST, so the source loads
                // above are never clobbered by it.
                if (xd >= 0) {
                    if (xacc >= 0 && xacc != xd) {
                        // movsd/movss xmm_d, xmm_acc (dest = acc)
                        emit_byte(prefix);
                        emit_byte(rex(false, xd >= 8, false, xacc >= 8));
                        emit_byte(0x0F); emit_byte(0x10);
                        emit_byte(modrm(3, xd & 7, xacc & 7));
                    } else if (xacc < 0) {
                        // acc unpinned: load from memory into XMM0, move to dest
                        emit_byte(prefix); emit_byte(0x0F); emit_byte(0x10);
                        emit_modrm_disp(0, CPU_REG, off_acc);
                        emit_byte(prefix);
                        emit_byte(rex(false, xd >= 8, false, false));
                        emit_byte(0x0F); emit_byte(0x10);
                        emit_byte(modrm(3, xd & 7, 0));
                    }
                    // xacc == xd: dest already holds acc
                } else {
                    if (xacc >= 0) {
                        emit_byte(prefix);
                        emit_byte(rex(false, false, false, xacc >= 8));
                        emit_byte(0x0F); emit_byte(0x10);
                        emit_byte(modrm(3, 0, xacc & 7));
                    } else {
                        emit_byte(prefix); emit_byte(0x0F); emit_byte(0x10);
                        emit_modrm_disp(0, CPU_REG, off_acc);
                    }
                }
                // VEX.128.66.0F38.W[is_double]: vfmadd231ss/sd xmmD, xmmS1, xmmS2
                // (FMA3 uses pp=01 for BOTH ss and sd — the W bit selects width).
                emit_vex3(2, is_double, xs1, 1, dst_xmm, xs2, true, opcode);
                if (xd >= 0) {
                    vec_cache_mark_dirty(inst.dest);
                } else {
                    fp_store_operand(0, inst.dest, is_double);
                }
                fp_zero_hi(inst.dest);
                return false;
            } else {
                // ── Decomposed path (no FMA3) ──
                // Load Vn into XMM0, Vm into XMM1, multiply → XMM0 = Vn*Vm
                emit_byte(prefix); emit_byte(0x0F); emit_byte(0x10);
                emit_modrm_disp(0, CPU_REG, off1);
                emit_byte(prefix); emit_byte(0x0F); emit_byte(0x10);
                emit_modrm_disp(1, CPU_REG, off2);
                emit_byte(prefix); emit_byte(0x0F); emit_byte(0x59);
                emit_byte(0xC1);  // mulsd xmm0, xmm1
                // Load Va (acc) into XMM2
                emit_byte(prefix); emit_byte(0x0F); emit_byte(0x10);
                emit_modrm_disp(2, CPU_REG, off_acc);
                // Combine per operation (ARM FMA semantics):
                //   FMADD:  r = acc + prod       → addsd xmm0, xmm2
                //   FMSUB:  r = acc - prod       → subsd xmm2, xmm0; movaps xmm0, xmm2
                //   FNMADD: r = -acc - prod      → addsd xmm0, xmm2; negate xmm0
                //   FNMSUB: r = prod - acc       → subsd xmm0, xmm2
                if (inst.op == IROp::FMADD) {
                    emit_byte(prefix); emit_byte(0x0F); emit_byte(0x58);
                    emit_byte(0xC2);  // addsd xmm0, xmm2
                } else if (inst.op == IROp::FMSUB) {
                    emit_byte(prefix); emit_byte(0x0F); emit_byte(0x5C);
                    emit_byte(0xD0);  // subsd xmm2, xmm0  (xmm2 = acc - prod)
                    emit_byte(0x0F); emit_byte(0x28); emit_byte(0xC2);  // movaps xmm0, xmm2
                } else if (inst.op == IROp::FNMSUB) {
                    emit_byte(prefix); emit_byte(0x0F); emit_byte(0x5C);
                    emit_byte(0xC2);  // subsd xmm0, xmm2  (xmm0 = prod - acc)
                } else {  // FNMADD
                    emit_byte(prefix); emit_byte(0x0F); emit_byte(0x58);
                    emit_byte(0xC2);  // addsd xmm0, xmm2  (xmm0 = prod + acc)
                    // Negate xmm0 by XORing with sign bit.
                    // mov rax, sign_mask  (0x8000000000000000 for double,
                    //                      0x80000000 for single, zero-extended)
                    if (is_double) {
                        emit_mov_imm64(RAX, 0x8000000000000000ULL);
                    } else {
                        emit_mov_imm32_zext(RAX, 0x80000000u);
                    }
                    // movq xmm1, rax  (REX.W + 66 0F 6E ModRM)
                    // NOTE: the REX.W prefix (0x48) is REQUIRED — without
                    // it, this is `movd xmm1, eax` which only moves the
                    // low 32 bits. For the double-precision sign mask
                    // 0x8000000000000000, the low 32 bits are 0, so the
                    // xorpd would be a no-op and the negation is lost.
                    // The mandatory prefix 66 comes first, then REX.
                    emit_byte(0x66);
                    emit_byte(0x48);  // REX.W
                    emit_byte(0x0F); emit_byte(0x6E);
                    emit_byte(0xC8);  // ModRM: xmm1, rax
                    // xorpd xmm0, xmm1  (0x66 0x0F 0x57 0xC1)
                    emit_byte(0x66); emit_byte(0x0F); emit_byte(0x57);
                    emit_byte(0xC1);
                }
            }
            // Store result (in XMM0) to v_lo[dest] (fp_store_operand: pinned
            // dest → reg-reg + dirty, else memory) and zero v_hi.
            fp_store_operand(0, inst.dest, is_double);
            fp_zero_hi(inst.dest);
            return false;
        }
        default:
            // Not an FP/SIMD op — clear fp_handled_ so the caller falls
            // through to the integer switch.
            fp_handled_ = false;
            return false;
    }
}
} // namespace arm64emu
