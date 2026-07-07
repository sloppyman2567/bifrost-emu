// jit/jit_codegen_fparith.cpp — FrostJIT FP arithmetic / conversion IR-op
// codegen.
//
// v1.4.5-alpha (Turn 69): split out of jit_codegen_fp.cpp. This file holds
// the FP arithmetic / conversion / move case bodies of the FP/SIMD IR-op
// switch, extracted into a separate method (compile_ir_fparith) for
// readability. The compile_ir_inst_fp_() dispatcher in jit_codegen_fp.cpp
// calls this method before its residual cases.
//
// No behavior change — pure file split. The method is a member of FrostJIT
// (declared in include/jit/frostjit.hpp) so it has full access to the JIT's
// emit_*, alloc_*, flush_*, etc. helpers.
//
// Return value (bool — see frostjit.hpp):
//   true  = op handled here (caller returns false; FP ops never end a block)
//   false = op not handled here (caller falls through to the next
//           dispatcher or the residual switch in compile_ir_inst_fp_())
//
// Cases handled:
//   FMOV_G2F / FMOV_F2G / FMOV_G2FHI / FMOV_FHI2G — GPR↔FP moves
//   FP_BINOP     — scalar FP add/sub/mul/div/max/min/fnmul (SSE2)
//   FP_UNOP      — scalar FP mov/abs/neg/sqrt (SSE2)
//   FP_F2I       — FP→int conversion (FCVTZS/FCVTZU)
//   FP_I2F       — int→FP conversion (SCVTF/UCVTF)
//   FP_F2I_FIXED — fixed-point FP→int (FCVTZS/FCVTZU with scale)
//   FP_I2F_FIXED — fixed-point int→FP (SCVTF/UCVTF with scale)
//   FP_CMP       — FCMP/FCMPE (UCOMISD/UCOMISS)
//   FP_MOVI      — load decoded FP immediate
//   FCVT_S2D / FCVT_D2S — single↔double conversion
#include "jit/frostjit.hpp"
#include "core/emulator.h"
#include "ir/ir.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>

namespace arm64emu {

// ── FrostJIT::compile_ir_fparith ──────────────────────────────────────
// Handles all scalar FP arithmetic / conversion / move IR ops. Returns
// true if the op was handled, false if not (caller falls through to the
// next dispatcher or residual switch). The case bodies below are verbatim
// from jit_codegen_fp.cpp (Turn 36 era) — no logic changes, just moved
// to a separate file/method. The original `return false;` (meaning
// "block does not end") becomes `return true;` here (meaning "handled,
// doesn't end a block").
bool FrostJIT::compile_ir_fparith(const IRInst& inst) {
    switch (inst.op) {
        // ── FMOV (general ↔ FP) — native codegen via emit_fmov_helper ───
        // These ops move data between cpu.regs[] and cpu.v_lo[]/v_hi[]
        // using direct memory access through CPU_REG (RBX).
        // No CALL_INTERP needed — pure memory moves through RAX.
        case IROp::FMOV_G2F:    emit_fmov_helper(/*dir=*/0, /*field=*/0, inst.dest, inst.src1, inst.dest); return true;
        case IROp::FMOV_F2G:    emit_fmov_helper(/*dir=*/1, /*field=*/0, inst.src1, inst.src1, inst.dest); return true;
        case IROp::FMOV_G2FHI:  emit_fmov_helper(/*dir=*/0, /*field=*/1, inst.dest, inst.src1, inst.dest); return true;
        case IROp::FMOV_FHI2G:  emit_fmov_helper(/*dir=*/1, /*field=*/1, inst.src1, inst.src1, inst.dest); return true;

        // ── FP scalar arithmetic — native SSE2 codegen ──────────────
        // These ops use XMM0/XMM1 as scratch, loading from and storing
        // to v_lo[]/v_hi[] via CPU_REG (RBX). They don't interact with
        // the GPR register allocator at all.
        //
        // replaced flush_all_vregs()+invalidate_all_vregs()
        // (O(max_vreg_) per op) with flush_invalidate_host_regs({RAX})
        // (O(1) per op). FP_BINOP only clobbers RAX (for the FNMUL sign
        // mask and the v_hi[dest]=0 zero store). Callee-saved vregs in
        // R12/R13/R15 are preserved.
        case IROp::FP_BINOP: {
            // v_lo[dest] = op(v_lo[src1], v_lo[src2]); v_hi[dest] = 0
            bool is_double = (inst.width == 1);
            uint8_t ld_prefix = is_double ? 0xF2 : 0xF3;  // MOVSD/MOVSS
            clobber_flags();
            // FP_BINOP only clobbers RAX (zero store to
            // v_hi[dest]; sign mask for FNMUL). XMM0/XMM1 are scratch and
            // don't hold vregs. Use targeted flush for O(1) instead of
            // O(max_vreg_) flush_all+invalidate_all.
            flush_invalidate_host_regs(1u << RAX);

            // Load src1 into XMM0: movsd/movss xmm0, [rbx+off]
            // BUGFIX: no REX needed — SSE regs are 0-7, RBX is 3.
            // REX.R would extend xmm1 to xmm9, breaking the op.
            int32_t off1 = V_LO_OFF + static_cast<int>(inst.src1) * 8;
            emit_byte(ld_prefix);
            emit_byte(0x0F); emit_byte(0x10);
            emit_modrm_disp(0, CPU_REG, off1);

            // Load src2 into XMM1: movsd/movss xmm1, [rbx+off]
            int32_t off2 = V_LO_OFF + static_cast<int>(inst.src2) * 8;
            emit_byte(ld_prefix);
            emit_byte(0x0F); emit_byte(0x10);
            emit_modrm_disp(1, CPU_REG, off2);

            // Execute SSE2 op
            uint8_t opc = static_cast<uint8_t>(inst.imm);
            uint8_t sse_op;
            switch (opc) {
                case 0: sse_op = 0x59; break;  // mul (mulsd)
                case 1: sse_op = 0x5E; break;  // div (divsd)
                case 2: sse_op = 0x58; break;  // add (addsd)
                case 3: sse_op = 0x5C; break;  // sub (subsd)
                case 4: sse_op = 0x5F; break;  // max (maxsd)
                case 5: sse_op = 0x5D; break;  // min (minsd)
                default:
                    // Unknown FP opcode — fall back to interpreter instead
                    // of silently emitting ADDSD (which would produce wrong
                    // results). This shouldn't happen (the IR translator
                    // only emits opcodes 0-6), but defensive coding here
                    // prevents silent miscompilation if a new opcode is
                    // added to the translator without updating this switch.
                    emit_call_interp(inst.arm_pc, false);
                    return true;
            }
            // Execute SSE2 op: ADDSD/MULSD/etc xmm0, xmm1 → xmm0 = xmm0 OP xmm1
            // BUGFIX: must use modrm(3, 0, 1) → reg=xmm0, rm=xmm1
            // The old code used modrm(3, 1, 0) with REX.R which encoded
            // ADDSD xmm1, xmm0 (result in xmm1) but stored xmm0 (stale).
            emit_byte(ld_prefix);
            emit_byte(0x0F); emit_byte(sse_op);
            emit_byte(modrm(3, 0, 1));  // xmm0, xmm1

            if (opc == 6) {  // FNMUL: negate
                emit_mov_imm64(RAX, 0x8000000000000000ULL);
                emit_byte(0x66); emit_byte(0x48); emit_byte(0x0F); emit_byte(0x6E); emit_byte(0xC8);
                emit_byte(0x66); emit_byte(0x0F); emit_byte(0x57); emit_byte(0xC1);
            }

            // Store result: movsd/movss [rbx+off], xmm0
            int32_t off_d = V_LO_OFF + static_cast<int>(inst.dest) * 8;
            emit_byte(ld_prefix);
            emit_byte(0x0F); emit_byte(0x11);
            emit_modrm_disp(0, CPU_REG, off_d);

            // Zero v_hi[dest]
            emit_mov_imm32_zext(RAX, 0);
            emit_store(CPU_REG, V_HI_OFF + static_cast<int>(inst.dest) * 8, RAX);
            return true;
        }

        case IROp::FP_UNOP: {
            bool is_double = (inst.width == 1);
            uint8_t prefix = is_double ? 0xF2 : 0xF3;
            clobber_flags();
            // FP_UNOP clobbers only RAX (sign mask for FABS/FNEG; zero store).
            // RCX/RDX are not touched — don't flush them.
            flush_invalidate_host_regs(1u << RAX);

            int32_t off1 = V_LO_OFF + static_cast<int>(inst.src1) * 8;
            emit_byte(prefix);
            emit_byte(0x0F); emit_byte(0x10);
            emit_modrm_disp(0, CPU_REG, off1);

            uint8_t opc = static_cast<uint8_t>(inst.imm);
            if (opc == 0) {
                // FMOV — no-op
            } else if (opc == 1) {
                // FABS
                emit_mov_imm64(RAX, 0x7FFFFFFFFFFFFFFFULL);
                emit_byte(0x66); emit_byte(0x48); emit_byte(0x0F); emit_byte(0x6E); emit_byte(0xC8);
                emit_byte(0x66); emit_byte(0x0F); emit_byte(0x54); emit_byte(0xC1);
            } else if (opc == 2) {
                // FNEG
                emit_mov_imm64(RAX, 0x8000000000000000ULL);
                emit_byte(0x66); emit_byte(0x48); emit_byte(0x0F); emit_byte(0x6E); emit_byte(0xC8);
                emit_byte(0x66); emit_byte(0x0F); emit_byte(0x57); emit_byte(0xC1);
            } else if (opc == 3) {
                // FSQRT
                emit_byte(prefix);
                emit_byte(0x0F); emit_byte(0x51);
                emit_byte(modrm(3, 0, 0));
            }

            int32_t off_d = V_LO_OFF + static_cast<int>(inst.dest) * 8;
            emit_byte(prefix);
            emit_byte(0x0F); emit_byte(0x11);
            emit_modrm_disp(0, CPU_REG, off_d);

            emit_mov_imm32_zext(RAX, 0);
            emit_store(CPU_REG, V_HI_OFF + static_cast<int>(inst.dest) * 8, RAX);
            return true;
        }

        // ── FP→int conversion (FCVTZS/FCVTZU) ──────────────────────
        case IROp::FP_F2I: {
            // regs[dest] = (int/uint)(v_lo[src1])
            // proper unsigned conversion via the
            // "subtract 2^63, convert signed, add 2^63" trick.
            //
            // sf (flags_op) is unused by the JIT — the JIT always emits
            // 64-bit CVTTSD2SI rax. 32-bit dest truncation (sf=0) is
            // handled by an explicit IROp::ZEXT emitted by ir_translate.cpp
            // after FP_F2I, which zero-extends the low 32 bits per AArch64
            // 32-bit register write semantics.
            //
            // For the unsigned path, the 2^63 constant must match the FP
            // precision: 0x43E0000000000000 (double) for width=1, or
            // 0x5F000000 (float) for width=0. Using the double constant
            // with single-precision ucomiss/subss reads only the low 32
            // bits (0x00000000 = 0.0f), breaking the >= 2^63 detection.
            bool is_double = (inst.width == 1);
            bool is_unsigned = (inst.imm != 0);
            // Validate FP register index (src1 is an FP reg index 0-31).
            check_fp_reg_index(inst.src1, "FP_F2I src1");
            clobber_flags();
            // FP_F2I clobbers RAX (CVTTSD2SI result) and, in the unsigned
            // path, RCX (2^63 constant). Flush+invalidate both.
            flush_invalidate_host_regs((1u << RAX) | (1u << RCX) | (1u << RDX));

            // Load FP value into XMM0
            int32_t off1 = V_LO_OFF + static_cast<int>(inst.src1) * 8;
            uint8_t prefix = is_double ? 0xF2 : 0xF3;
            emit_byte(prefix); emit_byte(0x0F); emit_byte(0x10);
            emit_modrm_disp(0, CPU_REG, off1);

            if (is_unsigned) {
                // Unsigned conversion: x86 lacks CVTTSD2USI, so we use:
                //   if (xmm0 >= 2^63) { xmm0 -= 2^63; CVTTSD2SI rax; rax += 2^63 }
                //   else                CVTTSD2SI rax
                // Use RCX for the comparison constant.
                // 2^63 in the matching FP precision.
                uint64_t pow63 = is_double ? 0x43E0000000000000ULL
                                           : 0x5F000000ULL;
                // mov rcx, pow63
                emit_mov_imm64(RCX, pow63);
                // movq xmm1, rcx
                emit_byte(0x66); emit_byte(0x48); emit_byte(0x0F); emit_byte(0x6E); emit_byte(0xC9);
                // ucomisd/iss xmm0, xmm1 (compare src against 2^63)
                // NOTE: ucomisd takes the 0x66 prefix, ucomiss takes NO
                // mandatory prefix. Using 0xF2/0xF3 here (as we do for
                // cvtsi2sd/ss) would generate invalid instruction encodings
                // on some CPUs and crash with SIGILL. The SSE/SSE2 prefix
                // conventions are NOT uniform across instructions.
                if (is_double) emit_byte(0x66);
                emit_byte(0x0F); emit_byte(0x2E); emit_byte(0xC1);
                // jae .large (CF=0 means src >= 2^63)
                size_t jae_patch = emit_jcc_rel32_placeholder(0x3);  // JAE rel32
                // CVTTSD2SI rax, xmm0 (small path)
                emit_byte(prefix); emit_byte(0x48); emit_byte(0x0F); emit_byte(0x2C);
                emit_byte(0xC0);  // rax, xmm0
                // jmp .done
                size_t jmp_done = emit_jmp_rel32_placeholder();
                size_t large_path = code_buf_used_;
                patch_jcc_rel32(jae_patch, static_cast<int32_t>(large_path - (jae_patch + 6)));
                // subsd/ss xmm0, xmm1
                emit_byte(prefix); emit_byte(0x0F); emit_byte(0x5C); emit_byte(0xC1);
                // CVTTSD2SI rax, xmm0
                emit_byte(prefix); emit_byte(0x48); emit_byte(0x0F); emit_byte(0x2C);
                emit_byte(0xC0);  // rax, xmm0
                // add rax, 0x8000000000000000 (using mov + add to avoid imm64 in add)
                emit_mov_imm64(RCX, 0x8000000000000000ULL);
                emit_byte(0x48); emit_byte(0x01); emit_byte(0xC8);  // add rax, rcx
                size_t done_path = code_buf_used_;
                patch_jmp_rel32(jmp_done, static_cast<int32_t>(done_path - (jmp_done + 5)));
            } else {
                // CVTTSD2SI rax, xmm0 (truncate toward zero, signed)
                emit_byte(prefix); emit_byte(0x48); emit_byte(0x0F); emit_byte(0x2C);
                emit_byte(0xC0);  // rax, xmm0
            }

            // Store result to cpu.regs[dest]
            store_reg_to_vreg(inst.dest, RAX);
            return true;
        }

        // ── int→FP conversion (SCVTF/UCVTF) ────────────────────────
        case IROp::FP_I2F: {
            // v_lo[dest] = (float/double)(regs[src1]); v_hi=0
            // proper unsigned conversion via the
            // "if (src >= 2^63) subtract 2^63, convert signed, add 2^63
            //  to result as double" trick.
            //
            // sf (flags_op) selects the source GPR width:
            //   sf=0 → 32-bit GPR (Wn)
            //   sf=1 → 64-bit GPR (Xn)
            // For SIGNED conversion (SCVTF) with sf=0, we use the 32-bit
            // CVTSI2SD/SS form (no REX.W) so eax is interpreted as int32.
            // For UNSIGNED conversion (UCVTF) with sf=0, we MUST use the
            // 64-bit form (REX.W) because the 32-bit value has been zero-
            // extended to 64 bits in the register, and interpreting it as
            // int64 gives the correct unsigned value (uint32 < 2^63).
            // Using the 32-bit form for UCVTF would treat eax as int32,
            // turning 0xFFFFFFFF (uint32 max = 4294967295) into -1 and
            // producing -1.0f instead of 4.29e+09.
            //
            // For the unsigned path, the 2^63 addend must match the FP
            // precision: 0x43E0000000000000 (double) for width=1, or
            // 0x5F000000 (float) for width=0. Using the double constant
            // with addss reads only the low 32 bits (0x00000000 = 0.0f),
            // silently losing the 2^63 correction.
            bool is_double = (inst.width == 1);
            bool is_unsigned = (inst.imm != 0);
            bool is_64bit_src = (inst.flags_op != 0);
            // Validate FP register index (dest is an FP reg index 0-31).
            check_fp_reg_index(inst.dest, "FP_I2F dest");
            // REX.W prefix: 64-bit form when source is 64-bit OR when
            // unsigned (so the zero-extended 32-bit value is read as
            // positive int64).
            uint8_t rex_w = (is_64bit_src || is_unsigned) ? 0x48 : 0x00;
            clobber_flags();
            // FP_I2F clobbers RAX (GPR load), RCX (subtract flag), and
            // RDX (2^63 constant) in the unsigned path. Flush+invalidate
            // all three.
            flush_invalidate_host_regs((1u << RAX) | (1u << RCX) | (1u << RDX));

            // Load GPR into RAX
            load_vreg_to_reg(RAX, inst.src1);

            uint8_t prefix = is_double ? 0xF2 : 0xF3;
            if (is_unsigned) {
                // Unsigned: if (rax >= 2^63) { rcx = 1; sub rax, 2^63 } else rcx = 0
                // CVTSI2SD xmm0, rax (signed convert of the adjusted value)
                // if (rcx) addsd xmm0, [2^63 as double]
                emit_mov_imm32_zext(RCX, 0);
                // cmp rax, 0x8000000000000000
                emit_mov_imm64(RDX, 0x8000000000000000ULL);
                emit_byte(0x48); emit_byte(0x39); emit_byte(0xD0);  // cmp rax, rdx
                // jb .small (CF=1 means rax < 2^63)
                size_t jb_patch = emit_jcc_rel32_placeholder(0x2);  // JB rel32
                // Fall-through (rax >= 2^63): sub rax, 2^63 (rax -= rdx)
                emit_byte(0x48); emit_byte(0x29); emit_byte(0xD0);  // sub rax, rdx
                emit_mov_imm32_zext(RCX, 1);  // mark that we subtracted
                size_t small_path = code_buf_used_;
                patch_jcc_rel32(jb_patch, static_cast<int32_t>(small_path - (jb_patch + 6)));
                // CVTSI2SD/SS xmm0, rax (or eax for 32-bit source)
                emit_byte(prefix);
                if (rex_w) emit_byte(rex_w);
                emit_byte(0x0F); emit_byte(0x2A);
                emit_byte(0xC0);  // xmm0, rax
                // if (rcx != 0) add 2^63 as double/single
                emit_byte(0x48); emit_byte(0x85); emit_byte(0xC9);  // test rcx, rcx
                size_t jz_patch = emit_jcc_rel32_placeholder(0x4);  // JZ rel32
                // 2^63 in the matching FP precision.
                //   double: 0x43E0000000000000
                //   single: 0x5F000000 (low 32 bits of xmm1)
                uint64_t pow63 = is_double ? 0x43E0000000000000ULL
                                           : 0x5F000000ULL;
                emit_mov_imm64(RDX, pow63);
                emit_byte(0x66); emit_byte(0x48); emit_byte(0x0F); emit_byte(0x6E); emit_byte(0xCA);  // movq xmm1, rdx
                // addsd/addss xmm0, xmm1
                emit_byte(prefix); emit_byte(0x0F); emit_byte(0x58); emit_byte(0xC1);
                size_t done_path = code_buf_used_;
                patch_jcc_rel32(jz_patch, static_cast<int32_t>(done_path - (jz_patch + 6)));
            } else {
                // CVTSI2SD/SS xmm0, rax (or eax for 32-bit source)
                emit_byte(prefix);
                if (rex_w) emit_byte(rex_w);
                emit_byte(0x0F); emit_byte(0x2A);
                emit_byte(0xC0);  // xmm0, rax
            }

            // Store to v_lo[dest]
            int32_t off_d = V_LO_OFF + static_cast<int>(inst.dest) * 8;
            emit_byte(prefix); emit_byte(0x0F); emit_byte(0x11);
            emit_modrm_disp(0, CPU_REG, off_d);

            // Zero v_hi[dest]
            emit_mov_imm32_zext(RAX, 0);
            emit_store(CPU_REG, V_HI_OFF + static_cast<int>(inst.dest) * 8, RAX);
            return true;
        }

        // ── fixed-point FP→int (FCVTZS/FCVTZU with scale) ───────────
        case IROp::FP_F2I_FIXED: {
            // Semantics: scale FP value by 2^fbits, truncate toward zero,
            // saturate to dest range, NaN → 0.
            //
            // Codegen: load FP into XMM0, multiply by 2^fbits (as double;
            // single-precision sources are promoted to double for precision),
            // then reuse the integer-variant FP_F2I saturating truncation.
            // For NaN inputs, ucomisd xmm0, xmm0 clears ZF/Parity iff NaN;
            // we detect and force result to 0.
            //
            // fbits range is 1..64. For fbits=64, 2^64 as double overflows
            // to +inf, and `scaled = a * inf` is ±inf or NaN — the saturate
            // path clamps to INT_MAX/UINT_MAX, matching the interpreter.
            bool is_double = (inst.width == 1);
            bool is_unsigned = (inst.imm != 0);
            bool is_64bit_dest = (inst.flags_op != 0);
            int fbits = inst.immr ? static_cast<int>(inst.immr) : 64;
            check_fp_reg_index(inst.src1, "FP_F2I_FIXED src1");
            clobber_flags();
            flush_invalidate_host_regs((1u << RAX) | (1u << RCX) | (1u << RDX));

            // Compute 2^fbits as a double constant in RCX → movq xmm1.
            // std::ldexp(1.0, fbits) gives the exact double; we materialize
            // it as a 64-bit immediate. For fbits=64, ldexp gives +inf
            // (0x7FF0000000000000), which is the correct scale: any finite
            // non-zero `a` becomes ±inf, and the saturate path clamps.
            double scale = std::ldexp(1.0, fbits);
            uint64_t scale_bits;
            memcpy(&scale_bits, &scale, 8);
            emit_mov_imm64(RCX, scale_bits);
            // movq xmm1, rcx
            emit_byte(0x66); emit_byte(0x48); emit_byte(0x0F); emit_byte(0x6E); emit_byte(0xC9);

            // Load FP value into XMM0 (as double; promote single via cvtss2sd).
            int32_t off1 = V_LO_OFF + static_cast<int>(inst.src1) * 8;
            if (is_double) {
                emit_byte(0xF2); emit_byte(0x0F); emit_byte(0x10);
                emit_modrm_disp(0, CPU_REG, off1);
            } else {
                // movss xmm0, [cpu+off]
                emit_byte(0xF3); emit_byte(0x0F); emit_byte(0x10);
                emit_modrm_disp(0, CPU_REG, off1);
                // cvtss2sd xmm0, xmm0 (promote to double)
                emit_byte(0xF3); emit_byte(0x0F); emit_byte(0x5A); emit_byte(0xC0);
            }
            // mulsd xmm0, xmm1 (scale by 2^fbits)
            emit_byte(0xF2); emit_byte(0x0F); emit_byte(0x59); emit_byte(0xC1);

            // NaN check: ucomisd xmm0, xmm0 sets PF=1 iff NaN/unordered.
            // JNP (cc=0xB) jumps when PF=0 (not NaN). Note: cc=0x5 is JNE,
            // NOT JNP — see the cc table in x86_backend.cpp.
            emit_byte(0x66); emit_byte(0x0F); emit_byte(0x2E); emit_byte(0xC0);
            size_t jnp_patch = emit_jcc_rel32_placeholder(0xB);  // JNP (not parity): not NaN
            // NaN path: set rax = 0, jump to store.
            emit_mov_imm32_zext(RAX, 0);
            size_t jmp_done = emit_jmp_rel32_placeholder();
            // Not-NaN path: truncate + saturate (reuse FP_F2I codegen pattern).
            size_t notnan_path = code_buf_used_;
            patch_jcc_rel32(jnp_patch, static_cast<int32_t>(notnan_path - (jnp_patch + 6)));

            // The saturating conversion uses CVTTSD2SI rax, xmm0 (signed
            // 64-bit truncation), then clamps. For unsigned dest, we use
            // the subtract-2^63 trick on the (already-scaled) double.
            if (is_unsigned) {
                // 2^63 in double precision.
                emit_mov_imm64(RCX, 0x43E0000000000000ULL);
                emit_byte(0x66); emit_byte(0x48); emit_byte(0x0F); emit_byte(0x6E); emit_byte(0xC9);
                // ucomisd xmm0, xmm1
                emit_byte(0x66); emit_byte(0x0F); emit_byte(0x2E); emit_byte(0xC1);
                size_t jae_patch = emit_jcc_rel32_placeholder(0x3);  // JAE
                // small path: cvttsd2si rax, xmm0
                emit_byte(0xF2); emit_byte(0x48); emit_byte(0x0F); emit_byte(0x2C); emit_byte(0xC0);
                size_t jmp_small = emit_jmp_rel32_placeholder();
                size_t large_path = code_buf_used_;
                patch_jcc_rel32(jae_patch, static_cast<int32_t>(large_path - (jae_patch + 6)));
                // subsd xmm0, xmm1; cvttsd2si rax; add rax, 2^63
                emit_byte(0xF2); emit_byte(0x0F); emit_byte(0x5C); emit_byte(0xC1);
                emit_byte(0xF2); emit_byte(0x48); emit_byte(0x0F); emit_byte(0x2C); emit_byte(0xC0);
                emit_mov_imm64(RCX, 0x8000000000000000ULL);
                emit_byte(0x48); emit_byte(0x01); emit_byte(0xC8);  // add rax, rcx
                size_t done_unsigned = code_buf_used_;
                patch_jmp_rel32(jmp_small, static_cast<int32_t>(done_unsigned - (jmp_small + 5)));
            } else {
                // Signed: cvttsd2si rax, xmm0 (saturates implicitly per x86 semantics).
                emit_byte(0xF2); emit_byte(0x48); emit_byte(0x0F); emit_byte(0x2C); emit_byte(0xC0);
            }

            // Saturate to dest range.
            if (!is_64bit_dest) {
                // 32-bit dest: clamp to [INT32_MIN, INT32_MAX] for signed,
                // [0, UINT32_MAX] for unsigned. x86's cvttsd2si already
                // returns INT32_MIN (0x80000000) on out-of-range for 32-bit
                // form, but we used 64-bit form. Manual clamp needed.
                if (is_unsigned) {
                    // Clamp negative → 0, > UINT32_MAX → UINT32_MAX.
                    // Code layout (after the test rax,rax):
                    //   JS negative_path           ; if rax < 0, jump to negative path
                    //   ; non-negative path:
                    //   cmp rax, 0xFFFFFFFF
                    //   JBE ok_path                ; if rax <= UINT32_MAX, ok
                    //   mov eax, 0xFFFFFFFF        ; clamp to UINT32_MAX
                    //   jmp done                   ; skip negative path
                    // ok_path:
                    //   jmp done                   ; (fall-through to done, skip negative)
                    // negative_path:
                    //   mov eax, 0                 ; clamp negative to 0
                    // done:
                    //
                    // The previous code had a bug: `after_neg_target` was set
                    // BEFORE the negative path's `mov eax, 0` was emitted, so
                    // it pointed AT the negative path. The `jmp_past_neg`
                    // (intended to skip the negative path) landed ON it,
                    // causing every non-negative result to be overwritten
                    // with 0. This was the "first FCVTZU produces 0" bug.
                    emit_byte(0x48); emit_byte(0x85); emit_byte(0xC0);  // test rax, rax
                    size_t js_patch = emit_jcc_rel32_placeholder(0x8);  // JS (negative)
                    // Non-negative: cmp rax, 0xFFFFFFFF; jbe ok; else clamp.
                    emit_mov_imm32_zext(RCX, 0xFFFFFFFFu);
                    emit_byte(0x48); emit_byte(0x39); emit_byte(0xC8);  // cmp rax, rcx
                    size_t jbe_patch = emit_jcc_rel32_placeholder(0x6);  // JBE
                    // Clamp to UINT32_MAX.
                    emit_mov_imm32_zext(RAX, 0xFFFFFFFFu);
                    // Skip the negative path (jump to done, which is AFTER
                    // the negative path's `mov eax, 0`).
                    size_t jmp_past_neg = emit_jmp_rel32_placeholder();
                    // ok_path: rax is already correct (from cvttsd2si).
                    // Also skip the negative path.
                    size_t ok_path = code_buf_used_;
                    patch_jcc_rel32(jbe_patch, static_cast<int32_t>(ok_path - (jbe_patch + 6)));
                    size_t jmp_ok_done = emit_jmp_rel32_placeholder();
                    // negative_path (target of JS): rax = 0.
                    size_t negative_path = code_buf_used_;
                    patch_jcc_rel32(js_patch, static_cast<int32_t>(negative_path - (js_patch + 6)));
                    emit_mov_imm32_zext(RAX, 0);
                    // done: both jmp_past_neg and jmp_ok_done land here.
                    size_t done_target = code_buf_used_;
                    patch_jmp_rel32(jmp_past_neg, static_cast<int32_t>(done_target - (jmp_past_neg + 5)));
                    patch_jmp_rel32(jmp_ok_done, static_cast<int32_t>(done_target - (jmp_ok_done + 5)));
                } else {
                    // Signed 32-bit: clamp to [INT32_MIN, INT32_MAX].
                    // NOTE: INT32_MIN must be sign-extended to 64-bit for the
                    // comparison. `emit_mov_imm32_zext` zero-extends, which
                    // would make 0x80000000 → 0x0000000080000000 (positive
                    // 2147483648) instead of 0xFFFFFFFF80000000 (negative
                    // -2147483648). The 64-bit `cmp rax, rcx` would then
                    // compare against the wrong value, clamping valid
                    // positive results to INT32_MIN. Use emit_mov_imm64 with
                    // the sign-extended value.
                    emit_mov_imm32_zext(RCX, static_cast<uint32_t>(INT32_MAX));
                    emit_byte(0x48); emit_byte(0x39); emit_byte(0xC8);  // cmp rax, rcx
                    size_t jle_patch = emit_jcc_rel32_placeholder(0xE);  // JLE
                    emit_mov_imm32_zext(RAX, static_cast<uint32_t>(INT32_MAX));
                    size_t after_hi = code_buf_used_;
                    patch_jcc_rel32(jle_patch, static_cast<int32_t>(after_hi - (jle_patch + 6)));
                    emit_mov_imm64(RCX, static_cast<uint64_t>(static_cast<int64_t>(INT32_MIN)));
                    emit_byte(0x48); emit_byte(0x39); emit_byte(0xC8);  // cmp rax, rcx
                    size_t jge_patch = emit_jcc_rel32_placeholder(0xD);  // JGE
                    emit_mov_imm32_zext(RAX, static_cast<uint32_t>(INT32_MIN));
                    size_t after_lo = code_buf_used_;
                    patch_jcc_rel32(jge_patch, static_cast<int32_t>(after_lo - (jge_patch + 6)));
                }
            } else {
                // 64-bit dest: cvttsd2si already saturates (INT64_MAX/MIN
                // for out-of-range) for signed. For unsigned, the
                // subtract-2^63 trick already handles the upper half.
                // No explicit clamp needed — matches interpreter semantics.
            }

            size_t done_path = code_buf_used_;
            patch_jmp_rel32(jmp_done, static_cast<int32_t>(done_path - (jmp_done + 5)));

            // Store result to cpu.regs[dest]
            store_reg_to_vreg(inst.dest, RAX);
            return true;
        }

        // ── fixed-point int→FP (SCVTF/UCVTF with scale) ─────────────
        case IROp::FP_I2F_FIXED: {
            // Semantics: convert int to FP, divide by 2^fbits.
            //
            // Codegen: reuse the integer-variant FP_I2F codegen to produce
            // the converted double in XMM0, then multiply by 2^-fbits
            // (= divide by 2^fbits). Single-precision dest: promote to
            // double, multiply, then demote back to single.
            bool is_double = (inst.width == 1);
            bool is_unsigned = (inst.imm != 0);
            bool is_64bit_src = (inst.flags_op != 0);
            int fbits = inst.immr ? static_cast<int>(inst.immr) : 64;
            check_fp_reg_index(inst.dest, "FP_I2F_FIXED dest");
            uint8_t rex_w = (is_64bit_src || is_unsigned) ? 0x48 : 0x00;
            clobber_flags();
            flush_invalidate_host_regs((1u << RAX) | (1u << RCX) | (1u << RDX));

            // Load GPR into RAX
            load_vreg_to_reg(RAX, inst.src1);

            // Convert int→double in XMM0. For single-precision dest, we
            // still convert to double first (for precision), multiply by
            // 2^-fbits in double, then demote to single at the end.
            if (is_unsigned) {
                // Unsigned: subtract-2^63 trick.
                emit_mov_imm32_zext(RCX, 0);
                emit_mov_imm64(RDX, 0x8000000000000000ULL);
                emit_byte(0x48); emit_byte(0x39); emit_byte(0xD0);  // cmp rax, rdx
                size_t jb_patch = emit_jcc_rel32_placeholder(0x2);  // JB
                emit_byte(0x48); emit_byte(0x29); emit_byte(0xD0);  // sub rax, rdx
                emit_mov_imm32_zext(RCX, 1);
                size_t small_path = code_buf_used_;
                patch_jcc_rel32(jb_patch, static_cast<int32_t>(small_path - (jb_patch + 6)));
                // cvtsi2sd xmm0, rax
                emit_byte(0xF2); emit_byte(0x48); emit_byte(0x0F); emit_byte(0x2A); emit_byte(0xC0);
                // if (rcx) add 2^63 as double
                emit_byte(0x48); emit_byte(0x85); emit_byte(0xC9);  // test rcx, rcx
                size_t jz_patch = emit_jcc_rel32_placeholder(0x4);  // JZ
                emit_mov_imm64(RDX, 0x43E0000000000000ULL);
                emit_byte(0x66); emit_byte(0x48); emit_byte(0x0F); emit_byte(0x6E); emit_byte(0xCA);
                emit_byte(0xF2); emit_byte(0x0F); emit_byte(0x58); emit_byte(0xC1);  // addsd
                size_t done_conv = code_buf_used_;
                patch_jcc_rel32(jz_patch, static_cast<int32_t>(done_conv - (jz_patch + 6)));
            } else {
                // Signed: cvtsi2sd xmm0, rax (or eax for 32-bit source)
                emit_byte(0xF2);
                if (rex_w) emit_byte(rex_w);
                emit_byte(0x0F); emit_byte(0x2A); emit_byte(0xC0);
            }

            // Multiply by 2^-fbits (= divide by 2^fbits). Use double precision
            // for the scale to avoid losing bits, even for single dest.
            double inv_scale = std::ldexp(1.0, -fbits);
            uint64_t inv_bits;
            memcpy(&inv_bits, &inv_scale, 8);
            emit_mov_imm64(RCX, inv_bits);
            emit_byte(0x66); emit_byte(0x48); emit_byte(0x0F); emit_byte(0x6E); emit_byte(0xC9);
            // mulsd xmm0, xmm1
            emit_byte(0xF2); emit_byte(0x0F); emit_byte(0x59); emit_byte(0xC1);

            // Store to v_lo[dest] (single-precision: demote first).
            int32_t off_d = V_LO_OFF + static_cast<int>(inst.dest) * 8;
            if (is_double) {
                emit_byte(0xF2); emit_byte(0x0F); emit_byte(0x11);
                emit_modrm_disp(0, CPU_REG, off_d);
            } else {
                // cvtsd2ss xmm0, xmm0 (demote to single)
                emit_byte(0xF2); emit_byte(0x0F); emit_byte(0x5A); emit_byte(0xC0);
                emit_byte(0xF3); emit_byte(0x0F); emit_byte(0x11);
                emit_modrm_disp(0, CPU_REG, off_d);
            }

            // Zero v_hi[dest]
            emit_mov_imm32_zext(RAX, 0);
            emit_store(CPU_REG, V_HI_OFF + static_cast<int>(inst.dest) * 8, RAX);
            return true;
        }

        // ── FP compare (FCMP/FCMPE) ────────────────────────────────
        case IROp::FP_CMP: {
            // Native FCMP/FCMPE using UCOMISD/UCOMISS.
            //
            // ARM FCMP sets NZCV:
            //   unordered (NaN): N=0 Z=0 C=1 V=1
            //   less than:       N=1 Z=0 C=0 V=0
            //   equal:           N=0 Z=1 C=1 V=0
            //   greater than:    N=0 Z=0 C=1 V=0
            //
            // x86 UCOMISD sets:
            //   unordered: PF=1, CF=1, ZF=1
            //   less than: PF=0, CF=1, ZF=0
            //   equal:     PF=0, CF=0, ZF=1
            //   greater:   PF=0, CF=0, ZF=0
            //
            // Translation:
            //   PF=1 (unordered) → pstate = 0x28000000 (C=1, V=1)
            //   else CF=1 (less) → pstate = 0x80000000 (N=1)
            //   else ZF=1 (equal) → pstate = 0x60000000 (Z=1, C=1)
            //   else (greater) → pstate = 0x20000000 (C=1)
            //
            // We use conditional sets (setcc) to build pstate in RDX,
            // then store to cpu.pstate.
            bool is_double = (inst.width == 1);
            clobber_flags();
            // FP_CMP clobbers RAX, RCX, RDX (flag manipulation).
            flush_invalidate_host_regs((1u << RAX) | (1u << RCX) | (1u << RDX));

            // Load src1 into XMM0
            int32_t off1 = V_LO_OFF + static_cast<int>(inst.src1) * 8;
            uint8_t prefix = is_double ? 0xF2 : 0xF3;
            emit_byte(prefix); emit_byte(0x0F); emit_byte(0x10);
            emit_modrm_disp(0, CPU_REG, off1);

            // Load src2 into XMM1 (or zero for FCMP #0.0).
            //
            // The IR translator marks the #0.0 form by setting bit 0 of
            // inst.imm (sentinel). Without this sentinel, FCMP Dn, D0
            // (register form with rm==0) would be indistinguishable from
            // FCMP Dn, #0.0 (zero form), because both have IR src2 == 0.
            //
            // Register form: src2 = rm (0..30). Load v_lo[src2] into XMM1.
            // Zero form: src2 = 0, imm bit 0 = 1. Use xorps to zero XMM1.
            bool with_zero = (inst.imm & 1) != 0;
            if (!with_zero) {
                int32_t off2 = V_LO_OFF + static_cast<int>(inst.src2) * 8;
                emit_byte(prefix); emit_byte(0x0F); emit_byte(0x10);
                emit_modrm_disp(1, CPU_REG, off2);
            } else {
                // FCMP Dn, #0.0 — XORPS xmm1, xmm1 to get 0.0
                emit_byte(0x0F); emit_byte(0x57); emit_byte(0xC9); // xorps xmm1, xmm1
            }

            // UCOMISD/UCOMISS xmm0, xmm1
            // Encoding: UCOMISD = 66 0F 2E /r ; UCOMISS = NP 0F 2E /r
            // (NOT F2/F3 — those prefixes are for arithmetic ops like ADDSD,
            // not for compare ops. Using F2/F3 here emits an illegal
            // instruction that raises SIGILL on real hardware.)
            if (is_double) emit_byte(0x66);
            emit_byte(0x0F); emit_byte(0x2E);
            emit_byte(0xC1);  // xmm0, xmm1

            // Build pstate in RDX using conditional moves.
            // pushfq to get flags into RAX, then test bits.
            emit_pushfq();
            emit_byte(0x58);  // pop rax (flags in rax)

            // RDX = 0 (default)
            emit_xor_reg(RDX, RDX);

            // Save rax (flags image) — we need it for multiple tests.
            emit_byte(0x50);  // push rax

            // Fixed flag conversion.
            // x86 UCOMISD sets: unordered (PF=1,CF=1,ZF=1), less (CF=1),
            //                    equal (ZF=1), greater (none).
            // We use a priority chain: check PF first (unordered), then
            // CF (less), then ZF (equal), else greater. Each cmovne only
            // fires if RDX is still 0 (i.e., no higher-priority case matched).
            // We use cmovne (ZF=0) because `test` sets ZF=0 when the bit
            // IS set (i.e., the flag IS 1).
            //
            // But the old chain had a bug: if unordered (PF=1,CF=1,ZF=1),
            // all three cmovne would fire, and the last one (equal=0x60000000)
            // would overwrite the correct unordered value (0x30000000).
            //
            // Fix: use a priority chain where each cmov only fires if RDX
            // is still 0. We do this by checking RDX after each set.
            //
            // Simpler approach: use conditional jumps (je/jne) to build
            // a proper if-else chain. This is slightly more code but
            // unambiguously correct.

            // Default: RDX = 0x20000000 (greater → C=1)
            emit_mov_imm32_zext(RDX, 0x20000000);

            // Fixed flag conversion using clean if-else chain.
            // After `test rax, bit`:
            //   ZF=1 iff (rax & bit) == 0 (flag bit is 0)
            //   ZF=0 iff (rax & bit) != 0 (flag bit is 1)
            // JNZ (0x5) jumps when ZF=0, i.e., when the flag bit IS set.
            // We use JNZ to jump OVER the value-set block when the condition
            // is NOT met (flag bit is 0), so the default/previous value stays.

            // if PF=1 (unordered): RDX = 0x30000000, then jmp done
            emit_byte(0x48); emit_byte(0xA9); emit_u32(0x04); // test rax, 4 (PF bit)
            size_t jz_skip1 = emit_jcc_rel32_placeholder(0x4);  // JZ: PF=0, skip
            emit_mov_imm32_zext(RDX, 0x30000000);
            size_t jmp_done1 = emit_jmp_rel32_placeholder();
            size_t after_pf = code_buf_used_;
            patch_jcc_rel32(jz_skip1, static_cast<int32_t>(after_pf - (jz_skip1 + 6)));

            // if CF=1 (less): RDX = 0x80000000 (N=1)
            emit_byte(0x48); emit_byte(0xA9); emit_u32(0x01); // test rax, 1 (CF)
            size_t jz_skip2 = emit_jcc_rel32_placeholder(0x4);  // JZ: CF=0, skip
            emit_mov_imm32_zext(RDX, 0x80000000);
            size_t jmp_done2 = emit_jmp_rel32_placeholder();
            size_t after_cf = code_buf_used_;
            patch_jcc_rel32(jz_skip2, static_cast<int32_t>(after_cf - (jz_skip2 + 6)));

            // if ZF=1 (equal): RDX = 0x60000000 (Z=1, C=1)
            emit_byte(0x48); emit_byte(0xA9); emit_u32(0x40); // test rax, 0x40 (ZF)
            size_t jz_skip3 = emit_jcc_rel32_placeholder(0x4);  // JZ: ZF=0, skip
            emit_mov_imm32_zext(RDX, 0x60000000);
            size_t done_flags = code_buf_used_;
            patch_jcc_rel32(jz_skip3, static_cast<int32_t>(done_flags - (jz_skip3 + 6)));
            patch_jmp_rel32(jmp_done1, static_cast<int32_t>(done_flags - (jmp_done1 + 5)));
            patch_jmp_rel32(jmp_done2, static_cast<int32_t>(done_flags - (jmp_done2 + 5)));

            emit_byte(0x58);  // pop rax (discard)

            // Store pstate (mask NZCV bits, OR in new value).
            // BUGFIX: bit 27 of pstate is the JIT-internal `from_sub` flag
            // used by BRCOND/ADCS/SBCS/CCMP to disambiguate CF semantics.
            // FCMP produces architectural NZCV directly from a float
            // comparison, NOT from a subtraction, so from_sub MUST be 0
            // after FCMP. The old mask 0x0FFFFFFF preserved bits 0–27,
            // leaving a stale from_sub from the prior SUBS — any flag
            // consumer that didn't normalize would read inverted CF.
            //
            // Correct mask: 0x07FFFFFF (clear bit 27 AND NZCV bits 28-31,
            // preserve bits 0-26 of other JIT-internal state).
            emit_load32(RCX, CPU_REG, PSTATE_OFF);
            emit_byte(0x81); emit_byte(0xE1); emit_u32(0x07FFFFFF);  // and ecx, 0x07FFFFFF (clear from_sub + NZCV)
            emit_byte(0x48); emit_byte(0x09); emit_byte(0xCA);  // or rdx, rcx
            emit_store32(CPU_REG, PSTATE_OFF, RDX);
            flags_in_host_ = false;
            // Defensive: clear from_sub so a future flag-setter that forgets
            // to set flags_from_sub_ doesn't read a stale value.
            flags_from_sub_ = false;
            return true;
        }

        // ── FMOV immediate (load decoded FP immediate) ──────────────
        case IROp::FP_MOVI: {
            // v_lo[dest] = imm; v_hi[dest] = 0
            // clobber_host_reg evicts any dirty GPR vreg
            // cached in RAX BEFORE we overwrite it with the immediate.
            // The old code silently dropped dirty vregs.
            clobber_host_reg(RAX);
            emit_mov_imm64(RAX, inst.imm);
            int32_t off_d = V_LO_OFF + static_cast<int>(inst.dest) * 8;
            emit_store(CPU_REG, off_d, RAX);
            emit_mov_imm32_zext(RAX, 0);
            emit_store(CPU_REG, V_HI_OFF + static_cast<int>(inst.dest) * 8, RAX);
            return true;
        }

        // ── FCVT: float <-> double conversion ────────────────────────
        // ── FCVT: float <-> double conversion ────────────────────────
        case IROp::FCVT_S2D:
        case IROp::FCVT_D2S: {
            // FCVT only clobbers RAX (zero store).
            clobber_flags();
            flush_invalidate_host_regs((1u << RAX) | (1u << RCX) | (1u << RDX));
            // Load FP value from v_lo[src1] into XMM0
            int32_t off = V_LO_OFF + static_cast<int>(inst.src1) * 8;
            uint8_t prefix = (inst.op == IROp::FCVT_S2D) ? 0xF3 : 0xF2;
            // movss/movsd xmm0, [rbx + off]
            emit_byte(prefix); emit_byte(0x0F); emit_byte(0x10);
            emit_modrm_disp(0, CPU_REG, off);
            if (inst.op == IROp::FCVT_S2D) {
                // cvtss2sd xmm0, xmm0
                emit_byte(0xF3); emit_byte(0x0F); emit_byte(0x5A);
                emit_byte(0xC0);
            } else {
                // cvtsd2ss xmm0, xmm0
                emit_byte(0xF2); emit_byte(0x0F); emit_byte(0x5A);
                emit_byte(0xC0);
            }
            // Store result to v_lo[dest]
            int32_t off_d = V_LO_OFF + static_cast<int>(inst.dest) * 8;
            emit_byte(prefix ^ 0x01); emit_byte(0x0F); emit_byte(0x11);
            emit_modrm_disp(0, CPU_REG, off_d);
            // Zero v_hi[dest]
            emit_mov_imm32_zext(RAX, 0);
            emit_store(CPU_REG, V_HI_OFF + static_cast<int>(inst.dest) * 8, RAX);
            return true;
        }

        default:
            return false;  // not handled — caller falls through
    }
}

} // namespace arm64emu
