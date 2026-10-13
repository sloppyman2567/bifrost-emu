// Inline bodies for the hot SIMD_NATIVE_EXTRA kinds. Each matches its
// run<KIND> helper in NativeSIMD.cpp lane for lane; the generated
// jit_native_simd_gen fixture checks both against QEMU. Every source is
// loaded before Vd is written, so aliased registers keep the helper's
// semantics. These ops are never vector- or FP-cache pinned (SIMD_NATIVE_EXTRA
// disqualifies both caches), so cpu.v_lo/v_hi are current and XMM0-5 are
// scratch. No GPR is read; only FMOV's constant uses RAX via emit_mask.
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
            return true;
        case NativeSimdKind::VariableShift:
            return has_avx2() && shift_lanes_inlinable(w);
        default:
            return false;
    }
}

bool FrostJIT::emit_native_simd_inline(uint32_t w) {
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
        case NativeSimdKind::Extract: {
            const uint8_t off = (w >> 11) & 15;
            if (q) {
                load_vec(0, rn);
                load_vec(1, rm);
                sse2_imm(0x73, 0, 3, off);                      // psrldq xmm0, off
                sse2_imm(0x73, 1, 7, static_cast<uint8_t>(16 - off));  // pslldq xmm1, 16-off
                sse2_op(0xEB, 0, 1);                             // por
            } else {
                load_lo(0, V_LO_OFF + rn * 8);                   // Vn.8B in bytes 0-7
                load_hi(0, V_LO_OFF + rm * 8);                   // Vm.8B in bytes 8-15
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
        default:
            return false;
    }
}
}  // namespace arm64emu
