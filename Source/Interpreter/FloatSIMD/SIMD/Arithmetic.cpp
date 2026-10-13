// Arithmetic.cpp — SIMD_DP group 4: vector integer
// arithmetic (ADD/SUB/MUL, long/wide/narrow, absolute difference,
// saturating and pairwise forms) matched on sub3.
#include "Interpreter/FloatSIMD/FloatCommon.hpp"
namespace arm64emu {
bool Emulator::exec_simd_arith(const SimdDpFields& f, CPU& cpu) {
    uint32_t op = f.op;
    bool Q = f.Q;
    bool U = f.U;
    uint8_t size = f.size;
    uint8_t rm = f.rm;
    uint8_t rn = f.rn;
    uint8_t rd = f.rd;
    uint32_t sub = f.sub;
    uint32_t sub_noq = f.sub_noq;
    (void)op; (void)Q; (void)U; (void)size; (void)rm; (void)rn; (void)rd;
    (void)sub; (void)sub_noq;
    // ── Vector ADD/SUB/MUL (integer) ──────────────────────────
    // Encoding: 0x0E208400 (ADD) / 0x2E208400 (SUB) / 0x0E209C00 (MUL)
    // These are the most common SIMD arithmetic ops used by
    // memcpy/memset/string routines.
    {
        uint32_t sub3 = op & 0xFF20FC00;
        uint32_t sub3_noq = sub3 & ~(1u << 30);
        // ADD (vector): 0E208400
        if (sub3_noq == 0x0E208400) {
            int esize = 1 << size;
            int elems = (Q ? 16 : 8) / esize;
            uint8_t buf_n[16], buf_m[16];
            memcpy(buf_n, &cpu.v_lo[rn], 8);
            if (Q) memcpy(buf_n + 8, &cpu.v_hi[rn], 8);
            memcpy(buf_m, &cpu.v_lo[rm], 8);
            if (Q) memcpy(buf_m + 8, &cpu.v_hi[rm], 8);
            uint8_t out[16] = {0};
            for (int i = 0; i < elems; i++) {
                uint64_t a = 0, b = 0;
                memcpy(&a, buf_n + i*esize, esize);
                memcpy(&b, buf_m + i*esize, esize);
                uint64_t r = a + b;
                memcpy(out + i*esize, &r, esize);
            }
            memcpy(&cpu.v_lo[rd], out, 8);
            if (Q) memcpy(&cpu.v_hi[rd], out + 8, 8);
            else cpu.v_hi[rd] = 0;
            return true;
        }
        // SUB (vector): 2E208400
        if (sub3_noq == 0x2E208400) {
            int esize = 1 << size;
            int elems = (Q ? 16 : 8) / esize;
            uint8_t buf_n[16], buf_m[16];
            memcpy(buf_n, &cpu.v_lo[rn], 8);
            if (Q) memcpy(buf_n + 8, &cpu.v_hi[rn], 8);
            memcpy(buf_m, &cpu.v_lo[rm], 8);
            if (Q) memcpy(buf_m + 8, &cpu.v_hi[rm], 8);
            uint8_t out[16] = {0};
            for (int i = 0; i < elems; i++) {
                uint64_t a = 0, b = 0;
                memcpy(&a, buf_n + i*esize, esize);
                memcpy(&b, buf_m + i*esize, esize);
                uint64_t r = a - b;
                memcpy(out + i*esize, &r, esize);
            }
            memcpy(&cpu.v_lo[rd], out, 8);
            if (Q) memcpy(&cpu.v_hi[rd], out + 8, 8);
            else cpu.v_hi[rd] = 0;
            return true;
        }
        // MUL (vector): 0E209C00
        if (sub3_noq == 0x0E209C00) {
            int esize = 1 << size;
            int elems = (Q ? 16 : 8) / esize;
            uint8_t buf_n[16], buf_m[16];
            memcpy(buf_n, &cpu.v_lo[rn], 8);
            if (Q) memcpy(buf_n + 8, &cpu.v_hi[rn], 8);
            memcpy(buf_m, &cpu.v_lo[rm], 8);
            if (Q) memcpy(buf_m + 8, &cpu.v_hi[rm], 8);
            uint8_t out[16] = {0};
            for (int i = 0; i < elems; i++) {
                uint64_t a = 0, b = 0;
                memcpy(&a, buf_n + i*esize, esize);
                memcpy(&b, buf_m + i*esize, esize);
                uint64_t r = a * b;
                memcpy(out + i*esize, &r, esize);
            }
            memcpy(&cpu.v_lo[rd], out, 8);
            if (Q) memcpy(&cpu.v_hi[rd], out + 8, 8);
            else cpu.v_hi[rd] = 0;
            return true;
        }
        // SADDW/UADDW/SSUBW/USUBW (+2 variants, vector, widening
        // add/sub): extend the narrow lanes of Vm (sign for S*,
        // zero for U*) and add/subtract them from the wide lanes
        // of Vn. Q selects which half of Vm is used: Q=0 → low
        // half (SADDW), Q=1 → high half (SADDW2). Vd/Vn are always
        // full 128-bit (8H, 4S or 2D); the narrow source is 8B, 4H or 2S.
        //   SADDW  v.8h ← v.8h + v.8b   (size=0 → src B)
        //   SADDW  v.4s ← v.4s + v.4h   (size=1 → src H)
        //   SADDW  v.2d ← v.2d + v.2s   (size=2 → src S)
        // Encoding sub3_noq values (bit13 = subtract):
        //   SADDW 0x0E201000 UADDW 0x2E201000
        //   SSUBW 0x0E203000 USUBW 0x2E203000
        // libjpeg-turbo's color conversion does `uaddw v4.8h,
        // v6.8h, v0.8b`.
        if (sub3_noq == 0x0E201000 || sub3_noq == 0x2E201000 ||
            sub3_noq == 0x0E203000 || sub3_noq == 0x2E203000) {
            bool is_unsigned = (sub3_noq >> 29) & 1;
            bool is_sub = (sub3_noq & 0x2000) != 0;
            int esize_src = 1 << size;            // 2 (H) or 4 (S)
            int esize_dst = esize_src * 2;
            int lanes = 16 / esize_dst;         // 4 (4S) or 2 (2D)
            uint8_t vn[16], vm[16], vd[16];
            memcpy(vn, &cpu.v_lo[rn], 8);
            memcpy(vn + 8, &cpu.v_hi[rn], 8);
            memcpy(vm, &cpu.v_lo[rm], 8);
            memcpy(vm + 8, &cpu.v_hi[rm], 8);
            int src_off = Q ? lanes : 0;        // element index into Vm
            for (int i = 0; i < lanes; i++) {
                uint64_t wide = 0;
                memcpy(&wide, vn + i * esize_dst, esize_dst);
                uint64_t narrow = 0;
                memcpy(&narrow, vm + (src_off + i) * esize_src, esize_src);
                // Byte sources (.8h ← .8b) sign-extend too; they used to
                // fall into the 32-bit case and stay positive.
                const int shift = 64 - 8 * esize_src;
                const uint64_t ext = is_unsigned ? narrow
                    : static_cast<uint64_t>(static_cast<int64_t>(narrow << shift) >> shift);
                uint64_t r = is_sub ? wide - ext : wide + ext;
                memcpy(vd + i * esize_dst, &r, esize_dst);
            }
            memcpy(&cpu.v_lo[rd], vd, 8);
            memcpy(&cpu.v_hi[rd], vd + 8, 8);
            return true;
        }
        // SADDL/UADDL/SSUBL/USUBL (vector, widening long add/sub).
        // Sign- or zero-extend the narrow lanes of BOTH Vn and Vm
        // and add (SADDL/UADDL) or subtract (SSUBL/USUBL) them into
        // the double-width lanes of Vd. Q selects which half of
        // Vn/Vm is used: Q=0 → low half (no "2" suffix), Q=1 → high
        // half (SADDL2 etc). Vd is always full 128-bit (8H/4S/2D);
        // the narrow sources are 8B/8H/4S.
        //   SADDL  v.4s ← sxt(v.4h) + sxt(v.4h)   (size=1 → src H)
        //   SADDL  v.2d ← sxt(v.2s) + sxt(v.2s)   (size=2 → src S)
        //   UADDL  v.4s ← uxt(v.4h) + uxt(v.4h)   (U=1)
        //   SSUBL  v.4s ← sxt(v.4h) - sxt(v.4h)
        // Encoding sub3_noq values:
        //   SADDL 0x0E200000, UADDL 0x2E200000 (bit15=0)
        //   SSUBL 0x0E202000, USUBL 0x2E202000 (bit13=1)
        if (sub3_noq == 0x0E200000 || sub3_noq == 0x2E200000 ||
            sub3_noq == 0x0E202000 || sub3_noq == 0x2E202000) {
            int esize_src = 1 << size;          // 1 (B), 2 (H), 4 (S)
            int esize_dst = esize_src * 2;
            int lanes = 16 / esize_dst;         // 8 (8H), 4 (4S), 2 (2D)
            bool is_unsigned = (sub3_noq >> 29) & 1;
            bool is_subl = sub3_noq == 0x0E202000 || sub3_noq == 0x2E202000;
            uint8_t vn[16], vm[16], vd[16];
            memcpy(vn, &cpu.v_lo[rn], 8);
            memcpy(vn + 8, &cpu.v_hi[rn], 8);
            memcpy(vm, &cpu.v_lo[rm], 8);
            memcpy(vm + 8, &cpu.v_hi[rm], 8);
            int src_off = Q ? lanes : 0;        // element index into Vn/Vm
            for (int i = 0; i < lanes; i++) {
                uint64_t an = 0, am = 0;
                memcpy(&an, vn + (src_off + i) * esize_src, esize_src);
                memcpy(&am, vm + (src_off + i) * esize_src, esize_src);
                uint64_t en = is_unsigned ? an :
                    static_cast<uint64_t>(static_cast<int64_t>(an << (64 - esize_src * 8)) >> (64 - esize_src * 8));
                uint64_t em = is_unsigned ? am :
                    static_cast<uint64_t>(static_cast<int64_t>(am << (64 - esize_src * 8)) >> (64 - esize_src * 8));
                uint64_t r = is_subl ? en - em : en + em;
                memcpy(vd + i * esize_dst, &r, esize_dst);
            }
            memcpy(&cpu.v_lo[rd], vd, 8);
            memcpy(&cpu.v_hi[rd], vd + 8, 8);
            return true;
        }
        // ── Absolute-difference families (three-different + ABD) ──
        // Opcode bits[14:12] of the three-different space:
        //   0101 SABAL/UABAL (accumulate)   0111 SABDL/UABDL
        // sub3_noq keeps size(bits[23:22]) and bit21, so every lane
        // width is a distinct value: {0x0E,0x2E} | size<<22 |
        // 0x200000(bit21) | opcode<<12:
        //   SABDL/UABDL: {0x0E,0x2E}{20,60,A0}7000
        //   SABAL/UABAL: {0x0E,0x2E}{20,60,A0}5000
        // libpng's NEON filter row does uabdl v5.8h, v4.8b, v7.8b.
        // Same-width SABD/UABD (three-same) handled separately below.
        {
            // CRITICAL: match the widening abs-diff group by its
            // FULL 6-bit opcode — NOT by bits[14:12] alone. The
            // crude `(sub3_noq & 0x7000) == 0x5000/0x7000` test
            // also matched SRSHL/SQRSHL (opcode 010101/010111),
            // SQABS/SQNEG/SUQADD-adjacent two-reg-misc forms
            // (011110/011111) etc., silently routing them into
            // this handler and corrupting their results.
            //   SABDL/UABDL opcode6 = 011100 = 0x1C
            //   SABAL/UABAL opcode6 = 010100 = 0x14
            // sub3_noq keeps size(bits[23:22]) so one check covers
            // all lane widths; U(bit29) picks signed/unsigned.
            uint32_t opc6 = (sub3_noq >> 10) & 0x3F;
            bool is_u = (sub3_noq >> 29) & 1;
            if ((sub3_noq & 0x9F200C00) == 0x0E200000 && size < 3 &&
                (opc6 == 0x1C || opc6 == 0x14)) {
                int esize_src = 1 << size;        // 1/2/4
                int esize_dst = esize_src * 2;
                int lanes = 16 / esize_dst;
                bool accum = (opc6 == 0x14);
                uint8_t vn[16], vm[16], vd[16];
                memcpy(vn, &cpu.v_lo[rn], 8);
                memcpy(vn + 8, &cpu.v_hi[rn], 8);
                memcpy(vm, &cpu.v_lo[rm], 8);
                memcpy(vm + 8, &cpu.v_hi[rm], 8);
                if (accum) {
                    memcpy(vd, &cpu.v_lo[rd], 8);
                    memcpy(vd + 8, &cpu.v_hi[rd], 8);
                } else {
                    memset(vd, 0, sizeof(vd));
                }
                int src_off = Q ? lanes : 0;
                for (int i = 0; i < lanes; i++) {
                    uint64_t an = 0, am = 0;
                    memcpy(&an, vn + (src_off + i) * esize_src, esize_src);
                    memcpy(&am, vm + (src_off + i) * esize_src, esize_src);
                    uint64_t r;
                    if (is_u) {
                        r = (an >= am) ? an - am : am - an;
                    } else {
                        int64_t en = (int64_t)(an << (64 - esize_src * 8)) >> (64 - esize_src * 8);
                        int64_t em = (int64_t)(am << (64 - esize_src * 8)) >> (64 - esize_src * 8);
                        // Compare signed lanes before taking the magnitude.
                        // Source lanes are at most 32 bits, so either
                        // difference fits in int64_t, including endpoints.
                        r = (en >= em) ? (uint64_t)(en - em) : (uint64_t)(em - en);
                    }
                    if (accum) {
                        uint64_t acc = 0;
                        memcpy(&acc, vd + i * esize_dst, esize_dst);
                        r += acc;
                    }
                    memcpy(vd + i * esize_dst, &r, esize_dst);
                }
                memcpy(&cpu.v_lo[rd], vd, 8);
                memcpy(&cpu.v_hi[rd], vd + 8, 8);
                return true;
            }
        }
        // Same-width absolute difference: SABD/UABD (three-same
        // space, opcode bits[15:10] = 011100). sub3_noq values:
        if (sub3_noq == 0x0E207400 || sub3_noq == 0x2E207400 ||
            sub3_noq == 0x0E607400 || sub3_noq == 0x2E607400 ||
            sub3_noq == 0x0EA07400 || sub3_noq == 0x2EA07400) {
            int esize = 1 << size;
            int elems = (Q ? 16 : 8) / esize;
            bool is_u = (sub3_noq >> 29) & 1;
            uint8_t buf_n[16], buf_m[16];
            memcpy(buf_n, &cpu.v_lo[rn], 8);
            memcpy(buf_n + 8, &cpu.v_hi[rn], 8);
            memcpy(buf_m, &cpu.v_lo[rm], 8);
            memcpy(buf_m + 8, &cpu.v_hi[rm], 8);
            uint8_t out[16] = {0};
            for (int i = 0; i < elems; i++) {
                uint64_t a = 0, b = 0;
                memcpy(&a, buf_n + i * esize, esize);
                memcpy(&b, buf_m + i * esize, esize);
                uint64_t r;
                if (is_u) {
                    r = (a >= b) ? a - b : b - a;
                } else {
                    int64_t ae = (int64_t)(a << (64 - esize * 8)) >> (64 - esize * 8);
                    int64_t be = (int64_t)(b << (64 - esize * 8)) >> (64 - esize * 8);
                    r = (ae >= be) ? (uint64_t)(ae - be) : (uint64_t)(be - ae);
                }
                memcpy(out + i * esize, &r, esize);
            }
            memcpy(&cpu.v_lo[rd], out, 8);
            if (Q) memcpy(&cpu.v_hi[rd], out + 8, 8);
            else cpu.v_hi[rd] = 0;
            return true;
        }
        // SMLAL/UMLAL/SMLSL/UMLSL/SMULL/UMULL (vector, widening
        // long multiply). Sign- or zero-extend the narrow lanes of
        // Vm and multiply into the wide lanes of Vd. Q selects which
        // half of Vm/Vn is used: Q=0 → low half (no "2" suffix),
        // Q=1 → high half (the "2" variants). Vd is always full
        // 128-bit (4S or 2D); the narrow source is 8H or 4S.
        //   SMLAL  v.4s ← v.4s + sxt(v.4h)*sxt(v.4h)  (size=1 → H)
        //   UMLAL  v.4s ← v.4s + uxt(v.4h)*uxt(v.4h)  (U=1)
        //   SMLSL  v.4s ← v.4s - sxt(v.4h)*sxt(v.4h)
        //   SMULL  v.4s ← sxt(v.4h)*sxt(v.4h)          (no accum)
        //   SMLAL  v.2d ← v.2d + sxt(v.2s)*sxt(v.2s)  (size=2 → S)
        // Encoding sub3_noq values:
        //   SMLAL 0x0E208000, UMLAL 0x2E208000 (bit15=1, +accum)
        //   SMLSL 0x0E20A000, UMLSL 0x2E20A000 (bit15=1, -accum)
        //   SMULL 0x0E20C000, UMULL 0x2E20C000 (bit15=1, no accum)
        if (sub3_noq == 0x0E208000 || sub3_noq == 0x2E208000 ||
            sub3_noq == 0x0E20A000 || sub3_noq == 0x2E20A000 ||
            sub3_noq == 0x0E20C000 || sub3_noq == 0x2E20C000) {
            int esize_src = 1 << size;            // 1 (B), 2 (H), 4 (S)
            int esize_dst = esize_src * 2;
            int lanes = 16 / esize_dst;           // 8 (8H), 4 (4S), 2 (2D)
            bool is_unsigned = (sub3_noq >> 29) & 1;
            bool is_subl = sub3_noq == 0x0E20A000 || sub3_noq == 0x2E20A000;
            bool no_accum = sub3_noq == 0x0E20C000 || sub3_noq == 0x2E20C000;
            uint8_t vn[16], vm[16], vd[16];
            memcpy(vn, &cpu.v_lo[rn], 8);
            memcpy(vn + 8, &cpu.v_hi[rn], 8);
            memcpy(vm, &cpu.v_lo[rm], 8);
            memcpy(vm + 8, &cpu.v_hi[rm], 8);
            memcpy(vd, &cpu.v_lo[rd], 8);
            memcpy(vd + 8, &cpu.v_hi[rd], 8);
            int src_off = Q ? lanes : 0;          // element index into Vn/Vm
            for (int i = 0; i < lanes; i++) {
                uint64_t an = 0, am = 0;
                memcpy(&an, vn + (src_off + i) * esize_src, esize_src);
                memcpy(&am, vm + (src_off + i) * esize_src, esize_src);
                uint64_t en = is_unsigned ? an :
                    static_cast<uint64_t>(static_cast<int64_t>(an << (64 - esize_src * 8)) >> (64 - esize_src * 8));
                uint64_t em = is_unsigned ? am :
                    static_cast<uint64_t>(static_cast<int64_t>(am << (64 - esize_src * 8)) >> (64 - esize_src * 8));
                uint64_t prod = en * em;
                uint64_t acc = 0;
                if (!no_accum) memcpy(&acc, vd + i * esize_dst, esize_dst);
                uint64_t r = is_subl ? acc - prod : acc + prod;
                memcpy(vd + i * esize_dst, &r, esize_dst);
            }
            memcpy(&cpu.v_lo[rd], vd, 8);
            memcpy(&cpu.v_hi[rd], vd + 8, 8);
            return true;
        }
        // ── Saturating integer arithmetic (three-same) ────────
        // Encodings verified against the cross assembler:
        //   SQADD/UQADD   sub3_noq 0x0E200C00 / 0x2E200C00
        //   SQSUB/UQSUB          0x0E202C00 / 0x2E202C00
        //   SQSHL/UQSHL (reg)    0x0E204C00 / 0x2E204C00
        //   SQRSHL/UQRSHL        0x0E205C00 / 0x2E205C00
        //   SRSHL/URSHL          0x0E205400 / 0x2E205400
        // The U bit (bit29) selects the unsigned variant. Shift-
        // by-register amounts: SQSHL reads a SIGNED Vm lane
        // (negative → arithmetic right), UQSHL an UNSIGNED lane
        // (left shifts only — huge amounts saturate/fill). The
        // ROUNDING variants always read a SIGNED amount (their
        // purpose is variable right-shifts expressed as negative
        // left-shifts); they round half-up on right shifts only.
        // Saturation applies only on left overflow.
        {
            const uint32_t s = sub3_noq;
            bool is_add = false, is_sub = false, is_shift = false;
            switch (s) {
            case 0x0E200C00: case 0x2E200C00: is_add = true; break;
            case 0x0E202C00: case 0x2E202C00: is_sub = true; break;
            case 0x0E204C00: case 0x2E204C00:
            case 0x0E205C00: case 0x2E205C00:
            case 0x0E205400: case 0x2E205400:
                is_shift = true; break;
            default: break;
            }
            if (is_add || is_sub || is_shift) {
                if (size == 3 && !Q) throw DecodeError(cpu.pc, op);  // .1d is reserved
                const bool uns = (s >> 29) & 1;
                const int esize = 1 << size;
                const int bits = esize * 8;
                const int elems = (Q ? 16 : 8) / esize;
                const bool round =
                    (s == 0x0E205C00 || s == 0x2E205C00 ||
                     s == 0x0E205400 || s == 0x2E205400);
                const bool saturate =
                    (s != 0x0E205400 && s != 0x2E205400);
                const uint64_t umax =
                    (bits >= 64) ? ~0ULL : ((1ULL << bits) - 1);
                uint8_t buf_n[16], buf_m[16];
                memcpy(buf_n, &cpu.v_lo[rn], 8);
                memcpy(buf_n + 8, &cpu.v_hi[rn], 8);
                memcpy(buf_m, &cpu.v_lo[rm], 8);
                memcpy(buf_m + 8, &cpu.v_hi[rm], 8);
                uint8_t out[16] = {0};
                for (int i = 0; i < elems; i++) {
                    uint64_t a = 0, b = 0;
                    memcpy(&a, buf_n + i * esize, esize);
                    memcpy(&b, buf_m + i * esize, esize);
                    // Operands as exact integers (Int(x, unsigned)).
                    __int128 v = uns ? static_cast<__int128>(a)
                        : static_cast<__int128>(static_cast<int64_t>(sext_lane(a, bits)));
                    const __int128 w = uns ? static_cast<__int128>(b)
                        : static_cast<__int128>(static_cast<int64_t>(sext_lane(b, bits)));
                    if (is_add) {
                        v += w;
                    } else if (is_sub) {
                        v -= w;
                    } else {
                        // Every register shift form takes the SIGNED low
                        // byte of the Vm lane (SInt(Vm<7:0>)) and computes
                        // (Int(a, unsigned) + round) << shift exactly,
                        // where round = 1 << (-shift - 1) for rounding
                        // right shifts.
                        const int sh = static_cast<int8_t>(b & 0xFF);
                        if (sh >= 0) {
                            // |v| < 2^64 and sh < 64 keeps v << sh in range;
                            // larger shifts are 0 or saturate.
                            if (v != 0 && sh >= bits)
                                v = !saturate ? 0 : v < 0 ? -((__int128)1 << 100) : ((__int128)1 << 100);
                            else
                                v <<= sh;
                        } else {
                            const int rs = -sh;  // 1..128
                            if (rs >= 100) v = round ? 0 : (v < 0 ? -1 : 0);
                            else v = (v + (round ? (__int128)1 << (rs - 1) : 0)) >> rs;
                        }
                    }
                    // Q forms saturate to the lane range (FPSR.QC).
                    if (saturate) {
                        const __int128 hi = uns ? static_cast<__int128>(umax)
                            : static_cast<__int128>(static_cast<int64_t>(smax_lane(bits)));
                        const __int128 lo = uns ? 0
                            : static_cast<__int128>(static_cast<int64_t>(sext_lane(smin_lane(bits), bits)));
                        if (v > hi) { v = hi; cpu.fpsr |= 1u << 27; }
                        else if (v < lo) { v = lo; cpu.fpsr |= 1u << 27; }
                    }
                    const uint64_t r = static_cast<uint64_t>(v);
                    memcpy(out + i * esize, &r, esize);
                }
                memcpy(&cpu.v_lo[rd], out, 8);
                if (Q) memcpy(&cpu.v_hi[rd], out + 8, 8);
                else cpu.v_hi[rd] = 0;
                return true;
            }
        }
        // ── SQDMULH/SQRDMULH, SABA/UABA, PMUL (three-same) and
        //    SQDMLAL/SQDMLSL/SQDMULL{2} (three-different) ─────────
        //   SQDMULH 0x0E20B400 / SQRDMULH 0x2E20B400   size 1/2
        //   SABA    0x0E207C00 / UABA     0x2E207C00   size 0-2
        //   PMUL    0x2E209C00                         size 0
        //   SQDMLAL 0x0E209000, SQDMLSL 0x0E20B000, SQDMULL 0x0E20D000
        //   (U=0, size 1/2; Q selects the source half, the `2` forms).
        // Other sizes are reserved. Doubling multiplies saturate with
        // FPSR.QC (arm_sqdmulh/arm_sqdmlal); PMUL is a carry-less
        // byte product.
        {
            const uint32_t s = sub3_noq & ~(3u << 22);
            const bool three_diff = s == 0x0E209000 || s == 0x0E20B000 || s == 0x0E20D000;
            const bool same = s == 0x0E20B400 || s == 0x2E20B400 || s == 0x0E207C00 ||
                              s == 0x2E207C00 || s == 0x2E209C00;
            if (three_diff || same) {
                const bool is_dmulh = s == 0x0E20B400 || s == 0x2E20B400;
                const bool is_aba = s == 0x0E207C00 || s == 0x2E207C00;
                const bool ok = s == 0x2E209C00 ? size == 0
                              : is_aba ? size != 3
                                       : size == 1 || size == 2;
                if (!ok) throw DecodeError(cpu.pc, op);
                const int esize = 1 << size, bits = esize * 8;
                const int dsize = three_diff ? 2 * esize : esize;
                const int elems = three_diff ? 8 / esize : (Q ? 16 : 8) / esize;
                const int src_base = three_diff && Q ? 8 : 0;
                uint8_t buf_n[16], buf_m[16], vd[16], out[16] = {0};
                memcpy(buf_n, &cpu.v_lo[rn], 8); memcpy(buf_n + 8, &cpu.v_hi[rn], 8);
                memcpy(buf_m, &cpu.v_lo[rm], 8); memcpy(buf_m + 8, &cpu.v_hi[rm], 8);
                memcpy(vd, &cpu.v_lo[rd], 8); memcpy(vd + 8, &cpu.v_hi[rd], 8);
                for (int i = 0; i < elems; i++) {
                    uint64_t a = 0, b = 0, acc = 0;
                    memcpy(&a, buf_n + src_base + i * esize, esize);
                    memcpy(&b, buf_m + src_base + i * esize, esize);
                    memcpy(&acc, vd + i * dsize, dsize);
                    const int64_t sa = static_cast<int64_t>(sext_lane(a, bits));
                    const int64_t sb = static_cast<int64_t>(sext_lane(b, bits));
                    uint64_t r;
                    if (is_dmulh) {
                        r = static_cast<uint64_t>(arm_sqdmulh(cpu, sa, sb, bits, U));
                    } else if (three_diff) {
                        const int64_t sacc = static_cast<int64_t>(sext_lane(acc, 2 * bits));
                        r = static_cast<uint64_t>(arm_sqdmlal(cpu, sacc, sa, sb, bits,
                                                              s == 0x0E20D000 ? 0 : s == 0x0E209000 ? 1 : -1));
                    } else if (is_aba) {
                        const int64_t x = U ? static_cast<int64_t>(a) : sa;
                        const int64_t y = U ? static_cast<int64_t>(b) : sb;
                        r = acc + static_cast<uint64_t>(x > y ? x - y : y - x);
                    } else {  // PMUL
                        r = 0;
                        for (int bit = 0; bit < 8; ++bit)
                            if ((b >> bit) & 1) r ^= a << bit;
                    }
                    memcpy(out + i * dsize, &r, dsize);
                }
                memcpy(&cpu.v_lo[rd], out, 8);
                memcpy(&cpu.v_hi[rd], out + 8, 8);
                if (!three_diff && !Q) cpu.v_hi[rd] = 0;
                return true;
            }
        }
        // ── SQABS / SQNEG / SUQADD / USQADD (2-reg misc) ──────
        //   SQABS/SQNEG   sub3_noq 0x0E207800 / 0x2E207800
        //   SUQADD/USQADD 0x0E203800 / 0x2E203800 (accumulate into Vd)
        // Unlike plain ABS/NEG these SATURATE to the signed range:
        // SQABS(INT_MIN) = INT_MAX, SQNEG(INT_MIN) = INT_MAX.
        // SUQADD adds UNSIGNED Vn lanes into signed Vd lanes with
        // signed saturation; USQADD adds SIGNED Vn lanes into
        // unsigned Vd lanes with unsigned saturation (USQADD used to
        // zero-extend Vn). Saturation sets FPSR.QC; .1d is reserved.
        // Q=0 zeroes the destination upper half like every other
        // vector op.
        if (sub3_noq == 0x0E207800 || sub3_noq == 0x2E207800 ||
            sub3_noq == 0x0E203800 || sub3_noq == 0x2E203800) {
            if (size == 3 && !Q) throw DecodeError(cpu.pc, op);
            const int esize = 1 << size;
            const int bits = esize * 8;
            const int elems = (Q ? 16 : 8) / esize;
            uint8_t vn[16], vd[16], out[16] = {0};
            memcpy(vn, &cpu.v_lo[rn], 8);
            memcpy(vn + 8, &cpu.v_hi[rn], 8);
            memcpy(vd, &cpu.v_lo[rd], 8);
            memcpy(vd + 8, &cpu.v_hi[rd], 8);
            const __int128 umax = (static_cast<__int128>(1) << bits) - 1;
            const __int128 smax = umax >> 1, smin = -smax - 1;
            for (int i = 0; i < elems; i++) {
                uint64_t n = 0, d = 0;
                memcpy(&n, vn + i * esize, esize);
                memcpy(&d, vd + i * esize, esize);
                const __int128 sn = static_cast<int64_t>(sext_lane(n, bits));
                const __int128 sd = static_cast<int64_t>(sext_lane(d, bits));
                const bool is_usqadd = sub3_noq == 0x2E203800;
                __int128 w;
                if (sub3_noq == 0x0E207800)      w = sn < 0 ? -sn : sn;  // SQABS
                else if (sub3_noq == 0x2E207800) w = -sn;                // SQNEG
                else if (!is_usqadd)             w = sd + static_cast<__int128>(n);  // SUQADD
                else                             w = static_cast<__int128>(d) + sn;  // USQADD
                const __int128 hi = is_usqadd ? umax : smax;
                const __int128 lo = is_usqadd ? 0 : smin;
                if (w > hi) { w = hi; cpu.fpsr |= 1u << 27; }
                else if (w < lo) { w = lo; cpu.fpsr |= 1u << 27; }
                const uint64_t r = static_cast<uint64_t>(w);
                memcpy(out + i * esize, &r, esize);
            }
            memcpy(&cpu.v_lo[rd], out, 8);
            if (Q) memcpy(&cpu.v_hi[rd], out + 8, 8);
            else cpu.v_hi[rd] = 0;
            return true;
        }
        // ── ABS / NEG (vector) — 0x0E20B800 / 0x2E20B800 ──
        // ABS Vd.<T>, Vn.<T> = |Vn| per lane; NEG = 0 - Vn per lane.
        // For ABS of the minimum signed value (e.g. INT8_MIN), ARM
        // defines the result as the input unchanged (no overflow).
        // NOTE: FCVTZS/FCVTZU share sub3_noq (0x0E20B800/0x2E20B800)
        // but set bit16 (0x10000), so exclude them here (sub3 drops
        // bits 20:16; any other value there is a different group).
        // .1d is reserved.
        if ((sub3_noq == 0x0E20B800 || sub3_noq == 0x2E20B800) &&
            !(op & 0x1F0000)) {
            if (size == 3 && !Q) throw DecodeError(cpu.pc, op);
            int esize = 1 << size;
            int elems = (Q ? 16 : 8) / esize;
            bool is_abs = sub3_noq == 0x0E20B800;
            uint8_t vn[16], out[16] = {0};
            memcpy(vn, &cpu.v_lo[rn], 8);
            if (Q) memcpy(vn + 8, &cpu.v_hi[rn], 8);
            for (int i = 0; i < elems; i++) {
                uint64_t v = 0;
                memcpy(&v, vn + i * esize, esize);
                if (is_abs) {
                    int64_t sv = static_cast<int64_t>(v << (64 - esize * 8)) >> (64 - esize * 8);
                    uint64_t r = static_cast<uint64_t>(sv < 0 ? -sv : sv);
                    memcpy(out + i * esize, &r, esize);
                } else {
                    uint64_t r = static_cast<uint64_t>(0) - v;
                    memcpy(out + i * esize, &r, esize);
                }
            }
            memcpy(&cpu.v_lo[rd], out, 8);
            if (Q) memcpy(&cpu.v_hi[rd], out + 8, 8);
            else cpu.v_hi[rd] = 0;
            return true;
        }
        // ── FCVTZS / FCVTZU (vector, FP→int, toward zero) ──
        // Same sub3_noq as ABS/NEG but with bit16 (0x10000) set.
        //   FCVTZS Vd.4S, Vn.4S = 0x4EA1B800  (signed, U=0)
        //   FCVTZU Vd.4S, Vn.4S = 0x6EA1B800  (unsigned, U=1)
        //   size=2 → 32-bit (4S/2S), size=3 → 64-bit (2D/1D)
        // Truncate toward zero (rmode=3). NaN → 0; ±inf and out-of-range
        // values saturate (the helpers implement FPToFixed's clamp).
        if ((sub3_noq == 0x0E20B800 || sub3_noq == 0x2E20B800) &&
            (op & 0x10000) && size >= 2) {
            if (size == 3 && !Q) throw DecodeError(cpu.pc, op);  // .1d is reserved
            bool is_unsigned = sub3_noq == 0x2E20B800;
            int esize = 1 << size;
            int elems = (Q ? 16 : 8) / esize;
            uint8_t vn[16], out[16] = {0};
            memcpy(vn, &cpu.v_lo[rn], 8);
            if (Q) memcpy(vn + 8, &cpu.v_hi[rn], 8);
            for (int i = 0; i < elems; i++) {
                uint64_t r = 0;
                if (esize == 4) {
                    float f;
                    memcpy(&f, vn + i * esize, 4);
                    r = is_unsigned ? fp_to_unsigned_sat(f, false)
                        : static_cast<uint32_t>(static_cast<int32_t>(fp_to_signed_sat(f, false)));
                } else {
                    double d;
                    memcpy(&d, vn + i * esize, 8);
                    r = is_unsigned ? fp_to_unsigned_sat(d, true)
                        : static_cast<uint64_t>(fp_to_signed_sat(d, true));
                }
                memcpy(out + i * esize, &r, esize);
            }
            memcpy(&cpu.v_lo[rd], out, 8);
            if (Q) memcpy(&cpu.v_hi[rd], out + 8, 8);
            else cpu.v_hi[rd] = 0;
            return true;
        }
        // UMIN/UMAX/SMIN/SMAX (vector): lane-wise min/max.
        //   UMIN 0x2E206C00 (Q=1 → 0x6E206C00)  UMAX 0x2E206400
        //   SMIN 0x0E206C00 (Q=1 → 0x4E206C00)  SMAX 0x0E206400
        if (sub3_noq == 0x2E206C00 || sub3_noq == 0x2E206400 ||
            sub3_noq == 0x0E206C00 || sub3_noq == 0x0E206400) {
            int esize = 1 << size;
            int elems = (Q ? 16 : 8) / esize;
            bool is_unsigned = sub3_noq == 0x2E206C00 || sub3_noq == 0x2E206400;
            bool is_min = sub3_noq == 0x2E206C00 || sub3_noq == 0x0E206C00;
            uint8_t vn[16], vm[16], out[16] = {0};
            memcpy(vn, &cpu.v_lo[rn], 8);
            if (Q) memcpy(vn + 8, &cpu.v_hi[rn], 8);
            memcpy(vm, &cpu.v_lo[rm], 8);
            if (Q) memcpy(vm + 8, &cpu.v_hi[rm], 8);
            for (int i = 0; i < elems; i++) {
                uint64_t a = 0, b = 0;
                memcpy(&a, vn + i * esize, esize);
                memcpy(&b, vm + i * esize, esize);
                uint64_t r;
                if (is_unsigned) {
                    r = is_min ? (a < b ? a : b) : (a > b ? a : b);
                } else {
                    int64_t sa = static_cast<int64_t>(a << (64 - esize * 8)) >> (64 - esize * 8);
                    int64_t sb = static_cast<int64_t>(b << (64 - esize * 8)) >> (64 - esize * 8);
                    int64_t sr = is_min ? (sa < sb ? sa : sb) : (sa > sb ? sa : sb);
                    r = static_cast<uint64_t>(sr);
                }
                memcpy(out + i * esize, &r, esize);
            }
            memcpy(&cpu.v_lo[rd], out, 8);
            if (Q) memcpy(&cpu.v_hi[rd], out + 8, 8);
            else cpu.v_hi[rd] = 0;
            return true;
        }
        // MLA / MLS (vector, multiply-accumulate):
        //   MLA Vd.<T>, Vn.<T>, Vm.<T> = Vd + Vn*Vm  (0x0E209400)
        //   MLS Vd.<T>, Vn.<T>, Vm.<T> = Vd - Vn*Vm  (0x2E209400)
        if (sub3_noq == 0x0E209400 || sub3_noq == 0x2E209400) {
            int esize = 1 << size;
            int elems = (Q ? 16 : 8) / esize;
            bool is_mls = sub3_noq == 0x2E209400;
            uint8_t vn[16], vm[16], vd[16];
            memcpy(vn, &cpu.v_lo[rn], 8);
            if (Q) memcpy(vn + 8, &cpu.v_hi[rn], 8);
            memcpy(vm, &cpu.v_lo[rm], 8);
            if (Q) memcpy(vm + 8, &cpu.v_hi[rm], 8);
            memcpy(vd, &cpu.v_lo[rd], 8);
            if (Q) memcpy(vd + 8, &cpu.v_hi[rd], 8);
            for (int i = 0; i < elems; i++) {
                uint64_t a = 0, b = 0, acc = 0;
                memcpy(&a, vn + i * esize, esize);
                memcpy(&b, vm + i * esize, esize);
                memcpy(&acc, vd + i * esize, esize);
                uint64_t r = is_mls ? acc - a * b : acc + a * b;
                memcpy(vd + i * esize, &r, esize);
            }
            memcpy(&cpu.v_lo[rd], vd, 8);
            if (Q) memcpy(&cpu.v_hi[rd], vd + 8, 8);
            else cpu.v_hi[rd] = 0;
            return true;
        }
        // UHADD/UHSUB/URHADD/SHADD/SHSUB/SRHADD (vector, halving
        // add/sub): HADD = (a+b)>>1, HSUB = (a-b)>>1, RHADD =
        // (a+b+1)>>1, per lane. Signed variants use arithmetic
        // shift (round toward -inf); RHADD rounds to nearest.
        //   UHADD 0x2E200400  UHSUB 0x2E202400  URHADD 0x2E201400
        //   SHADD 0x0E200400  SHSUB 0x0E202400  SRHADD 0x0E201400
        if (sub3_noq == 0x2E200400 || sub3_noq == 0x2E202400 ||
            sub3_noq == 0x2E201400 || sub3_noq == 0x0E200400 ||
            sub3_noq == 0x0E202400 || sub3_noq == 0x0E201400) {
            if (size == 3) throw DecodeError(cpu.pc, op);  // 64-bit lanes are reserved
            int esize = 1 << size;
            int elems = (Q ? 16 : 8) / esize;
            bool is_unsigned = sub3_noq == 0x2E200400 || sub3_noq == 0x2E202400 || sub3_noq == 0x2E201400;
            bool is_sub = sub3_noq == 0x2E202400 || sub3_noq == 0x0E202400;
            bool is_rhadd = sub3_noq == 0x2E201400 || sub3_noq == 0x0E201400;
            uint8_t vn[16], vm[16], out[16] = {0};
            memcpy(vn, &cpu.v_lo[rn], 8);
            if (Q) memcpy(vn + 8, &cpu.v_hi[rn], 8);
            memcpy(vm, &cpu.v_lo[rm], 8);
            if (Q) memcpy(vm + 8, &cpu.v_hi[rm], 8);
            for (int i = 0; i < elems; i++) {
                uint64_t a = 0, b = 0;
                memcpy(&a, vn + i * esize, esize);
                memcpy(&b, vm + i * esize, esize);
                // Arm computes the sum/difference exactly (Int(a) op
                // Int(b) [+1]) and shifts arithmetically before
                // truncating: UHSUB 0 - 3 is -3 >> 1 = -2 → 0xFE.
                const int sh = 64 - esize * 8;
                const int64_t sa = is_unsigned ? static_cast<int64_t>(a)
                    : static_cast<int64_t>(a << sh) >> sh;
                const int64_t sb = is_unsigned ? static_cast<int64_t>(b)
                    : static_cast<int64_t>(b << sh) >> sh;
                int64_t sum = is_sub ? (sa - sb) : (sa + sb);
                if (is_rhadd) sum += 1;
                uint64_t r = static_cast<uint64_t>(sum >> 1);
                memcpy(out + i * esize, &r, esize);
            }
            memcpy(&cpu.v_lo[rd], out, 8);
            if (Q) memcpy(&cpu.v_hi[rd], out + 8, 8);
            else cpu.v_hi[rd] = 0;
            return true;
        }
        // ── FP by-element (FMUL/FMLA/FMLS/FMULX × element) ──
        // FMUL Vd.4S, Vn.4S, Vm.S[index]: multiply Vn by the scalar
        // lane of Vm selected by the index field.
        // Encoding: 0 Q 0 11111 size L M Rm 1 opc[2:0] H Rn Rd
        // sub3_noq (mask 0xFF00F400, strips L/M/H index bits) =
        //   FMLA 0x0F001000  FMLS 0x0F005000
        //   FMUL 0x0F009000  FMULX 0x2F009000 (U=1)
        // size: 1=H(16b), 2=S(32b), 3=D(64b). The element index is
        // formed from H:L:M: D→{H}, S→{H:L}, H→{H:L:M}.
        {
            uint32_t be = (op & 0xFF00F400) & ~(1u << 30);
            if ((be == 0x0F001000 || be == 0x0F005000 ||
                 be == 0x0F009000 || be == 0x2F009000) && size >= 2) {
                int esize = 1 << size;   // 2, 4, or 8
                int lanes = (Q ? 16 : 8) / esize;
                uint32_t H = (op >> 11) & 1;
                uint32_t L = (op >> 21) & 1;
                uint32_t M = (op >> 20) & 1;
                int idx;
                if (size == 3) idx = H;
                else if (size == 2) idx = (H << 1) | L;
                else idx = (H << 2) | (L << 1) | M;
                int opc = be == 0x2F009000 ? 13 : (op >> 12) & 0xF; // FMLA/FMLS/FMUL/FMULX
                const bool dn = (cpu.fpcr >> 25) & 1;
                uint8_t vn[16], vm[16];
                memcpy(vn, &cpu.v_lo[rn], 8);
                if (Q) memcpy(vn + 8, &cpu.v_hi[rn], 8);
                memcpy(vm, &cpu.v_lo[rm], 8);
                memcpy(vm + 8, &cpu.v_hi[rm], 8);
                if (esize == 8) {
                    double sc;
                    memcpy(&sc, vm + idx * 8, 8);
                    // Snapshot Vd (FMLA/FMLS accumulate into it).
                    uint64_t dlo0 = cpu.v_lo[rd], dhi0 = cpu.v_hi[rd];
                    for (int i = 0; i < lanes; i++) {
                        double a, r;
                        memcpy(&a, vn + i * 8, 8);
                        double d;
                        memcpy(&d, (i == 0) ? &dlo0 : &dhi0, 8);
                        if (opc == 9 || opc == 13) {
                            r = arm_fp_binop(opc == 13 ? ArmFpOp::MulX : ArmFpOp::Mul, a, sc, dn);
                        } else {
                            r = arm_fp_muladd(d, opc == 1 ? a : fp_neg(a), sc, dn);
                        }
                        if (i == 0) memcpy(&cpu.v_lo[rd], &r, 8);
                        else memcpy(&cpu.v_hi[rd], &r, 8);
                    }
                } else if (esize == 4) {
                    float sc;
                    memcpy(&sc, vm + idx * 4, 4);
                    // Snapshot Vd (FMLA/FMLS accumulate into it).
                    uint64_t dlo0 = cpu.v_lo[rd], dhi0 = cpu.v_hi[rd];
                    for (int i = 0; i < lanes; i++) {
                        float a, r;
                        memcpy(&a, vn + i * 4, 4);
                        uint64_t chunk = (i < 2) ? dlo0 : dhi0;
                        int sh = (i & 1) * 32;
                        float d;
                        uint32_t db = (chunk >> sh) & 0xFFFFFFFF;
                        memcpy(&d, &db, 4);
                        if (opc == 9 || opc == 13) {
                            r = arm_fp_binop(opc == 13 ? ArmFpOp::MulX : ArmFpOp::Mul, a, sc, dn);
                        } else {
                            r = arm_fp_muladd(d, opc == 1 ? a : fp_neg(a), sc, dn);
                        }
                        uint32_t rb;
                        memcpy(&rb, &r, 4);
                        uint64_t* chunkp = (i < 2) ? &cpu.v_lo[rd] : &cpu.v_hi[rd];
                        *chunkp = (*chunkp & ~(0xFFFFFFFFULL << sh))
                                | (static_cast<uint64_t>(rb) << sh);
                    }
                }
                if (!Q) cpu.v_hi[rd] = 0;
                return true;
            }
        }
        // ── FABS / FNEG / FSQRT (FP unary, vector) ──
        // FABS Vd.<T>, Vn.<T>: clear sign bit. FNEG: flip sign bit.
        // Bit 23 is fixed; bit 22 selects S/D. FSQRT sets bit 16,
        // while FABS/FNEG clear it. Q selects 64/128-bit output.
        {
            // Keep bit16: FSQRT differs from FNEG only by this bit.
            // Strip Q and the S/D selector, retaining the fixed size bit.
            uint32_t fs = op & 0xBFBFFC00;
            if (fs == 0x0EA0F800 || fs == 0x2EA0F800 || fs == 0x2EA1F800) {
                bool is_neg = fs == 0x2EA0F800;
                bool is_sqrt = fs == 0x2EA1F800;
                int esize = 1 << size;
                int lanes = (Q ? 16 : 8) / esize;
                uint8_t vn[16];
                memcpy(vn, &cpu.v_lo[rn], 8);
                if (Q) memcpy(vn + 8, &cpu.v_hi[rn], 8);
                for (int i = 0; i < lanes; i++) {
                    uint64_t v = 0;
                    memcpy(&v, vn + i * esize, esize);
                    if (is_sqrt) {
                        if (esize == 8) {
                            double x, r;
                            memcpy(&x, &v, 8);
                            r = arm_fp_sqrt(x, (cpu.fpcr >> 25) & 1);
                            memcpy(&v, &r, 8);
                        } else {
                            float x, r;
                            memcpy(&x, &v, 4);
                            r = arm_fp_sqrt(x, (cpu.fpcr >> 25) & 1);
                            memcpy(&v, &r, 4);
                        }
                    } else if (is_neg) v ^= (1ULL << (esize * 8 - 1));
                    else v &= ~(1ULL << (esize * 8 - 1));
                    if (esize == 8) {
                        if (i == 0) memcpy(&cpu.v_lo[rd], &v, 8);
                        else memcpy(&cpu.v_hi[rd], &v, 8);
                    } else {
                        uint64_t* chunkp = (i < 2) ? &cpu.v_lo[rd] : &cpu.v_hi[rd];
                        int sh = (i & 1) * 32;
                        uint32_t rb = static_cast<uint32_t>(v);
                        *chunkp = (*chunkp & ~(0xFFFFFFFFULL << sh))
                                | (static_cast<uint64_t>(rb) << sh);
                    }
                }
                if (!Q) cpu.v_hi[rd] = 0;
                return true;
            }
        }
        // ── SCVTF / UCVTF (vector, integer → float, 2-operand) ──
        // SCVTF Vd.<T>, Vn.<T>: signed int → FP. UCVTF: unsigned.
        // Encoding (3-same, 2-operand): 0 Q U 0 1110 size 1 0 0000
        // 1111 1 0 Rn Rd. sub3_noq: SCVTF=0x0E20D800 (size 0/1),
        // UCVTF=0x2E20D800 (size 0/1). size=0 → 4S/2S (32-bit ints),
        // size=1 → 2D/1D (64-bit ints). Note FRECPE/FRSQRTE share
        // these sub3_noq values but use size=2/3, so `size < 2` here.
        // Bits 20:16 must be 00001 (bit 16 clear is unallocated), and
        // the 1D form is reserved.
        {
            uint32_t cv = (op & 0xFF20FC00) & ~(1u << 30);
            if ((cv == 0x0E20D800 || cv == 0x2E20D800) && size < 2 &&
                ((op >> 16) & 0x1F) == 1) {
                bool is_unsigned = cv == 0x2E20D800;
                bool is_double = size == 1;   // 2D output
                if (is_double && !Q) throw DecodeError(cpu.pc, op);
                int lanes = is_double ? (Q ? 2 : 1) : (Q ? 4 : 2);
                uint8_t vn[16];
                memcpy(vn, &cpu.v_lo[rn], 8);
                if (Q) memcpy(vn + 8, &cpu.v_hi[rn], 8);
                if (is_double) {
                    for (int i = 0; i < lanes; i++) {
                        int64_t sv;
                        memcpy(&sv, vn + i * 8, 8);
                        double r = is_unsigned ? static_cast<double>(static_cast<uint64_t>(sv))
                                               : static_cast<double>(sv);
                        if (i == 0) memcpy(&cpu.v_lo[rd], &r, 8);
                        else memcpy(&cpu.v_hi[rd], &r, 8);
                    }
                } else {
                    for (int i = 0; i < lanes; i++) {
                        int32_t sv;
                        memcpy(&sv, vn + i * 4, 4);
                        float r = is_unsigned ? static_cast<float>(static_cast<uint32_t>(sv))
                                              : static_cast<float>(sv);
                        uint32_t rb;
                        memcpy(&rb, &r, 4);
                        uint64_t* chunkp = (i < 2) ? &cpu.v_lo[rd] : &cpu.v_hi[rd];
                        int sh = (i & 1) * 32;
                        *chunkp = (*chunkp & ~(0xFFFFFFFFULL << sh))
                                | (static_cast<uint64_t>(rb) << sh);
                    }
                }
                if (!Q) cpu.v_hi[rd] = 0;
                return true;
            }
        }
        // FADD/FSUB/FMUL/FDIV (vector, float) — 0x0E20D400 (FADD 4S)
        // and 0x0E20DC00 (FMUL 4S).
        // These use the FP arithmetic encoding with Q form.
        // For correctness with musl's printf path:
        if ((op & 0xBF20FC00) == 0x0E20D400) {
            // FADD (vector) — fadd v0.4s, v1.4s, v2.4s
            if (size == 0) { // 4S (32-bit float)
                float* fn = reinterpret_cast<float*>(&cpu.v_lo[rn]);
                float* fm = reinterpret_cast<float*>(&cpu.v_lo[rm]);
                float* fd = reinterpret_cast<float*>(&cpu.v_lo[rd]);
                float fn_hi[2], fm_hi[2], fd_hi[2];
                if (Q) {
                    memcpy(fn_hi, &cpu.v_hi[rn], 8);
                    memcpy(fm_hi, &cpu.v_hi[rm], 8);
                }
                fd[0] = fn[0] + fm[0];
                fd[1] = fn[1] + fm[1];
                if (Q) {
                    memcpy(fd_hi, &cpu.v_hi[rd], 8);
                    fd_hi[0] = fn_hi[0] + fm_hi[0];
                    fd_hi[1] = fn_hi[1] + fm_hi[1];
                    memcpy(&cpu.v_hi[rd], fd_hi, 8);
                } else cpu.v_hi[rd] = 0;
            } else if (size == 1) { // 2D (64-bit double)
                double fn_l, fm_l, fd_l;
                memcpy(&fn_l, &cpu.v_lo[rn], sizeof(double));
                memcpy(&fm_l, &cpu.v_lo[rm], sizeof(double));
                fd_l = fn_l + fm_l;
                memcpy(&cpu.v_lo[rd], &fd_l, sizeof(double));
                if (Q) {
                    double fn_h, fm_h, fd_h;
                    memcpy(&fn_h, &cpu.v_hi[rn], sizeof(double));
                    memcpy(&fm_h, &cpu.v_hi[rm], sizeof(double));
                    fd_h = fn_h + fm_h;
                    memcpy(&cpu.v_hi[rd], &fd_h, sizeof(double));
                } else cpu.v_hi[rd] = 0;
            }
            return true;
        }
    }
    return false;
}
} // namespace arm64emu
