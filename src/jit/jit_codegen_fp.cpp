// jit/jit_codegen_fp.cpp — FrostJIT FP/SIMD IR-op codegen.
//
// v1.4.5-alpha (Turn 36): split out of frostjit.cpp. This file holds
// the FP_* and SIMD_* case bodies of the IR-op switch, extracted into
// a separate method (compile_ir_inst_fp_) for readability. The main
// switch in frostjit.cpp dispatches to this method for FP/SIMD ops.
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
// The case bodies below are verbatim from the original frostjit.cpp
// (Turn 34 era) — no logic changes, just moved to a separate file.
bool FrostJIT::compile_ir_inst_fp_(const IRInst& inst) {
    // Set fp_handled_ = true optimistically; the default: case clears
    // it. The caller (compile_ir_inst) checks fp_handled_ after the
    // call to decide whether to fall through to the integer switch.
    fp_handled_ = true;
    switch (inst.op) {
        // ── FMOV (general ↔ FP) — native codegen via emit_fmov_helper ───
        // These ops move data between cpu.regs[] and cpu.v_lo[]/v_hi[]
        // using direct memory access through CPU_REG (RBX).
        // No CALL_INTERP needed — pure memory moves through RAX.
        case IROp::FMOV_G2F:    emit_fmov_helper(/*dir=*/0, /*field=*/0, inst.dest, inst.src1, inst.dest); return false;
        case IROp::FMOV_F2G:    emit_fmov_helper(/*dir=*/1, /*field=*/0, inst.src1, inst.src1, inst.dest); return false;
        case IROp::FMOV_G2FHI:  emit_fmov_helper(/*dir=*/0, /*field=*/1, inst.dest, inst.src1, inst.dest); return false;
        case IROp::FMOV_FHI2G:  emit_fmov_helper(/*dir=*/1, /*field=*/1, inst.src1, inst.src1, inst.dest); return false;

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
                    return false;
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
            return false;
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
            return false;
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
            return false;
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
            return false;
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
            return false;
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
            return false;
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

            // Store pstate (mask NZCV bits, OR in new value)
            emit_load32(RCX, CPU_REG, PSTATE_OFF);
            emit_byte(0x81); emit_byte(0xE1); emit_u32(0x0FFFFFFF);  // and ecx, 0x0FFFFFFF
            emit_byte(0x48); emit_byte(0x09); emit_byte(0xCA);  // or rdx, rcx
            emit_store32(CPU_REG, PSTATE_OFF, RDX);
            flags_in_host_ = false;
            return false;
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
            return false;
        }

        // ── SIMD LOGICAL (AND/ORR/EOR/BIC/ORN/EON) — native SSE2 ────
        case IROp::SIMD_LOGICAL: {
            // v_lo[dest],v_hi[dest] = src1 OP src2
            // imm = opcode (0=and,1=orr,2=xor,3=bic,4=orn,5=eon)
            // SIMD_LOGICAL only touches XMM0/XMM1/XMM2, no GPRs.
            //
            // SSE2 opcodes used:
            //   AND  (0): pand     = 66 0F DB /r
            //   ORR  (1): por      = 66 0F EB /r
            //   EOR  (2): pxor     = 66 0F EF /r
            //   BIC  (3): a & ~b   = pandn xmm2,xmm1 (xmm2=~xmm1); pand xmm0,xmm2
            //   ORN  (4): a | ~b   = pandn xmm2,xmm1 (xmm2=~xmm1); por  xmm0,xmm2
            //   EON  (5): a ^ ~b   = pxor xmm0,xmm1; pcmpeqd xmm1,xmm1 (all-ones);
            //                       pxor xmm0,xmm1  →  ~xmm0
            clobber_flags();
            flush_invalidate_host_regs((1u << RAX) | (1u << RCX) | (1u << RDX));

            uint8_t opc = static_cast<uint8_t>(inst.imm);
            // For opc 0-2 we use a single SSE2 op; for 3-5 we emit a
            // 2-3 instruction sequence.
            uint8_t sse_op = 0;
            bool simple = false;
            if (opc == 0) { sse_op = 0xDB; simple = true; }       // PAND
            else if (opc == 1) { sse_op = 0xEB; simple = true; }  // POR
            else if (opc == 2) { sse_op = 0xEF; simple = true; }  // PXOR
            else if (opc > 5) {
                // Unknown opcode — fall back to interpreter.
                emit_call_interp(inst.arm_pc, false);
                return false;
            }

            auto emit_logical_half = [&](int32_t off1, int32_t off2, int32_t offd) {
                // movsd xmm0, [rbx+off1]
                emit_byte(0xF3); emit_byte(0x0F); emit_byte(0x10);
                emit_modrm_disp(0, CPU_REG, off1);
                // movsd xmm1, [rbx+off2]
                emit_byte(0xF3); emit_byte(0x0F); emit_byte(0x10);
                emit_modrm_disp(1, CPU_REG, off2);

                if (simple) {
                    // 66 0F sse_op C1  (xmm0, xmm1)
                    emit_byte(0x66); emit_byte(0x0F); emit_byte(sse_op);
                    emit_byte(0xC1);
                } else if (opc == 3) {
                    // BIC: a & ~b
                    // pandn xmm2, xmm1  →  xmm2 = ~xmm1 & xmm2
                    // First set xmm2 to all-ones: pcmpeqd xmm2, xmm2
                    emit_byte(0x66); emit_byte(0x0F); emit_byte(0x76);
                    emit_byte(0xE2);  // modrm(3, xmm2, xmm2)
                    // pandn xmm2, xmm1  →  xmm2 = ~xmm1 & xmm2 = ~xmm1
                    emit_byte(0x66); emit_byte(0x0F); emit_byte(0xDF);
                    emit_byte(0xE1);  // modrm(3, xmm2, xmm1)
                    // pand xmm0, xmm2
                    emit_byte(0x66); emit_byte(0x0F); emit_byte(0xDB);
                    emit_byte(0xC2);  // modrm(3, xmm0, xmm2)
                } else if (opc == 4) {
                    // ORN: a | ~b
                    // pcmpeqd xmm2, xmm2  (xmm2 = all-ones)
                    emit_byte(0x66); emit_byte(0x0F); emit_byte(0x76);
                    emit_byte(0xE2);
                    // pandn xmm2, xmm1  →  xmm2 = ~xmm1
                    emit_byte(0x66); emit_byte(0x0F); emit_byte(0xDF);
                    emit_byte(0xE1);
                    // por xmm0, xmm2
                    emit_byte(0x66); emit_byte(0x0F); emit_byte(0xEB);
                    emit_byte(0xC2);
                } else { // opc == 5: EON: a ^ ~b = ~(a^b)
                    // pxor xmm0, xmm1
                    emit_byte(0x66); emit_byte(0x0F); emit_byte(0xEF);
                    emit_byte(0xC1);
                    // pcmpeqd xmm1, xmm1  (xmm1 = all-ones)
                    emit_byte(0x66); emit_byte(0x0F); emit_byte(0x76);
                    emit_byte(0xE1);
                    // pxor xmm0, xmm1  →  ~xmm0
                    emit_byte(0x66); emit_byte(0x0F); emit_byte(0xEF);
                    emit_byte(0xC1);
                }

                // movsd [rbx+offd], xmm0
                emit_byte(0xF3); emit_byte(0x0F); emit_byte(0x11);
                emit_modrm_disp(0, CPU_REG, offd);
            };

            int32_t off1lo = V_LO_OFF + static_cast<int>(inst.src1) * 8;
            int32_t off1hi = V_HI_OFF + static_cast<int>(inst.src1) * 8;
            int32_t off2lo = V_LO_OFF + static_cast<int>(inst.src2) * 8;
            int32_t off2hi = V_HI_OFF + static_cast<int>(inst.src2) * 8;
            int32_t offdlo = V_LO_OFF + static_cast<int>(inst.dest) * 8;
            int32_t offdhi = V_HI_OFF + static_cast<int>(inst.dest) * 8;
            emit_logical_half(off1lo, off2lo, offdlo);
            emit_logical_half(off1hi, off2hi, offdhi);
            return false;
        }

        // ── SIMD ARITH (integer lane-wise add/sub/mul/min/max) ───────
        // Uses SSE2/SSE4.1 integer SIMD ops. Only the common element
        // sizes (1/2/4/8 bytes) and opcodes (add/sub/mul) are native;
        // rare combinations fall back to CALL_INTERP.
        case IROp::SIMD_ARITH: {
            uint8_t opc = static_cast<uint8_t>(inst.imm);
            int esize = static_cast<int>(inst.width);
            if (esize != 1 && esize != 2 && esize != 4 && esize != 8) {
                emit_call_interp(inst.arm_pc, false);
                return false;
            }
            // mul (opc=2) for size=8 is not in SSE2 — fall back.
            if (opc == 2 && esize == 8) {
                emit_call_interp(inst.arm_pc, false);
                return false;
            }
            // min/max (opc=3..6) for size=8 not in SSE2 — fall back.
            if (opc >= 3 && opc <= 6 && esize == 8) {
                emit_call_interp(inst.arm_pc, false);
                return false;
            }
            clobber_flags();
            flush_invalidate_host_regs((1u << RAX) | (1u << RCX) | (1u << RDX));

            // SSE2 opcodes (with 66 0F prefix):
            //   paddb/h/w/d/q  = FC/FD/FE/D8
            //   psubb/h/w/d/q  = F8/F9/FA/EB
            //   pmullw (size=2) = D5   (only 16-bit multiply low)
            //   pmulld (size=4, SSE4.1) = 40 5F (needs 66 0F 38 5F)
            // min/max (unsigned/signed):
            //   pminub/pmaxub (size=1) = DA/DE
            //   pminsw/pmaxsw (size=2, signed) = EA/EE
            //   pminud/pmaxud (size=4, SSE4.1) = 38 3B / 38 3F
            // For signed min/max on size=1, we can use pminsb/pmaxsb (SSE4.1=38 38/3C)
            // For simplicity, only support the SSE2 ones natively; fall back otherwise.
            uint8_t op_byte = 0;
            bool needs_38_prefix = false;  // SSE4.1 3-byte opcodes (66 0F 38 XX)
            bool supported = true;

            if (opc == 0) {  // ADD
                switch (esize) {
                    case 1: op_byte = 0xFC; break;  // paddb
                    case 2: op_byte = 0xFD; break;  // paddw
                    case 4: op_byte = 0xFE; break;  // paddd
                    case 8: op_byte = 0xD4; break;  // paddq (note: 0F D4)
                }
            } else if (opc == 1) {  // SUB
                switch (esize) {
                    case 1: op_byte = 0xF8; break;  // psubb
                    case 2: op_byte = 0xF9; break;  // psubw
                    case 4: op_byte = 0xFA; break;  // psubd
                    case 8: op_byte = 0xFB; break;  // psubq (note: 0F FB)
                }
            } else if (opc == 2) {  // MUL
                if (esize == 2) {
                    op_byte = 0xD5;        // pmullw (66 0F D5)
                } else if (esize == 4 && has_sse41()) {
                    // pmulld (SSE4.1): 66 0F 38 5F. Guarded — SIGILL on
                    // pre-Westmere CPUs without the runtime check.
                    needs_38_prefix = true;
                    op_byte = 0x5F;
                } else {
                    supported = false;  // size=1/8 or no SSE4.1: no native multiply
                }
            } else if (opc == 3 || opc == 4) {  // unsigned min/max
                if (esize == 1) {
                    op_byte = (opc == 3) ? 0xDA : 0xDE;  // pminub/pmaxub
                } else if (esize == 4 && has_sse41()) {
                    // pminud = 66 0F 38 3B ; pmaxud = 66 0F 38 3F (SSE4.1)
                    needs_38_prefix = true;
                    op_byte = (opc == 3) ? 0x3B : 0x3F;
                } else {
                    supported = false;
                }
            } else if (opc == 5 || opc == 6) {  // signed min/max
                if (esize == 2) {
                    op_byte = (opc == 5) ? 0xEA : 0xEE;  // pminsw/pmaxsw
                } else if ((esize == 1 || esize == 4) && has_sse41()) {
                    needs_38_prefix = true;
                    if (esize == 1) {
                        // pminsb = 66 0F 38 38 ; pmaxsb = 66 0F 38 3C
                        op_byte = (opc == 5) ? 0x38 : 0x3C;
                    } else {
                        // pminsd = 66 0F 38 39 ; pmaxsd = 66 0F 38 3D
                        op_byte = (opc == 5) ? 0x39 : 0x3D;
                    }
                } else {
                    supported = false;
                }
            } else {
                supported = false;
            }

            if (!supported) {
                emit_call_interp(inst.arm_pc, false);
                return false;
            }

            auto emit_arith_half = [&](int32_t off1, int32_t off2, int32_t offd) {
                // movsd xmm0, [rbx+off1]
                emit_byte(0xF3); emit_byte(0x0F); emit_byte(0x10);
                emit_modrm_disp(0, CPU_REG, off1);
                // movsd xmm1, [rbx+off2]
                emit_byte(0xF3); emit_byte(0x0F); emit_byte(0x10);
                emit_modrm_disp(1, CPU_REG, off2);
                // emit the SSE op (xmm0, xmm1)
                emit_byte(0x66); emit_byte(0x0F);
                if (needs_38_prefix) {
                    emit_byte(0x38);
                }
                emit_byte(op_byte);
                emit_byte(0xC1);  // modrm(3, xmm0, xmm1)
                // movsd [rbx+offd], xmm0
                emit_byte(0xF3); emit_byte(0x0F); emit_byte(0x11);
                emit_modrm_disp(0, CPU_REG, offd);
            };

            int32_t off1lo = V_LO_OFF + static_cast<int>(inst.src1) * 8;
            int32_t off1hi = V_HI_OFF + static_cast<int>(inst.src1) * 8;
            int32_t off2lo = V_LO_OFF + static_cast<int>(inst.src2) * 8;
            int32_t off2hi = V_HI_OFF + static_cast<int>(inst.src2) * 8;
            int32_t offdlo = V_LO_OFF + static_cast<int>(inst.dest) * 8;
            int32_t offdhi = V_HI_OFF + static_cast<int>(inst.dest) * 8;
            emit_arith_half(off1lo, off2lo, offdlo);
            emit_arith_half(off1hi, off2hi, offdhi);
            return false;
        }

        // ── SIMD CMP (integer lane-wise compare) ─────────────────────
        // Only eq (opc=0) is fully native via PCMPEQB/W/D/Q. Other
        // comparisons fall back to CALL_INTERP for now.
        case IROp::SIMD_CMP: {
            uint8_t opc = static_cast<uint8_t>(inst.imm);
            int esize = static_cast<int>(inst.width);
            if (opc != 0 || (esize != 1 && esize != 2 && esize != 4 && esize != 8)) {
                emit_call_interp(inst.arm_pc, false);
                return false;
            }
            clobber_flags();
            flush_invalidate_host_regs((1u << RAX) | (1u << RCX) | (1u << RDX));

            uint8_t op_byte = 0;
            switch (esize) {
                case 1: op_byte = 0x74; break;  // pcmpeqb
                case 2: op_byte = 0x75; break;  // pcmpeqw
                case 4: op_byte = 0x76; break;  // pcmpeqd
                case 8:  // pcmpeqq requires SSE4.1
                    if (!has_sse41()) {
                        emit_call_interp(inst.arm_pc, false);
                        return false;
                    }
                    op_byte = 0x29; break;  // pcmpeqq (SSE4.1: 66 0F 38 29)
            }
            bool needs_38_prefix = (esize == 8);

            auto emit_cmp_half = [&](int32_t off1, int32_t off2, int32_t offd) {
                emit_byte(0xF3); emit_byte(0x0F); emit_byte(0x10);
                emit_modrm_disp(0, CPU_REG, off1);
                emit_byte(0xF3); emit_byte(0x0F); emit_byte(0x10);
                emit_modrm_disp(1, CPU_REG, off2);
                emit_byte(0x66); emit_byte(0x0F);
                if (needs_38_prefix) emit_byte(0x38);
                emit_byte(op_byte);
                emit_byte(0xC1);
                emit_byte(0xF3); emit_byte(0x0F); emit_byte(0x11);
                emit_modrm_disp(0, CPU_REG, offd);
            };

            int32_t off1lo = V_LO_OFF + static_cast<int>(inst.src1) * 8;
            int32_t off1hi = V_HI_OFF + static_cast<int>(inst.src1) * 8;
            int32_t off2lo = V_LO_OFF + static_cast<int>(inst.src2) * 8;
            int32_t off2hi = V_HI_OFF + static_cast<int>(inst.src2) * 8;
            int32_t offdlo = V_LO_OFF + static_cast<int>(inst.dest) * 8;
            int32_t offdhi = V_HI_OFF + static_cast<int>(inst.dest) * 8;
            emit_cmp_half(off1lo, off2lo, offdlo);
            emit_cmp_half(off1hi, off2hi, offdhi);
            return false;
        }

        // ── SIMD DUP (broadcast GPR to both halves) ────────────────
        case IROp::SIMD_DUP: {
            // v_lo[dest] = v_hi[dest] = src1 (GPR value)
            // DON'T drop src1's cache mapping after the
            // store — src1 may be read again later in the block. The old
            // code did `vreg_home_[reg_vreg_[RAX]] = -1; reg_vreg_[RAX] = -1`
            // which silently dropped a dirty src1.
            int s = ensure_vreg(inst.src1, RAX);
            if (s != RAX) {
                // src1 is cached in another reg (s). We need its value in
                // RAX for the store. clobber_host_reg(RAX) spills any dirty
                // vreg currently in RAX BEFORE we overwrite it.
                clobber_host_reg(RAX);
                emit_mov_reg(RAX, s);
            }
            int32_t offlo = V_LO_OFF + static_cast<int>(inst.dest) * 8;
            int32_t offhi = V_HI_OFF + static_cast<int>(inst.dest) * 8;
            emit_store(CPU_REG, offlo, RAX);
            emit_store(CPU_REG, offhi, RAX);
            // src1 stays cached in its original reg (s) for later readers.
            // RAX holds a copy (not a cached vreg) — no mapping to update.
            return false;
        }

        // ── SIMD LDST (read/write v_lo/v_hi to/from vregs) ─────────
        case IROp::SIMD_LDST: {
            // width=1 (load): src1=lo vreg, src2=hi vreg → v_lo[dest], v_hi[dest]
            // width=0 (store): v_lo[dest] → src1 vreg, v_hi[dest] → src2 vreg
            if (inst.width == 1) {
                // Load: write vregs to v_lo/v_hi
                // use separate host regs for lo/hi so we
                // don't clobber src1's cached value when loading src2.
                // The old code reused RAX for both, dropping src1's mapping.
                int slo = ensure_vreg(inst.src1, RAX);
                int32_t offlo = V_LO_OFF + static_cast<int>(inst.dest) * 8;
                emit_store(CPU_REG, offlo, slo);

                // For the hi half, use RCX. If src2 is cached in a different
                // reg, ensure_vreg returns it (no eviction). If src2 is not
                // cached, ensure_vreg loads it into RCX (evicting RCX's
                // current occupant via alloc_reg, which calls evict_vreg).
                int shi = ensure_vreg(inst.src2, RCX);
                int32_t offhi = V_HI_OFF + static_cast<int>(inst.dest) * 8;
                emit_store(CPU_REG, offhi, shi);
            } else {
                // Store: read v_lo/v_hi into vregs
                int dlo = alloc_reg();
                int32_t offlo = V_LO_OFF + static_cast<int>(inst.dest) * 8;
                emit_load(dlo, CPU_REG, offlo);
                set_vreg_reg(inst.src1, dlo);

                int dhi = alloc_reg();
                int32_t offhi = V_HI_OFF + static_cast<int>(inst.dest) * 8;
                emit_load(dhi, CPU_REG, offhi);
                set_vreg_reg(inst.src2, dhi);
            }
            return false;
        }

        // ── SIMD SHL/USHR/SSHR (vector, by immediate) — native SSE2 ──
        // v1.4.5-alpha: native SSE2 codegen via psllw/pslld/psllq (SHL),
        // psrlw/psrld/psrlq (USHR), psraw/psrad (SSHR). Previously these
        // fell back to CALL_INTERP (~20% overhead on SIMD-heavy workloads).
        //
        // SSE2 shift-by-immediate encoding (66 0F <subop> <modrm> <imm8>):
        //   PSLLW xmmN, imm8 : 66 0F 71 F0|N  imm8    (reg field = 6)
        //   PSLLD xmmN, imm8 : 66 0F 72 F0|N  imm8
        //   PSLLQ xmmN, imm8 : 66 0F 73 F0|N  imm8
        //   PSRLW xmmN, imm8 : 66 0F 71 D0|N  imm8    (reg field = 2)
        //   PSRLD xmmN, imm8 : 66 0F 72 D0|N  imm8
        //   PSRLQ xmmN, imm8 : 66 0F 73 D0|N  imm8
        //   PSRAW xmmN, imm8 : 66 0F 71 E0|N  imm8    (reg field = 4)
        //   PSRAD xmmN, imm8 : 66 0F 72 E0|N  imm8
        //
        // The modrm byte is 11_<reg>_<rm> where <rm> selects the xmmN.
        // PSLL/PSRL/PSRA do NOT have a PSLLB/PSRLB/PSRAB form in SSE2
        // (8-bit element shifts); we fall back to CALL_INTERP for esize=1.
        // 64-bit SSHR (psraq) requires AVX-512 — we fall back for esize=8.
        //
        // x86 shift semantics match ARM for shift ∈ [0, esize*8]:
        //   - shift=0: no-op (both)
        //   - shift=esize*8: SHL/USHR clear the lane; SSHR sign-fills it.
        // The IR translator guarantees shift ∈ [0, esize*8].
        case IROp::SIMD_SHL:
        case IROp::SIMD_USHR:
        case IROp::SIMD_SSHR: {
            int esize = static_cast<int>(inst.width);
            uint8_t shift = static_cast<uint8_t>(inst.imm);
            // Fall back to CALL_INTERP for unsupported element sizes.
            //  - esize=1 (8-bit): no PSLLB/PSRLB/PSRAB in SSE2.
            //  - esize=8 SSHR: no PSRAQ in SSE2 (needs AVX-512).
            //  - Invalid esize: shouldn't happen, but be safe.
            if (esize != 2 && esize != 4 && esize != 8) {
                emit_call_interp(inst.arm_pc, false);
                return false;
            }
            if (inst.op == IROp::SIMD_SSHR && esize == 8) {
                emit_call_interp(inst.arm_pc, false);
                return false;
            }
            // All SHL/USHR variants for esize ∈ {2,4,8} are supported.
            // (SHL Q-word uses PSLLQ; USHR Q-word uses PSRLQ; both SSE2.)

            // Decode the SSE2 subop byte (0x71/0x72/0x73) and the reg
            // field (6=PSLL, 2=PSRL, 4=PSRA).
            uint8_t subop = 0;
            uint8_t reg_field = 0;
            if (esize == 2)      subop = 0x71;
            else if (esize == 4) subop = 0x72;
            else                 subop = 0x73;  // esize == 8

            if (inst.op == IROp::SIMD_SHL)       reg_field = 6;
            else if (inst.op == IROp::SIMD_USHR) reg_field = 2;
            else                                 reg_field = 4;  // SIMD_SSHR

            clobber_flags();
            // SSE2 shifts only use XMM0 (no GPRs). But emit_call_interp
            // and other paths below might clobber RAX/RCX/RDX, so flush
            // them to keep the register-cache consistent.
            flush_invalidate_host_regs((1u << RAX) | (1u << RCX) | (1u << RDX));

            // For each half (v_lo, v_hi):
            //   movsd xmm0, [rbx+off1]    (F2 0F 10 — load 64 bits, zero upper 64)
            //   66 0F <subop> <modrm> imm  (PSLL/PSRL/PSRA xmm0, imm8)
            //   movsd [rbx+offd], xmm0    (F2 0F 11 — store low 64 bits)
            //
            // CRITICAL: use 0xF2 (movsd, 64-bit) NOT 0xF3 (movss, 32-bit).
            // movss would load only the low 32 bits (lane 0) and zero lanes
            // 1-3, then the SSE2 shift would only shift lane 0, then movss
            // would store only lane 0 — corrupting lanes 1-3. The existing
            // SIMD_LOGICAL/SIMD_ARITH handlers also use 0xF3, but their native
            // paths are not triggered for the current test suite (the IR
            // translator routes most SIMD ops to CALL_INTERP), so the latent
            // bug there is not exercised. We use 0xF2 here to be correct.
            auto emit_shift_half = [&](int32_t off1, int32_t offd) {
                // movsd xmm0, [rbx+off1]   (F2 0F 10 /r — load 64 bits)
                emit_byte(0xF2); emit_byte(0x0F); emit_byte(0x10);
                emit_modrm_disp(0, CPU_REG, off1);
                // PSLL/PSRL/PSRA xmm0, imm8
                emit_byte(0x66); emit_byte(0x0F); emit_byte(subop);
                emit_byte(0xC0 | (reg_field << 3) | 0);  // modrm(3, reg_field, xmm0)
                emit_byte(shift);
                // movsd [rbx+offd], xmm0   (F2 0F 11 /r — store low 64 bits)
                emit_byte(0xF2); emit_byte(0x0F); emit_byte(0x11);
                emit_modrm_disp(0, CPU_REG, offd);
            };

            int32_t off1lo = V_LO_OFF + static_cast<int>(inst.src1) * 8;
            int32_t off1hi = V_HI_OFF + static_cast<int>(inst.src1) * 8;
            int32_t offdlo = V_LO_OFF + static_cast<int>(inst.dest) * 8;
            int32_t offdhi = V_HI_OFF + static_cast<int>(inst.dest) * 8;
            emit_shift_half(off1lo, offdlo);
            emit_shift_half(off1hi, offdhi);
            return false;
        }

        // These are very common (SXTB/SXTH/SXTW/UXTB/UXTH/UXTW/LSL/LSR/
        // ASR/SBFIZ/UBFIZ/BFI/BFXIL) and falling back to CALL_INTERP
        // for each one is both slow and a source of correctness bugs
        // (the interp call's PC-change check can cause spurious block
        // exits). Implement them directly.
        //
        // ARM64 bitfield semantics (width W = 32 or 64):
        //   SBFM Rd, Rn, #immr, #imms:
        //     R = ROR(Rn, immr)  (rotate right by immr)
        //     if imms < immr:  Rd = sign_extend(R[W-1:imms], imms+1 bits)
        //     else:            Rd = sign_extend(R[imms:0], imms-immr+1 bits)
        //   UBFM Rd, Rn, #immr, #imms:
        //     R = ROR(Rn, immr)
        //     if imms < immr:  Rd = zero_extend(R[imms:0], imms+1 bits)
        //     else:            Rd = R[imms:0] zero-extended (i.e. extract)
        //   BFM Rd, Rn, #immr, #imms:
        //     inserts Rn's bits into Rd (preserve outside bits)
        //   EXTR Rd, Rn, Rm, #imms:

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
            return false;
        }

        // ── FRINT: FP round to integer ───────────────────────────────
        case IROp::FRINT: {
            // roundsd/roundss are SSE4.1 instructions. Fall back to
            // CALL_INTERP on hosts without SSE4.1 to avoid SIGILL.
            // BUGFIX: previously emitted unconditionally.
            if (!has_sse41()) {
                emit_call_interp(inst.arm_pc, false);
                return false;
            }
            // FRINT only clobbers RAX (zero store).
            clobber_flags();
            flush_invalidate_host_regs((1u << RAX) | (1u << RCX) | (1u << RDX));
            int32_t off = V_LO_OFF + static_cast<int>(inst.src1) * 8;
            // ftype encoding: 0 = single (S), 1 = double (D)
            // Width is `ftype ? 64 : 32` (set by ir_translate.cpp) — use
            // `width == 64` to distinguish from 32 (single). The old
            // `width != 0` check treated both as double, breaking all
            // single-precision FRINT.
            bool is_double = (inst.width == 64);
            uint8_t prefix = is_double ? 0xF2 : 0xF3;
            // Load FP value into XMM0
            emit_byte(prefix); emit_byte(0x0F); emit_byte(0x10);
            emit_modrm_disp(0, CPU_REG, off);
            // roundsd/roundss xmm0, xmm0, imm8
            // x86 rounding mode mapping: 0=nearest, 1=down(-inf), 2=up(+inf), 3=truncate(0)
            uint8_t x86_mode;
            switch (inst.imm & 0x7) {
                case 0: x86_mode = 0; break;  // N → nearest
                case 1: x86_mode = 2; break;  // P → +inf (up)
                case 2: x86_mode = 1; break;  // M → -inf (down)
                case 3: x86_mode = 3; break;  // Z → 0 (truncate)
                default: x86_mode = 4; break; // I/X → current MXCSR rounding
            }
            // 66 0F 3A 0B C0 imm8 = roundsd xmm0, xmm0, imm8
            // 66 0F 3A 0A C0 imm8 = roundss xmm0, xmm0, imm8
            emit_byte(0x66); emit_byte(0x0F); emit_byte(0x3A);
            emit_byte(is_double ? 0x0B : 0x0A);
            emit_byte(0xC0);  // xmm0, xmm0
            emit_byte(x86_mode);
            // Store result
            int32_t off_d = V_LO_OFF + static_cast<int>(inst.dest) * 8;
            emit_byte(prefix); emit_byte(0x0F); emit_byte(0x11);
            emit_modrm_disp(0, CPU_REG, off_d);
            emit_mov_imm32_zext(RAX, 0);
            emit_store(CPU_REG, V_HI_OFF + static_cast<int>(inst.dest) * 8, RAX);
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
