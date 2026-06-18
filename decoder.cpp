// decoder.cpp — Pure ARM64 instruction decoder.
//
// This is the SINGLE SOURCE OF TRUTH for instruction decode. The
// interpreter calls decode() once per instruction, then dispatches on
// d.cls. The interpreter NEVER does bit extraction — it only reads
// d.* fields and executes.
//
// All decode logic lives here:
//   - Bit pattern matching
//   - Field extraction (rd, rn, imm, shift, etc.)
//   - SP vs XZR disambiguation
//   - Shift amount calculation
//   - Addressing mode computation
//   - InstClass classification
//
// The future JIT will call decode() then emit x86_64 code based on
// d.cls — sharing the exact same decode logic as the interpreter.

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
// `sf` is currently unused — extend_reg produces a full 64-bit value and the
// caller is responsible for masking to the operand width. Kept in the
// signature so the future JIT can specialize on sf without an API change.
uint64_t extend_reg(uint64_t val, uint8_t option, uint8_t shift, bool /*sf*/) {
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

// ── Decode logical immediate bitmask ────────────────────────────────────
// The ARM ARM uses a compact encoding for logical immediates:
//   N:imms:immr defines a bitmask of `width` ones, rotated by immr,
//   then replicated to fill 64 bits. This is complex but deterministic.
static uint64_t decode_bitmask_imm(bool N, uint8_t immr, uint8_t imms, bool sf) {
    int len = 0;
    if (N) len = 6;
    else {
        // Find highest set bit in (~imms & 0x3F) — gives len
        uint8_t combined = (~imms) & 0x3F;
        if (combined == 0) return 0; // reserved
        for (int i = 5; i >= 0; i--) {
            if (combined & (1 << i)) { len = i; break; }
        }
    }
    int esize = 1 << len;
    int levels = esize - 1;
    int S = imms & levels;
    int R = immr & levels;
    int width = S + 1;
    if (width > esize) return 0; // reserved
    // Build the element: `width` ones at the bottom
    uint64_t elem = (width >= 64) ? ~0ULL : ((1ULL << width) - 1);
    // Rotate right by R within esize bits
    elem = (elem >> R) | (elem << (esize - R));
    elem &= (esize == 64) ? ~0ULL : ((1ULL << esize) - 1);
    // Replicate to 64 bits
    uint64_t result = 0;
    for (int i = 0; i < 64; i += esize) {
        result |= elem << i;
    }
    if (!sf) result &= 0xFFFFFFFF;
    return result;
}

// ── Main decode function ────────────────────────────────────────────────
// Hierarchical decode. Fills ALL fields the interpreter will need.
// The interpreter never does bit extraction — it reads d.* and executes.
//
// Structure: a top-level switch on the major encoding group (bits 28:24),
// then nested checks within each group. This prevents encoding collisions
// (e.g. pre-index STP/LDP vs ORR) by checking the major group FIRST.
bool decode(DecodedInst& d, uint32_t inst) {
    d.raw = inst;
    d.cls = InstClass::UNKNOWN;

    // Extract common fields
    d.rd  = inst & 0x1F;
    d.rn  = (inst >> 5) & 0x1F;
    d.rm  = (inst >> 16) & 0x1F;
    d.rt  = inst & 0x1F;
    d.rt2 = (inst >> 10) & 0x1F;
    d.rs  = (inst >> 16) & 0x1F;
    d.sf  = (inst >> 31) & 1;
    d.size = (inst >> 30) & 3;
    d.cond = inst & 0xF;
    d.Q = (inst >> 30) & 1;
    d.ftype = (inst >> 22) & 3;

    // ── Hierarchical dispatch on bits 28:24 ──────────────────────────
    // The ARM ARM defines major encoding groups by bits 28:24 (with
    // bits 31:29 as sub-selectors within each group). We switch on
    // bits 28:24 first, then narrow down. This prevents collisions
    // like pre-index STP/LDP (bits 28:24 = 01000, bit 25=1) being
    // caught by logical shifted register (bits 28:24 = 01010).
    //
    // Major groups (bits 28:24):
    //   00001 → Unconditional branch (B/BL)
    //   00101 → Conditional branch / system
    //   01010 → Data processing (register) — logical shifted
    //   01011 → Data processing (register) — add/sub shifted
    //   10000 → Load/store pair
    //   10001 → Load/store pair (post-index variant)
    //   10010 → Load/store (various)
    //   10011 → Load/store (various)
    //   10100 → Data processing (immediate) — MOVZ/MOVK/ADRP/etc
    //   10101 → Data processing (immediate) — ADD/SUB imm
    //   10110 → Data processing (immediate) — logical imm
    //   10111 → Data processing (immediate) — bitfield
    //   11010 → Branches (register) / system
    //   11011 → Data processing (register) — 3-source

    uint8_t op28 = (inst >> 24) & 0x1F;  // bits 28:24

    switch (op28) {

    // ════════════════════════════════════════════════════════════════
    // Group: Unconditional branch (B/BL)
    //   bits 28:24 = 00101, but we also catch ADR/ADRP here (bits 28:24=10000)
    // ════════════════════════════════════════════════════════════════
    default:
        break;  // fall through to flat checks below for complex groups
    }

    // For now, keep the existing flat checks but with the STP/LDP fix.
    // The switch above documents the intended hierarchy; the flat checks
    // below are ordered to match the hierarchical priority.

    // ── B / BL ──
    if ((inst & 0x7C000000) == 0x14000000) {
        d.cls = d.sf ? InstClass::BL : InstClass::B;
        d.imm = arm64emu::sign_extend(inst & 0x03FFFFFF, 26) << 2;
        return true;
    }

    // ── B.cond ──
    if ((inst & 0xFF000010) == 0x54000000) {
        d.cls = InstClass::Bcond;
        d.cond = inst & 0xF;
        d.imm = arm64emu::sign_extend((inst >> 5) & 0x7FFFF, 19) << 2;
        return true;
    }

    // ── CBZ / CBNZ ──
    if ((inst & 0x7E000000) == 0x34000000) {
        d.cls = ((inst >> 24) & 1) ? InstClass::CBNZ : InstClass::CBZ;
        d.imm = arm64emu::sign_extend((inst >> 5) & 0x7FFFF, 19) << 2;
        d.rt = inst & 0x1F;
        return true;
    }

    // ── TBZ / TBNZ ──
    if ((inst & 0x7E000000) == 0x36000000) {
        d.cls = ((inst >> 24) & 1) ? InstClass::TBNZ : InstClass::TBZ;
        d.imm = arm64emu::sign_extend((inst >> 5) & 0x3FFF, 14) << 2;
        uint8_t b40 = (inst >> 19) & 0x1F;
        uint8_t b5  = (inst >> 31) & 1;
        d.imm_u = (b5 << 5) | b40;
        d.rt = inst & 0x1F;
        return true;
    }

    // ── BR / BLR / RET ──
    if ((inst & 0xFFFFFC00) == 0xD61F0000) { d.cls = InstClass::BR;  d.rn = (inst>>5)&0x1F; return true; }
    if ((inst & 0xFFFFFC00) == 0xD63F0000) { d.cls = InstClass::BLR; d.rn = (inst>>5)&0x1F; return true; }
    if ((inst & 0xFFFFFC1F) == 0xD65F0000) { d.cls = InstClass::RET; d.rn = (inst>>5)&0x1F; return true; }

    // ── System (SVC/BRK/HLT/MSR/MRS/CLREX/HINT) ──
    if ((inst & 0xFFE0001F) == 0xD4000001) { d.cls = InstClass::SVC_IMM; d.imm_u = (inst >> 5) & 0xFFFF; return true; }
    if ((inst & 0xFFE0001F) == 0xD4000002) { d.cls = InstClass::HVC_IMM; return true; }
    if ((inst & 0xFFE0001F) == 0xD4000003) { d.cls = InstClass::SMC_IMM; return true; }
    if ((inst & 0xFFE0001F) == 0xD4200000) { d.cls = InstClass::BRK_IMM; d.imm_u = (inst >> 5) & 0xFFFF; return true; }
    if ((inst & 0xFFE0001F) == 0xD4400000) { d.cls = InstClass::HLT_IMM; d.imm_u = (inst >> 5) & 0xFFFF; return true; }
    if (inst == 0xD503305F) { d.cls = InstClass::CLREX_INST; return true; }
    if ((inst & 0xFF000000) == 0xD5000000) {
        d.sys_L = (inst >> 21) & 1;
        d.sys_op0 = (inst >> 19) & 0x3;
        d.sys_op1 = (inst >> 16) & 0x7;
        d.sys_crn = (inst >> 12) & 0xF;
        d.sys_crm = (inst >> 8) & 0xF;
        d.sys_op2 = (inst >> 5) & 0x7;
        d.rt = inst & 0x1F;
        d.cls = d.sys_L ? InstClass::MRS_SYS : InstClass::MSR_SYS;
        return true;
    }
    if ((inst & 0xFFFFF000) == 0xD5033000) { d.cls = InstClass::HINT; return true; }

    // ── Data processing (immediate) ──
    // ADR / ADRP
    if ((inst & 0x1F000000) == 0x10000000) {
        bool is_adrp = (inst >> 31) & 1;
        d.cls = is_adrp ? InstClass::ADRP : InstClass::ADR;
        uint64_t immlo = (inst >> 29) & 3;
        uint64_t immhi = (inst >> 5) & 0x7FFFF;
        d.imm_u = (immhi << 2) | immlo;
        if (is_adrp) d.imm = arm64emu::sign_extend(d.imm_u << 12, 33);
        else d.imm = arm64emu::sign_extend(d.imm_u, 21);
        d.rd = inst & 0x1F;
        return true;
    }

    // MOVN/MOVZ/MOVK
    if ((inst & 0x1F800000) == 0x12800000) {
        uint8_t opc = (inst >> 29) & 3;
        switch (opc) {
            case 0: d.cls = InstClass::MOVN; break;
            case 2: d.cls = InstClass::MOVZ; break;
            case 3: d.cls = InstClass::MOVK; break;
            default: d.cls = InstClass::UNKNOWN; return false;
        }
        d.imm16 = (inst >> 5) & 0xFFFF;
        d.hw = (inst >> 21) & 3;
        d.shift = d.hw * 16;
        d.rd = inst & 0x1F;
        return true;
    }

    // ADD/SUB immediate
    if ((inst & 0x1F000000) == 0x11000000) {
        bool S = (inst >> 29) & 1;
        d.is_sub = (inst >> 30) & 1;
        d.set_flags = S;
        d.imm_u = (inst >> 10) & 0xFFF;
        bool sh = (inst >> 22) & 1;
        d.shift = sh ? 12 : 0;
        d.rn = (inst >> 5) & 0x1F;
        d.rd = inst & 0x1F;
        d.reads_sp = (d.rn == 31 && !S);
        d.writes_sp = (d.rd == 31 && !S);
        if (d.is_sub) d.cls = S ? InstClass::SUBS_IMM : InstClass::SUB_IMM;
        else          d.cls = S ? InstClass::ADDS_IMM : InstClass::ADD_IMM;
        return true;
    }

    // Bitfield (SBFM/BFM/UBFM)
    if ((inst & 0x1F000000) == 0x13000000) {
        uint8_t opc = (inst >> 29) & 3;
        d.N = (inst >> 22) & 1;
        d.immr = (inst >> 16) & 0x3F;
        d.imms = (inst >> 10) & 0x3F;
        d.rn = (inst >> 5) & 0x1F;
        d.rd = inst & 0x1F;
        switch (opc) {
            case 0: d.cls = InstClass::SBFM; break;
            case 1: d.cls = InstClass::BFM;  break;
            case 2: d.cls = InstClass::UBFM; break;
            default: d.cls = InstClass::UNKNOWN; return false;
        }
        return true;
    }

    // EXTR
    if ((inst & 0x1F800000) == 0x13800000) {
        d.cls = InstClass::EXTR;
        d.N = (inst >> 22) & 1;
        d.immr = (inst >> 16) & 0x3F;
        d.rm = (inst >> 16) & 0x1F;
        d.imms = (inst >> 10) & 0x3F;
        d.rn = (inst >> 5) & 0x1F;
        d.rd = inst & 0x1F;
        return true;
    }

    // Logical immediate (AND/ORR/EOR/ANDS)
    if ((inst & 0x1F800000) == 0x12000000) {
        uint8_t opc = (inst >> 29) & 3;
        d.N = (inst >> 22) & 1;
        d.immr = (inst >> 16) & 0x3F;
        d.imms = (inst >> 10) & 0x3F;
        d.rn = (inst >> 5) & 0x1F;
        d.rd = inst & 0x1F;
        d.set_flags = (opc == 3);
        d.writes_sp = (d.rd == 31 && opc == 1);
        d.imm_u = decode_bitmask_imm(d.N, d.immr, d.imms, d.sf);
        switch (opc) {
            case 0: d.cls = InstClass::AND_IMM; break;
            case 1: d.cls = InstClass::ORR_IMM; break;
            case 2: d.cls = InstClass::EOR_IMM; break;
            case 3: d.cls = InstClass::ANDS_IMM; break;
        }
        return true;
    }

    // ════════════════════════════════════════════════════════════════
    // Group: Load/Store  (checked BEFORE data-proc-register to prevent
    //        STP/LDP pre-index collision with logical shifted register)
    // ════════════════════════════════════════════════════════════════

    // ── LSE atomics ──
    if ((inst & 0x3F200C00) == 0x38200000) {
        d.size = (inst >> 30) & 3;
        d.acquire = (inst >> 23) & 1;
        d.is_load = (inst >> 22) & 1;
        d.rs = (inst >> 16) & 0x1F;
        d.atom_op = (inst >> 12) & 0xF;
        d.rn = (inst >> 5) & 0x1F;
        d.rt = inst & 0x1F;
        d.cls = InstClass::LSE_ATOMIC;
        return true;
    }

    // ── Load/store pair (STP/LDP, ALL modes including pre-index) ──
    // HIERARCHICAL FIX: check bits 29:25 = 10100 (post/offset) OR 10101 (pre)
    // for BOTH V=0 (GP) and V=1 (SIMD). The mask 0x3A000000 masks bit 26
    // (V) to 0, so it catches both GP and SIMD. We check two patterns:
    //   0x28000000 = bits 29:25 = 10100 (post-index or signed offset)
    //   0x2A000000 = bits 29:25 = 10101 (pre-index)
    // This is checked BEFORE logical shifted register to prevent the
    // pre-index STP/LDP collision with ORR.
    if ((inst & 0x3A000000) == 0x28000000 ||  // post/offset (bit 25=0)
        (inst & 0x3A000000) == 0x2A000000) {  // pre-index (bit 25=1)
        // Verify this is actually STP/LDP by checking bits 25:23 are a
        // valid addressing mode (001=post, 010=offset, 011=pre).
        // LDUR/STUR (SIMD) also has bits 29:25 = 10100 but bits 25:23 = 000.
        uint8_t mode_check = (inst >> 23) & 7;
        if (mode_check != 1 && mode_check != 2 && mode_check != 3) {
            // Not STP/LDP — fall through to other handlers
        } else {
        uint8_t opc = (inst >> 30) & 3;
        d.is_vec = (inst >> 26) & 1;
        d.is_load = (inst >> 22) & 1;
        // bits 25:23 determine the addressing mode:
        //   001 = post-index, 010 = signed offset, 011 = pre-index
        d.mode = (inst >> 23) & 3;
        d.writeback = (d.mode == 1 || d.mode == 3);
        int16_t imm7 = arm64emu::sign_extend((inst >> 15) & 0x7F, 7);
        d.rt2 = (inst >> 10) & 0x1F;
        d.rn = (inst >> 5) & 0x1F;
        d.rt = inst & 0x1F;
        int esize;
        if (d.is_vec) {
            esize = (opc == 0) ? 4 : (opc == 1) ? 8 : 16;
        } else {
            esize = (opc == 2) ? 8 : 4;
        }
        d.disp = imm7 * esize;
        d.cls = d.is_load ? InstClass::LDP : InstClass::STP;
        return true;
        }  // end else (valid STP mode)
    }

    // ── Load/store exclusive ──
    if ((inst & 0x3F000000) == 0x08000000) {
        d.size = (inst >> 30) & 3;
        d.acquire = (inst >> 23) & 1;
        d.is_load = (inst >> 22) & 1;
        d.rs = (inst >> 16) & 0x1F;
        d.rn = (inst >> 5) & 0x1F;
        d.rt = inst & 0x1F;
        d.excl_low6 = (inst >> 10) & 0x3F;
        if (d.excl_low6 == 0x0F || d.excl_low6 == 0x1F) {
            if (d.is_load) d.cls = d.acquire ? InstClass::LDAXR : InstClass::LDXR;
            else           d.cls = d.acquire ? InstClass::STLXR : InstClass::STXR;
        } else if (d.excl_low6 == 0x3F) {
            d.cls = d.is_load ? InstClass::LDAR : InstClass::STLR;
        } else {
            d.cls = InstClass::UNKNOWN;
            return false;
        }
        return true;
    }

    // ── Load/store (unsigned immediate offset) ──
    if ((inst & 0x3B000000) == 0x39000000) {
        d.size = (inst >> 30) & 3;
        d.opc_ls = (inst >> 22) & 3;
        d.is_vec = (inst >> 26) & 1;
        uint16_t imm12 = (inst >> 10) & 0xFFF;
        d.rn = (inst >> 5) & 0x1F;
        d.rt = inst & 0x1F;
        bool is_q = (d.opc_ls & 2) && d.size == 0;
        uint64_t scale = is_q ? 4 : d.size;
        d.disp = (int64_t)(imm12 << scale);
        d.is_load = d.is_vec ? (d.opc_ls & 1) : ((d.opc_ls & 2) || (d.opc_ls & 1));
        d.cls = d.is_load ? InstClass::LDR_IMM : InstClass::STR_IMM;
        return true;
    }

    // ── Load/store (unscaled / post-index / pre-index) ──
    if ((inst & 0x3B200C00) == 0x38000000 ||
        (inst & 0x3B200C00) == 0x38000400 ||
        (inst & 0x3B200C00) == 0x38000C00) {
        d.size = (inst >> 30) & 3;
        d.opc_ls = (inst >> 22) & 3;
        d.is_vec = (inst >> 26) & 1;
        int16_t imm9 = arm64emu::sign_extend((inst >> 12) & 0x1FF, 9);
        d.rn = (inst >> 5) & 0x1F;
        d.rt = inst & 0x1F;
        uint8_t mode_bits = (inst >> 10) & 3;
        d.writeback = (mode_bits == 1 || mode_bits == 3);
        d.mode = (mode_bits == 1) ? 1 : (mode_bits == 3) ? 2 : 0;
        d.disp = imm9;
        d.is_load = d.is_vec ? (d.opc_ls & 1) : ((d.opc_ls & 2) || (d.opc_ls & 1));
        d.cls = d.is_load ? InstClass::LDR_UNS : InstClass::STR_UNS;
        return true;
    }

    // ── Load/store (register offset) ──
    if ((inst & 0x3B200C00) == 0x38200800) {
        d.size = (inst >> 30) & 3;
        d.opc_ls = (inst >> 22) & 3;
        d.is_vec = (inst >> 26) & 1;
        d.rm = (inst >> 16) & 0x1F;
        d.extend = (inst >> 13) & 7;
        d.shift = (inst >> 12) & 1;
        d.rn = (inst >> 5) & 0x1F;
        d.rt = inst & 0x1F;
        d.is_load = d.is_vec ? (d.opc_ls & 1) : ((d.opc_ls & 2) || (d.opc_ls & 1));
        d.cls = d.is_load ? InstClass::LDR_REG : InstClass::STR_REG;
        return true;
    }

    // ════════════════════════════════════════════════════════════════
    // Group: Data processing (register)
    // ════════════════════════════════════════════════════════════════

    // Add/subtract shifted register
    if ((inst & 0x1F200000) == 0x0B000000) {
        bool S = (inst >> 29) & 1;
        d.is_sub = (inst >> 30) & 1;
        d.set_flags = S;
        d.shift_type = (inst >> 22) & 3;
        d.shift = (inst >> 10) & 0x3F;
        d.rm = (inst >> 16) & 0x1F;
        d.rn = (inst >> 5) & 0x1F;
        d.rd = inst & 0x1F;
        if (d.is_sub) d.cls = S ? InstClass::SUBS_REG : InstClass::SUB_REG;
        else          d.cls = S ? InstClass::ADDS_REG : InstClass::ADD_REG;
        return true;
    }

    // Add/subtract extended register
    if ((inst & 0x1FE00000) == 0x0B200000) {
        bool S = (inst >> 29) & 1;
        d.is_sub = (inst >> 30) & 1;
        d.set_flags = S;
        d.extend = (inst >> 13) & 7;
        d.shift = (inst >> 10) & 7;
        d.rm = (inst >> 16) & 0x1F;
        d.rn = (inst >> 5) & 0x1F;
        d.rd = inst & 0x1F;
        d.reads_sp = (d.rn == 31);
        d.writes_sp = (d.rd == 31 && !S);
        if (d.is_sub) d.cls = S ? InstClass::SUBS_REG : InstClass::SUB_REG;
        else          d.cls = S ? InstClass::ADDS_REG : InstClass::ADD_REG;
        return true;
    }

    // Add/subtract with carry (ADC/ADCS/SBC/SBCS)
    if ((inst & 0x1FE00000) == 0x1A000000) {
        bool S = (inst >> 29) & 1;
        d.is_sub = (inst >> 30) & 1;
        d.set_flags = S;
        d.rm = (inst >> 16) & 0x1F;
        d.rn = (inst >> 5) & 0x1F;
        d.rd = inst & 0x1F;
        if (d.is_sub) d.cls = S ? InstClass::SBCS_REG : InstClass::SBC_REG;
        else          d.cls = S ? InstClass::ADCS_REG : InstClass::ADC_REG;
        return true;
    }

    // Logical shifted register (AND/ORR/EOR/ANDS + BIC/ORN/EON/BICS)
    if ((inst & 0x1F000000) == 0x0A000000) {
        uint8_t opc = (inst >> 29) & 3;
        d.set_flags = (opc == 3);
        d.shift_type = (inst >> 22) & 3;
        d.N = (inst >> 21) & 1;
        d.shift = (inst >> 10) & 0x3F;
        d.rm = (inst >> 16) & 0x1F;
        d.rn = (inst >> 5) & 0x1F;
        d.rd = inst & 0x1F;
        d.writes_sp = (d.rd == 31 && opc == 1);
        switch (opc) {
            case 0: d.cls = InstClass::AND_REG;  break;
            case 1: d.cls = InstClass::ORR_REG;  break;
            case 2: d.cls = InstClass::EOR_REG;  break;
            case 3: d.cls = InstClass::ANDS_REG; break;
        }
        return true;
    }

    // Conditional select (CSEL/CSINC/CSINV/CSNEG)
    if ((inst & 0x1FE00000) == 0x1A800000) {
        uint8_t op = (inst >> 10) & 3;
        d.rm = (inst >> 16) & 0x1F;
        d.cond = (inst >> 12) & 0xF;
        d.rn = (inst >> 5) & 0x1F;
        d.rd = inst & 0x1F;
        switch (op) {
            case 0: d.cls = InstClass::CSEL;  break;
            case 1: d.cls = InstClass::CSINC; break;
            case 2: d.cls = InstClass::CSINV; break;
            case 3: d.cls = InstClass::CSNEG; break;
        }
        return true;
    }

    // Conditional compare (CCMP/CCMN)
    if ((inst & 0x3FE00000) == 0x3A400000) {
        d.is_sub = (inst >> 30) & 1;
        d.is_register = (inst >> 21) & 1;
        d.rm = (inst >> 16) & 0x1F;
        d.cond = (inst >> 12) & 0xF;
        d.rn = (inst >> 5) & 0x1F;
        d.nzcv_field = inst & 0xF;
        d.cls = d.is_sub ? InstClass::CCMP : InstClass::CCMN;
        return true;
    }

    // Data processing (1-source): RBIT/REV/REV16/REV32/CLZ/CLS
    if ((inst & 0x5FE00000) == 0x5AC00000) {
        d.dp_opcode = (inst >> 10) & 0x3F;
        d.rn = (inst >> 5) & 0x1F;
        d.rd = inst & 0x1F;
        switch (d.dp_opcode) {
            case 0:  d.cls = InstClass::RBIT;  break;
            case 1:  d.cls = InstClass::REV16; break;
            case 2:  d.cls = InstClass::REV32; break;
            case 3:  d.cls = InstClass::REV;   break;
            case 4:  d.cls = InstClass::CLZ;   break;
            case 5:  d.cls = InstClass::CLS;   break;
            default: d.cls = InstClass::UNKNOWN; return false;
        }
        return true;
    }

    // Data processing (2-source): UDIV/SDIV/LSL/LSR/ASR/ROR
    if ((inst & 0x5FE00000) == 0x1AC00000) {
        d.dp_opcode = (inst >> 10) & 0x3F;
        d.rm = (inst >> 16) & 0x1F;
        d.rn = (inst >> 5) & 0x1F;
        d.rd = inst & 0x1F;
        switch (d.dp_opcode) {
            case 2:  d.cls = InstClass::UDIV; break;
            case 3:  d.cls = InstClass::SDIV; break;
            case 8:  d.cls = InstClass::LSL;  break;
            case 9:  d.cls = InstClass::LSR;  break;
            case 10: d.cls = InstClass::ASR;  break;
            case 11: d.cls = InstClass::ROR;  break;
            default: d.cls = InstClass::UNKNOWN; return false;
        }
        return true;
    }

    // Data processing (3-source): MADD/MSUB/SMADDL/SMSUBL/UMADDL/UMSUBL/UMULH/SMULH
    if ((inst & 0x1F000000) == 0x1B000000) {
        d.sub_op = (inst >> 21) & 0x7;
        d.o0 = (inst >> 15) & 1;
        d.rm = (inst >> 16) & 0x1F;
        d.ra = (inst >> 10) & 0x1F;
        d.rn = (inst >> 5) & 0x1F;
        d.rd = inst & 0x1F;
        switch (d.sub_op) {
            case 0: d.cls = d.o0 ? InstClass::MSUB   : InstClass::MADD;   break;
            case 1: d.cls = d.o0 ? InstClass::SMSUBL : InstClass::SMADDL; break;
            case 2: d.cls = InstClass::SMULH; break;
            case 5: d.cls = d.o0 ? InstClass::UMSUBL : InstClass::UMADDL; break;
            case 6: d.cls = InstClass::UMULH; break;
            default: d.cls = InstClass::UNKNOWN; return false;
        }
        return true;
    }

    // ════════════════════════════════════════════════════════════════
    // Group: SIMD / FP
    // ════════════════════════════════════════════════════════════════

    // FMOV Vd.D[1], Rn / FMOV Rn, Vm.D[1]
    if ((inst & 0xFFE0FC00) == 0x9EA00000) {
        bool to_fp = (inst >> 16) & 1;
        d.cls = to_fp ? InstClass::FMOV_VD1 : InstClass::FMOV_RVD1;
        d.is_vec = true;
        d.rn = (inst >> 5) & 0x1F;
        d.rd = inst & 0x1F;
        return true;
    }

    // SIMD load/store multiple structures (LD1/ST1)
    if ((inst & 0xBE000000) == 0x0C000000) {
        d.is_vec = true;
        d.is_load = (inst >> 22) & 1;
        d.cls = d.is_load ? InstClass::SIMD_LD1 : InstClass::SIMD_ST1;
        d.size = (inst >> 10) & 3;
        d.rt = inst & 0x1F;
        d.rn = (inst >> 5) & 0x1F;
        d.rm = (inst >> 16) & 0x1F;
        return true;
    }

    // SIMD data processing
    if ((inst & 0x9E000000) == 0x0E000000) {
        d.is_vec = true;
        d.cls = InstClass::SIMD_DP;
        d.Q = (inst >> 30) & 1;
        d.size = (inst >> 22) & 3;
        d.rd = inst & 0x1F;
        d.rn = (inst >> 5) & 0x1F;
        d.rm = (inst >> 16) & 0x1F;
        d.cmode = (inst >> 12) & 0xF;
        return true;
    }

    // FP scalar
    if ((inst & 0xFFE00000) == 0x1E200000 ||
        (inst & 0xFF000000) == 0x1E000000 ||
        (inst & 0xFF000000) == 0x9E000000) {
        d.is_vec = true;
        d.cls = InstClass::FP_SCALAR;
        d.ftype = (inst >> 22) & 3;
        d.rd = inst & 0x1F;
        d.rn = (inst >> 5) & 0x1F;
        d.rm = (inst >> 16) & 0x1F;
        d.fp_opcode = (inst >> 12) & 0xF;
        d.rmode = (inst >> 19) & 3;
        return true;
    }

    // Unrecognized
    d.cls = InstClass::UNKNOWN;
    return false;
}

} // namespace arm64emu
