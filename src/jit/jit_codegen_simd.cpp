// jit/jit_codegen_simd.cpp — FrostJIT SIMD/NEON IR-op codegen.
//
// v1.4.5-alpha: split out of jit_codegen_fp.cpp. This file holds
// the SIMD_* case bodies of the FP/SIMD IR-op switch, extracted into a
// separate method (compile_ir_simd) for readability. The compile_ir_inst_fp_()
// dispatcher in jit_codegen_fp.cpp calls this method before its residual
// cases.
//
// No behavior change — pure file split. The method is a member of FrostJIT
// (declared in include/jit/frostjit.hpp) so it has full access to the JIT's
// emit_*, alloc_*, flush_*, etc. helpers.
//
// Return value (bool — see frostjit.hpp):
//   true  = op handled here (caller returns false; FP/SIMD ops never end
//           a block)
//   false = op not handled here (caller falls through to the next
//           dispatcher or the residual switch in compile_ir_inst_fp_())
//
// Cases handled:
//   SIMD_LOGICAL — AND/ORR/EOR/BIC/ORN/EON (SSE2)
//   SIMD_ARITH   — integer lane-wise add/sub/mul/min/max
//   SIMD_CMP     — integer lane-wise compare (eq/gt/ge, signed & unsigned)
//   SIMD_DUP     — broadcast GPR to both halves
//   SIMD_LDST    — load/store v_lo/v_hi to/from vregs
//   SIMD_SHL/USHR/SSHR — vector, by immediate (SSE2 psll/psrl/psra)
#include "jit/frostjit.hpp"
#include "core/emulator.h"
#include "ir/ir.hpp"
#include <cstddef>
#include <cstdint>

// Slow-path helpers (defined extern "C" in x86_backend.cpp).
extern "C" {
void jit_load_mem16_slow(arm64emu::Emulator* emu, arm64emu::CPU* cpu, uint64_t addr, int dst);
void jit_store_mem16_slow(arm64emu::Emulator* emu, arm64emu::CPU* cpu, uint64_t addr, int src);
}
namespace arm64emu {
// ── FrostJIT::compile_ir_simd ─────────────────────────────────────────
// Handles all SIMD_* IR ops. Returns true if the op was handled, false if
// not (caller falls through to the next dispatcher or residual switch).
// The case bodies below are verbatim from jit_codegen_fp.cpp ()
// — no logic changes, just moved to a separate file/method.
bool FrostJIT::compile_ir_simd(const IRInst& inst) {
    switch (inst.op) {
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
                return true;
            }
            // ── Vector cache fast path (1.5.2-alpha) ──────────────
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
            emit_logical_half(off1hi, off2hi, offdhi);
            return true;
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
            emit_arith_half(off1hi, off2hi, offdhi);
            return true;
        }
        // ── SIMD FP lane-wise arithmetic (1.5.2-alpha) ─────────────
        // Emits packed SSE: addps/subps/mulps/divps/minps/maxps (single,
        // 0F prefix) or addpd/... (double, 66 0F prefix). FABD = sub then
        // clear sign bit (andps). width = element bytes (4/8); flags_op
        // = Q (1 = process both v_lo and v_hi, 0 = v_lo only).
        case IROp::SIMD_FP_ARITH: {
            uint8_t opc = static_cast<uint8_t>(inst.imm);
            int esize = static_cast<int>(inst.width);
            bool Q = (inst.flags_op != 0);
            bool is_double = (esize == 8);
            if (esize != 4 && esize != 8) {
                emit_call_interp(inst.arm_pc, false);
                return true;
            }
            // Map opcode to SSE op byte.
            //   single: 0F 58 addps, 5C subps, 59 mulps, 5E divps, 5D minps, 5F maxps
            //   double: 66 0F 58 addpd, 5C subpd, 59 mulpd, 5E divpd, 5D minpd, 5F maxpd
            uint8_t op_byte = 0;
            bool is_sub_for_fabd = false;
            switch (opc) {
                case 0: op_byte = 0x58; break;  // add
                case 1: op_byte = 0x5C; break;  // sub
                case 2: case 0xB: op_byte = 0x59; break;  // mul / fmulx
                case 3: op_byte = 0x5E; break;  // div
                case 4: case 6: op_byte = 0x5F; break;  // max / maxnm
                case 5: case 7: op_byte = 0x5D; break;  // min / minnm
                case 0xD: op_byte = 0x5C; is_sub_for_fabd = true; break;  // fabd
                default:
                    emit_call_interp(inst.arm_pc, false);
                    return true;
            }
            // ── Vector cache fast path (1.5.2-alpha) ──────────────
            // Same single-VEX-instruction trick as SIMD_FP_FMA: when all
            // three operands are pinned in host XMM regs, the whole
            // 128-bit vector op is one vaddps/pd-family instruction.
            {
                int xd = vec_xmm(inst.dest), xs1 = vec_xmm(inst.src1),
                    xs2 = vec_xmm(inst.src2);
                if (xd >= 0 && xs1 >= 0 && xs2 >= 0) {
                    clobber_flags();
                    if (is_sub_for_fabd) {
                        // vsubps/pd then clear sign bits (vandps with mask).
                        flush_invalidate_host_regs(1u << RAX);
                        emit_vex_fp_binop(xd, xs1, xs2, 0x5C, is_double);
                        uint64_t mask = is_double ? 0x7FFFFFFFFFFFFFFFULL
                                                  : 0x7FFFFFFF7FFFFFFFULL;
                        emit_mov_imm64(RAX, mask);
                        emit_byte(0x66); emit_byte(0x48);
                        emit_byte(0x0F); emit_byte(0x6E);
                        emit_byte(0xC0);  // movq xmm0, rax
                        emit_vex_fp_binop(xd, xd, 0, 0x54, is_double);
                    } else {
                        emit_vex_fp_binop(xd, xs1, xs2, op_byte, is_double);
                    }
                    if (!Q) {
                        // Zero upper 64 bits (guest 64-bit result).
                        bool r = (xd >= 8);
                        emit_byte(0xF3);
                        emit_byte(rex(false, r, false, r));
                        emit_byte(0x0F); emit_byte(0x7E);
                        emit_byte(static_cast<uint8_t>(0xC0 |
                                        ((xd & 7) << 3) | (xd & 7)));
                    }
                    vec_cache_mark_dirty(static_cast<int>(inst.dest));
                    return true;
                }
            }
            clobber_flags();
            flush_invalidate_host_regs((1u << RAX));
            auto emit_fp_chunk = [&](int32_t off1, int32_t off2, int32_t offd) {
                // movq xmm0, [rbx+off1] — load the 8-byte chunk (2 floats
                // or 1 double) into the low 64 bits of xmm0, zero upper.
                emit_byte(0xF3); emit_byte(0x0F); emit_byte(0x7E);
                emit_modrm_disp(0, CPU_REG, off1);
                // load src2 chunk into xmm1
                emit_byte(0xF3); emit_byte(0x0F); emit_byte(0x7E);
                emit_modrm_disp(1, CPU_REG, off2);
                // op: xmm0 = xmm0 OP xmm1 (packed; upper 64 are zero and
                // only the low 64 are stored back, so unused lanes are OK)
                if (is_double) emit_byte(0x66);
                emit_byte(0x0F); emit_byte(op_byte);
                emit_byte(0xC1);  // modrm(3, xmm0, xmm1)
                if (is_sub_for_fabd) {
                    // FABD: clear the sign bit of each lane. Single clears
                    // bit 31 of each 32-bit lane (mask 0x7FFF...7FFF);
                    // double clears bit 63. Load mask into xmm1 via RAX.
                    uint64_t mask = is_double ? 0x7FFFFFFFFFFFFFFFULL
                                              : 0x7FFFFFFF7FFFFFFFULL;
                    emit_mov_imm64(RAX, mask);
                    // movq xmm1, rax
                    emit_byte(0x66); emit_byte(0x48); emit_byte(0x0F); emit_byte(0x6E);
                    emit_byte(0xC8);
                    // andps (0F 54) / andpd (66 0F 54)
                    if (is_double) emit_byte(0x66);
                    emit_byte(0x0F); emit_byte(0x54); emit_byte(0xC1);
                }
                // movq [rbx+offd], xmm0 — store the low 8 bytes.
                emit_byte(0x66); emit_byte(0x0F); emit_byte(0xD6);
                emit_modrm_disp(0, CPU_REG, offd);
            };
            int32_t o1lo = V_LO_OFF + static_cast<int>(inst.src1) * 8;
            int32_t o1hi = V_HI_OFF + static_cast<int>(inst.src1) * 8;
            int32_t o2lo = V_LO_OFF + static_cast<int>(inst.src2) * 8;
            int32_t o2hi = V_HI_OFF + static_cast<int>(inst.src2) * 8;
            int32_t odlo = V_LO_OFF + static_cast<int>(inst.dest) * 8;
            int32_t odhi = V_HI_OFF + static_cast<int>(inst.dest) * 8;
            emit_fp_chunk(o1lo, o2lo, odlo);
            if (Q) {
                emit_fp_chunk(o1hi, o2hi, odhi);
            } else {
                // Zero v_hi[dest] for Q=0 (64-bit result).
                emit_mov_imm32_zext(RAX, 0);
                emit_store(CPU_REG, V_HI_OFF + static_cast<int>(inst.dest) * 8, RAX);
            }
            return true;
        }
        // ── SIMD FP fused 3-source (FMLA/FMLS) ───────────────────────
        // dest = dest ± src1*src2 per lane. Packed SSE: for each 8-byte
        // chunk, dest += src1*src2 (mul then add/sub). width = element
        // size (4=float via *ps, 8=double via *pd); flags_op = Q.
        // imm: 0=FMLA (+), 1=FMLS (-).
        case IROp::SIMD_FP_FMA: {
            int esize = static_cast<int>(inst.width);
            bool Q = (inst.flags_op != 0);
            bool is_double = (esize == 8);
            bool is_sub = (static_cast<uint8_t>(inst.imm) == 1);
            if (esize != 4 && esize != 8) {
                emit_call_interp(inst.arm_pc, false);
                return true;
            }
            // ── Vector cache fast path (1.5.2-alpha) ──────────────
            // When v_lo/v_hi of dest/src1/src2 are all pinned in host XMM
            // regs (vec_cache_active_ from the block pre-scan), the whole
            // 128-bit vector is processed in ONE fused VEX instruction —
            // a single vfmadd231ps/pd replaces the old 8 memory ops
            // (2 loads + fma + store per 64-bit half). The accumulator
            // stays in XMM across self-loop iterations (prologue skipped).
            {
                int xd = vec_xmm(inst.dest), xs1 = vec_xmm(inst.src1),
                    xs2 = vec_xmm(inst.src2);
                if (xd >= 0 && xs1 >= 0 && xs2 >= 0) {
                    clobber_flags();
                    emit_vex_fma(xd, xs1, xs2, is_double, is_sub);
                    if (!Q) {
                        // Zero upper 64 bits (guest 64-bit result): VMOVQ
                        // xmm, xmm — F3 0F 7E /r (zero-extending move).
                        bool r = (xd >= 8);
                        emit_byte(0xF3);
                        emit_byte(rex(false, r, false, r));
                        emit_byte(0x0F); emit_byte(0x7E);
                        emit_byte(static_cast<uint8_t>(0xC0 |
                                        ((xd & 7) << 3) | (xd & 7)));
                    }
                    vec_cache_mark_dirty(static_cast<int>(inst.dest));
                    return true;
                }
            }
            clobber_flags();
            flush_invalidate_host_regs((1u << RAX) | (1u << RCX) | (1u << RDX));
            auto emit_fma_chunk = [&](int32_t off1, int32_t off2, int32_t offd) {
                if (has_fma3()) {
                    // ── FMA3 native (single-rounded, IEEE 754-correct) ──
                    // Load the destination accumulator (Vd) into XMM0 and the
                    // first source (Vn) into XMM1; Vm is a memory operand.
                    //   FMLA: vfmadd231ps/pd xmm0, xmm1, [off2]
                    //           xmm0 = xmm0 + xmm1*Vm   (fused, 1 round)
                    //   FMLS: vfnmadd231ps/pd xmm0, xmm1, [off2]
                    //           xmm0 = xmm0 - xmm1*Vm   (fused, 1 round)
                    // VEX.NDS.128.66.0F38: byte1=0xE2 (R~=X~=B~=1, map 0F38),
                    //   byte2 = W<<7 | (~vvvv)<<3 | L<<2 | pp where vvvv~ =
                    //   ~1 = 1110 (NDS=xmm1), L=0, pp=01 (66). Opcode 0xB8
                    //   (vfmadd231ps/pd) / 0xBC (vfnmadd231ps/pd). W picks
                    //   single (0) vs double (1). The m128 memory operand
                    //   reads 16 bytes but only the low 8 are used (upper
                    //   lanes of the packed op are discarded on the 8-byte
                    //   store back); the read stays inside the CPU struct.
                    emit_byte(0xF2); emit_byte(0x0F); emit_byte(0x10);
                    emit_modrm_disp(0, CPU_REG, offd);  // movsd xmm0, [rbx+offd]
                    emit_byte(0xF2); emit_byte(0x0F); emit_byte(0x10);
                    emit_modrm_disp(1, CPU_REG, off1);  // movsd xmm1, [rbx+off1]
                    emit_byte(0xC4); emit_byte(0xE2);
                    emit_byte(is_double ? 0xF1 : 0x71);  // W | 0x70 | 0x01
                    emit_byte(is_sub ? 0xBC : 0xB8);
                    emit_modrm_disp(0, CPU_REG, off2);   // vf[n]madd231ps/pd xmm0, xmm1, [rbx+off2]
                    emit_byte(0xF2); emit_byte(0x0F); emit_byte(0x11);
                    emit_modrm_disp(0, CPU_REG, offd);  // movsd [rbx+offd], xmm0
                    return;
                }
                // movsd xmm0, [rbx+offd]  (dest accumulator)
                emit_byte(0xF2); emit_byte(0x0F); emit_byte(0x10);
                emit_modrm_disp(0, CPU_REG, offd);
                // movsd xmm1, [rbx+off1]
                emit_byte(0xF2); emit_byte(0x0F); emit_byte(0x10);
                emit_modrm_disp(1, CPU_REG, off1);
                // movsd xmm2, [rbx+off2]
                emit_byte(0xF2); emit_byte(0x0F); emit_byte(0x10);
                emit_modrm_disp(2, CPU_REG, off2);
                // mulps/mulpd xmm1, xmm2
                if (is_double) emit_byte(0x66);
                emit_byte(0x0F); emit_byte(0x59); emit_byte(0xCA);  // modrm(3, xmm1, xmm2)
                // addps/addpd (or subps/subpd) xmm0, xmm1
                if (is_double) emit_byte(0x66);
                emit_byte(0x0F);
                emit_byte(is_sub ? 0x5C : 0x58);
                emit_byte(0xC1);  // modrm(3, xmm0, xmm1)
                // movsd [rbx+offd], xmm0
                emit_byte(0xF2); emit_byte(0x0F); emit_byte(0x11);
                emit_modrm_disp(0, CPU_REG, offd);
            };
            int32_t o1lo = V_LO_OFF + static_cast<int>(inst.src1) * 8;
            int32_t o1hi = V_HI_OFF + static_cast<int>(inst.src1) * 8;
            int32_t o2lo = V_LO_OFF + static_cast<int>(inst.src2) * 8;
            int32_t o2hi = V_HI_OFF + static_cast<int>(inst.src2) * 8;
            int32_t odlo = V_LO_OFF + static_cast<int>(inst.dest) * 8;
            int32_t odhi = V_HI_OFF + static_cast<int>(inst.dest) * 8;
            emit_fma_chunk(o1lo, o2lo, odlo);
            if (Q) {
                emit_fma_chunk(o1hi, o2hi, odhi);
            } else {
                emit_mov_imm32_zext(RAX, 0);
                emit_store(CPU_REG, V_HI_OFF + static_cast<int>(inst.dest) * 8, RAX);
            }
            return true;
        }
        // ── SIMD CMP (integer lane-wise compare) ─────────────────────
        // imm (opc) = 0=CMEQ, 1=CMGT, 2=CMGE, 3=CMHI, 4=CMHS; width =
        // esize (1/2/4/8); flags_op = Q (threaded by the translator so the
        // Q=0 64-bit forms zero v_hi, matching the interpreter). XMM
        // scratch: XMM0 = a, XMM1 = b, XMM2 = sign-flip mask / all-ones.
        // SIMD_CMP is NOT vec-cache-compatible (see vec_cache_compatible_op
        // in jit_codegen_vec_cache.cpp), so blocks containing it never get
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
        case IROp::SIMD_CMP: {
            uint8_t opc = static_cast<uint8_t>(inst.imm);
            int esize = static_cast<int>(inst.width);
            bool Q = (inst.flags_op != 0);
            if (opc > 4 || (esize != 1 && esize != 2 && esize != 4 && esize != 8)) {
                emit_call_interp(inst.arm_pc, false);
                return true;
            }
            const bool ge  = (opc == 2 || opc == 4);  // >= variants: compute b>a, invert
            const bool uns = (opc == 3 || opc == 4);  // unsigned: sign-flip trick
            clobber_flags();
            flush_invalidate_host_regs((1u << RAX) | (1u << RCX) | (1u << RDX));
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
                // interpreter's SIMD_DP compare block (interp_fp.cpp).
                emit_byte(0x66); emit_byte(0x0F); emit_byte(0xEF);
                emit_byte(0xC0);  // pxor xmm0, xmm0
                emit_byte(0xF2); emit_byte(0x0F); emit_byte(0x11);
                emit_modrm_disp(0, CPU_REG, offdhi);
            }
            return true;
        }
        // ── SIMD DUP (broadcast GPR to both halves) ────────────────
        case IROp::SIMD_DUP: {
            // v_lo[dest] = v_hi[dest] = broadcast of src1's low element.
            // width = esize (1/2/4/8, default 8); flags_op = Q.
            // esize<8: mask the low element then shift-replicate it across
            // the qword in RAX (RCX = scratch); esize==8: full GPR value.
            const int esize = inst.width ? static_cast<int>(inst.width) : 8;
            const bool q = inst.flags_op != 0;
            auto emit_dup_qword = [&]() {
                if (esize >= 8) return;
                clobber_host_reg(RCX);
                switch (esize) {
                case 1:  // keep low byte, replicate to 8 copies
                    emit_shift_imm8(RAX, 4, 56); emit_shift_imm8(RAX, 5, 56);
                    emit_mov_reg(RCX, RAX); emit_shift_imm8(RAX, 4, 8);  emit_or_reg(RAX, RCX);
                    emit_mov_reg(RCX, RAX); emit_shift_imm8(RAX, 4, 16); emit_or_reg(RAX, RCX);
                    emit_mov_reg(RCX, RAX); emit_shift_imm8(RAX, 4, 32); emit_or_reg(RAX, RCX);
                    break;
                case 2:  // keep low halfword, replicate to 4 copies
                    emit_shift_imm8(RAX, 4, 48); emit_shift_imm8(RAX, 5, 48);
                    emit_mov_reg(RCX, RAX); emit_shift_imm8(RAX, 4, 16); emit_or_reg(RAX, RCX);
                    emit_mov_reg(RCX, RAX); emit_shift_imm8(RAX, 4, 32); emit_or_reg(RAX, RCX);
                    break;
                case 4:  // keep low word, replicate to 2 copies
                    emit_shift_imm8(RAX, 4, 32); emit_shift_imm8(RAX, 5, 32);
                    emit_mov_reg(RCX, RAX); emit_shift_imm8(RAX, 4, 32); emit_or_reg(RAX, RCX);
                    break;
                }
            };
            // ── Vector cache fast path (1.5.2-alpha) ──────────────
            // dest pinned: vmovq xd, gpr (zero upper) then vmovddup
            // (broadcast low qword to both halves) — no memory bounce.
            {
                int xd = vec_xmm(inst.dest);
                if (xd >= 0) {
                    int s = ensure_vreg(inst.src1, RAX);
                    if (s != RAX) {
                        clobber_host_reg(RAX);
                        emit_mov_reg(RAX, s);
                    } else if (esize < 8) {
                        // s == RAX: the shift-replicate chain below destroys
                        // src1's value in RAX while RAX is still mapped to
                        // src1. Drop the mapping (spilling if dirty) BEFORE
                        // the chain so later readers of src1 in this block
                        // reload from its home instead of the broadcast.
                        clobber_host_reg(RAX);
                    }
                    emit_dup_qword();
                    // vmovq xd, rax  (VEX.128.66.0F.W1 6E /r — pp=66, NOT F3:
                    // the F3.0F.W1 6E form is not a valid AVX encoding; the
                    // assembler emits 66.0F.W1 6E with the XMM dest in
                    // ModRM.reg and vvvv=1111 unused).
                    emit_vex3(1, true, 0, 1, xd, RAX, true, 0x6E);
                    // vmovddup xd, xd  (VEX.128.F2.0F.WIG 12: broadcast low qword)
                    if (q) emit_vex3(1, false, 0, 3, xd, xd, true, 0x12);
                    vec_cache_mark_dirty(static_cast<int>(inst.dest));
                    return true;
                }
            }
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
            } else if (esize < 8) {
                // s == RAX: the shift-replicate chain destroys src1's value
                // in RAX while RAX is still mapped to src1. Drop the mapping
                // (spilling if dirty) BEFORE the chain so later readers of
                // src1 reload from its home instead of the broadcast. (For
                // Q=0 the old code's trailing clobber masked this when src1
                // was clean, but a dirty src1 — or Q=1 — corrupted it.)
                clobber_host_reg(RAX);
            }
            emit_dup_qword();
            int32_t offlo = V_LO_OFF + static_cast<int>(inst.dest) * 8;
            int32_t offhi = V_HI_OFF + static_cast<int>(inst.dest) * 8;
            emit_store(CPU_REG, offlo, RAX);
            if (q) {
                emit_store(CPU_REG, offhi, RAX);
            } else {
                clobber_host_reg(RAX);
                emit_mov_imm32_zext(RAX, 0);
                emit_store(CPU_REG, offhi, RAX);
            }
            // src1 stays cached in its original reg (s) for later readers.
            // RAX holds a copy (not a cached vreg) — no mapping to update.
            return true;
        }
        // ── SIMD UMOV (vector element -> GPR) ───────────────────────
        // regs[dest] = element[imm] of vector src1. width = esize
        // (1/2/4/8), imm = lane index, flags_op = Q (0=W d, 1=X d — the
        // codegen reads the full element regardless, mirroring the interp).
        // NOT in vec_cache_compatible_op, so src1 is never pinned: reading
        // cpu.v_lo/v_hi directly is always current (no stale-XMM hazard).
        // The zero-extending loads (emit_load32/16/8 are movzx) match the
        // interp's zero-extended element read for esize < 8; esize == 8
        // loads the full qword from v_lo or v_hi per the lane index.
        case IROp::SIMD_UMOV: {
            const int esize = inst.width ? static_cast<int>(inst.width) : 8;
            const int index = static_cast<int>(inst.imm);
            const int byte_off = index * esize;        // offset into the 16-byte vector
            const int qword = byte_off / 8;            // 0 -> v_lo, 1 -> v_hi
            const int32_t off = (qword == 0 ? V_LO_OFF : V_HI_OFF)
                              + static_cast<int>(inst.src1) * 8 + (byte_off % 8);
            int d = alloc_reg();
            switch (esize) {
                case 1:  emit_load8(d, CPU_REG, off);  break;
                case 2:  emit_load16(d, CPU_REG, off); break;
                case 4:  emit_load32(d, CPU_REG, off); break;
                default: emit_load(d, CPU_REG, off);   break;  // 8 bytes
            }
            set_vreg_reg(inst.dest, d);
            return true;
        }
        // ── SIMD MOVI/MVNI (broadcast lane pattern, AdvSIMD modified imm) ──
        // v_lo[dest] = imm; v_hi[dest] = Q ? imm : 0. flags_op = Q.
        // One movabs + one vmovq (vmovq already zeroes the upper half for
        // Q=0); vmovddup broadcasts the pattern to both halves for Q=1.
        case IROp::SIMD_MOVI: {
            clobber_host_reg(RAX);
            emit_mov_imm64(RAX, inst.imm);
            {
                int xd = vec_xmm(static_cast<int>(inst.dest));
                if (xd >= 0) {
                    emit_vex3(1, true, 0, 1, xd, RAX, true, 0x6E);  // vmovq xd, rax
                    if (inst.flags_op)
                        emit_vex3(1, false, 0, 3, xd, xd, true, 0x12);  // vmovddup xd,xd
                    vec_cache_mark_dirty(static_cast<int>(inst.dest));
                    return true;
                }
            }
            int32_t mofflo = V_LO_OFF + static_cast<int>(inst.dest) * 8;
            int32_t moffhi = V_HI_OFF + static_cast<int>(inst.dest) * 8;
            emit_store(CPU_REG, mofflo, RAX);
            if (inst.flags_op) {
                emit_store(CPU_REG, moffhi, RAX);
            } else {
                emit_mov_imm32_zext(RAX, 0);
                emit_store(CPU_REG, moffhi, RAX);
            }
            return true;
        }
        // ── SIMD ORR/BIC immediate (read-modify-write the destination) ──
        // v_lo[dest],v_hi[dest] = dest OR/BIC imm. cond = 0=ORR, 1=BIC;
        // flags_op = Q. ORR/BIC (vector, immediate) read Vd as their source,
        // exactly like the interpreter's apply_or_bic_u{32,16}.
        case IROp::SIMD_ORRIMM: {
            clobber_flags();
            flush_invalidate_host_regs((1u << RAX) | (1u << RCX));
            clobber_host_reg(RAX);
            emit_mov_imm64(RAX, inst.imm);
            {
                int xd = vec_xmm(static_cast<int>(inst.dest));
                if (xd >= 0) {
                    // Pattern into scratch XMM0, then VEX logical with xd.
                    emit_vex3(1, true, 0, 1, 0, RAX, true, 0x6E);    // vmovq xmm0, rax
                    if (inst.flags_op)
                        emit_vex3(1, false, 0, 3, 0, 0, true, 0x12); // vmovddup xmm0,xmm0
                    if (inst.cond) {
                        // BIC: xd = xd & ~imm = vpandn xd, xmm0(imm), xd
                        emit_vex3(1, false, 0, 1, xd, xd, true, 0xDF);
                    } else {
                        // ORR: xd = xd | imm
                        emit_vex3(1, false, xd, 1, xd, 0, true, 0xEB);
                    }
                    if (!inst.flags_op)
                        emit_vex3(1, false, 0, 2, xd, xd, true, 0x7E);  // vmovq xd,xd → zero upper (F3)
                    vec_cache_mark_dirty(static_cast<int>(inst.dest));
                    return true;
                }
            }
            // Memory path: GPR or/and against the pattern in RAX.
            int32_t olo = V_LO_OFF + static_cast<int>(inst.dest) * 8;
            int32_t ohi = V_HI_OFF + static_cast<int>(inst.dest) * 8;
            emit_load(RCX, CPU_REG, olo);
            if (inst.cond) { emit_not_reg(RAX); emit_and_reg(RCX, RAX); }
            else           { emit_or_reg(RCX, RAX); }
            emit_store(CPU_REG, olo, RCX);
            if (inst.flags_op) {
                emit_mov_imm64(RAX, inst.imm);
                emit_load(RCX, CPU_REG, ohi);
                if (inst.cond) { emit_not_reg(RAX); emit_and_reg(RCX, RAX); }
                else           { emit_or_reg(RCX, RAX); }
                emit_store(CPU_REG, ohi, RCX);
            } else {
                emit_mov_imm32_zext(RCX, 0);
                emit_store(CPU_REG, ohi, RCX);
            }
            return true;
        }
        // ── SIMD LDST (read/write v_lo/v_hi to/from vregs) ─────────
        case IROp::SIMD_LDST: {
            // width=1 (load): src1=lo vreg, src2=hi vreg → v_lo[dest], v_hi[dest]
            // width=0 (store): v_lo[dest] → src1 vreg, v_hi[dest] → src2 vreg
            if (inst.width == 1) {
                // Load: write vregs to v_lo/v_hi
                // use separate host regs for lo/hi so we
                // don't clobber src1's cached value when loading src2.
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
            return true;
        }
        // ── SIMD LD16/ST16 (16-byte guest memory access, 1..4 vregs) ──
        // One bounds-check + one movupd per 16 bytes (fast path), or one
        // call into jit_load_mem16_slow/jit_store_mem16_slow per reg
        // (slow path). flags_op = register count (1..4, default 1);
        // dest/src2 = FIRST vreg; vregs dest+0..count-1 are contiguous.
        // Replaces per-register 8-byte LOAD_MEM/STORE_MEM + SIMD_LDST
        // chains, which each did their own 10-byte mov imm64 limit + cmp +
        // jbe + add window.
        case IROp::SIMD_LD16: {
            clobber_flags();
            constexpr uint16_t MEM_CLOBBER =
                (1u << RAX) | (1u << RCX) | (1u << RDX) |
                (1u << R8)  | (1u << R9)  | (1u << R11);
            flush_invalidate_host_regs(MEM_CLOBBER);
            uint32_t nregs = inst.flags_op ? inst.flags_op : 1;
            // rax = addr + imm
            load_vreg_to_reg(RAX, inst.src1);
            if (inst.imm != 0) emit_add_reg_imm(RAX, static_cast<int32_t>(inst.imm));
            // Limit check: addr + 16*nregs <= DIRECT_WINDOW_SIZE.
            int tmp = (RAX != RDX) ? RDX : RCX;
            emit_mov_imm64(tmp, Memory::DIRECT_WINDOW_SIZE - static_cast<uint64_t>(16 * nregs));
            emit_cmp_reg(RAX, tmp);
            size_t jbe_patch = emit_jcc_rel32_placeholder(6);  // JBE
            // Slow path: N calls to jit_load_mem16_slow(emu, cpu, addr, dst).
            // NOTE: reload the base address each iteration — the C call
            // clobbers RAX (caller-saved), so a running "addr += 16" across
            // calls would add to garbage for i>=1.
            // Vec-cache guard: the C helper writes cpu.v_lo/v_hi[dest] and the
            // host call clobbers all XMM0-15 (caller-saved), so write back any
            // dirty pinned vectors first and reload them after — same round-trip
            // as emit_call_interp. CRITICAL: use writeback_all(FALSE) and emit
            // the reloads WITHOUT clearing vec_dirty_ — the slow-path code is
            // SKIPPED at runtime on the fast path, so clearing the flags here at
            // codegen time would suppress the epilogue writeback for vectors
            // dirtied earlier in the block (lost values → wrong branch → hang).
            if (vec_cache_active_) vec_cache_writeback_all(false);
            for (uint32_t i = 0; i < nregs; i++) {
                load_vreg_to_reg(RAX, inst.src1);
                if (inst.imm != 0 || i != 0) {
                    emit_add_reg_imm(RAX, static_cast<int32_t>(inst.imm + 16 * i));
                }
                emit_push(WIN_REG);
                emit_mov_reg(RDI, EMU_REG);
                emit_mov_reg(RSI, CPU_REG);
                emit_mov_reg(RDX, RAX);
                emit_mov_imm32(RCX, (inst.dest + i) & 31);
                emit_call_aligned(&jit_load_mem16_slow, /*num_pushed=*/1);
                emit_pop(WIN_REG);
            }
            if (vec_cache_active_) {
                for (int pi = 0; pi < vec_pinned_count_; pi++) {
                    int v = vec_pinned_[pi];
                    vec_emit_load_lo_hi(vec_cache_[v], v);
                }
            }
            size_t jmp_past = emit_jmp_rel32_placeholder();
            // Fast path: rax += window; per reg load the 16 bytes.
            // If dest+i is pinned in the vec cache, load straight into the
            // pinned XMM (one movupd) and mark it dirty for the epilogue
            // writeback. Otherwise movupd into scratch xmm0 then spill both
            // halves to v_lo/v_hi[dest+i].
            int32_t fast_rel = static_cast<int32_t>(code_buf_used_ - (jbe_patch + 6));
            patch_jcc_rel32(jbe_patch, fast_rel);
            emit_add_reg(RAX, WIN_REG);
            for (uint32_t i = 0; i < nregs; i++) {
                int xdst = vec_xmm((inst.dest + i) & 31);
                if (xdst >= 0) {
                    // movupd xmmN, [rax+i*16] — 66 [REX.R] 0F 10 /r.
                    emit_byte(0x66);
                    if (xdst >= 8) emit_byte(rex(false, true, false, false));
                    emit_byte(0x0F); emit_byte(0x10);
                    if (i == 0) {
                        emit_modrm_disp(xdst, 0, 0);
                    } else {
                        emit_modrm_disp(xdst, 0, static_cast<int32_t>(16 * i));
                    }
                    vec_cache_mark_dirty((inst.dest + i) & 31);
                } else {
                    // movupd xmm0, [rax+i*16]  — 66 0F 10 /r.
                    emit_byte(0x66); emit_byte(0x0F); emit_byte(0x10);
                    if (i == 0) {
                        emit_modrm_disp(0, 0, 0);
                    } else {
                        emit_modrm_disp(0, 0, static_cast<int32_t>(16 * i));
                    }
                    // movsd [rbx+v_lo[dest+i]], xmm0 — F2 0F 11 /r.
                    int32_t offlo = V_LO_OFF + ((inst.dest + i) & 31) * 8;
                    emit_byte(0xF2); emit_byte(0x0F); emit_byte(0x11);
                    emit_modrm_disp(0, CPU_REG, offlo);
                    // movhpd [rbx+v_hi[dest+i]], xmm0 — 66 0F 17 /r.
                    int32_t offhi = V_HI_OFF + ((inst.dest + i) & 31) * 8;
                    emit_byte(0x66); emit_byte(0x0F); emit_byte(0x17);
                    emit_modrm_disp(0, CPU_REG, offhi);
                }
            }
            int32_t end_rel = static_cast<int32_t>(code_buf_used_ - (jmp_past + 5));
            patch_jmp_rel32(jmp_past, end_rel);
            return true;
        }
        case IROp::SIMD_ST16: {
            clobber_flags();
            constexpr uint16_t MEM_CLOBBER =
                (1u << RAX) | (1u << RCX) | (1u << RDX) |
                (1u << R8)  | (1u << R9)  | (1u << R11);
            flush_invalidate_host_regs(MEM_CLOBBER);
            uint32_t nregs = inst.flags_op ? inst.flags_op : 1;
            // cond=1 → broadcast: every 16-byte half stores the SAME source
            // vector (`stp q0,q0` memset pattern), so src2 stays constant
            // across the loop instead of src2+i.
            bool broadcast = (inst.cond != 0);
            // rax = addr + imm
            load_vreg_to_reg(RAX, inst.src1);
            if (inst.imm != 0) emit_add_reg_imm(RAX, static_cast<int32_t>(inst.imm));
            // Limit check.
            int tmp = (RAX != RDX) ? RDX : RCX;
            emit_mov_imm64(tmp, Memory::DIRECT_WINDOW_SIZE - static_cast<uint64_t>(16 * nregs));
            emit_cmp_reg(RAX, tmp);
            size_t jbe_patch = emit_jcc_rel32_placeholder(6);  // JBE
            // Slow path: N calls to jit_store_mem16_slow(emu, cpu, addr, src).
            // Reload the base address each iteration (the C call clobbers
            // RAX), same as the LD16 slow path above.
            // Vec-cache guard: the C helper reads cpu.v_lo/v_hi[src] and the
            // host call clobbers all XMM0-15 (caller-saved), so write back any
            // dirty pinned vectors first and reload them after — same round-trip
            // as emit_call_interp. CRITICAL: use writeback_all(FALSE) and emit
            // the reloads WITHOUT clearing vec_dirty_ — the slow-path code is
            // SKIPPED at runtime on the fast path, so clearing the flags here at
            // codegen time would suppress the epilogue writeback for vectors
            // dirtied earlier in the block (lost values → wrong branch → hang).
            if (vec_cache_active_) vec_cache_writeback_all(false);
            for (uint32_t i = 0; i < nregs; i++) {
                load_vreg_to_reg(RAX, inst.src1);
                if (inst.imm != 0 || i != 0) {
                    emit_add_reg_imm(RAX, static_cast<int32_t>(inst.imm + 16 * i));
                }
                emit_push(WIN_REG);
                emit_mov_reg(RDI, EMU_REG);
                emit_mov_reg(RSI, CPU_REG);
                emit_mov_reg(RDX, RAX);
                emit_mov_imm32(RCX, broadcast ? (inst.src2 & 31) : ((inst.src2 + i) & 31));
                emit_call_aligned(&jit_store_mem16_slow, /*num_pushed=*/1);
                emit_pop(WIN_REG);
            }
            if (vec_cache_active_) {
                for (int pi = 0; pi < vec_pinned_count_; pi++) {
                    int v = vec_pinned_[pi];
                    vec_emit_load_lo_hi(vec_cache_[v], v);
                }
            }
            size_t jmp_past = emit_jmp_rel32_placeholder();
            // Fast path: rax += window; per reg store the source vector.
            // If src2+i is pinned in the vec cache, store straight from the
            // pinned XMM (one movupd, no cpu.v_lo/v_hi reload) — the memset
            // loop's loop-invariant q0 stays resident across iterations.
            // Otherwise build xmm0 = {v_lo[src], v_hi[src]} then store.
            int32_t fast_rel = static_cast<int32_t>(code_buf_used_ - (jbe_patch + 6));
            patch_jcc_rel32(jbe_patch, fast_rel);
            emit_add_reg(RAX, WIN_REG);
            for (uint32_t i = 0; i < nregs; i++) {
                int xsrc = vec_xmm(broadcast ? (inst.src2 & 31) : ((inst.src2 + i) & 31));
                if (xsrc >= 0) {
                    // movupd [rax+i*16], xmmN — 66 [REX.R] 0F 11 /r.
                    emit_byte(0x66);
                    if (xsrc >= 8) emit_byte(rex(false, true, false, false));
                    emit_byte(0x0F); emit_byte(0x11);
                    if (i == 0) {
                        emit_modrm_disp(xsrc, 0, 0);
                    } else {
                        emit_modrm_disp(xsrc, 0, static_cast<int32_t>(16 * i));
                    }
                } else {
                    // movsd xmm0, [rbx+v_lo[src]] — F2 0F 10 /r.
                    int32_t s_lo = V_LO_OFF + (broadcast ? (inst.src2 & 31) : ((inst.src2 + i) & 31)) * 8;
                    emit_byte(0xF2); emit_byte(0x0F); emit_byte(0x10);
                    emit_modrm_disp(0, CPU_REG, s_lo);
                    // movhpd xmm0, [rbx+v_hi[src]] — 66 0F 16 /r.
                    int32_t s_hi = V_HI_OFF + (broadcast ? (inst.src2 & 31) : ((inst.src2 + i) & 31)) * 8;
                    emit_byte(0x66); emit_byte(0x0F); emit_byte(0x16);
                    emit_modrm_disp(0, CPU_REG, s_hi);
                    // movupd [rax+i*16], xmm0 — 66 0F 11 /r.
                    emit_byte(0x66); emit_byte(0x0F); emit_byte(0x11);
                    if (i == 0) {
                        emit_modrm_disp(0, 0, 0);
                    } else {
                        emit_modrm_disp(0, 0, static_cast<int32_t>(16 * i));
                    }
                }
            }
            int32_t end_rel = static_cast<int32_t>(code_buf_used_ - (jmp_past + 5));
            patch_jmp_rel32(jmp_past, end_rel);
            return true;
        }
        // ── SIMD SHL/USHR/SSHR/USRA/SSRA/SLI/SRI (vector, by immediate) ──
        // v1.4.5-alpha: native SSE2 codegen via psllw/pslld/psllq (SHL),
        // psrlw/psrld/psrlq (USHR), psraw/psrad (SSHR). 1.5.2-alpha:
        // extended to USRA/SSRA/SLI/SRI (accumulate / insert-merge) and
        // added an AVX2 256-bit VEX path on capable hosts. Previously
        // these all fell back to CALL_INTERP (~20% overhead on SIMD-heavy
        // workloads).
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
        // Per-op semantic mapping (Vn = src1, Vd = dest accumulator):
        //   SHL   Vd = Vn << shift                          (PSLL)
        //   USHR  Vd = Vn >> shift   (logical)              (PSRL)
        //   SSHR  Vd = Vn >> shift   (arithmetic)           (PSRA)
        //   USRA  Vd = Vd + (Vn >> shift)                   (PSRL + PADD)
        //   SSRA  Vd = Vd + (Vn >> shift)  (arithmetic)     (PSRA + PADD)
        //   SLI   Vd = (Vn << shift) | (Vd & ((1<<shift)-1))
        //                                                    (PSLL + PSLL/PSRL + POR)
        //   SRI   Vd = (Vn >> shift) | (Vd & ~((1<<(esize*8-shift))-1))
        //                                                    (PSRL + PSRL/PSLL + POR)
        //
        // SLI/SRI are shift-INSERT ops: the source is shifted and OR'd with
        // the UNCHANGED destination bits in the vacated positions. Per the
        // ARM ARM pseudocode (result = (Vd AND NOT(mask)) OR shifted), SLI
        // retains Vd's low `shift` bits and SRI retains Vd's top `shift`
        // bits. A single shift of Vd by (esize*8-shift) would read the WRONG
        // half of Vd, so the retained half is produced by shifting Vd left
        // and right by (esize*8-shift) in sequence (which isolates the kept
        // bits in place).
        //
        // The modrm byte is 11_<reg>_<rm> where <rm> selects the xmmN.
        // PSLL/PSRL/PSRA do NOT have a PSLLB/PSRLB/PSRAB form in SSE2
        // (8-bit element shifts); we fall back to CALL_INTERP for esize=1.
        // 64-bit SSRA (psraq) requires AVX-512 — we fall back for esize=8.
        //
        // x86 shift semantics match ARM for shift ∈ [0, esize*8]:
        //   - shift=0: no-op (both)
        //   - shift=esize*8: SHL/USHR clear the lane; SSHR sign-fills it
        //     (immediate shifts with count ≥ element size saturate).
        // The IR translator guarantees shift ∈ [0, esize*8].
        //
        // Q bit (flags_op): 1 = 128-bit (process v_lo AND v_hi), 0 = 64-bit
        // (process v_lo only, ZERO v_hi of the result). On AVX2 hosts we
        // process both halves in one 256-bit VEX instruction: v_lo in
        // bits[0:63], v_hi in bits[128:191] (packed via vinserti128), so the
        // guest lane numbering maps directly onto 256-bit lanes and both
        // halves shift together. VEX 3-byte prefix format:
        //   C4 [R~ X~ B~ mmmmm] [W vvvv~ L pp] <opcode> <modrm>
        // with mmmmm=00001 (0F map) → byte1 0xE1, mmmmm=00011 (0F3A) → 0xE3,
        // L=1 (256-bit), pp=01 (66 prefix). vvvv~ is the inverted 4-bit
        // NDS register; 1111b when unused.
        case IROp::SIMD_SHL:
        case IROp::SIMD_USHR:
        case IROp::SIMD_SSHR:
        case IROp::SIMD_USRA:
        case IROp::SIMD_SSRA:
        case IROp::SIMD_URSRA:
        case IROp::SIMD_SRSRA:
        case IROp::SIMD_SLI:
        case IROp::SIMD_SRI: {
            int esize = static_cast<int>(inst.width);
            uint8_t shift = static_cast<uint8_t>(inst.imm);
            bool q = (inst.flags_op != 0);
            // Fall back to CALL_INTERP for unsupported element sizes.
            //  - esize=1 (8-bit): no PSLLB/PSRLB/PSRAB in SSE2.
            //  - esize=8 SSHR/SSRA/SRSRA: 64-bit arithmetic shifts need
            //    PSRAQ, which is AVX-512F only (NOT SSE2/AVX2 — a plain
            //    `66 0F 73 /4 ib` = PSRAQ SIGILLs the host).
            //  - Invalid esize: shouldn't happen, but be safe.
            if (esize != 2 && esize != 4 && esize != 8) {
                emit_call_interp(inst.arm_pc, false);
                return true;
            }
            if ((inst.op == IROp::SIMD_SSHR || inst.op == IROp::SIMD_SSRA ||
                 inst.op == IROp::SIMD_SRSRA) && esize == 8) {
                emit_call_interp(inst.arm_pc, false);
                return true;
            }
            // Decode the shift group subop (0x71/0x72/0x73) plus the primary
            // reg field (6=PSLL, 2=PSRL, 4=PSRA) applied to Vn.
            uint8_t subop = 0;
            uint8_t reg_field = 0;
            uint8_t padd_op = 0;
            if (esize == 2)      { subop = 0x71; padd_op = 0xFD; }  // paddw
            else if (esize == 4) { subop = 0x72; padd_op = 0xFE; }  // paddd
            else                 { subop = 0x73; padd_op = 0xD4; }  // paddq (esize == 8)
            if (inst.op == IROp::SIMD_SHL || inst.op == IROp::SIMD_SLI) {
                reg_field = 6;   // PSLL (shift left)
            } else if (inst.op == IROp::SIMD_USHR ||
                       inst.op == IROp::SIMD_USRA ||
                       inst.op == IROp::SIMD_URSRA ||
                       inst.op == IROp::SIMD_SRI) {
                reg_field = 2;   // PSRL (logical shift right)
            } else {             // SIMD_SSHR / SIMD_SSRA / SIMD_SRSRA
                reg_field = 4;   // PSRA (arithmetic shift right)
            }
            // URSRA/SRSRA are rounding shift-accumulates: the rounded
            // right-shift RShr(x, s) = (x >> s) + ((x >> (s-1)) & 1),
            // i.e. add the top bit of the discarded low `s` bits. Native
            // pipeline: shifted = Vn >> s; round_bit = (Vn >> (s-1)) & mask;
            // Vd = Vd + shifted + round_bit. (No overflow: the rounding
            // carry never re-enters the shifted value.)
            bool is_rounding = (inst.op == IROp::SIMD_URSRA ||
                                inst.op == IROp::SIMD_SRSRA);
            uint8_t insert_shift = static_cast<uint8_t>(esize * 8) - shift;
            clobber_flags();
            // SSE2 shifts only use XMM regs (no GPRs). But emit_call_interp
            // and other paths below might clobber RAX/RCX/RDX, so flush
            // them to keep the register-cache consistent.
            flush_invalidate_host_regs((1u << RAX) | (1u << RCX) | (1u << RDX));
            int32_t off1lo = V_LO_OFF + static_cast<int>(inst.src1) * 8;
            int32_t off1hi = V_HI_OFF + static_cast<int>(inst.src1) * 8;
            int32_t offdlo = V_LO_OFF + static_cast<int>(inst.dest) * 8;
            int32_t offdhi = V_HI_OFF + static_cast<int>(inst.dest) * 8;
            // movsd xmmN, [rbx+off]  (F2 0F 10 — load 64 bits, zero upper 64)
            auto emit_load64 = [&](int x, int32_t off) {
                emit_byte(0xF2); emit_byte(0x0F); emit_byte(0x10);
                emit_modrm_disp(x, CPU_REG, off);
            };
            // movsd [rbx+off], xmmN  (F2 0F 11 — store low 64 bits)
            auto emit_store64 = [&](int x, int32_t off) {
                emit_byte(0xF2); emit_byte(0x0F); emit_byte(0x11);
                emit_modrm_disp(x, CPU_REG, off);
            };
            // 66 0F <subop> <modrm(3, rf, xmmN)> <imm8>  (PSLL/PSRL/PSRA)
            auto emit_shift_imm = [&](int x, uint8_t rf, uint8_t sh) {
                emit_byte(0x66); emit_byte(0x0F); emit_byte(subop);
                emit_byte(0xC0 | (rf << 3) | x);
                emit_byte(sh);
            };
            auto emit_padd = [&](int d, int s) {
                emit_byte(0x66); emit_byte(0x0F); emit_byte(padd_op);
                emit_byte(0xC0 | (d << 3) | s);
            };
            auto emit_por = [&](int d, int s) {
                emit_byte(0x66); emit_byte(0x0F); emit_byte(0xEB);
                emit_byte(0xC0 | (d << 3) | s);
            };
            // VEX 3-byte prefix: C4 <b1> <b2> — mmmmm selects the opcode map
            // (00001=0F, 00011=0F3A), R~=X~=B~=1 (low regs), L=1 (256-bit).
            // `vvvv_nds` is the raw NDS register index; pass 0 for "unused"
            // (encodes 1111b, the required reserved value).
            auto emit_vex_prefix = [&](uint8_t mmmmm, int vvvv_nds, uint8_t pp) {
                emit_byte(0xC4);
                emit_byte(0xE0 | mmmmm);          // 0xE1 (0F) / 0xE3 (0F3A)
                emit_byte((((~vvvv_nds) & 0x0F) << 3) | 0x04 | pp);  // L=1
            };
            // VEX.256 shift-by-immediate: vpsll*/vpsrl*/vpsra* ymm<dest>, ymm<src>, imm8
            auto emit_vex_shift_imm = [&](int dest, int src, uint8_t rf, uint8_t sh) {
                emit_vex_prefix(0x01, src, 0x01);
                emit_byte(subop);
                emit_byte(0xC0 | (rf << 3) | dest);
                emit_byte(sh);
            };
            // VEX.256 3-operand: vpadd*/vpor ymm<dest>, ymm<nds>, ymm<src>
            auto emit_vex_3op = [&](uint8_t opcode, int dest, int nds, int src) {
                emit_vex_prefix(0x01, nds, 0x01);
                emit_byte(opcode);
                emit_byte(0xC0 | (dest << 3) | src);
            };
            // VEX.256.66.0F3A.W0 38 /r ib — vinserti128 ymm<dest>, ymm<nds>, xmm<src>, 1
            auto emit_vinserti128 = [&](int dest, int nds, int src) {
                emit_vex_prefix(0x03, nds, 0x01);
                emit_byte(0x38);
                emit_byte(0xC0 | (dest << 3) | src);
                emit_byte(0x01);
            };
            // VEX.256.66.0F3A.W0 39 /r ib — vextracti128 xmm<dest>, ymm<src>, 1
            // NOTE: for vextracti128 ModRM.reg encodes the SRC (ymm) and
            // ModRM.rm the DEST (xmm) — the reverse of vinserti128. vvvv
            // is reserved (1111b).
            auto emit_vextracti128 = [&](int dest, int src) {
                emit_vex_prefix(0x03, 0, 0x01);
                emit_byte(0x39);
                emit_byte(0xC0 | (src << 3) | dest);
                emit_byte(0x01);
            };
            // Pack two 64-bit halves into ymm<base>: bits[0:63] = lo,
            // bits[128:191] = hi (uses xmm<base> and xmm<base+1>).
            auto emit_pack_halves = [&](int base, int32_t offlo, int32_t offhi) {
                emit_load64(base, offlo);        // xmm<base>   = lo
                emit_load64(base + 1, offhi);    // xmm<base+1> = hi
                emit_vinserti128(base, base, base + 1);  // ymm<base> = {hi, lo}
            };
            // Store ymm0's low 128 (lo half) and extracted high 128 (hi half).
            auto emit_store_256 = [&](int32_t offlo, int32_t offhi) {
                emit_vextracti128(1, 0);   // xmm1 = bits[128:255] (hi half)
                emit_store64(1, offhi);
                emit_store64(0, offlo);
            };
            // ── SSE2 128-bit path — process one 64-bit half ────────────
            // CRITICAL: use 0xF2 (movsd, 64-bit) NOT 0xF3 (movss, 32-bit).
            // movss would load only the low 32 bits (lane 0) and zero lanes
            // 1-3, then the SSE2 shift would only shift lane 0, then movss
            // would store only lane 0 — corrupting lanes 1-3. We use 0xF2
            // here to be correct.
            auto emit_half_sse = [&](int32_t off1, int32_t offd) {
                if (inst.op == IROp::SIMD_SHL || inst.op == IROp::SIMD_USHR ||
                    inst.op == IROp::SIMD_SSHR) {
                    emit_load64(0, off1);
                    emit_shift_imm(0, reg_field, shift);
                    emit_store64(0, offd);
                } else if (inst.op == IROp::SIMD_USRA ||
                           inst.op == IROp::SIMD_SSRA) {
                    emit_load64(0, off1);
                    emit_shift_imm(0, reg_field, shift);
                    emit_load64(1, offd);
                    emit_padd(1, 0);            // xmm1 = Vd + shifted Vn
                    emit_store64(1, offd);
                } else if (is_rounding) {
                    // URSRA/SRSRA: Vd += round(Vn >> #shift).
                    // RShr(Vn, shift) = (Vn >> shift) + roundbit, where
                    // roundbit is the top discarded bit (bit shift-1 of Vn),
                    // i.e. 1 if set else 0. Isolate it per element with a
                    // logical shift + PSLL/PSRL round-trip (bit0 -> top ->
                    // bit0), which avoids any per-element mask constant.
                    emit_load64(0, off1);                     // xmm0 = Vn
                    emit_load64(2, off1);                     // xmm2 = Vn (copy)
                    emit_shift_imm(2, 2, shift - 1);          // xmm2 >> (shift-1)
                    emit_shift_imm(2, 6, static_cast<uint8_t>(esize * 8) - 1);  // bit0 -> top
                    emit_shift_imm(2, 2, static_cast<uint8_t>(esize * 8) - 1);  // top -> bit0
                    emit_shift_imm(0, reg_field, shift);      // xmm0 = Vn >> shift
                    emit_padd(0, 2);                          // xmm0 = shifted + roundbit
                    emit_load64(1, offd);                     // xmm1 = Vd
                    emit_padd(1, 0);                          // xmm1 = Vd + rounded
                    emit_store64(1, offd);
                } else {  // SIMD_SLI / SIMD_SRI
                    emit_load64(0, off1);
                    emit_shift_imm(0, reg_field, shift);
                    emit_load64(1, offd);
                    if (inst.op == IROp::SIMD_SLI) {
                        // Vd_keep = Vd & ((1<<shift)-1): shift left then right.
                        emit_shift_imm(1, 6, insert_shift);
                        emit_shift_imm(1, 2, insert_shift);
                    } else {
                        // Vd_keep = Vd & (top shift bits): shift right then left.
                        emit_shift_imm(1, 2, insert_shift);
                        emit_shift_imm(1, 6, insert_shift);
                    }
                    emit_por(1, 0);             // xmm1 = Vn_part | Vd_keep
                    emit_store64(1, offd);
                }
            };
            if (has_avx2() && q) {
                // ── AVX2 256-bit path (both halves in one instruction) ──
                if (inst.op == IROp::SIMD_SHL || inst.op == IROp::SIMD_USHR ||
                    inst.op == IROp::SIMD_SSHR) {
                    emit_pack_halves(0, off1lo, off1hi);       // ymm0 = Vn
                    emit_vex_shift_imm(0, 0, reg_field, shift);
                    emit_store_256(offdlo, offdhi);
                } else if (inst.op == IROp::SIMD_USRA ||
                           inst.op == IROp::SIMD_SSRA) {
                    emit_pack_halves(0, off1lo, off1hi);       // ymm0 = Vn
                    emit_vex_shift_imm(0, 0, reg_field, shift);
                    emit_pack_halves(2, offdlo, offdhi);       // ymm2 = Vd
                    emit_vex_3op(padd_op, 0, 2, 0);            // ymm0 = Vd + shifted
                    emit_store_256(offdlo, offdhi);
                } else if (is_rounding) {
                    emit_pack_halves(0, off1lo, off1hi);       // ymm0 = Vn
                    emit_pack_halves(1, off1lo, off1hi);       // ymm1 = Vn (copy)
                    emit_vex_shift_imm(1, 1, 2, shift - 1);    // ymm1 >> (shift-1)
                    emit_vex_shift_imm(1, 1, 6, static_cast<uint8_t>(esize * 8) - 1);  // bit0 -> top
                    emit_vex_shift_imm(1, 1, 2, static_cast<uint8_t>(esize * 8) - 1);  // top -> bit0
                    emit_vex_shift_imm(0, 0, reg_field, shift); // ymm0 = Vn >> shift
                    emit_vex_3op(padd_op, 0, 0, 1);            // ymm0 = shifted + roundbit
                    emit_pack_halves(2, offdlo, offdhi);       // ymm2 = Vd
                    emit_vex_3op(padd_op, 0, 2, 0);            // ymm0 = Vd + rounded
                    emit_store_256(offdlo, offdhi);
                } else {  // SIMD_SLI / SIMD_SRI
                    emit_pack_halves(0, off1lo, off1hi);       // ymm0 = Vn
                    emit_vex_shift_imm(0, 0, reg_field, shift);
                    emit_pack_halves(2, offdlo, offdhi);       // ymm2 = Vd
                    if (inst.op == IROp::SIMD_SLI) {
                        // Vd_keep = Vd & ((1<<shift)-1): shift left then right.
                        emit_vex_shift_imm(2, 2, 6, insert_shift);
                        emit_vex_shift_imm(2, 2, 2, insert_shift);
                    } else {
                        // Vd_keep = Vd & (top shift bits): shift right then left.
                        emit_vex_shift_imm(2, 2, 2, insert_shift);
                        emit_vex_shift_imm(2, 2, 6, insert_shift);
                    }
                    emit_vex_3op(0xEB, 0, 2, 0);               // ymm0 = parts|parts
                    emit_store_256(offdlo, offdhi);
                }
            } else {
                // ── SSE2 128-bit path (Q=1: both halves; Q=0: lo + zero hi) ──
                emit_half_sse(off1lo, offdlo);
                if (q) {
                    emit_half_sse(off1hi, offdhi);
                } else {
                    // Q=0: the IR contract zeros v_hi of the result.
                    emit_byte(0x66); emit_byte(0x0F); emit_byte(0xEF);
                    emit_byte(0xC0);                 // pxor xmm0, xmm0
                    emit_store64(0, offdhi);
                }
            }
            return true;
        }
        // ── 1.5.2-alpha: AES / PMULL native codegen ──────────────────
        // Uses AES-NI (aesenc/aesdec/aesimc/aesmc) and PCLMULQDQ
        // (pclmulqdq) when the host CPU supports them. Falls back to
        // CALL_INTERP on hosts without these extensions.
        //
        // The 128-bit V register is stored as v_lo (bits[63:0]) +
        // v_hi (bits[127:64]). We load both halves into XMM0 (low in
        // bits[63:0], high in bits[127:64]), perform the 128-bit op,
        // then store back.
        //
        // XMM register usage:
        //   XMM0 = state (src1) → result (dest)
        //   XMM1 = key (src2) for AES, or second operand for PMULL
        //
        // AES-NI instruction encodings (66 0F 38 DC/DE/DD/DF /r):
        //   AESE   = 66 0F 38 DC /r  (aesenc)
        //   AESD   = 66 0F 38 DE /r  (aesdec)
        //   AESMC  = 66 0F 38 DD /r  (aesimc — note: AESMC maps to aesimc,
        //                              AESIMC maps to aesmc; the x86 and
        //                              ARM names are swapped relative to
        //                              each other — see below)
        //   AESIMC = 66 0F 38 DF /r
        //
        // IMPORTANT: ARM and x86 AES instructions are NOT identical:
        //   ARM AESE  = AddRoundKey + SubBytes + ShiftRows
        //   x86 AESENC = SubBytes + ShiftRows + MixColumns + XOR roundkey
        // The x86 instruction includes MixColumns, which ARM's AESE does
        // NOT. ARM splits this into AESE (no MixColumns) + AESMC
        // (MixColumns). To emulate ARM AESE on x86, we would need to
        // use the AESENC instruction WITHOUT the MixColumns step, which
        // x86 doesn't expose directly.
        //
        // However, in practice, ARM code always pairs AESE+AESMC (the
        // ARM ARM shows them used together in every AES round). The
        // x86 AESENC instruction does both in one step. So we can't
        // directly map them 1:1.
        //
        // For correctness, we fall back to CALL_INTERP for AESE/AESD
        // (which uses the interpreter's software table-driven
        // implementation). We DO use AES-NI for AESMC/AESIMC via the
        // aesimc/aesmc x86 instructions... actually those also have
        // semantic differences.
        //
        // Given the semantic mismatch, the safest approach is to
        // emit CALL_INTERP for all AES ops. The native AES-NI path is
        // left as a future optimization that requires careful mapping
        // of ARM's split semantics to x86's combined semantics.
        //
        // For PMULL/PMULL2, the semantics ARE identical (carry-less
        // multiplication is the same on both architectures), so we
        // use PCLMULQDQ natively when available.
        case IROp::AES_CRYPTO: {
            uint8_t sub_op = static_cast<uint8_t>(inst.imm);
            // PMULL/PMULL2 — use PCLMULQDQ when available.
            if ((sub_op == 4 || sub_op == 5) && has_pclmulqdq()) {
                clobber_flags();
                flush_invalidate_host_regs((1u << RAX) | (1u << RCX) | (1u << RDX));
                int32_t off1lo = V_LO_OFF + static_cast<int>(inst.src1) * 8;
                int32_t off1hi = V_HI_OFF + static_cast<int>(inst.src1) * 8;
                int32_t off2lo = V_LO_OFF + static_cast<int>(inst.src2) * 8;
                int32_t off2hi = V_HI_OFF + static_cast<int>(inst.src2) * 8;
                int32_t offdlo = V_LO_OFF + static_cast<int>(inst.dest) * 8;
                int32_t offdhi = V_HI_OFF + static_cast<int>(inst.dest) * 8;
                // Load src1 into XMM0 (low 64 bits in bits[63:0]).
                // movsd xmm0, [rbx+off1lo]
                emit_byte(0xF3); emit_byte(0x0F); emit_byte(0x10);
                emit_modrm_disp(0, CPU_REG, off1lo);
                // Load src2 into XMM1 (low 64 bits in bits[63:0]).
                // movsd xmm1, [rbx+off2lo]
                emit_byte(0xF3); emit_byte(0x0F); emit_byte(0x10);
                emit_modrm_disp(1, CPU_REG, off2lo);
                if (sub_op == 4) {
                    // PMULL: multiply low 64 bits, produce 128-bit result.
                    // pclmulqdq xmm0, xmm1, 0x00  (imm=0x00 selects low×low)
                    // Encoding: 66 0F 3A 44 /r ib
                    emit_byte(0x66); emit_byte(0x0F); emit_byte(0x3A); emit_byte(0x44);
                    emit_byte(0xC1);  // modrm(3, xmm0, xmm1)
                    emit_byte(0x00);  // imm8 = 0x00 (low × low)
                } else {
                    // PMULL2: multiply high 64 bits, produce 128-bit result.
                    // Need to load the high halves into bits[63:0] of XMM0/XMM1
                    // because pclmulqdq operates on the low 64 bits of each
                    // operand (selected by the imm).
                    //
                    // Actually, pclmulqdq imm bits select which 64-bit half
                    // of each source to use:
                    //   imm[0] = 0: src1 low 64, 1: src1 high 64
                    //   imm[4] = 0: src2 low 64, 1: src2 high 64
                    // So for PMULL2 (high × high), imm = 0x11.
                    // We need the full 128-bit values in XMM0/XMM1.
                    //
                    // Reload with movdqa (128-bit load) instead of movsd.
                    // movdqa xmm0, [rbx+off1lo]  (requires 16-byte aligned;
                    //   our v_lo/v_hi are 8-byte apart, NOT 16-byte aligned)
                    // Use movdqu (unaligned) instead: F3 0F 6F /r
                    // But we need to load v_lo AND v_hi into one XMM.
                    // v_lo is at V_LO_OFF + src1*8, v_hi is at V_HI_OFF + src1*8.
                    // V_LO_OFF = 288, V_HI_OFF = 544. They're 256 bytes apart,
                    // NOT adjacent. So we can't load both with one movdqu.
                    //
                    // Instead: load v_lo into bits[63:0] of xmm0, v_hi into
                    // bits[127:64] via pinsrq.
                    // movsd  xmm0, [rbx+off1lo]   ; bits[63:0] = v_lo, bits[127:64] = 0
                    // pinsrq xmm0, [rbx+off1hi], 1 ; bits[127:64] = v_hi
                    // pinsrq = 66 0F 3A 22 /r ib
                    emit_byte(0xF3); emit_byte(0x0F); emit_byte(0x10);
                    emit_modrm_disp(0, CPU_REG, off1lo);
                    emit_byte(0x66); emit_byte(0x0F); emit_byte(0x3A); emit_byte(0x22);
                    emit_modrm_disp(0, CPU_REG, off1hi);
                    emit_byte(0x01);  // imm8 = 1 (high 64 bits)
                    emit_byte(0xF3); emit_byte(0x0F); emit_byte(0x10);
                    emit_modrm_disp(1, CPU_REG, off2lo);
                    emit_byte(0x66); emit_byte(0x0F); emit_byte(0x3A); emit_byte(0x22);
                    emit_modrm_disp(1, CPU_REG, off2hi);
                    emit_byte(0x01);
                    // pclmulqdq xmm0, xmm1, 0x11  (high × high)
                    emit_byte(0x66); emit_byte(0x0F); emit_byte(0x3A); emit_byte(0x44);
                    emit_byte(0xC1);
                    emit_byte(0x11);  // imm8 = 0x11 (high × high)
                }
                // Store the 128-bit result: bits[63:0] → v_lo, bits[127:64] → v_hi.
                // movsd [rbx+offdlo], xmm0
                emit_byte(0xF3); emit_byte(0x0F); emit_byte(0x11);
                emit_modrm_disp(0, CPU_REG, offdlo);
                // pextrq [rbx+offdhi], xmm0, 1  (extract bits[127:64])
                // pextrq = 66 0F 3A 16 /r ib
                emit_byte(0x66); emit_byte(0x0F); emit_byte(0x3A); emit_byte(0x16);
                emit_modrm_disp(0, CPU_REG, offdhi);
                emit_byte(0x01);  // imm8 = 1 (high 64 bits)
                return true;
            }
            // AESE/AESD/AESMC/AESIMC — fall back to CALL_INTERP.
            // (The ARM-vs-x86 semantic mismatch makes direct AES-NI
            // mapping incorrect. The interpreter's table-driven
            // implementation is correct.)
            emit_call_interp(inst.arm_pc, false);
            return true;
        }
        default:
            return false;  // not handled — caller falls through
    }
}
} // namespace arm64emu
