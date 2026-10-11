// SIMD pairwise min/max, absolute difference, widening/narrowing arithmetic and by-element multiply.
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include "System/Core/Emulator.hpp"
#include "FrostJIT/IR/IR.hpp"
#include "FrostJIT/CodeGen/Native/NativeSIMD.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace arm64emu {

// ── SIMD_PAIRMIN pshufb even/odd deinterleave masks ─────────────────────
// Pairwise max/min of a vector = lane-wise max/min of its EVEN and ODD
// elements (out[i] = max/min(a[2i], a[2i+1])). Each source register is
// deinterleaved with pshufb into even/odd registers, the pmin/pmax op
// fuses them, and for Q=1 the two halves punpcklqdq together. `out_bytes`
// is 8 for Q=1 (elems/2 results) and 4 for Q=0 (elems/2 results of the
// 8-byte operand); the trailing mask bytes are 0x80 (pshufb -> 0), so the
// Q=0 low 64 bits come out {results, 0,0,0,0} exactly like the interp.
// esize = element size in bytes (1, 2, 4).
static void build_pair_masks(int esize, bool q,
                             uint8_t maskEven[16], uint8_t maskOdd[16]) {
    for (int k = 0; k < 16; k++) { maskEven[k] = 0x80; maskOdd[k] = 0x80; }
    const int out_bytes = q ? 8 : 4;
    const int results = out_bytes / esize;
    for (int oi = 0; oi < results; oi++) {
        for (int j = 0; j < esize; j++) {
            const int o = oi * esize + j;
            maskEven[o] = static_cast<uint8_t>((2 * oi) * esize + j);
            maskOdd[o]  = static_cast<uint8_t>((2 * oi + 1) * esize + j);
        }
    }
}
bool FrostJIT::compile_simd_widening_(const IRInst& inst) {
    switch (inst.op) {
        case IROp::SIMD_PAIRMIN: {
            if (vec_cache_active_) {
                emit_call_interp(inst.arm_pc, false);
                return true;
            }
            const SimdBinopParams pp = inst.pairmin_params();
            const int esize = static_cast<int>(pp.esize);
            const bool q = pp.q;
            const int subop = static_cast<int>(pp.subop);
            if (esize != 1 && esize != 2 && esize != 4) {
                emit_call_interp(inst.arm_pc, false);
                return true;
            }
            const bool is_min = (subop == 1 || subop == 3);
            const bool is_uns = (subop == 2 || subop == 3);
            uint8_t op_byte = 0;
            bool need38 = false;
            bool supported = true;
            if (is_uns) {
                if (esize == 1) {
                    op_byte = is_min ? 0xDA : 0xDE;   // pminub/pmaxub (SSE2)
                } else {
                    if (!has_sse41()) { supported = false; }
                    else {
                        need38 = true;
                        op_byte = is_min
                            ? (esize == 2 ? 0x3A : 0x3B)   // pminuw/pminud
                            : (esize == 2 ? 0x3E : 0x3F);  // pmaxuw/pmaxud
                    }
                }
            } else {  // signed
                if (esize == 2) {
                    op_byte = is_min ? 0xEA : 0xEE;   // pminsw/pmaxsw (SSE2)
                } else {
                    if (!has_sse41()) { supported = false; }
                    else {
                        need38 = true;
                        op_byte = is_min
                            ? (esize == 1 ? 0x38 : 0x39)   // pminsb/pminsd
                            : (esize == 1 ? 0x3C : 0x3D);  // pmaxsb/pmaxsd
                    }
                }
            }
            if (!supported) {
                emit_call_interp(inst.arm_pc, false);
                return true;
            }
            uint8_t maskEven[16], maskOdd[16];
            build_pair_masks(esize, q, maskEven, maskOdd);
            clobber_flags();
            load_vec(0, static_cast<int>(inst.src1));   // X0 = A (Vn)
            load_vec(1, static_cast<int>(inst.src2));   // X1 = B (Vm)
            emit_mask16(2, maskEven);
            emit_mask16(3, maskOdd);
            movdqa(4, 0);            // X4 = A copy
            sse2_38(0x00, 4, 2);     // X4 = even(A)
            sse2_38(0x00, 0, 3);     // X0 = odd(A)
            if (need38) sse2_38(op_byte, 4, 0);
            else        sse2_op(op_byte, 4, 0);   // X4 = minmax(even(A), odd(A))
            movdqa(5, 1);            // X5 = B copy
            sse2_38(0x00, 5, 2);     // X5 = even(B)
            sse2_38(0x00, 1, 3);     // X1 = odd(B)
            if (need38) sse2_38(op_byte, 5, 1);
            else        sse2_op(op_byte, 5, 1);   // X5 = minmax(even(B), odd(B))
            // Pack both halves: a pairwise op ALWAYS reads both sources.
            // Q=1: punpcklqdq gives [8 result bytes of Vn, 8 of Vm].
            // Q=0: each XMM holds its out_bytes=4 result bytes in the low
            // bytes (high bytes 0x80'd by the masks); pslldq the Vm half
            // up by 4 bytes and OR it in so the low 8 bytes come out
            // {pairwise(Vn), pairwise(Vm)} — real ARM semantics, matching
            // the interp. (The old Q=0 path zeroed the Vm half, breaking
            // Q=0 uses with differing srcs; a plain punpcklqdq for Q=0
            // would push Vm's bytes into v_hi, which store_vec then
            // discards for Q=0.)
            if (q) {
                sse2_op(0x6C, 4, 5);     // punpcklqdq X4 = [low64(X4), low64(X5)]
            } else {
                sse2_imm(0x73, 5, 7, 4); // pslldq X5, 4 (up to byte 4)
                sse2_op(0xEB, 4, 5);     // por  X4, X5
            }
            movdqa(0, 4);
            store_vec(0, static_cast<int>(inst.dest), q);
            return true;
        }
        // ── SIMD ABD (SABD/UABD — same-width absolute difference) ────
        // src1=rn, src2=rm; width=esize (1/2/4); imm=subop (0=S, 1=U);
        // flags_op=Q. r = |a - b| per lane. SSE2-only via the xor-shift
        // abs trick: t = a-b; m = t >>s (bits-1); r = (t ^ m) - m.
        case IROp::SIMD_ABD: {
            if (vec_cache_active_) { emit_call_interp(inst.arm_pc, false); return true; }
            const SimdSubopParams sp = inst.abd_params();
            const int esize = static_cast<int>(sp.esize);
            const bool q = sp.q;
            const bool is_unsigned = sp.subop != 0;
            if (((esize == 2 && is_unsigned) || esize == 4) && !has_sse41()) {
                emit_call_interp(inst.arm_pc, false); return true;
            }
            clobber_flags();
            load_vec(0, static_cast<int>(inst.src1));
            load_vec(1, static_cast<int>(inst.src2));
            if (esize == 1) {
                // Byte shifts operate on 16-bit lanes, so the xor/shift
                // absolute-value idiom leaks across adjacent bytes. Compute
                // max-min instead, with signed max/min requiring SSE4.1.
                if (!is_unsigned && !has_sse41()) {
                    emit_call_interp(inst.arm_pc, false); return true;
                }
                movdqa(2, 0);
                movdqa(3, 0);
                if (is_unsigned) {
                    sse2_op(0xDE, 2, 1);             // pmaxub
                    sse2_op(0xDA, 3, 1);             // pminub
                } else {
                    sse2_38(0x3C, 2, 1);             // pmaxsb
                    sse2_38(0x38, 3, 1);             // pminsb
                }
                sse2_op(0xF8, 2, 3);                 // psubb: max-min
            } else {
                movdqa(2, 0);
                movdqa(3, 0);
                if (esize == 2) {
                    if (is_unsigned) {
                        sse2_38(0x3E, 2, 1);         // pmaxuw
                        sse2_38(0x3A, 3, 1);         // pminuw
                    } else {
                        sse2_op(0xEE, 2, 1);         // pmaxsw
                        sse2_op(0xEA, 3, 1);         // pminsw
                    }
                    sse2_op(0xF9, 2, 3);             // psubw: max-min
                } else if (esize == 4) {
                    if (is_unsigned) {
                        sse2_38(0x3F, 2, 1);         // pmaxud
                        sse2_38(0x3B, 3, 1);         // pminud
                    } else {
                        sse2_38(0x3D, 2, 1);         // pmaxsd
                        sse2_38(0x39, 3, 1);         // pminsd
                    }
                    sse2_op(0xFA, 2, 3);             // psubd: max-min
                } else {
                    emit_call_interp(inst.arm_pc, false); return true;
                }
            }
            movdqa(0, 2);
            store_vec(0, static_cast<int>(inst.dest), q);
            return true;
        }
        // ── SIMD ABDL (SABDL/UABDL/SABAL/UABAL — widening abs diff) ──
        // width = SOURCE esize; imm = subop (0=SABDL,1=UABDL,2=SABAL,
        // 3=UABAL); flags_op = Q (source-half select). The absolute
        // difference is computed at SOURCE width then zero/sign-extended
        // to double width (|a|-|b| pattern is extension-safe because the
        // result is non-negative). Accumulate variants add Vd.
        case IROp::SIMD_ABDL: {
            // 32-bit max/min and unsigned 16-bit max/min below require SSE4.1.
            if (vec_cache_active_ || !has_sse41()) {
                emit_call_interp(inst.arm_pc, false); return true;
            }
            const SimdSubopParams sp = inst.abdl_params();
            const int esize = static_cast<int>(sp.esize);
            const bool q = sp.q;
            const int subop = static_cast<int>(sp.subop);
            if (esize != 1 && esize != 2 && esize != 4) {
                emit_call_interp(inst.arm_pc, false); return true;
            }
            clobber_flags();
            load_vec(0, static_cast<int>(inst.src1));
            load_vec(1, static_cast<int>(inst.src2));
            if (q) {
                // Q=1 ("2" variants): select the HIGH source halves by
                // shifting them down into the low half.
                // NOTE: psrldq is 0F 73 /3 — /7 is PSLLDQ!
                sse2_imm(0x73, 0, 3, 8);   // psrldq X0, 8
                sse2_imm(0x73, 1, 3, 8);   // psrldq X1, 8
            }
            // Absolute difference at source width. Use max-min so subtraction
            // cannot wrap before the absolute value is taken.
            if (esize == 1) {
                movdqa(2, 0);
                movdqa(3, 0);
                if (subop & 1) { // unsigned UABDL/UABAL
                    sse2_op(0xDE, 2, 1);        // pmaxub
                    sse2_op(0xDA, 3, 1);        // pminub
                } else {         // signed SABDL/SABAL
                    sse2_38(0x3C, 2, 1);         // pmaxsb
                    sse2_38(0x38, 3, 1);         // pminsb
                }
                sse2_op(0xF8, 2, 3);             // psubb: max - min
            } else {
                movdqa(2, 0);
                movdqa(3, 0);
                if (esize == 2) {
                    if (subop & 1) {               // unsigned UABDL/UABAL
                        sse2_38(0x3E, 2, 1);       // pmaxuw
                        sse2_38(0x3A, 3, 1);       // pminuw
                    } else {                       // signed SABDL/SABAL
                        sse2_op(0xEE, 2, 1);       // pmaxsw
                        sse2_op(0xEA, 3, 1);       // pminsw
                    }
                    sse2_op(0xF9, 2, 3);           // psubw: max-min
                } else {
                    if (subop & 1) {               // unsigned UABDL/UABAL
                        sse2_38(0x3F, 2, 1);       // pmaxud
                        sse2_38(0x3B, 3, 1);       // pminud
                    } else {                       // signed SABDL/SABAL
                        sse2_38(0x3D, 2, 1);       // pmaxsd
                        sse2_38(0x39, 3, 1);       // pminsd
                    }
                    sse2_op(0xFA, 2, 3);           // psubd: max-min
                }
            }
            // Widen low half → full 128 (pmovzx: 20/30/31 bw/wd/dq...
            // pmovzxbw=0x30, pmovzxwd=0x33, pmovzxdq=0x35)
            if (esize == 1)      sse2_38(0x30, 2, 2);
            else if (esize == 2) sse2_38(0x33, 2, 2);
            else                 sse2_38(0x35, 2, 2);
            if (subop >= 2) {          // SABAL/UABAL: accumulate into Vd
                load_vec(3, static_cast<int>(inst.dest));
                sse2_op(esize == 1 ? 0xFD : esize == 2 ? 0xFE : 0xD4, 2, 3);
            }
            movdqa(0, 2);
            store_vec(0, static_cast<int>(inst.dest), true);  // dest always 128-bit
            return true;
        }
        // ── SIMD ADDW (SADDW/UADDW/SSUBW/USUBW — wide op + narrow src) ─
        // width = SOURCE esize (2/4); imm = subop (bit13=sub, bit29=uns);
        // flags_op = Q (source-half select). Dest/Vn always full 128.
        case IROp::SIMD_ADDW: {
            if (vec_cache_active_ || !has_sse41()) {
                emit_call_interp(inst.arm_pc, false); return true;
            }
            const SimdSubopParams sp = inst.addw_params();
            const int esize = static_cast<int>(sp.esize);   // src esize
            const bool q = sp.q;
            const int subop = static_cast<int>(sp.subop);
            const bool is_sub = (subop & 2) != 0;
            const bool is_uns = (subop & 1) != 0;
            clobber_flags();
            load_vec(0, static_cast<int>(inst.src1));   // wide Vn
            load_vec(1, static_cast<int>(inst.src2));   // narrow Vm
            if (q) sse2_imm(0x73, 1, 3, 8);             // psrldq (/3!): Q=1 takes high half
            // Extend narrow → wide lanes
            if (esize == 2) sse2_38(is_uns ? 0x33 : 0x23, 1, 1); // pmovzx/pmovsxwd
            else            sse2_38(is_uns ? 0x35 : 0x25, 1, 1); // pmovzx/pmovsxdq
            // Wide add/sub (dest width = 2*esize: 4→dword padd/psub, 8→q)
            if (esize == 2)
                sse2_op(is_sub ? 0xFA : 0xFE, 0, 1);    // psubd/paddd
            else
                sse2_op(is_sub ? 0xFB : 0xD4, 0, 1);    // psubq/paddq
            store_vec(0, static_cast<int>(inst.dest), true);
            return true;
        }
        // ── SIMD ADDHN (ADDHN/RADDHN/SUBHN/RSUBHN — narrow high half) ──
        // width = INPUT esize (2/4/8); imm = subop (0=ADDHN, 1=RADDHN,
        // 2=SUBHN, 3=RSUBHN); flags_op = Q (DEST-half select). Sources
        // always full 128-bit.
        case IROp::SIMD_ADDHN: {
            if (vec_cache_active_) { emit_call_interp(inst.arm_pc, false); return true; }
            const SimdSubopParams sp = inst.addhn_params();
            const int esize_in = static_cast<int>(sp.esize);
            const bool q = sp.q;
            const int subop = static_cast<int>(sp.subop);
            const bool is_sub = (subop & 2) != 0;
            const bool round = (subop & 1) != 0;
            if (esize_in < 2 || esize_in > 8) {
                emit_call_interp(inst.arm_pc, false); return true;
            }
            clobber_flags();
            load_vec(0, static_cast<int>(inst.src1));
            load_vec(1, static_cast<int>(inst.src2));
            // Combine sources FIRST (add or subtract), then apply the
            // rounding constant before the high-half shift.
            if (is_sub)
                sse2_op(esize_in == 2 ? 0xF9 : esize_in == 4 ? 0xFA : 0xFB,
                        0, 1);                            // psub*
            else
                sse2_op(esize_in == 2 ? 0xFD : esize_in == 4 ? 0xFE : 0xD4,
                        0, 1);                            // padd*
            if (round) {
                uint8_t rc[16] = {0};
                uint64_t c = 1ULL << (esize_in * 4 - 1);  // 2^(esize_out*8-1)
                for (int i = 0; i < 16; i += esize_in)
                    memcpy(rc + i, &c, esize_in < 8 ? esize_in : 8);
                emit_mask16(2, rc);
                sse2_op(esize_in == 2 ? 0xFD : esize_in == 4 ? 0xFE : 0xD4,
                        0, 2);                            // padd*/paddq
            }
            // Shift right to the high half (psrl* = /2)
            sse2_imm(esize_in == 2 ? 0x71 : esize_in == 4 ? 0x72 : 0x73,
                     0, 2, static_cast<uint8_t>(esize_in * 4)); // psrl* /2^half
            // Logical narrow (XTN-style)
            if (esize_in == 2) {
                emit_mask(1, 0x00FF00FF00FF00FFULL);
                sse2_op(0xDB, 0, 1);                      // pand
                sse2_op(0x67, 0, 0);                      // packuswb
            } else if (esize_in == 4) {
                emit_mask(1, 0x0000FFFF0000FFFFULL);
                sse2_op(0xDB, 0, 1);
                sse2_imm(0x72, 0, 6, 16);                 // pslld 16
                sse2_imm(0x72, 0, 4, 16);                 // psrad 16
                sse2_op(0x6B, 0, 0);                      // packssdw
            } else {
                sse2_op(0x70, 0, 0);                      // pshufd
                emit_byte(0x08);
            }
            if (q) {
                store_hi(0, V_HI_OFF + static_cast<int>(inst.dest) * 8);
            } else {
                store_lo(0, V_LO_OFF + static_cast<int>(inst.dest) * 8);
                fp_zero_hi(static_cast<int>(inst.dest));
            }
            return true;
        }
        // Saturating narrowing shifts use the reference handler. Native
        // lowering must widen before rounding, preserve destination halves,
        // and report saturation through FPSR.QC before it can be reinstated.
        case IROp::SIMD_SHRN_SAT:
            emit_call_interp(inst.arm_pc, false);
            return true;
        // ── SIMD MUL_ELEM (integer vector x indexed element) ──────────
        // width = SOURCE esize; imm = subop | (idx << 8); flags_op = Q.
        // subop: 0=MUL 1=MLA 2=MLS 3=SMULL 4=UMULL 5=SMLAL 6=UMLAL
        //        7=SMLSL 8=UMLSL 9=SQDMULH 10=SQRDMULH 11=SQDMULL
        // SQDMULH/SQRDMULH/SQDMULL stay on the interpreter for now
        // (saturating doubling needs a wider sequence).
        case IROp::SIMD_MUL_ELEM: {
            const SimdMulElemParams mp = inst.mul_elem_params();
            const int esize = static_cast<int>(mp.esize);
            const int subop = static_cast<int>(mp.subop);
            const int idx = static_cast<int>(mp.lane);
            const bool q = mp.q;
            const bool widen = (subop >= 3 && subop <= 8) || subop == 11;
            if (vec_cache_active_ || !has_sse41() || esize == 1 ||
                esize > 4 || subop >= 9) {
                emit_call_interp(inst.arm_pc, false); return true;
            }
            clobber_flags();
            load_vec(0, static_cast<int>(inst.src1));   // Vn
            // Widening multiply forms use Q to select the source half
            // (SMULL2/UMLAL2/etc. read Vn[upper lanes]). The widening
            // unpack below consumes only the low 64 bits, so move the upper
            // source half down before extending it.
            if (widen && q)
                sse2_imm(0x73, 0, 3, 8);                // psrldq xmm0, 8
            load_vec(1, static_cast<int>(inst.src2));   // Vm
            // Broadcast Vm[idx] to all lanes of X1
            if (esize == 2) {
                if (idx < 4) {
                    sse_f2(0x70, 1, 1);                   // pshuflw
                    emit_byte(static_cast<uint8_t>(idx * 0x55));
                    sse2_op(0x70, 1, 1);                // pshufd spread
                    emit_byte(0x00);
                } else {
                    sse2_f3(0x70, 1, 1);                 // pshufhw
                    emit_byte(static_cast<uint8_t>((idx - 4) * 0x55));
                    sse2_op(0x70, 1, 1);                // pshufd: high→both
                    emit_byte(0xEE);
                }
            } else {
                sse2_op(0x70, 1, 1);                    // pshufd (.s)
                emit_byte(static_cast<uint8_t>(idx * 0x55));
            }
            // Extend both operands to the multiply width
            const bool uns = (subop == 4 || subop == 6 || subop == 8);
            if (widen) {
                // SMULL/UMULL/SMLAL/UMLAL/SMLSL/UMLSL: widen to 2x
                if (esize == 2) {
                    sse2_38(uns ? 0x33 : 0x23, 0, 0);   // pmovzx/pmovsxwd
                    sse2_38(uns ? 0x33 : 0x23, 1, 1);
                } else {
                    sse2_38(uns ? 0x35 : 0x25, 0, 0);   // pmovzx/pmovsxdq
                    sse2_38(uns ? 0x35 : 0x25, 1, 1);
                }
            }
            // Multiply
            if (esize == 2 && !widen)
                sse2_op(0xD5, 0, 1);                    // pmullw (exact low 16)
            else if (widen && esize == 4)
                if (uns) sse2_op(0xF4, 0, 1);           // pmuludq (unsigned .s→.d)
                else     sse2_38(0x28, 0, 1);           // pmuldq (signed low 64)
            else
                sse2_38(0x40, 0, 1);                    // pmulld (.s MUL/MLA/MLS + widened .h)
            // Accumulate / subtract. NOTE: subtract forms (MLS/SMLSL/
            // UMLSL) compute acc - product, so the SUB must run as
            // X2 = X2 - X0 (acc first), not X0 = X0 - X2.
            if (subop == 1 || subop == 2 || (subop >= 5 && subop <= 8)) {
                const bool w_sub = (subop == 2 || subop == 7 || subop == 8);
                const int aw = widen ? esize * 2 : esize;
                const int sub_add = aw == 2 ? (w_sub ? 0xF9 : 0xFD) :
                                    aw == 4 ? (w_sub ? 0xFA : 0xFE) : (w_sub ? 0xFB : 0xD4);
                load_vec(2, static_cast<int>(inst.dest));
                if (w_sub) {
                    sse2_op(sub_add, 2, 0);
                    movdqa(0, 2);
                } else {
                    sse2_op(sub_add, 0, 2);
                }
            }
            store_vec(0, static_cast<int>(inst.dest), widen ? true : q);
            return true;
        }
        default:
            return false;
    }
}
}  // namespace arm64emu
