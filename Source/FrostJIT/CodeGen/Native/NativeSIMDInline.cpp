// Inline bodies for the hot SIMD_NATIVE_EXTRA kinds. Each matches its
// run<KIND> helper in NativeSIMD.cpp lane for lane; the generated
// jit_native_simd_gen fixture checks both against QEMU. Every source is
// loaded before Vd is written, so aliased registers keep the helper's
// semantics. Cache-compatible bodies use XMM0-2 and cache-aware vector
// loads/stores. VariableShift uses XMM0-5 and still disqualifies pinning.
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include "FrostJIT/CodeGen/Native/NativeSIMD.hpp"
namespace arm64emu {
namespace {
bool shift_lanes_inlinable(uint32_t w) {
    // USHL/SSHL with 32- or 64-bit lanes; 8/16-bit lanes have no x86
    // variable shift below AVX-512BW.
    return ((w >> 22) & 3) >= 2;
}
}  // namespace

bool FrostJIT::native_simd_inlinable(uint32_t w) const {
    switch (classify_native_simd(w)) {
        case NativeSimdKind::Extract:
        case NativeSimdKind::CompareFp:
        case NativeSimdKind::CompareZeroFp:
        case NativeSimdKind::CompareZeroInt:
        case NativeSimdKind::ImmediateFp:
        case NativeSimdKind::Reverse64:
        case NativeSimdKind::WidenShift:
        case NativeSimdKind::WidenHigh:
        case NativeSimdKind::NarrowShift:
        case NativeSimdKind::WidenFp:
        case NativeSimdKind::NarrowFp:
            return true;
        case NativeSimdKind::Select:
        case NativeSimdKind::Dup:
        case NativeSimdKind::ReduceAdd:
            return true;
        case NativeSimdKind::MultiplyAdd:
            return ((w >> 22) & 3) != 2 || has_sse41();  // .4s/.2s need pmulld
        case NativeSimdKind::WidenThree:
            return has_sse41();                          // pmovsx/pmovzx, pmulld, pmuldq
        case NativeSimdKind::PairLong:
            return ((w >> 22) & 3) != 2 || has_sse41();  // 32-bit lanes widen with pmovsxdq
        case NativeSimdKind::RoundFp:
            // FRINTN/M/P/Z map to ROUNDPS modes; FRINTA/I/X (U=1) need ties-away
            // or FPCR.RMode and keep the helper.
            return !((w >> 29) & 1) && has_sse41();
        case NativeSimdKind::HalfFp:
            return cpu_features_.has_f16c();
        case NativeSimdKind::VariableShift:
            return has_avx2() && shift_lanes_inlinable(w);
        default:
            return false;
    }
}

bool FrostJIT::native_simd_cache_compatible(uint32_t w) const {
    // VariableShift borrows pinned XMM3-5. Every other inline body confines
    // scratch to XMM0-2 and accesses architectural vectors through load/store_vec.
    return native_simd_inlinable(w) && classify_native_simd(w) != NativeSimdKind::VariableShift;
}

bool FrostJIT::emit_native_simd_inline(const IRInst& inst) {
    const uint32_t w = inst.native_simd_opcode();
    if (!native_simd_inlinable(w)) return false;
    const int rd = w & 31, rn = (w >> 5) & 31, rm = (w >> 16) & 31;
    const bool q = (w >> 30) & 1, u = (w >> 29) & 1;
    const unsigned sz = (w >> 22) & 3;
    // cmpps/cmppd dst, src, predicate. 0 EQ_OQ, 1 LT_OS, 2 LE_OS: an
    // unordered (NaN) lane compares false, as on ARM.
    auto fp_compare = [&](int dst, int src, uint8_t predicate, bool is_double) {
        if (is_double) sse2_op(0xC2, dst, src);
        else sse_fp(0xC2, dst, src);
        emit_byte(predicate);
    };
    switch (classify_native_simd(w)) {
        case NativeSimdKind::WidenShift:
        case NativeSimdKind::WidenHigh: {
            const bool high = classify_native_simd(w) == NativeSimdKind::WidenHigh;
            const unsigned h = (w >> 19) & 15;
            const unsigned lane = high ? sz : 31u - __builtin_clz(h);
            const unsigned bits = 8u << lane;
            const unsigned shift = high ? bits : ((w >> 16) & 127) - bits;
            load_vec(0, rn);
            if (q) sse2_imm(0x73, 0, 3, 8);     // selected upper half
            sse2_op(0xEF, 1, 1);
            if (!high && !u)
                sse2_op(0x64 + lane, 1, 0);     // 0 > source: sign extension
            sse2_op(lane == 0 ? 0x60 : lane == 1 ? 0x61 : 0x62, 0, 1);
            if (shift) sse2_imm(0x71 + lane, 0, 6, static_cast<uint8_t>(shift));
            store_vec(0, rd, true);              // Q selects input half, not result width
            return true;
        }
        case NativeSimdKind::NarrowShift: {
            const unsigned lane = 31u - __builtin_clz((w >> 19) & 15);
            const unsigned bits = 8u << lane;
            sse2_op(0x76, 1, 1);                // all ones
            load_vec(0, rn);
            sse2_imm(0x71 + lane, 0, 2, static_cast<uint8_t>(2 * bits - ((w >> 16) & 127)));
            if (lane == 0) {
                sse2_imm(0x71, 1, 2, 8);        // mask to unsigned byte before packing
                sse2_op(0xDB, 0, 1);
                sse2_op(0x67, 0, 0);            // packuswb (no saturation after mask)
            } else if (lane == 1) {
                sse2_imm(0x72, 0, 6, 16);
                sse2_imm(0x72, 0, 4, 16);       // sign extend retained halfword
                sse2_op(0x6B, 0, 0);            // packssdw
            } else {
                sse2_op(0x70, 0, 0); emit_byte(0x88); // low dword of each qword
            }
            if (q) {
                load_vec(1, rd);
                sse2_op(0x6C, 1, 0);            // preserved low half + narrowed high half
                store_vec(1, rd, true);
            } else store_vec(0, rd, false);
            return true;
        }
        case NativeSimdKind::WidenFp:
        case NativeSimdKind::NarrowFp:
        case NativeSimdKind::HalfFp: {
            const auto kind = classify_native_simd(w);
            const bool half = kind == NativeSimdKind::HalfFp;
            const bool widen = kind == NativeSimdKind::WidenFp || (half && (w & 0x1000));
            clobber_flags();
            load_vec(0, rn);
            if (widen && q) sse2_imm(0x73, 0, 3, 8);
            SimdFpGuard special_done;
            if (widen) {
                if (half) emit_vex3(2, false, 0, 1, 0, 0, true, 0x13); // vcvtph2ps
                else sse_fp(0x5A, 0, 0);                              // cvtps2pd
                special_done = emit_simd_fp_nan_guard(inst, half ? 4 : 8, true);
                store_vec(0, rd, true);
            } else {
                // Guard original FP lanes before narrowing. A NaN step sees
                // the original aliased inputs and implements FPCR.DN/payloads.
                special_done = emit_simd_fp_nan_guard(inst, half ? 4 : 8, true);
                if (half) {
                    emit_vex3(3, false, 0, 1, 0, 0, true, 0x1D); // vcvtps2ph
                    emit_byte(0);                              // nearest-even, independent of MXCSR
                } else sse2_op(0x5A, 0, 0);                    // cvtpd2ps
                if (q) {
                    load_vec(1, rd);
                    sse2_op(0x6C, 1, 0);
                    store_vec(1, rd, true);
                } else store_vec(0, rd, false);
            }
            finish_simd_fp_guard(special_done);
            return true;
        }
        case NativeSimdKind::Extract: {
            const uint8_t off = (w >> 11) & 15;
            if (q) {
                load_vec(0, rn);
                load_vec(1, rm);
                sse2_imm(0x73, 0, 3, off);                      // psrldq xmm0, off
                sse2_imm(0x73, 1, 7, static_cast<uint8_t>(16 - off));  // pslldq xmm1, 16-off
                sse2_op(0xEB, 0, 1);                             // por
            } else {
                load_vec(0, rn);
                load_vec(1, rm);
                sse2_op(0x6C, 0, 1);                            // Vn.8B : Vm.8B
                sse2_imm(0x73, 0, 3, off);                      // psrldq xmm0, off
            }
            store_vec(0, rd, q);
            return true;
        }
        case NativeSimdKind::CompareFp: {
            const bool is_double = (w >> 22) & 1;
            load_vec(0, rn);
            load_vec(1, rm);
            if (!u) { fp_compare(0, 1, 0, is_double); store_vec(0, rd, q); }  // FCMEQ
            else {
                // FCMGE n>=m is m<=n; FCMGT n>m is m<n.
                fp_compare(1, 0, (w >> 23) & 1 ? 1 : 2, is_double);
                store_vec(1, rd, q);
            }
            return true;
        }
        case NativeSimdKind::CompareZeroFp: {
            const bool is_double = (w >> 22) & 1;
            const unsigned c = (w >> 12) & 3;  // 0 GT/GE, 1 EQ/LE, 2 LT
            load_vec(0, rn);
            sse2_op(0xEF, 1, 1);                                 // pxor xmm1: +0.0
            if (c == 0) {                                        // 0<n (GT), 0<=n (GE)
                fp_compare(1, 0, u ? 2 : 1, is_double);
                store_vec(1, rd, q);
            } else {                                             // n==0, n<=0, n<0
                fp_compare(0, 1, c == 2 ? 1 : (u ? 2 : 0), is_double);
                store_vec(0, rd, q);
            }
            return true;
        }
        case NativeSimdKind::CompareZeroInt: {
            load_vec(0, rn);
            sse2_op(0xEF, 1, 1);
            if (sz < 3) {
                sse2_op(0x74 + static_cast<int>(sz), 0, 1);      // pcmpeq{b,w,d}
            } else {
                // 64-bit lanes with SSE2: both dwords of a lane must be zero.
                sse2_op(0x76, 0, 1);                             // pcmpeqd
                sse2_op(0x70, 1, 0); emit_byte(0xB1);            // pshufd xmm1, xmm0: swap dwords
                sse2_op(0xDB, 0, 1);                             // pand
            }
            store_vec(0, rd, q);
            return true;
        }
        case NativeSimdKind::ImmediateFp: {
            const unsigned imm = (((w >> 16) & 7) << 5) | ((w >> 5) & 31);
            uint64_t value;
            if (u) {
                value = (uint64_t(imm & 63) << 48) | (uint64_t(imm & 128) << 56) |
                        ((imm & 64) ? 0x3FC0000000000000ULL : 0x4000000000000000ULL);
            } else {
                const uint64_t single = (uint64_t(imm & 63) << 19) | (uint64_t(imm & 128) << 24) |
                                        ((imm & 64) ? 0x3E000000ULL : 0x40000000ULL);
                value = single | (single << 32);
            }
            emit_mask(0, value);
            store_vec(0, rd, q);
            return true;
        }
        case NativeSimdKind::Reverse64: {
            load_vec(0, rn);
            if (sz == 2) {
                sse2_op(0x70, 0, 0); emit_byte(0xB1);            // pshufd: swap dwords
            } else {
                sse_f2(0x70, 0, 0); emit_byte(0x1B);             // pshuflw: reverse words
                sse2_f3(0x70, 0, 0); emit_byte(0x1B);            // pshufhw
                if (sz == 0) {                                   // then swap bytes in words
                    movdqa(1, 0);
                    sse2_imm(0x71, 0, 6, 8);                     // psllw xmm0, 8
                    sse2_imm(0x71, 1, 2, 8);                     // psrlw xmm1, 8
                    sse2_op(0xEB, 0, 1);                         // por
                }
            }
            store_vec(0, rd, q);
            return true;
        }
        case NativeSimdKind::VariableShift: {
            // Each lane shifts by SInt(Vm lane<7:0>): left when positive,
            // right when negative; a magnitude >= lane width gives 0 (or
            // the sign for SSHL). The counts are the low byte zero-extended:
            //   left  = n << b       (b >= 128 for a negative shift: 0)
            //   right = n >> (-b & 255)  (>= 129 for a positive one: 0)
            // vpsllv/vpsrlv give 0 for counts >= the lane width, so for USHL
            // left | right is exact (both equal n when b == 0). SSHL's right
            // shift is srl(n ^ sign, c) ^ sign, which gives the sign for a
            // large count, so it selects right only in negative-count lanes.
            const bool q64 = sz == 3;
            const uint8_t lane_bits = q64 ? 64 : 32;
            const int shift_group = q64 ? 0x73 : 0x72;
            load_vec(0, rn);                                     // xmm0 = n
            load_vec(1, rm);
            sse2_imm(shift_group, 1, 6, static_cast<uint8_t>(lane_bits - 8));  // psll{d,q}
            movdqa(5, 1);                                        // xmm5 = Vm byte at the top
            sse2_imm(shift_group, 1, 2, static_cast<uint8_t>(lane_bits - 8));  // xmm1 = b
            sse2_op(0xEF, 2, 2);
            sse2_op(q64 ? 0xFB : 0xFA, 2, 1);                    // psub{q,d}: xmm2 = -b
            sse2_imm(shift_group, 2, 6, static_cast<uint8_t>(lane_bits - 8));
            sse2_imm(shift_group, 2, 2, static_cast<uint8_t>(lane_bits - 8));  // xmm2 = -b & 255
            emit_vex3(2, q64, 0, 1, 3, 1, true, 0x47);           // vpsllv{d,q} xmm3, xmm0, xmm1
            if (u) {
                emit_vex3(2, q64, 0, 1, 4, 2, true, 0x45);       // vpsrlv{d,q} xmm4, xmm0, xmm2
                sse2_op(0xEB, 3, 4);                             // por
                store_vec(3, rd, q);
                return true;
            }
            movdqa(4, 0);
            sse2_imm(0x72, 4, 4, 31);                            // psrad xmm4, 31
            if (q64) { sse2_op(0x70, 4, 4); emit_byte(0xF5); }  // pshufd: 64-bit sign
            sse2_op(0xEF, 0, 4);                                 // xmm0 = n ^ sign
            emit_vex3(2, q64, 0, 1, 0, 2, true, 0x45);           // vpsrlv xmm0, xmm0, xmm2
            sse2_op(0xEF, 0, 4);                                 // xmm0 = right
            sse2_imm(0x72, 5, 4, 31);                            // psrad xmm5, 31: b < 0
            if (q64) { sse2_op(0x70, 5, 5); emit_byte(0xF5); }
            sse2_op(0xDB, 0, 5);                                 // pand right, mask
            sse2_op(0xDF, 5, 3);                                 // pandn: ~mask & left
            sse2_op(0xEB, 0, 5);                                 // por
            store_vec(0, rd, q);
            return true;
        }
        case NativeSimdKind::Select: {
            // BSL d ? n : m, BIT m ? n : d, BIF m ? d : n, as base ^ ((base ^ other) & mask).
            const unsigned which = sz;  // 1 BSL, 2 BIT, 3 BIF
            const int base = which == 1 ? 1 : which == 2 ? 2 : 0;   // m, d, n
            const int other = which == 1 ? 0 : which == 2 ? 0 : 2;  // n, n, d
            const int mask = which == 1 ? 2 : 1;                    // d, m, m
            load_vec(0, rn);
            load_vec(1, rm);
            load_vec(2, rd);
            const int t = base == 0 ? other : 0;  // a scratch that is not base
            if (t != other) movdqa(t, other);
            sse2_op(0xEF, t, base);  // pxor: base ^ other
            sse2_op(0xDB, t, mask);  // pand
            sse2_op(0xEF, t, base);  // pxor
            store_vec(t, rd, q);
            return true;
        }
        case NativeSimdKind::Dup: {
            const unsigned imm5 = (w >> 16) & 31, shift = __builtin_ctz(imm5);
            const unsigned lane = imm5 >> (shift + 1);
            load_vec(0, rn);
            if (shift == 3) {
                sse2_op(0x70, 0, 0); emit_byte(lane ? 0xEE : 0x44);      // pshufd: qword
            } else if (shift == 2) {
                sse2_op(0x70, 0, 0); emit_byte(static_cast<uint8_t>(lane * 0x55));
            } else {
                if (lane) sse2_imm(0x73, 0, 3, static_cast<uint8_t>(lane << shift)); // psrldq
                if (shift == 0) sse2_op(0x60, 0, 0);                     // punpcklbw: byte pair
                sse_f2(0x70, 0, 0); emit_byte(0);                        // pshuflw: word 0
                sse2_op(0x70, 0, 0); emit_byte(0);                       // pshufd: dword 0
            }
            store_vec(0, rd, q);
            return true;
        }
        case NativeSimdKind::ReduceAdd: {
            // ADDV: wrapping sum of the active lanes into lane 0, zero above.
            const unsigned bytes = 1u << sz;
            const int add = 0xFC + static_cast<int>(sz);                 // paddb/w/d
            load_vec(0, rn);
            if (!q) { emit_byte(0xF3); emit_byte(0x0F); emit_byte(0x7E); emit_byte(0xC0); } // movq
            for (unsigned step = q ? 8 : 4; step >= bytes; step /= 2) {
                movdqa(1, 0);
                sse2_imm(0x73, 1, 3, static_cast<uint8_t>(step));        // psrldq
                sse2_op(add, 0, 1);
            }
            sse2_imm(0x73, 0, 7, static_cast<uint8_t>(16 - bytes));      // pslldq
            sse2_imm(0x73, 0, 3, static_cast<uint8_t>(16 - bytes));      // psrldq
            store_vec(0, rd, true);
            return true;
        }
        case NativeSimdKind::MultiplyAdd: {
            // MLA: d + n * m per lane, modulo the lane width.
            load_vec(0, rn);
            load_vec(1, rm);
            if (sz == 0) {
                // No byte multiply: even bytes are the low bytes of word
                // products, odd bytes the products of the shifted high bytes.
                movdqa(2, 0);
                sse2_op(0xD5, 2, 1);                                     // pmullw: even
                sse2_imm(0x71, 0, 2, 8);                                 // psrlw
                sse2_imm(0x71, 1, 2, 8);
                sse2_op(0xD5, 0, 1);                                     // pmullw: odd
                sse2_imm(0x71, 0, 6, 8);                                 // psllw: to high bytes
                sse2_imm(0x71, 2, 6, 8);                                 // keep even low bytes
                sse2_imm(0x71, 2, 2, 8);
                sse2_op(0xEB, 0, 2);                                     // por
            } else if (sz == 1) {
                sse2_op(0xD5, 0, 1);                                     // pmullw
            } else {
                sse2_38(0x40, 0, 1);                                     // pmulld
            }
            load_vec(1, rd);
            sse2_op(0xFC + static_cast<int>(sz), 0, 1);                  // paddb/w/d
            store_vec(0, rd, q);
            return true;
        }
        case NativeSimdKind::WidenThree: {
            // S/U ADDL, SUBL, ADDW, SUBW, MLAL, MLSL, MULL (Q = the "2" forms).
            // The narrow lanes of Vn and Vm come from the low half, or from the
            // upper half when Q is set, extended to the double width (ADDW and
            // SUBW keep Vn at full width). Products of extended lanes are exact
            // in the wide lane, so the low-lane multiplies below match the helper.
            const unsigned op = (w >> 12) & 15;
            const bool wide_n = op == 0x1 || op == 0x3;
            const int extend = (u ? 0x30 : 0x20) + (sz == 0 ? 0 : sz == 1 ? 3 : 5);  // pmov{s,z}x{bw,wd,dq}
            const int add = sz == 0 ? 0xFD : sz == 1 ? 0xFE : 0xD4;                  // paddw/d/q
            const int sub = sz == 0 ? 0xF9 : sz == 1 ? 0xFA : 0xFB;                  // psubw/d/q
            load_vec(0, rn);
            load_vec(1, rm);
            if (q) {
                if (!wide_n) sse2_imm(0x73, 0, 3, 8);                    // psrldq: upper narrow half
                sse2_imm(0x73, 1, 3, 8);
            }
            if (!wide_n) sse2_38(extend, 0, 0);
            sse2_38(extend, 1, 1);
            switch (op) {
                case 0x0: case 0x1: sse2_op(add, 0, 1); break;
                case 0x2: case 0x3: sse2_op(sub, 0, 1); break;
                default:
                    if (sz == 0) sse2_op(0xD5, 0, 1);                    // pmullw
                    else if (sz == 1) sse2_38(0x40, 0, 1);               // pmulld
                    else if (u) sse2_op(0xF4, 0, 1);                     // pmuludq
                    else sse2_38(0x28, 0, 1);                            // pmuldq
                    if (op != 0xC) {                                     // MLAL / MLSL accumulate into Vd
                        load_vec(2, rd);
                        sse2_op(op == 0x8 ? add : sub, 2, 0);
                        store_vec(2, rd, true);
                        return true;
                    }
                    break;
            }
            store_vec(0, rd, true);
            return true;
        }
        case NativeSimdKind::PairLong: {
            // S/U ADDLP and ADALP: adjacent lane pairs summed into the double-width
            // lane. Only the low half of Vn (and Vd for ADALP) takes part unless Q.
            const bool accumulate = (w >> 14) & 1;
            const int add = sz == 0 ? 0xFD : sz == 1 ? 0xFE : 0xD4;                  // paddw/d/q
            auto movq_low = [&](int x) {                                             // zero the upper half
                emit_byte(0xF3); emit_byte(0x0F); emit_byte(0x7E); emit_byte(static_cast<uint8_t>(0xC0 | (x << 3) | x));
            };
            load_vec(0, rn);
            if (!q) movq_low(0);
            if (sz == 2) {
                movdqa(1, 0);
                sse2_op(0x70, 1, 1); emit_byte(0xE8);                    // pshufd: even dwords
                sse2_op(0x70, 0, 0); emit_byte(0xED);                    // pshufd: odd dwords
                sse2_38(u ? 0x35 : 0x25, 1, 1);                          // pmov{z,s}xdq
                sse2_38(u ? 0x35 : 0x25, 0, 0);
            } else {
                const unsigned bits = 8u << sz;                          // narrow lane width
                const int lshift = sz == 0 ? 0x71 : 0x72;                // psllw / pslld group
                movdqa(1, 0);
                if (u) {
                    sse2_op(0x76, 2, 2);                                 // all ones
                    sse2_imm(lshift, 2, 2, static_cast<uint8_t>(bits));  // psrl: low-lane mask
                    sse2_op(0xDB, 1, 2);                                 // pand: even lanes
                    sse2_imm(lshift, 0, 2, static_cast<uint8_t>(bits));  // psrl: odd lanes
                } else {
                    sse2_imm(lshift, 1, 6, static_cast<uint8_t>(bits));  // psll
                    sse2_imm(lshift, 1, 4, static_cast<uint8_t>(bits));  // psra: sign-extended even lanes
                    sse2_imm(lshift, 0, 4, static_cast<uint8_t>(bits));  // psra: odd lanes
                }
            }
            sse2_op(add, 0, 1);
            if (accumulate) {
                load_vec(2, rd);
                if (!q) movq_low(2);
                sse2_op(add, 0, 2);
            }
            store_vec(0, rd, true);
            return true;
        }
        case NativeSimdKind::RoundFp: {
            // FRINTN/M/P/Z: (sz bit 23, opcode bit 12) is ROUNDPS mode 0-3.
            // NaN lanes take the exact interpreter step (payloads, FPCR.DN).
            const bool is_double = (w >> 22) & 1;
            const uint8_t mode = static_cast<uint8_t>((((w >> 23) & 1) << 1) | ((w >> 12) & 1));
            clobber_flags();
            load_vec(0, rn);
            emit_byte(0x66); emit_byte(0x0F); emit_byte(0x3A);
            emit_byte(is_double ? 0x09 : 0x08); emit_byte(0xC0);             // roundp{s,d} xmm0,xmm0
            emit_byte(mode | 8);                                             // precision exception off
            const SimdFpGuard special_done = emit_simd_fp_nan_guard(inst, is_double ? 8 : 4, q);
            store_vec(0, rd, q);
            finish_simd_fp_guard(special_done);
            return true;
        }
        default:
            return false;
    }
}
}  // namespace arm64emu
