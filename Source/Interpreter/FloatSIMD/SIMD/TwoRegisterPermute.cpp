// TwoRegisterPermute.cpp — SIMD_DP group 2: the two-register misc
// switch (sub2), EXT/INS, TBL/TBX and the other permutes that follow it.
#include "Interpreter/FloatSIMD/FloatCommon.hpp"
namespace arm64emu {
bool Emulator::exec_simd_misc(const SimdDpFields& f, CPU& cpu) {
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
    // Sub-discriminator for 1-source vector ops (REV/CNT/CMEQ#0).
    // Mask off Q (30), U (29), size (23:22), Rm (20:16), Rn (9:5), Rd (4:0).
    //
    // size field (bits 23:22) or the U bit (29). This caused:
    //   - REV64 v0.4s (size=2) to not match the REV64 constant (size=0)
    //   - REV32 (U=1) to not match the REV64 case (U=0)
    // Every REV with size != 0 or U=1 fell through and was silently
    // NOP'd. This broke byte-swapping in MD5, SHA, and any NEON
    // code using REV32/REV64 on non-8-bit data.
    uint32_t sub2 = op & 0x9F3FFC00;  // mask Q, U, size, Rm, Rn, Rd
    switch (sub2) {
    // ── REV64 (vector, U=0) / REV32 (vector, U=1) ──
    // Both have bits[21:16] = 100000, bits[15:10] = 001000.
    // U bit (29) distinguishes them: REV64 (U=0) reverses within
    // 64-bit containers, REV32 (U=1) reverses within 32-bit words.
    // Since we masked off U, we check it explicitly inside.
    case 0x0E200800: {  // bits[21:16]=10, bits[15:10]=001000
        bool is_rev32 = (op >> 29) & 1;  // U=1 → REV32
        // size field determines the element size for reversal:
        //   size=0 → 8-bit elements, size=1 → 16-bit,
        //   size=2 → 32-bit, size=3 → 64-bit (REV64 only, reserved for REV32)
        // REV64 reverses elements within each 64-bit container.
        // REV32 reverses elements within each 32-bit word.
        int esize = (1 << size);  // bytes per element
        int container = is_rev32 ? 4 : 8;  // REV32: 32-bit, REV64: 64-bit
        uint8_t buf[16];
        memcpy(buf, &cpu.v_lo[rn], 8);
        if (Q) memcpy(buf + 8, &cpu.v_hi[rn], 8);
        int nbytes = Q ? 16 : 8;
        for (int i = 0; i < nbytes; i += container) {
            // Reverse elements within this container
            for (int j = 0; j < container / 2; j += esize) {
                for (int k = 0; k < esize; k++) {
                    std::swap(buf[i + j + k],
                              buf[i + container - esize - j + k]);
                }
            }
        }
        memcpy(&cpu.v_lo[rd], buf, 8);
        if (Q) memcpy(&cpu.v_hi[rd], buf + 8, 8);
        else cpu.v_hi[rd] = 0;
        return true;
    }
    // ── REV16 (vector) ──
    case 0x0E201800: {
        uint8_t buf[16];
        memcpy(buf, &cpu.v_lo[rn], 8);
        if (Q) memcpy(buf + 8, &cpu.v_hi[rn], 8);
        int nbytes = Q ? 16 : 8;
        for (int i = 0; i < nbytes; i += 2) std::swap(buf[i], buf[i+1]);
        memcpy(&cpu.v_lo[rd], buf, 8);
        if (Q) memcpy(&cpu.v_hi[rd], buf + 8, 8);
        else cpu.v_hi[rd] = 0;
        return true;
    }
    // ── CNT / NOT / RBIT (vector) ──
    // The sub2 mask (0x9F3FFC00) masks off size (bit 22) and U
    // (bit 29), so CNT/NOT/RBIT all collapse to 0x0E205800:
    //   CNT  = 0x0E205800 (U=0, size=0) — popcount per byte
    //   NOT  = 0x6E205800 (U=1, size=0) — bitwise invert
    //   RBIT = 0x6E605800 (U=1, size=1) — bit-reverse per byte
    // v0 silently ran popcount for RBIT (vrbitq_u8 gave 0 1 1 2
    // instead of 0 128 64). Distinguish by the raw op bits.
    case 0x0E205800: {
        bool is_rbit = ((op >> 22) & 1) == 1;   // size=1
        bool is_not  = !is_rbit && ((op >> 29) & 1) == 1;
        uint8_t buf[16];
        memcpy(buf, &cpu.v_lo[rn], 8);
        if (Q) memcpy(buf + 8, &cpu.v_hi[rn], 8);
        int nbytes = Q ? 16 : 8;
        for (int i = 0; i < nbytes; i++) {
            if (is_rbit) {
                // Reverse the 8 bits of each byte.
                uint8_t v = buf[i];
                v = ((v & 0xAA) >> 1) | ((v & 0x55) << 1);
                v = ((v & 0xCC) >> 2) | ((v & 0x33) << 2);
                v = ((v & 0xF0) >> 4) | ((v & 0x0F) << 4);
                buf[i] = v;
            } else if (is_not) {
                buf[i] = ~buf[i];
            } else {
                buf[i] = __builtin_popcount(buf[i]);
            }
        }
        memcpy(&cpu.v_lo[rd], buf, 8);
        if (Q) memcpy(&cpu.v_hi[rd], buf + 8, 8);
        else cpu.v_hi[rd] = 0;
        return true;
    }
    // ── CMEQ vs zero ──
    // but actual CMEQ #0 encoding (e.g. 0x4e209801) has bits[15:10]=0x26.
    // The old case NEVER matched — CMEQ #0 was silently NOP'd, breaking
    // glibc's strlen SIMD path. Corrected to 0x0E209800.
    case 0x0E209800: {
        int esize = (size == 0) ? 1 : (size == 1 ? 2 : (size == 2 ? 4 : 8));
        int elems = (Q ? 16 : 8) / esize;
        uint8_t buf[16];
        memcpy(buf, &cpu.v_lo[rn], 8);
        if (Q) memcpy(buf + 8, &cpu.v_hi[rn], 8);
        for (int i = 0; i < elems; i++) {
            bool is_zero = true;
            for (int b = 0; b < esize; b++) {
                if (buf[i*esize + b] != 0) { is_zero = false; break; }
            }
            memset(buf + i*esize, is_zero ? 0xFF : 0x00, esize);
        }
        memcpy(&cpu.v_lo[rd], buf, 8);
        if (Q) memcpy(&cpu.v_hi[rd], buf + 8, 8);
        else cpu.v_hi[rd] = 0;
        return true;
    }
    // ── Integer compare against zero (SIMD 2-reg misc) ───────
    // CMGT/CMGE/CMLT/CMLE <Vd>.<T>, <Vn>.<T>, #0: per-element
    // signed comparison against 0; each lane becomes all-ones if
    // the test passes, all-zeros otherwise. Encodings
    // (0 Q U 01110 size 1 0 Rn opcode Rd, opcode = bits[15:10]):
    //   CMGT (signed >):  0x0E208800 (U=0, opcode 100010)
    //   CMGE (signed >=): 0x2E208800 (U=1, opcode 100010)
    //   CMLE (signed <=): 0x2E209800 (U=1, opcode 100110)
    //   CMLT (signed <):  0x0E20A800 (U=0, opcode 101010)
    // The size bits (23:22) land in sub_noq, so each size variant
    // is its own case label. The source register (Vn) is in the
    // 2-reg-misc Rn field (bits[9:5]) which the decoder maps to
    // `rn`. These were previously unhandled → DecodeError (SIGILL)
    // — Qt5Core's text handling lowers `v<0` to CMLT #0.
    case 0x0E208800: case 0x0E608800: case 0x0EA08800: case 0x0EE08800:
    case 0x2E208800: case 0x2E608800: case 0x2EA08800: case 0x2EE08800:
    case 0x2E209800: case 0x2E609800: case 0x2EA09800: case 0x2EE09800:
    case 0x0E20A800: case 0x0E60A800: case 0x0EA0A800: case 0x0EE0A800: {
        int esize = 1 << size;
        int elems = (Q ? 16 : 8) / esize;
        uint8_t buf[16];
        memcpy(buf, &cpu.v_lo[rn], 8);
        if (Q) memcpy(buf + 8, &cpu.v_hi[rn], 8);
        int opc = (op >> 10) & 0x3F;
        bool is_u = U;
        // opcode 100010: CMGT(U=0) / CMGE(U=1); 101010: CMLT(U=0);
        // 100110: CMLE(U=1). cmp: 0=GT 1=GE 2=LT 3=LE.
        int cmp;
        if (opc == 0x22)      cmp = is_u ? 1 : 0;
        else if (opc == 0x2A) cmp = 2;
        else                  cmp = 3;
        for (int i = 0; i < elems; i++) {
            int64_t v = 0;
            memcpy(&v, buf + i * esize, esize);
            int64_t sv = (esize == 8) ? v
                         : (esize == 4) ? (int32_t)v
                         : (esize == 2) ? (int16_t)v
                                        : (int8_t)v;
            bool pass = (cmp == 0) ? (sv > 0)
                      : (cmp == 1) ? (sv >= 0)
                      : (cmp == 2) ? (sv < 0)
                                   : (sv <= 0);
            memset(buf + i * esize, pass ? 0xFF : 0x00, esize);
        }
        memcpy(&cpu.v_lo[rd], buf, 8);
        if (Q) memcpy(&cpu.v_hi[rd], buf + 8, 8);
        else cpu.v_hi[rd] = 0;
        return true;
    }
    // ── SMAXV/SMINV/UMAXV/UMINV (across-lanes reduction) ─────
    // Compute the maximum (bit16=0) or minimum (bit16=1) of all
    // elements of Vn and write the scalar result to element 0 of
    // Rd (upper bits zeroed). U selects signed (SMAXV/SMINV) vs
    // unsigned (UMAXV/UMINV). size selects 8/16/32-bit elements;
    // Q selects 8 vs 16 bytes of source lanes.
    // Encoding: 0 Q U 01110 0 size 00000 <max/min> 10101 0 Vn Rd
    //   SMAXV s0,v1.4s = 0x4EB0A820, SMINV = 0x4EB1A820
    // The sub2 mask (0x9F3FFC00) strips Q/U/size, so both size
    // variants land on these two labels; read size/U off raw op.
    case 0x0E30A800:
    case 0x0E31A800: {
        int esize = 1 << size;          // 1 (B), 2 (H), 4 (S)
        int elems = (Q ? 16 : 8) / esize;
        bool is_unsigned = (op >> 29) & 1;
        bool is_min = (op >> 16) & 1;
        uint8_t buf[16];
        memcpy(buf, &cpu.v_lo[rn], 8);
        if (Q) memcpy(buf + 8, &cpu.v_hi[rn], 8);
        uint64_t best = 0;
        memcpy(&best, buf, esize);
        for (int i = 1; i < elems; i++) {
            uint64_t v = 0;
            memcpy(&v, buf + i * esize, esize);
            if (is_unsigned) {
                if (is_min ? (v < best) : (v > best)) best = v;
            } else {
                int64_t sv, sb;
                if (esize == 1)      { sv = (int8_t)v;       sb = (int8_t)best; }
                else if (esize == 2) { sv = (int16_t)v;      sb = (int16_t)best; }
                else                 { sv = (int32_t)v;      sb = (int32_t)best; }
                if (is_min ? (sv < sb) : (sv > sb)) best = v;
            }
        }
        cpu.v_lo[rd] = 0;
        cpu.v_hi[rd] = 0;
        memcpy(&cpu.v_lo[rd], &best, esize);
        return true;
    }
    default: break;
    }
    // ADDV: horizontal sum truncates to the source lane width.
    if ((op & 0xBF3FFC00) == 0x0E31B800) {
        int esize = 1 << size;
        int elems = (Q ? 16 : 8) / esize;
        uint8_t buf[16];
        memcpy(buf, &cpu.v_lo[rn], 8);
        if (Q) memcpy(buf + 8, &cpu.v_hi[rn], 8);
        uint64_t sum = 0;
        for (int i = 0; i < elems; i++) {
            uint64_t v = 0;
            memcpy(&v, buf + i * esize, esize);
            sum += v;
        }
        cpu.v_lo[rd] = sum & ((1ULL << (esize*8))-1);
        cpu.v_hi[rd] = 0;
        return true;
    }
    // SADDLV/UADDLV: widen the horizontal sum to twice the lane
    // width. Keep bit16 in the match: dropping it aliases these
    // reductions with the later SUQADD/USQADD accumulator handler.
    if ((op & 0x9F3FFC00) == 0x0E303800) {
        if (size == 3 || (size == 2 && !Q))
            throw DecodeError(cpu.pc, op);
        const int esize = 1 << size;
        const int bits = esize * 8;
        const int elems = (Q ? 16 : 8) / esize;
        const bool uns = (op >> 29) & 1;
        uint8_t source[16] = {};
        memcpy(source, &cpu.v_lo[rn], 8);
        if (Q) memcpy(source + 8, &cpu.v_hi[rn], 8);
        int64_t sum = 0;
        for (int i = 0; i < elems; ++i) {
            uint64_t value = 0;
            memcpy(&value, source + i * esize, esize);
            if (!uns && (value & (1ULL << (bits - 1))))
                sum += static_cast<int64_t>(value) - (1LL << bits);
            else
                sum += static_cast<int64_t>(value);
        }
        const uint64_t mask = bits == 32 ? ~0ULL : (1ULL << (bits * 2)) - 1;
        cpu.v_lo[rd] = static_cast<uint64_t>(sum) & mask;
        cpu.v_hi[rd] = 0;
        return true;
    }
    // INS (element, vector): mov vd.<T>[i], vn.<T>[j]
    // Encoding: 0 Q 0 0 1 1 1 0 0 0 0 imm5 0 <src_byte_off[3:0]> 1 Rn Rd
    // Same 0x2E000000 prefix as EXT but bit10=1 (EXT keeps bit10=0
    // and puts imm4 in bits[14:11]). src byte offset = j*esize.
    // GCC lowers vextq_u8 to a 2-source TBL whose index vector is
    // built with these INS moves, so this must not fall into EXT.
    if ((op & 0xBFE00000) == 0x2E000000 && (op & 0x400) != 0) {
        uint8_t imm5 = (op >> 16) & 0x1F;
        int esize, didx;
        // imm5 = (didx << (size+1)) | (1 << size): the lowest set
        // bit selects the element size, the upper bits hold the
        // destination element index.
        if (imm5 & 0x01) { esize = 1; didx = imm5 >> 1; }
        else if (imm5 & 0x02) { esize = 2; didx = imm5 >> 2; }
        else if (imm5 & 0x04) { esize = 4; didx = imm5 >> 3; }
        else if (imm5 & 0x08) { esize = 8; didx = imm5 >> 4; }
        else { dump_decode_error(cpu, mem_, op);
            throw DecodeError(cpu.pc, op); }
        int src_off = (op >> 11) & 0xF;
        int sidx = src_off / esize;
        uint64_t src_val;
        int elems_per_qword = 8 / esize;
        if (sidx < elems_per_qword) {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(&cpu.v_lo[rn]);
            memcpy(&src_val, p + sidx * esize, esize);
        } else {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(&cpu.v_hi[rn]);
            memcpy(&src_val, p + (sidx - elems_per_qword) * esize, esize);
        }
        uint8_t* d = (didx < elems_per_qword)
            ? reinterpret_cast<uint8_t*>(&cpu.v_lo[rd])
            : reinterpret_cast<uint8_t*>(&cpu.v_hi[rd]);
        memcpy(d + (didx % elems_per_qword) * esize, &src_val, esize);
        return true;
    }
    // EXT (extract) — v0 mask 0xFFE00000 val 0x6E000000
    // MISSED Q=0 form. Fixed: strip Q from the comparison.
    if ((op & 0xBFE00000) == 0x2E000000) {
        uint8_t imm4 = (op >> 11) & 0xF;
        uint8_t buf[32];
        // EXT forms Vn || Vm for both vector lengths. Q=0 uses
        // only the low 64 bits of each source; Q=1 uses all 128.
        memcpy(buf, &cpu.v_lo[rn], 8);
        if (Q) {
            memcpy(buf + 8, &cpu.v_hi[rn], 8);
            memcpy(buf + 16, &cpu.v_lo[rm], 8);
            memcpy(buf + 24, &cpu.v_hi[rm], 8);
        } else {
            // Do not insert Vn's unused high half between sources.
            memcpy(buf + 8, &cpu.v_lo[rm], 8);
        }
        uint8_t out[16] = {0};
        int nbytes = Q ? 16 : 8;
        memcpy(out, buf + imm4, nbytes);
        memcpy(&cpu.v_lo[rd], out, 8);
        if (Q) memcpy(&cpu.v_hi[rd], out + 8, 8);
        else cpu.v_hi[rd] = 0;
        return true;
    }
    // ZIP1/ZIP2 / UZP1/UZP2 / TRN1/TRN2 — permute pairs.
    // Encoding: 0 Q size 01110 00 Rm 0 opc 10 Rn Rd (U=0).
    // opc6 = bits[15:10] (empirically from gas):
    //   UZP1=0x06 TRN1=0x0A ZIP1=0x0E
    //   UZP2=0x16 TRN2=0x1A ZIP2=0x1E
    if (((op >> 24) & 0x1F) == 0x0E && ((op >> 21) & 1) == 0
        && ((op >> 29) & 1) == 0) {
        uint8_t opc6 = (op >> 10) & 0x3F;
        if (opc6 == 0x06 || opc6 == 0x0A || opc6 == 0x0E
            || opc6 == 0x16 || opc6 == 0x1A || opc6 == 0x1E) {
            int esize = 1 << size;
            int pairs = (Q ? 16 : 8) / (esize * 2);
            if (pairs < 1) pairs = 1;
            uint8_t a[16], b[16], out[16] = {0};
            memcpy(a, &cpu.v_lo[rn], 8);
            if (Q) memcpy(a + 8, &cpu.v_hi[rn], 8);
            memcpy(b, &cpu.v_lo[rm], 8);
            if (Q) memcpy(b + 8, &cpu.v_hi[rm], 8);
            int part = (opc6 == 0x16 || opc6 == 0x1A || opc6 == 0x1E) ? 1 : 0;
            bool is_zip = (opc6 == 0x0E || opc6 == 0x1E);
            bool is_uzp = (opc6 == 0x06 || opc6 == 0x16);
            // TRN otherwise.
            if (is_zip) {
                // ZIP1: interleave low halves; ZIP2: high halves.
                int base = part * pairs * esize;
                for (int i = 0; i < pairs; i++) {
                    memcpy(out + (2 * i) * esize, a + base + i * esize, esize);
                    memcpy(out + (2 * i + 1) * esize, b + base + i * esize, esize);
                }
            } else if (is_uzp) {
                // UZP1: even elements from a||b; UZP2: odd.
                int total = pairs * 2;
                int out_i = 0;
                for (int src = 0; src < 2; src++) {
                    const uint8_t* s = src ? b : a;
                    for (int i = 0; i < total; i++) {
                        if ((i & 1) == part) {
                            memcpy(out + out_i * esize, s + i * esize, esize);
                            out_i++;
                        }
                    }
                }
            } else {
                // TRN1/TRN2: transpose 2x2 element pairs.
                for (int i = 0; i < pairs; i++) {
                    const uint8_t* s0 = a + (2 * i + part) * esize;
                    const uint8_t* s1 = b + (2 * i + part) * esize;
                    memcpy(out + (2 * i) * esize, s0, esize);
                    memcpy(out + (2 * i + 1) * esize, s1, esize);
                }
            }
            memcpy(&cpu.v_lo[rd], out, 8);
            if (Q) memcpy(&cpu.v_hi[rd], out + 8, 8);
            else cpu.v_hi[rd] = 0;
            return true;
        }
    }
    return false;
}
} // namespace arm64emu
