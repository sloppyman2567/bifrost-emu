// ThreeRegister.cpp — SIMD_DP group 1: the early special cases
// (FCVTL, SHLL, FCVTN/FCVTXN, FRINT*, ...) and the three-same switch on
// sub_noq. execute_fp runs the groups in order; the first that returns
// true handled the instruction.
#include "Interpreter/FloatSIMD/FloatCommon.hpp"
namespace arm64emu {
bool Emulator::exec_simd_three(const SimdDpFields& f, CPU& cpu) {
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
    // FP across-lanes reductions, single precision (the half forms need
    // FEAT_FP16): FMAXNMV 0x6E30C800, FMINNMV 0x6EB0C800, FMAXV 0x6E30F800,
    // FMINV 0x6EB0F800; Q=0 is reserved. Arm's Reduce() pairs lanes as a
    // tree, op(op(e0, e1), op(e2, e3)), which decides NaN and signed-zero
    // results. FMINNMV used to reach an unrelated handler; the others trapped.
    {
        int which = -1;
        switch (op & 0xBFFFFC00) {
            case 0x2E30C800: which = 0; break;
            case 0x2EB0C800: which = 1; break;
            case 0x2E30F800: which = 2; break;
            case 0x2EB0F800: which = 3; break;
        }
        if (which >= 0) {
            if (!Q) throw DecodeError(cpu.pc, op);
            static constexpr ArmFpOp reduce[] = {ArmFpOp::MaxNum, ArmFpOp::MinNum,
                                                 ArmFpOp::Max, ArmFpOp::Min};
            const ArmFpOp fop = reduce[which];
            const bool dn = (cpu.fpcr >> 25) & 1;
            float e[4];
            memcpy(e, &cpu.v_lo[rn], 8);
            memcpy(e + 2, &cpu.v_hi[rn], 8);
            const float r = arm_fp_binop(fop, arm_fp_binop(fop, e[0], e[1], dn),
                                         arm_fp_binop(fop, e[2], e[3], dn), dn);
            uint32_t bits;
            memcpy(&bits, &r, 4);
            cpu.v_lo[rd] = bits;
            cpu.v_hi[rd] = 0;
            return true;
        }
    }
    // FCVTL (sz=0) widens four halves from the selected half to singles.
    // It used to fall through to an unrelated handler and return garbage
    // (SpongeBob's half-float atlas repack). Keep bit16 here too.
    if ((op & 0xBFFFFC00) == 0x0E217800) {
        const uint64_t source = Q ? cpu.v_hi[rn] : cpu.v_lo[rn];
        const bool dn = (cpu.fpcr >> 25) & 1;
        uint32_t out[4];
        for (int i = 0; i < 4; ++i) {
            const float value = h2f(static_cast<uint16_t>(source >> (16 * i)), dn);
            memcpy(&out[i], &value, 4);
        }
        cpu.v_lo[rd] = out[0] | static_cast<uint64_t>(out[1]) << 32;
        cpu.v_hi[rd] = out[2] | static_cast<uint64_t>(out[3]) << 32;
        return true;
    }
    // FCVTN (sz=0) narrows four singles to halves: Q=0 writes the low half
    // and zeroes the top, Q=1 (FCVTN2) writes the top and keeps the low.
    // FCVTXN has no sz=0 form; its encoding stays undefined below.
    if ((op & 0xBFFFFC00) == 0x0E216800) {
        const bool dn = (cpu.fpcr >> 25) & 1;
        uint64_t result = 0;
        for (int i = 0; i < 4; ++i) {
            const uint64_t raw = i < 2 ? cpu.v_lo[rn] : cpu.v_hi[rn];
            const uint32_t bits = static_cast<uint32_t>(raw >> (32 * (i & 1)));
            float value; memcpy(&value, &bits, 4);
            result |= static_cast<uint64_t>(f2h(value, dn)) << (16 * i);
        }
        if (Q) cpu.v_hi[rd] = result;
        else { cpu.v_lo[rd] = result; cpu.v_hi[rd] = 0; }
        return true;
    }
    if ((op & 0xBFFFFC00) == 0x2E216800) throw DecodeError(cpu.pc, op);  // FCVTXN sz=0
    // FCVTL widens two floats from the selected half to doubles.
    // Keep bit16 to avoid the integer two-register-misc handlers.
    if ((op & 0xBFFFFC00) == 0x0E617800) {
        uint64_t source = Q ? cpu.v_hi[rn] : cpu.v_lo[rn];
        float values[2]; memcpy(values, &source, 8);
        const bool dn = (cpu.fpcr >> 25) & 1;
        double a = arm_fp_convert<double>(values[0], dn);
        double b = arm_fp_convert<double>(values[1], dn);
        memcpy(&cpu.v_lo[rd], &a, 8); memcpy(&cpu.v_hi[rd], &b, 8);
        return true;
    }
    // SHLL widens unsigned lanes and shifts by their original width.
    // Its bit16 distinguishes it from saturating accumulate-add.
    if ((op & 0xBF3FFC00) == 0x2E213800 && size < 3) {
        unsigned esize = 1u << size;
        uint64_t source = Q ? cpu.v_hi[rn] : cpu.v_lo[rn];
        uint8_t result[16]{};
        for (unsigned i=0; i<8/esize; ++i) {
            uint64_t value=0;
            memcpy(&value, reinterpret_cast<const uint8_t*>(&source)+i*esize, esize);
            value <<= esize*8; memcpy(result+i*esize*2, &value, esize*2);
        }
        memcpy(&cpu.v_lo[rd], result, 8); memcpy(&cpu.v_hi[rd], result+8, 8);
        return true;
    }
    // ── FP narrowing convert (FCVTN / FCVTXN, d -> s) ────────
    // NOTE: these CANNOT go through the sub_noq switch — their
    // opcode uses bits[20:16] (=00001), which the 0xFFE0FC00
    // mask clears (0x0E616844 masks to 0x0E606800). Match on
    // op directly, stripping ONLY Q (bit30 — 0xBFFFFF00);
    // U (bit29) picks FCVTN vs FCVTXN. Verified with the cross
    // assembler:
    //   fcvtn  v5.2s,v3.2d = 0x0E616865 / fcvtn2 .4s 0x4E6168xx
    //   fcvtxn v6.2s,v7.2d = 0x2E6168E6 / fcvtxn2 .4s 0x6E6168xx
    // Narrowing semantics (same family as ADDHN/XTN): BOTH
    // source doubles (Vn low AND high qwords) are ALWAYS
    // converted — Q only selects the destination half:
    //   Q=0 → result in LOW half; FCVTN zeroes the top half,
    //     FCVTXN leaves it UNCHANGED (per ARM ARM).
    //   Q=1 (*2 variants) → result in HIGH half, LOW preserved.
    // Mask 0xBFFFFC00 strips Q (bit30) AND Rn/Rd (bits 9:0).
    // The old 0xBFFFFF00 kept Rn bits [9:8], so every source
    // register >= v8 silently missed (vkQuake's
    // `fcvtn v31.2s,v21.2d` = 0x0e616bbf DecodeError'd).
    if ((op & 0xBFFFFC00) == 0x0E616800 ||
        (op & 0xBFFFFC00) == 0x2E616800) {
        const bool rto = ((op >> 29) & 1) != 0;
        const bool dn = (cpu.fpcr >> 25) & 1;
        uint32_t out[2];
        for (int i = 0; i < 2; i++) {
            uint64_t raw = (i == 0) ? cpu.v_lo[rn] : cpu.v_hi[rn];
            double dv;
            memcpy(&dv, &raw, 8);
            float fv = rto ? arm_fp_dn(fp_to_f32_rto(dv), dn)
                           : arm_fp_convert<float>(dv, dn);
            memcpy(&out[i], &fv, 4);
        }
        const uint64_t res =
            (uint64_t)out[0] | ((uint64_t)out[1] << 32);
        if (Q) cpu.v_hi[rd] = res;
        else {
            cpu.v_lo[rd] = res;
            cpu.v_hi[rd] = 0;  // FCVTXN too: Q=0 clears the top half
        }
        return true;
    }
    // FCVTNS/NU, FCVTPS/PU, FCVTMS/MU, FCVTAS/AU (vector): round with the
    // instruction's mode, independent of FPCR.RMode, then saturate.
    // Skia's icon rasterizer uses the in-place FCVTNU .4s form. FCVTZS/ZU
    // live in Arithmetic.cpp; bit 23 set on the A opcode is URECPE/URSQRTE.
    // Q=0,.d is reserved.
    int fcvt_mode = -1;  // arm_frint mode
    switch (op & 0x9FBFFC00) {
        case 0x0E21A800: fcvt_mode = 0; break;  // N
        case 0x0EA1A800: fcvt_mode = 6; break;  // P
        case 0x0E21B800: fcvt_mode = 2; break;  // M
        case 0x0E21C800: fcvt_mode = 1; break;  // A
    }
    if (fcvt_mode >= 0) {
        const bool dbl = (op >> 22) & 1;
        if (dbl && !Q) throw DecodeError(cpu.pc, op);
        uint8_t source[16], result[16] = {};
        memcpy(source, &cpu.v_lo[rn], 8);
        memcpy(source + 8, &cpu.v_hi[rn], 8);
        RecipFpEnv environment(cpu);
        fesetround(FE_TONEAREST);
        const int bytes = dbl ? 8 : 4;
        const double limit = dbl ? 18446744073709551616.0 : 4294967296.0;
        for (int i = 0; i < (Q ? 16 : 8) / bytes; ++i) {
            double value;
            if (dbl) { memcpy(&value, source + i * bytes, 8); value = recip_input(cpu, value); }
            else { float f; memcpy(&f, source + i * bytes, 4); value = recip_input(cpu, f); }
            double rounded = arm_frint(fcvt_mode, value, false);
            const double lo = U ? 0 : -limit / 2, hi = U ? limit : limit / 2;
            if (std::isnan(value) || rounded < lo || rounded >= hi) cpu.fpsr |= 1;
            else if (rounded != value) cpu.fpsr |= 16;
            uint64_t integer = U ? fp_to_unsigned_sat(rounded, dbl)
                                 : static_cast<uint64_t>(fp_to_signed_sat(rounded, dbl));
            memcpy(result + i * bytes, &integer, bytes);
        }
        memcpy(&cpu.v_lo[rd], result, 8);
        memcpy(&cpu.v_hi[rd], result + 8, 8);
        return true;
    }
    // ── Vector FRINTN/A/M/I/Z/X (round to integral) ──
    // Checked BEFORE the sub_noq switch: FRINTx shares sub_noq
    // with CMGT/CMGE/CMLE-vs-zero (they differ only in bit16,
    // which the sub mask drops), so switch cases cannot express
    // it. arm_frint_vector_mode matches U/bit23/bit16/opc (drops Q,
    // sz and the register fields) instead. Width from bit22
    // (0=single .2s/.4s, 1=double .2d). All 12 verified against
    // the cross assembler, e.g. `frinta v31.2d` = 0x6E618BFF.
    // libmvec _ZGVnN2v_sin needs FRINTA .2d; without this it
    // threw DecodeError right after the facge fix.
    // Half-precision lives in a different slice — untouched.
    if (const int frmode = arm_frint_vector_mode(op); frmode >= 0) {
        const bool frdbl = (op >> 22) & 1;
        // Q=0+double is scalar space (never SIMD_DP); reject
        // like the old fallthrough did.
        if (frdbl && !Q) {
            dump_decode_error(cpu, mem_, op);
            throw DecodeError(cpu.pc, op);
        }
        const bool dn = (cpu.fpcr >> 25) & 1;
        const int fesz = frdbl ? 8 : 4;
        const int felems = (Q ? 16 : 8) / fesz;
        uint8_t fbuf[16];
        memcpy(fbuf, &cpu.v_lo[rn], 8);
        if (Q) memcpy(fbuf + 8, &cpu.v_hi[rn], 8);
        for (int i = 0; i < felems; i++) {
            if (!frdbl) {
                float a; memcpy(&a, fbuf + i * 4, 4);
                const float r = arm_frint(frmode, a, dn);
                memcpy(fbuf + i * 4, &r, 4);
            } else {
                double a; memcpy(&a, fbuf + i * 8, 8);
                const double r = arm_frint(frmode, a, dn);
                memcpy(fbuf + i * 8, &r, 8);
            }
        }
        memcpy(&cpu.v_lo[rd], fbuf, 8);
        if (Q) memcpy(&cpu.v_hi[rd], fbuf + 8, 8);
        else cpu.v_hi[rd] = 0;
        return true;
    }
    // SADDLP/UADDLP/SADALP/UADALP: pair adjacent lanes, widen,
    // optionally accumulate. Keep bits[20:16] in the match: bit16
    // distinguishes ADDLP from XTN/SQXTUN, which the switch's mask
    // otherwise discards. Snapshot both operands for Rd == Rn.
    if ((op & 0x9F3FBC00) == 0x0E202800) {
        if (size == 3) throw DecodeError(cpu.pc, op);
        const int bytes = 1 << size;
        const int bits = bytes * 8;
        const int wide_bytes = bytes * 2;
        const int pairs = (Q ? 16 : 8) / wide_bytes;
        const bool uns = (op >> 29) & 1;
        const bool accumulate = (op >> 14) & 1;
        uint8_t source[16], accumulator[16], result[16] = {};
        memcpy(source, &cpu.v_lo[rn], 8);
        memcpy(source + 8, &cpu.v_hi[rn], 8);
        memcpy(accumulator, &cpu.v_lo[rd], 8);
        memcpy(accumulator + 8, &cpu.v_hi[rd], 8);
        for (int i = 0; i < pairs; ++i) {
            int64_t sum = 0;
            for (int lane = 0; lane < 2; ++lane) {
                uint64_t value = 0;
                memcpy(&value, source + (2 * i + lane) * bytes, bytes);
                sum += !uns && (value & (1ULL << (bits - 1)))
                    ? static_cast<int64_t>(value) - (1LL << bits)
                    : static_cast<int64_t>(value);
            }
            uint64_t value = 0;
            if (accumulate)
                memcpy(&value, accumulator + i * wide_bytes, wide_bytes);
            value += static_cast<uint64_t>(sum);
            memcpy(result + i * wide_bytes, &value, wide_bytes);
        }
        memcpy(&cpu.v_lo[rd], result, 8);
        memcpy(&cpu.v_hi[rd], result + 8, 8);
        return true;
    }
    // CLS (U=0) / CLZ (U=1), vector: opcode 00100. Like ADDLP, they
    // differ from SQXTN/UQXTN (opcode 10100) only in bit16, which the
    // switch key discards, so match them first with bit16 kept.
    if ((op & 0x9F3FFC00) == 0x0E204800) {
        if (size == 3) throw DecodeError(cpu.pc, op);
        const int bits = 8 << size;
        const uint64_t mask = (1ULL << bits) - 1;  // bits <= 32
        const bool clz = (op >> 29) & 1;
        uint64_t halves[2] = {cpu.v_lo[rn], cpu.v_hi[rn]};
        for (int h = 0; h < (Q ? 2 : 1); ++h) {
            uint64_t& half = halves[h];
            uint64_t out = 0;
            for (int at = 0; at < 64; at += bits) {
                uint64_t v = (half >> at) & mask;
                if (!clz && (v >> (bits - 1)))  // CLS counts sign copies
                    v = ~v & mask;
                int n = 0;
                for (int b = bits - 1; b >= 0 && !((v >> b) & 1); --b) ++n;
                out |= static_cast<uint64_t>(clz ? n : n - 1) << at;
            }
            half = out;
        }
        cpu.v_lo[rd] = halves[0];
        cpu.v_hi[rd] = Q ? halves[1] : 0;  // Q=0 zeroes the upper 64
        return true;
    }
    switch (sub_noq) {
    // ── DUP (general): sf 0 0 11110 00 0 imm5 0000 0 1 Rn Rd ──
    // v0 only matched Q=0 (mask 0xFFE0FC00 val 0x0E000C00).
    // Fixed: strip Q, match both forms.
    case 0x0E000C00: {
        // The lowest set bit of imm5 selects the size; higher bits are
        // ignored. imm5 = x0000 and the 64-bit form without Q are reserved.
        uint8_t imm5 = (op >> 16) & 0x1F;
        if ((imm5 & 0xF) == 0 || ((imm5 & 0xF) == 8 && !Q)) {
            dump_decode_error(cpu, mem_, op);
            throw DecodeError(cpu.pc, op);
        }
        int esize = 1 << __builtin_ctz(static_cast<unsigned>(imm5));
        int elems = (Q ? 16 : 8) / esize;
        uint64_t src = cpu.regs[rn];
        uint8_t bytes[16] = {0};
        for (int i = 0; i < elems; i++) {
            uint64_t v = src;
            if (esize == 1) v = src & 0xFF;
            else if (esize == 2) v = src & 0xFFFF;
            else if (esize == 4) v = src & 0xFFFFFFFF;
            memcpy(bytes + i * esize, &v, esize);
        }
        memcpy(&cpu.v_lo[rd], bytes, 8);
        if (Q) memcpy(&cpu.v_hi[rd], bytes + 8, 8);
        else cpu.v_hi[rd] = 0;
        return true;
    }
    // ── DUP (element): sf 0 0 11110 1 0 imm5 0000 1 1 Rn Rd ──
    // Copies one element from Vn to all lanes of Vd.
    // Encoding base: 0x4E000400 (Q=1) / 0x0E000400 (Q=0).
    // default and got silently NOP'd. This broke the vectorized
    // TLS init pattern `dup vN.2d, vM.d[0]` used by GCC -O2 to
    // broadcast a base value before adding an index vector,
    // causing multi-element __thread TLS arrays to get corrupted
    // (only lane 0 was correct, lanes 1+ stayed 0 or stale).
    case 0x0E000400: {
        // imm5 = (index << (size+1)) | (1 << size): lowest set bit
        // gives esize, remaining high bits give the element index.
        // The old switch only matched imm5 == {1,2,4,8} (index 0),
        // so `dup v23.2s, v1.s[1]` (imm5=12 = (1<<3)|4) threw
        // DecodeError inside sha256_finish.
        uint8_t imm5 = (op >> 16) & 0x1F;
        // imm5 = x0000 is reserved, as is a 64-bit element without Q. The
        // old loop left esize 0 (a host divide-by-zero) or 16 (an
        // overflowing copy) for these.
        if ((imm5 & 0xF) == 0 || ((imm5 & 0xF) == 8 && !Q)) {
            dump_decode_error(cpu, mem_, op);
            throw DecodeError(cpu.pc, op);
        }
        int esize = 1 << __builtin_ctz(static_cast<unsigned>(imm5)), idx = 0;
        idx = imm5 >> (esize == 1 ? 1 : (esize == 2 ? 2 : (esize == 4 ? 3 : 4)));
        // Read the source element from Vn.
        uint64_t src_val;
        int elems_per_qword = 8 / esize;
        if (idx < elems_per_qword) {
            // Source is in v_lo[rn]
            const uint8_t* p = reinterpret_cast<const uint8_t*>(&cpu.v_lo[rn]);
            memcpy(&src_val, p + idx * esize, esize);
        } else {
            // Source is in v_hi[rn] (Q must be 1)
            const uint8_t* p = reinterpret_cast<const uint8_t*>(&cpu.v_hi[rn]);
            memcpy(&src_val, p + (idx - elems_per_qword) * esize, esize);
        }
        // Broadcast src_val to all lanes of Vd.
        int elems = (Q ? 16 : 8) / esize;
        uint8_t bytes[16] = {0};
        for (int i = 0; i < elems; i++) {
            memcpy(bytes + i * esize, &src_val, esize);
        }
        memcpy(&cpu.v_lo[rd], bytes, 8);
        if (Q) memcpy(&cpu.v_hi[rd], bytes + 8, 8);
        else cpu.v_hi[rd] = 0;
        return true;
    }
    // ── INS (general): sf 0 0 11110 10 0 imm5 0000 0 1 Rn Rd ──
    // v0 case label 0x4E000C00 was unreachable (it's DUP with
    // Q=1, which strips to the same sub_noq as DUP Q=0).
    // Real INS has bits[23:22]=10, giving sub_noq=0x0E001C00.
    case 0x0E001C00: {
        uint8_t imm5 = (op >> 16) & 0x1F;
        // INS (general) exists only with Q = 1; imm5 = x0000 is reserved
        // (the old loop left esize 0, a host divide-by-zero).
        if ((imm5 & 0xF) == 0 || !Q) {
            dump_decode_error(cpu, mem_, op);
            throw DecodeError(cpu.pc, op);
        }
        int esize = 1 << __builtin_ctz(static_cast<unsigned>(imm5)), idx = 0;
        idx = imm5 >> (esize == 1 ? 1 : (esize == 2 ? 2 : (esize == 4 ? 3 : 4)));
        uint64_t src = cpu.regs[rn];
        // (8/esize) must write to v_hi, not v_lo. The old code
        // always wrote to v_lo, causing out-of-bounds writes for
        // lane indices >= 2 (32-bit) or >= 1 (64-bit). This broke
        // INS used by MD5 to load message words into vector lanes.
        int elems_per_qword = 8 / esize;
        if (esize == 1) {
            if (idx < elems_per_qword)
                reinterpret_cast<uint8_t*>(&cpu.v_lo[rd])[idx] = src & 0xFF;
            else if (Q)
                reinterpret_cast<uint8_t*>(&cpu.v_hi[rd])[idx - elems_per_qword] = src & 0xFF;
        } else if (esize == 2) {
            if (idx < elems_per_qword)
                reinterpret_cast<uint16_t*>(&cpu.v_lo[rd])[idx] = src & 0xFFFF;
            else if (Q)
                reinterpret_cast<uint16_t*>(&cpu.v_hi[rd])[idx - elems_per_qword] = src & 0xFFFF;
        } else if (esize == 4) {
            if (idx < elems_per_qword)
                reinterpret_cast<uint32_t*>(&cpu.v_lo[rd])[idx] = src & 0xFFFFFFFF;
            else if (Q)
                reinterpret_cast<uint32_t*>(&cpu.v_hi[rd])[idx - elems_per_qword] = src & 0xFFFFFFFF;
        } else if (esize == 8) {
            if (idx == 0) cpu.v_lo[rd] = src;
            else if (idx == 1 && Q) cpu.v_hi[rd] = src;
        }
        return true;
    }
    // ── ORR (vector) / MOV (vector alias): ... 0 1 Rn 0 0 0 1 1 1 0 0 0 0 0 Rm Rd
    // v0 mask 0xFF20FC00 val 0x0EA01C00 only matched Q=0.
    // Also note: v0 had a separate "ORR (vector) — full form"
    // check at 0x0EA01C00 that was unreachable (the MOV alias
    // caught it first). Merged here.
    case 0x0EA01C00: {
        cpu.v_lo[rd] = cpu.v_lo[rn] | cpu.v_lo[rm];
        if (Q) cpu.v_hi[rd] = cpu.v_hi[rn] | cpu.v_hi[rm];
        else cpu.v_hi[rd] = 0;
        return true;
    }
    // ── BIC (vector): Vd = Vn & ~Vm ──
    // Encoding: 0_Q_0_01110_01_1_Rm_000111_Rn_Rd (sub_noq=0x0E601C00)
    // never matched the actual BIC encoding (which has size=01
    // and bits[15:10]=000111, giving sub_noq=0x0E601C00). With
    // the wrong case, BIC was silently NOP'd, but no test caught
    // it because GCC -O2 usually lowers vbicq to AND + NOT rather
    // than the BIC instruction. glibc's SIMD strlen/strchr DO use
    // BIC, so the silent NOP broke these functions in subtle ways.
    case 0x0E601C00: {
        cpu.v_lo[rd] = cpu.v_lo[rn] & ~cpu.v_lo[rm];
        if (Q) cpu.v_hi[rd] = cpu.v_hi[rn] & ~cpu.v_hi[rm];
        else cpu.v_hi[rd] = 0;
        return true;
    }
    // ── ORN (vector): Vd = Vn | ~Vm ──
    // Encoding: 0_Q_0_01110_11_1_Rm_000111_Rn_Rd (sub_noq=0x0EE01C00)
    case 0x0EE01C00: {
        cpu.v_lo[rd] = cpu.v_lo[rn] | ~cpu.v_lo[rm];
        if (Q) cpu.v_hi[rd] = cpu.v_hi[rn] | ~cpu.v_hi[rm];
        else cpu.v_hi[rd] = 0;
        return true;
    }
    // ── AND (vector) ──
    case 0x0E201C00: {
        cpu.v_lo[rd] = cpu.v_lo[rn] & cpu.v_lo[rm];
        if (Q) cpu.v_hi[rd] = cpu.v_hi[rn] & cpu.v_hi[rm];
        else cpu.v_hi[rd] = 0;
        return true;
    }
    // ── EOR (vector) ──
    case 0x2E201C00: {
        cpu.v_lo[rd] = cpu.v_lo[rn] ^ cpu.v_lo[rm];
        if (Q) cpu.v_hi[rd] = cpu.v_hi[rn] ^ cpu.v_hi[rm];
        else cpu.v_hi[rd] = 0;
        return true;
    }
    // ── BSL (vector): Vd = (Vn & Vd) | (Vm & ~Vd) ──
    // Vd is the mask; for each bit, if Vd=1 take Vn bit, else take Vm bit.
    // Encoding: 0_Q_1_01110_01_1_Rm_000111_Rn_Rd (sub_noq=0x2E601C00)
    // strchr/strchrnul use BSL to combine NUL-match and char-match
    // bitmaps. Without BSL, the function returned wrong results,
    // breaking curl's URL parser ("URL using bad/illegal format").
    case 0x2E601C00: {
        uint64_t mask_lo = cpu.v_lo[rd];
        uint64_t mask_hi = Q ? cpu.v_hi[rd] : 0;
        cpu.v_lo[rd] = (cpu.v_lo[rn] & mask_lo) | (cpu.v_lo[rm] & ~mask_lo);
        if (Q) cpu.v_hi[rd] = (cpu.v_hi[rn] & mask_hi) | (cpu.v_hi[rm] & ~mask_hi);
        else cpu.v_hi[rd] = 0;
        return true;
    }
    // ── BIT (vector): Vd = (Vn & Vm) | (Vd & ~Vm) ──
    // Vm is the mask; for each bit, if Vm=1 take Vn bit, else keep Vd bit.
    // Encoding: 0_Q_1_01110_10_1_Rm_000111_Rn_Rd (sub_noq=0x2EA01C00)
    // strchr uses BIT to merge char-match bits into the NUL-match
    // bitmap (the "Bitwise Insert if True" operation). Without BIT,
    // strchr could not find ':' or other non-NUL chars, returning
    // a pointer to the NUL terminator instead.
    case 0x2EA01C00: {
        uint64_t mask_lo = cpu.v_lo[rm];
        uint64_t mask_hi = Q ? cpu.v_hi[rm] : 0;
        cpu.v_lo[rd] = (cpu.v_lo[rn] & mask_lo) | (cpu.v_lo[rd] & ~mask_lo);
        if (Q) cpu.v_hi[rd] = (cpu.v_hi[rn] & mask_hi) | (cpu.v_hi[rd] & ~mask_hi);
        else cpu.v_hi[rd] = 0;
        return true;
    }
    // ── BIF (vector): Vd = (Vn & ~Vm) | (Vd & Vm) ──
    // Vm is the mask; for each bit, if Vm=0 take Vn bit, else keep Vd bit.
    // Encoding: 0_Q_1_01110_11_1_Rm_000111_Rn_Rd (sub_noq=0x2EE01C00)
    // complement of BIT; used by glibc's strrchr and some strlen paths.
    case 0x2EE01C00: {
        uint64_t mask_lo = cpu.v_lo[rm];
        uint64_t mask_hi = Q ? cpu.v_hi[rm] : 0;
        cpu.v_lo[rd] = (cpu.v_lo[rn] & ~mask_lo) | (cpu.v_lo[rd] & mask_lo);
        if (Q) cpu.v_hi[rd] = (cpu.v_hi[rn] & ~mask_hi) | (cpu.v_hi[rd] & mask_hi);
        else cpu.v_hi[rd] = 0;
        return true;
    }
    // ── ADDP (vector) ──
    // (used when the string is near a page boundary) uses ADDP to
    // reduce CMEQ results. Without it, strlen returned wrong lengths
    // for strings near page boundaries, breaking curl's URL parser.
    // ADDP: 0 Q 0 01110 size 1 Rm 0 101111 Rn Rd (base 0x0E20BC00)
    // Adds pairwise elements from Vn and Vm, placing results in Vd.
    case 0x0E20BC00: case 0x0E60BC00:
    case 0x0EA0BC00: case 0x0EE0BC00: {
        if (size == 3 && !Q) throw DecodeError(cpu.pc, op);
        const int esize = 1 << size;
        const int elems = (Q ? 16 : 8) / esize;
        uint8_t buf_n[16], buf_m[16], buf_d[16] = {};
        memcpy(buf_n, &cpu.v_lo[rn], 8);
        if (Q) memcpy(buf_n + 8, &cpu.v_hi[rn], 8);
        memcpy(buf_m, &cpu.v_lo[rm], 8);
        if (Q) memcpy(buf_m + 8, &cpu.v_hi[rm], 8);
        // Both sources contribute even with Q=0. Snapshot them before
        // writing, then truncate each sum to its original lane width.
        for (int source = 0; source < 2; ++source) {
            const uint8_t* input = source ? buf_m : buf_n;
            for (int i = 0; i < elems / 2; ++i) {
                uint64_t a = 0, b = 0;
                memcpy(&a, input + 2 * i * esize, esize);
                memcpy(&b, input + (2 * i + 1) * esize, esize);
                const uint64_t sum = a + b;
                memcpy(buf_d + (source * elems / 2 + i) * esize, &sum, esize);
            }
        }
        memcpy(&cpu.v_lo[rd], buf_d, 8);
        if (Q) memcpy(&cpu.v_hi[rd], buf_d + 8, 8);
        else cpu.v_hi[rd] = 0;
        return true;
    }
    // ── ADDHN / RADDHN / SUBHN / RSUBHN (vector, narrowing) ────
    // Three-DIFFERENT space: 0 Q U 01110 size 1 Rm opcode 000 Rn Rd
    //   ADDHN  U=0 opcode=0100    RADDHN U=1 opcode=0100 (rounding)
    //   SUBHN  U=0 opcode=0110    RSUBHN U=1 opcode=0110 (rounding)
    // Add/subtract corresponding 2*size-byte elements of Vn and Vm,
    // take the HIGH half (optionally after adding the rounding
    // constant 2^(esize_out*8-1)), and narrow into Vd. Q selects
    // the destination half (Q=1 → "2" suffix, writes v_hi).
    // libjpeg-turbo does `addhn v16.4h, v11.4s, v9.4s`; glibc's SIMD
    // strlen narrows its CMEQ result with `addhn v2.8b, v1.8h, v1.8h`.
    // sub_noq keeps size (bits[23:22]) and bit21, so every lane
    // width is a distinct case value:
    //   size=00 (.8b/.4h/.2s dest) → 0x?E204000 / 0x?E206000
    //   size=01                    → 0x?E604000 / 0x?E606000
    //   size=10                    → 0x?EA04000 / 0x?EA06000
    case 0x0E204000: case 0x2E204000:  // ADDHN(2) / RADDHN(2), .8b
    case 0x0E604000: case 0x2E604000:  // ADDHN(2) / RADDHN(2), .4h
    case 0x0EA04000: case 0x2EA04000:  // ADDHN(2) / RADDHN(2), .2s
    case 0x0E206000: case 0x2E206000:  // SUBHN(2) / RSUBHN(2), .8b
    case 0x0E606000: case 0x2E606000:  // SUBHN(2) / RSUBHN(2), .4h
    case 0x0EA06000: case 0x2EA06000:  // SUBHN(2) / RSUBHN(2), .2s
    {
        bool is_sub = (op & 0x2000) != 0;      // opcode bit13
        bool round  = (op & (1u << 29)) != 0;  // U
        int esize_in = 1 << (size + 1);        // 2, 4, 8 bytes
        int esize_out = esize_in / 2;
        int elems = 16 / esize_in;
        uint8_t buf_n[16], buf_m[16];
        memcpy(buf_n, &cpu.v_lo[rn], 8);
        memcpy(buf_n + 8, &cpu.v_hi[rn], 8);
        memcpy(buf_m, &cpu.v_lo[rm], 8);
        memcpy(buf_m + 8, &cpu.v_hi[rm], 8);
        uint64_t round_c = round ? (1ULL << (esize_out * 8 - 1)) : 0;
        uint8_t out[8];
        for (int i = 0; i < elems; i++) {
            uint64_t a = 0, b = 0;
            memcpy(&a, buf_n + i * esize_in, esize_in);
            memcpy(&b, buf_m + i * esize_in, esize_in);
            uint64_t r = (is_sub ? a - b : a + b) + round_c;
            uint64_t hi = r >> (esize_out * 8);
            memcpy(out + i * esize_out, &hi, esize_out);
        }
        if (Q) {
            memcpy(&cpu.v_hi[rd], out, 8);
        } else {
            memcpy(&cpu.v_lo[rd], out, 8);
            cpu.v_hi[rd] = 0;
        }
        return true;
    }
    // ── SSHL / USHL (vector, shift left by register) ─────────────
    // Encoding: 0 Q U 01110 size 1 Rm 000100 Rn Rd (sub_noq=0x0E204400
    // for U=0, 0x2E204400 for U=1; size selects 8b/8h/4s/2d lanes).
    // Both forms interpret only Vm's low byte as a signed count.
    // Negative counts shift right: SSHL sign-fills, USHL zero-fills.
    // Counts outside the lane width produce zero or sign fill.
    // glibc strspn lowers its SWAR "has zero / has non-matching byte"
    // trick to `sshl v0.16b, v0.16b, v2.16b` + `add`; without this
    // the interp SIGILL'd (signal 4) inside strspn during fontconfig
    // config parsing.
    case 0x0E204400: case 0x0E604400: case 0x0EA04400: case 0x0EE04400:  // SSHL
    case 0x2E204400: case 0x2E604400: case 0x2EA04400: case 0x2EE04400:  // USHL
    {
        int esize = 1 << size;          // 1 / 2 / 4 / 8 bytes
        int bits = esize * 8;
        int elems = (Q ? 16 : 8) / esize;
        uint64_t mask = (bits == 64) ? ~0ULL : ((1ULL << bits) - 1);
        uint8_t buf_n[16], buf_m[16];
        memcpy(buf_n, &cpu.v_lo[rn], 8);
        memcpy(buf_n + 8, &cpu.v_hi[rn], 8);
        memcpy(buf_m, &cpu.v_lo[rm], 8);
        memcpy(buf_m + 8, &cpu.v_hi[rm], 8);
        uint8_t out[16] = {0};
        for (int i = 0; i < elems; i++) {
            uint64_t vn_lane = 0, vm_lane = 0;
            memcpy(&vn_lane, buf_n + i * esize, esize);
            memcpy(&vm_lane, buf_m + i * esize, esize);
            int shift = static_cast<int8_t>(static_cast<uint8_t>(vm_lane));
            uint64_t result;
            if (shift >= 0) {
                result = shift >= bits ? 0 : (vn_lane << shift) & mask;
            } else {
                int count = -shift; // low-byte range: 1..128, no overflow
                bool negative = !U && (vn_lane & (uint64_t{1} << (bits - 1)));
                if (count >= bits) {
                    result = negative ? mask : 0;
                } else {
                    result = vn_lane >> count;
                    if (negative) result |= mask ^ (mask >> count);
                }
            }
            memcpy(out + i * esize, &result, esize);
        }
        if (Q) {
            memcpy(&cpu.v_lo[rd], out, 8);
            memcpy(&cpu.v_hi[rd], out + 8, 8);
        } else {
            memcpy(&cpu.v_lo[rd], out, 8);
            cpu.v_hi[rd] = 0;
        }
        return true;
    }
    // ── SIMD vector FP 2-source (FADD/FSUB/FMUL/FDIV/FMAX/FMIN/
    //     FMAXNM/FMINNM/FABD/FMULX, vector form) ──────────────
    // These are in the 0x0E/0x2E SIMD group (bits[28:24]=0b01110)
    // with bit[23]=1 (FP), bit[21]=1, bits[15:12]=opcode,
    // bits[11:10]=0b01. ftype in bits[23:22] (but bit23 is always
    // 1 here, so effectively bit22: 0=single, 1=double). Q (bit30)
    // selects .2s (Q=0, 2 lanes) vs .4s (Q=1, 4 lanes) for single,
    // .1d (Q=0, 1 lane) vs .2d (Q=1, 2 lanes) for double.
    // Without this, NEON-vectorized FP (libc memcpy/memset with
    // NEON, image/audio processing) silently produces wrong
    // results — the instruction falls through to the default.
    case 0x0e20d400:  // FADD v (single)
    case 0x0e60d400:  // FADD v (double)
    case 0x0ea0d400:  // FSUB v (single)
    case 0x0ee0d400:  // FSUB v (double)
    case 0x2e20dc00:  // FMUL v (single)
    case 0x2e60dc00:  // FMUL v (double)
    case 0x0e20f400:  // FMAX v (single)
    case 0x0e60f400:  // FMAX v (double)
    case 0x0ea0f400:  // FMIN v (single)
    case 0x0ee0f400:  // FMIN v (double)
    case 0x2ea0d400:  // FABD v (single)
    case 0x2ee0d400:  // FABD v (double)
    case 0x0e20c400:  // FMAXNM v (single)
    case 0x0e60c400:  // FMAXNM v (double)
    case 0x0ea0c400:  // FMINNM v (single)
    case 0x0ee0c400:  // FMINNM v (double)
    case 0x0e20dc00:  // FMULX v (single)
    case 0x0e60dc00:  // FMULX v (double)
    case 0x2e20fc00:  // FDIV v (single)
    case 0x2e60fc00:  // FDIV v (double)
    case 0x0e20cc00:  // FMLA v (single)
    case 0x0e60cc00:  // FMLA v (double)
    case 0x0ea0cc00:  // FMLS v (single)
    case 0x0ee0cc00: { // FMLS v (double)
        // The opcode is encoded across bits[29](U), bits[15:12],
        // and bits[11:10] — NOT a single field. Dispatch on the
        // full sub_noq (already matched by the case labels above).
        // sub_noq keeps bit22, so the double forms (.2d/.1d) have
        // bit22=1; mask it out so both sizes hit the same opcode.
        uint32_t key = sub_noq & ~(1u << 22);
        bool is_double = (op >> 22) & 1;  // bit22: 0=S, 1=D
        int lanes = is_double ? (Q ? 2 : 1) : (Q ? 4 : 2);
        auto get_op = [&](uint32_t k) -> int {
            if (k == 0x0e20d400) return 0;  // FADD
            if (k == 0x0ea0d400) return 1;  // FSUB
            if (k == 0x2e20dc00) return 2;  // FMUL
            if (k == 0x2e20fc00) return 3;  // FDIV
            if (k == 0x0e20f400) return 4;  // FMAX
            if (k == 0x0ea0f400) return 5;  // FMIN
            if (k == 0x0e20c400) return 6;  // FMAXNM
            if (k == 0x0ea0c400) return 7;  // FMINNM
            if (k == 0x0e20dc00) return 0xB; // FMULX
            if (k == 0x2ea0d400) return 0xD; // FABD
            if (k == 0x0e20cc00) return 0xE; // FMLA
            if (k == 0x0ea0cc00) return 0xF; // FMLS
            return -1;
        };
        int opcode = get_op(key);
        // ARM NaN/zero rules per lane (FloatCommon.hpp); FMLA/FMLS
        // are fused (FPMulAdd) and FMLS negates its multiplicand first.
        const bool dn = (cpu.fpcr >> 25) & 1;
        auto lane_op = [opcode, dn](auto a, auto b, auto acc) {
            switch (opcode) {
                case 0x0: return arm_fp_binop(ArmFpOp::Add, a, b, dn);
                case 0x1: return arm_fp_binop(ArmFpOp::Sub, a, b, dn);
                case 0x2: return arm_fp_binop(ArmFpOp::Mul, a, b, dn);
                case 0x3: return arm_fp_binop(ArmFpOp::Div, a, b, dn);
                case 0x4: return arm_fp_binop(ArmFpOp::Max, a, b, dn);
                case 0x5: return arm_fp_binop(ArmFpOp::Min, a, b, dn);
                case 0x6: return arm_fp_binop(ArmFpOp::MaxNum, a, b, dn);
                case 0x7: return arm_fp_binop(ArmFpOp::MinNum, a, b, dn);
                case 0xB: return arm_fp_binop(ArmFpOp::MulX, a, b, dn);
                case 0xD: return arm_fp_binop(ArmFpOp::Abd, a, b, dn);
                case 0xE: return arm_fp_muladd(acc, a, b, dn);
                default:  return arm_fp_muladd(acc, fp_neg(a), b, dn);  // FMLS
            }
        };
        if (is_double) {
            auto gv = [&](int r, int lane) -> double {
                uint64_t bits = (lane == 0) ? cpu.v_lo[r] : cpu.v_hi[r];
                double d; memcpy(&d, &bits, 8); return d;
            };
            auto pv = [&](int r, int lane, double v) {
                uint64_t bits; memcpy(&bits, &v, 8);
                if (lane == 0) cpu.v_lo[r] = bits; else cpu.v_hi[r] = bits;
            };
            for (int i = 0; i < lanes; i++)
                pv(rd, i, lane_op(gv(rn, i), gv(rm, i), gv(rd, i)));
            // Zero unused high lanes for Q=0 (.1d → high 64 zeroed,
            // matching the JIT/IR which always zero v_hi for Q=0).
            if (!Q) cpu.v_hi[rd] = 0;
        } else {
            auto gv = [&](int r, int lane) -> float {
                // lanes 0,1 in v_lo (low 64), lanes 2,3 in v_hi.
                uint64_t chunk = (lane < 2) ? cpu.v_lo[r] : cpu.v_hi[r];
                uint32_t bits = static_cast<uint32_t>(chunk >> ((lane & 1) * 32));
                float f; memcpy(&f, &bits, 4); return f;
            };
            auto pv = [&](int r, int lane, float v) {
                uint32_t bits; memcpy(&bits, &v, 4);
                uint64_t* chunk = (lane < 2) ? &cpu.v_lo[r] : &cpu.v_hi[r];
                int sh = (lane & 1) * 32;
                *chunk = (*chunk & ~(0xFFFFFFFFULL << sh))
                       | (static_cast<uint64_t>(bits) << sh);
            };
            for (int i = 0; i < lanes; i++)
                pv(rd, i, lane_op(gv(rn, i), gv(rm, i), gv(rd, i)));
            // Zero unused high lanes for Q=0 (.2s → high 64 zeroed).
            if (!Q) cpu.v_hi[rd] = 0;
        }
        return true;
    }
    // ── SIMD vector FP pairwise (FADDP/FMAXP/FMINP/FMAXNMP/
    //     FMINNMP, three-same with U=1) ───────────────────────
    // ARM ARM: the source is the concatenation Vm:Vn (Vn in the
    // low elements) and element e = op(concat[2e], concat[2e+1]),
    // so the low half of Vd comes from Vn's pairs and the high half
    // from Vm's. sz:Q=10 (.1d) is RESERVED. All inputs are read
    // before Vd is written: `faddp v17.2s, v17.2s, v18.2s` aliases.
    case 0x2e20d400:  // FADDP v (single)
    case 0x2e60d400:  // FADDP v (double)
    case 0x2e20f400:  // FMAXP v (single)
    case 0x2e60f400:  // FMAXP v (double)
    case 0x2ea0f400:  // FMINP v (single)
    case 0x2ee0f400:  // FMINP v (double)
    case 0x2e20c400:  // FMAXNMP v (single)
    case 0x2e60c400:  // FMAXNMP v (double)
    case 0x2ea0c400:  // FMINNMP v (single)
    case 0x2ee0c400: { // FMINNMP v (double)
        const uint32_t key = sub_noq & ~(1u << 22);
        const bool is_double = (op >> 22) & 1;
        if (is_double && !Q) {
            dump_decode_error(cpu, mem_, op);
            throw DecodeError(cpu.pc, op);
        }
        const bool dn = (cpu.fpcr >> 25) & 1;
        auto pair = [key, dn](auto a, auto b) {
            switch (key) {
                case 0x2e20d400: return arm_fp_binop(ArmFpOp::Add, a, b, dn);     // FADDP
                case 0x2e20f400: return arm_fp_binop(ArmFpOp::Max, a, b, dn);     // FMAXP
                case 0x2ea0f400: return arm_fp_binop(ArmFpOp::Min, a, b, dn);     // FMINP
                case 0x2e20c400: return arm_fp_binop(ArmFpOp::MaxNum, a, b, dn);  // FMAXNMP
                default:         return arm_fp_binop(ArmFpOp::MinNum, a, b, dn);  // FMINNMP
            }
        };
        uint8_t src[32], out[16] = {};
        memcpy(src, &cpu.v_lo[rn], 8);
        memcpy(src + 8, &cpu.v_hi[rn], 8);
        memcpy(src + 16, &cpu.v_lo[rm], 8);
        memcpy(src + 24, &cpu.v_hi[rm], 8);
        const size_t esize = is_double ? 8 : 4;
        const size_t lanes = (Q ? 16 : 8) / esize;
        for (size_t e = 0; e < lanes; e++) {
            // Pairs 0..lanes/2-1 come from Vn, the rest from Vm.
            const size_t half = lanes / 2;
            const uint8_t* base = src + (e < half ? 0 : 16);
            const size_t idx = 2 * (e < half ? e : e - half);
            if (is_double) {
                double a, b;
                memcpy(&a, base + idx * 8, 8);
                memcpy(&b, base + (idx + 1) * 8, 8);
                double r = pair(a, b);
                memcpy(out + e * 8, &r, 8);
            } else {
                float a, b;
                memcpy(&a, base + idx * 4, 4);
                memcpy(&b, base + (idx + 1) * 4, 4);
                float r = pair(a, b);
                memcpy(out + e * 4, &r, 4);
            }
        }
        memcpy(&cpu.v_lo[rd], out, 8);
        memcpy(&cpu.v_hi[rd], out + 8, 8);  // zero for Q=0
        return true;
    }
    // ── TBL/TBX (Table Lookup) ──
    // AArch64 TBL/TBX permute bytes from 1-4 source vectors
    // using indices from a third vector. Each byte
    // index in the index vector selects a byte from the
    // concatenated source(s). Indices >= the source length:
    //   TBL: result byte = 0
    //   TBX: result byte unchanged (in-place update)
    //
    // TBL/TBX encoding: 0 Q 0 0 1 1 1 0 size 0 0 Rm op2 L Rn Rd
    //   - op2 = bit 12 (0x1000): 0 = TBL, 1 = TBX
    //   - L   = bit 13 (0x2000): 0 = one source reg, 1 = two
    // Both bits survive the sub_noq mask (0xFFE0FC00 keeps
    // bits[15:10]), so each form has its own case:
    //   - TBL1: 0x0E000000, TBL2: 0x0E002000
    //   - TBX1: 0x0E001000, TBX2: 0x0E003000
    // v0 only matched TBL1 (mask 0xFFE00000 val 0x0E000000,
    // which also masks off bit 13); the 2-source TBL2 form
    // (e.g. GCC lowering of vextq_u8 with a 2-vector table)
    // silently fell through as a NOP, producing all zeros.
    case 0x0E000000:   // TBL (1 source reg)
    case 0x0E002000:   // TBL (2 source regs)
    case 0x0E001000:   // TBX (1 source reg)
    case 0x0E003000:   // TBX (2 source regs)
    case 0x0E004000:   // TBL (3 source regs)
    case 0x0E006000:   // TBL (4 source regs)
    case 0x0E005000:   // TBX (3 source regs)
    case 0x0E007000: { // TBX (4 source regs)
        bool is_tbx     = (op & 0x1000) != 0;  // op2 bit (bit 12)
        int nregs       = ((op >> 13) & 0x3) + 1;  // bits[14:13]
        // Q bit (bit 30) — already extracted as `Q` above.
        // Build the table: nregs full 16-byte registers ({Vn.16B, ...}).
        // Q only sizes the index and result (8B or 16B); an 8-byte table
        // per register made every .8b lookup read the wrong bytes.
        const int bytes_per_reg = 16;
        uint8_t table[4 * 16];
        for (int r = 0; r < nregs; r++) {
            int rn_r = (rn + r) & 31;
            memcpy(table + r * bytes_per_reg, &cpu.v_lo[rn_r], 8);
            memcpy(table + r * bytes_per_reg + 8, &cpu.v_hi[rn_r], 8);
        }
        int table_len = nregs * bytes_per_reg;
        // Read the index vector Vm.
        uint8_t idx[16];
        memcpy(idx,    &cpu.v_lo[rm], 8);
        memcpy(idx+8,  &cpu.v_hi[rm], 8);
        int out_len = Q ? 16 : 8;
        // Read current Vd (for TBX in-place update).
        uint8_t out[16];
        memcpy(out,    &cpu.v_lo[rd], 8);
        memcpy(out+8,  &cpu.v_hi[rd], 8);
        for (int i = 0; i < out_len; i++) {
            uint8_t b = idx[i];
            if (b < table_len) {
                out[i] = table[b];
            } else if (!is_tbx) {
                // TBL: out-of-range indices produce 0.
                out[i] = 0;
            }
            // TBX: out-of-range indices leave out[i] unchanged.
        }
        cpu.v_lo[rd] = 0;
        memcpy(&cpu.v_lo[rd], out, 8);
        if (Q) {
            memcpy(&cpu.v_hi[rd], out+8, 8);
        } else {
            cpu.v_hi[rd] = 0;
        }
        return true;
    }
    // ── CMGT / CMGE / CMHI / CMHS (register compare, vector) ──
    // SIMD 3-register integer compare. Encoding:
    //   0 Q U 01110 size 1 Rm opcode Rn Rd
    // opcode bits[15:10]: 0b001101 → GT/HI (strict >),
    //                      0b001111 → GE/HS (>=).
    // U bit (29): 0 = signed (CMGT/CMGE), 1 = unsigned (CMHI/CMHS).
    // sub_noq keeps U but strips Q (bit 30), so each of the four
    // ops has a distinct sub_noq base × 4 size variants (size in
    // bits 23:22, which survives the mask):
    //   CMGT (signed >):    0x0E203400 0x0E603400 0x0EA03400 0x0EE03400
    //   CMGE (signed >=):   0x0E203C00 0x0E603C00 0x0EA03C00 0x0EE03C00
    //   CMHI (unsigned >):  0x2E203400 0x2E603400 0x2EA03400 0x2EE03400
    //   CMHS (unsigned >=): 0x2E203C00 0x2E603C00 0x2EA03C00 0x2EE03C00
    // (Verified against cross binutils: cmhi v0.8h,v2.8h,v4.8h =
    // 0x6E643440 → sub_noq 0x2E603400.)
    case 0x0E203400: case 0x0E603400: case 0x0EA03400: case 0x0EE03400:
    case 0x0E203C00: case 0x0E603C00: case 0x0EA03C00: case 0x0EE03C00:
    case 0x2E203400: case 0x2E603400: case 0x2EA03400: case 0x2EE03400:
    case 0x2E203C00: case 0x2E603C00: case 0x2EA03C00: case 0x2EE03C00: {
        bool U = (op >> 29) & 1;       // 0=signed, 1=unsigned
        bool ge = (op >> 11) & 1;      // opcode 0x0F (>=) vs 0x0D (>)
        int esize = 1 << size;
        int elems = (Q ? 16 : 8) / esize;
        uint8_t buf_n[16], buf_m[16];
        memcpy(buf_n, &cpu.v_lo[rn], 8);
        if (Q) memcpy(buf_n + 8, &cpu.v_hi[rn], 8);
        memcpy(buf_m, &cpu.v_lo[rm], 8);
        if (Q) memcpy(buf_m + 8, &cpu.v_hi[rm], 8);
        uint8_t out[16] = {0};
        for (int i = 0; i < elems; i++) {
            bool c;
            if (U) {
                uint64_t n = 0, m = 0;
                memcpy(&n, buf_n + i*esize, esize);
                memcpy(&m, buf_m + i*esize, esize);
                c = ge ? (n >= m) : (n > m);
            } else {
                int64_t n = 0, m = 0;
                memcpy(&n, buf_n + i*esize, esize);
                memcpy(&m, buf_m + i*esize, esize);
                if (esize == 1) { n = (int8_t)n; m = (int8_t)m; }
                else if (esize == 2) { n = (int16_t)n; m = (int16_t)m; }
                else if (esize == 4) { n = (int32_t)n; m = (int32_t)m; }
                c = ge ? (n >= m) : (n > m);
            }
            memset(out + i*esize, c ? 0xFF : 0x00, esize);
        }
        memcpy(&cpu.v_lo[rd], out, 8);
        if (Q) memcpy(&cpu.v_hi[rd], out + 8, 8);
        else cpu.v_hi[rd] = 0;
        return true;
    }
    // ── UMAXP/UMINP/SMAXP/SMINP family ──
    // (unsigned) for what was labeled SMAXP/SMINP (U=0, signed).
    // For byte elements with values 0x80-0xFF, signed vs unsigned
    // differ. We now use int64_t with sign-extension for the
    // signed (U=0) path.
    //
    // Encoding: 0 Q U 01110 size Rm 01101 0 Rn Rd
    //   U=0, C=0: SMAXP (signed pairwise max)
    //   U=0, C=1: SMINP (signed pairwise min)
    //   U=1, C=0: UMAXP (unsigned pairwise max)
    //   U=1, C=1: UMINP (unsigned pairwise min)
    // After sub_noq (Q stripped, U kept):
    //   U=1 → sub_noq = 0x2E20A400 (UMAXP/UMINP, Q=0 base 0x2E)
    //   U=0 → sub_noq = 0x0E20A400 (SMAXP/SMINP, Q=0 base 0x0E)
    // sub_noq strips ONLY Q (bit 30), so the U bit (29) still
    // distinguishes signed (0x0E*) from unsigned (0x2E*) — both
    // label sets must be present or the signed forms throw
    // DecodeError. The handler reads the runtime U bit (op>>29)
    // and C bit (op>>11, max vs min) so one shared body serves
    // all four ops; the 0x*C00 (bit11=1 = min) label sets are
    // REQUIRED too — sub_noq keeps bit 11, so without them
    // SMINP/UMINP fall through to DecodeError (the original code
    // only listed the max forms and threw SIGILL on the min ops).
    case 0x0E20A400: case 0x0E60A400: case 0x0EA0A400: case 0x0EE0A400:
    case 0x0E20AC00: case 0x0E60AC00: case 0x0EA0AC00: case 0x0EE0AC00:
    case 0x2E20A400: case 0x2E60A400: case 0x2EA0A400: case 0x2EE0A400:
    case 0x2E20AC00: case 0x2E60AC00: case 0x2EA0AC00: case 0x2EE0AC00: {  // UMAXP/UMINP (U=1) and SMAXP/SMINP (U=0); size 0-3
        // bit 15. Verified by comparing UMAXP (0x6e20a400) vs UMINP
        // (0x6e20ac00) — they differ only at bit 11. The old code
        // used bit 15, which is part of the opcode that
        // distinguishes pairwise ops from other SIMD ops, NOT max
        // from min. With the wrong bit, UMAXP was treated as UMINP
        // (C=1), computing min instead of max. This broke glibc's
        // strchrnul: `umaxp v4.16b, v3.16b, v3.16b` computed min
        // instead of max, so the mask-reduction returned 0 even
        // when a match was found, preventing '%' detection in
        // printf format strings.
        bool C = (op >> 11) & 1;   // 0=max, 1=min
        bool U = (op >> 29) & 1;   // 0=signed, 1=unsigned
        int esize = 1 << size;
        int elems = (Q ? 16 : 8) / esize;
        uint8_t buf_n[16], buf_m[16];
        memcpy(buf_n, &cpu.v_lo[rn], 8);
        if (Q) memcpy(buf_n + 8, &cpu.v_hi[rn], 8);
        memcpy(buf_m, &cpu.v_lo[rm], 8);
        if (Q) memcpy(buf_m + 8, &cpu.v_hi[rm], 8);
        uint8_t out[16] = {0};
        // ops that operate on EACH source independently, producing
        // TWO half-results:
        //   first half of Vd = pairwise(max/min) of Vn
        //   second half of Vd = pairwise(max/min) of Vm
        // which is wrong — it only produced half the output bytes
        // AND used the wrong semantics. This broke glibc's
        // strchrnul SIMD path, which uses `umaxp v4.16b, v3.16b,
        // v3.16b` to reduce a 16-byte mask to 8 bytes; the old
        // code zeroed the second half and used a 4-way max that
        // happened to produce zeros for the test pattern,
        // preventing strchrnul from finding '%' in format strings.
        auto load_elem = [&](const uint8_t* p) -> int64_t {
            uint64_t u = 0;
            memcpy(&u, p, esize);
            if (!U) {
                if (esize == 1) return (int8_t)u;
                if (esize == 2) return (int16_t)u;
                if (esize == 4) return (int32_t)u;
                return (int64_t)u;
            }
            return (int64_t)u;
        };
        auto do_pair = [&](int64_t a, int64_t b) -> int64_t {
            return C ? ((a < b) ? a : b) : ((a > b) ? a : b);
        };
        // First half: pairwise op on Vn.
        for (int i = 0; i < elems / 2; i++) {
            int64_t n0 = load_elem(buf_n + (2*i) * esize);
            int64_t n1 = load_elem(buf_n + (2*i+1) * esize);
            int64_t res = do_pair(n0, n1);
            memcpy(out + i * esize, &res, esize);
        }
        // Second half: pairwise op on Vm. This runs for BOTH Q=0
        // and Q=1: a 64-bit (Q=0) pairwise op packs half its
        // result bytes from Vn and half from Vm into v_lo — the
        // second half is NOT zeroed. (The old Q=0 gating produced
        // {Vn pairs, 0,0,0,0}, which broke any Q=0 use where the
        // sources differ; the sub3_noq UMINP handler at ~2649
        // already computed both halves, and real ARM does too.)
        {
            int half = (elems / 2) * esize;  // offset into out
            for (int i = 0; i < elems / 2; i++) {
                int64_t m0 = load_elem(buf_m + (2*i) * esize);
                int64_t m1 = load_elem(buf_m + (2*i+1) * esize);
                int64_t res = do_pair(m0, m1);
                memcpy(out + half + i * esize, &res, esize);
            }
        }
        memcpy(&cpu.v_lo[rd], out, 8);
        if (Q) memcpy(&cpu.v_hi[rd], out + 8, 8);
        else cpu.v_hi[rd] = 0;
        return true;
    }
    // ── UMOV (vector element to GPR) / INS (element, vector to vector) ──
    // Encoding: Q 0 0 11110 size 1 imm5 0 0 1 1 Rn Rd
    // Q=0 → 32-bit GPR dest, Q=1 → 64-bit GPR dest (UMOV only)
    // For INS(element), Rd is a vector register.
    // imm5 encodes element size and index.
    case 0x0E003C00: {
        uint8_t imm5 = (op >> 16) & 0x1F;
        // Find lowest set bit → element size
        int esize_log2 = 0;
        for (int b = 0; b < 5; b++) {
            if (imm5 & (1 << b)) { esize_log2 = b; break; }
        }
        int esize = 1 << esize_log2;  // bytes: 1, 2, 4, or 8
        int index = imm5 >> (esize_log2 + 1);
        // UMOV: read from vector element, write to GPR.
        // (8/esize) must read from v_hi, not v_lo. The old code
        // always read from v_lo, returning wrong values for lane
        // indices >= 2 (32-bit) or >= 1 (64-bit).
        int elems_per_qword = 8 / esize;
        uint64_t val = 0;
        if (esize == 8) {
            val = (index == 0) ? cpu.v_lo[rn] : cpu.v_hi[rn];
        } else if (esize == 4) {
            if (index < elems_per_qword)
                val = reinterpret_cast<uint32_t*>(&cpu.v_lo[rn])[index];
            else
                val = reinterpret_cast<uint32_t*>(&cpu.v_hi[rn])[index - elems_per_qword];
        } else if (esize == 2) {
            if (index < elems_per_qword)
                val = reinterpret_cast<uint16_t*>(&cpu.v_lo[rn])[index];
            else
                val = reinterpret_cast<uint16_t*>(&cpu.v_hi[rn])[index - elems_per_qword];
        } else {  // esize == 1
            if (index < elems_per_qword)
                val = reinterpret_cast<uint8_t*>(&cpu.v_lo[rn])[index];
            else
                val = reinterpret_cast<uint8_t*>(&cpu.v_hi[rn])[index - elems_per_qword];
        }
        if (rd != 31) cpu.regs[rd] = val;
        return true;
    }
    // ── SMOV (vector element -> GPR, sign-extended) ──────────────
    // Encoding: same ASIMDINS group as UMOV but bits[15:12]=0010
    // (sub_noq 0x0E002C00; Q=1 forms land here too because sub_noq
    // strips bit30). imm5 encodes esize AND index exactly like
    // UMOV: esize = 1 << ctz(imm5), index = imm5 >> (ctz+1).
    // Q=0 -> Wd (sign-extend to 32), Q=1 -> Xd (extend to 64).
    // CRITICAL: only B/H/S elements are allocated — the .D form
    // does NOT exist (identity extension; GNU as rejects it), so
    // esize==8 falls through to the DecodeError default.
    case 0x0E002C00: {
        uint8_t imm5 = (op >> 16) & 0x1F;
        int esize_log2 = 0;
        for (int b = 0; b < 5; b++) {
            if (imm5 & (1 << b)) { esize_log2 = b; break; }
        }
        int esize = 1 << esize_log2;
        int index = imm5 >> (esize_log2 + 1);
        if (esize == 8) break;   // unallocated — DecodeError
        int elems_per_qword = 8 / esize;
        uint64_t raw = 0;
        if (esize == 4) {
            raw = (index < elems_per_qword)
                ? reinterpret_cast<uint32_t*>(&cpu.v_lo[rn])[index]
                : reinterpret_cast<uint32_t*>(&cpu.v_hi[rn])[index - elems_per_qword];
        } else if (esize == 2) {
            raw = (index < elems_per_qword)
                ? reinterpret_cast<uint16_t*>(&cpu.v_lo[rn])[index]
                : reinterpret_cast<uint16_t*>(&cpu.v_hi[rn])[index - elems_per_qword];
        } else {  // esize == 1
            raw = (index < elems_per_qword)
                ? reinterpret_cast<uint8_t*>(&cpu.v_lo[rn])[index]
                : reinterpret_cast<uint8_t*>(&cpu.v_hi[rn])[index - elems_per_qword];
        }
        uint64_t sext = static_cast<uint64_t>(
            static_cast<int64_t>(raw << (64 - esize * 8)) >>
            (64 - esize * 8));
        if (Q) {
            if (rd != 31) cpu.regs[rd] = sext;
        } else {
            if (rd != 31) cpu.regs[rd] =
                static_cast<uint64_t>(static_cast<uint32_t>(sext));
        }
        return true;
    }
    // ── CMEQ two registers ──
    // CMEQ encoding: 0 Q U 01110 size Rm 100011 1 Rn Rd
    // Both U=0 and U=1 are valid and produce the same result
    // (equality is sign-agnostic). After sub_noq masking
    // (Q stripped, U kept), both map to 0x2E208C00 because
    // the U bit (29) is the same for 0x2E... and 0x6E...
    // after Q (bit 30) is removed: 0x6E... & ~(1<<30) = 0x2E...
    // So a single case 0x2E208C00 handles both encodings.
    case 0x2E208C00: case 0x2E608C00: case 0x2EA08C00: case 0x2EE08C00: { // CMEQ (U=0 or U=1, after sub_noq); size 0-3
        int esize = (size == 0) ? 1 : (size == 1 ? 2 : (size == 2 ? 4 : 8));
        int elems = (Q ? 16 : 8) / esize;
        uint8_t buf_rn[16], buf_rm[16];
        memcpy(buf_rn, &cpu.v_lo[rn], 8);
        if (Q) memcpy(buf_rn + 8, &cpu.v_hi[rn], 8);
        memcpy(buf_rm, &cpu.v_lo[rm], 8);
        if (Q) memcpy(buf_rm + 8, &cpu.v_hi[rm], 8);
        uint8_t out[16] = {0};
        for (int i = 0; i < elems; i++) {
            bool eq = (memcmp(buf_rn + i*esize, buf_rm + i*esize, esize) == 0);
            memset(out + i*esize, eq ? 0xFF : 0x00, esize);
        }
        memcpy(&cpu.v_lo[rd], out, 8);
        if (Q) memcpy(&cpu.v_hi[rd], out + 8, 8);
        else cpu.v_hi[rd] = 0;
        return true;
    }
    // ── CMTST (test bits, vector) ──
    // Encoding 0 Q U 01110 size 1 Rm 100011 0 Rn Rd (U=0).
    // sub_noq = 0x0E208C00 (size 0) / 0x0E608C00 (1) / 0x0EA08C00
    // (2) / 0x0EE08C00 (3). Unlike CMEQ (U=1 → 0x2E...8C00), the
    // U=0 form keeps the 0x0E prefix after Q is stripped, so CMTST
    // gets its OWN case. (Old code assumed both U=0/U=1 map to
    // 0x2E208C00 — that only holds for the Q=1 0x6E... variant.)
    // Result per lane: all-ones if (Vn & Vm) != 0, else zero.
    case 0x0E208C00: case 0x0E608C00: case 0x0EA08C00: case 0x0EE08C00: {
        int esize = (size == 0) ? 1 : (size == 1 ? 2 : (size == 2 ? 4 : 8));
        int elems = (Q ? 16 : 8) / esize;
        uint8_t buf_rn[16], buf_rm[16];
        memcpy(buf_rn, &cpu.v_lo[rn], 8);
        if (Q) memcpy(buf_rn + 8, &cpu.v_hi[rn], 8);
        memcpy(buf_rm, &cpu.v_lo[rm], 8);
        if (Q) memcpy(buf_rm + 8, &cpu.v_hi[rm], 8);
        uint8_t out[16] = {0};
        for (int i = 0; i < elems; i++) {
            bool any = false;
            for (int k = 0; k < esize; k++) {
                if ((buf_rn[i*esize + k] & buf_rm[i*esize + k]) != 0) {
                    any = true;
                    break;
                }
            }
            memset(out + i*esize, any ? 0xFF : 0x00, esize);
        }
        memcpy(&cpu.v_lo[rd], out, 8);
        if (Q) memcpy(&cpu.v_hi[rd], out + 8, 8);
        else cpu.v_hi[rd] = 0;
        return true;
    }
    // Ordered vector FP comparisons against zero. Q=0 clears v_hi;
    // NaNs compare false, and +0/-0 compare equal. sub_noq drops bits
    // 20:16; with bit 16 set, FCMGT/FCMGE #0 are URECPE/URSQRTE, which
    // used to run as the comparisons. .1d and 64-bit URECPE are reserved.
    case 0x0EA0D800: case 0x2EA0C800: case 0x0EA0C800:
    case 0x2EA0D800: case 0x0EA0E800:
    case 0x0EE0D800: case 0x2EE0C800: case 0x0EE0C800:
    case 0x2EE0D800: case 0x0EE0E800: {
        const bool is_double = (sub_noq >> 22) & 1;
        const uint32_t bits20_16 = (op >> 16) & 0x1F;
        if (bits20_16 == 1 && (sub_noq & 0xFFBFFFFF) == (U ? 0x2EA0C800u : 0x0EA0C800u)) {
            if (is_double) throw DecodeError(cpu.pc, op);
            uint32_t lanes[4] = {static_cast<uint32_t>(cpu.v_lo[rn]),
                                 static_cast<uint32_t>(cpu.v_lo[rn] >> 32),
                                 static_cast<uint32_t>(cpu.v_hi[rn]),
                                 static_cast<uint32_t>(cpu.v_hi[rn] >> 32)};
            for (uint32_t& lane : lanes) lane = arm_unsigned_recip_estimate(lane, U);
            cpu.v_lo[rd] = lanes[0] | static_cast<uint64_t>(lanes[1]) << 32;
            cpu.v_hi[rd] = Q ? lanes[2] | static_cast<uint64_t>(lanes[3]) << 32 : 0;
            return true;
        }
        if (bits20_16) break;
        if (is_double && !Q) throw DecodeError(cpu.pc, op);
        const int esz = is_double ? 8 : 4;
        const uint32_t kind = sub_noq & ~(1u << 22);
        uint64_t source[2] = {cpu.v_lo[rn], cpu.v_hi[rn]};
        uint8_t out[16] = {};
        for (int i = 0; i < (Q ? 16 : 8) / esz; ++i) {
            double value;
            if (is_double) memcpy(&value, reinterpret_cast<uint8_t*>(source) + i * 8, 8);
            else {
                float f;
                memcpy(&f, reinterpret_cast<uint8_t*>(source) + i * 4, 4);
                value = f;
            }
            bool result = false;
            switch (kind) {
                case 0x0EA0D800: result = value == 0.0; break;
                case 0x2EA0C800: result = value >= 0.0; break;
                case 0x0EA0C800: result = value > 0.0; break;
                case 0x2EA0D800: result = value <= 0.0; break;
                case 0x0EA0E800: result = value < 0.0; break;
            }
            memset(out + i * esz, result ? 0xff : 0, esz);
        }
        memcpy(&cpu.v_lo[rd], out, 8);
        memcpy(&cpu.v_hi[rd], out + 8, 8);
        return true;
    }
    // ── FP compare-register (FCMEQ/FCMGE/FCMGT/FACGE/FACGT) ──
    // SIMD three-same, type=00 (single), verified with the cross
    // assembler:
    //   fcmeq v.4s = 0x4E20E400 / .2s = 0x0E20E400 (U=0)
    //   fcmge      = 0x6E20E400 /     = 0x2E20E400 (U=1)
    //   fcmgt      = 0x6EA0E400 /     = 0x2EA0E400 (U=1, bit23=1)
    //   facge      = 0x6E20EC00 /     = 0x2E20EC00 (bits[11:10]=11)
    //   facgt      = 0x6EA0EC00 /     = 0x2EA0EC00
    // Double (.2d, type bit22=1, Q=1) forms add 0x40 to the byte-2
    // nibble; the neverball crash was libmvec _ZGVnN2v_sin doing
    // `facge v3.2d, v0.2d, v3.2d` (0x6E63EC03), which matched no
    // case and threw DecodeError ("executing garbage" — it was a
    // missing op, not corruption).
    // Per-lane all-ones if condition else 0; NaN makes ordered
    // compares false; FA* compare absolute values.
    // vkQuake's renderer does `fcmgt v28.2s, v19.2s, v31.2s`
    // (0x2EBFE67C) in its math code.
    case 0x0E20E400: case 0x2E20E400: case 0x2EA0E400:
    case 0x2E20EC00: case 0x2EA0EC00:
    case 0x0E60E400: case 0x2E60E400: case 0x2EE0E400:
    case 0x2E60EC00: case 0x2EE0EC00: {
        const bool is_double = (sub_noq >> 22) & 1;
        const int esz = is_double ? 8 : 4;
        const int elems = (Q ? 16 : 8) / esz;
        uint8_t out[16] = {0};
        for (int i = 0; i < elems; i++) {
            bool res;
            if (!is_double) {
                float a, b;
                uint64_t lo_a = (i < 2) ? cpu.v_lo[rn] : cpu.v_hi[rn];
                uint64_t lo_b = (i < 2) ? cpu.v_lo[rm] : cpu.v_hi[rm];
                uint32_t ua = (i & 1) ? (lo_a >> 32) : (uint32_t)lo_a;
                uint32_t ub = (i & 1) ? (lo_b >> 32) : (uint32_t)lo_b;
                memcpy(&a, &ua, 4);
                memcpy(&b, &ub, 4);
                switch (sub_noq) {
                case 0x0E20E400: res = a == b; break;              // FCMEQ
                case 0x2E20E400: res = a >= b; break;              // FCMGE
                case 0x2EA0E400: res = a > b; break;               // FCMGT
                default: {                                          // FA*
                    float fa = std::fabs(a), fb = std::fabs(b);
                    res = (sub_noq == 0x2E20EC00) ? (fa >= fb)
                                                  : (fa > fb);
                    break;
                }
                }
                memset(out + i * 4, res ? 0xFF : 0x00, 4);
            } else {
                double a, b;
                uint64_t ua = (i == 0) ? cpu.v_lo[rn] : cpu.v_hi[rn];
                uint64_t ub = (i == 0) ? cpu.v_lo[rm] : cpu.v_hi[rm];
                memcpy(&a, &ua, 8);
                memcpy(&b, &ub, 8);
                switch (sub_noq) {
                case 0x0E60E400: res = a == b; break;              // FCMEQ
                case 0x2E60E400: res = a >= b; break;              // FCMGE
                case 0x2EE0E400: res = a > b; break;               // FCMGT
                default: {                                          // FA*
                    double fa = std::fabs(a), fb = std::fabs(b);
                    res = (sub_noq == 0x2E60EC00) ? (fa >= fb)
                                                  : (fa > fb);
                    break;
                }
                }
                memset(out + i * 8, res ? 0xFF : 0x00, 8);
            }
        }
        memcpy(&cpu.v_lo[rd], out, 8);
        if (Q) memcpy(&cpu.v_hi[rd], out + 8, 8);
        else cpu.v_hi[rd] = 0;
        return true;
    }
    // ── Narrowing (XTN/SQXTN/UQXTN/SQXTUN ±2) ──
    // bits[15:10] discriminates the op: 001010 → XTN (U=0) /
    // SQXTUN (U=1); 010010 → SQXTN (U=0) / UQXTN (U=1). size
    // (bits 23:22) selects the SOURCE element size (2× the dest
    // size): 00 → 16-bit Source/8-bit dst, 01 → 32/16, 10 → 64/32.
    // Q=0 writes the low 64 bits of Rd and zeroes the upper 64;
    // Q=1 (*2 variants) writes the high 64 bits, preserving the
    // low half. SQXTUN saturates signed values into the unsigned
    // destination range. Verified encodings (cross-as):
    //   XTN   0x0E212820 (8b,8h) / 0x0E612820 (4h,4s) / 0x0EA12820 (2s,2d)
    //   XTN2  0x4E…, SQXTN 0x0E…4820, UQXTN 0x2E…4820, SQXTUN 0x2E…2820.
    case 0x0E202800: case 0x0E602800: case 0x0EA02800:  // XTN (U=0)
    case 0x2E202800: case 0x2E602800: case 0x2EA02800:  // SQXTUN (U=1)
    case 0x0E204800: case 0x0E604800: case 0x0EA04800:  // SQXTN (U=0)
    case 0x2E204800: case 0x2E604800: case 0x2EA04800:  // UQXTN (U=1)
    {
        if (((op >> 16) & 0x1F) != 1) break;  // sub_noq drops bits 20:16
        if (size == 3) {
            dump_decode_error(cpu, mem_, op);
            throw DecodeError(cpu.pc, op);
        }
        int src_esize = 2 << size;      // 2 / 4 / 8 bytes
        int dst_esize = src_esize >> 1; // 1 / 2 / 4 bytes
        int elems = 16 / src_esize;     // 8 / 4 / 2
        uint32_t opc = (op >> 10) & 0x3F;
        // opc 0x0A → XTN/SQXTUN; 0x12 → SQXTN/UQXTN. U picks the
        // saturating form in both families.
        bool sat_signed = (opc == 0x12) && !U;   // SQXTN
        bool sat_unsigned = (opc == 0x12) && U;  // UQXTN
        bool sat_to_unsigned = (opc == 0x0A) && U; // SQXTUN
        uint64_t max_u = (dst_esize == 8) ? ~0ULL : ((1ULL << (8 * dst_esize)) - 1);
        int64_t s_lo = -(1LL << (8 * dst_esize - 1));
        int64_t s_hi = (1LL << (8 * dst_esize - 1)) - 1;
        uint8_t buf[16];
        memcpy(buf, &cpu.v_lo[rn], 8);
        memcpy(buf + 8, &cpu.v_hi[rn], 8);  // source is always full 128 bits
        uint8_t out[8] = {0};
        for (int i = 0; i < elems; i++) {
            uint64_t v = 0;
            memcpy(&v, buf + i * src_esize, src_esize);
            const uint64_t before = v;
            if (sat_signed) {
                int64_t sv;
                if      (src_esize == 2) sv = (int64_t)(int16_t)(uint16_t)v;
                else if (src_esize == 4) sv = (int64_t)(int32_t)(uint32_t)v;
                else                     sv = (int64_t)v;
                if (sv < s_lo) sv = s_lo;
                if (sv > s_hi) sv = s_hi;
                v = (uint64_t)sv;
            } else if (sat_unsigned) {
                if (v > max_u) v = max_u;
            } else if (sat_to_unsigned) {
                int64_t sv;
                if      (src_esize == 2) sv = (int64_t)(int16_t)(uint16_t)v;
                else if (src_esize == 4) sv = (int64_t)(int32_t)(uint32_t)v;
                else                     sv = (int64_t)v;
                if (sv < 0) v = 0;
                else if ((uint64_t)sv > max_u) v = max_u;
                else v = (uint64_t)sv;
            }
            // A saturating form that changed the value saturated (QC).
            // Signed results compare as sign-extended source lanes.
            if ((sat_signed || sat_unsigned || sat_to_unsigned) &&
                (sat_signed ? sext_lane(v, 8 * src_esize) != sext_lane(before, 8 * src_esize)
                            : v != before))
                cpu.fpsr |= 1u << 27;
            // XTN: plain truncation — low dst_esize bytes copied below.
            memcpy(out + i * dst_esize, &v, dst_esize);
        }
        if (Q) {
            memcpy(&cpu.v_hi[rd], out, 8);  // *2: upper half, low preserved
        } else {
            memcpy(&cpu.v_lo[rd], out, 8);
            cpu.v_hi[rd] = 0;
        }
        return true;
    }
    default: break;  // fall through to size-based checks below
    }
    return false;
}
} // namespace arm64emu
