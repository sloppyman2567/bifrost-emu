// ir.hpp — Intermediate Representation for bifrost-emu JIT ()
//
// The IR is a list of micro-operations that represent the semantics
// of ARM64 instructions. Each ARM64 instruction translates into 1-N
// IR ops. The IR is then:
//   1. Optimized (DCE, constant folding, copy propagation, peephole)
//   2. Either executed by ops.cpp's tight switch loop (debug path),
//      or compiled to native x86-64 code by frostjit.cpp (the real JIT).
//
// ── Design ────────────────────────────────────────────────────────────
//
// Virtual registers (vregs) are indices into a per-block value table.
//   vreg 0..30  → ARM64 X0..X30   (live across the block)
//   vreg 31     → SP               (live across the block)
//   vreg 32     → XZR              (always 0; writes are discarded)
//   vreg 33..   → scratch temporaries (local to one ARM64 instruction)
//
// The translator reads ARM64 register state into vregs with LOAD_REG,
// performs computation in vreg space, then writes results back with
// STORE_REG. The optimizer can eliminate LOAD_REG/STORE_REG pairs
// when the value is already in a vreg, and can fold IMM + ALU chains
// into a single IMM.
//
// IR ops are deliberately close to x86 semantics so codegen is a
// near 1:1 mapping. This is what makes the JIT fast: each IR op
// typically emits 1-3 x86 instructions, and the optimizer removes
// redundant loads/stores between consecutive ARM64 instructions.
#pragma once
#include "decoder.hpp"
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <vector>
#include <utility>
namespace arm64emu {
// IR opcodes. Keep this list tight — every opcode must be handled in
// both ops.cpp (executor) and frostjit.cpp (codegen).
enum class IROp : uint8_t {
    NOP,            // no operation (used by optimizer as a tombstone)
    IMM,            // dest = imm                          (constant)
    MOV,            // dest = src1                         (copy)
    LOAD_REG,       // dest = arm64_reg[src1]              (read ARM64 reg)
    STORE_REG,      // arm64_reg[dest] = src1              (write ARM64 reg)
    LOAD_MEM,       // dest = mem[src1 + imm]              (width in `width`)
    STORE_MEM,      // mem[src1 + imm] = src2              (width in `width`)
    ADD,            // dest = src1 + src2
    SUB,            // dest = src1 - src2
    MUL,            // dest = src1 * src2      (low 64 bits)
    AND,            // dest = src1 & src2
    OR,             // dest = src1 | src2
    XOR,            // dest = src1 ^ src2
    SHL,            // dest = src1 << (src2 & 63)
    SHR,            // dest = src1 >> (src2 & 63)   (unsigned)
    SAR,            // dest = src1 >> (src2 & 63)   (signed)
    ROR,            // dest = ROR(src1, src2 & 63)
    NOT,            // dest = ~src1
    NEG,            // dest = -src1
    SEXT,           // dest = sign_extend(src1, imm=bits)
    ZEXT,           // dest = zero_extend(src1, imm=bits)
    CLZ,            // dest = count_leading_zeros(src1)
    CLS,            // dest = count_leading_sign_bits(src1)
    RBIT,           // dest = bit_reverse(src1)
    REV16,          // dest = byte_reverse_16(src1)
    REV32,          // dest = byte_reverse_32(src1)
    REV64,          // dest = byte_reverse_64(src1)
    // Flag-setting ops (compute result + set NZCV in cpu.pstate)
    ADDS,           // dest = src1 + src2, set flags.   flags_op=0
    SUBS,           // dest = src1 - src2, set flags.   flags_op=1
    ADCS,           // dest = src1 + src2 + C,  set flags
    SBCS,           // dest = src1 - src2 - 1 + C, set flags
    TST,            // set flags from src1 & src2 (no dest write)
    TST_ZERO,       // set Z flag from (src1 == 0), clear N/C/V (for CBZ/CBNZ)
    BRCOND_ZERO,    // if (src1 == 0) == (cond==EQ) : pc = imm  [no flag deps]
    BRCOND_BIT,     // if ((src1 >> imm) & 1) == (cond==NE) : pc = imm  [no flag deps]
    // Conditional
    CSEL,           // dest = cond ? src1 : src2
    CSINC,          // dest = cond ? src1 : (src2 + 1)
    CSINV,          // dest = cond ? src1 : ~src2
    CSNEG,          // dest = cond ? src1 : -src2
    CCMP,           // if cond: set flags from src1 - src2; else: imm=nzcv
    // Bitfield
    BFM,            // dest = (src1 & ~mask) | (src2 << immr & mask)
    UBFM,           // dest = (src1 ror immr) & mask   (mask from imms)
    SBFM,           // dest = sign_extend((src1 ror immr) & mask, width)
    EXTR,           // dest = (src1:src2) >> immr
    // Branch
    BR,             // pc = src1 (unconditional, register)
    BRCOND,         // if cond(imm): pc = target(imm)  (also arm_pc for fallthrough bookkeeping)
    BRCOND_FALLTHRU,// like BRCOND but fall-through branch — used for B (taken always) / BL
    BL_CALL,        // BL within block — call target block, continue after return.
                    // imm = target PC, arm_pc = BL's PC (LR = arm_pc + 4).
                    // Does NOT end the block. Caller-saved ARM regs (x0-x18, x30)
                    // are invalidated after the call. Callee-saved (x19-x28) survive
                    // if cached in callee-saved host regs (R12/R13/R15).
    // Interpreter fallback (single instruction)
    CALL_INTERP,    // call interpreter for ARM instruction at arm_pc
    // Syscall (treated as block-ending side effect)
    SVC,            // call syscall handler; pc may change
    // FMOV (general ↔ FP register) — native JIT support
    FMOV_G2F,      // v_lo[dest] = src1; v_hi[dest] = 0  (GPR → FP, 64-bit)
    FMOV_F2G,      // dest = v_lo[src1]                    (FP → GPR, 64-bit)
    FMOV_G2FHI,    // v_hi[dest] = src1                    (GPR → FP high half)
    FMOV_FHI2G,    // dest = v_hi[src1]                    (FP high half → GPR)
    // Native FP scalar arithmetic (operate on v_lo/v_hi directly)
    FP_BINOP,      // v_lo[dest] = op(v_lo[src1], v_lo[src2]); v_hi[dest]=0
                   // imm = opcode (0=mul,1=div,2=add,3=sub,4=max,5=min,6=nmul)
                   // width = ftype (0=S, 1=D)
    FP_UNOP,       // v_lo[dest] = op(v_lo[src1]); v_hi[dest]=0
                   // imm = opcode (0=mov,1=abs,2=neg,3=sqrt)
                   // width = ftype (0=S, 1=D)
    // Native SIMD/NEON ops (operate on v_lo/v_hi directly via SSE2/AVX)
    SIMD_LOGICAL,  // v_lo[dest],v_hi[dest] = src1 OP src2
                   // imm = opcode (0=and,1=orr,2=xor,3=bic,4=orn,5=eon)
    SIMD_DUP,      // v_lo[dest] = v_hi[dest] = src1 (broadcast 64-bit)
    SIMD_LDST,     // Load/store 128-bit from memory
                   // dest = vreg index, src1 = addr vreg, imm = offset
                   // width = 0 (store), 1 (load)
    SIMD_ARITH,    // v_lo[dest],v_hi[dest] = src1 OP src2 (integer lane-wise)
                   // imm = opcode (0=add,1=sub,2=mul,3=umin,4=umax,5=smin,6=smax,
                   //                7=orr_imm_lo,8=orr_imm_hi)  — see JIT
                   // width = element size in bytes (1, 2, 4, 8)
                   //   size=8 only valid for add/sub (no pmul etc.)
    SIMD_CMP,      // v_lo[dest],v_hi[dest] = compare(src1, src2) ? all-ones : 0
                   // imm = opcode (0=eq,1=ge_u,2=gt_u,3=ge_s,4=gt_s,5=hi_u,6=hs_u)
                   // width = element size in bytes (1, 2, 4, 8)
    // Native SIMD vector shifts by immediate (v1.4.5-alpha):
    //   SHL:  dest = src1 << shift   (logical left)
    //   USHR: dest = src1 >> shift   (logical right, unsigned)
    //   SSHR: dest = src1 >> shift   (arithmetic right, signed)
    // All operate lane-wise; width = element size in bytes (1, 2, 4, 8);
    // imm = shift amount (0..esize*8-1). On AVX2 hosts the JIT emits
    // 256-bit vpsubw/vpsrld/vpslld etc. for Q=1; on SSE2 hosts it emits
    // two 128-bit psllw/psrld/psrad ops. Without AVX2 the two halves
    // are shifted independently (functionally identical, just slower).
    SIMD_SHL,      // dest = src1 << imm  (per-lane logical left shift)
    SIMD_USHR,     // dest = src1 >> imm  (per-lane logical right shift)
    SIMD_SSHR,     // dest = src1 >>> imm (per-lane arithmetic right shift)
    // Native SIMD FP lane-wise arithmetic (v1.5.1-alpha). Same shape as
    // SIMD_ARITH but for FP elements. Operates on v_lo/v_hi (each 8
    // bytes) across all lanes; JIT emits SSE addps/subps/mulps/divps/
    // minps/maxps (single) or addpd/... (double). FABD = sub + clear
    // sign bit.
    //   imm  = opcode (0=fadd,1=fsub,2=fmul,3=fdiv,4=fmax,5=fmin,
    //          6=fmaxnm,7=fminnm,0xB=fmulx,0xD=fabd)
    //   width = element size in bytes (4=float, 8=double)
    //   flags_op = Q (0=64-bit operand, 1=128-bit: process v_lo AND v_hi)
    SIMD_FP_ARITH,
    // Native FP↔int conversions
    FP_F2I,        // regs[dest] = (int/uint)(v_lo[src1])
                   // imm = 0 (signed), 1 (unsigned); width = ftype
                   // flags_op = sf (JIT ignores this; ir_translate emits
                   //                an explicit ZEXT after FP_F2I when sf=0)
    FP_I2F,        // v_lo[dest] = (float/double)(regs[src1]); v_hi=0
                   // imm = 0 (signed), 1 (unsigned); width = ftype
                   // flags_op = sf (0=32-bit GPR source, 1=64-bit GPR source)
    // Native FP↔int fixed-point conversions (FCVTZS/FCVTZU/SCVTF/UCVTF with
    // a 6-bit scale field). These are the fixed-point variants — the integer
    // variants use FP_F2I/FP_I2F above. The fixed-point form scales the FP
    // value by 2^fbits before/after conversion (fbits = 64 - scale, where
    // scale comes from bits[15:10] of the ARM encoding).
    //
    // These used to route to CALL_INTERP (~20% overhead on workloads
    // that use them, like MD5 K-table init and audio DSP). Native IR ops
    // avoid the interpreter round-trip.
    FP_F2I_FIXED,  // regs[dest] = sat_trunc(v_lo[src1] * 2^fbits, signedness/range)
                   // imm = 0 (signed), 1 (unsigned); width = ftype
                   // flags_op = sf (0=32-bit dest, 1=64-bit dest)
                   // immr = fbits (1..64)
    FP_I2F_FIXED,  // v_lo[dest] = (float/double)(regs[src1]) / 2^fbits; v_hi=0
                   // imm = 0 (signed), 1 (unsigned); width = ftype
                   // flags_op = sf (0=32-bit GPR source, 1=64-bit GPR source)
                   // immr = fbits (1..64)
    FP_CMP,        // compare v_lo[src1] vs v_lo[src2], set pstate
                   // imm bit 0 = 1 for FCMP Dn,#0.0 form, 0 for register form;
                   // width = ftype
    FP_MOVI,       // v_lo[dest] = imm (decoded FP immediate); v_hi=0
                   // width = ftype (0=S, 1=D)
    // Native division (x86 div/idiv)
    UDIV,          // dest = src1 / src2 (unsigned, 64-bit)
    SDIV,          // dest = src1 / src2 (signed, 64-bit)
    // Native multiply-accumulate long (widening)
    SMADDL,        // dest = (int64_t)(int32_t)src2 * (int32_t)src1 + src1_hi
    UMADDL,        // dest = (uint64_t)(uint32_t)src2 * (uint32_t)src1 + src1_hi
    // Native multiply high (128-bit result, high 64 bits)
    SMULH,         // dest = bits[127:64] of (int128)src1 * (int128)src2
    UMULH,         // dest = bits[127:64] of (uint128)src1 * (uint128)src2
    // Native multiply-subtract long (widening)
    SMSUBL,        // dest = (int64_t)acc - (int64)(int32)src1 * (int32)src2
    UMSUBL,        // dest = (uint64_t)acc - (uint64)(uint32)src1 * (uint32)src2
    // Native FP conversions (float <-> double, via x86 cvtss2sd/cvtsd2ss)
    FCVT_S2D,      // v_lo[dest] = (double)(float)v_lo[src1]
    FCVT_D2S,      // v_lo[dest] = (float)(double)v_lo[src1]
    // Native FP round to integer (via x86 roundss/roundsd)
    FRINT,         // v_lo[dest] = round(v_lo[src1]); imm = rounding mode
                   // 0=N(nearest), 1=P(+inf), 2=M(-inf), 3=Z(0), 4=I(current), 5=X(exact)
    // Native FP fused multiply-add (via x86 vfmadd or decomposition)
    // FMADD:  v_lo[dest] =  v_lo[src2] * v_lo[src1] + v_lo[acc]; width=ftype
    // FMSUB:  v_lo[dest] = -v_lo[src2] * v_lo[src1] + v_lo[acc]  (= acc - src1*src2)
    // FNMADD: v_lo[dest] = -v_lo[src2] * v_lo[src1] + v_lo[acc]  (same numerical
    //         result as FMSUB, but IEEE 754 sign rules differ on NaN/signed-zero
    //         inputs — must be modeled as a fused op, not decomposed)
    // FNMSUB: v_lo[dest] = -v_lo[src2] * v_lo[src1] - v_lo[acc]  (= -(src1*src2 + acc))
    //
    // On x86 with FMA3 (Haswell+, 2013+): emit vfmadd231ss/sd (FMADD),
    // vfmsub231ss/sd (FMSUB), vfnmadd231ss/sd (FNMADD), vfnmsub231ss/sd
    // (FNMSUB) — single-rounded, IEEE 754-correct fused mul-add.
    //
    // Without FMA3: decompose into mulsd+addsd (double-rounded, NOT
    // IEEE 754-correct for edge cases — a known limitation).
    // The decomposition path produces the same numerical result as the
    // interpreter (which also decomposes), so JIT/interpreter agree.
    FMADD,         // v_lo[dest] = v_lo[src2] * v_lo[src1] + v_lo[acc]; width=ftype
    FMSUB,         // v_lo[dest] = -v_lo[src2] * v_lo[src1] + v_lo[acc]; width=ftype
    FNMADD,        // v_lo[dest] = -v_lo[src2] * v_lo[src1] + v_lo[acc]; width=ftype (negated product)
    FNMSUB,        // v_lo[dest] = -v_lo[src2] * v_lo[src1] - v_lo[acc]; width=ftype
    // System register access (TPIDR_EL0, NZCV, FPCR, FPSR)
    MRS,           // dest = system_reg[imm]  (imm = reg index)
    MSR,           // system_reg[imm] = src1
    // Native LSE atomics (via x86 lock-prefixed instructions)
    // ATOMIC: atomic operation on mem[src1].
    //   src1 = base address vreg
    //   src2 = operand vreg (rs for non-CAS; desired=rt for CAS)
    //   dest = destination vreg (old value, 0 if store-only)
    //   width = 1/2/4/8
    //   cond = atom_op (0=LDADD,1=LDCLR,2=LDEOR,3=LDSET,4=SMAX,5=SMIN,
    //                    6=UMAX,7=UMIN,8=SWP,0xC-0xF=CAS)
    //   flags_op = is_load (1=return old value, 0=store-only)
    //   imm = ARM reg index for slow-path result reload (rt for non-CAS,
    //         rs for CAS — also the expected-value source for CAS)
    ATOMIC,
    // Fast LL/SC via C helpers (bypass interpreter decode overhead)
    LDXR_FAST,     // dest = jit_ldxr(emu, cpu, src1, width); marks reservation
                   // width = 1/2/4/8; also loads to ARM reg dest
    STXR_FAST,     // dest = jit_stxr(emu, cpu, src1, src2, width); 0=ok, 1=fail
                   // src1=addr, src2=value, dest=ARM reg for status (rs)
    STLR_FAST,     // jit_stlr(emu, cpu, src1, src2, width); store-release
                   // src1=addr, src2=value
    // v1.5.0.alpha: Native ARMv8 Crypto Extensions (AES-NI / PCLMULQDQ).
    // These operate on the full 128-bit V register (v_lo + v_hi).
    // dest = result vreg; src1 = state vreg; src2 = key vreg (AES) or
    // second operand (PMULL).
    // imm = sub-opcode:
    //   0 = AESE  (AddRoundKey + SubBytes + ShiftRows)
    //   1 = AESD  (AddRoundKey + InvSubBytes + InvShiftRows)
    //   2 = AESMC (MixColumns)
    //   3 = AESIMC (InvMixColumns)
    //   4 = PMULL  (low 64-bit poly mul → 128-bit)
    //   5 = PMULL2 (high 64-bit poly mul → 128-bit)
    // On hosts with AES-NI/PCLMULQDQ, the JIT emits native aesenc/aesdec
    // /aesimc/aesmc / pclmulqdq. On hosts without, it falls back to
    // CALL_INTERP (which calls the interpreter's software table-driven
    // implementation in interp_crypto.hpp).
    AES_CRYPTO,
};
// Condition codes (same encoding as ARM64 cond field).
// 0=EQ, 1=NE, 2=CS, 3=CC, 4=MI, 5=PL, 6=VS, 7=VC,
// 8=HI, 9=LS, 10=GE, 11=LT, 12=GT, 13=LE, 14=AL, 15=NV
static inline const char* cond_name(uint8_t c) {
    static const char* names[16] = {
        "EQ","NE","CS","CC","MI","PL","VS","VC",
        "HI","LS","GE","LT","GT","LE","AL","NV"
    };
    return names[c & 15];
}
// A single IR instruction.
struct IRInst {
    IROp    op;
    uint16_t dest;   // destination vreg (0 if no dest)  [was uint8_t]
    uint16_t src1;   // source vreg 1                     [was uint8_t]
    uint16_t src2;   // source vreg 2                     [was uint8_t]
    uint16_t aux;    // auxiliary vreg (SMADDL/SMSUBL accumulator)
    uint8_t width;   // for LOAD_MEM/STORE_MEM: 1/2/4/8; for SEXT/ZEXT: bits; for BFM/UBFM/SBFM/EXTR: encoded
    uint8_t cond;    // for CSEL*/CCMP/BRCOND: ARM64 condition code
    uint8_t flags_op;// for ADDS/SUBS/ADCS/SBCS: 0=add, 1=sub (controls C flag inversion)
    uint64_t imm;    // immediate value / mem offset / branch target
    uint64_t arm_pc; // PC of the original ARM instruction (for CALL_INTERP, BRCOND, SVC)
    // Optional metadata used by the optimizer. Defaults to zero.
    uint8_t immr = 0;   // for BFM/UBFM/SBFM: rotate amount
    uint8_t imms = 0;   // for BFM/UBFM/SBFM: field width selector
    uint8_t sf   = 0;   // 1 if 64-bit, 0 if 32-bit (for masking)
};
// An IR block: a list of IR instructions translated from a basic block
// of ARM64 code.
struct IRBlock {
    uint64_t start_pc = 0;
    int count = 0;  // number of ARM64 instructions translated (for fall-through PC)
    std::vector<IRInst> insts;
    bool ends_with_branch = false;
    // Optimization stats (filled by optimize_ir)
    int dce_removed = 0;
    int fold_subst  = 0;
    IRBlock() = default;
};
// Translate a single ARM64 instruction into IR ops.
// Appends 1+ IRInst to `block->insts`. Returns true if the instruction
// ends the block (branch/ret/svc).
bool translate_to_ir(IRBlock& block, const DecodedInst& d, uint64_t cur_pc);
// Reset the per-block vreg allocator. Call at the start of each block.
void ir_reset_vreg_alloc();
// Optimize an IR block in place. Performs:
//   - Constant folding (IMM + ALU → IMM)
//   - Copy propagation (MOV chains)
//   - Dead code elimination (unused stores/scratch ops)
//   - Peephole (load+op fusion, mask elimination when sf=1)
//   - Local register caching (LOAD_REG → reuse cached vreg if value is live)
void optimize_ir(IRBlock& block);
// Dump an IR block to stderr for debugging.
void dump_ir(const IRBlock& block, FILE* out = stderr);
} // namespace arm64emu
