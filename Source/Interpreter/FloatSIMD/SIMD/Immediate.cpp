// Immediate.cpp — SIMD_DP group 3: modified immediate
// (MOVI/MVNI/ORR/BIC/FMOV), shift by immediate, fixed-point conversions,
// narrowing shifts and by-element operations.
#include "Interpreter/FloatSIMD/FloatCommon.hpp"
namespace arm64emu {
bool Emulator::exec_simd_imm(const SimdDpFields& f, CPU& cpu) {
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
    // Advanced SIMD modified immediate (MOVI/MVNI/ORR/BIC + MSL).
    // Encoding: 0 Q op 01111 0 abc cmode o2 1 defgh Rd
    // Must require immh==0 (bits[22:19]) so shift-by-immediate
    // encodings (SHL/USHR/SSHR, which share bits[28:24]=01111) are
    // not stolen. Real MOVI/MVNI/ORR/BIC always have bits[22:19]==0.
    //
    // NOTE: no SHRN exclusion (bits[15:10]=100001) is needed here:
    // every VALID SHRN has immh (bits[22:19]) >= 1 (its immh:immb
    // field encodes the source size + shift), so the bits[22:19]==0
    // test already separates SHRN from this block. A crude
    // `bits[15:10] != 0x21` clause instead REJECTED cmode=8 MOVI
    // (16-bit LSL #0), whose bits[15:10] = 1000 o2 1 = 100001 == the
    // SHRN opcode, silently zeroing `movi vN.4h, #imm` in both the
    // interp and the JIT (0x0F058560 = movi v6.4h, #0xab returned 0).
    //
    // cmode/op decode per ARM ARM:
    //   0xx0 op=0/1 → MOVI/MVNI 32-bit LSL #(cmode[2:1]*8)
    //   0xx1 op=0/1 → ORR /BIC  32-bit LSL #(cmode[2:1]*8)
    //   10x0 op=0/1 → MOVI/MVNI 16-bit LSL #(cmode[1]*8)
    //   10x1 op=0/1 → ORR /BIC  16-bit LSL #(cmode[1]*8)
    //   110x op=0/1 → MOVI/MVNI 32-bit MSL #((cmode[0]+1)*8)
    //   1110 op=0   → MOVI 8-bit (replicate imm8 to every byte)
    //   1110 op=1   → MOVI 64-bit (each imm8 bit → 0x00/0xFF byte)
    //   1111        → FMOV (handled elsewhere / reserved here)
    // Use immh = bits[22:19] (not [23:20]): SSHLL/SHRN keep bit23=0
    // so bits[23:20] can look like "immh==0" while bits[22:19]!=0.
    // Real MOVI/MVNI/ORR/BIC always have bits[22:19]==0.
    if (((op & ~((1u << 30) | (1u << 29))) & 0xFF800C00) == 0x0F000400
        && ((op >> 19) & 0xF) == 0) {
        uint8_t cmode = (op >> 12) & 0xF;
        uint8_t imm8 = static_cast<uint8_t>(
            (((op >> 16) & 0x7) << 5) | ((op >> 5) & 0x1F));
        bool opbit = (op >> 29) & 1;  // 'op' in ARM encoding
        uint8_t dst[16];
        memcpy(dst, &cpu.v_lo[rd], 8);
        memcpy(dst + 8, &cpu.v_hi[rd], 8);

        auto replicate_u32 = [&](uint32_t lane) {
            int n = Q ? 4 : 2;
            for (int i = 0; i < n; i++)
                memcpy(dst + i * 4, &lane, 4);
            if (!Q) memset(dst + 8, 0, 8);
        };
        auto replicate_u16 = [&](uint16_t lane) {
            int n = Q ? 8 : 4;
            for (int i = 0; i < n; i++)
                memcpy(dst + i * 2, &lane, 2);
            if (!Q) memset(dst + 8, 0, 8);
        };
        auto apply_or_bic_u32 = [&](uint32_t imm, bool bic) {
            int n = Q ? 4 : 2;
            for (int i = 0; i < n; i++) {
                uint32_t v;
                memcpy(&v, dst + i * 4, 4);
                v = bic ? (v & ~imm) : (v | imm);
                memcpy(dst + i * 4, &v, 4);
            }
            if (!Q) memset(dst + 8, 0, 8);
        };
        auto apply_or_bic_u16 = [&](uint16_t imm, bool bic) {
            int n = Q ? 8 : 4;
            for (int i = 0; i < n; i++) {
                uint16_t v;
                memcpy(&v, dst + i * 2, 2);
                v = bic ? static_cast<uint16_t>(v & ~imm)
                        : static_cast<uint16_t>(v | imm);
                memcpy(dst + i * 2, &v, 2);
            }
            if (!Q) memset(dst + 8, 0, 8);
        };

        if ((cmode & 0x9) == 0x0) {
            // 0xx0: MOVI/MVNI 32-bit LSL
            uint32_t imm = static_cast<uint32_t>(imm8)
                           << (((cmode >> 1) & 3) * 8);
            if (opbit) imm = ~imm;
            replicate_u32(imm);
        } else if ((cmode & 0x9) == 0x1) {
            // 0xx1: ORR/BIC 32-bit LSL
            uint32_t imm = static_cast<uint32_t>(imm8)
                           << (((cmode >> 1) & 3) * 8);
            apply_or_bic_u32(imm, opbit);
        } else if ((cmode & 0xD) == 0x8) {
            // 10x0: MOVI/MVNI 16-bit LSL
            uint16_t imm = static_cast<uint16_t>(
                static_cast<uint16_t>(imm8) << ((cmode & 2) ? 8 : 0));
            if (opbit) imm = static_cast<uint16_t>(~imm);
            replicate_u16(imm);
        } else if ((cmode & 0xD) == 0x9) {
            // 10x1: ORR/BIC 16-bit LSL
            uint16_t imm = static_cast<uint16_t>(
                static_cast<uint16_t>(imm8) << ((cmode & 2) ? 8 : 0));
            apply_or_bic_u16(imm, opbit);
        } else if ((cmode & 0xE) == 0xC) {
            // 110x: MOVI/MVNI 32-bit MSL #8 or #16
            int shift = ((cmode & 1) + 1) * 8;
            uint32_t ones = (shift == 16) ? 0xFFFFu : 0xFFu;
            uint32_t imm = (static_cast<uint32_t>(imm8) << shift) | ones;
            if (opbit) imm = ~imm;
            replicate_u32(imm);
        } else if (cmode == 0xE && !opbit) {
            // MOVI 8-bit: replicate imm8 across all bytes
            uint64_t val = 0;
            for (int i = 0; i < 8; i++)
                val |= static_cast<uint64_t>(imm8) << (i * 8);
            cpu.v_lo[rd] = val;
            cpu.v_hi[rd] = Q ? val : 0;
            return true;
        } else if (cmode == 0xE && opbit) {
            // MOVI 64-bit: each imm8 bit selects 0x00 or 0xFF byte
            uint64_t val = 0;
            for (int i = 0; i < 8; i++) {
                if (imm8 & (1u << i))
                    val |= 0xFFULL << (i * 8);
            }
            cpu.v_lo[rd] = val;
            cpu.v_hi[rd] = Q ? val : 0;
            return true;
        } else {
            // cmode=0xF FMOV (vector, immediate): floating-point
            // constant.  AdvSIMDExpandImm; op bit picks precision:
            //   op=1 → 64-bit double, replicated to .2D (Q=1)
            //   op=0 → 32-bit single, replicated to .2S/.4S
            if (opbit) {
                uint64_t val = (static_cast<uint64_t>(imm8 & 0x3F)) << 48;
                if (imm8 & 0x80) val |= 0x8000000000000000ULL;   // sign
                if (imm8 & 0x40) val |= 0x3FC0000000000000ULL;   // exp
                else             val |= 0x4000000000000000ULL;
                cpu.v_lo[rd] = val;
                cpu.v_hi[rd] = Q ? val : 0;
            } else {
                uint32_t val = (static_cast<uint32_t>(imm8 & 0x3F)) << 19;
                if (imm8 & 0x80) val |= 0x80000000u;             // sign
                if (imm8 & 0x40) val |= 0x3E000000u;             // exp[7:2] = 0:11111
                else             val |= 0x40000000u;
                replicate_u32(val);
                memcpy(&cpu.v_lo[rd], dst, 8);
                memcpy(&cpu.v_hi[rd], dst + 8, 8);
                return true;
            }
            return true;
        }
        memcpy(&cpu.v_lo[rd], dst, 8);
        memcpy(&cpu.v_hi[rd], dst + 8, 8);
        return true;
    }
    // Shift by immediate (SSHR/USHR, SSRA/USRA, SRSHR/URSHR, SRSRA/URSRA,
    // SRI, SHL/SLI, SQSHLU, SQSHL/UQSHL) shares one decoder and executor
    // with the scalar forms (FloatCommon.hpp). The per-opcode handlers it
    // replaced each had their own size table; several left the .2d forms
    // as silent no-ops.
    if (arm_simd_shift_imm(op, cpu)) return true;
    // SCVTF/UCVTF/FCVTZS/FCVTZU (vector, fixed-point) — QEMU
    // a64.decode *_vf: 0 Q U 011110 immh immb 111001 / 111111 Rn Rd.
    // immh 01xx = .s (fbits = 64 - immh:immb), 1xxx = .d (fbits =
    // 128 - immh:immb, Q=0 reserved). 001x is half precision, which
    // needs FEAT_FP16 (not advertised), so it stays UNDEF; immh 0000
    // is the modified-immediate group above. Teeworlds' map editor
    // runs `fcvtzs v31.2s, v30.2s, #10`.
    if ((op & 0x9F80E400) == 0x0F00E400
        && (((op >> 11) & 3) == 0 || ((op >> 11) & 3) == 3)
        && ((op >> 19) & 0xC) != 0 && (((op >> 22) & 1) == 0 || Q)) {
        bool is_unsigned = (op >> 29) & 1;
        bool to_int = ((op >> 11) & 3) == 3;
        bool is_double = (op >> 22) & 1;
        int fbits = (is_double ? 128 : 64) - static_cast<int>((op >> 16) & 0x7F);
        int esize = is_double ? 8 : 4;
        int elems = (Q ? 16 : 8) / esize;
        uint8_t buf[16];
        memcpy(buf, &cpu.v_lo[rn], 8);
        memcpy(buf + 8, &cpu.v_hi[rn], 8);
        for (int i = 0; i < elems; i++) {
            if (is_double) {
                uint64_t v; memcpy(&v, buf + i*8, 8);
                if (to_int) {
                    double f; memcpy(&f, &v, 8);
                    double scaled = std::ldexp(f, fbits);
                    v = is_unsigned ? fp_to_unsigned_sat(scaled, true)
                                    : static_cast<uint64_t>(fp_to_signed_sat(scaled, true));
                } else {
                    double f = is_unsigned ? static_cast<double>(v)
                                           : static_cast<double>(static_cast<int64_t>(v));
                    f = std::ldexp(f, -fbits);
                    memcpy(&v, &f, 8);
                }
                memcpy(buf + i*8, &v, 8);
            } else {
                uint32_t v; memcpy(&v, buf + i*4, 4);
                if (to_int) {
                    float f; memcpy(&f, &v, 4);
                    double scaled = std::ldexp(static_cast<double>(f), fbits);
                    v = is_unsigned ? static_cast<uint32_t>(fp_to_unsigned_sat(scaled, false))
                                    : static_cast<uint32_t>(fp_to_signed_sat(scaled, false));
                } else {
                    float f = is_unsigned ? static_cast<float>(v)
                                          : static_cast<float>(static_cast<int32_t>(v));
                    f = std::ldexp(f, -fbits);
                    memcpy(&v, &f, 4);
                }
                memcpy(buf + i*4, &v, 4);
            }
        }
        memcpy(&cpu.v_lo[rd], buf, 8);
        if (Q) memcpy(&cpu.v_hi[rd], buf + 8, 8);
        else cpu.v_hi[rd] = 0;
        return true;
    }
    // Narrowing shift right (SHRN). immh determines SOURCE element size
    // (2× the destination size).
    //
    // wrong. Verified empirically by compiling `shrn v0.8b, v0.8h, #4`
// (0x0f0c8400), `shrn v0.4h, v0.4s, #4` (0x0f1c8400), and
    // `shrn v0.2s, v0.2d, #4` (0x0f3c8400):
    //   immh=0 → esize=2 (16-bit source), immh=1 → esize=4 (32-bit),
    //   immh=2,3 → esize=8 (64-bit)
    //   shift = esize*8 - (immh:immb)
    // (16-bit source), and used `2*esize*8 - immh:immb` for the shift
    // (off by esize*8). This produced wrong shift amounts, corrupting
    // the narrowed result. With immh=0, esize was set to 8 (64-bit)
    // instead of 2 (16-bit), and shift = 128-12 = 116 instead of 4.
    //
    // Q register), even when Q=0. Q=0 means the destination is 64
    // bits (SHRN), Q=1 means 128 bits (SHRN2, writes to upper half).
    if ((op & 0xBF00FC00) == 0x0F008400) {
        // The architectural immh is bits[22:19]; 1xxx (a 64-bit result)
        // is reserved. The fields below are shifted views of it.
        if ((op >> 22) & 1) throw DecodeError(cpu.pc, op);
        uint8_t immh = (op >> 20) & 0xF;
        uint8_t immb = (op >> 16) & 0xF;
        int esize, shift;
        if (immh == 0) { esize = 2; }       // 16-bit source → 8-bit dest
        else if (immh == 1) { esize = 4; }   // 32-bit source → 16-bit dest
        else { esize = 8; }                   // 64-bit source → 32-bit dest
        shift = (esize * 8) - ((immh << 4) | immb);
        if (shift < 0) shift = 0;  // sanity: shift can't be negative
        // Source is ALWAYS the full 128-bit register (8/4/2 elements).
        uint8_t buf[16];
        memcpy(buf, &cpu.v_lo[rn], 8);
        memcpy(buf + 8, &cpu.v_hi[rn], 8);  // always read full 128 bits
        int dst_elems = 16 / esize;  // 8 for 16-bit, 4 for 32-bit, 2 for 64-bit
        uint8_t out[16] = {0};
        for (int i = 0; i < dst_elems; i++) {
            uint64_t v = 0;
            memcpy(&v, buf + i * esize, esize);
            v >>= shift;
            memcpy(out + i * (esize / 2), &v, esize / 2);
        }
        if (Q) {
            // SHRN2: write to upper 64 bits, preserve lower 64 bits.
            memcpy(&cpu.v_hi[rd], out, 8);
        } else {
            // SHRN: write to lower 64 bits, zero upper 64 bits.
            memcpy(&cpu.v_lo[rd], out, 8);
            cpu.v_hi[rd] = 0;
        }
        return true;
    }
    // SSHLL / USHLL / SSHLL2 / USHLL2 — widen + left shift.
    // Encoding: 0 Q U 01111 0 immh[3:0] immb[2:0] 101001 Rn Rd
    // immh is bits[22:19], immb bits[18:16] (bit23 always 0).
    // Using bits[23:20] wrongly sees immh=0 for 8-bit sources.
    if (((op & ~((1u << 30) | (1u << 29))) & 0xFF00FC00) == 0x0F00A400) {
        uint8_t immh = (op >> 19) & 0xF;   // bits[22:19]
        uint8_t immb = (op >> 16) & 0x7;   // bits[18:16]
        if (immh >= 8) throw DecodeError(cpu.pc, op);  // 64-bit sources are reserved
        if (immh != 0) {
        bool is_unsigned = (op >> 29) & 1;
        int src_esize, shift;
        if (immh == 1) { src_esize = 1; }
        else if (immh <= 3) { src_esize = 2; }
        else { src_esize = 4; }
        int dst_esize = src_esize * 2;
        shift = ((immh << 3) | immb) - (src_esize * 8);
        if (shift < 0) shift = 0;
        uint8_t src[16];
        memcpy(src, &cpu.v_lo[rn], 8);
        memcpy(src + 8, &cpu.v_hi[rn], 8);
        int elems = 8 / src_esize;  // always 8/4/2 from a 64-bit half
        int src_base = Q ? 8 : 0;
        uint8_t out[16] = {0};
        for (int i = 0; i < elems; i++) {
            uint64_t v = 0;
            memcpy(&v, src + src_base + i * src_esize, src_esize);
            if (!is_unsigned) {
                // Sign-extend from src_esize bytes.
                int bits = src_esize * 8;
                int64_t sv = static_cast<int64_t>(v << (64 - bits)) >> (64 - bits);
                v = static_cast<uint64_t>(sv);
            }
            v <<= shift;
            memcpy(out + i * dst_esize, &v, dst_esize);
        }
        memcpy(&cpu.v_lo[rd], out, 8);
        memcpy(&cpu.v_hi[rd], out + 8, 8);
        return true;
        }
    }
    // ── Saturating / rounded narrowing shift-by-immediate ───────
    // Encoding: 0 Q U 011111 immh immb opcode Rn Rd (imm space,
    // bits[28:24]=01111). Opcode (bits[15:10]):
    //   100001 SQSHRUN (U=1)   100011 SQRSHRUN (U=1) / RSHRN (U=0)
    //   100101 SQSHRN (U=0) / UQSHRN (U=1)
    //   100111 SQRSHRN (U=0) / UQRSHRN (U=1)
    // Shift right by (source_bits − immh:immb), saturate, and
    // narrow to half-width lanes. Q selects the destination half.
    // libjpeg-turbo's IDCT tail does `sqrshrn v1.8b, v1.8h, #2`.
    // MUST be tested before the by-element block below: both live
    // in the 01111 space and SQSHRUN's bits[15:12] (1000) collides
    // with by-element MUL.
    {
        uint32_t opc6 = (op >> 10) & 0x3F;
        if (((op >> 24) & 0x1F) == 0x0F &&
            (opc6 == 0x21 || opc6 == 0x23 || opc6 == 0x25 || opc6 == 0x27)) {
            bool U = (op >> 29) & 1;
            bool round = (opc6 & 0x2) != 0;
            bool saturating = (opc6 & 0x4) || U;
            bool unsigned_src = !saturating || ((opc6 & 0x4) && U);
            bool unsigned_dst = U;
            uint32_t immh = (op >> 19) & 0xF;
            uint32_t immb = (op >> 16) & 7;
            // immh describes the NARROW destination lanes. The source
            // is always twice that width; Q selects the output half,
            // never the number of input lanes.
            if (immh == 0 || immh >= 8)
                throw DecodeError(cpu.pc, op);
            int dst_esize = immh >= 4 ? 4 : immh >= 2 ? 2 : 1;
            int src_esize = dst_esize * 2;
            int src_bits = src_esize * 8;
            int dst_bits = dst_esize * 8;
            int shift = src_bits - (int)((immh << 3) | immb);
            uint8_t buf_n[16];
            memcpy(buf_n, &cpu.v_lo[rn], 8);
            memcpy(buf_n + 8, &cpu.v_hi[rn], 8);
            uint8_t out[8];
            for (int i = 0; i < 8 / dst_esize; i++) {
                uint64_t a = 0;
                memcpy(&a, buf_n + i * src_esize, src_esize);
                // Widen before rounding: even UINT64_MAX + round
                // must not wrap. Signed inputs need sign extension.
                __int128 value = unsigned_src ? (__int128)a :
                    (__int128)((int64_t)(a << (64 - src_bits)) >> (64 - src_bits));
                if (round) value += (__int128)1 << (shift - 1);
                value >>= shift;
                __int128 lo = unsigned_dst ? 0 : -((__int128)1 << (dst_bits - 1));
                __int128 hi = unsigned_dst ? (((__int128)1 << dst_bits) - 1) :
                    (((__int128)1 << (dst_bits - 1)) - 1);
                if (saturating && value < lo) { value = lo; cpu.fpsr |= 1u << 27; }
                if (saturating && value > hi) { value = hi; cpu.fpsr |= 1u << 27; }
                uint64_t r = (uint64_t)value;
                memcpy(out + i * dst_esize, &r, dst_esize);
            }
            if (Q) {
                memcpy(&cpu.v_hi[rd], out, 8);
            } else {
                memcpy(&cpu.v_lo[rd], out, 8);
                cpu.v_hi[rd] = 0;
            }
            return true;
        }
    }
    // ── Integer by-element (vector x indexed element) ─────────
    // Encoding: 0 Q U 01111 size L M Rm opcode H 0 Rn Rd
    // (bits[28:24]=01111 — the "by element" space, distinct from
    // the three-same 01110 space the sub3 chain below handles).
    // Index bits: H=bit11, L=bit21, M=bit20; for words M is the top
    // Rm bit. Lane sizes: size=01 → H src (.h[idx]), size=10
    // → S src (.s[idx]). Widening ops (SMULL/UMULL/SMLAL/UMLAL/
    // SMLSL/UMLSL) double the lane width; MUL/MLA/MLS keep it.
    // Opcode map (verified against the cross assembler):
    //   0000 MLA(U=1)  0100 MLS(U=1)  0010 SMLAL/UMLAL
    //   0110 SMLSL/UMLSL  1000 MUL(U=0)  1010 SMULL/UMULL
    // (MLS is 0100, not 0001 — opc 0001/U=0 is FMLSL, an FP16 op
    // that must NOT execute as integer MLS.)
    // libjpeg-turbo's AArch64 IDCT does `smull v18.4s, v7.4h,
    // v6.h[3]` / `smlal …` — without this the interp SIGILL'd
    // during JPEG decode.
    // By-element encodings have bit 10 clear (bit 10 set is the shift and
    // modified-immediate space).
    if (((op >> 24) & 0x1F) == 0x0F && !(op & 0x400)) {
        const uint32_t opc = (op >> 12) & 0xF;
        const bool U = (op >> 29) & 1;
        const uint32_t sz = (op >> 22) & 3;
        const int H = (op >> 11) & 1, L = (op >> 21) & 1, M = (op >> 20) & 1;
        // Allocated integer forms: U=1 MLA 0000, MLS 0100; U=0 MUL 1000,
        // SQDMULH 1100, SQRDMULH 1101, SQDMLAL 0011, SQDMLSL 0111, SQDMULL
        // 1011; either U: [SU]MLAL 0010, [SU]MLSL 0110, [SU]MULL 1010. The
        // other opcodes are FP, FHM, dot-product or RDM forms and fall
        // through (U=0 0000 is FMLAL, not MLA; 1110 is SDOT, not SQDMULL).
        const bool doubling = !U && (opc == 0x3 || opc == 0x7 || opc == 0xB);
        const bool known = (U && (opc == 0x0 || opc == 0x4)) ||
            (!U && (opc == 0x8 || opc == 0xC || opc == 0xD)) ||
            opc == 0x2 || opc == 0x6 || opc == 0xA || doubling;
        if (known) {
            if (sz != 1 && sz != 2) throw DecodeError(cpu.pc, op);
            // Halfword elements: Vm is v0-v15 (bits 19:16), index H:L:M.
            // Word elements: Vm is M:Rm (v0-v31), index H:L. Using the
            // 4-bit form for words read the wrong register when M was set.
            const uint32_t rm_reg = sz == 1 ? (op >> 16) & 0xF : (op >> 16) & 0x1F;
            const int esize_src = sz == 1 ? 2 : 4;
            const int idx = sz == 1 ? (H << 2) | (L << 1) | M : (H << 1) | L;
            const bool widen = opc == 0x2 || opc == 0x6 || opc == 0xA || doubling;
            const bool is_sub = opc == 0x4 || opc == 0x6 || opc == 0x7;
            const bool accum = opc == 0x0 || opc == 0x4 || opc == 0x2 || opc == 0x6 ||
                               opc == 0x3 || opc == 0x7;
            const int esize = widen ? esize_src * 2 : esize_src;
            // Non-widening Q selects a 64- or 128-bit destination. Widening
            // forms produce 128 bits; Q selects the source half (the `2`
            // forms such as SMULL2 in libjpeg's IDCT).
            const int elems = widen ? 8 / esize_src : (Q ? 16 : 8) / esize;
            const int src_base = (widen && Q) ? 8 : 0;
            const int bw = esize * 8;
            const uint64_t mask = esize == 8 ? ~0ULL : ((1ULL << bw) - 1);
            uint8_t buf_n[16], buf_m[16], vd[16];
            memcpy(buf_n, &cpu.v_lo[rn], 8);
            memcpy(buf_n + 8, &cpu.v_hi[rn], 8);
            memcpy(buf_m, &cpu.v_lo[rm_reg], 8);
            memcpy(buf_m + 8, &cpu.v_hi[rm_reg], 8);
            if (accum) {
                memcpy(vd, &cpu.v_lo[rd], 8);
                memcpy(vd + 8, &cpu.v_hi[rd], 8);
            } else {
                memset(vd, 0, sizeof(vd));
            }
            uint64_t mul_v = 0;
            memcpy(&mul_v, buf_m + idx * esize_src, esize_src);
            const int src_shift = 64 - esize_src * 8;
            const int64_t smul_v = static_cast<int64_t>(mul_v << src_shift) >> src_shift;
            for (int i = 0; i < elems; i++) {
                // Source lanes are esize_src wide (widening reads narrower
                // lanes than it writes).
                uint64_t a = 0;
                memcpy(&a, buf_n + src_base + i * esize_src, esize_src);
                const int64_t sa = static_cast<int64_t>(a << src_shift) >> src_shift;
                uint64_t acc = 0;
                memcpy(&acc, vd + i * esize, esize);
                const int64_t sacc = static_cast<int64_t>(acc << (64 - bw)) >> (64 - bw);
                uint64_t r;
                if (opc == 0xC || opc == 0xD) {
                    r = static_cast<uint64_t>(arm_sqdmulh(cpu, sa, smul_v, bw, opc == 0xD));
                } else if (doubling) {
                    r = static_cast<uint64_t>(arm_sqdmlal(cpu, sacc, sa, smul_v, bw / 2,
                                                          opc == 0xB ? 0 : is_sub ? -1 : 1));
                } else {
                    const uint64_t prod = widen && !U ? (uint64_t)(sa * smul_v) : a * mul_v;
                    r = !accum ? prod : is_sub ? acc - prod : acc + prod;
                }
                r &= mask;
                memcpy(vd + i * esize, &r, esize);
            }
            if (!widen && !Q) {
                // Q=0 same-width ops zero the unused upper half.
                memset(vd + elems * esize, 0, 16 - elems * esize);
            }
            memcpy(&cpu.v_lo[rd], vd, 8);
            memcpy(&cpu.v_hi[rd], vd + 8, 8);
            return true;
        }
    }
    return false;
}
} // namespace arm64emu
