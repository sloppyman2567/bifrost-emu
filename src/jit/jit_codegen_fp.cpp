// jit/jit_codegen_fp.cpp — FrostJIT FP/SIMD IR-op codegen dispatcher.
//
// v1.4.5-alpha (Turn 36): split out of frostjit.cpp. This file held the
// FP_* and SIMD_* case bodies of the IR-op switch, extracted into a
// separate method (compile_ir_inst_fp_) for readability. The main switch
// in frostjit.cpp dispatches to this method for FP/SIMD ops.
//
// v1.4.5-alpha (Turn 69): split further into two sub-files:
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

    // v1.4.5-alpha (Turn 69): FP arithmetic/conversion/move ops and
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
        case IROp::FRINT: {
            // BUGFIX (Turn 88): Fall back to CALL_INTERP for FRINT.
            // The native roundsd codegen was broken because it used
            // V_LO_OFF + inst.dest * 8 to store the result, but
            // inst.dest is a scratch vreg (index > 32) whose data
            // lives on the stack, not in v_lo[]. The write went to
            // the wrong memory location, and the subsequent STORE_REG
            // read garbage from the stack slot.
            // The interpreter handles FRINT correctly (Turn 87 fix:
            // 6-bit opcodes 0x08-0x0E). Performance impact is minimal
            // since FRINT is rare (only floor/ceil/trunc/round).
            emit_call_interp(inst.arm_pc, false);
            return false;
        }

        // ── FMADD / FMSUB / FNMADD / FNMSUB: FP fused multiply-add family
        //
        // ARM FMA semantics (per ARM ARM):
        //   FMADD:  Vd = Va + Vn*Vm       = c + a*b
        //   FMSUB:  Vd = Va - Vn*Vm       = c - a*b
        //   FNMADD: Vd = -Vn*Vm + Va      = -a*b + c  (same numerical
        //                                            result as FMSUB but
        //                                            different IEEE 754
        //                                            sign rules)
        //   FNMSUB: Vd = -Vn*Vm - Va      = -a*b - c  (= -(a*b + c))
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
                // ── FMA3 native codegen ──
                // Load Va (accumulator) into XMM0 — the FMA3 231 form uses
                // XMM0 as both acc input and result dest.
                emit_byte(prefix); emit_byte(0x0F); emit_byte(0x10);
                emit_modrm_disp(0, CPU_REG, off_acc);  // movsd xmm0, [rbx+off_acc]
                // Load Vn into XMM1 (the NDS register — multiply operand 1).
                emit_byte(prefix); emit_byte(0x0F); emit_byte(0x10);
                emit_modrm_disp(1, CPU_REG, off1);  // movsd xmm1, [rbx+off1]
                // Vm is read directly from memory via ModRM.rm (no load needed).

                // Pick opcode based on operation:
                //   FMADD         → vfmadd231  (0xB9)
                //   FMSUB/FNMADD  → vfnmadd231 (0xBD)  (numerically same)
                //   FNMSUB        → vfnmsub231 (0xBF)
                //
                // Opcodes per Intel SDM Vol 2A, FMA3 table:
                //   vfmadd231ss/sd:  0xB9   (132=0x99, 213=0xA9, 231=0xB9)
                //   vfnmadd231ss/sd: 0xBD   (132=0x9D, 213=0xAD, 231=0xBD)
                //   vfnmsub231ss/sd: 0xBF   (132=0x9F, 213=0xAF, 231=0xBF)
                //
                // NOTE: FMA3 uses VEX.pp=01 (66 prefix) for BOTH ss and sd —
                // the W bit (not pp) distinguishes single (W=0) from double
                // (W=1). This is different from scalar SSE FP (mulsd uses
                // pp=11/F2, mulss uses pp=10/F3).
                uint8_t opcode;
                switch (inst.op) {
                    case IROp::FMADD:  opcode = 0xB9; break;
                    case IROp::FMSUB:  opcode = 0xBD; break;  // vfnmadd231
                    case IROp::FNMADD: opcode = 0xBD; break;  // vfnmadd231
                    case IROp::FNMSUB: opcode = 0xBF; break;  // vfnmsub231
                    default: return false;  // unreachable
                }

                // VEX 3-byte prefix:
                //   C4
                //   byte1: 0xE2  (R~=X~=B~=1 for low registers xmm0-xmm7
                //                 and rbx; mmmmm=00010 for 0F38 map)
                //   byte2: W<<7 | (~1)<<3 | 0<<2 | pp
                //     W = is_double ? 1 : 0
                //     vvvv~ = ~0001 = 1110 (NDS = xmm1)
                //     L = 0 (LIG — ignored by FMA3, set to 0 for 128-bit)
                //     pp = 01 (66 — mandatory for FMA3, NOT F2/F3!)
                //
                // NOTE: VEX byte1's R/X/B bits are INVERTED relative to
                // REX.R/X/B. For low registers (no high bit), R=X=B=0 in
                // REX sense, so R~=X~=B~=1 in VEX. The old code used 0x02
                // (R~=X~=B~=0) which means R=X=B=1 — indicating xmm8-15
                // and rbx-with-REX.B, causing the CPU to access xmm8 as
                // the destination and segfault on the memory operand.
                uint8_t vex_b1 = 0xE2;
                uint8_t vex_b2 = (is_double ? 0x80 : 0x00)   // W
                               | (0x0E << 3)                   // vvvv~ = ~1 = 1110
                               | 0x00                          // L = 0
                               | 0x01;                         // pp = 01 (66)
                emit_byte(0xC4);
                emit_byte(vex_b1);
                emit_byte(vex_b2);
                emit_byte(opcode);
                // ModRM: reg=xmm0 (dest + acc), rm=[rbx+off2] (Vm memory operand).
                emit_modrm_disp(0, CPU_REG, off2);
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
                // Combine per operation:
                //   FMADD:  r = prod + acc       → addsd xmm0, xmm2
                //   FMSUB:  r = acc - prod       → subsd xmm2, xmm0; movaps xmm0, xmm2
                //   FNMADD: r = -prod + acc      = acc - prod → same as FMSUB
                //   FNMSUB: r = -prod - acc      → addsd xmm0, xmm2; negate xmm0
                if (inst.op == IROp::FMADD) {
                    emit_byte(prefix); emit_byte(0x0F); emit_byte(0x58);
                    emit_byte(0xC2);  // addsd xmm0, xmm2
                } else if (inst.op == IROp::FMSUB || inst.op == IROp::FNMADD) {
                    emit_byte(prefix); emit_byte(0x0F); emit_byte(0x5C);
                    emit_byte(0xD0);  // subsd xmm2, xmm0  (xmm2 = acc - prod)
                    emit_byte(0x0F); emit_byte(0x28); emit_byte(0xC2);  // movaps xmm0, xmm2
                } else {  // FNMSUB
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
            // Store result (in XMM0) to v_lo[dest]
            int32_t off_d = V_LO_OFF + static_cast<int>(inst.dest) * 8;
            emit_byte(prefix); emit_byte(0x0F); emit_byte(0x11);
            emit_modrm_disp(0, CPU_REG, off_d);
            emit_mov_imm32_zext(RAX, 0);
            emit_store(CPU_REG, V_HI_OFF + static_cast<int>(inst.dest) * 8, RAX);
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
