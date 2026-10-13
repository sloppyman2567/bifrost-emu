// SIMD integer codegen: native helpers, logical, arithmetic and compares.
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include "System/Core/Emulator.hpp"
#include "FrostJIT/IR/IR.hpp"
#include "FrostJIT/CodeGen/Native/NativeSIMD.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace arm64emu {
bool FrostJIT::compile_simd_arithmetic_(const IRInst& inst) {
    switch (inst.op) {
        case IROp::SIMD_NATIVE_EXTRA: {
            const uint32_t opcode = inst.native_simd_opcode();
            if (emit_native_simd_inline(opcode)) return true;
            auto fn = native_simd_helper(classify_native_simd(opcode));
            if (!fn) { emit_call_interp(inst.arm_pc, false); return true; }
            clobber_flags();
            // These helpers use the CPU vector file. Blocks containing this
            // IR op are never cache-pinned; flush GPRs for INS-general inputs.
            flush_all_vregs();
            emit_mov_reg(RDI, CPU_REG);
            emit_mov_imm32_zext(RSI, opcode);
            emit_push(WIN_REG);
            emit_call_aligned(fn, 1);
            emit_pop(WIN_REG);
            invalidate_host_regs((1u<<RAX)|(1u<<RCX)|(1u<<RDX)|
                                 (1u<<R8)|(1u<<R9)|(1u<<R11));
            return true;
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
            //   BIC/ORN/EON (3-5): invert b with pcmpeqd + pxor,
            //                     then apply pand/por/pxor to a and ~b.
            clobber_flags();
            // SIMD_LOGICAL touches no GPRs: the SSE2 XMM half-loop and the
            // VEX vec-cache path both use XMM0-2 only, and the RBX base
            // register is reserved (never holds a cached vreg). No GPR flush.
            const auto logic = inst.logical_params();
            uint8_t opc = logic.subop;
            // Opcodes 3-5 apply the same operation after inverting b.
            if (opc > 5) {
                // Unknown opcode — fall back to interpreter.
                emit_call_interp(inst.arm_pc, false);
                return true;
            }
            // ── Vector cache fast path ──────────────
            // Full-128-bit bitwise op on pinned XMM regs: one VEX op for
            // AND/ORR/EOR, VPANDN for BIC, and 2-3 VEX ops for ORN/EON
            // (using XMM0, which is scratch in cache-active blocks).
            {
                int xd = vec_xmm(inst.dest), xs1 = vec_xmm(inst.src1),
                    xs2 = vec_xmm(inst.src2);
                if (xd >= 0 && xs1 >= 0 && xs2 >= 0 && opc <= 5) {
                    clobber_flags();
                    // 0F map, pp=1 (66): vpand DB / vpor EB / vpxor EF /
                    // vpandn DF / vpcmpeqd 76. VEX.NDS: vvvv=src1, rm=src2.
                    if (opc == 0) {
                        emit_vex3(1, false, xs1, 1, xd, xs2, true, 0xDB);  // vpand xd,xs1,xs2
                    } else if (opc == 1) {
                        emit_vex3(1, false, xs1, 1, xd, xs2, true, 0xEB);  // vpor xd,xs1,xs2
                    } else if (opc == 2) {
                        emit_vex3(1, false, xs1, 1, xd, xs2, true, 0xEF);  // vpxor xd,xs1,xs2
                    } else if (opc == 3) {
                        // BIC: xs1 & ~xs2  =  vpandn xd, xs2, xs1
                        emit_vex3(1, false, xs2, 1, xd, xs1, true, 0xDF);
                    } else if (opc == 4) {
                        // ORN: xs1 | ~xs2
                        emit_vex3(1, false, 0, 1, 0, 0, true, 0x76);    // vpcmpeqd xmm0,xmm0
                        emit_vex3(1, false, 0, 1, 0, xs2, true, 0xEF);  // vpxor xmm0,xmm0,xs2 → ~xs2
                        emit_vex3(1, false, xs1, 1, xd, 0, true, 0xEB); // vpor xd,xs1,xmm0
                    } else { // opc == 5: EON = xs1 ^ ~xs2 = ~(xs1 ^ xs2)
                        emit_vex3(1, false, xs1, 1, xd, xs2, true, 0xEF);  // vpxor xd,xs1,xs2
                        emit_vex3(1, false, 0, 1, 0, 0, true, 0x76);       // vpcmpeqd xmm0,xmm0
                        emit_vex3(1, false, xd, 1, xd, 0, true, 0xEF);     // vpxor xd,xd,xmm0 → ~
                    }
                    if (!logic.q) emit_vec_zero_upper(xd);
                    vec_cache_mark_dirty(static_cast<int>(inst.dest));
                    return true;
                }
            }
            auto emit_logical_half = [&](int32_t off1, int32_t off2, int32_t offd) {
                // movsd xmm0, [rbx+off1]  (MOVSD = F2 0F 10, 64-bit)
                emit_byte(0xF2); emit_byte(0x0F); emit_byte(0x10);
                emit_modrm_disp(0, CPU_REG, off1);
                // movsd xmm1, [rbx+off2]
                emit_byte(0xF2); emit_byte(0x0F); emit_byte(0x10);
                emit_modrm_disp(1, CPU_REG, off2);
                if (opc >= 3) {
                    // Legacy PANDN computes ~dst & src, so an all-ones
                    // destination would produce zero, not ~src. Invert b
                    // explicitly; the helpers also encode scratch regs.
                    sse2_op(0x76, 2, 2);  // pcmpeqd xmm2,xmm2 (all ones)
                    sse2_op(0xEF, 1, 2);  // pxor xmm1,xmm2 (~b)
                }
                static constexpr uint8_t logical_ops[] = {0xDB, 0xEB, 0xEF};
                sse2_op(logical_ops[opc % 3], 0, 1);  // a OP b (or ~b)
                // movsd [rbx+offd], xmm0  (MOVSD store = F2 0F 11)
                emit_byte(0xF2); emit_byte(0x0F); emit_byte(0x11);
                emit_modrm_disp(0, CPU_REG, offd);
            };
            int32_t off1lo = V_LO_OFF + static_cast<int>(inst.src1) * 8;
            int32_t off1hi = V_HI_OFF + static_cast<int>(inst.src1) * 8;
            int32_t off2lo = V_LO_OFF + static_cast<int>(inst.src2) * 8;
            int32_t off2hi = V_HI_OFF + static_cast<int>(inst.src2) * 8;
            int32_t offdlo = V_LO_OFF + static_cast<int>(inst.dest) * 8;
            int32_t offdhi = V_HI_OFF + static_cast<int>(inst.dest) * 8;
            emit_logical_half(off1lo, off2lo, offdlo);
            if (logic.q) emit_logical_half(off1hi, off2hi, offdhi);
            else {
                emit_byte(0x0F); emit_byte(0x57); emit_byte(0xC0);
                emit_byte(0xF2); emit_byte(0x0F); emit_byte(0x11);
                emit_modrm_disp(0, CPU_REG, offdhi);
            }
            return true;
        }
        // ── SIMD ARITH (integer lane-wise add/sub/mul/min/max) ───────
        // Uses SSE2/SSE4.1 integer SIMD ops. Only the common element
        // sizes (1/2/4/8 bytes) and opcodes (add/sub/mul) are native;
        // rare combinations fall back to CALL_INTERP.
        case IROp::SIMD_ARITH: {
            const SimdArithParams ap = inst.arith_params();
            uint8_t opc = ap.subop;
            int esize = static_cast<int>(ap.esize);
            if (esize != 1 && esize != 2 && esize != 4 && esize != 8) {
                emit_call_interp(inst.arm_pc, false);
                return true;
            }
            // mul (opc=2) for size=8 is not in SSE2 — fall back.
            if (opc == 2 && esize == 8) {
                emit_call_interp(inst.arm_pc, false);
                return true;
            }
            // min/max (opc=3..6) for size=8 not in SSE2 — fall back.
            if (opc >= 3 && opc <= 6 && esize == 8) {
                emit_call_interp(inst.arm_pc, false);
                return true;
            }
            clobber_flags();
            // SIMD_ARITH touches no GPRs (XMM half-loop / VEX vec-cache
            // only; unsupported cases above already returned via
            // emit_call_interp, which self-flushes). No GPR flush.
            // SSE2 opcodes (with 66 0F prefix):
            //   paddb/h/w/d/q  = FC/FD/FE/D8
            //   psubb/h/w/d/q  = F8/F9/FA/EB
            //   pmullw (size=2) = D5   (only 16-bit multiply low)
            //   pmulld (size=4, SSE4.1) = 66 0F 38 40
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
                    // pmulld (SSE4.1): 66 0F 38 40. Guarded — SIGILL on
                    // pre-Westmere CPUs without the runtime check.
                    needs_38_prefix = true;
                    op_byte = 0x40;
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
                return true;
            }
            auto emit_arith_half = [&](int32_t off1, int32_t off2, int32_t offd) {
                // movsd xmm0, [rbx+off1]  (MOVSD = F2 0F 10, 64-bit)
                emit_byte(0xF2); emit_byte(0x0F); emit_byte(0x10);
                emit_modrm_disp(0, CPU_REG, off1);
                // movsd xmm1, [rbx+off2]
                emit_byte(0xF2); emit_byte(0x0F); emit_byte(0x10);
                emit_modrm_disp(1, CPU_REG, off2);
                // emit the SSE op (xmm0, xmm1)
                emit_byte(0x66); emit_byte(0x0F);
                if (needs_38_prefix) {
                    emit_byte(0x38);
                }
                emit_byte(op_byte);
                emit_byte(0xC1);  // modrm(3, xmm0, xmm1)
                // movsd [rbx+offd], xmm0  (MOVSD store = F2 0F 11)
                emit_byte(0xF2); emit_byte(0x0F); emit_byte(0x11);
                emit_modrm_disp(0, CPU_REG, offd);
            };
            int32_t off1lo = V_LO_OFF + static_cast<int>(inst.src1) * 8;
            int32_t off1hi = V_HI_OFF + static_cast<int>(inst.src1) * 8;
            int32_t off2lo = V_LO_OFF + static_cast<int>(inst.src2) * 8;
            int32_t off2hi = V_HI_OFF + static_cast<int>(inst.src2) * 8;
            int32_t offdlo = V_LO_OFF + static_cast<int>(inst.dest) * 8;
            int32_t offdhi = V_HI_OFF + static_cast<int>(inst.dest) * 8;
            emit_arith_half(off1lo, off2lo, offdlo);
            if (ap.q) emit_arith_half(off1hi, off2hi, offdhi);
            else {
                // Q=0 always clears v_hi, including when Rd aliases a source.
                emit_byte(0x48); emit_byte(0xC7);
                emit_modrm_disp(0, CPU_REG, offdhi); emit_u32(0);
            }
            return true;
        }
        // ── SIMD FP lane-wise arithmetic ─────────────
        // Emits packed SSE: addps/subps/mulps/divps/minps/maxps (single,
        // 0F prefix) or addpd/... (double, 66 0F prefix). FABD = sub then
        // clear sign bit (andps). width = element bytes (4/8); flags_op
        // = Q (1 = process both v_lo and v_hi, 0 = v_lo only).
        case IROp::SIMD_CMP: {
            const SimdSubopParams cp = inst.cmp_params();
            uint8_t opc = cp.subop;
            int esize = static_cast<int>(cp.esize);
            bool Q = cp.q;
            if (opc > 4 || (esize != 1 && esize != 2 && esize != 4 && esize != 8)) {
                emit_call_interp(inst.arm_pc, false);
                return true;
            }
            const bool ge  = (opc == 2 || opc == 4);  // >= variants: compute b>a, invert
            const bool uns = (opc == 3 || opc == 4);  // unsigned: sign-flip trick
            clobber_flags();
            // Only the unsigned sign-flip path touches a GPR (RAX, for the
            // movabs sign mask); signed/eq compares are pure XMM. A zero
            // mask makes flush_invalidate walk nothing.
            flush_invalidate_host_regs(uns ? (1u << RAX) : 0);
            uint8_t op_byte = 0;
            bool needs_38_prefix = false;
            switch (esize) {
                case 1: op_byte = (opc == 0) ? 0x74 : 0x64; break;  // pcmpeqb / pcmpgtb
                case 2: op_byte = (opc == 0) ? 0x75 : 0x65; break;  // pcmpeqw / pcmpgtw
                case 4: op_byte = (opc == 0) ? 0x76 : 0x66; break;  // pcmpeqd / pcmpgtd
                case 8:  // 64-bit: pcmpeqq (SSE4.1) / pcmpgtq (SSE4.2)
                    if (opc == 0) {
                        if (!has_sse41()) {
                            emit_call_interp(inst.arm_pc, false);
                            return true;
                        }
                        op_byte = 0x29;
                    } else {
                        if (!has_sse42()) {
                            emit_call_interp(inst.arm_pc, false);
                            return true;
                        }
                        op_byte = 0x37;
                    }
                    needs_38_prefix = true;
                    break;
            }
            uint64_t sign_mask = 0;
            if (uns) {
                switch (esize) {
                    case 1: sign_mask = 0x8080808080808080ULL; break;
                    case 2: sign_mask = 0x8000800080008000ULL; break;
                    case 4: sign_mask = 0x8000000080000000ULL; break;
                    default: sign_mask = 0x8000000000000000ULL; break;  // esize 8
                }
            }
            auto emit_cmp_half = [&](int32_t off1, int32_t off2, int32_t offd) {
                // GE variants compute PCMPGT(b, a): load the operands swapped.
                int32_t offa = ge ? off2 : off1;
                int32_t offb = ge ? off1 : off2;
                // MOVSD (64-bit) — F2 0F 10/11, NOT MOVSS (32-bit).
                emit_byte(0xF2); emit_byte(0x0F); emit_byte(0x10);
                emit_modrm_disp(0, CPU_REG, offa);
                emit_byte(0xF2); emit_byte(0x0F); emit_byte(0x10);
                emit_modrm_disp(1, CPU_REG, offb);
                if (uns) {
                    // movabs rax, mask; movq xmm2, rax (66 REX.W 0F 6E);
                    // pxor xmm0,xmm2; pxor xmm1,xmm2
                    emit_mov_imm64(RAX, sign_mask);
                    emit_byte(0x66); emit_byte(0x48); emit_byte(0x0F); emit_byte(0x6E);
                    emit_byte(0xD0);  // modrm(3, xmm2, rax)
                    emit_byte(0x66); emit_byte(0x0F); emit_byte(0xEF);
                    emit_byte(0xC2);  // pxor xmm0, xmm2
                    emit_byte(0x66); emit_byte(0x0F); emit_byte(0xEF);
                    emit_byte(0xCA);  // pxor xmm1, xmm2
                }
                // 66 0F [38] op modrm(3, xmm0, xmm1)
                emit_byte(0x66); emit_byte(0x0F);
                if (needs_38_prefix) emit_byte(0x38);
                emit_byte(op_byte);
                emit_byte(0xC1);
                if (ge) {
                    // pcmpeqb xmm2, xmm2 (all-ones) then pxor xmm0, xmm2.
                    emit_byte(0x66); emit_byte(0x0F); emit_byte(0x74);
                    emit_byte(0xD2);
                    emit_byte(0x66); emit_byte(0x0F); emit_byte(0xEF);
                    emit_byte(0xC2);  // pxor xmm0, xmm2
                }
                emit_byte(0xF2); emit_byte(0x0F); emit_byte(0x11);
                emit_modrm_disp(0, CPU_REG, offd);
            };
            int32_t off1lo = V_LO_OFF + static_cast<int>(inst.src1) * 8;
            int32_t off1hi = V_HI_OFF + static_cast<int>(inst.src1) * 8;
            int32_t off2lo = V_LO_OFF + static_cast<int>(inst.src2) * 8;
            int32_t off2hi = V_HI_OFF + static_cast<int>(inst.src2) * 8;
            int32_t offdlo = V_LO_OFF + static_cast<int>(inst.dest) * 8;
            int32_t offdhi = V_HI_OFF + static_cast<int>(inst.dest) * 8;
            emit_cmp_half(off1lo, off2lo, offdlo);
            if (Q) {
                emit_cmp_half(off1hi, off2hi, offdhi);
            } else {
                // Q=0 (.8b/.4h/.2s/.1d): 64-bit result — zero v_hi like the
                // interpreter's SIMD_DP compare block (Dispatch.cpp).
                emit_byte(0x66); emit_byte(0x0F); emit_byte(0xEF);
                emit_byte(0xC0);  // pxor xmm0, xmm0
                emit_byte(0xF2); emit_byte(0x0F); emit_byte(0x11);
                emit_modrm_disp(0, CPU_REG, offdhi);
            }
            return true;
        }
        // ── SIMD DUP (broadcast GPR to both halves) ────────────────
        default:
            return false;
    }
}
}  // namespace arm64emu
