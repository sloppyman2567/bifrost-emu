// decoder.hpp — Shared ARM64 instruction decode tables and helpers.
//
// This header is included by both the interpreter (interpreter.cpp) and
// the JIT (frostjit.cpp). It defines:
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
    SIMD_SHIFT,     // vector SHL/USHR/SHRN
    SIMD_DUP,       // vector duplicate
    SIMD_CNT,       // vector count
    SIMD_REV,       // vector byte reverse
    FMOV,           // FP move (register or general)
    FMOV_IMM,       // FP move immediate
    FMOV_VD1,       // FMOV Vd.D[1], Rn (move GPR to high 64 bits)
    FMOV_RVD1,      // FMOV Rn, Vm.D[1] (move high 64 bits to GPR)
    FADD,           // FP add
    FSUB,           // FP subtract
    FMUL,           // FP multiply
    FDIV,           // FP divide
    FMAX,           // FP max
    FMIN,           // FP min
    FNMUL,          // FP negative multiply
    FMADD,          // FP fused multiply-add
    FMSUB,          // FP fused multiply-subtract
    FABS,           // FP absolute value
    FNEG,           // FP negate
    FSQRT,          // FP square root
    FCMP,           // FP compare
    FCMPE,          // FP compare with exception
    FCVT,           // FP convert (S↔D)
    FCVTZS,         // FP to signed int (toward zero)
    FCVTZU,         // FP to unsigned int (toward zero)
    SCVTF,          // signed int to FP
    UCVTF,          // unsigned int to FP
    FRINT,          // FP round to integer (all modes)
    FCSEL,          // FP conditional select

    // System
    SVC_IMM,        // SVC #imm
    HVC_IMM,        // HVC #imm (error in user mode)
    SMC_IMM,        // SMC #imm (error in user mode)
    BRK_IMM,        // BRK #imm
    HLT_IMM,        // HLT #imm
    MSR_SYS,        // MSR <sysreg>, Xt
    MRS_SYS,        // MRS Xt, <sysreg>
    HINT,           // NOP/WFE/WFI/SEV/YIELD
    // BARRIER already declared above
    CLREX_INST,     // CLREX
    SYS_NOP,        // Other system instructions (NOP)

    // SIMD data processing (sub-dispatched within the case)
    SIMD_DP,        // Generic SIMD data processing (sub-dispatch by raw bits)
    FP_SCALAR,      // Generic FP scalar (sub-dispatch by raw bits)
};

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
// interpreter (src/interp/interpreter.cpp) and the JIT's IR translator
// (src/ir/ir_translate.cpp) decode identically. Drift between the two
// historically caused FCMP/FABS/FSQRT/FMOV-imm bugs that were tedious
// to track down.

namespace fp_decode {

// True iff `op` is an FCMP/FCMPE encoding.
//   bits[31:24]=0x1E, bits[23:22]=ftype, bit[21]=1, bits[15:10]=0b001000
inline bool is_fcmp(uint32_t op) {
    return (op & 0xFF200000) == 0x1E200000 && ((op >> 10) & 0x3F) == 0x08;
}

// True iff this FCMP encoding is the "#0.0" form (vs register form).
//   #0.0 form:    bits[4:0] = 0b01000 (Op = 8)
//   register form: bits[4:0] = 0b00000, Rm in bits[20:16]
inline bool fcmp_with_zero(uint32_t op) {
    return (op & 0x1F) == 0x08;
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
    return ((op >> 21) & 1) == 1 && ((op >> 10) & 0x1F) == 0x10;
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
// (Defined in arm64_emu.hpp — included here for reference.)
// inline uint64_t sign_extend(uint64_t v, int bits) { ... }

// Rotate right (used by immediate decode).
// (Defined in arm64_emu.hpp — included here for reference.)
// inline uint64_t ror64(uint64_t v, unsigned r) { ... }

} // namespace arm64emu
