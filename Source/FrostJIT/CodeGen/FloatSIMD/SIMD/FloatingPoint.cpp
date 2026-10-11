// SIMD floating-point codegen: scalar DUP, FP unary/arithmetic/FMA.
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include "System/Core/Emulator.hpp"
#include "FrostJIT/IR/IR.hpp"
#include "FrostJIT/CodeGen/Native/NativeSIMD.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace arm64emu {
bool FrostJIT::compile_simd_floating_point_(const IRInst& inst) {
    switch (inst.op) {
        case IROp::SIMD_SCALAR_DUP: {
            const auto p = inst.scalar_dup_params();
            flush_invalidate_host_regs(1u << RAX);
            int byte = p.lane * p.esize;
            int off = (byte < 8 ? V_LO_OFF : V_HI_OFF) + inst.src1 * 8 + byte % 8;
            switch (p.esize) {
                case 1: emit_load8(RAX, CPU_REG, off); break;
                case 2: emit_load16(RAX, CPU_REG, off); break;
                case 4: emit_load32(RAX, CPU_REG, off); break;
                default: emit_load(RAX, CPU_REG, off); break;
            }
            emit_store(CPU_REG, V_LO_OFF + inst.dest * 8, RAX);
            emit_mov_imm32_zext(RAX, 0);
            emit_store(CPU_REG, V_HI_OFF + inst.dest * 8, RAX);
            return true;
        }
        case IROp::SIMD_FP_UNARY: {
            const SimdSubopParams p = inst.fp_unary_params();
            int xd = vec_xmm(inst.dest), xs = vec_xmm(inst.src1);
            if (xd >= 0 && xs >= 0) {
                if (p.subop == 2) {
                    emit_vex3(1, false, 0, p.esize == 8 ? 1 : 0,
                              xd, xs, true, 0x51); // vsqrtps/pd (reserved vvvv)
                } else {
                    emit_vec_sign_mask(p.esize, p.subop == 0);
                    emit_vex_fp_binop(xd, xs, 0, p.subop == 0 ? 0x54 : 0x57,
                                      p.esize == 8);
                }
                if (!p.q) emit_vec_zero_upper(xd);
                vec_cache_mark_dirty(inst.dest);
                return true;
            }
            clobber_flags();
            flush_invalidate_host_regs(1u << RAX);
            // Uncached fallback: load both halves before writing either,
            // preserving same-register aliasing.
            emit_byte(0xF3); emit_byte(0x0F); emit_byte(0x7E);
            emit_modrm_disp(0,CPU_REG,V_LO_OFF + inst.src1*8);
            if (p.q) {
                emit_byte(0xF3); emit_byte(0x0F); emit_byte(0x7E);
                emit_modrm_disp(1,CPU_REG,V_HI_OFF + inst.src1*8);
                emit_byte(0x66); emit_byte(0x0F); emit_byte(0x6C); emit_byte(0xC1);
            }
            if (p.subop == 2) {
                if (p.esize == 8) emit_byte(0x66);
                emit_byte(0x0F); emit_byte(0x51); emit_byte(0xC0); // sqrtps/pd
            } else {
                uint64_t mask = p.esize == 8 ? 0x8000000000000000ULL : 0x8000000080000000ULL;
                if (p.subop == 0) mask = ~mask;
                emit_mov_imm64(RAX,mask);
                emit_byte(0x66); emit_byte(0x48); emit_byte(0x0F); emit_byte(0x6E); emit_byte(0xC8);
                emit_byte(0x66); emit_byte(0x0F); emit_byte(0x6C); emit_byte(0xC9);
                emit_byte(0x0F); emit_byte(p.subop == 0 ? 0x54 : 0x57); emit_byte(0xC1);
            }
            emit_byte(0x66); emit_byte(0x0F); emit_byte(0xD6);
            emit_modrm_disp(0,CPU_REG,V_LO_OFF + inst.dest*8);
            if (p.q) {
                emit_byte(0x66); emit_byte(0x0F); emit_byte(0x73); emit_byte(0xD8); emit_byte(8);
                emit_byte(0x66); emit_byte(0x0F); emit_byte(0xD6);
                emit_modrm_disp(0,CPU_REG,V_HI_OFF + inst.dest*8);
            } else {
                emit_mov_imm32_zext(RAX,0); emit_store(CPU_REG,V_HI_OFF + inst.dest*8,RAX);
            }
            return true;
        }
        case IROp::SIMD_FP_ARITH: {
            const SimdFpArithParams fp = inst.fp_arith_params();
            const int esize = fp.esize;
            const bool Q = fp.q, is_double = esize == 8;
            if (esize != 4 && esize != 8) {
                emit_call_interp(inst.arm_pc, false);
                return true;
            }
            const bool minmax = fp.subop >= 4 && fp.subop <= 7;
            const bool abd = fp.subop == 0xD;
            uint8_t opcode;
            switch (fp.subop) {
                case 0: opcode = 0x58; break;
                case 1: case 0xD: opcode = 0x5C; break;
                case 2: case 0xB: opcode = 0x59; break;
                case 3: opcode = 0x5E; break;
                case 4: case 6: opcode = 0x5F; break;
                case 5: case 7: opcode = 0x5D; break;
                default: emit_call_interp(inst.arm_pc, false); return true;
            }
            clobber_flags();
            const int xd = vec_xmm(inst.dest), xn = vec_xmm(inst.src1), xm = vec_xmm(inst.src2);
            const bool cached = xd >= 0 && xn >= 0 && xm >= 0;
            int a = xn, b = xm;
            if (!cached) {
                load_fp_vector(0, inst.src1, Q);
                load_fp_vector(2, inst.src2, fp.lane >= 0 || Q);
                a = 0; b = 2;
            }
            if (fp.lane >= 0) {
                // Broadcast BEFORE touching any destination (including aliases).
                if (cached) {
                    emit_vex3(1, false, is_double ? b : 0, 1, 2, b, true,
                              is_double ? (fp.lane ? 0x15 : 0x14) : 0x70);
                } else {
                    if (!is_double) {
                        emit_byte(0x66); emit_byte(0x0F); emit_byte(0x70); emit_byte(0xD2);
                    } else {
                        emit_byte(0x66); emit_byte(0x0F); emit_byte(fp.lane ? 0x15 : 0x14); emit_byte(0xD2);
                    }
                }
                if (!is_double) emit_byte(fp.lane * 0x55);
                b = 2;
            }
            if (minmax) {
                // x86 MIN/MAX can hide a NaN by selecting its second input.
                // Test both inputs, rather than just the tentative result.
                if (cached) emit_vex_fp_binop(1, a, b, 0xC2, is_double);
                else {
                    emit_byte(0x0F); emit_byte(0x28); emit_byte(0xC8);
                    if (is_double) emit_byte(0x66);
                    emit_byte(0x0F); emit_byte(0xC2); emit_byte(0xCA);
                }
                emit_byte(3); // unordered
            }
            if (cached) emit_vex_fp_binop(0, a, b, opcode, is_double);
            else {
                if (is_double) emit_byte(0x66);
                emit_byte(0x0F); emit_byte(opcode); emit_byte(0xC2);
            }
            if (minmax) {
                // Equal signed zeros also differ between ARM and x86. A zero
                // result takes the same cold path; other finite results stay native.
                emit_byte(0x66); emit_byte(0x0F); emit_byte(0xEF); emit_byte(0xD2);
                if (is_double) emit_byte(0x66);
                emit_byte(0x0F); emit_byte(0xC2); emit_byte(0xD0); emit_byte(0);
                emit_byte(0x0F); emit_byte(0x56); emit_byte(0xCA); // orps xmm1,xmm2
            }
            const size_t special_done = emit_simd_fp_nan_guard(inst, esize, Q, minmax);
            if (abd) {
                // The guard sees the subtraction's NaN before clearing its sign.
                // XMM0 must retain the result while XMM2 holds the packed mask.
                emit_push(RAX);
                emit_mov_imm64(RAX, is_double ? 0x7FFFFFFFFFFFFFFFULL : 0x7FFFFFFF7FFFFFFFULL);
                emit_byte(0x66); emit_byte(0x48); emit_byte(0x0F); emit_byte(0x6E); emit_byte(0xD0);
                emit_pop(RAX);
                emit_byte(0x66); emit_byte(0x0F); emit_byte(0x6C); emit_byte(0xD2);
                emit_byte(0x0F); emit_byte(0x54); emit_byte(0xC2);
            }
            if (cached) {
                emit_vex3(1, false, 0, 0, xd, 0, true, 0x28); // vmovaps
                if (!Q) emit_vec_zero_upper(xd);
                vec_cache_mark_dirty(inst.dest);
            } else store_fp_vector(inst.dest, Q);
            patch_jmp_rel32(special_done, static_cast<int32_t>(code_buf_used_ - (special_done + 5)));
            return true;
        }
        case IROp::SIMD_FP_FMA: {
            const SimdSubopParams fp = inst.fp_fma_params();
            const int esize = fp.esize;
            const bool Q = fp.q, is_double = esize == 8;
            if ((esize != 4 && esize != 8) || !has_fma3()) {
                // Multiplication followed by addition is not a fused operation.
                emit_call_interp(inst.arm_pc, false);
                return true;
            }
            clobber_flags();
            const int xd = vec_xmm(inst.dest), xn = vec_xmm(inst.src1), xm = vec_xmm(inst.src2);
            const bool cached = xd >= 0 && xn >= 0 && xm >= 0;
            if (cached) {
                emit_vex3(1, false, 0, 0, 0, xd, true, 0x28);
                emit_vex_fma(0, xn, xm, is_double, fp.subop == 1);
            } else {
                load_fp_vector(0, inst.dest, Q);
                load_fp_vector(1, inst.src1, Q);
                load_fp_vector(2, inst.src2, Q);
                emit_vex_fma(0, 1, 2, is_double, fp.subop == 1);
            }
            const size_t special_done = emit_simd_fp_nan_guard(inst, esize, Q);
            if (cached) {
                emit_vex3(1, false, 0, 0, xd, 0, true, 0x28);
                if (!Q) emit_vec_zero_upper(xd);
                vec_cache_mark_dirty(inst.dest);
            } else store_fp_vector(inst.dest, Q);
            patch_jmp_rel32(special_done, static_cast<int32_t>(code_buf_used_ - (special_done + 5)));
            return true;
        }
        // ── SIMD CMP (integer lane-wise compare) ─────────────────────
        // imm (opc) = 0=CMEQ, 1=CMGT, 2=CMGE, 3=CMHI, 4=CMHS; width =
        // esize (1/2/4/8); flags_op = Q (threaded by the translator so the
        // Q=0 64-bit forms zero v_hi, matching the interpreter). XMM
        // scratch: XMM0 = a, XMM1 = b, XMM2 = sign-flip mask / all-ones.
        // SIMD_CMP is NOT vec-cache-compatible (see vec_cache_compatible_op
        // in VectorCache.cpp), so blocks containing it never get
        // XMM3-15 pinned — XMM2 scratch is always safe here.
        //
        // x86 lowering per opc (dest = a op b, per lane):
        //   CMEQ (0): PCMPEQ(a,b)                   66 0F 74/75/76 (8/16/32),
        //                                              66 0F 38 29 (64, SSE4.1)
        //   CMGT (1): PCMPGT(a,b)                   66 0F 64/65/66 (8/16/32),
        //                                              66 0F 38 37 (64, SSE4.2)
        //   CMGE (2): NOT(PCMPGT(b,a))              — swap operands, then
        //              invert with pxor all-ones. (a>=b <=> !(b>a))
        //   CMHI (3): XOR(a,msk); XOR(b,msk); PCMPGT(a,b) — flipping the
        //              sign bit maps the unsigned order onto signed order.
        //              mask = 0x80.. per lane (0x8080.. / 0x8000.. /
        //              0x80000000.. / 0x80000000'00000000..).
        //   CMHS (4): XOR both with msk, PCMPGT(b,a), invert. (a>=b <=> !(b>a))
        // Each 64-bit half is processed in isolation: MOVSD loads zero the
        // upper 64 bits, so the masked compare only ever touches the half
        // being computed (the sign-flip mask's upper 64 bits are 0 and the
        // compare of zeros in the unused lane is discarded by the MOVSD
        // store). Same pattern as the existing CMEQ half-loop.
        default:
            return false;
    }
}
}  // namespace arm64emu
