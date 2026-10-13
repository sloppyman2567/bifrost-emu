// SIMD element ops: two-register misc, conversions, ADDP, narrowing, TBL/TBX, INS, ZIP/UZP/TRN.
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include "System/Core/Emulator.hpp"
#include "FrostJIT/IR/IR.hpp"
#include "FrostJIT/CodeGen/Native/NativeSIMD.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace arm64emu {
// ── SIMD_PERMUTE pshufb control-mask builder ────────────────────────────
// Builds the two 16-byte pshufb control masks for a ZIP/UZP/TRN permutation.
// Mirrors the interpreter's permute-pairs block in Dispatch.cpp exactly:
// source A occupies bytes 0..len, source B logically bytes len..2len, and
// each output byte `o` selects one source byte. maskA/maskB are the pshufb
// controls for A and B respectively; bytes not sourced from a register get
// 0x80 (pshufb -> 0), so the two masked results POR together exactly.
// opc6 = bits[15:10] of the encoding (0x06 UZP1, 0x0A TRN1, 0x0E ZIP1,
// 0x16 UZP2, 0x1A TRN2, 0x1E ZIP2); esize = element size in bytes; q =
// 128-bit (Q=1) vs 64-bit (Q=0, only the first 8 output bytes used).
static void build_permute_masks(uint8_t opc6, int esize, bool q,
                                uint8_t maskA[16], uint8_t maskB[16]) {
    for (int k = 0; k < 16; k++) { maskA[k] = 0x80; maskB[k] = 0x80; }
    const bool part = (opc6 == 0x16 || opc6 == 0x1A || opc6 == 0x1E);
    const bool is_zip = (opc6 == 0x0E || opc6 == 0x1E);
    const bool is_uzp = (opc6 == 0x06 || opc6 == 0x16);
    int pairs = (q ? 16 : 8) / (esize * 2);
    if (pairs < 1) pairs = 1;
    const int total = pairs * 2;
    const int out_bytes = q ? 16 : 8;
    for (int oi = 0; oi < total; oi++) {
        int src_byte = 0;
        bool from_b = false;
        if (is_zip) {
            // out[2i] = A[base+i], out[2i+1] = B[base+i], base=part*pairs*esize
            src_byte = part * pairs * esize + (oi >> 1) * esize;
            from_b = (oi & 1) != 0;
        } else if (is_uzp) {
            // out[0..half) = A even/odd elements, out[half..) = B's
            const int half = total >> 1;
            const int lane = 2 * ((oi < half) ? oi : (oi - half)) + (part ? 1 : 0);
            src_byte = lane * esize;
            from_b = oi >= half;
        } else {
            // TRN: out[2i] = A[2i+part], out[2i+1] = B[2i+part]
            src_byte = (2 * (oi >> 1) + (part ? 1 : 0)) * esize;
            from_b = (oi & 1) != 0;
        }
        for (int j = 0; j < esize; j++) {
            const int out_byte = oi * esize + j;
            if (out_byte >= out_bytes) break;
            (from_b ? maskB : maskA)[out_byte] =
                static_cast<uint8_t>(src_byte + j);
        }
    }
}
bool FrostJIT::compile_simd_elements_(const IRInst& inst) {
    switch (inst.op) {
        case IROp::SIMD_2REG: {
            if (vec_cache_active_) { emit_call_interp(inst.arm_pc, false); return true; }
            const SimdSubopParams sp = inst.p2reg_params();
            const int esize = static_cast<int>(sp.esize);
            const int subop = static_cast<int>(sp.subop);
            if ((subop == 3 || subop == 4) &&
                esize != 1 && esize != 2 && esize != 4 && esize != 8) {
                emit_call_interp(inst.arm_pc, false);
                return true;
            }
            clobber_flags();
            load_vec(0, static_cast<int>(inst.src1));
            switch (subop) {
                case 0: {  // CNT — per-byte popcount (Muła SWAR, 16-bit lanes)
                    emit_mask(2, 0x5555555555555555ULL);
                    emit_mask(3, 0x3333333333333333ULL);
                    emit_mask(4, 0x0F0F0F0F0F0F0F0FULL);
                    movdqa(1, 0);
                    sse2_imm(0x71, 0, 2, 1);   // psrlw xmm0, 1
                    sse2_op(0xDB, 0, 2);       // pand xmm0, 0x5555
                    sse2_op(0xF8, 1, 0);       // psubb xmm1, xmm0 (2-bit popcounts)
                    movdqa(0, 1);
                    sse2_op(0xDB, 1, 3);       // pand xmm1, 0x3333
                    sse2_imm(0x71, 0, 2, 2);   // psrlw xmm0, 2
                    sse2_op(0xDB, 0, 3);       // pand xmm0, 0x3333
                    sse2_op(0xFC, 1, 0);       // paddb xmm1, xmm0 (4-bit popcounts)
                    movdqa(0, 1);
                    sse2_imm(0x71, 0, 2, 4);   // psrlw xmm0, 4
                    sse2_op(0xFC, 1, 0);       // paddb xmm1, xmm0
                    sse2_op(0xDB, 1, 4);       // pand xmm1, 0x0F0F
                    movdqa(0, 1);
                    break;
                }
                case 1: {  // NOT — byte-wise invert
                    sse2_op(0x74, 1, 1);       // pcmpeqb xmm1, xmm1 (all ones)
                    sse2_op(0xEF, 0, 1);       // pxor xmm0, xmm1
                    break;
                }
                case 2: {  // RBIT — reverse bits within each byte (3 stages)
                    emit_mask(2, 0xAAAAAAAAAAAAAAAAULL);
                    emit_mask(3, 0x5555555555555555ULL);
                    emit_mask(4, 0xCCCCCCCCCCCCCCCCULL);
                    emit_mask(5, 0x3333333333333333ULL);
                    emit_mask(6, 0xF0F0F0F0F0F0F0F0ULL);
                    emit_mask(7, 0x0F0F0F0F0F0F0F0FULL);
                    // stage 1: (x<<1)&0xAAAA | (x>>1)&0x5555
                    movdqa(1, 0);
                    sse2_imm(0x71, 1, 6, 1);   // psllw xmm1, 1
                    sse2_op(0xDB, 1, 2);       // pand 0xAAAA
                    sse2_imm(0x71, 0, 2, 1);   // psrlw xmm0, 1
                    sse2_op(0xDB, 0, 3);       // pand 0x5555
                    sse2_op(0xEB, 0, 1);       // por
                    // stage 2: (x<<2)&0xCCCC | (x>>2)&0x3333
                    movdqa(1, 0);
                    sse2_imm(0x71, 1, 6, 2);   // psllw xmm1, 2
                    sse2_op(0xDB, 1, 4);       // pand 0xCCCC
                    sse2_imm(0x71, 0, 2, 2);   // psrlw xmm0, 2
                    sse2_op(0xDB, 0, 5);       // pand 0x3333
                    sse2_op(0xEB, 0, 1);       // por
                    // stage 3: (x<<4)&0xF0F0 | (x>>4)&0x0F0F
                    movdqa(1, 0);
                    sse2_imm(0x71, 1, 6, 4);   // psllw xmm1, 4
                    sse2_op(0xDB, 1, 6);       // pand 0xF0F0
                    sse2_imm(0x71, 0, 2, 4);   // psrlw xmm0, 4
                    sse2_op(0xDB, 0, 7);       // pand 0x0F0F
                    sse2_op(0xEB, 0, 1);       // por
                    break;
                }
                case 3: {  // ABS — lane-wise absolute value
                    if (esize == 8) {
                        movdqa(1, 0);
                        sse2_imm(0x72, 1, 4, 31);  // psrad xmm1, 31
                        sse2_op(0x70, 1, 1);       // pshufd xmm1, xmm1, 0xF5
                        emit_byte(0xF5);           // broadcast dword1/3 → 64-bit signs
                        sse2_op(0xEF, 0, 1);       // pxor xmm0, xmm1
                        sse2_op(0xFB, 0, 1);       // psubq xmm0, xmm1  ((x^s)-s)
                    } else {
                        int cmp = (esize == 1) ? 0x64 : (esize == 2) ? 0x65 : 0x66;
                        int sub = (esize == 1) ? 0xF8 : (esize == 2) ? 0xF9 : 0xFA;
                        sse2_op(0xEF, 1, 1);       // pxor xmm1, xmm1 (zero)
                        sse2_op(cmp, 1, 0);        // xmm1 = (xmm1 > xmm0) = sign mask
                        movdqa(2, 0);
                        sse2_op(0xEF, 2, 1);       // xmm2 = x ^ sign
                        sse2_op(sub, 2, 1);        // xmm2 = (x ^ sign) - sign
                        movdqa(0, 2);
                    }
                    break;
                }
                default: {  // NEG — lane-wise negate (0 - x)
                    int sub = (esize == 1) ? 0xF8 : (esize == 2) ? 0xF9
                           : (esize == 4) ? 0xFA : 0xFB;
                    sse2_op(0xEF, 1, 1);           // pxor xmm1, xmm1
                    sse2_op(sub, 1, 0);            // xmm1 = 0 - x
                    movdqa(0, 1);
                    break;
                }
            }
            store_vec(0, static_cast<int>(inst.dest), sp.q);
            return true;
        }
        // ── SIMD CVTF (SCVTF/UCVTF/FCVTZS/FCVTZU, 32-bit lanes) ──────
        // imm = subop (0=SCVTF s32→f32, 1=UCVTF u32→f32, 2=FCVTZS f32→s32,
        // 3=FCVTZU f32→u32). width is always 4. flags_op = Q.
        case IROp::SIMD_CVTF: {
            if (vec_cache_active_) { emit_call_interp(inst.arm_pc, false); return true; }
            const SimdSubopParams sp = inst.cvtf_params();
            const int subop = static_cast<int>(sp.subop);
            clobber_flags();
            load_vec(0, static_cast<int>(inst.src1));
            switch (subop) {
                case 0:  // SCVTF
                    sse_fp(0x5B, 0, 0);           // cvtdq2ps xmm0, xmm0
                    break;
                case 1: {  // UCVTF — (float)(x>>1)*2 + (float)(x&1)
                    emit_mask(1, 0x0000000100000001ULL);
                    movdqa(2, 0);
                    sse2_imm(0x72, 2, 2, 1);      // psrld xmm2, 1
                    sse_fp(0x5B, 2, 2);           // cvtdq2ps xmm2, xmm2
                    sse_fp(0x58, 2, 2);           // addps xmm2, xmm2
                    sse2_op(0xDB, 0, 1);          // pand xmm0, 1
                    sse_fp(0x5B, 0, 0);           // cvtdq2ps xmm0, xmm0
                    sse_fp(0x58, 2, 0);           // addps xmm2, xmm0
                    movdqa(0, 2);
                    break;
                }
                case 2: {  // FCVTZS — truncate; NaN → 0; ±inf and range saturate
                    // FPToFixed (fp_to_signed_sat in the interpreter): x >= 2^31
                    // and +inf → INT32_MAX, x < -2^31 and -inf → INT32_MIN.
                    // cvttps2dq alone returns the 0x80000000 indefinite value
                    // for ANY out-of-range input, so pre-clamp first. The
                    // not-NaN mask comes from the ORIGINAL x, hence the save
                    // in xmm12.
                    emit_mask(5, 0x7FFFFFFF7FFFFFFFULL);
                    movdqa(3, 0);
                    sse_fp(0xC2, 3, 0); emit_byte(0);  // cmpps xmm3, xmm0, 0 (x==x: not NaN)
                    movdqa(12, 0);                   // save original x (saturation test)
                    emit_mask(1, 0xCF000000CF000000ULL);  // -2147483648.0f (exact)
                    sse_fp(0x5F, 0, 1);              // maxps xmm0, lo
                    emit_mask(1, 0x4EFFFFFF4EFFFFFFULL);  // 2147483520.0f (top float < 2^31)
                    sse_fp(0x5D, 0, 1);              // minps xmm0, hi
                    sse2_f3(0x5B, 0, 0);             // cvttps2dq xmm0
                    // Exact top edge: x == 2^31 clamps to 2147483520 above,
                    // but interp saturates it to INT32_MAX — OR the mask in.
                    emit_mask(2, 0x4F0000004F000000ULL);  // 2147483648.0f
                    sse_fp(0xC2, 12, 2); emit_byte(5); // cmpps xmm12, 2^31, 5 (>=)
                    sse2_op(0xDB, 12, 5);            // xmm12 &= 0x7FFFFFFF → INT32_MAX/0
                    sse2_op(0xEB, 0, 12);            // xmm0 |= saturate
                    sse2_op(0xDB, 0, 3);             // xmm0 &= not-NaN
                    break;
                }
                default: {  // FCVTZU — truncate; NaN and negatives → 0; saturate
                    // x >= 2^32 and +inf saturate to UINT32_MAX (FPToFixed,
                    // fp_to_unsigned_sat in the interpreter). The sub+OR
                    // hi-path alone yields 0x80000000 there (cvtt of x-2^31
                    // is indefinite), so OR the saturation mask in. The
                    // not-NaN mask (from the ORIGINAL x) is applied last.
                    emit_mask(8, 0x4F0000004F000000ULL);  // 2^31
                    emit_mask(9, 0x8000000080000000ULL);  // sign bit
                    movdqa(3, 0);
                    sse_fp(0xC2, 3, 0); emit_byte(0);  // x==x: not NaN
                    sse2_op(0xEF, 10, 10);             // pxor xmm10, xmm10 (0.0)
                    sse_fp(0x5F, 0, 10);               // maxps xmm0, 0 (negatives → 0)
                    movdqa(12, 0);                     // save x>=0 (saturation test)
                    movdqa(11, 0);
                    sse_fp(0xC2, 11, 8); emit_byte(5); // cmpps xmm11, 2^31, 5 (>=)
                    movdqa(1, 0);
                    sse_fp(0x5C, 1, 8);                // subps xmm1, xmm8 (x - 2^31)
                    sse2_f3(0x5B, 1, 1);               // cvttps2dq xmm1 (indefinite for x≥2^32)
                    sse2_op(0xEB, 1, 9);               // xmm1 |= 0x80000000
                    sse2_f3(0x5B, 0, 0);               // cvttps2dq xmm0 (lo)
                    sse2_op(0xDB, 1, 11);              // xmm1 = hi & sel
                    sse2_op(0xDF, 11, 0);              // xmm11 = ~sel & lo
                    sse2_op(0xEB, 1, 11);              // xmm1 = (sel&hi)|(~sel&lo)
                    emit_mask(2, 0x4F8000004F800000ULL);  // 4294967296.0f (2^32)
                    sse_fp(0xC2, 12, 2); emit_byte(5); // cmpps xmm12, 2^32, 5 (>=)
                    sse2_op(0xEB, 1, 12);              // saturate: xmm1 |= mask
                    sse2_op(0xDB, 1, 3);               // xmm1 &= not-NaN
                    movdqa(0, 1);
                    break;
                }
            }
            store_vec(0, static_cast<int>(inst.dest), sp.q);
            return true;
        }
        // ── SIMD ADDP (integer pairwise add) — native SSE2 ───────────
        // Vd = Vn's pair sums, then Vm's (wrapping). Q=0 pairs only the low
        // 64 bits of each source and zeroes v_hi.
        case IROp::SIMD_ADDP: {
            if (vec_cache_active_) { emit_call_interp(inst.arm_pc, false); return true; }
            const SimdAddpParams ap = inst.addp_params();
            const bool q = ap.q;
            clobber_flags();
            if (ap.esize != 1) {
                // Load both sources before writing Vd: it may alias either.
                load_vec(0, static_cast<int>(inst.src1));
                load_vec(1, static_cast<int>(inst.src2));
                if (!q) {
                    // Pair within [Vn.lo, Vm.lo]; its low half is the result.
                    sse2_op(0x6C, 0, 1);              // punpcklqdq xmm0, xmm1
                    movdqa(1, 0);
                }
                if (ap.esize == 2) {
                    // Each dword's low 16 bits become h0+h1 (wrapping); sign-
                    // extending them lets PACKSSDW pack without saturating.
                    for (int x = 0; x < 2; x++) {
                        movdqa(2, x);
                        sse2_imm(0x72, 2, 2, 16);     // psrld xmm2, 16 (h1)
                        sse2_op(0xFE, x, 2);          // paddd: low 16 = h0+h1
                        sse2_imm(0x72, x, 6, 16);     // pslld 16
                        sse2_imm(0x72, x, 4, 16);     // psrad 16
                    }
                    sse2_op(0x6B, 0, 1);              // packssdw xmm0, xmm1
                } else if (ap.esize == 4) {
                    movdqa(2, 0);
                    sse_fp(0xC6, 0, 1); emit_byte(0x88);  // shufps: [n0,n2,m0,m2]
                    sse_fp(0xC6, 2, 1); emit_byte(0xDD);  // shufps: [n1,n3,m1,m3]
                    sse2_op(0xFE, 0, 2);                  // paddd
                } else {                                  // esize 8, Q=1
                    movdqa(2, 0);
                    sse2_op(0x6C, 0, 1);              // punpcklqdq: [n0, m0]
                    sse2_op(0x6D, 2, 1);              // punpckhqdq: [n1, m1]
                    sse2_op(0xD4, 0, 2);              // paddq
                }
                store_vec(0, static_cast<int>(inst.dest), q);
                return true;
            }
            emit_mask(2, 0x00FF00FF00FF00FFULL);   // 0x00FF per 16-bit lane
            auto reduce = [&](int x) {
                movdqa(1, x);
                sse2_imm(0x71, 1, 2, 8);   // psrlw xmm1, 8  (b1 → low bytes)
                sse2_op(0xDB, x, 2);       // pand xmm{x}, 0x00FF (b0)
                sse2_op(0xFD, x, 1);       // paddw xmm{x}, xmm1  (b0 + b1)
                sse2_op(0xDB, x, 2);       // pand → (b0+b1) & 0xFF
                sse2_op(0x67, x, x);       // packuswb xmm{x}, xmm{x}
            };
            load_vec(0, static_cast<int>(inst.src1));
            reduce(0);
            int32_t dlo = V_LO_OFF + static_cast<int>(inst.dest) * 8;
            if (q) {
                // Load Vm before any destination write: Vd may alias Vm.
                load_vec(3, static_cast<int>(inst.src2));
                store_lo(0, dlo);
                movdqa(0, 3);
                reduce(0);
                store_lo(0, V_HI_OFF + static_cast<int>(inst.dest) * 8);
            } else {
                // 8B form: dest[0..3] = Vn pairs, dest[4..7] = Vm pairs.
                movdqa(3, 0);                        // save r_n
                load_vec(0, static_cast<int>(inst.src2));
                reduce(0);                           // r_m
                emit_mask(4, 0x00000000FFFFFFFFULL); // keep low 4 bytes of r_n
                sse2_op(0xDB, 3, 4);                 // r_n & 0xFFFFFFFF
                sse2_imm(0x73, 0, 7, 4);             // pslldq xmm0, 4 (r_m[0..3] → bytes 4-7)
                sse2_op(0xEB, 0, 3);                 // por
                store_lo(0, dlo);
                fp_zero_hi(static_cast<int>(inst.dest));
            }
            return true;
        }
        // ── SIMD XTN (XTN/SQXTUN/SQXTN/UQXTN, narrowing) — native ────
        // width = SOURCE esize (2/4/8); imm = subop (0=XTN, 1=SQXTUN,
        // 2=SQXTN, 3=UQXTN); flags_op = Q (0 → v_lo + zero v_hi, 1 → v_hi,
        // preserving v_lo). Full 128-bit source; 8 result bytes.
        case IROp::SIMD_XTN: {
            if (vec_cache_active_) { emit_call_interp(inst.arm_pc, false); return true; }
            const SimdSubopParams sp = inst.xtn_params();
            const int esize = static_cast<int>(sp.esize);
            const int subop = static_cast<int>(sp.subop);
            clobber_flags();
            load_vec(0, static_cast<int>(inst.src1));
            switch (subop) {
                case 0: {  // XTN — truncate
                    if (esize == 2) {
                        emit_mask(1, 0x00FF00FF00FF00FFULL);
                        sse2_op(0xDB, 0, 1);      // pand 0x00FF
                        sse2_op(0x67, 0, 0);      // packuswb
                    } else if (esize == 4) {
                        emit_mask(1, 0x0000FFFF0000FFFFULL);
                        sse2_op(0xDB, 0, 1);      // pand 0x0000FFFF
                        sse2_imm(0x72, 0, 6, 16); // pslld xmm0, 16
                        sse2_imm(0x72, 0, 4, 16); // psrad xmm0, 16 (sign-extend)
                        sse2_op(0x6B, 0, 0);      // packssdw
                    } else {                     // esize 8
                        sse2_op(0x70, 0, 0);      // pshufd xmm0, xmm0, 0x08
                        emit_byte(0x08);          // low 32 of each qword
                    }
                    break;
                }
                case 1: {  // SQXTUN — signed → unsigned saturating
                    if (esize == 2) {
                        sse2_op(0x67, 0, 0);      // packuswb (SSE2)
                    } else if (esize == 4) {
                        if (!has_sse41()) { emit_call_interp(inst.arm_pc, false); return true; }
                        sse2_38(0x2B, 0, 0);      // packusdw (SSE4.1)
                    } else {
                        emit_call_interp(inst.arm_pc, false);
                        return true;
                    }
                    break;
                }
                case 2: {  // SQXTN — signed → signed saturating
                    if (esize == 2) {
                        sse2_op(0x63, 0, 0);      // packsswb
                    } else if (esize == 4) {
                        sse2_op(0x6B, 0, 0);      // packssdw
                    } else {
                        emit_call_interp(inst.arm_pc, false);
                        return true;
                    }
                    break;
                }
                default: {  // UQXTN — unsigned → unsigned saturating
                    if (esize == 2) {
                        if (!has_sse41()) { emit_call_interp(inst.arm_pc, false); return true; }
                        emit_mask(1, 0x00FF00FF00FF00FFULL);
                        sse2_38(0x3A, 0, 1);      // pminuw xmm0, 0x00FF
                        sse2_op(0x67, 0, 0);      // packuswb
                    } else if (esize == 4) {
                        if (!has_sse41()) { emit_call_interp(inst.arm_pc, false); return true; }
                        emit_mask(1, 0x0000FFFF0000FFFFULL);
                        sse2_38(0x3B, 0, 1);      // pminud xmm0, 0x0000FFFF
                        sse2_imm(0x72, 0, 6, 16); // pslld 16
                        sse2_imm(0x72, 0, 4, 16); // psrad 16
                        sse2_op(0x6B, 0, 0);      // packssdw
                    } else {
                        emit_call_interp(inst.arm_pc, false);
                        return true;
                    }
                    break;
                }
            }
            if (sp.q) {
                store_lo(0, V_HI_OFF + static_cast<int>(inst.dest) * 8);
            } else {
                store_lo(0, V_LO_OFF + static_cast<int>(inst.dest) * 8);
                fp_zero_hi(static_cast<int>(inst.dest));
            }
            return true;
        }
        // ── SIMD TBL/TBX (vector table lookup) — native SSSE3 ────────
        // src1 = table base (rn; table regs are rn..rn+nregs-1), src2 =
        // index vector (rm). flags_op = (is_tbx<<1)|Q; imm = nregs (1/2).
        // Out-of-range indices → 0 (TBL) / keep dest byte (TBX). Only
        // XMM0-9 used; all helpers are REX-aware. Requires SSSE3 (pshufb).
        case IROp::SIMD_TBL: {
            const SimdTblParams tp = inst.tbl_params();
            if (vec_cache_active_ || !has_ssse3() || tp.nregs > 2) {
                emit_call_interp(inst.arm_pc, false);
                return true;
            }
            clobber_flags();
            const bool is_tbx = tp.is_tbx;
            const bool q = tp.q;
            const int nregs = static_cast<int>(tp.nregs);
            const int bp = q ? 16 : 8;                 // bytes per table reg
            const int table_len = nregs * bp;
            // ge(T) = (idx >= T) = pcmpgtb(paddb(idx,0x80), (T-129)&0xFF)
            //   (unsigned-compare trick: idx+128 as signed > -(129-T) ⟺ idx ≥ T)
            const uint8_t thr_bp = static_cast<uint8_t>((bp - 129) & 0xFF);
            const uint8_t thr_tl = static_cast<uint8_t>((table_len - 129) & 0xFF);
            load_vec(0, static_cast<int>(inst.src2));   // X0 = index vector
            load_vec(1, static_cast<int>(inst.src1));   // X1 = table reg 1
            if (nregs == 2)
                load_vec(3, (static_cast<int>(inst.src1) + 1) & 31);  // X3 = table reg 2
            emit_mask(4, bp == 16 ? 0x1010101010101010ULL : 0x0808080808080808ULL);
            emit_mask(5, 0x8080808080808080ULL);
            emit_mask(6, static_cast<uint64_t>(thr_bp) * 0x0101010101010101ULL);
            emit_mask(7, static_cast<uint64_t>(thr_tl) * 0x0101010101010101ULL);
            movdqa(9, 0);                               // X9 = index copy
            // control1 = idx | ge(bp); result1 = table1[control1]
            movdqa(8, 9);
            sse2_op(0xFC, 8, 5);                        // paddb X8, 0x80
            sse2_op(0x64, 8, 6);                        // X8 = ge(bp)
            sse2_op(0xEB, 0, 8);                        // X0 = control1
            sse2_38(0x00, 1, 0);                        // X1 = table1[control1]
            movdqa(2, 1);                               // X2 = result (so far)
            if (nregs == 2) {
                // control2 = (idx - bp) | ge(table_len); result2 = table2[control2]
                movdqa(8, 9);
                sse2_op(0xFC, 8, 5);
                sse2_op(0x64, 8, 7);                    // X8 = ge(table_len)
                movdqa(0, 9);
                sse2_op(0xF8, 0, 4);                    // psubb X0 = idx - bp
                sse2_op(0xEB, 0, 8);                    // X0 = control2
                sse2_38(0x00, 3, 0);                    // X3 = table2[control2]
                sse2_op(0xEB, 2, 3);                    // X2 |= result2
            }
            if (is_tbx) {
                // out = (dest & ge) | (result & ~ge)
                load_vec(0, static_cast<int>(inst.dest));   // X0 = dest
                sse2_op(0xDB, 0, 8);                       // X0 = dest & ge(tl)
                sse2_op(0xDF, 8, 2);                       // X8 = ~ge & result
                sse2_op(0xEB, 0, 8);                       // X0 = TBX out
            } else {
                movdqa(0, 2);                              // X0 = TBL out
            }
            store_vec(0, static_cast<int>(inst.dest), q);
            return true;
        }
        // ── SIMD INS (element, vector → element) — native GPR RMW ───
        // Copies the element at sidx (aux) of src1 into the element at
        // didx (imm / esize) of dest. GPR-mediated read-modify-write on
        // cpu.v_lo/v_hi (never vec-cache pinned). flags_op = Q.
        case IROp::SIMD_INS: {
            if (vec_cache_active_) { emit_call_interp(inst.arm_pc, false); return true; }
            const SimdInsParams ip = inst.ins_params();
            const int esize = static_cast<int>(ip.esize);
            const int src_byte = static_cast<int>(ip.sidx) * esize;
            const int sq = src_byte / 8;                   // 0 → v_lo, 1 → v_hi
            const int32_t soff = (sq ? V_HI_OFF : V_LO_OFF)
                + static_cast<int>(inst.src1) * 8 + (src_byte % 8);
            const int dst_byte = static_cast<int>(ip.dst_off);
            const int dq = dst_byte / 8;
            const int b_in_q = dst_byte % 8;               // field offset in the qword
            const int32_t doff = (dq ? V_HI_OFF : V_LO_OFF)
                + static_cast<int>(inst.dest) * 8;
            // Mask: keep everything except the esize-byte field at b_in_q.
            const int field_top = b_in_q * 8 + esize * 8;
            const uint64_t keep_high = (field_top < 64) ? (~0ULL << field_top) : 0;
            const uint64_t keep_low = (b_in_q == 0) ? 0 : (~0ULL >> (64 - b_in_q * 8));
            const uint64_t mask = keep_high | keep_low;
            clobber_flags();
            flush_invalidate_host_regs((1u << RAX) | (1u << RCX) | (1u << RDX));
            // Source element → RCX (zero-extended), shifted to its dest field.
            switch (esize) {
                case 1: emit_load8(RCX, CPU_REG, soff); break;
                case 2: emit_load16(RCX, CPU_REG, soff); break;
                case 4: emit_load32(RCX, CPU_REG, soff); break;
                default: emit_load(RCX, CPU_REG, soff); break;
            }
            emit_shift_imm8(RCX, 4, static_cast<uint8_t>(b_in_q * 8));
            emit_load(RAX, CPU_REG, doff);                 // dest qword
            emit_mov_imm64(RDX, mask);
            emit_and_reg(RAX, RDX);                        // dest & mask (clear field)
            emit_or_reg(RAX, RCX);                         // | element << shift
            emit_store(CPU_REG, doff, RAX);
            return true;
        }
        // ── SIMD PERMUTE (ZIP1/ZIP2/UZP1/UZP2/TRN1/TRN2) — SSSE3 ─────
        // Element-wise permute of two source vectors. imm = opc6
        // (0x06 UZP1, 0x0A TRN1, 0x0E ZIP1, 0x16 UZP2, 0x1A TRN2, 0x1E
        // ZIP2). Memory path: reads cpu.v_lo/v_hi directly (never
        // vec-cache pinned), so all XMM regs are free scratch. Requires
        // SSSE3 (pshufb): the two 16-byte control masks select bytes from
        // each source (0x80 elsewhere -> 0) and the masked halves POR
        // together. XMM0=A, XMM1=B, XMM2/3=maskA/maskB.
        case IROp::SIMD_PERMUTE: {
            if (vec_cache_active_ || !has_ssse3()) {
                emit_call_interp(inst.arm_pc, false);
                return true;
            }
            const SimdBinopParams pp = inst.permute_params();
            const int esize = static_cast<int>(pp.esize);
            const bool q = pp.q;
            uint8_t maskA[16], maskB[16];
            build_permute_masks(static_cast<uint8_t>(pp.subop), esize, q,
                                maskA, maskB);
            clobber_flags();
            load_vec(0, static_cast<int>(inst.src1));   // X0 = A
            load_vec(1, static_cast<int>(inst.src2));   // X1 = B
            emit_mask16(2, maskA);
            emit_mask16(3, maskB);
            sse2_38(0x00, 0, 2);   // X0 = pshufb(A, maskA)
            sse2_38(0x00, 1, 3);   // X1 = pshufb(B, maskB)
            sse2_op(0xEB, 0, 1);   // X0 |= X1
            store_vec(0, static_cast<int>(inst.dest), q);
            return true;
        }
        // ── SIMD PAIRMIN (SMAXP/SMINP/UMAXP/UMINP) — pairwise max/min ──
        // Pairwise min/max = lane-wise min/max of the EVEN and ODD elements
        // of each source (deinterleaved via pshufb), combined so Q=1 gives
        // pairwise(Vn) ++ pairwise(Vm). imm = subop (0=SMAXP, 1=SMINP,
        // 2=UMAXP, 3=UMINP); width = esize (1/2/4 — size==3 never encodes);
        // flags_op = Q. Memory path (never vec-cache pinned). Signed byte/
        // dword and ALL unsigned word/dword need SSE4.1 (pmin* ops);
        // unsigned byte and signed word are SSE2. esize==8 is a defensive
        // CALL_INTERP (no valid encoding exists, but the size bits are left
        // in the classify mask). XMM0=A, XMM1=B, XMM2/3=even/odd masks,
        // XMM4/5 = fusing scratch.
        default:
            return false;
    }
}
}  // namespace arm64emu
