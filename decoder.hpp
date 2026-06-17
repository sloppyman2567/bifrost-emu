// decoder.hpp — Shared ARM64 instruction decode tables and helpers.
//
// This header is included by both the interpreter (interpreter.cpp) and
// the future JIT compiler (jit.cpp, planned for v2.0). It defines:
//   - Instruction class enumerations
//   - The DecodedInst struct (fields extracted from a 32-bit ARM64 word)
//   - The decode() function that fills a DecodedInst from raw bits
//   - Shared helpers (condition codes, extend_reg, sign_extend, etc.)
//
// The interpreter calls decode() then dispatches on DecodedInst::cls.
// The JIT will call decode() then emit x86_64 code based on
// DecodedInst::cls. Both share the same decode logic, so adding a new
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
    MADD,           // multiply-accumulate
    MSUB,           // multiply-subtract
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
    LSE_ATOMIC,     // LSE atomic (LDADD/LDCLR/LDEOR/LDSET/SMAX/SMIN/UMAX/UMIN/SWP)
    CAS,            // compare and swap (LSE)

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
};

// ── Decoded instruction ─────────────────────────────────────────────────
// All fields extracted from a 32-bit ARM64 instruction word. Not all
// fields are valid for all instruction classes; the interpreter/JIT
// should only read fields relevant to InstClass.
struct DecodedInst {
    InstClass cls = InstClass::UNKNOWN;
    uint32_t  raw = 0;          // the original 32-bit word

    // Common register fields (positions vary by class, but we extract
    // them into a uniform layout for easy access).
    uint8_t  rd  = 0;           // destination register
    uint8_t  rn  = 0;           // first source register (often base)
    uint8_t  rm  = 0;           // second source register
    uint8_t  ra  = 0;           // third source register (MADD/MSUB)
    uint8_t  rt  = 0;           // transfer register (load/store/atomic)
    uint8_t  rt2 = 0;           // second transfer register (pair)
    uint8_t  rs  = 0;           // status register (STXR) / source (atomic)
    uint8_t  cond = 0;          // condition code (0-15)

    // Width / size
    uint8_t  size = 0;          // 0=8bit, 1=16bit, 2=32bit, 3=64bit
    bool     sf = false;        // 64-bit (vs 32-bit) for data processing
    bool     is_load = false;   // true for LDR/LDP, false for STR/STP
    bool     is_vec = false;    // true for SIMD/FP, false for GP
    bool     set_flags = false; // true for ADDS/SUBS/ANDS

    // Immediates
    int64_t  imm = 0;           // signed immediate (various forms)
    uint64_t imm_u = 0;         // unsigned immediate
    uint8_t  shift = 0;         // shift amount
    uint8_t  shift_type = 0;    // 0=LSL, 1=LSR, 2=ASR, 3=ROR
    uint8_t  extend = 0;        // extend type for extended register

    // Memory addressing
    int64_t  disp = 0;          // displacement for load/store
    uint8_t  mode = 0;          // 0=offset, 1=post-index, 2=pre-index
    bool     writeback = false; // true if base register is written back

    // Atomics
    uint8_t  atom_op = 0;       // LSE atomic opcode (0-15)
    bool     acquire = false;   // acquire-release hint
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

// Sign-extend a value from `bits` width to 64 bits.
// (Defined in arm64_emu.hpp — included here for reference.)
// inline uint64_t sign_extend(uint64_t v, int bits) { ... }

// Rotate right (used by immediate decode).
// (Defined in arm64_emu.hpp — included here for reference.)
// inline uint64_t ror64(uint64_t v, unsigned r) { ... }

} // namespace arm64emu
