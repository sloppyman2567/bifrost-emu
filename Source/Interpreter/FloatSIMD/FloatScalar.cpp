// interp/interp_fp_scalar.cpp — FP_SCALAR: the 0x1E/0x9E scalar FP group
// and the AdvSIMD scalar forms the decoder routes there (FMOV, FADD..FDIV,
// FCMP, FCSEL, FCVT*, SCVTF/UCVTF, FRINT*, FMADD family, crypto SHA1H/SU).
#include "Interpreter/FloatSIMD/FloatCommon.hpp"
namespace arm64emu {
void Emulator::exec_fp_scalar(const DecodedInst& d, CPU& cpu) {
    uint32_t op = d.raw;
    uint8_t rn = (op >> 5) & 0x1F;
    uint8_t rd = op & 0x1F;
    uint8_t rm = (op >> 16) & 0x1F;
    uint8_t sf_val = (op >> 31) & 1;
    uint8_t ftype = (op >> 22) & 3;  // 0=S(32-bit), 1=D(64-bit), 3=H(16-bit)
    // ARMv8 Crypto Extensions (SHA1H, SHA1SU0/SU1, SHA256SU0/SU1)
    // are encoded in the 0x5Exxxxxx range, which the decoder
    // classifies as FP_SCALAR (because 0x5E has bits[28:24]=11110
    // matching the FP group). Dispatch them via exec_crypto first
    // — exec_crypto returns false for non-crypto instructions, so
    // this is safe. Without this, SHA1/SHA256 schedule-update
    // instructions would be silently NOP'd, producing wrong hashes.
    if (exec_crypto(op, cpu)) return;
    if (exec_reciprocal(op, cpu)) return;
    // AdvSIMD scalar integer compare (Dd, Dn, Dm), used by Skia bounds.
    // Read both operands before writing Rd; scalar results clear the high half.
    const uint32_t compare = op & 0xFFE0FC00;
    if (compare == 0x5EE03400 || compare == 0x5EE03C00 ||
        compare == 0x7EE03400 || compare == 0x7EE03C00) {
        const uint64_t a = cpu.v_lo[rn], b = cpu.v_lo[rm];
        const bool ge = (op >> 11) & 1, uns = (op >> 29) & 1;
        bool result = uns ? (ge ? a >= b : a > b)
                         : (ge ? int64_t(a) >= int64_t(b) : int64_t(a) > int64_t(b));
        cpu.v_lo[rd] = result ? ~0ULL : 0;
        cpu.v_hi[rd] = 0;
        return;
    }
    // ── SHL (scalar, immediate): Dd, Dn, #imm ─────────────────
    // Encoding: 0x5F005400 (mask 0xFF00FC00).
    // silently NOP'd, breaking GCC -O2's vectorized TLS init
    // pattern (shl d31, d31, #2 to multiply by 4).
    if ((op & 0xFF00FC00) == 0x5F005400) {
        uint8_t immh = (op >> 20) & 0xF;
        uint8_t immb = (op >> 16) & 0xF;
        int shift = ((immh << 4) | immb) - 64;
        if (shift < 0) shift = 0;
        uint64_t v = cpu.v_lo[rn];
        if (shift >= 64) v = 0;  // shift == 64 clears (avoids UB `<< 64`)
        else v <<= shift;
        cpu.v_lo[rd] = v;
        cpu.v_hi[rd] = 0;
        return;
    }
    // ── USHR (scalar, immediate): Dd, Dn, #imm ────────────────
    // Encoding: 0x7F000400 (mask 0xFF00FC00).
    if ((op & 0xFF00FC00) == 0x7F000400) {
        uint8_t immh = (op >> 20) & 0xF;
        uint8_t immb = (op >> 16) & 0xF;
        int shift = 128 - ((immh << 4) | immb);
        if (shift < 0) shift = 0;
        if (shift >= 64) cpu.v_lo[rd] = 0;
        else cpu.v_lo[rd] = cpu.v_lo[rn] >> shift;
        cpu.v_hi[rd] = 0;
        return;
    }
    // ── SSHR (scalar, immediate, signed): Dd, Dn, #imm ────────
    // Encoding: 0x5F000400 (mask 0xFF00FC00). Scalar USHR is the
    // unsigned twin with mask 0x7F000400 (handled above). The old
    // mask 0x7F000000 never matched the real SSHR encodings
    // (e.g. `sshr d9, d8, #32` = 0x5f600509), silently NOP'ing
    // the shift and breaking raster/geometry code.
    if ((op & 0xFF00FC00) == 0x5F000400) {
        uint8_t immh = (op >> 20) & 0xF;
        uint8_t immb = (op >> 16) & 0xF;
        int shift = 128 - ((immh << 4) | immb);
        if (shift < 0) shift = 0;
        int64_t v = static_cast<int64_t>(cpu.v_lo[rn]);
        if (shift >= 64) cpu.v_lo[rd] = (v < 0) ? ~0ULL : 0;
        else cpu.v_lo[rd] = static_cast<uint64_t>(v >> shift);
        cpu.v_hi[rd] = 0;
        return;
    }
    // ── DUP (element → scalar FP register): MOV Sd, Vn.S[i] ──
    // Also MOV Dd, Vn.D[i]. Encoding base 0x5E000400
    // (bits[31:24]=0x5E, bit21=0, bits[15:12]=0, bits[11:10]=01).
    //   mov s11, v9.s[1]  = 0x5e0c052b   (imm5 = 0x0C = 1<<3|4)
    //   mov d11, v9.d[1]  = 0x5e18052b   (imm5 = 0x18 = 1<<4|8)
    // Copies a single element of Vn into the scalar FP register Rd
    // (upper 64 bits zeroed). Qt's raster/bezier code pulls lane
    // values out of SIMD accumulators with this; without it the
    // instruction was silently NOP'd.
    if ((op & 0xFF20FC00) == 0x5E000400) {
        uint8_t imm5 = (op >> 16) & 0x1F;
        // The LOWEST set bit selects the element size; higher
        // bits encode the lane. For .s[1], imm5=0x0c: bit 3 is
        // the lane bit, not the size bit for a D element.
        if (!imm5 || imm5 == 16) throw DecodeError(cpu.pc, op);
        const int size_log2 = __builtin_ctz(static_cast<unsigned>(imm5));
        const int esize = 1 << size_log2;
        const int idx = imm5 >> (size_log2 + 1);
        uint64_t src_val = 0;
        int elems_per_qword = 8 / esize;
        if (idx < elems_per_qword) {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(&cpu.v_lo[rn]);
            memcpy(&src_val, p + idx * esize, esize);
        } else {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(&cpu.v_hi[rn]);
            memcpy(&src_val, p + (idx - elems_per_qword) * esize, esize);
        }
        cpu.v_lo[rd] = src_val;
        cpu.v_hi[rd] = 0;
        return;
    }
    // FP register access + half-precision helpers are now
    // file-scope functions (read_fp_d, read_fp_s, write_fp_d,
    // write_fp_s, h2f, f2h, d2h) — see top of this file.
    // FMOV (general ↔ FP, 64-bit)
    // Bit[18]=1 distinguishes FMOV from SCVTF/UCVTF (bit[18]=0).
    // Without this, SCVTF (0x9E62xxxx) matches the FMOV mask.
    // Bit[17]=1 additionally excludes FCVTAS (0x9E640020, bit17=0),
    // which would otherwise be misdecoded as a raw GPR↔FP bit copy.
    if ((op & 0xFFE0FC00) == 0x9E600000 && (op & (1u << 18))
        && (op & (1u << 17))) {
        bool to_fp = (op >> 16) & 1;
        if (to_fp) { cpu.v_lo[rd] = cpu.regs[rn]; cpu.v_hi[rd] = 0; }
        else if (rd != 31) { cpu.regs[rd] = cpu.v_lo[rn]; }
        return;
    }
    // FMOV (general ↔ FP, 32-bit)
    // Bit[18]=1 distinguishes FMOV from SCVTF/UCVTF (bit[18]=0),
    // exactly mirroring the 64-bit check above. Without this
    // guard, `scvtf s0, w0` (0x1E220000) and `ucvtf s0, w0`
    // (0x1E230000) match this mask and get misdecoded as a raw
    // GPR↔FP bit copy, producing garbage for any int→FP
    // conversion from a 32-bit GPR. This broke musl's
    // __floatscan inf/nan detection (strtod("-inf") returned
    // -nan) because the sign computation does `scvtf s1, w23`
    // with w23=-1 and expects s1=-1.0f.
    // Bit[17]=1 additionally excludes FCVTAS (0x1E240000, bit17=0),
    // which would otherwise be misdecoded as a raw GPR↔FP bit copy.
    if ((op & 0xFFE0FC00) == 0x1E200000 && (op & (1u << 18))
        && (op & (1u << 17))) {
        bool to_fp = (op >> 16) & 1;
        if (to_fp) { cpu.v_lo[rd] = cpu.regs[rn] & 0xFFFFFFFF; cpu.v_hi[rd] = 0; }
        else if (rd != 31) { cpu.regs[rd] = cpu.v_lo[rn] & 0xFFFFFFFF; }
        return;
    }
    // FMOV (scalar, immediate)
    // The 8-bit immediate is decoded via VFPExpandImm (ARM ARM):
    //   imm = sign : NOT(imm8[6]) : Replicate(imm8[6], K) : imm8[5:0] : Zeros(M)
    // where K and M depend on FP precision:
    //   single (32): K=5,  M=19  (1+1+5+6+19 = 32)
    //   double (64): K=8,  M=48  (1+1+8+6+48 = 64)
    //   half   (16): K=2,  M=6   (1+1+2+6+6  = 16)
    // FMOV (scalar, immediate): uses shared fp_decode helper.
    if (fp_decode::is_fmov_imm(op)) {
        uint8_t imm8 = (op >> 13) & 0xFF;
        uint64_t bits = fp_decode::vfp_expand_imm(imm8, ftype);
        if (ftype == 1) {
            cpu.v_lo[rd] = bits; cpu.v_hi[rd] = 0;
        } else if (ftype == 0) {
            cpu.v_lo[rd] = bits; cpu.v_hi[rd] = 0;
        } else {
            // ftype == 3 → half precision. We don't model 16-bit
            // FP natively, so expand to single-precision bits via
            // the standard half→single conversion of the imm value.
            // (Preserved from the original implementation.)
            uint64_t sign = (imm8 >> 7) & 1;
            uint64_t b     = (imm8 >> 6) & 1;
            uint64_t not_b = b ^ 1;
            uint64_t imm6  = imm8 & 0x3F;
            uint16_t rep_b = static_cast<uint16_t>(b * 0x3u);
            uint16_t hbits = static_cast<uint16_t>((sign << 15)
                          | (not_b << 14)
                          | (rep_b << 12)
                          | (imm6 << 6));
            uint32_t sexp = (hbits >> 10) & 0x1F;
            uint32_t smant = hbits & 0x3FF;
            uint32_t sbits;
            if (sexp == 0) {
                if (smant == 0) sbits = static_cast<uint32_t>(sign) << 31;
                else {
                    int e = -1;
                    while (!(smant & 0x400)) { smant <<= 1; e--; }
                    smant &= 0x3FF;
                    sbits = (static_cast<uint32_t>(sign) << 31)
                          | ((static_cast<uint32_t>(127 + e - 14)) << 23)
                          | (smant << 13);
                }
            } else if (sexp == 0x1F) {
                sbits = (static_cast<uint32_t>(sign) << 31) | (0xFFu << 23) | (smant << 13);
            } else {
                sbits = (static_cast<uint32_t>(sign) << 31)
                      | ((sexp - 15 + 127) << 23)
                      | (smant << 13);
            }
            cpu.v_lo[rd] = sbits; cpu.v_hi[rd] = 0;
        }
        return;
    }
    // FMOV (register, FP to FP)
    if ((op & 0xFFFFFC00) == 0x1E604000 || (op & 0xFFFFFC00) == 0x1E204000) {
        cpu.v_lo[rd] = cpu.v_lo[rn];
        if (ftype) cpu.v_hi[rd] = 0;
        else { cpu.v_lo[rd] &= 0xFFFFFFFF; cpu.v_hi[rd] = 0; }
        return;
    }
    // FP arithmetic (2-source): FADD/FSUB/FMUL/FDIV/FMAX/FMIN/
    // FMAXNM/FMINNM/FNMUL.
    // Per ARMv8 ARM C4.2.28:
    //   0x0=FMUL, 0x1=FDIV, 0x2=FADD, 0x3=FSUB,
    //   0x4=FMAX, 0x5=FMIN, 0x6=FMAXNM, 0x7=FMINNM, 0x8=FNMUL
    // BUGFIX: opcode 0x6 was FNMUL (should be FMAXNM), 0x7 was
    // missing (FMINNM), 0x8 (FNMUL) fell through to default=0.
    if ((op & 0xFF200000) == 0x1E200000 && ((op >> 21) & 1) == 1
        && ((op >> 10) & 0x3) == 0x2) {
        uint8_t opcode = (op >> 12) & 0xF;
        if (ftype) {
            double a = read_fp_d(cpu, rn), b = read_fp_d(cpu, rm), r = 0;
            switch (opcode) {
                case 0x0: r = a * b; break;                      // FMUL
                case 0x1: r = a / b; break;                      // FDIV
                case 0x2: r = a + b; break;                      // FADD
                case 0x3: r = a - b; break;                      // FSUB
                case 0x4: r = (std::isnan(a) || std::isnan(b))   // FMAX
                              ? std::numeric_limits<double>::quiet_NaN()
                              : std::fmax(a, b); break;
                case 0x5: r = (std::isnan(a) || std::isnan(b))   // FMIN
                              ? std::numeric_limits<double>::quiet_NaN()
                              : std::fmin(a, b); break;
                case 0x6: r = std::fmax(a, b); break;            // FMAXNM
                case 0x7: r = std::fmin(a, b); break;            // FMINNM
                case 0x8: r = -(a * b); break;                   // FNMUL
                default: r = 0; break;
            }
            write_fp_d(cpu, rd, r);
        } else {
            float a = read_fp_s(cpu, rn), b = read_fp_s(cpu, rm), r = 0;
            switch (opcode) {
                case 0x0: r = a * b; break;                      // FMUL
                case 0x1: r = a / b; break;                      // FDIV
                case 0x2: r = a + b; break;                      // FADD
                case 0x3: r = a - b; break;                      // FSUB
                case 0x4: r = (std::isnan(a) || std::isnan(b))   // FMAX
                              ? std::numeric_limits<float>::quiet_NaN()
                              : std::fmax(a, b); break;
                case 0x5: r = (std::isnan(a) || std::isnan(b))   // FMIN
                              ? std::numeric_limits<float>::quiet_NaN()
                              : std::fmin(a, b); break;
                case 0x6: r = std::fmax(a, b); break;            // FMAXNM
                case 0x7: r = std::fmin(a, b); break;            // FMINNM
                case 0x8: r = -(a * b); break;                   // FNMUL
                default: r = 0; break;
            }
            write_fp_s(cpu, rd, r);
        }
        return;
    }
    // SIMD scalar/vector 2-source FP (0x7E group): FABD.
    // Encoding: bits[31:24]=0x7E, bits[15:12]=0xD (FABD opcode),
    // bits[11:10]=0b01. bit22: 0=single, 1=double (not masked).
    // FABD computes |a - b|. musl's fabsf(got-want) is lowered to
    // `fabd s_, s0, s1` by the compiler; without this handler it
    // returns the first operand unchanged, breaking float
    // comparisons.
    if ((op & 0xFF00FC00) == 0x7E00D400) {
        bool is_double = (op >> 22) & 1;
        if (is_double) {
            double a = read_fp_d(cpu, rn), b = read_fp_d(cpu, rm);
            write_fp_d(cpu, rd, std::fabs(a - b));
        } else {
            float a = read_fp_s(cpu, rn), b = read_fp_s(cpu, rm);
            write_fp_s(cpu, rd, std::fabs(a - b));
        }
        return;
    }
    // FP compare-with-zero: FCMGE/FCMGT/FCMLE/FCMLT <Dd>,<Dn>,#0.0
    // (also the 2D/4S/2S vector forms, size bits free).
    //   FCMGE 0x7EE08800  FCMGT 0x7EE08C00
    //   FCMLE 0x7EE0C800  FCMLT 0x7EE0CC00
    // mask 0xFF3F9000/0x7E208000: bits[31:24]=0x7E, bit21=1,
    // bits[20:16]=0, bit15=1, bit12=0; bit10 selects strict
    // (1=GT/LT, 0=GE/LE) and bit14 selects the swapped operand
    // (1=compare #0 vs Dn: LE/LT). libgcc's aarch64 unwinder
    // (uw_frame_state_for) lowers `context->ra` + a flag mask via
    // `cmge d0,d0,#0` (0x7EE08800) — a silent NOP here OR'd
    // 0x4000000000000000 into the pc, crashing every C++
    // exception with a bogus _Unwind_Find_FDE address.
    if ((op & 0xFF3F9000) == 0x7E208000) {
        // Result is a compare MASK (all-ones/all-zeros), not 1.0/0.0:
        // libgcc's aarch64 unwinder (uw_frame_state_for) does
        // `cmge d0,d0,#0` (0x7EE08800), `fmov x0,d0`, `add x0,ra,x0`
        // to fold a flag mask into the FDE pc (ra-1 when set). A
        // 1.0/0.0 result would OR 0x3FF0000000000000 into the pc.
        bool is_double = (op >> 22) & 1;
        bool strict    = (op >> 10) & 1;   // 0: GE/LE, 1: GT/LT
        bool swapped   = (op >> 14) & 1;   // 0: Dn op #0, 1: #0 op Dn
        if (is_double) {
            double a = read_fp_d(cpu, rn);
            bool t = swapped ? (strict ? (0.0 > a) : (0.0 >= a))
                             : (strict ? (a > 0.0) : (a >= 0.0));
            cpu.v_lo[rd] = t ? ~0ULL : 0ULL;
            cpu.v_hi[rd] = t ? ~0ULL : 0ULL;
        } else {
            float a = read_fp_s(cpu, rn);
            bool t = swapped ? (strict ? (0.0f > a) : (0.0f >= a))
                             : (strict ? (a > 0.0f) : (a >= 0.0f));
            cpu.v_lo[rd] = t ? 0xFFFFFFFFu : 0u;
            cpu.v_hi[rd] = 0;
        }
        return;
    }
    // FCMP/FCMPE: FP compare (sets NZCV).
    // Uses shared fp_decode helpers for encoding detection.
    // Must be checked BEFORE FP 1-source (below) because both
    // have bits[11:10]=0b00; without this explicit check,
    // FCMP would be misdecoded as FNEG.
    if (fp_decode::is_fcmp(op)) {
        bool with_zero = fp_decode::fcmp_with_zero(op);
        uint32_t nzcv;
        auto set_nzcv = [&](bool unordered, bool less, bool equal) {
            // ARM FCMP NZCV (bits[31:28] = N Z C V):
            //   unordered: N=0 Z=0 C=1 V=1 = 0x30000000
            //   less:      N=1 Z=0 C=0 V=0 = 0x80000000
            //   equal:     N=0 Z=1 C=1 V=0 = 0x60000000
            //   greater:   N=0 Z=0 C=1 V=0 = 0x20000000
            // (Previous code used 0x28000000 for unordered, which
            //  decodes as N=0 Z=0 C=1 V=0 — same as "greater" —
            //  diverging from both the JIT and the ARM ARM.)
            if (unordered)      nzcv = 0x30000000;
            else if (less)       nzcv = 0x80000000;
            else if (equal)      nzcv = 0x60000000;
            else                 nzcv = 0x20000000;
        };
        if (ftype == 1) {  // double
            double a = read_fp_d(cpu, rn);
            double b = with_zero ? 0.0 : read_fp_d(cpu, rm);
            if (std::isnan(a) || std::isnan(b))
                set_nzcv(true, false, false);
            else if (a < b) set_nzcv(false, true, false);
            else if (a > b) set_nzcv(false, false, false);
            else            set_nzcv(false, false, true);
        } else if (ftype == 0) {  // single
            float a = read_fp_s(cpu, rn);
            float b = with_zero ? 0.0f : read_fp_s(cpu, rm);
            if (std::isnan(a) || std::isnan(b))
                set_nzcv(true, false, false);
            else if (a < b) set_nzcv(false, true, false);
            else if (a > b) set_nzcv(false, false, false);
            else            set_nzcv(false, false, true);
        } else {
            nzcv = 0x30000000;  // half-precision: treat as unordered
        }
        cpu.pstate = (cpu.pstate & 0x0FFFFFFF) | nzcv;
        return;
    }
    // ── FCCMP/FCCMPE: FP conditional compare ──────────────────
    // `fccmp d1, d2, #0x0, eq` = 0x1e620420.
    // Encoding: bits[31:24]=0x1E, bit21=1, bits[11:10]=01,
    // Rm=bits[20:16], cond=bits[15:12], nzcv=bits[3:0],
    // E bit=bit[4] (FCCMPE — we don't model FP exceptions, so
    // FCCMPE behaves identically to FCCMP here).
    // If cond is true: compare Fn vs Fm and set NZCV like FCMP.
    // If false: set NZCV = nzcv immediate. Without this the
    // instruction was silently NOP'd, leaving NZCV stale and
    // corrupting every subsequent conditional branch (Qt raster
    // code compares bezier/color values with FCCMP).
    if ((op & 0xFF200C00) == 0x1E200400) {
        uint8_t cond = (op >> 12) & 0xF;
        uint8_t nzcv = op & 0xF;
        if (cond_true(cond, cpu.pstate)) {
            bool unordered = false, less = false, equal = false;
            if (ftype == 1) {  // double
                double a = read_fp_d(cpu, rn);
                double b = read_fp_d(cpu, rm);
                if (std::isnan(a) || std::isnan(b))       unordered = true;
                else if (a < b)                            less = true;
                else if (a > b)                            { /* greater */ }
                else                                       equal = true;
            } else {  // single (or half — no native FP16)
                float a = read_fp_s(cpu, rn);
                float b = read_fp_s(cpu, rm);
                if (std::isnan(a) || std::isnan(b))       unordered = true;
                else if (a < b)                            less = true;
                else if (a > b)                            { /* greater */ }
                else                                       equal = true;
            }
            uint32_t n;
            if (unordered)      n = 0x30000000;
            else if (less)       n = 0x80000000;
            else if (equal)      n = 0x60000000;
            else                 n = 0x20000000;
            cpu.pstate = (cpu.pstate & 0x0FFFFFFF) | n;
        } else {
            uint32_t n = static_cast<uint32_t>(nzcv) << 28;
            cpu.pstate = (cpu.pstate & 0x0FFFFFFF) | n;
        }
        return;
    }
    // FP 1-source: FMOV/FABS/FNEG/FSQRT/FRINT*
    // Uses shared fp_decode helpers. The 6-bit opcode is in
    // bits[20:15] (= rmode:opcode in the ARM ARM).
    if (fp_decode::is_fp_1source(op)) {
        uint8_t opcode = fp_decode::fp_1source_opcode(op);
        if (ftype) {
            double a = read_fp_d(cpu, rn), r = 0;
            switch (opcode) {
                case 0x0: r = a; break;                        // FMOV
                case 0x1: r = std::fabs(a); break;             // FABS
                case 0x2: r = -a; break;                       // FNEG
                case 0x3: r = std::sqrt(a); break;             // FSQRT
                case 0x8: r = std::rint(a); break;             // FRINTN
                case 0x9: r = std::ceil(a); break;             // FRINTP
                case 0xA: r = std::floor(a); break;            // FRINTM
                case 0xB: r = std::trunc(a); break;            // FRINTZ
                case 0xC: r = std::round(a); break;            // FRINTA (ties-away)
                // 0xD unused in A64
                case 0xE: r = std::rint(a); break;             // FRINTX
                case 0xF: r = std::rint(a); break;             // FRINTI
                default: r = a; break;
            }
            write_fp_d(cpu, rd, r);
        } else {
            float a = read_fp_s(cpu, rn), r = 0;
            switch (opcode) {
                case 0x0: r = a; break;
                case 0x1: r = std::fabs(a); break;
                case 0x2: r = -a; break;
                case 0x3: r = std::sqrt(a); break;
                case 0x8: r = std::rint(a); break;             // FRINTN
                case 0x9: r = std::ceil(a); break;             // FRINTP
                case 0xA: r = std::floor(a); break;            // FRINTM
                case 0xB: r = std::trunc(a); break;            // FRINTZ
                case 0xC: r = std::round(a); break;            // FRINTA (ties-away)
                // 0xD unused in A64
                case 0xE: r = std::rint(a); break;             // FRINTX
                case 0xF: r = std::rint(a); break;             // FRINTI
                default: r = a; break;
            }
            write_fp_s(cpu, rd, r);
        }
        return;
    }
    // FCVT{N,P,M,Z,A}{S,U} — FP to int with explicit rounding mode.
    // Encoding (integer variant, bit 21 = 1):
    //   0x1E200000 = FCVTNS, 0x1E280000 = FCVTPS,
    //   0x1E300000 = FCVTMS, 0x1E240000 = FCVTAS,
    //   0x1E380000 = FCVTZS/FCVTZU.
    // The rounding mode is in bits[20:19]:
    //   00 = N (nearest even), 01 = P (+inf), 10 = M (-inf),
    //   11 = Z (zero); bit[18]=1 selects A (nearest, ties away
    //   from zero). bit[16]=1 selects the unsigned variant.
    // Mask 0x7F220000 leaves bits[20:19], bit 18 and bit 16 free
    // so every rounding/unsigned variant matches; the old mask
    // (0x7F3E0000 == 0x1E280000) only matched FCVTPS, silently
    // NOPing FCVTNS/FCVTMS/FCVTAS. bits[15:10]==0 excludes FCSEL
    // (0x1E200C00, bits[13:10]=1100), which otherwise collides.
    // The fixed-point variant (bit 21 = 0) is dispatched below.
    if ((op & 0x7F220000) == 0x1E200000 && ((op >> 10) & 0x3F) == 0) {
        bool is_away = (op >> 18) & 1;       // FCVTAS/FCVTAU
        uint8_t rmode = (op >> 19) & 0x3;    // bits 20:19: 0=N,1=P,2=M,3=Z
        bool is_unsigned = ((op >> 16) & 1);  // bit 16 = U
        bool is_64bit = sf_val;
        auto round_d = [&](double v) -> double {
            if (is_away) return std::round(v);      // A: ties away from zero
            switch (rmode) {
                case 0: return std::nearbyint(v);   // N (nearest-even)
                case 1: return std::ceil(v);        // P
                case 2: return std::floor(v);       // M
                default: return v;                  // Z (trunc — identity)
            }
        };
        auto round_s = [&](float v) -> double {
            if (is_away) return std::round(v);      // A: ties away from zero
            switch (rmode) {
                case 0: return std::nearbyint(v);   // N (nearest-even)
                case 1: return std::ceil(v);        // P
                case 2: return std::floor(v);       // M
                default: return v;                  // Z (trunc — identity)
            }
        };
        if (ftype) {
            double a = read_fp_d(cpu, rn);
            double r = round_d(a);
            if (is_unsigned) {
                uint64_t v = fp_to_unsigned_sat(r, is_64bit);
                if (rd != 31) cpu.regs[rd] = is_64bit ? v : static_cast<uint32_t>(v);
            } else {
                int64_t v = fp_to_signed_sat(r, is_64bit);
                if (rd != 31) cpu.regs[rd] = is_64bit ? static_cast<uint64_t>(v) : static_cast<uint32_t>(static_cast<int32_t>(v));
            }
        } else {
            float a = read_fp_s(cpu, rn);
            double r = round_s(a);
            if (is_unsigned) {
                uint64_t v = fp_to_unsigned_sat(r, is_64bit);
                if (rd != 31) cpu.regs[rd] = is_64bit ? v : static_cast<uint32_t>(v);
            } else {
                int64_t v = fp_to_signed_sat(r, is_64bit);
                if (rd != 31) cpu.regs[rd] = is_64bit ? static_cast<uint64_t>(v) : static_cast<uint32_t>(static_cast<int32_t>(v));
            }
        }
        return;
    }
    // FCVT (between FP precisions)
    if ((op & 0xFFFFFC00) == 0x1E624000) { // FCVT Sd, Dn
        write_fp_s(cpu, rd, static_cast<float>(read_fp_d(cpu, rn))); return;
    }
    if ((op & 0xFFFFFC00) == 0x1E22C000) { // FCVT Dd, Sn
        write_fp_d(cpu, rd, static_cast<double>(read_fp_s(cpu, rn))); return;
    }
    // FCVT H — half-precision conversions. We don't model 16-bit FP
    // natively, but we can route H↔S via host __gnu_f2h_ieee / __gnu_h2f_ieee
    // (or std::floor of a manual conversion) so at least the value
    // survives round-tripping. Common case is musl's printf path,
    // which sometimes uses FCVT Hn, Dn for hex-float formatting.
    if ((op & 0xFFFFFC00) == 0x1E63C000) { // FCVT Hd, Dn (D → H)
        // Half is stored in low 16 bits of v_lo.
        double d = read_fp_d(cpu, rn);
        uint16_t hbits = d2h(d);
        cpu.v_lo[rd] = hbits; cpu.v_hi[rd] = 0;
        return;
    }
    if ((op & 0xFFFFFC00) == 0x1E23C000) { // FCVT Hn, Sn (S → H)
        float f = read_fp_s(cpu, rn);
        uint16_t hbits = f2h(f);
        cpu.v_lo[rd] = hbits; cpu.v_hi[rd] = 0;
        return;
    }
    if ((op & 0xFFFFFC00) == 0x1E634000) { // FCVT Sn, Hn (H → S)
        uint16_t hbits = static_cast<uint16_t>(cpu.v_lo[rn] & 0xFFFF);
        float f = h2f(hbits);
        write_fp_s(cpu, rd, f);
        return;
    }
    if ((op & 0xFFFFFC00) == 0x1E224000) { // FCVT Dd, Hn (H → D)
        uint16_t hbits = static_cast<uint16_t>(cpu.v_lo[rn] & 0xFFFF);
        double d = static_cast<double>(h2f(hbits));
        write_fp_d(cpu, rd, d);
        return;
    }
    // Integer FCVTZS/FCVTZU are handled by the rounding-mode
    // conversion family above. A second loose mask here used to
    // capture FCSEL whenever Rm was S24/S25 or D24/D25, writing
    // the FP destination number as a GPR (even XZR).
    // ── Fixed-point int↔FP conversions (SCVTF/UCVTF/FCVTZS/FCVTZU #fbits) ──
    // Decoded by the generated table (Tools/CodeGen/fp_fixconv.txt →
    // Include/Generated/FixedPointTable.hpp) so interp, IR translator and JIT
    // gate share one mask set. Subops:
    //   0 = SCVTF/UCVTF int→FP, GPR source (U=bit16, sf=bit31,
    //       fbits = 64-bits[15:10])
    //   1 = FCVTZS/FCVTZU FP→int, GPR dest (same field decode)
    //   2 = SCVTF/UCVTF int→FP, FP source/dest (U=bit29, size=bit22,
    //       fbits = 64-bits[21:16])
    //   3 = FCVTZS/FCVTZU FP→int, FP source/dest (same field decode)
    {
        fpfixed::Op fc = fpfixed::classify(op);
        if (fc.family == fpfixed::Family::FIXCONV) {
            if (fc.subop <= 1) {
                // FPDataProc1 forms: GPR source/dest.
                bool is_unsigned = ((op >> 16) & 1);
                bool is_64bit = sf_val;
                int fbits = 64 - static_cast<int>((op >> 10) & 0x3F);
                if (fc.subop == 1) {
                    // FCVTZS/FCVTZU: scale by 2^fbits, truncate toward
                    // zero, saturate to dest range, NaN → 0. Without
                    // this handler every fixed-point FCVTZU (e.g. toybox
                    // MD5 K-table init `fcvtzu w1, d0, #32`) was silently
                    // NOP'd, leaving wrong hashes.
                    double a = ftype ? read_fp_d(cpu, rn)
                                     : static_cast<double>(read_fp_s(cpu, rn));
                    double scaled = std::ldexp(a, fbits);
                    if (is_unsigned) {
                        uint64_t v = fp_to_unsigned_sat(scaled, is_64bit);
                        if (rd != 31) cpu.regs[rd] = is_64bit ? v : static_cast<uint32_t>(v);
                    } else {
                        double hi = is_64bit ? 9223372036854775808.0
                                             : 2147483648.0;
                        double lo = -hi;
                        int64_t v = std::isnan(a) ? 0
                                  : (scaled >= hi) ? (is_64bit ? INT64_MAX : INT32_MAX)
                                  : (scaled < lo)  ? (is_64bit ? INT64_MIN : INT32_MIN)
                                  : static_cast<int64_t>(scaled);
                        if (rd != 31) cpu.regs[rd] = is_64bit ? static_cast<uint64_t>(v)
                                                : static_cast<uint32_t>(static_cast<int32_t>(v));
                    }
                } else {
                    // SCVTF/UCVTF: convert integer to FP, divide by
                    // 2^fbits (treat the source as fixed-point).
                    double v = is_unsigned
                        ? static_cast<double>(is_64bit ? cpu.regs[rn]
                                                       : static_cast<uint32_t>(cpu.regs[rn]))
                        : static_cast<double>(is_64bit ? static_cast<int64_t>(cpu.regs[rn])
                                                       : static_cast<int32_t>(cpu.regs[rn]));
                    double result = std::ldexp(v, -fbits);
                    if (ftype) write_fp_d(cpu, rd, result);
                    else       write_fp_s(cpu, rd, static_cast<float>(result));
                }
            } else {
                // AdvSIMD-scalar forms: FP register source/dest.
                // GCC emits `scvtf s0, s0, #1` (0x5F3FE400) when the
                // integer already sits in an FP register (e.g. `(float)x`
                // with x loaded from memory). The old silent NOP left the
                // dest holding the raw integer bit pattern as a denormal
                // float, breaking the inRect() hit test in rudolf-cart.
                bool is_unsigned = (op >> 29) & 1;
                bool is_double = (op >> 22) & 1;
                int fbits = 64 - static_cast<int>((op >> 16) & 0x3F);
                uint64_t src_bits = cpu.v_lo[rn];
                if (fc.subop == 2) {
                    if (is_double) {
                        double v = is_unsigned
                            ? static_cast<double>(static_cast<uint64_t>(src_bits))
                            : static_cast<double>(static_cast<int64_t>(src_bits));
                        write_fp_d(cpu, rd, std::ldexp(v, -fbits));
                    } else {
                        float v = is_unsigned
                            ? static_cast<float>(static_cast<uint32_t>(src_bits))
                            : static_cast<float>(static_cast<int32_t>(src_bits));
                        write_fp_s(cpu, rd, std::ldexp(v, -fbits));
                    }
                } else {
                    double a = is_double ? read_fp_d(cpu, rn)
                                         : static_cast<double>(read_fp_s(cpu, rn));
                    double scaled = std::ldexp(a, fbits);
                    if (is_unsigned) {
                        // Route via helper: direct cast of
                        // [2^63,2^64) hits x86 cvttsd2si sentinel.
                        uint64_t out = fp_to_unsigned_sat(scaled, is_double);
                        // Write to Sd zeroes the upper 32 bits (matches
                        // the two-register-misc FCVTZS handler below).
                        cpu.v_lo[rd] = is_double ? out : (out & 0xFFFFFFFFULL);
                        cpu.v_hi[rd] = 0;
                    } else {
                        double hi = is_double ? 9223372036854775808.0
                                              : 2147483648.0;
                        int64_t out = std::isnan(a) ? 0
                                    : (scaled >= hi) ? (is_double ? INT64_MAX : INT32_MAX)
                                    : (scaled < -hi) ? (is_double ? INT64_MIN : INT32_MIN)
                                    : static_cast<int64_t>(scaled);
                        cpu.v_lo[rd] = is_double ? static_cast<uint64_t>(out)
                                                 : (static_cast<uint32_t>(out) & 0xFFFFFFFFULL);
                        cpu.v_hi[rd] = 0;
                    }
                }
            }
            return;
        }
    }
    // SCVTF/UCVTF (integer variant)
    // Mask 0x7F3EFC00 with constant 0x1E220000 requires bit 21 = 1
    // and bits[15:10] == 0. The 0xFC00 bits are essential: FCSEL
    // (0x1E220C01, bits[15:10] = 0b0011) matches the old loose mask
    // 0x7F3E0000 and was mis-executed as SCVTF (int→FP), converting
    // the hash GPR into an FP register. This broke grad3's
    // `fcsel s1, s0, s2, eq` (hash=151 → s1 became 151.0f).
    if ((op & 0x7F3EFC00) == 0x1E220000) {  // SCVTF/UCVTF
        bool is_unsigned = ((op >> 16) & 1);
        bool is_64bit = sf_val;
        if (ftype) {
            if (is_unsigned) {
                uint64_t v = is_64bit ? cpu.regs[rn] : static_cast<uint32_t>(cpu.regs[rn]);
                write_fp_d(cpu, rd, static_cast<double>(v));
            } else {
                int64_t v = is_64bit ? static_cast<int64_t>(cpu.regs[rn]) : static_cast<int32_t>(cpu.regs[rn]);
                write_fp_d(cpu, rd, static_cast<double>(v));
            }
        } else {
            if (is_unsigned) {
                uint64_t v = is_64bit ? cpu.regs[rn] : static_cast<uint32_t>(cpu.regs[rn]);
                write_fp_s(cpu, rd, static_cast<float>(v));
            } else {
                int64_t v = is_64bit ? static_cast<int64_t>(cpu.regs[rn]) : static_cast<int32_t>(cpu.regs[rn]);
                write_fp_s(cpu, rd, static_cast<float>(v));
            }
        }
        return;
    }
    // ── SIMD scalar int↔FP conversions (FP source/dest) ───────────────
    // These are in the "Advanced SIMD scalar two-register miscellaneous"
    // group (bits[31:24]=0x5E, bit 30=Q=1 for scalar form). They
    // differ from the standard FP scalar forms (0x1E...) by having
    // the integer source/dest in an FP register (Dn/Sn) instead of
    // a GPR (Xn/Wn). GCC/clang emit these for `(double)long_var`
    // when the long is already in an FP register from a load —
    // saving a GPR move.
    //
    // Encoding layout (after masking out U/size/opcode/Rn/Rd):
    //   bits[31:24] = 0x5E (constant — scalar SIMD)
    //   bit 29      = U (0=signed, 1=unsigned)
    //   bits[23:22] = size (prec + int width, see per-op below)
    //   bits[21:17] = 10000 (constant)
    //   bits[16:12] = opcode (5 bits)
    //   bits[11:10] = 10 (constant)
    //   bits[9:5]   = Rn (source FP reg)
    //   bits[4:0]   = Rd (dest FP reg)
    //
    // Group mask: 0xDF3E0C00 (bit 29 = U, allowed to vary),
    // group constant: 0x5E200800 (with U=0).
    // SCVTF/UCVTF opcode = 11101 (0x1D) → constant 0x5E21D800.
    // FCVTZS/FCVTZU opcode = 11011 (0x1B) → constant 0x5E21B800.
    //
    // For SCVTF/UCVTF (int→FP):
    //   size=01 → 64-bit int src, double-precision dest (Dd, Dn)
    //   size=00 → 32-bit int src, single-precision dest (Sd, Sn)
    // For FCVTZS/FCVTZU (FP→int):
    //   size=11 → double-precision src, 64-bit int dest (Dd, Dn)
    //   size=10 → single-precision src, 32-bit int dest (Sd, Sn)
    if ((op & 0xDF3E0C00) == 0x5E200800) {
        bool is_unsigned = (op >> 29) & 1;
        uint8_t opcode = (op >> 12) & 0x1F;
        bool is_double = (op >> 22) & 1;  // also doubles as "is 64-bit int"
        if (opcode == 0x1D) {  // SCVTF/UCVTF (int → FP, FP source)
            // Read integer bits from FP source register.
            uint64_t src_bits = cpu.v_lo[rn];
            if (is_double) {
                // 64-bit int → double-precision float
                double v = is_unsigned
                    ? static_cast<double>(static_cast<uint64_t>(src_bits))
                    : static_cast<double>(static_cast<int64_t>(src_bits));
                write_fp_d(cpu, rd, v);
            } else {
                // 32-bit int → single-precision float
                float v = is_unsigned
                    ? static_cast<float>(static_cast<uint32_t>(src_bits))
                    : static_cast<float>(static_cast<int32_t>(src_bits));
                write_fp_s(cpu, rd, v);
            }
            return;
        }
        if (opcode == 0x1B) {  // FCVTZS/FCVTZU (FP → int, FP dest)
            if (is_double) {
                double v = read_fp_d(cpu, rn);
                uint64_t out = is_unsigned
                    ? fp_to_unsigned_sat(v, true)
                    : static_cast<uint64_t>(fp_to_signed_sat(v, true));
                cpu.v_lo[rd] = out;
            } else {
                float v = read_fp_s(cpu, rn);
                uint64_t out = is_unsigned
                    ? fp_to_unsigned_sat(v, false)
                    : static_cast<uint64_t>(fp_to_signed_sat(v, false));
                // Zero-extend 32-bit result into 64-bit FP register
                cpu.v_lo[rd] = out & 0xFFFFFFFFULL;
            }
            return;
        }
        // Other opcodes in this group (FRINTN, FABS, etc.) are
        // encoded in the 0x1E... group, not 0x5E.... Fall through
        // to "Unknown FP" if we ever see a different opcode here.
    }
    // FCSEL
    if ((op & 0xFF200C00) == 0x1E200C00) {  // FCSEL (bit 21=0, bits[13:10]=1100)
        uint8_t cond = (op >> 12) & 0xF;
        if (ftype) {
            double r = cond_true(cond, cpu.pstate) ? read_fp_d(cpu, rn) : read_fp_d(cpu, rm);
            write_fp_d(cpu, rd, r);
        } else {
            float r = cond_true(cond, cpu.pstate) ? read_fp_s(cpu, rn) : read_fp_s(cpu, rm);
            write_fp_s(cpu, rd, r);
        }
        return;
    }
    // FMADD/FMSUB/FNMADD/FNMSUB
    // Encoding: bits[31:24]=0x1F, bit 15=o1 (sub), bit 21=o2 (neg).
    // matched FMADD/FMSUB (o2=0); FNMADD/FNMSUB (o2=1) fell
    // through to the "Unknown FP — NOP" path, silently
    // returning whatever was in Vd. This broke any guest
    // program that used FNMADD/FNMSUB (e.g. musl's __muldf3
    // long-double fallback for printf %Lf).
    //
    // Per ARM ARM, the four FMA variants are:
    //   FMADD  (o2=0, o1=0): Vd = Va + Vn*Vm       = c + a*b
    //   FMSUB  (o2=0, o1=1): Vd = Va - Vn*Vm       = c - a*b
    //   FNMADD (o2=1, o1=0): Vd = -Va - Vn*Vm      = -c - a*b
    //   FNMSUB (o2=1, o1=1): Vd = -Va + Vn*Vm      = a*b - c
    //
    // std::fma performs the product and sum with one final
    // rounding, as required by the AArch64 fused instructions.
    if ((op & 0xFF000000) == 0x1F000000) {
        uint8_t ra = (op >> 10) & 0x1F;
        bool sub = (op >> 15) & 1;   // o1
        bool neg = (op >> 21) & 1;   // o2
        if (ftype) {
            double a = read_fp_d(cpu, rn), b = read_fp_d(cpu, rm),
                   c = read_fp_d(cpu, ra);
            double r;
            if      (!neg && !sub) r = std::fma(a, b, c);       // FMADD  = a*b + c
            else if (!neg &&  sub) r = std::fma(-a, b, c);      // FMSUB  = c - a*b
            else if ( neg && !sub) r = std::fma(-a, b, -c);     // FNMADD = -a*b - c
            else                   r = std::fma(a, b, -c);      // FNMSUB = a*b - c
            write_fp_d(cpu, rd, r);
        } else {
            float a = read_fp_s(cpu, rn), b = read_fp_s(cpu, rm),
                  c = read_fp_s(cpu, ra);
            float r;
            if      (!neg && !sub) r = std::fma(a, b, c);       // FMADD  = a*b + c
            else if (!neg &&  sub) r = std::fma(-a, b, c);      // FMSUB  = c - a*b
            else if ( neg && !sub) r = std::fma(-a, b, -c);     // FNMADD = -a*b - c
            else                   r = std::fma(a, b, -c);      // FNMSUB = a*b - c
            write_fp_s(cpu, rd, r);
        }
        return;
    }
    // ── SIMD scalar INTEGER ADD/SUB (add d / sub d) ───────────
    // Scalar three-same group: bits[28:24]=11110, bit21=1,
    // bits[11:10]=01 (NOT the vector's 10). Verified with objdump:
    //   add d2,d0,d1 = 0x5EE18402 → masked 0x5EE08400
    //   sub d2,d0,d1 = 0x7EE18402 → masked 0x7EE08400
    // (U=bit29 picks add vs sub; size must be 11.) Do NOT use the
    // bits[11:10]=11 patterns — that space is CMTST/CMEQ
    // (cmtst d = 0x5EE18C02, cmeq d = 0x7EE18C02). mimalloc's
    // bitmap code does `add d2, d0, d1` then `str d2`.
    if ((op & 0xFFE0FC00) == 0x5EE08400) {
        cpu.v_lo[rd] = cpu.v_lo[rn] + cpu.v_lo[rm];
        cpu.v_hi[rd] = 0;
        return;
    }
    if ((op & 0xFFE0FC00) == 0x7EE08400) {
        cpu.v_lo[rd] = cpu.v_lo[rn] - cpu.v_lo[rm];
        cpu.v_hi[rd] = 0;
        return;
    }
    // ── Scalar FCVTXN (Sd ← Dn, round-to-odd) ─────────────────
    // fcvtxn s8,d7 = 0x7E6168E8 → masked 0x7E616800 (rn/rd
    // stripped; U=1 and size=01 are part of the constant).
    // Round-to-odd double→single narrowing; writes Sd (low 32
    // bits of Vd), the rest of Vd unchanged (scalar rule).
    // The vector forms live in the SIMD_DP section above.
    if ((op & 0xFFFFFC00) == 0x7E616800) {
        uint64_t raw = cpu.v_lo[rn];
        double dv;
        memcpy(&dv, &raw, 8);
        float fv = fp_to_f32_rto(dv);
        memcpy(reinterpret_cast<uint32_t*>(&cpu.v_lo[rd]), &fv, 4);
        return;
    }
    // ── Scalar pairwise FADDP/FMAXP/FMINP ─────────────────────
    //   faddp s0,v0.2s = 0x7E30D800   faddp d0,v0.2d = 0x7E70D800
    //   fmaxp s0,v0.2s = 0x7E30F800   fmaxp d0,v0.2d = 0x7E70F800
    //   fminp s0,v0.2s = 0x7EB0F800   fminp d0,v0.2d = 0x7EF0F800
    // (mask 0xFFFFFC00 strips Rn/Rd; bit23=FMINP, bit22=double,
    //  opcode bits[15:12]: D=add, F=max/min). compiler-rt's
    // vectorized sin() emits `faddp d4,v4.2d`; previously these
    // fell through to the silent FP-NOP fallback, corrupting sin().
    {
        const uint32_t pw = op & 0xFFFFFC00;
        // Integer scalar ADDP (add pair): Dd = Vn.d[0] + Vn.d[1]
        // (wrapping). Encoding verified via cross-assembler:
        // `addp d0, v0.2d` = 0x5EF1B800 (only .2d form exists;
        // combiner/loop code like long sums lowers to it).
        // Previously fell to the FP-NOP fallback (stale d31 read
        // back, e.g. sum {1,2,3,4} returned 3).
        if (pw == 0x5EF1B800) {
            cpu.v_lo[rd] = cpu.v_lo[rn] + cpu.v_hi[rn];
            cpu.v_hi[rd] = 0;
            return;
        }
        if (pw == 0x7E30D800 || pw == 0x7E70D800 ||
            pw == 0x7E30F800 || pw == 0x7E70F800 ||
            pw == 0x7EB0F800 || pw == 0x7EF0F800) {
            const bool is_add = ((op >> 12) & 0xF) == 0xD;
            const bool is_min = (op & 0x00800000) != 0;
            if (op & 0x00400000) {  // double: lanes = Vn.2D
                double a = read_fp_d(cpu, rn);
                uint64_t hi_bits = cpu.v_hi[rn];
                double b; memcpy(&b, &hi_bits, 8);
                if (is_add)      write_fp_d(cpu, rd, a + b);
                // FMAXP/FMINP propagate NaN like FMAX/FMIN (they are
                // the non-NM forms; FMAXNMP/FMINNMP are separate
                // encodings verified via cross-as).
                else if (is_min) write_fp_d(cpu, rd, (std::isnan(a) || std::isnan(b))
                                    ? std::numeric_limits<double>::quiet_NaN()
                                    : std::fmin(a, b));
                else             write_fp_d(cpu, rd, (std::isnan(a) || std::isnan(b))
                                    ? std::numeric_limits<double>::quiet_NaN()
                                    : std::fmax(a, b));
            } else {                // single: both lanes in Vn low word
                uint32_t lbits = static_cast<uint32_t>(cpu.v_lo[rn]);
                float a; memcpy(&a, &lbits, 4);
                uint32_t hbits = static_cast<uint32_t>(cpu.v_lo[rn] >> 32);
                float b; memcpy(&b, &hbits, 4);
                if (is_add)      write_fp_s(cpu, rd, a + b);
                else if (is_min) write_fp_s(cpu, rd, (std::isnan(a) || std::isnan(b))
                                    ? std::numeric_limits<float>::quiet_NaN()
                                    : std::fmin(a, b));
                else             write_fp_s(cpu, rd, (std::isnan(a) || std::isnan(b))
                                    ? std::numeric_limits<float>::quiet_NaN()
                                    : std::fmax(a, b));
            }
            return;
        }
    }
    // ── Scalar × indexed-element FMLA/FMLS/FMUL/FMULX ──────────
    //   fmla d4,d2,v3.d[1] = 0x5FC31844   fmla s4,s2,v3.s[3] = 0x5FA31844
    //   fmls d4,d2,v3.d[1] = 0x5FC35844
    //   fmul d4,d2,v3.d[1] = 0x5FC39844   fmul s4,s2,v3.s[3] = 0x5FA39844
    //   fmulx d4,d2,v3.d[1]= 0x7FC39844   fmulx s4,s2,v3.s[3]= 0x7FA39844
    // Layout (verified vs cross-assembler): bits[31:24]=01U11111,
    // bit23=1 (fixed), bit22=sz (1=d), bit21=L (.s index MSB),
    // bit20=M=Rm<4>, bits[19:16]=Rm<3:0>, bit11=H (index LSB),
    // bit10=0. Index: .d → H; .s → L:H.
    // bit10=0. Index: .d → H; .s → L:H. Discriminator vs the
    // scalar shift/narrow-imm space (sqshrn/sshr/shl): those
    // carry immh with bit23==0; this group always sets bit23.
    if ((op & 0xC0000400) == 0x40000000 &&
        ((op >> 24) & 0x1F) == 0x1F && ((op >> 23) & 1) == 1) {
        uint8_t opc = (op >> 12) & 0xF;
        bool is_u = (op >> 29) & 1;
        if ((opc == 0x1 || opc == 0x5 || opc == 0x9) && !(is_u && opc != 0x9)) {
            bool is_dbl = (op >> 22) & 1;
            uint8_t rm_reg =
                (uint8_t)((((op >> 20) & 1) << 4) | ((op >> 16) & 0xF));
            unsigned idx = is_dbl ? ((op >> 11) & 1)
                                  : ((((op >> 11) & 1) << 1) |
                                     ((op >> 21) & 1));
            double elem;
            float elem_s;
            if (is_dbl) {
                uint64_t bits = idx ? cpu.v_hi[rm_reg]
                                    : cpu.v_lo[rm_reg];
                memcpy(&elem, &bits, 8);
            } else {
                const uint64_t& src =
                    (idx >= 2) ? cpu.v_hi[rm_reg] : cpu.v_lo[rm_reg];
                uint32_t w = static_cast<uint32_t>(
                    src >> ((idx & 1) ? 32 : 0));
                memcpy(&elem_s, &w, 4);
            }
            if (opc == 0x9) {              // FMUL / FMULX
                if (is_dbl)
                    write_fp_d(cpu, rd, read_fp_d(cpu, rn) * elem);
                else
                    write_fp_s(cpu, rd, read_fp_s(cpu, rn) * elem_s);
            } else {                       // FMLA/FMLS: rd accumulates
                if (is_dbl) {
                    double acc = read_fp_d(cpu, rd);
                    double prod = read_fp_d(cpu, rn) * elem;
                    write_fp_d(cpu, rd, opc == 0x1 ? acc + prod
                                                   : acc - prod);
                } else {
                    float acc = read_fp_s(cpu, rd);
                    float prod = read_fp_s(cpu, rn) * elem_s;
                    write_fp_s(cpu, rd, opc == 0x1 ? acc + prod
                                                   : acc - prod);
                }
            }
            return;
        }
    }
    // Unknown FP_SCALAR — NOP + log (don't crash). NOTE: this diverges
    // from the SIMD_DP policy above (DecodeError → SIGILL); unify
    // to DecodeError if a missing scalar op ever hides here.
    {
        static uint64_t fp_nop_count_ = 0;
        if (fp_nop_count_ < 20) {
            fprintf(stderr, "[FP-NOP] op=0x%08x pc=0x%llx\n", op,
                    (unsigned long long)cpu.pc);
        }
        fp_nop_count_++;
    }
    (void)sf_val; (void)rm;
    return;
}
} // namespace arm64emu
