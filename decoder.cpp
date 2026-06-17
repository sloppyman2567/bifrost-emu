// decoder.cpp — Pure ARM64 instruction decoder.
//
// This file implements decode() and the shared helpers (cond_true,
// extend_reg). It contains NO execution logic and NO memory access —
// just bit extraction. The interpreter and JIT both call decode() to
// turn a 32-bit ARM64 word into a DecodedInst, then dispatch on
// InstClass.
//
// When adding a new instruction:
//   1. Add an InstClass enum value in decoder.hpp
//   2. Add a decode branch here that sets d.cls and the relevant fields
//   3. Add an execute branch in interpreter.cpp
//   4. (Future) Add a code-emit branch in jit.cpp

#include "decoder.hpp"
#include "arm64_emu.hpp"  // for sign_extend, ror64

namespace arm64emu {

// ── Condition codes ─────────────────────────────────────────────────────
bool cond_true(uint32_t cond, uint32_t pstate) {
    bool N = pstate & (1u<<31);
    bool Z = pstate & (1u<<30);
    bool C = pstate & (1u<<29);
    bool V = pstate & (1u<<28);
    switch (cond & 0xF) {
        case 0x0: return Z;                  // EQ
        case 0x1: return !Z;                 // NE
        case 0x2: return C;                  // CS/HS
        case 0x3: return !C;                 // CC/LO
        case 0x4: return N;                  // MI
        case 0x5: return !N;                 // PL
        case 0x6: return V;                  // VS
        case 0x7: return !V;                 // VC
        case 0x8: return C && !Z;            // HI
        case 0x9: return !C || Z;            // LS
        case 0xA: return N == V;             // GE
        case 0xB: return N != V;             // LT
        case 0xC: return !Z && (N == V);     // GT
        case 0xD: return Z || (N != V);      // LE
        case 0xE: return true;               // AL
        case 0xF: return true;               // AL (NV unused)
    }
    return true;
}

// ── Register extend ─────────────────────────────────────────────────────
uint64_t extend_reg(uint64_t val, uint8_t option, uint8_t shift, bool sf) {
    switch (option & 7) {
        case 0: val = val & 0xFF; break;
        case 1: val = val & 0xFFFF; break;
        case 2: val = val & 0xFFFFFFFF; break;
        case 3: break; // UXTX
        case 4: val = (int64_t)(int8_t)val;  break;
        case 5: val = (int64_t)(int16_t)val; break;
        case 6: val = (int64_t)(int32_t)val; break;
        case 7: break; // SXTX
    }
    return val << shift;
}

// ── Main decode function ────────────────────────────────────────────────
// Hierarchical decode following the ARM Architecture Reference Manual.
// The top-level dispatch is on bits 28:25, which cleanly separates
// the major encoding groups without the collision issues of the old
// flat-list decoder.
//
// Bit patterns (bits 28:25):
//   0000 → reserved
//   0001 → uncond branch / data proc imm
//   0010 → branches / system / cond branch
//   0011 → data proc imm
//   0100 → data proc register
//   0101 → data proc register / branch
//   0110 → load/store
//   0111 → load/store
//   1000 → load/store (pair, no-allocate)
//   1001 → load/store (pair)
//   1010 → branches / data proc register
//   1011 → branches / data proc register
//   1100 → data proc SIMD/FP
//   1101 → data proc SIMD/FP
//   1110 → data proc SIMD/FP
//   1111 → data proc SIMD/FP
bool decode(DecodedInst& d, uint32_t inst) {
    d.raw = inst;
    d.cls = InstClass::UNKNOWN;

    // Extract common fields that most instructions use
    d.rd  = inst & 0x1F;
    d.rn  = (inst >> 5) & 0x1F;
    d.rm  = (inst >> 16) & 0x1F;
    d.rt  = inst & 0x1F;          // same bit position as rd
    d.rt2 = (inst >> 10) & 0x1F;
    d.rs  = (inst >> 16) & 0x1F;  // same as rm
    d.sf  = (inst >> 31) & 1;
    d.size = (inst >> 30) & 3;
    d.cond = inst & 0xF;

    // ── Top-level dispatch on bits 28:25 ────────────────────────────
    // This is the hierarchical decode that prevents the ORR vs STP
    // collision. Each major group is dispatched cleanly.

    // B / BL (unconditional branch, immediate)
    // Encoding: sf 00101 imm26  (sf=0 for B, sf=1 for BL)
    if ((inst & 0x7C000000) == 0x14000000) {
        d.cls = ((inst >> 31) & 1) ? InstClass::BL : InstClass::B;
        d.imm = sign_extend(inst & 0x03FFFFFF, 26) << 2;
        return true;
    }

    // B.cond (conditional branch, immediate)
    // Encoding: 01010100 imm19 0 cond
    if ((inst & 0xFF000010) == 0x54000000) {
        d.cls = InstClass::Bcond;
        d.imm = sign_extend((inst >> 5) & 0x7FFFF, 19) << 2;
        return true;
    }

    // CBZ / CBNZ
    // Encoding: sf 011010 0 imm19 Rt (CBZ) / sf 011010 1 imm19 Rt (CBNZ)
    if ((inst & 0x7E000000) == 0x34000000) {
        d.cls = ((inst >> 24) & 1) ? InstClass::CBNZ : InstClass::CBZ;
        d.imm = sign_extend((inst >> 5) & 0x7FFFF, 19) << 2;
        return true;
    }

    // TBZ / TBNZ
    if ((inst & 0x7E000000) == 0x36000000) {
        d.cls = ((inst >> 24) & 1) ? InstClass::TBNZ : InstClass::TBZ;
        d.imm = sign_extend((inst >> 5) & 0x3FFF, 14) << 2;
        uint8_t b40 = (inst >> 19) & 0x1F;
        uint8_t b5  = (inst >> 31) & 1;
        d.imm_u = (b5 << 5) | b40;  // bit number
        return true;
    }

    // BR / BLR / RET (branch to register)
    if ((inst & 0xFFFFFC00) == 0xD61F0000) {
        d.cls = InstClass::BR;
        return true;
    }
    if ((inst & 0xFFFFFC00) == 0xD63F0000) {
        d.cls = InstClass::BLR;
        return true;
    }
    if ((inst & 0xFFFFFC1F) == 0xD65F0000) {
        d.cls = InstClass::RET;
        return true;
    }

    // SVC / HVC / SMC
    if ((inst & 0xFFE0001F) == 0xD4000001) {
        d.cls = InstClass::SVC;
        d.imm_u = (inst >> 5) & 0xFFFF;
        return true;
    }

    // BRK
    if ((inst & 0xFFE0001F) == 0xD4200000) {
        d.cls = InstClass::BRK;
        d.imm_u = (inst >> 5) & 0xFFFF;
        return true;
    }

    // HLT
    if ((inst & 0xFFE0001F) == 0xD4400000) {
        d.cls = InstClass::HLT;
        d.imm_u = (inst >> 5) & 0xFFFF;
        return true;
    }

    // CLREX (specific encoding)
    if (inst == 0xD503305F) {
        d.cls = InstClass::CLREX;
        return true;
    }

    // MSR / MRS (system register access)
    // Encoding: 1101010100 0 L 0 o0 op1 CRn CRm op2 Rt
    if ((inst & 0xFFE00000) == 0xD5000000) {
        d.cls = ((inst >> 21) & 1) ? InstClass::MRS : InstClass::MSR;
        return true;
    }

    // Barriers / hints (DSB/DMB/ISB/NOP/YIELD)
    if ((inst & 0xFFFFF010) == 0xD5033090 ||
        (inst & 0xFFFFF0E0) == 0xD5033000) {
        d.cls = InstClass::BARRIER;
        return true;
    }

    // ── LSE atomics (bits 29:24 = 111000, bit 21 = 0, bits 11:10 = 00) ──
    // This MUST come before load/store to prevent CAS/LDADD/etc. from
    // being misinterpreted as regular loads/stores.
    if ((inst & 0x3F000000) == 0x38000000 &&
        (inst & 0x00200000) == 0 &&
        (inst & 0x00000C00) == 0) {
        d.atom_op = (inst >> 12) & 0xF;
        d.is_load = (inst >> 22) & 1;
        d.acquire = (inst >> 23) & 1;
        if (d.atom_op >= 0xC) {
            d.cls = InstClass::CAS;
        } else {
            d.cls = InstClass::LSE_ATOMIC;
        }
        return true;
    }

    // ── Load/store pair (ALL modes: post, offset, pre) ──────────────
    // Encoding: opc 101 V mode L imm7 Rt2 Rn Rt
    //   mode (bits 25:24): 00=post, 01=offset, 10=pre
    //   V (bit 26): 0=GP, 1=SIMD
    // Mask checks bits 29:27 = 101 (pair group).
    if ((inst & 0x38000000) == 0x28000000) {
        d.is_vec = (inst >> 26) & 1;
        d.is_load = (inst >> 22) & 1;
        d.cls = d.is_load ? InstClass::LDP : InstClass::STP;
        d.mode = (inst >> 24) & 3;  // 0=post, 1=offset, 2=pre
        d.writeback = (d.mode == 0 || d.mode == 2);
        int16_t imm7 = sign_extend((inst >> 15) & 0x7F, 7);
        int esize;
        if (d.is_vec) {
            esize = (d.size == 0) ? 4 : (d.size == 1) ? 8 : 16;
        } else {
            esize = (d.size == 2) ? 8 : 4;
        }
        d.disp = imm7 * esize;
        return true;
    }

    // ── Load/store exclusive (bits 29:24 = 001000) ──────────────────
    if ((inst & 0x3F000000) == 0x08000000) {
        uint8_t low6 = (inst >> 10) & 0x3F;
        bool L  = (inst >> 22) & 1;
        bool o0 = (inst >> 23) & 1;
        d.is_load = L;
        d.acquire = o0;
        if (low6 == 0x0F || low6 == 0x1F) {
            // STXR/LDXR/STLXR/LDAXR
            d.cls = L ? (o0 ? InstClass::LDAXR : InstClass::LDXR)
                      : (o0 ? InstClass::STLXR : InstClass::STXR);
        } else if (low6 == 0x3F) {
            // STLR/LDAR
            d.cls = L ? InstClass::LDAR : InstClass::STLR;
        }
        return true;
    }

    // ── Load/store (immediate, unsigned offset) ─────────────────────
    // Encoding: size 111 0 01 opc imm12 Rn Rt
    if ((inst & 0x3B000000) == 0x39000000) {
        d.is_vec = (inst >> 26) & 1;
        uint8_t opc = (inst >> 22) & 3;
        d.is_load = (opc & 1);
        d.cls = d.is_load ? InstClass::LDR_IMM : InstClass::STR_IMM;
        d.imm_u = (inst >> 10) & 0xFFF;
        d.disp = d.imm_u << d.size;
        return true;
    }

    // ── Load/store (unscaled, post-index, pre-index) ────────────────
    // Encoding: size 111 0 00 opc 0 imm9 mode Rn Rt
    if ((inst & 0x3B200C00) == 0x38000000 ||  // unscaled
        (inst & 0x3B200C00) == 0x38000400 ||  // post-index
        (inst & 0x3B200C00) == 0x38000C00) {  // pre-index
        d.is_vec = (inst >> 26) & 1;
        uint8_t opc = (inst >> 22) & 3;
        d.is_load = (opc & 1);
        d.cls = d.is_load ? InstClass::LDR_UNS : InstClass::STR_UNS;
        int16_t imm9 = sign_extend((inst >> 12) & 0x1FF, 9);
        d.disp = imm9;
        uint8_t mode_bits = (inst >> 10) & 3;
        d.writeback = (mode_bits == 1 || mode_bits == 3);
        d.mode = (mode_bits == 1) ? 1 : (mode_bits == 3) ? 2 : 0;
        return true;
    }

    // ── Load/store (register offset) ────────────────────────────────
    // Encoding: size 111 0 00 opc 1 Rm option S 10 Rn Rt
    if ((inst & 0x3B200C00) == 0x38200800) {
        d.is_vec = (inst >> 26) & 1;
        uint8_t opc = (inst >> 22) & 3;
        d.is_load = (opc & 1);
        d.cls = d.is_load ? InstClass::LDR_REG : InstClass::STR_REG;
        d.extend = (inst >> 13) & 7;
        d.shift = (inst >> 12) & 1;
        return true;
    }

    // ── ADR / ADRP ──────────────────────────────────────────────────
    if ((inst & 0x1F000000) == 0x10000000) {
        bool is_adrp = (inst >> 31) & 1;
        d.cls = is_adrp ? InstClass::ADRP : InstClass::ADR;
        uint64_t immlo = (inst >> 29) & 3;
        uint64_t immhi = (inst >> 5) & 0x7FFFF;
        d.imm_u = (immhi << 2) | immlo;
        if (is_adrp) d.imm = sign_extend(d.imm_u << 12, 33);
        else d.imm = sign_extend(d.imm_u, 21);
        return true;
    }

    // ── Data processing - immediate ─────────────────────────────────
    // MOVN/MOVZ/MOVK: sf 00 100101 hw imm16 Rd
    if ((inst & 0x1F800000) == 0x12800000) {
        uint8_t opc = (inst >> 29) & 3;
        switch (opc) {
            case 0: d.cls = InstClass::MOVN; break;
            case 2: d.cls = InstClass::MOVZ; break;
            case 3: d.cls = InstClass::MOVK; break;
        }
        d.imm_u = (inst >> 5) & 0xFFFF;
        d.shift = ((inst >> 21) & 3) << 4;
        return true;
    }

    // ADD/SUB immediate
    if ((inst & 0x1F000000) == 0x11000000) {
        bool S = (inst >> 29) & 1;
        bool sub = (inst >> 30) & 1;
        if (sub) {
            d.cls = S ? InstClass::SUBS_IMM : InstClass::SUB_IMM;
        } else {
            d.cls = S ? InstClass::ADDS_IMM : InstClass::ADD_IMM;
        }
        d.set_flags = S;
        d.imm_u = (inst >> 10) & 0xFFF;
        d.shift = ((inst >> 22) & 3) << 4;  // 0, 12, or 24
        return true;
    }

    // Logical immediate (AND/ORR/EOR/ANDS)
    if ((inst & 0x1F800000) == 0x12000000) {
        uint8_t opc = (inst >> 29) & 3;
        switch (opc) {
            case 0: d.cls = InstClass::AND_IMM; break;
            case 1: d.cls = InstClass::ORR_IMM; break;
            case 2: d.cls = InstClass::EOR_IMM; break;
            case 3: d.cls = InstClass::ANDS_IMM; break;
        }
        d.set_flags = (opc == 3);
        return true;
    }

    // Bitfield (SBFM/BFM/UBFM)
    if ((inst & 0x1F800000) == 0x13000000) {
        uint8_t opc = (inst >> 29) & 3;
        switch (opc) {
            case 0: d.cls = InstClass::SBFM; break;
            case 1: d.cls = InstClass::BFM; break;
            case 2: d.cls = InstClass::UBFM; break;
        }
        return true;
    }

    // EXTR
    if ((inst & 0x1F800000) == 0x13800000) {
        d.cls = InstClass::EXTR;
        return true;
    }

    // ── Data processing - register ──────────────────────────────────
    // Logical shifted register (AND/ORR/EOR/ANDS)
    // Encoding: sf 00 01010 shift Rm imm6 Rn Rd  (AND)
    //           sf 01 01010 shift Rm imm6 Rn Rd  (ORR)
    //           sf 10 01010 shift Rm imm6 Rn Rd  (EOR)
    //           sf 11 01010 shift Rm imm6 Rn Rd  (ANDS)
    if ((inst & 0x1F000000) == 0x0A000000) {
        uint8_t opc = (inst >> 29) & 3;
        switch (opc) {
            case 0: d.cls = InstClass::AND_REG; break;
            case 1: d.cls = InstClass::ORR_REG; break;
            case 2: d.cls = InstClass::EOR_REG; break;
            case 3: d.cls = InstClass::ANDS_REG; break;
        }
        d.set_flags = (opc == 3);
        d.shift_type = (inst >> 22) & 3;
        d.shift = (inst >> 10) & 0x3F;
        return true;
    }

    // Add/subtract shifted register
    if ((inst & 0x1F200000) == 0x0B000000) {
        bool S = (inst >> 29) & 1;
        bool sub = (inst >> 30) & 1;
        if (sub) {
            d.cls = S ? InstClass::SUBS_REG : InstClass::SUB_REG;
        } else {
            d.cls = S ? InstClass::ADDS_REG : InstClass::ADD_REG;
        }
        d.set_flags = S;
        d.shift_type = (inst >> 22) & 3;
        d.shift = (inst >> 10) & 0x3F;
        return true;
    }

    // Add/subtract extended register
    if ((inst & 0x1FE00000) == 0x0B200000) {
        bool S = (inst >> 29) & 1;
        bool sub = (inst >> 30) & 1;
        if (sub) {
            d.cls = S ? InstClass::SUBS_REG : InstClass::SUB_REG;
        } else {
            d.cls = S ? InstClass::ADDS_REG : InstClass::ADD_REG;
        }
        d.set_flags = S;
        d.extend = (inst >> 13) & 7;
        d.shift = (inst >> 10) & 7;
        return true;
    }

    // Add/subtract with carry (ADC/ADCS/SBC/SBCS)
    // Encoding: sf op 1 1010000 Rm 000000 Rn Rd
    if ((inst & 0x1FE00000) == 0x1A000000) {
        bool S = (inst >> 29) & 1;
        bool sub = (inst >> 30) & 1;
        if (sub) {
            d.cls = S ? InstClass::SBCS_REG : InstClass::SBC_REG;
        } else {
            d.cls = S ? InstClass::ADCS_REG : InstClass::ADC_REG;
        }
        d.set_flags = S;
        return true;
    }

    // Conditional select (CSEL/CSINC/CSINV/CSNEG)
    if ((inst & 0x1FE00000) == 0x1A800000) {
        uint8_t op = (inst >> 10) & 3;
        switch (op) {
            case 0: d.cls = InstClass::CSEL; break;
            case 1: d.cls = InstClass::CSINC; break;
            case 2: d.cls = InstClass::CSINV; break;
            case 3: d.cls = InstClass::CSNEG; break;
        }
        d.cond = (inst >> 12) & 0xF;
        return true;
    }

    // Data processing (1-source): RBIT/REV/CLZ/CLS
    if ((inst & 0x5FE00000) == 0x5AC00000) {
        uint16_t opcode = (inst >> 10) & 0x3F;
        switch (opcode) {
            case 0: d.cls = InstClass::RBIT; break;
            case 1: d.cls = InstClass::REV16; break;
            case 2: d.cls = InstClass::REV32; break;
            case 3: d.cls = InstClass::REV; break;
            case 4: d.cls = InstClass::CLZ; break;
            case 5: d.cls = InstClass::CLS; break;
        }
        return true;
    }

    // Data processing (2-source): UDIV/SDIV/LSL/LSR/ASR/ROR
    if ((inst & 0x5FE00000) == 0x1AC00000) {
        uint16_t opcode = (inst >> 10) & 0x3F;
        switch (opcode) {
            case 2: d.cls = InstClass::UDIV; break;
            case 3: d.cls = InstClass::SDIV; break;
            case 8: d.cls = InstClass::LSL; break;
            case 9: d.cls = InstClass::LSR; break;
            case 10: d.cls = InstClass::ASR; break;
            case 11: d.cls = InstClass::ROR; break;
        }
        return true;
    }

    // Data processing (3-source): MADD/MSUB
    if ((inst & 0x1F000000) == 0x1B000000) {
        bool o0 = (inst >> 21) & 1;
        d.cls = o0 ? InstClass::MSUB : InstClass::MADD;
        d.ra = (inst >> 10) & 0x1F;
        return true;
    }

    // Conditional compare (CCMP/CCMN)
    if ((inst & 0x5FE00000) == 0x7A400000 ||
        (inst & 0x5FE00000) == 0x7A000000) {
        d.cls = ((inst >> 11) & 1) ? InstClass::CCMP : InstClass::CCMN;
        d.cond = (inst >> 12) & 0xF;
        return true;
    }

    // ── SIMD / FP (subset — full implementation in 1.2.0) ───────────
    // For now, just recognize the categories so we don't crash.

    // FMOV Vd.D[1], Rn / FMOV Rn, Vm.D[1] (general ↔ FP, 64-bit with index)
    // Encoding: 1001 1110 1010 1111 0000 00 Rn Rd (to FP, high half)
    //           1001 1110 1011 1111 0000 00 Rn Rd (from FP, high half)
    if ((inst & 0xFFE0FC00) == 0x9EA00000) {
        bool to_fp = (inst >> 16) & 1;
        d.cls = to_fp ? InstClass::FMOV_VD1 : InstClass::FMOV_RVD1;
        d.is_vec = true;
        return true;
    }

    if ((inst & 0xBE000000) == 0x0C000000) {
        // SIMD load/store
        d.cls = ((inst >> 22) & 1) ? InstClass::SIMD_LD1 : InstClass::SIMD_ST1;
        return true;
    }
    if ((inst & 0x9E000000) == 0x0E000000) {
        // SIMD logical/shift/other
        d.is_vec = true;
        // Detect DUP, CNT, REV, etc. by sub-opcode
        d.cls = InstClass::SIMD_LOGICAL;  // default
        return true;
    }
    if ((inst & 0xFFE00000) == 0x1E200000) {
        // Scalar FP (FADD/FSUB/FMUL/FDIV/FCMP/FCVT/FMOV)
        d.is_vec = true;
        d.cls = InstClass::FMOV;  // default, will refine in 1.2.0
        return true;
    }

    // Unrecognized — mark as UNKNOWN. The interpreter will throw a
    // DecodeError, which is what we want (better to crash loudly than
    // silently mis-execute).
    d.cls = InstClass::UNKNOWN;
    return false;
}

} // namespace arm64emu
