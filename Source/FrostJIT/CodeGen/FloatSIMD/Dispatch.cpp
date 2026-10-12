// Dispatch.cpp — FrostJIT FP/SIMD IR-op codegen dispatcher.
//
// split out of FrostJIT.cpp. This file held the
// FP_* and SIMD_* case bodies of the IR-op switch, extracted into a
// separate method (compile_ir_inst_fp_) for readability. The main switch
// in FrostJIT.cpp dispatches to this method for FP/SIMD ops.
//
// split further into two sub-files:
//   - FloatArithmetic.cpp (compile_ir_fparith): FMOV_*, FP_BINOP,
//     FP_UNOP, FP_CMP, FP_MOVI, FP_F2I, FP_I2F, FP_F2I_FIXED,
//     FP_I2F_FIXED, FCVT_S2D, FCVT_D2S
//   - SIMD.cpp    (compile_ir_simd):    SIMD_LOGICAL,
//     SIMD_ARITH, SIMD_CMP, SIMD_DUP, SIMD_LDST, SIMD_SHL/USHR/SSHR
//
// This file now holds only the residual cases (FRINT, FMADD family) plus
// the dispatcher logic that calls the two sub-dispatchers before the
// residual switch.
//
// No behavior change — pure file split. The method is a member of
// FrostJIT (declared in Source/FrostJIT/Compiler/FrostJIT.hpp) so it has full access
// to the JIT's emit_*, alloc_*, flush_*, etc. helpers.
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include "System/Core/Emulator.hpp"
#include "FrostJIT/IR/IR.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
namespace arm64emu {
// SCVTF/UCVTF core: convert the integer in RAX to XMM0 (single or double).
// Signed 32-bit uses cvtsi2s* eax; everything else converts a 64-bit value.
// Unsigned 32-bit zero-extends first (Xn's upper half is not part of Wn).
// Unsigned x >= 2^63 halves with a sticky low bit, converts, and doubles:
// y = (x >> 1) | (x & 1) keeps round-to-nearest-even exact in one rounding
// (the GCC/LLVM lowering). cvt(x - 2^63) + 2^63 rounds twice (1-ULP errors).
void FrostJIT::emit_int_to_fp_xmm0(bool is_double, bool is_unsigned, bool is_64bit_src) {
    const uint8_t prefix = is_double ? 0xF2 : 0xF3;
    auto cvt64 = [&] {  // cvtsi2sd/ss xmm0, rax
        emit_byte(prefix); emit_byte(0x48); emit_byte(0x0F); emit_byte(0x2A); emit_byte(0xC0);
    };
    if (!is_unsigned) {
        emit_byte(prefix);
        if (is_64bit_src) emit_byte(0x48);
        emit_byte(0x0F); emit_byte(0x2A); emit_byte(0xC0);  // cvtsi2s* xmm0, rax/eax
        return;
    }
    if (!is_64bit_src) {  // < 2^32: one signed 64-bit convert is exact-rounded
        emit_byte(0x89); emit_byte(0xC0);  // mov eax, eax
        cvt64();
        return;
    }
    emit_byte(0x48); emit_byte(0x85); emit_byte(0xC0);   // test rax, rax
    const size_t jns_small = emit_jcc_rel32_placeholder(0x9);  // JNS: x < 2^63
    emit_byte(0x48); emit_byte(0x89); emit_byte(0xC1);   // mov rcx, rax
    emit_byte(0x83); emit_byte(0xE1); emit_byte(0x01);   // and ecx, 1
    emit_byte(0x48); emit_byte(0xD1); emit_byte(0xE8);   // shr rax, 1
    emit_byte(0x48); emit_byte(0x09); emit_byte(0xC8);   // or rax, rcx
    cvt64();
    emit_byte(prefix); emit_byte(0x0F); emit_byte(0x58); emit_byte(0xC0);  // adds* xmm0, xmm0
    const size_t jmp_done = emit_jmp_rel32_placeholder();
    patch_jcc_rel32(jns_small, static_cast<int32_t>(code_buf_used_ - (jns_small + 6)));
    cvt64();
    patch_jmp_rel32(jmp_done, static_cast<int32_t>(code_buf_used_ - (jmp_done + 5)));
}
// Round by truncating first, then adjust only when the fractional magnitude
// is >= 0.5. Adding 0.5 to the input would mis-round values just below a tie
// and large already-integral values. Scratch XMM0-2 stay below pinned XMM3-15.
void FrostJIT::emit_fp_round_away(bool is_double) {
    emit_byte(0x66); emit_byte(0x0F); emit_byte(0x3A);
    emit_byte(is_double ? 0x0B : 0x0A); emit_byte(0xC8); emit_byte(3); // trunc X1,X0
    emit_byte(0x66); emit_byte(0x48); emit_byte(0x0F); emit_byte(0x7E); emit_byte(0xC0); // RAX=original bits
    emit_byte(is_double ? 0xF2 : 0xF3); emit_byte(0x0F); emit_byte(0x5C); emit_byte(0xC1);
    emit_mov_imm64(RCX, is_double ? 0x7FFFFFFFFFFFFFFFULL : 0x7FFFFFFFULL);
    emit_byte(0x66); emit_byte(0x48); emit_byte(0x0F); emit_byte(0x6E); emit_byte(0xD1);
    emit_byte(0x0F); emit_byte(0x54); emit_byte(0xC2); // abs fractional part
    emit_mov_imm64(RCX, is_double ? 0x3FE0000000000000ULL : 0x3F000000ULL);
    emit_byte(0x66); emit_byte(0x48); emit_byte(0x0F); emit_byte(0x6E); emit_byte(0xD1);
    if (is_double) emit_byte(0x66);
    emit_byte(0x0F); emit_byte(0x2E); emit_byte(0xC2);
    size_t keep = emit_jcc_rel32_placeholder(0x2); // below or unordered (NaN/inf)
    emit_mov_imm64(RCX, is_double ? 0x8000000000000000ULL : 0x80000000ULL);
    emit_and_reg(RAX, RCX);
    emit_mov_imm64(RCX, is_double ? 0x3FF0000000000000ULL : 0x3F800000ULL);
    emit_or_reg(RAX, RCX); // signed 1.0
    emit_byte(0x66); emit_byte(0x48); emit_byte(0x0F); emit_byte(0x6E); emit_byte(0xC0);
    emit_byte(is_double ? 0xF2 : 0xF3); emit_byte(0x0F); emit_byte(0x58); emit_byte(0xC8);
    patch_jcc_rel32(keep, static_cast<int32_t>(code_buf_used_ - (keep + 6)));
    emit_byte(0x0F); emit_byte(0x28); emit_byte(0xC1); // XMM0=rounded XMM1
}
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
    // FP arithmetic/conversion/move ops and
    // SIMD/NEON ops are dispatched to compile_ir_fparith() and
    // compile_ir_simd() (defined in FloatArithmetic.cpp and
    // SIMD.cpp) before the residual switch below. Both
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
            flush_invalidate_host_regs((1u << RAX) | (1u << RCX));
            const FpFrintParams fp = inst.fp_frint_params();
            bool is_double = (fp.bits == 64);
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
            //   4 (FRINTI) → 4 (MXCSR, default = nearest)
            //   5 (FRINTX) → 4 (uses FPCR rounding mode)
            //   6 (FRINTA) → truncate + fractional tie adjustment.
            uint8_t x86_mode;
            switch (fp.mode & 0x7) {
                case 0: x86_mode = 0; break;  // N → nearest
                case 1: x86_mode = 2; break;  // P → +inf (ceil)
                case 2: x86_mode = 1; break;  // M → -inf (floor)
                case 3: x86_mode = 3; break;  // Z → truncate
                case 6: x86_mode = 0xFF; break; // A → round away (below)
                default: x86_mode = 4; break; // I/X → current MXCSR
            }
            if (x86_mode == 0xFF) {
                emit_fp_round_away(is_double);
            } else {
                // roundsd xmm0, xmm0, imm8:  66 0F 3A 0B C0 imm8
                // roundss xmm0, xmm0, imm8:  66 0F 3A 0A C0 imm8
                emit_byte(0x66); emit_byte(0x0F); emit_byte(0x3A);
                emit_byte(is_double ? 0x0B : 0x0A);
                emit_byte(0xC0);  // xmm0, xmm0
                emit_byte(x86_mode);
            }
            // NaNs are quieted like Arm; FPCR.DN needs the interpreter step.
            const size_t special = emit_fp_nan_check(is_double);
            // Store result (fp_store_operand: pinned dest → reg-reg + dirty, else
            // memory) and zero v_hi.
            fp_store_operand(0, inst.dest, is_double);
            fp_zero_hi(inst.dest);
            emit_fp_cold_tail(inst, {special});
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
        //   FMSUB  → vfnmadd231ss/sd  (xmm0 = -Vn*Vm + Va)
        //   FNMADD → vfnmsub231ss/sd  (xmm0 = -Vn*Vm - Va)
        //   FNMSUB → vfmsub231ss/sd   (xmm0 = +Vn*Vm - Va)
        //
        // VEX 3-byte encoding (FMA3 uses 0F38 escape map):
        //   C4 [R~ X~ B~ mmmmm] [W vvvv~ L pp] [opcode] [modrm]
        //   W selects sd (1) or ss (0); vvvv = Vn's XMM; L=0; pp=01.
        //
        // Opcodes (per Intel SDM Vol 2A, FMA3 231 forms):
        //   vfmadd231ss/sd:  0xB9    vfmsub231ss/sd:  0xBB
        //   vfnmadd231ss/sd: 0xBD    vfnmsub231ss/sd: 0xBF
        //
        // Hosts without FMA3 step the interpreter: separate multiply and
        // add would round twice.
        //
        // The clobber list (RAX, RCX, RDX) matches the existing FP
        // codegen convention — FP ops only touch XMM0/XMM1/XMM2 plus
        // those three GPRs (RAX for the zero store at the end).
        case IROp::FMADD:
        case IROp::FMSUB:
        case IROp::FNMADD:
        case IROp::FNMSUB: {
            // Without FMA3 there is no single-rounding host operation.
            if (!has_fma3()) {
                emit_call_interp(inst.arm_pc, false);
                return false;
            }
            clobber_flags();
            flush_invalidate_host_regs((1u << RAX) | (1u << RCX) | (1u << RDX));
            // Width encoding: 64 = double (D), 32 = single (S); the
            // translator emits `ftype ? 64 : 32` for FMADD/FRINT.
            const FpFusedParams fp = inst.fp_fused_params();
            const bool is_double = (fp.bits == 64);
            uint8_t opcode;
            switch (inst.op) {
                case IROp::FMADD:  opcode = 0xB9; break;  // vfmadd231   (acc + prod)
                case IROp::FMSUB:  opcode = 0xBD; break;  // vfnmadd231  (acc - prod)
                case IROp::FNMADD: opcode = 0xBF; break;  // vfnmsub231  (-acc - prod)
                default:           opcode = 0xBB; break;  // vfmsub231   (prod - acc)
            }
            // 231 form into scratch XMM0 = Va; pinned Vn/Vm are read in
            // place. Vd is written only after the NaN check, so the
            // interpreter step still sees every source, aliases included.
            const int xs1 = fp_resolve_operand(inst.src1, 1, is_double);
            const int xs2 = fp_resolve_operand(inst.src2, 2, is_double);
            fp_load_operand(0, fp.acc, is_double);
            // VEX.128.66.0F38.W[is_double]: vfm*231ss/sd xmm0, xmmS1, xmmS2
            // (FMA3 uses pp=01 for BOTH ss and sd — the W bit selects width).
            emit_vex3(2, is_double, xs1, 1, 0, xs2, true, opcode);
            // Arm NaN priority, +qNaN for invalid operations and FPCR.DN.
            const size_t special = emit_fp_nan_check(is_double);
            fp_store_operand(0, inst.dest, is_double);
            fp_zero_hi(inst.dest);
            emit_fp_cold_tail(inst, {special});
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
