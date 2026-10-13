// Decoder.hpp — Shared ARM64 instruction decode tables and helpers.
//
// This header is included by both the interpreter (Interpreter.cpp) and
// the JIT (FrostJIT.cpp). It defines:
//   - Instruction class enumerations
//   - The DecodedInst struct (fields extracted from a 32-bit ARM64 word)
//   - The decode() function that fills a DecodedInst from raw bits
//   - Shared helpers (condition codes, extend_reg, sign_extend, etc.)
//
// The interpreter calls decode() then dispatches on DecodedInst::cls.
// frostJIT calls decode() then emits x86_64 code based on
// DecodedInst::cls, falling back to the interpreter for unsupported
// instructions. Both share the same decode logic, so adding a new
// instruction in one place automatically makes it available to both.
//
// Design principle: decode() does NO execution and NO memory access.
// It only extracts fields. Execution is the interpreter's job;
// code emission is the JIT's job. This keeps decode() pure and
// reusable.
#pragma once
#include <cstdint>
#include <cstddef>
namespace arm64emu {
struct CPU;  // forward declaration
// ── Instruction classes ─────────────────────────────────────────────────
// Top-level decode categories. Each corresponds to a major encoding
// group in the ARM Architecture Reference Manual.
enum class InstClass : uint16_t {
    UNKNOWN = 0,
    // Branches
    B,              // unconditional branch (immediate)
    BL,             // branch with link (immediate)
    Bcond,          // conditional branch (immediate)
    BR,             // branch to register
    BLR,            // branch with link to register
    RET,            // return from subroutine
    CBZ,            // compare and branch if zero
    CBNZ,           // compare and branch if nonzero
    TBZ,            // test bit and branch if zero
    TBNZ,           // test bit and branch if nonzero
    // System
    SVC,            // supervisor call (syscall)
    BRK,            // breakpoint (fatal in our emulator)
    HLT,            // halt (used as exit in baremetal)
    MSR,            // move to system register
    MRS,            // move from system register
    CLREX,          // clear exclusive monitor
    BARRIER,        // DSB/DMB/ISB/NOP/YIELD (hint space)
    // Data processing - immediate
    ADR,            // compute address (PC-relative)
    ADRP,           // compute page address (PC-relative)
    MOVN,           // move inverted immediate
    MOVZ,           // move zero-extended immediate
    MOVK,           // move keep (insert immediate)
    ADD_IMM,        // add immediate
    SUB_IMM,        // subtract immediate
    ADDS_IMM,       // add immediate setting flags
    SUBS_IMM,       // subtract immediate setting flags
    AND_IMM,        // AND immediate
    ORR_IMM,        // OR immediate
    EOR_IMM,        // EOR immediate
    ANDS_IMM,       // AND immediate setting flags
    SBFM,           // signed bitfield move
    BFM,            // bitfield move
    UBFM,           // unsigned bitfield move
    EXTR,           // extract register
    // Data processing - register
    ADD_REG,        // add (shifted register)
    SUB_REG,        // subtract (shifted register)
    ADDS_REG,       // add setting flags (shifted register)
    SUBS_REG,       // subtract setting flags (shifted register)
    ADC_REG,        // add with carry
    ADCS_REG,       // add with carry setting flags
    SBC_REG,        // subtract with carry
    SBCS_REG,       // subtract with carry setting flags
    AND_REG,        // AND (shifted register)
    ORR_REG,        // OR (shifted register)
    EOR_REG,        // EOR (shifted register)
    ANDS_REG,       // AND setting flags (shifted register)
    MADD,           // multiply-accumulate (sub_op=0, o0=0)
    MSUB,           // multiply-subtract (sub_op=0, o0=1)
    SMADDL,         // signed multiply-add long (sub_op=1, o0=0)
    SMSUBL,         // signed multiply-sub long (sub_op=1, o0=1)
    UMADDL,         // unsigned multiply-add long (sub_op=5, o0=0)
    UMSUBL,         // unsigned multiply-sub long (sub_op=5, o0=1)
    SMULH,          // signed multiply high (sub_op=7)
    UMULH,          // unsigned multiply high (sub_op=6)
    UDIV,           // unsigned divide
    SDIV,           // signed divide
    LSL,            // logical shift left (alias of LSLV)
    LSR,            // logical shift right (alias of LSRV)
    ASR,            // arithmetic shift right (alias of ASRV)
    ROR,            // rotate right (alias of RORV)
    RBIT,           // reverse bits
    REV16,          // reverse bytes in 16-bit halfwords
    REV32,          // reverse bytes in 32-bit words
    REV,            // reverse bytes (64-bit)
    CLZ,            // count leading zeros
    CLS,            // count leading sign bits
    CRC32,          // CRC32/CRC32C (B/H/W/X variants)
    CSEL,           // conditional select
    CSINC,          // conditional select increment
    CSINV,          // conditional select invert
    CSNEG,          // conditional select negate
    CCMP,           // conditional compare
    CCMN,           // conditional compare negative
    // Load/Store
    LDR_IMM,        // load (unsigned immediate offset)
    STR_IMM,        // store (unsigned immediate offset)
    LDR_UNS,        // load (unscaled, LDUR)
    STR_UNS,        // store (unscaled, STUR)
    LDR_REG,        // load (register offset)
    STR_REG,        // store (register offset)
    LDP,            // load pair
    STP,            // store pair
    LDRSW,          // load signed word (sign-extend to 64)
    LDRSB,          // load signed byte
    LDRSH,          // load signed halfword
    // Atomics
    LDXR,           // load exclusive
    STXR,           // store exclusive
    LDAXR,          // load acquire exclusive
    STLXR,          // store release exclusive
    STLR,           // store release
    LDAR,           // load acquire
    LSE_ATOMIC,     // LSE atomic (LDADD/LDCLR/LDEOR/LDSET/SMAX/SMIN/UMAX/UMIN/SWP/CAS — sub-dispatched on atom_op)
    // SIMD/FP (subset)
    SIMD_LD1,       // vector load single structure
    SIMD_ST1,       // vector store single structure
    SIMD_LOGICAL,   // vector AND/ORR/EOR/BIC
    SIMD_DUP,       // vector duplicate
    FMOV_VD1,       // FMOV Vd.D[1], Rn (move GPR to high 64 bits)
    FMOV_RVD1,      // FMOV Rn, Vm.D[1] (move high 64 bits to GPR)
    // FP arithmetic (FADD/FSUB/FMUL/FDIV/FMAX/FMIN/FNMUL/FMADD/FMSUB/FABS/
    // FNEG/FSQRT/FCMP/FCMPE/FCVT/FCVTZS/FCVTZU/SCVTF/UCVTF/FRINT/FCSEL/
    // FMOV/FMOV_IMM) are all dispatched via FP_SCALAR in both the
    // interpreter and IR translator — the decoder never emits them as
    // distinct InstClass values. The enum entries were removed to avoid
    // dead-code accumulation.
    // System
    SVC_IMM,        // SVC #imm
    HVC_IMM,        // HVC #imm (error in user mode)
    SMC_IMM,        // SMC #imm (error in user mode)
    BRK_IMM,        // BRK #imm
    HLT_IMM,        // HLT #imm
    MSR_SYS,        // MSR <sysreg>, Xt
    MRS_SYS,        // MRS Xt, <sysreg>
    HINT,           // NOP/WFE/WFI/SEV/YIELD and DMB/DSB/ISB
    // BARRIER already declared above
    CLREX_INST,     // CLREX
    SYS_NOP,        // Other system instructions (NOP)
    // SIMD data processing (sub-dispatched within the case)
    SIMD_DP,        // Generic SIMD data processing (sub-dispatch by raw bits)
    FP_SCALAR,      // Generic FP scalar (sub-dispatch by raw bits)
};
// DMB/DSB share the hint decode class, but have architectural memory effects.
// Preserve every domain/type option; a full host fence may be stronger.
constexpr bool is_data_memory_barrier(uint32_t raw) {
    return (raw & 0xFFFFF0FFu) == 0xD50330BFu ||
           (raw & 0xFFFFF0FFu) == 0xD503309Fu;
}
// ── Decoded instruction ─────────────────────────────────────────────────
// All fields extracted from a 32-bit ARM64 instruction word. The decoder
// fills these; the interpreter reads them. The interpreter NEVER does bit
// extraction — it only reads d.* fields and executes.
struct DecodedInst {
    InstClass cls = InstClass::UNKNOWN;
    uint32_t  raw = 0;          // the original 32-bit word
    // ── Common register fields ──
    uint8_t  rd  = 0;           // destination register
    uint8_t  rn  = 0;           // first source register (often base)
    uint8_t  rm  = 0;           // second source register
    uint8_t  ra  = 0;           // third source register (MADD/MSUB)
    uint8_t  rt  = 0;           // transfer register (load/store/atomic)
    uint8_t  rt2 = 0;           // second transfer register (pair)
    uint8_t  rs  = 0;           // status register (STXR) / source (atomic)
    uint8_t  cond = 0;          // condition code (0-15)
    // ── Width / size ──
    uint8_t  size = 0;          // 0=8bit, 1=16bit, 2=32bit, 3=64bit
    bool     sf = false;        // 64-bit (vs 32-bit) for data processing
    bool     pc_relative = false; // literal load: base is this instruction PC
    bool     is_load = false;   // true for LDR/LDP, false for STR/STP
    bool     is_vec = false;    // true for SIMD/FP, false for GP
    bool     set_flags = false; // true for ADDS/SUBS/ANDS
    // ── SP / XZR disambiguation ──
    // For ADD/SUB immediate: Rn==31 reads SP (not XZR) when !set_flags.
    // Rd==31 writes SP (not XZR) when !set_flags.
    bool     reads_sp = false;  // source Rn is SP (not XZR)
    bool     writes_sp = false; // destination Rd is SP (not XZR)
    // ── Immediates ──
    int64_t  imm = 0;           // signed immediate (various forms)
    uint64_t imm_u = 0;         // unsigned immediate
    uint16_t imm16 = 0;         // 16-bit immediate (MOVZ/MOVK/etc.)
    uint8_t  shift = 0;         // shift amount (already computed)
    uint8_t  shift_type = 0;    // 0=LSL, 1=LSR, 2=ASR, 3=ROR
    uint8_t  extend = 0;        // extend type for extended register
    uint8_t  hw = 0;            // half-word selector (MOVZ/MOVK: shift = hw*16)
    // ── Bitfield (SBFM/BFM/UBFM/EXTR) ──
    uint8_t  immr = 0;          // bitfield rotate amount
    uint8_t  imms = 0;          // bitfield width selector
    bool     N = false;         // bit 22 (bitfield / logical imm)
    // ── Logical immediate ──
    // The bitmask is pre-decoded by the decoder into `imm_u` (64-bit value).
    // The interpreter just uses it directly.
    // ── Memory addressing ──
    int64_t  disp = 0;          // displacement (already computed, includes scale)
    uint8_t  mode = 0;          // 0=offset, 1=post-index, 2=pre-index
    bool     writeback = false; // true if base register is written back
    uint8_t  opc_ls = 0;        // load/store opc field (0=STR,1=LDR,2=STRQ/LDRSW,3=LDRQ/LDR)
    // ── Atomics ──
    uint8_t  atom_op = 0;       // LSE atomic opcode (0-15)
    bool     acquire = false;   // acquire-release hint
    uint8_t  excl_low6 = 0;     // exclusive load/store low 6 bits
    // ── Data processing (1-source, 2-source, 3-source) ──
    uint8_t  dp_opcode = 0;     // sub-opcode for 1/2-source data proc
    bool     o0 = false;        // bit 15 (MADD vs MSUB, etc.)
    uint8_t  sub_op = 0;        // bits 23:21 for 3-source data proc (0=MADD/MSUB, 1=SMADDL/SMSUBL, 5=UMADDL/UMSUBL, 6=UMULH, 7=SMULH)
    // ── Conditional compare (CCMP/CCMN) ──
    uint8_t  nzcv_field = 0;    // NZCV to set if condition is false
    bool     is_register = false; // CCMP reg vs imm
    // ── SIMD / FP ──
    bool     Q = false;         // 128-bit (Q form) vs 64-bit (D form)
    uint8_t  ftype = 0;         // 0=S, 1=D, 3=H
    uint8_t  cmode = 0;         // SIMD immediate cmode field
    uint8_t  fp_opcode = 0;     // FP arithmetic opcode
    uint8_t  rmode = 0;         // FP rounding mode
    uint8_t  simd_count = 1;    // SIMD LD1/ST1 register count (1-4)
    uint8_t  simd_struct = 1;   // multi-structure type: 1=LD1/ST1 (consecutive),
                                // 2=LD2/ST2 (de-interleave by 2),
                                // 3=LD3/ST3 (by 3), 4=LD4/ST4 (by 4)
    // uses bits[14:13] for the element index (not register count) and
    // bit[12]=1 to distinguish from multi-structure. The interpreter needs
    // these to dispatch correctly — without them, a single-structure LD1
    // {V0.S}[2] would be misdecoded as a 3-register multi-structure LD1,
    // reading/writing 48 bytes instead of 4.
    bool     is_single_struct = false;  // true for lane/replicate LD1..LD4
    bool     is_ld1r = false;           // true for LD1R/LD2R/LD3R/LD4R
    uint8_t  simd_index = 0;            // element index for single-structure
    // LD1/ST1 post-index writeback. bit[23]=1 selects the post-indexed
    // form ([Xn], #imm / [Xn], Xm); bit[23]=0 is plain [Xn] (no writeback).
    // The offset is: Rm==0b11111 → nregs*(Q?16:8) (multi-struct) or esize
    // (single-struct); Rm==0b11110 → 0; else the value in register Xm.
    bool     post_indexed = false;      // true if the base register is updated
    bool     is_sub = false;    // SUB vs ADD (various groups)
    // ── System registers (MSR/MRS) ──
    uint8_t  sys_op0 = 0;
    uint8_t  sys_op1 = 0;
    uint8_t  sys_crn = 0;
    uint8_t  sys_crm = 0;
    uint8_t  sys_op2 = 0;
    bool     sys_L = false;     // 0=MSR (write), 1=MRS (read)
};
// ── Decode function ─────────────────────────────────────────────────────
// Pure function: extracts fields from `inst` into `d`. No execution.
// Returns true if the instruction was recognized, false if UNKNOWN.
bool decode(DecodedInst& d, uint32_t inst);
// ── Shared helpers (used by both interpreter and JIT) ───────────────────
// Evaluate an ARM64 condition code (0-15) against PSTATE flags.
bool cond_true(uint32_t cond, uint32_t pstate);
// Extend a register value per the option/shift encoding.
// option: 000 UXTB, 001 UXTH, 010 UXTW, 011 UXTX,
//         100 SXTB, 101 SXTH, 110 SXTW, 111 SXTX
uint64_t extend_reg(uint64_t val, uint8_t option, uint8_t shift, bool sf);
// ── FP scalar decode helpers (shared between interpreter and JIT) ───────
//
// The 0x1Exxxxxx encoding space packs many FP/SIMD ops into overlapping
// bit fields. These helpers centralize the field extraction so the
// interpreter (Source/Interpreter/Core/Interpreter.cpp) and the JIT's IR translator
// (Source/FrostJIT/IR/Translate/Integer.cpp) decode identically. Drift between the two
// historically caused FCMP/FABS/FSQRT/FMOV-imm bugs that were tedious
// to track down.
namespace fp_decode {
// AdvSIMD reciprocal estimates/steps must be matched before scalar
// integer conversions: FRECPE's bit16 otherwise disappears in that mask.
constexpr bool is_reciprocal_fp16(uint32_t op) {
    if ((op & (1u << 28)) && !(op & (1u << 30))) return false;
    uint32_t step = op & 0xBFE0FC00;
    uint32_t estimate = op & 0x9FFFFC00;
    return step == 0x0E403C00 || step == 0x0EC03C00 ||
           step == 0x1E403C00 || step == 0x1EC03C00 ||
           estimate == 0x0EF9D800 || estimate == 0x1EF9D800;
}
constexpr bool is_reciprocal(uint32_t op) {
    if ((op & (1u << 28)) && !(op & (1u << 30))) return false;
    uint32_t step = op & 0xBFA0FC00;
    uint32_t estimate = op & 0x9FBFFC00;
    return step == 0x0E20FC00 || step == 0x0EA0FC00 ||
           step == 0x1E20FC00 || step == 0x1EA0FC00 ||
           estimate == 0x0EA1D800 || estimate == 0x1EA1D800 ||
           is_reciprocal_fp16(op);
}
// Allocated encodings of the scalar FP data-processing group (bits 28:24 =
// 1111x, bit 30 = 0) in this CPU profile, which advertises FEAT_FP only.
// S and (except for conversions) M must be 0. ptype 10 is reserved apart
// from FMOV to/from Vn.D[1]; FEAT_FP16 (ptype 11) is not advertised, so only
// the FCVT precision conversions to and from half are allocated there. The
// decoder rejects the rest, so both engines raise SIGILL.
constexpr bool is_allocated_fp_scalar(uint32_t op) {
    const uint32_t sf = op >> 31, ptype = (op >> 22) & 3;
    if ((op >> 29) & 1) return false;                 // S
    if ((op >> 24) & 1) return !sf && ptype < 2;      // FMADD/FMSUB/FNMADD/FNMSUB
    if (!((op >> 21) & 1)) {                          // fixed-point conversions
        const uint32_t rmode_opcode = (op >> 16) & 0x1F;
        const bool known = rmode_opcode == 0x02 || rmode_opcode == 0x03 ||   // SCVTF/UCVTF
                           rmode_opcode == 0x18 || rmode_opcode == 0x19;     // FCVTZS/FCVTZU
        return known && ptype < 2 && (sf || ((op >> 15) & 1));  // W forms: scale >= 32
    }
    switch ((op >> 10) & 3) {
        case 1: case 3: return !sf && ptype < 2;      // FCCMP/FCCMPE, FCSEL
        case 2: return !sf && ptype < 2 && ((op >> 12) & 0xF) <= 8;  // 2-source
    }
    if (((op >> 10) & 0x3F) == 0) {                  // FP <-> integer
        const uint32_t rmode = (op >> 19) & 3, opcode = (op >> 16) & 7;
        if (opcode <= 1) return ptype < 2;                    // FCVT{N,P,M,Z}{S,U}
        if (opcode <= 5) return rmode == 0 && ptype < 2;      // SCVTF/UCVTF/FCVTA{S,U}
        if (rmode == 0) return ptype == sf;                   // FMOV W<->S, X<->D
        return sf && ptype == 2 && rmode == 1;                // FMOV X<->Vn.D[1]
    }
    if (((op >> 10) & 0x1F) == 0x10) {                // 1-source, opcode bits 20:15
        const uint32_t opcode = (op >> 15) & 0x3F;
        if (sf) return false;
        if (opcode >= 4 && opcode <= 7)               // FCVT to another precision
            return ptype != 2 && (opcode & 3) != 2 && (opcode & 3) != ptype;
        // FMOV/FABS/FNEG/FSQRT, FRINT{N,P,M,Z,A,X,I}
        return ptype < 2 && (opcode <= 3 || (opcode >= 8 && opcode <= 15 && opcode != 13));
    }
    if (((op >> 10) & 0xF) == 0x8)                    // FCMP/FCMPE: op = 00, opcode2<2:0> = 0
        return !sf && ptype < 2 && !((op >> 14) & 3) && !(op & 7);
    if (((op >> 10) & 7) == 4)                        // FMOV (immediate): imm5 = 0
        return !sf && ptype < 2 && !((op >> 5) & 0x1F);
    return false;
}
// AdvSIMD vector (0 Q U 01111) and scalar (01 U 11111) words with bit 10
// set are the shift-by-immediate / modified-immediate classes, which need
// bit 23 clear (op1 = 10); by-element words have bit 10 clear. Bit 23 and
// bit 10 both set is unallocated.
constexpr bool is_unallocated_advsimd_imm(uint32_t op) {
    const bool vector = (op & 0x9F000000) == 0x0F000000;
    const bool scalar = (op & 0xDF000000) == 0x5F000000;
    return (vector || scalar) && (op & 0x00800400) == 0x00800400;
}
// Reserved sizes in the AdvSIMD vector three-same (0 Q U 01110 size 1 Rm
// opcode 1) and three-different (... 1 Rm opcode 00) groups, from Arm's
// encoding index. Integer three-same: per (U, opcode) allowed sizes, where
// most size-3 forms need Q (no .1d). FP three-same (opcode 11xxx): bit 23 is
// part of the opcode and double precision needs Q; FHM rows (FMLAL/FMLSL,
// not advertised) are left to the handlers.
constexpr bool is_unallocated_advsimd_three(uint32_t op) {
    const uint32_t U = (op >> 29) & 1, Q = (op >> 30) & 1, size = (op >> 22) & 3;
    if ((op & 0x9F200400) == 0x0E200400) {
        const uint32_t opcode = (op >> 11) & 0x1F;
        if (opcode >= 0x18) {
            const uint32_t a = size >> 1, sz = size & 1;
            if ((opcode == 0x1D && !U) || (opcode == 0x19 && U)) return false;  // FHM
            if (a && (opcode == 0x1B || (opcode == 0x1C && !U) || (opcode == 0x1F && U)))
                return true;
            return sz && !Q;
        }
        // Bits 0-3: allowed size values; bit 4: size 3 needs Q.
        constexpr uint8_t A = 0x1F, N3 = 0x07, L = 0x0F, H = 0x06, B = 0x01, X = 0x00;
        constexpr uint8_t allowed[2][24] = {
            {N3, A, N3, L, N3, A, A, A, A, A, A, A, N3, N3, N3, N3, A, A, N3, N3, N3, N3, H, A},
            {N3, A, N3, L, N3, A, A, A, A, A, A, A, N3, N3, N3, N3, A, A, N3, B,  N3, N3, H, X},
        };
        const uint8_t m = allowed[U][opcode];
        return !((m >> size) & 1) || (size == 3 && (m & 0x10) && !Q);
    }
    if ((op & 0x9F200C00) == 0x0E200000) {
        const uint32_t opcode = (op >> 12) & 0xF;
        if (opcode == 0xF) return true;
        if (opcode == 0xE) return U || size == 1 || size == 2;      // PMULL{2} .8h/.1q
        if (opcode == 0x9 || opcode == 0xB || opcode == 0xD)        // SQDMLAL/SQDMLSL/SQDMULL
            return U || size == 0 || size == 3;
        return size == 3;
    }
    return false;
}
// True iff `op` is an FCMP/FCMPE encoding.
//   bits[31:24]=0x1E, bits[23:22]=ftype, bit[21]=1, bits[15:10]=0b001000
inline bool is_fcmp(uint32_t op) {
    return (op & 0xFF200000) == 0x1E200000 && ((op >> 10) & 0x3F) == 0x08;
}
// True iff this FCMP encoding is the "#0.0" form (vs register form).
//   #0.0 form:    bit[3] = 1 (distinguishes from register form)
//   register form: bit[3] = 0, Rm in bits[20:16]
// Note: bit[4] is the "E" bit (FCMPE vs FCMP), NOT the #0.0 indicator.
//   fcmp d0, #0.0  = 0x1e602008, bits[4:0] = 0b01000
//   fcmpe d0, #0.0 = 0x1e602018, bits[4:0] = 0b11000
//   fcmp d0, d1    = 0x1e612000, bits[4:0] = 0b00000
//   fcmpe d0, d1   = 0x1e612010, bits[4:0] = 0b10000
// The previous check `(op & 0x1F) == 0x08` only matched FCMP #0.0,
// NOT FCMPE #0.0 (0x18). This caused FCMPE #0.0 to be treated as
// the register form, comparing against d24 (bits[4:0]=0x18→Rm=24)
// instead of 0.0. This broke `s < 0 ? -s : s` in musl's sin_test
// (compiler emits `fcmpe d0, #0.0; fcsel d1, d1, d0, mi`).
inline bool fcmp_with_zero(uint32_t op) {
    return (op & 0x08) != 0;  // bit[3] = 1 for #0.0 form
}
// True iff `op` is an FMOV (scalar, immediate) encoding.
//   0 00 11110 ftype 1 imm8 100 00000 Rd
// Mask off ftype (bits[23:22]), imm8 (bits[20:13]), and Rd (bits[4:0])
// so both single and double forms match.
inline bool is_fmov_imm(uint32_t op) {
    return (op & 0xFF201FE0) == 0x1E201000;
}
// True iff `op` is an FP 1-source instruction (FMOV-reg/FABS/FNEG/FSQRT/FRINT*).
//   bit[21]=1, bits[14:10]=0b10000 (constant)
// The 6-bit opcode is in bits[20:15] (= rmode:opcode in the ARM ARM).
inline bool is_fp_1source(uint32_t op) {
    // FCVT (bit[17]=1) but ALSO excluded FRINTA/FRINTX/FRINTI, which
    // have bit[17]=1 AND bit[18]=1. FCVT has bit[18]=0, bit[17]=1.
    // The correct exclusion is bits[18:17] == 0b01 (FCVT only).
    //   bits[18:17]:
    //     00 = FMOV/FABS/FNEG/FSQRT (opcode 0x00-0x03)
    //     01 = FCVT                 (opcode 0x04-0x07) — EXCLUDED
    //     10 = FRINTN/P/M/Z         (opcode 0x08-0x0B)
    //     11 = FRINTA/X/I           (opcode 0x0C-0x0F, 0x0D unused)
    // MUST also require bits[31:24]==0x1E (FPDataProc1). Without it,
    // FMA encodings whose Ra field happens to be 0b10000 collide:
    // `fnmsub s16, s7, s1, s16` = 0x1F21C0F0 has bit21=1 (o2=neg),
    // bits[14:10]=Ra=16=0b10000 and bits[18:17]=00, so it matched
    // is_fp_1source and was misrouted to the FP 1-source handler
    // (opcode 0x21 → default → r=a), silently corrupting the UP
    // column of glm_lookat's cross product (u.y became s7 instead of
    // 0*0-(-1)=1) — degenerate view matrices in minecraft_weekend.
    // The same collision hit interp (Dispatch.cpp), the IR translator
    // (FloatSIMD.cpp) and the JIT gate (BlockCompiler.cpp)
    // because all three share this helper.
    return (op & 0xFF000000) == 0x1E000000 && ((op >> 21) & 1) == 1
           && ((op >> 10) & 0x1F) == 0x10
           && ((op >> 17) & 0x3) != 0x1;
}
// Extract the FP 1-source opcode (bits[20:15], 6 bits).
//   0x00 = FMOV (register)
//   0x01 = FABS
//   0x02 = FNEG
//   0x03 = FSQRT
//   0x04..0x0F = FRINT* family
inline uint8_t fp_1source_opcode(uint32_t op) {
    return (op >> 15) & 0x3F;
}
// VFPExpandImm: expand an 8-bit FP immediate to its 32-bit (single) or
// 64-bit (double) IEEE 754 representation. ftype: 0=S, 1=D.
inline uint64_t vfp_expand_imm(uint8_t imm8, uint8_t ftype) {
    uint64_t sign  = (imm8 >> 7) & 1;
    uint64_t b     = (imm8 >> 6) & 1;
    uint64_t not_b = b ^ 1;
    uint64_t imm6  = imm8 & 0x3F;
    if (ftype == 1) {  // double
        uint64_t rep_b = b * 0xFFULL;
        return (sign << 63) | (not_b << 62) | (rep_b << 54) | (imm6 << 48);
    } else {  // single (ftype == 0)
        uint32_t rep_b = static_cast<uint32_t>(b * 0x1Fu);
        return static_cast<uint64_t>(
            (sign << 31) | (not_b << 30) | (rep_b << 25) | (imm6 << 19));
    }
}
}  // namespace fp_decode
// Sign-extend a value from `bits` width to 64 bits.
// (Defined in Include/Bifrost/Types.hpp — shown here for reference.)
// inline uint64_t sign_extend(uint64_t v, int bits) { ... }
// Rotate right (used by immediate decode).
// (Defined in Include/Bifrost/Types.hpp — shown here for reference.)
// inline uint64_t ror64(uint64_t v, unsigned r) { ... }
} // namespace arm64emu
