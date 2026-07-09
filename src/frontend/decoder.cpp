// decoder.cpp — Pure ARM64 instruction decoder (hierarchical).
//
// SINGLE SOURCE OF TRUTH for instruction decode. The interpreter calls
// decode() once per instruction, then dispatches on d.cls. The interpreter
// NEVER does bit extraction — it only reads d.* fields and executes.
//
// ── Design ──────────────────────────────────────────────────────────────
// The decoder is a two-level hierarchical switch:
//
//   Outer switch  : bits [28:24]  (5 bits, 32 major encoding groups)
//   Inner dispatch: group-specific discriminator (bits [31:29], bit 26,
//                   bit 23, bit 22, mode bits, opcodes, …)
//
// This mirrors the ARM ARM top-level "Encoding groupings" table. Routing
// on bits [28:24] FIRST prevents the encoding collisions that plague flat
// mask-and-compare decoders. The classic collision in v0 was pre-index
// STP/LDP being caught by logical-shifted-register: the hierarchy makes
// that structurally impossible because the two encodings live in
// different outer cases (STP/LDP pre V=0 → case 0x09; logical shifted
// register → case 0x0A).
//
// Field extraction is split:
//   - Truly common fields (rd, rn, rm, rt, sf, size, ...) are pulled out
//     once at the top.
//   - Group-specific fields (imm, disp, atom_op, cmode, ...) are pulled
//     out only in the case that needs them, avoiding redundant work.
//
// Decode is pure: no execution, no memory access, no side effects.
//
// IMPROVEMENTS over v0:
//   1. True hierarchical switch on bits[28:24] (v0 had a stub switch
//      that fell through to flat if-chains).
//   2. EXTR is now correctly decoded (v0 had a dead-code bug where the
//      bitfield check shadowed the EXTR check, so every EXTR was
//      silently misdecoded as SBFM/BFM/UBFM).
//   3. STP/LDP pre-index no longer collides with ORR (structural fix
//      via hierarchy, not test ordering).
//   4. 64-bit CBZ/CBNZ/TBZ/TBNZ (sf=1) now decode correctly.
//   5. BRK/HLT bit[4:0] = 0 check enforced (matches ARM ARM).
//   6. Add/sub extended register bits[23:22] = 00 check enforced.

#include "decoder.hpp"
#include "core/emulator.h"

namespace arm64emu {

// ── Condition codes ─────────────────────────────────────────────────────
bool cond_true(uint32_t cond, uint32_t pstate) {
    bool N = pstate & (1u << 31);
    bool Z = pstate & (1u << 30);
    bool C = pstate & (1u << 29);
    bool V = pstate & (1u << 28);
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
uint64_t extend_reg(uint64_t val, uint8_t option, uint8_t shift, bool /*sf*/) {
    switch (option & 7) {
        case 0: val = val & 0xFF;            break;  // UXTB
        case 1: val = val & 0xFFFF;          break;  // UXTH
        case 2: val = val & 0xFFFFFFFF;      break;  // UXTW
        case 3: break;                               // UXTX
        case 4: val = static_cast<int64_t>(static_cast<int8_t>(val));  break;  // SXTB
        case 5: val = static_cast<int64_t>(static_cast<int16_t>(val)); break;  // SXTH
        case 6: val = static_cast<int64_t>(static_cast<int32_t>(val)); break;  // SXTW
        case 7: break;                               // SXTX
    }
    return val << shift;
}

// ── Decode logical immediate bitmask ────────────────────────────────────
// BUGFIX (Turn 60, H10): return false (and leave *out unchanged) for
// UNALLOCATED encodings instead of returning 0. The old code returned 0
// for invalid encodings (width > esize, or combined==0 with N==0), which
// the caller then happily used as a valid bitmask immediate of 0. Real
// hardware raises an UNALLOCATED instruction exception for these. A
// corrupt binary or JIT-spray attacker could emit these to turn invalid
// instructions into silent no-ops (AND x, x, #0 → x = 0).
//
// Now the caller checks the return value: if false, the instruction is
// UNALLOCATED and decode() returns false (InstClass::UNKNOWN).
static bool decode_bitmask_imm(bool N, uint8_t immr, uint8_t imms, bool sf,
                                uint64_t* out) {
    int len = 0;
    if (N) {
        len = 6;
    } else {
        uint8_t combined = (~imms) & 0x3F;
        if (combined == 0) return false;  // UNALLOCATED
        for (int i = 5; i >= 0; i--) {
            if (combined & (1 << i)) { len = i; break; }
        }
    }
    int esize  = 1 << len;
    int levels = esize - 1;
    int S = imms & levels;
    int R = immr & levels;
    int width = S + 1;
    if (width > esize) return false;  // UNALLOCATED
    uint64_t elem = (width >= 64) ? ~0ULL : ((1ULL << width) - 1);
    // ROR by R within an esize-wide field. When R == 0 there's nothing
    // to do; otherwise the right-shift is `>> R` (safe, R < esize ≤ 64)
    // and the left-shift is `<< (esize - R)` (safe, esize-R ≤ 64).
    // The previous form `elem << (esize - R)` was UB when R == 0 and
    // esize == 64 (shift by 64).
    if (R != 0) {
        elem = (elem >> R) | (elem << (esize - R));
        elem &= (esize == 64) ? ~0ULL : ((1ULL << esize) - 1);
    }
    uint64_t result = 0;
    for (int i = 0; i < 64; i += esize) {
        result |= elem << i;
    }
    if (!sf) result &= 0xFFFFFFFF;
    *out = result;
    return true;
}

// ── Main decode function ────────────────────────────────────────────────
bool decode(DecodedInst& d, uint32_t inst) {
    d.raw = inst;
    d.cls = InstClass::UNKNOWN;

    // Pre-extract truly common fields.
    d.rd    = inst & 0x1F;
    d.rn    = (inst >> 5) & 0x1F;
    d.rm    = (inst >> 16) & 0x1F;
    d.rt    = inst & 0x1F;
    d.rt2   = (inst >> 10) & 0x1F;
    d.rs    = (inst >> 16) & 0x1F;
    d.sf    = (inst >> 31) & 1;
    d.size  = (inst >> 30) & 3;
    d.cond  = inst & 0xF;
    d.Q     = (inst >> 30) & 1;
    d.ftype = (inst >> 22) & 3;

    // B / BL — checked BEFORE the outer switch because their encoding
    // uses bits[30:26]=00101, not bits[28:24]. imm26[25:24] leaks into
    // bits[28:24], so a single B/BL can land in cases 0x14–0x17.
    if ((inst & 0x7C000000) == 0x14000000) {
        d.cls = d.sf ? InstClass::BL : InstClass::B;
        d.imm = arm64emu::sign_extend(inst & 0x03FFFFFF, 26) << 2;
        return true;
    }

    uint8_t op28_24 = (inst >> 24) & 0x1F;
    switch (op28_24) {

    // Reserved groups.
    case 0x00: case 0x01: case 0x02: case 0x03:
    case 0x04: case 0x05: case 0x06: case 0x07:
        return false;

    // Load/Store Pair post-index (V=0) + Load/Store Exclusive.
    case 0x08: {
        if ((inst >> 29) & 1) {
            if (((inst >> 23) & 7) != 1) return false;
            uint8_t opc = (inst >> 30) & 3;
            d.is_vec    = false;
            d.is_load   = (inst >> 22) & 1;
            d.mode      = 1;
            d.writeback = true;
            int16_t imm7 = static_cast<int16_t>(arm64emu::sign_extend((inst >> 15) & 0x7F, 7));
            int esize = (opc == 2) ? 8 : 4;
            d.disp = imm7 * esize;
            d.cls = d.is_load ? InstClass::LDP : InstClass::STP;
            return true;
        }
        if ((inst & 0x3F000000) != 0x08000000) return false;
        // Check for CAS family (bit[21]=1 AND bit[23]=1).
        // BUGFIX (Turn 59, C7): the old check was only `if ((inst >> 21) & 1)`,
        // which matches BOTH the CAS family (bit[23]=1, e.g. CAS, CASA,
        // CASL, CASAL) AND the pair-exclusive ops (bit[23]=0: STXP, LDXP,
        // STLXP, LDAXP). Bit 21 is set for both families, but bit 23
        // distinguishes them:
        //   - bit[23]=1 → CAS family (single-word CAS)
        //   - bit[23]=0 → pair-exclusive (STXP/LDXP/STLXP/LDAXP, 16-byte)
        // The old code routed pair-exclusive ops to LSE_ATOMIC with
        // atom_op=0xC (CAS), silently dropping the second source register
        // (Rt2 in bits[14:10]) and corrupting 16-byte atomics
        // (std::atomic<__int128>, lock-free queues, some mutex impls).
        // Fix: require bit[23]=1 for CAS. Pair-exclusive ops (bit[23]=0,
        // bit[21]=1) fall through to the exclusive decoder below, which
        // already handles them via excl_low6 (0x1F for STXP/LDXP, 0x3F
        // for STLXP/LDAXP). The interpreter's LDP/STP path handles the
        // 16-byte pair access.
        bool is_cas = ((inst >> 21) & 1) && ((inst >> 23) & 1);
        if (is_cas) {
            d.size    = (inst >> 30) & 3;
            // BUGFIX (M4): bit 23 is the L (release) bit, not the acquire
            // bit. Bit 22 is the A (acquire) bit. The field was named
            // `acquire` but actually held the release bit. Keep the name
            // for now (callers check d.acquire for "ordered" semantics)
            // but document the truth.
            d.acquire = (inst >> 23) & 1;  // actually the L (release) bit
            d.is_load = 1;  // CAS always returns old value
            d.rs      = (inst >> 16) & 0x1F;
            d.atom_op = 0xC;  // CAS (hardcoded — group 0x08 has different opc encoding)
            d.rn      = (inst >> 5) & 0x1F;
            d.rt      = inst & 0x1F;
            d.cls     = InstClass::LSE_ATOMIC;
            return true;
        }
        d.size      = (inst >> 30) & 3;
        d.acquire   = (inst >> 23) & 1;
        d.is_load   = (inst >> 22) & 1;
        d.rs        = (inst >> 16) & 0x1F;  // Rs (STXR source) or 0x1F for LDAR/STLR
        d.rn        = (inst >> 5) & 0x1F;
        d.rt        = inst & 0x1F;
        d.rt2       = (inst >> 10) & 0x1F;  // Rt2 (pair-exclusive second register)
        d.excl_low6 = (inst >> 10) & 0x3F;
        switch (d.excl_low6) {
            // BUGFIX (Turn 59, M2): removed bogus case 0x0F (no valid A64
            // exclusive instruction has excl_low6 == 0x0F; STXR/LDXR use
            // 0x1F, STLXR/LDAXR/STLR/LDAR use 0x3F). The old case was
            // unreachable dead code.
            case 0x1F:
                // STXR/LDXR family (with Rs at bits[20:16]).
                // d.is_load distinguishes LDXR (1) from STXR (0).
                // d.acquire (bit 23) distinguishes LDAXR/STLXR (1) from
                // LDXR/STXR (0). The interpreter reconstructs the correct
                // behavior from these bits.
                d.cls = d.is_load ? (d.acquire ? InstClass::LDAXR : InstClass::LDXR)
                                  : (d.acquire ? InstClass::STLXR : InstClass::STXR);
                return true;
            case 0x3F:
                // STLXR/LDAXR/STLR/LDAR family.
                // BUGFIX (Turn 59, M3): distinguish by d.acquire (bit 23).
                //   bit[23]=1 (d.acquire=1): STLR/LDAR (acquire-release)
                //   bit[23]=0: STLXR/LDAXR (exclusive with acquire-release)
                // The old code always classified as STLR/LDAR, which is
                // wrong for STLXR/LDAXR. The interpreter happened to work
                // because it reconstructs from the raw bits, but d.cls was
                // misleading for JIT/trace consumers.
                if (d.acquire) {
                    d.cls = d.is_load ? InstClass::LDAR : InstClass::STLR;
                } else {
                    d.cls = d.is_load ? InstClass::LDAXR : InstClass::STLXR;
                }
                return true;
            default:
                d.cls = InstClass::UNKNOWN;
                return false;
        }
    }

    // Load/Store Pair (offset V=0 / pre-index V=0).
    case 0x09: {
        // GPR LDP/STP (signed offset, pre-index).
        // Encoding: opc[31:30] 0 1 0 0 1 0/1 [22]=L ...
        // bit[29] is 0 for GPR STP/LDP (it's part of the fixed pattern).
        // The old code checked bit[29]=1 which excluded all GPR STP/LDP,
        // causing 32-bit STP W to be silently dropped.
        // Now: check bits[27:23] = 00100 (signed offset) or 00110 (pre-index).
        uint8_t mode_check = (inst >> 23) & 7;
        if (mode_check != 2 && mode_check != 3) return false;
        // Also check that bit[26]=0 (GPR, not SIMD/FP).
        if ((inst >> 26) & 1) return false;
        uint8_t opc = (inst >> 30) & 3;
        d.is_vec    = false;
        d.is_load   = (inst >> 22) & 1;
        d.mode      = mode_check;
        d.writeback = (mode_check == 3);
        int16_t imm7 = static_cast<int16_t>(arm64emu::sign_extend((inst >> 15) & 0x7F, 7));
        int esize = (opc == 2) ? 8 : 4;
        d.disp = imm7 * esize;
        d.cls = d.is_load ? InstClass::LDP : InstClass::STP;
        return true;
    }

    // Data processing — register: logical shifted register.
    case 0x0A: {
        uint8_t opc = (inst >> 29) & 3;
        d.set_flags  = (opc == 3);
        d.shift_type = (inst >> 22) & 3;
        d.N          = (inst >> 21) & 1;
        d.shift      = (inst >> 10) & 0x3F;
        d.rm         = (inst >> 16) & 0x1F;
        d.rn         = (inst >> 5) & 0x1F;
        d.rd         = inst & 0x1F;
        d.writes_sp  = (d.rd == 31 && opc == 1);
        switch (opc) {
            case 0: d.cls = InstClass::AND_REG;  break;
            case 1: d.cls = InstClass::ORR_REG;  break;
            case 2: d.cls = InstClass::EOR_REG;  break;
            case 3: d.cls = InstClass::ANDS_REG; break;
        }
        return true;
    }

    // Data processing — register: add/subtract (shifted or extended).
    case 0x0B: {
        bool S      = (inst >> 29) & 1;
        bool bit21  = (inst >> 21) & 1;
        // For extended register (bit21=1), bits[23:22] must be 00.
        if (bit21 && (((inst >> 22) & 3) != 0)) return false;

        d.is_sub    = (inst >> 30) & 1;
        d.set_flags = S;
        d.rm        = (inst >> 16) & 0x1F;
        d.rn        = (inst >> 5) & 0x1F;
        d.rd        = inst & 0x1F;

        if (bit21) {
            d.extend    = (inst >> 13) & 7;
            d.shift     = (inst >> 10) & 7;
            d.reads_sp  = (d.rn == 31);
            d.writes_sp = (d.rd == 31 && !S);
        } else {
            d.shift_type = (inst >> 22) & 3;
            d.shift      = (inst >> 10) & 0x3F;
        }

        if (d.is_sub) d.cls = S ? InstClass::SUBS_REG : InstClass::SUB_REG;
        else          d.cls = S ? InstClass::ADDS_REG : InstClass::ADD_REG;
        return true;
    }

    // SIMD LD1/ST1 OR Load/Store Pair (V=1, all modes).
    case 0x0C: case 0x0D: {
        if (!((inst >> 29) & 1)) {
            // SIMD LD1/ST1 — bit 31 must be 0.
            if ((inst >> 31) & 1) return false;
            d.is_vec  = true;
            d.is_load = (inst >> 22) & 1;
            d.size    = (inst >> 10) & 3;
            d.rt      = inst & 0x1F;
            d.rn      = (inst >> 5) & 0x1F;
            d.rm      = (inst >> 16) & 0x1F;
            // BUGFIX (Turn 60, H11): distinguish single-structure from
            // multi-structure LD1/ST1.
            //   - Multi-structure (bit[12]=0): bits[14:13] = register count
            //     (00=1, 01=2, 10=3, 11=4 regs). LD1 {Vt.16B}, {Vt.4S, Vt2.4S}, etc.
            //   - Single-structure (bit[12]=1): bits[14:13] = element index,
            //     and bits[12:10] encode (index, size) per the ARM ARM.
            //     LD1 {Vt.S}[idx], LD1 {Vt.H}[idx], etc. — used for matrix
            //     transpose, RGBA channel interleaving, etc.
            // The old code unconditionally set simd_count from bits[14:13]
            // for BOTH variants, so a single-structure LD1 {V0.S}[2]
            // (bits[14:13]=10) was misdecoded as a 3-register multi-structure
            // LD1, reading/writing 48 bytes instead of 4. Real games using
            // single-structure LD1/ST1 (matrix ops, color conversion) would
            // silently corrupt memory.
            //
            // We set is_single_struct so the interpreter can dispatch to
            // the single-element path. simd_count stays 1 for single-struct
            // (one register, one element).
            // BUGFIX (Turn 75): the old code used bit[12] alone to distinguish
            // single-structure from multi-structure. But LD1/ST1 multi (1 reg)
            // has opcode 0b0111 which has bit[12]=1 — same as single-structure.
            // The correct check: if bits[11:10]==00, there's no element index,
            // so it's multi-structure. If bits[11:10]!=00, it's single-structure.
            // This matches the ARM ARM: multi-structure has no index field,
            // single-structure encodes the element index in bits[11:10].
            bool is_single_struct = ((inst >> 12) & 1) && (((inst >> 10) & 3) != 0);
            if (is_single_struct) {
                // Single-structure LD1/ST1.
                // The element index and size are encoded across bits[14:10].
                // Per the ARM ARM (C4.1.66):
                //   size[14:13] (for LD1/ST1 1-element), index[12] (high bit),
                //   size[11:10] (low bits). The full decode is:
                //     Q=0 (64-bit): idx = (size[0] << 1) | index[1]; size = size[2:1]
                //     Q=1 (128-bit): idx = size[0]; index[1:0]; size = size[2:1]
                //   For simplicity we store the raw bits and let the interpreter
                //   do the final decode (the encoding is complex and varies by
                //   variant — LD1 vs LD2 vs LD3 vs LD4).
                d.is_single_struct = true;
                d.simd_count = 1;
                d.simd_index = ((inst >> 13) & 3);  // raw bits[14:13] as index
                d.Q = (inst >> 30) & 1;
            } else {
                // Multi-structure LD1/ST1.
                // Register count: 00=1, 01=2, 10=3, 11=4 regs.
                d.is_single_struct = false;
                d.simd_count = ((inst >> 13) & 3) + 1;
                d.Q = (inst >> 30) & 1;  // BUGFIX: Q was missing for multi-struct!
            }
            d.cls = d.is_load ? InstClass::SIMD_LD1 : InstClass::SIMD_ST1;
            return true;
        }
        uint8_t mode_check = (inst >> 23) & 7;
        if (mode_check != 1 && mode_check != 2 && mode_check != 3) return false;
        uint8_t opc = (inst >> 30) & 3;
        // bit[26] = V: 1 = SIMD/FP, 0 = GPR.
        // GPR LDP/STP (opc=0 → 32-bit, opc=2 → 64-bit) must have is_vec=false
        // so the interpreter uses the GPR path, not the vector path.
        // Vector LDP/STP (opc=0 → S, opc=1 → D, opc=2 → Q) have V=1.
        bool is_v = (inst >> 26) & 1;
        d.is_vec    = is_v;
        d.is_load   = is_v ? ((inst >> 22) & 1) : ((inst >> 22) & 1);
        d.mode      = mode_check;
        d.writeback = (mode_check == 1 || mode_check == 3);
        int16_t imm7 = static_cast<int16_t>(arm64emu::sign_extend((inst >> 15) & 0x7F, 7));
        int esize;
        if (is_v) {
            esize = (opc == 0) ? 4 : (opc == 1) ? 8 : 16;
        } else {
            esize = (opc == 2) ? 8 : 4;
        }
        d.disp = imm7 * esize;
        d.cls = d.is_load ? InstClass::LDP : InstClass::STP;
        return true;
    }

    // SIMD data processing (catch-all). v0's mask doesn't cover bit 24
    // but requires bit 31 = 0.
    //
    // Turn 41: added 0x2E, 0x4E, 0x6E, 0x4F to cover Q=1 and U=1 variants.
    // The AArch64 SIMD encoding uses bits[31:29] to distinguish:
    //   0x0E = Q=0, U=0  (e.g., ADD v.8b)
    //   0x2E = Q=0, U=1  (e.g., SUB v.8b — unsigned sub is 0x2E208400)
    //   0x4E = Q=1, U=0  (e.g., ADD v.16b — 128-bit variant)
    //   0x6E = Q=1, U=1  (e.g., SUB v.16b — 128-bit unsigned sub)
    //   0x0F, 0x4F = additional SIMD variants
    // Without these, Q=1 (128-bit) and U=1 (unsigned/modified) SIMD
    // instructions were rejected as "decode error", breaking real-world
    // programs that use 128-bit NEON operations.
    case 0x0E: case 0x0F: case 0x2E:
    case 0x4E: case 0x6E: case 0x4F: {
        if ((inst >> 31) & 1) return false;
        d.is_vec = true;
        d.cls    = InstClass::SIMD_DP;
        d.Q      = (inst >> 30) & 1;
        d.size   = (inst >> 22) & 3;
        d.rd     = inst & 0x1F;
        d.rn     = (inst >> 5) & 0x1F;
        d.rm     = (inst >> 16) & 0x1F;
        d.cmode  = (inst >> 12) & 0xF;
        return true;
    }

    // PC-relative addressing: ADR / ADRP.
    case 0x10: {
        bool is_adrp = (inst >> 31) & 1;
        d.cls = is_adrp ? InstClass::ADRP : InstClass::ADR;
        uint64_t immlo = (inst >> 29) & 3;
        uint64_t immhi = (inst >> 5) & 0x7FFFF;
        d.imm_u = (immhi << 2) | immlo;
        if (is_adrp) d.imm = arm64emu::sign_extend(d.imm_u << 12, 33);
        else         d.imm = arm64emu::sign_extend(d.imm_u, 21);
        d.rd = inst & 0x1F;
        return true;
    }

    // Add/subtract (immediate).
    case 0x11: {
        bool S      = (inst >> 29) & 1;
        d.is_sub    = (inst >> 30) & 1;
        d.set_flags = S;
        d.imm_u     = (inst >> 10) & 0xFFF;
        bool sh     = (inst >> 22) & 1;
        d.shift     = sh ? 12 : 0;
        d.rn        = (inst >> 5) & 0x1F;
        d.rd        = inst & 0x1F;
        d.reads_sp  = (d.rn == 31 && !S);
        d.writes_sp = (d.rd == 31 && !S);
        if (d.is_sub) d.cls = S ? InstClass::SUBS_IMM : InstClass::SUB_IMM;
        else          d.cls = S ? InstClass::ADDS_IMM : InstClass::ADD_IMM;
        return true;
    }

    // Wide immediate (MOVN/MOVZ/MOVK) OR Logical immediate.
    case 0x12: {
        if ((inst >> 23) & 1) {
            // MOVN/MOVZ/MOVK
            uint8_t opc = (inst >> 29) & 3;
            switch (opc) {
                case 0: d.cls = InstClass::MOVN; break;
                case 2: d.cls = InstClass::MOVZ; break;
                case 3: d.cls = InstClass::MOVK; break;
                default: d.cls = InstClass::UNKNOWN; return false;
            }
            d.imm16 = (inst >> 5) & 0xFFFF;
            d.hw    = (inst >> 21) & 3;
            d.shift = d.hw * 16;
            d.rd    = inst & 0x1F;
            return true;
        }
        // Logical immediate.
        uint8_t opc = (inst >> 29) & 3;
        d.N          = (inst >> 22) & 1;
        d.immr       = (inst >> 16) & 0x3F;
        d.imms       = (inst >> 10) & 0x3F;
        d.rn         = (inst >> 5) & 0x1F;
        d.rd         = inst & 0x1F;
        d.set_flags  = (opc == 3);
        d.writes_sp  = (d.rd == 31 && opc == 1);
        // BUGFIX (Turn 60, H10): if decode_bitmask_imm returns false, the
        // encoding is UNALLOCATED — return UNKNOWN instead of treating it
        // as a valid immediate of 0.
        if (!decode_bitmask_imm(d.N, d.immr, d.imms, d.sf, &d.imm_u)) {
            d.cls = InstClass::UNKNOWN;
            return false;
        }
        switch (opc) {
            case 0: d.cls = InstClass::AND_IMM;  break;
            case 1: d.cls = InstClass::ORR_IMM;  break;
            case 2: d.cls = InstClass::EOR_IMM;  break;
            case 3: d.cls = InstClass::ANDS_IMM; break;
        }
        return true;
    }

    // Bitfield (SBFM/BFM/UBFM) OR EXTR.
    // IMPROVEMENT: v0 had a bug where the bitfield check (mask 0x1F000000)
    // ignored bit 23 and came BEFORE the EXTR check (mask 0x1F800000),
    // making EXTR unreachable. This routes on bit 23 first, so EXTR
    // is correctly decoded.
    case 0x13: {
        d.N    = (inst >> 22) & 1;
        d.immr = (inst >> 16) & 0x3F;
        d.imms = (inst >> 10) & 0x3F;
        d.rn   = (inst >> 5) & 0x1F;
        d.rd   = inst & 0x1F;

        if ((inst >> 23) & 1) {
            // EXTR. Rm shares bits[20:16] with immr.
            d.rm  = (inst >> 16) & 0x1F;
            d.cls = InstClass::EXTR;
            return true;
        }
        uint8_t opc = (inst >> 29) & 3;
        switch (opc) {
            case 0: d.cls = InstClass::SBFM; break;
            case 1: d.cls = InstClass::BFM;  break;
            case 2: d.cls = InstClass::UBFM; break;
            default: d.cls = InstClass::UNKNOWN; return false;
        }
        return true;
    }

    // B.cond / CBZ (32+64) / SVC / BRK / HLT / HVC / SMC.
    case 0x14: {
        uint8_t op31_29 = (inst >> 29) & 7;
        switch (op31_29) {
            case 0b001: case 0b101: {  // CBZ (32-bit sf=0 / 64-bit sf=1)
                d.cls  = InstClass::CBZ;
                d.imm  = arm64emu::sign_extend((inst >> 5) & 0x7FFFF, 19) << 2;
                d.rt   = inst & 0x1F;
                return true;
            }
            case 0b010: {  // B.cond
                if ((inst & 0xFF000010) != 0x54000000) return false;
                d.cls  = InstClass::Bcond;
                d.cond = inst & 0xF;
                d.imm  = arm64emu::sign_extend((inst >> 5) & 0x7FFFF, 19) << 2;
                return true;
            }
            case 0b110: {  // System calls
                uint8_t sys_op = (inst >> 21) & 7;
                uint8_t sys_low5 = inst & 0x1F;
                switch (sys_op) {
                    case 0b000:
                        switch (sys_low5) {
                            case 0b00001:
                                d.cls   = InstClass::SVC_IMM;
                                d.imm_u = (inst >> 5) & 0xFFFF;
                                return true;
                            case 0b00010:
                                d.cls = InstClass::HVC_IMM;
                                return true;
                            case 0b00011:
                                d.cls = InstClass::SMC_IMM;
                                return true;
                            default:
                                d.cls = InstClass::UNKNOWN;
                                return false;
                        }
                    case 0b001:  // BRK — bits[4:0] must be 00000
                        if (sys_low5 != 0) { d.cls = InstClass::UNKNOWN; return false; }
                        d.cls   = InstClass::BRK_IMM;
                        d.imm_u = (inst >> 5) & 0xFFFF;
                        return true;
                    case 0b010:  // HLT — bits[4:0] must be 00000
                        if (sys_low5 != 0) { d.cls = InstClass::UNKNOWN; return false; }
                        d.cls   = InstClass::HLT_IMM;
                        d.imm_u = (inst >> 5) & 0xFFFF;
                        return true;
                    default:
                        d.cls = InstClass::UNKNOWN;
                        return false;
                }
            }
            default:
                return false;
        }
    }

    // CBNZ (32+64) / MSR / MRS / HINT / CLREX.
    case 0x15: {
        uint8_t op31_29 = (inst >> 29) & 7;
        switch (op31_29) {
            case 0b001: case 0b101: {  // CBNZ
                d.cls = InstClass::CBNZ;
                d.imm = arm64emu::sign_extend((inst >> 5) & 0x7FFFF, 19) << 2;
                d.rt  = inst & 0x1F;
                return true;
            }
            case 0b110: {  // System register access / hints
                if (inst == 0xD503305F) {
                    d.cls = InstClass::CLREX_INST;
                    return true;
                }
                if ((inst & 0xFFFFF000) == 0xD5033000) {
                    d.cls = InstClass::HINT;
                    return true;
                }
                if ((inst & 0xFF000000) == 0xD5000000) {
                    d.sys_L    = (inst >> 21) & 1;
                    d.sys_op0  = (inst >> 19) & 0x3;
                    d.sys_op1  = (inst >> 16) & 0x7;
                    d.sys_crn  = (inst >> 12) & 0xF;
                    d.sys_crm  = (inst >> 8) & 0xF;
                    d.sys_op2  = (inst >> 5) & 0x7;
                    d.rt       = inst & 0x1F;
                    d.cls = d.sys_L ? InstClass::MRS_SYS : InstClass::MSR_SYS;
                    return true;
                }
                return false;
            }
            default:
                return false;
        }
    }

    // TBZ (32+64) / BR / BLR / RET.
    case 0x16: {
        uint8_t op31_29 = (inst >> 29) & 7;
        switch (op31_29) {
            case 0b001: case 0b101: {  // TBZ
                d.cls   = InstClass::TBZ;
                d.imm   = arm64emu::sign_extend((inst >> 5) & 0x3FFF, 14) << 2;
                uint8_t b40 = (inst >> 19) & 0x1F;
                uint8_t b5  = (inst >> 31) & 1;
                d.imm_u = (b5 << 5) | b40;
                d.rt    = inst & 0x1F;
                return true;
            }
            case 0b110: {  // BR/BLR/RET
                if ((inst & 0xFFFFFC00) == 0xD61F0000) {
                    d.cls = InstClass::BR;
                    d.rn  = (inst >> 5) & 0x1F;
                    return true;
                }
                if ((inst & 0xFFFFFC00) == 0xD63F0000) {
                    d.cls = InstClass::BLR;
                    d.rn  = (inst >> 5) & 0x1F;
                    return true;
                }
                if ((inst & 0xFFFFFC1F) == 0xD65F0000) {
                    d.cls = InstClass::RET;
                    d.rn  = (inst >> 5) & 0x1F;
                    return true;
                }
                return false;
            }
            default:
                return false;
        }
    }

    // TBNZ (32+64).
    case 0x17: {
        uint8_t op31_29 = (inst >> 29) & 7;
        if (op31_29 != 0b001 && op31_29 != 0b101) return false;
        d.cls   = InstClass::TBNZ;
        d.imm   = arm64emu::sign_extend((inst >> 5) & 0x3FFF, 14) << 2;
        uint8_t b40 = (inst >> 19) & 0x1F;
        uint8_t b5  = (inst >> 31) & 1;
        d.imm_u = (b5 << 5) | b40;
        d.rt    = inst & 0x1F;
        return true;
    }

    // Load/Store (various) — bits[28:24] ∈ {11000, 11100}:
    //   LSE atomics / LDUR/STUR / LDR/STR reg offset.
    // v0's masks don't cover bit 26 (V), so both V=0 (case 0x18) and
    // V=1 (case 0x1C) variants route here. All v0 masks require
    // bit 29 = 1.
    case 0x18: case 0x1C: {
        if (!((inst >> 29) & 1)) return false;

        bool    bit21  = (inst >> 21) & 1;
        bool    bit26  = (inst >> 26) & 1;  // V
        uint8_t mode_b = (inst >> 10) & 3;

        if (!bit21) {
            // LDUR/STUR. mode_b ∈ {0,1,3}; mode_b == 2 is reg offset.
            if (mode_b == 2) return false;
            d.size      = (inst >> 30) & 3;
            d.opc_ls    = (inst >> 22) & 3;
            d.is_vec    = (inst >> 26) & 1;
            int16_t imm9 = static_cast<int16_t>(arm64emu::sign_extend((inst >> 12) & 0x1FF, 9));
            d.rn        = (inst >> 5) & 0x1F;
            d.rt        = inst & 0x1F;
            d.writeback = (mode_b == 1 || mode_b == 3);
            d.mode      = (mode_b == 1) ? 1 : (mode_b == 3) ? 2 : 0;
            d.disp      = imm9;
            d.is_load   = d.is_vec ? (d.opc_ls & 1)
                                   : ((d.opc_ls & 2) || (d.opc_ls & 1));
            d.cls = d.is_load ? InstClass::LDR_UNS : InstClass::STR_UNS;
            return true;
        }
        switch (mode_b) {
            case 0b00: {  // LSE atomics — only valid when V=0
                if (bit26) return false;
                d.size    = (inst >> 30) & 3;
                d.acquire = (inst >> 23) & 1;
                d.is_load = (inst >> 22) & 1;
                d.rs      = (inst >> 16) & 0x1F;
                d.atom_op = (inst >> 12) & 0xF;
                d.rn      = (inst >> 5) & 0x1F;
                d.rt      = inst & 0x1F;
                d.cls     = InstClass::LSE_ATOMIC;
                return true;
            }
            case 0b10: {  // LDR/STR register offset
                d.size    = (inst >> 30) & 3;
                d.opc_ls  = (inst >> 22) & 3;
                d.is_vec  = (inst >> 26) & 1;
                d.rm      = (inst >> 16) & 0x1F;
                d.extend  = (inst >> 13) & 7;
                d.shift   = (inst >> 12) & 1;
                d.rn      = (inst >> 5) & 0x1F;
                d.rt      = inst & 0x1F;
                d.is_load = d.is_vec ? (d.opc_ls & 1)
                                     : ((d.opc_ls & 2) || (d.opc_ls & 1));
                d.cls = d.is_load ? InstClass::LDR_REG : InstClass::STR_REG;
                return true;
            }
            default:
                d.cls = InstClass::UNKNOWN;
                return false;
        }
    }

    // Load/Store (unsigned immediate offset) — bits[28:24] ∈ {11001, 11101}.
    // v0's mask doesn't cover bit 26 (V), requires bit 29 = 1.
    case 0x19: case 0x1D: {
        if (!((inst >> 29) & 1)) return false;
        d.size    = (inst >> 30) & 3;
        d.opc_ls  = (inst >> 22) & 3;
        d.is_vec  = (inst >> 26) & 1;
        uint16_t imm12 = (inst >> 10) & 0xFFF;
        d.rn      = (inst >> 5) & 0x1F;
        d.rt      = inst & 0x1F;
        // Address offset = imm12 << scale.
        //   Non-SIMD: scale = size (1/2/4/8 bytes per element).
        //   SIMD&FP:  scale = Q ? 4 : 3 (Q-form=16B, D-form=8B).
        //
        // The old code used `is_q = (opc_ls & 2) && size == 0` which
        // incorrectly matched LDRSB (size=0, opc=11) and LDRSB got
        // scale=4 instead of 0, multiplying the offset by 16. This
        // caused `ldrsb w0, [x0, #176]` to access [x0+2816] and crash
        // toybox ls / with UnmappedMemory.
        uint64_t scale;
        if (d.is_vec) {
            scale = (d.size & 2) ? 4 : 3;   // bit 30 = Q
        } else {
            scale = d.size;
        }
        d.disp    = static_cast<int64_t>(imm12 << scale);
        d.is_load = d.is_vec ? (d.opc_ls & 1)
                             : ((d.opc_ls & 2) || (d.opc_ls & 1));
        d.cls = d.is_load ? InstClass::LDR_IMM : InstClass::STR_IMM;
        return true;
    }

    // Data processing — register (5 sub-groups share bits[28:24]=11010):
    //   ADC/SBC, Cond compare, Cond select, DP 1-source, DP 2-source.
    // All sub-patterns require bit 21 = 0.
    case 0x1A: {
        bool    bit30     = (inst >> 30) & 1;
        bool    bit29     = (inst >> 29) & 1;
        bool    bit21     = (inst >> 21) & 1;
        uint8_t bits23_22 = (inst >> 22) & 3;

        if (bit21) return false;

        switch (bits23_22) {
            case 0b00: {
                // ADC/ADCS/SBC/SBCS.
                bool S = (inst >> 29) & 1;
                d.is_sub    = (inst >> 30) & 1;
                d.set_flags = S;
                d.rm        = (inst >> 16) & 0x1F;
                d.rn        = (inst >> 5) & 0x1F;
                d.rd        = inst & 0x1F;
                if (d.is_sub) d.cls = S ? InstClass::SBCS_REG : InstClass::SBC_REG;
                else          d.cls = S ? InstClass::ADCS_REG : InstClass::ADC_REG;
                return true;
            }
            case 0b01: {
                // Cond compare (CCMP/CCMN). Requires bit 29 = 1.
                if (!bit29) return false;
                d.is_sub      = (inst >> 30) & 1;
                // Register vs immediate form is distinguished by bit 11:
                //   bit 11 = 0 → register form (operand is Rm)
                //   bit 11 = 1 → immediate form (operand is imm5)
                // v0 used bit 21 which is always 0 (the encoding group
                // requires it), so CCMP was always treated as immediate
                // form. This broke __eqtf2 which uses `ccmp x6, x7, #0, eq`
                // (register form) — the emulator compared x6 with #7
                // instead of x7, producing wrong flags and making
                // printf("%f") hang forever.
                d.is_register = !((inst >> 11) & 1);
                d.rm          = (inst >> 16) & 0x1F;
                d.cond        = (inst >> 12) & 0xF;
                d.rn          = (inst >> 5) & 0x1F;
                d.nzcv_field  = inst & 0xF;
                // for the IMMEDIATE form, the 5-bit
                // immediate lives in bits[20:16] — the same bit position as
                // Rm in the register form. The IR translator reads d.imm_u
                // (not d.rm) when d.is_register == false, so we must populate
                // d.imm_u here. Without this, the IR translator passes an
                // uninitialized/zero immediate to CCMP, producing wrong flags
                // and corrupting word counts in toybox wc (5 vs 7 on a
                // 3-line input).
                if (!d.is_register) {
                    d.imm_u = (inst >> 16) & 0x1F;
                }
                d.cls = d.is_sub ? InstClass::CCMP : InstClass::CCMN;
                return true;
            }
            case 0b10: {
                // Cond select (CSEL/CSINC/CSINV/CSNEG)
                // Encoding uses BOTH bits[30:29] (opc) AND bits[11:10] (op2):
                //   opc=00, op2=00 → CSEL
                //   opc=00, op2=01 → CSINC
                //   opc=10, op2=00 → CSINV
                //   opc=10, op2=01 → CSNEG
                // v0 only checked bits[11:10], confusing CSINC with CSNEG
                // and CSINV with CSEL. This broke CNEG (alias for CSNEG
                // with inverted condition), which is used by __gttf2 and
                // __lttf2 to negate the return value — producing -1
                // instead of 1, breaking all long double comparisons.
                uint8_t opc = (inst >> 29) & 3;
                uint8_t op2 = (inst >> 10) & 3;
                d.rm   = (inst >> 16) & 0x1F;
                d.cond = (inst >> 12) & 0xF;
                d.rn   = (inst >> 5) & 0x1F;
                d.rd   = inst & 0x1F;
                if (opc == 0 && op2 == 0)      d.cls = InstClass::CSEL;
                else if (opc == 0 && op2 == 1) d.cls = InstClass::CSINC;
                else if (opc == 2 && op2 == 0) d.cls = InstClass::CSINV;
                else if (opc == 2 && op2 == 1) d.cls = InstClass::CSNEG;
                else return false;
                return true;
            }
            case 0b11: {
                // DP 1-source (bit 30=1) OR DP 2-source (bit 30=0)
                if (bit30) {
                    d.dp_opcode = (inst >> 10) & 0x3F;
                    d.rn        = (inst >> 5) & 0x1F;
                    d.rd        = inst & 0x1F;
                    switch (d.dp_opcode) {
                        case 0:  d.cls = InstClass::RBIT;  return true;
                        case 1:  d.cls = InstClass::REV16; return true;
                        // opcode=2: REV (32-bit) when sf=0, REV32 (64-bit) when sf=1.
                        // REV32 reverses bytes within each 32-bit word of a 64-bit reg.
                        case 2:  d.cls = d.sf ? InstClass::REV32 : InstClass::REV; return true;
                        // opcode=3: REV (64-bit only). UNALLOCATED when sf=0.
                        case 3:  d.cls = InstClass::REV;   return true;
                        case 4:  d.cls = InstClass::CLZ;   return true;
                        case 5:  d.cls = InstClass::CLS;   return true;
                        default: d.cls = InstClass::UNKNOWN; return false;
                    }
                }
                d.dp_opcode = (inst >> 10) & 0x3F;
                d.rm        = (inst >> 16) & 0x1F;
                d.rn        = (inst >> 5) & 0x1F;
                d.rd        = inst & 0x1F;
                switch (d.dp_opcode) {
                    case 2:  d.cls = InstClass::UDIV; return true;
                    case 3:  d.cls = InstClass::SDIV; return true;
                    case 8:  d.cls = InstClass::LSL;  return true;
                    case 9:  d.cls = InstClass::LSR;  return true;
                    case 10: d.cls = InstClass::ASR;  return true;
                    case 11: d.cls = InstClass::ROR;  return true;
                    // CRC32 instructions (opcodes 0x10-0x1F).
                    // Encoding: bits[20:16]=size(00=B,01=H,10=W,11=X),
                    // bit[21]=1 for CRC32C (castagnoli), 0 for CRC32.
                    // We use dp_opcode to carry the full opcode + size info.
                    case 0x10: d.cls = InstClass::CRC32;  d.is_sub = 0; d.imm = 0; return true; // CRC32B
                    case 0x11: d.cls = InstClass::CRC32;  d.is_sub = 0; d.imm = 1; return true; // CRC32H
                    case 0x12: d.cls = InstClass::CRC32;  d.is_sub = 0; d.imm = 2; return true; // CRC32W
                    case 0x14: d.cls = InstClass::CRC32;  d.is_sub = 0; d.imm = 3; return true; // CRC32X
                    case 0x18: d.cls = InstClass::CRC32;  d.is_sub = 1; d.imm = 0; return true; // CRC32CB
                    case 0x19: d.cls = InstClass::CRC32;  d.is_sub = 1; d.imm = 1; return true; // CRC32CH
                    case 0x1A: d.cls = InstClass::CRC32;  d.is_sub = 1; d.imm = 2; return true; // CRC32CW
                    case 0x1C: d.cls = InstClass::CRC32;  d.is_sub = 1; d.imm = 3; return true; // CRC32CX
                    default: d.cls = InstClass::UNKNOWN; return false;
                }
            }
        }
        return false;
    }

    // Data processing — register (3-source): MADD/MSUB/SMADDL/etc.
    case 0x1B: {
        d.sub_op = (inst >> 21) & 0x7;
        d.o0     = (inst >> 15) & 1;
        d.rm     = (inst >> 16) & 0x1F;
        d.ra     = (inst >> 10) & 0x1F;
        d.rn     = (inst >> 5) & 0x1F;
        d.rd     = inst & 0x1F;
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

    // FMOV (Vd.D[1] <-> Rn) + FP scalar (catch-all).
    case 0x1E: {
        if ((inst & 0xFFE0FC00) == 0x9EA00000) {
            bool to_fp = (inst >> 16) & 1;
            d.cls    = to_fp ? InstClass::FMOV_VD1 : InstClass::FMOV_RVD1;
            d.is_vec = true;
            d.rn     = (inst >> 5) & 0x1F;
            d.rd     = inst & 0x1F;
            return true;
        }
        uint8_t op31_24 = (inst >> 24) & 0xFF;
        // Accept scalar FP (0x1E), FMOV Vd.D[1] (0x9E), and vector FP
        // with Q=1 (0x5E). The 0x5E form covers instructions like
        // FCVTZS Vd.2D, Vn.2D (vector 2-lane 64-bit FP→int conversion)
        // used by toybox seq. The IR translator's FP_SCALAR case will
        // fall back to CALL_INTERP for vector-specific encodings it
        // doesn't handle natively.
        //
        // Turn 40: added 0x7E — 'Advanced SIMD three same, extra'
        // (SQRDMLAH/SQRDMLSH — saturating rounding multiply-add).
        // Used by libc's printf formatting code (NEON for wide-char
        // operations). Without this, `toybox uptime` crashes with
        // "decode error at pc=0x... inst=0x7e61d821". The instruction
        // falls through to CALL_INTERP which uses the interpreter.
        if (op31_24 != 0x1E && op31_24 != 0x9E &&
            op31_24 != 0x5E && op31_24 != 0x7E) return false;
        d.is_vec    = true;
        d.cls       = InstClass::FP_SCALAR;
        d.ftype     = (inst >> 22) & 3;
        d.rd        = inst & 0x1F;
        d.rn        = (inst >> 5) & 0x1F;
        d.rm        = (inst >> 16) & 0x1F;
        d.fp_opcode = (inst >> 12) & 0xF;
        d.rmode     = (inst >> 19) & 3;
        return true;
    }

    // FMADD / FMSUB / FNMADD / FNMSUB — FP fused multiply-add/subtract
    // (3-source). bits[28:24]=11111, bit[15]=o1 (0=FMADD/FNMADD, 1=FMSUB/FNMSUB),
    // bit[21]=o2 (0=FMADD/FMSUB, 1=FNMADD/FNMSUB). The IR translator's
    // FP_SCALAR case handles these via the (op & 0xFF200000) == 0x1F000000
    // check, so we just classify them as FP_SCALAR and let the translator
    // dispatch.
    case 0x1F: {
        d.is_vec    = true;
        d.cls       = InstClass::FP_SCALAR;
        d.ftype     = (inst >> 22) & 3;
        d.rd        = inst & 0x1F;
        d.rn        = (inst >> 5) & 0x1F;
        d.rm        = (inst >> 16) & 0x1F;
        d.fp_opcode = (inst >> 12) & 0xF;
        d.rmode     = (inst >> 19) & 3;
        return true;
    }

    }  // end outer switch

    d.cls = InstClass::UNKNOWN;
    return false;
}

} // namespace arm64emu
