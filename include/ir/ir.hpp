// ir.hpp — Intermediate Representation for bifrost-emu JIT ()
//
// The IR is a list of micro-operations that represent the semantics
// of ARM64 instructions. Each ARM64 instruction translates into 1-N
// IR ops. The IR is then:
//   1. Optimized (DCE, constant folding, copy propagation, peephole)
//   2. Compiled to native x86-64 code by frostjit.cpp (the real JIT).
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
// Forward declaration for IRInst's friend passes (defined below).
struct IRBlock;
// IR opcodes. Keep this list tight — every opcode must be handled in
// frostjit.cpp (codegen).
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
    BRCOND_SKIP,    // mid-block conditional skip (leaf inlining). if cond(imm):
                    // jump over the next `imm` IR ops (to the op right after
                    // the skip region). Does NOT end the block — both paths
                    // stay inline. Used to flatten a leaf's forward branch.
                    // cond = ARM64 condition code; imm = number of IR ops in
                    // the skipped region; arm_pc = the branch's ARM PC.
    BL_CALL,        // BL within block — call target block, continue after return.
                    // imm = target PC, arm_pc = BL's PC (LR = arm_pc + 4).
                    // Does NOT end the block. Caller-saved ARM regs (x0-x18, x30)
                    // are invalidated after the call. Callee-saved (x19-x28) survive
                    // if cached in callee-saved host regs (R12/R13/R15).
    BLR_CALL,       // BLR within block — call block at src1 (a register target),
                    // continue after return. Same semantics as BL_CALL (arm_pc =
                    // BLR's PC, LR = arm_pc + 4, does NOT end the block) but the
                    // target is dynamic (function pointer in a GPR), so codegen
                    // loads src1 into the arg reg at the call site instead of an
                    // immediate. Mirrors BL_CALL's invalidate/cache rules.
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
    FP_MOV,        // FP↔FP register move: v_lo[dest] = v_lo[src1]
                    // width = ftype (0=S single: v_lo[dest] = (float)v_lo[src1],
                    // v_hi[dest] = 0; 1=D double: v_lo/v_hi both copied).
                    // One IR op instead of the old 4-op
                    // FMOV_F2G→(IMM/AND)→FMOV_G2F(→FHI2G/G2FHI) round-trip —
                    // noise3/grad3 alone emit 67 fmovs per body.
    // Native SIMD/NEON ops (operate on v_lo/v_hi directly via SSE2/AVX)
    SIMD_LOGICAL,  // v_lo[dest],v_hi[dest] = src1 OP src2
                   // imm = opcode (0=and,1=orr,2=xor,3=bic,4=orn,5=eon)
    SIMD_DUP,      // v_lo[dest] = v_hi[dest] = src1 (broadcast 64-bit)
    SIMD_UMOV,     // regs[dest] = element[imm] of vector src1 (v_lo/v_hi)
                   // width = element size in bytes (1, 2, 4, 8);
                   // imm = lane index (0..15/esize-1); flags_op = Q
                   // (0=W d zero-extended, 1=X d — codegen reads the full
                   // element regardless, mirroring the interpreter)
    SIMD_SMOV,     // regs[dest] = SIGN-extended element[imm] of vector src1
                    // (SMOV). width = element size in bytes (1, 2, 4 — the
                    // .D form is UNALLOCATED and never emitted); imm = lane
                    // index; flags_op = Q (0=Wd sext to 32 bits, 1=Xd sext
                    // to 64 bits)
    SIMD_SATADDSUB,// v_lo[dest],v_hi[dest] = SATURATING src1 +/- src2
                    // imm = subop (0=SQADD, 1=UQADD, 2=SQSUB, 3=UQSUB);
                    // width = element size in bytes (1 or 2 ONLY — the table
                    // guard `size < 2` keeps 32/64-bit lanes on the interp,
                    // which has no pre-AVX512 native form); flags_op = Q
    SIMD_MOVI,     // v_lo[dest],v_hi[dest] = broadcast of imm (MOVI/MVNI)
                   // imm = pre-expanded 64-bit lane pattern; flags_op = Q
                   // (1=128-bit: v_hi = pattern; 0=64-bit: v_hi = 0)
    SIMD_ORRIMM,   // v_lo[dest],v_hi[dest] = dest OR/BIC imm (read-modify-write:
                   // ORR/BIC (vector, immediate) read the DESTINATION as their
                   // source, like the interpreter). imm = lane pattern;
                   // cond = 0=ORR, 1=BIC; flags_op = Q.
    SIMD_LDST,     // Load/store 128-bit from memory
                   // dest = vreg index, src1 = addr vreg, imm = offset
                   // width = 0 (store), 1 (load)
    SIMD_LD16,     // 16-byte vector load: v_lo[dest]&v_hi[dest] = mem[src1+imm]
                   // single bounds-check + one 16-byte load (unlike two
                   // 8-byte LOAD_MEM). src2 ignored.
    SIMD_ST16,     // 16-byte vector store: mem[src1+imm] = v_lo[src2]&v_hi[src2]
                   // single bounds-check + one 16-byte store.
                   // flags_op = register count (1..4, contiguous regs src2+i);
                   // cond=1 → broadcast (`stp q0,q0`): every half stores the
                   // SAME src2 vector (nregs halves span 16*nregs bytes).
    SIMD_ARITH,    // v_lo[dest],v_hi[dest] = src1 OP src2 (integer lane-wise)
                   // imm = opcode (0=add,1=sub,2=mul,3=umin,4=umax,5=smin,6=smax,
                   //                7=orr_imm_lo,8=orr_imm_hi)  — see JIT
                   // width = element size in bytes (1, 2, 4, 8)
                   //   size=8 only valid for add/sub (no pmul etc.)
    SIMD_CMP,      // v_lo[dest],v_hi[dest] = compare(src1, src2) ? all-ones : 0
                   // imm = opcode (0=eq CMEQ, 1=gt_s CMGT, 2=ge_s CMGE,
                   //              3=gt_u CMHI, 4=ge_u CMHS)
                   // width = element size in bytes (1, 2, 4, 8)
                   // flags_op = Q (1=128-bit: process v_lo AND v_hi; 0=64-bit:
                   // process v_lo only and ZERO v_hi — matches the interp)
    // Native SIMD vector shifts by immediate (v1.4.5-alpha):
    //   SHL:  dest = src1 << shift   (logical left)
    //   USHR: dest = src1 >> shift   (logical right, unsigned)
    //   SSHR: dest = src1 >>> shift  (arithmetic right, signed)
    //   USRA: dest = dest + (src1 >> shift)            (logical right + accumulate)
    //   SSRA: dest = dest + (src1 >>> shift)           (arithmetic right + accumulate)
    //   SLI:  dest = (src1 << shift) | (dest & ((1<<shift)-1))  (shift-left insert)
    //   SRI:  dest = (src1 >> shift) | (dest & ~((1<<(esize*8-shift))-1))  (shift-right insert)
    // All operate lane-wise; width = element size in bytes (1, 2, 4, 8);
    // imm = shift amount (0..esize*8-1); flags_op = Q (1=128-bit, process
    // v_lo AND v_hi; 0=64-bit, process v_lo only and ZERO v_hi). On AVX2
    // hosts the JIT emits 256-bit VEX ops (vinserti128/vextracti128 +
    // vpsllw/vpsrld/vpslld/vpaddw/vpor...) for Q=1; otherwise each 64-bit
    // half is processed with 128-bit SSE2 ops (functionally identical).
    SIMD_SHL,      // dest = src1 << imm  (per-lane logical left shift)
    SIMD_USHR,     // dest = src1 >> imm  (per-lane logical right shift)
    SIMD_SSHR,     // dest = src1 >>> imm (per-lane arithmetic right shift)
    SIMD_USRA,     // dest += src1 >> imm (unsigned logical right + accumulate)
    SIMD_SSRA,     // dest += src1 >>> imm (signed arithmetic right + accumulate)
    SIMD_URSRA,    // dest += round(src1 >> imm) (unsigned, round-half-up)
    SIMD_SRSRA,    // dest += round(src1 >>> imm) (signed, round-half-up)
    SIMD_SLI,      // dest = (src1 << imm) | (dest & ((1<<imm)-1))
    SIMD_SRI,      // dest = (src1 >> imm) | (dest & ~((1<<(esize*8-imm))-1))
    // Native SIMD FP lane-wise arithmetic (1.5.4-alpha). Same shape as
    // SIMD_ARITH but for FP elements. Operates on v_lo/v_hi (each 8
    // bytes) across all lanes; JIT emits SSE addps/subps/mulps/divps/
    // minps/maxps (single) or addpd/... (double). FABD = sub + clear
    // sign bit.
    //   imm  = opcode (0=fadd,1=fsub,2=fmul,3=fdiv,4=fmax,5=fmin,
    //          6=fmaxnm,7=fminnm,0xB=fmulx,0xD=fabd)
    //   width = element size in bytes (4=float, 8=double)
    //   flags_op = Q (0=64-bit operand, 1=128-bit: process v_lo AND v_hi)
    SIMD_FP_ARITH,
    // Native SIMD FP fused 3-source (FMLA/FMLS, 1.5.4-alpha). Accumulates
    // into dest: dest = dest ± src1*src2 per lane. Same shape as
    // SIMD_FP_ARITH but reads the OLD dest as the accumulator.
    //   imm  = opcode (0=fmla, 1=fmls)
    //   width = element size in bytes (4=float, 8=double)
    //   flags_op = Q (0=64-bit operand, 1=128-bit: process v_lo AND v_hi)
    SIMD_FP_FMA,
    // Native SIMD 2-register misc (CNT/NOT/RBIT/ABS/NEG, 1.5.4-alpha).
    // Unary lane-wise ops on the FULL 128-bit source (v_lo + v_hi).
    //   imm  = opcode (0=CNT, 1=NOT, 2=RBIT, 3=ABS, 4=NEG)
    //   width = element size in bytes (1..8; CNT/NOT/RBIT only valid 1/2/4,
    //           ABS/NEG valid 1/2/4/8)
    //   flags_op = Q (0=64-bit operand: process v_lo only, ZERO v_hi;
    //                 1=128-bit: process v_lo AND v_hi)
    SIMD_2REG,
    // Native SIMD vector int<->FP converts (SCVTF/UCVTF/FCVTZS/FCVTZU,
    // 1.5.4-alpha). Operates on the FULL 128-bit source.
    //   imm  = opcode (0=SCVTF s32->f32, 1=UCVTF u32->f32, 2=FCVTZS f32->s32,
    //                  3=FCVTZU f32->u32)
    //   width = element size in bytes (4 only; the 8-byte 2D/1D forms stay
    //           on the interpreter)
    //   flags_op = Q (0=64-bit operand: process v_lo only, ZERO v_hi;
    //                 1=128-bit: process v_lo AND v_hi)
    SIMD_CVTF,
    // Native SIMD ADDP (vector pairwise add, 1.5.4-alpha). Byte pairs only
    // (esize=1, 8B/16B — the interp's size=0 path is the semantic ref).
    //   width = 1
    //   flags_op = Q (0=64-bit operand: 8B -> 4 results in v_lo, ZERO v_hi;
    //                 1=128-bit: 16B -> 8 results in v_lo, v_hi = high 8)
    SIMD_ADDP,
    // Native SIMD narrowing (XTN/SQXTN/SQXTUN/UQXTN, 1.5.4-alpha). Reads the
    // FULL 128-bit source (both 64-bit halves) and narrows each element to
    // half width. Dest element size = width/2.
    //   imm  = opcode (0=XTN, 1=SQXTUN, 2=SQXTN, 3=UQXTN)
    //   width = SOURCE element size in bytes (2, 4, 8)
    //   flags_op = Q (0=low 64 bits written, v_hi=0; 1=HIGH 64 bits written,
    //                 v_lo preserved — the *2 forms)
    SIMD_XTN,
    // Native SIMD TBL/TBX (vector table lookup, 1.5.4-alpha). Two forms:
    // one or two source table regs (TBL1/TBX1 vs TBL2/TBX2). Vn (rn) is the
    // TABLE, Vm (rm) is the INDEX vector (interp_fp.cpp case 0x0E000000).
    //   Vd[i] = table[index[i]]; out-of-range -> 0 (TBL) / keep old (TBX).
    //   src1 = table vreg (rn, first of nregs); src2 = index vreg (rm).
    //   flags_op = (is_tbx << 1) | Q; imm = number of table regs
    //          (1 = TBL1/TBX1, 2 = TBL2/TBX2)
    SIMD_TBL,
    // Native SIMD INS (element, vector -> element, 1.5.4-alpha). Copies one
    // element of src1 into dest at a byte offset: the interp's
    // read-modify-write on v_lo/v_hi. GPR-mediated (NOT vec-cache pinned).
    //   src1 = source vector (Vn), src2 = read-modify-write dest (= dest)
    //   width = element size in bytes (1, 2, 4, 8)
    //   imm   = destination element byte offset (didx * esize)
    //   aux   = source element index (sidx)
    //   flags_op = Q (1=128-bit destination, 0=64-bit)
    // Typed form: SimdInsParams + make_ins/ins_params (like SIMD_TBL).
    SIMD_INS,
    // Native SIMD permute (ZIP1/ZIP2/UZP1/UZP2/TRN1/TRN2, 1.5.4-alpha).
    // Element-wise permute of two source vectors into dest (the interp's
    // permute-pairs block in interp_fp.cpp is the semantic reference).
    //   src1 = source A (rn), src2 = source B (rm)
    //   width = element size in bytes (1, 2, 4, 8)
    //   imm  = opc6 (bits[15:10] of the encoding): 0x06=UZP1, 0x0A=TRN1,
    //          0x0E=ZIP1, 0x16=UZP2, 0x1A=TRN2, 0x1E=ZIP2
    //   flags_op = Q (0=64-bit operands: process v_lo only, ZERO v_hi;
    //                 1=128-bit: process v_lo AND v_hi)
    SIMD_PERMUTE,
    // Native SIMD pairwise max/min (SMAXP/SMINP/UMAXP/UMINP, 1.5.4-alpha).
    // Element-wise min/max of adjacent pairs within each source (the
    // interp's pairwise block in interp_fp.cpp is the semantic reference).
    // Q=1: Vd = pairwise(Vn) ++ pairwise(Vm) (first half then second half);
    // Q=0: Vd low 8 bytes = {pairwise(Vn), pairwise(Vm)} (both sources
    // contribute; Vm half is NOT zeroed — real-ARM semantics).
    //   src1 = source A (rn), src2 = source B (rm)
    //   width = element size in bytes (1, 2, 4 — size==3 has no valid
    //           encoding, gas rejects .1d/.2d)
    //   imm  = subop (0=SMAXP, 1=SMINP, 2=UMAXP, 3=UMINP)
    //   flags_op = Q (0=64-bit operands, 1=128-bit)
    SIMD_PAIRMIN,
    // SIMD widening abs-diff / abs-diff-accumulate (SABDL/UABDL/SABAL/
    // UABAL). src1=rn, src2=rm; width = SOURCE esize (1/2/4);
    // imm = subop (0=SABDL, 1=UABDL, 2=SABAL, 3=UABAL); flags_op = Q
    // (Q selects which SOURCE half; dest is always full 128-bit).
    SIMD_ABDL,
    // Same-width absolute difference (SABD/UABD). src1=rn, src2=rm;
    // width = esize; imm = subop (0=S signed, 1=U unsigned); flags_op = Q.
    SIMD_ABD,
    // Widening add/sub with narrow source (SADDW/UADDW/SSUBW/USUBW).
    // width = SOURCE esize (2/4); imm = subop (0=SADDW, 1=UADDW,
    // 2=SSUBW, 3=USUBW); flags_op = Q (source-half select).
    SIMD_ADDW,
    // Add/sub and narrow high (ADDHN/RADDHN/SUBHN/RSUBHN).
    // width = INPUT esize (2/4/8); imm = subop (0=ADDHN, 1=RADDHN,
    // 2=SUBHN, 3=RSUBHN); flags_op = Q (dest-half select: Q=1 writes
    // v_hi and preserves v_lo — the "2" variants).
    SIMD_ADDHN,
    // Saturating narrowing shift-by-immediate (SQSHRN/UQSHRN/SQRSHRN/
    // URQSHRN/SQSHRUN/SQRSHRUN). width = SOURCE esize; imm = subop |
    // (shift << 8) where shift = esize_src*16 − (immh:immb), 1..esize*8;
    // flags_op = Q (dest-half select).
    SIMD_SHRN_SAT,
    // Integer by-element multiply family (vector x indexed element):
    // MUL/MLA/MLS/SMULL/UMULL/SMLAL/UMLAL/SMLSL/UMLSL/SQDMULH/SQRDMULH/
    // SQDMULL. src1=rn, src2=rm; width = SOURCE esize (1/2/4);
    // imm = subop | (lane_index << 8); flags_op = Q.
    SIMD_MUL_ELEM,
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
    // FMSUB:  v_lo[dest] =  v_lo[acc] - v_lo[src2] * v_lo[src1]; width=ftype
    // FNMADD: v_lo[dest] = -(v_lo[src2] * v_lo[src1] + v_lo[acc]); width=ftype
    // FNMSUB: v_lo[dest] =  v_lo[src2] * v_lo[src1] - v_lo[acc]; width=ftype
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
    // 1.5.4-alpha: Native ARMv8 Crypto Extensions (AES-NI / PCLMULQDQ).
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
    FP_CSEL,  // fcsel Sd/Dd,Sn,Sm,cond → v_lo[d] = cond ? v_lo[n] : v_lo[m]; v_hi[d]=0
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
    // ── Public dataflow fields ────────────────────────────────────
    // Everyone may read/write these: they carry vreg plumbing, never
    // per-op parameter contracts.
    IROp    op;
    uint16_t dest;   // destination vreg (0 if no dest)  [was uint8_t]
    uint16_t src1;   // source vreg 1                     [was uint8_t]
    uint16_t src2;   // source vreg 2                     [was uint8_t]
    uint16_t aux;    // auxiliary vreg (SMADDL/SMSUBL accumulator)
    uint64_t arm_pc; // PC of the original ARM instruction (for CALL_INTERP, BRCOND, SVC)
 private:
    // ── Private parameter fields ──────────────────────────────────
    // Per-op contracts (see each factory). NOBODY outside this struct
    // and the three friends below may touch these: translators pack via
    // factories, codegen unpacks via readers. This is what makes a
    // pack/unpack mismatch a compile error instead of a miscompile.
    uint8_t width;   // (see factories — meaning varies per op)
    uint8_t cond;    // (see factories — meaning varies per op)
    uint8_t flags_op;// (see factories — meaning varies per op)
    uint64_t imm;    // (see factories — meaning varies per op)
    // Optional metadata used by the optimizer. Defaults to zero.
    uint8_t immr = 0;   // for BFM/UBFM/SBFM: rotate amount
    uint8_t imms = 0;   // for BFM/UBFM/SBFM: field width selector
    uint8_t sf   = 0;   // 1 if 64-bit, 0 if 32-bit (for masking)
    // Generic passes with legitimate raw access: the validator (checks
    // packing), the disassembler (prints raw), the optimizer (folds and
    // rewrites generically). Everything else goes through factories.
    friend bool validate_ir_block(const IRBlock& block, FILE* out);
    friend void dump_ir(const IRBlock& block, FILE* out);
    friend void optimize_ir(IRBlock& block, bool force_fwd);
 public:

    // ── Typed per-op factories/readers ──────────────────────────────
    // The raw parameter fields above carry a different contract per op
    // (see each IROp comment). Factories pack a typed param struct into
    // those fields; readers unpack. Both are total functions (no traps)
    // so codegen can use readers on its hot path; range checking lives
    // in validate_ir_block(). Ops migrate one by one — unmigrated ops
    // keep using the raw fields directly.
    static IRInst make_tbl(uint16_t dest, uint16_t table, uint16_t index,
                           const struct SimdTblParams& p, uint64_t arm_pc);
    struct SimdTblParams tbl_params() const;
    static IRInst make_ins(uint16_t dest, uint16_t src_vec, uint16_t rmw_dest,
                           const struct SimdInsParams& p, uint64_t arm_pc);
    struct SimdInsParams ins_params() const;
    static IRInst make_2reg(uint16_t dest, uint16_t src,
                            const struct SimdSubopParams& p, uint64_t arm_pc);
    struct SimdSubopParams p2reg_params() const;
    static IRInst make_cvtf(uint16_t dest, uint16_t src,
                            const struct SimdSubopParams& p, uint64_t arm_pc);
    struct SimdSubopParams cvtf_params() const;
    static IRInst make_xtn(uint16_t dest, uint16_t src,
                           const struct SimdSubopParams& p, uint64_t arm_pc);
    struct SimdSubopParams xtn_params() const;
    static IRInst make_permute(uint16_t dest, uint16_t src1, uint16_t src2,
                               const struct SimdBinopParams& p,
                               uint64_t arm_pc);
    struct SimdBinopParams permute_params() const;
    static IRInst make_pairmin(uint16_t dest, uint16_t src1, uint16_t src2,
                               const struct SimdBinopParams& p,
                               uint64_t arm_pc);
    struct SimdBinopParams pairmin_params() const;
    static IRInst make_addp(uint16_t dest, uint16_t src1, uint16_t src2,
                            const struct SimdAddpParams& p, uint64_t arm_pc);
    struct SimdAddpParams addp_params() const;
    static IRInst make_arith(uint16_t dest, uint16_t src1, uint16_t src2,
                             const struct SimdArithParams& p, uint64_t arm_pc);
    struct SimdArithParams arith_params() const;
    static IRInst make_logical(uint16_t dest, uint16_t src1, uint16_t src2,
                               const struct SimdLogicParams& p,
                               uint64_t arm_pc);
    struct SimdLogicParams logical_params() const;
    static IRInst make_shift(IROp op, uint16_t dest, uint16_t src,
                             const struct SimdShiftParams& p,
                             uint64_t arm_pc);
    struct SimdShiftParams shift_params() const;
    static IRInst make_umov(uint16_t dest, uint16_t src_vec,
                            const struct SimdMovParams& p, uint64_t arm_pc);
    struct SimdMovParams umov_params() const;
    static IRInst make_smov(uint16_t dest, uint16_t src_vec,
                            const struct SimdMovParams& p, uint64_t arm_pc);
    struct SimdMovParams smov_params() const;
    static IRInst make_orrimm(uint16_t dest, const struct SimdOrrImmParams& p,
                              uint64_t arm_pc);
    struct SimdOrrImmParams orrimm_params() const;
    static IRInst make_movi(uint16_t dest, const struct SimdMoviParams& p,
                            uint64_t arm_pc);
    struct SimdMoviParams movi_params() const;
    static IRInst make_dup(uint16_t dest, uint16_t src,
                           const struct SimdDupParams& p, uint64_t arm_pc);
    struct SimdDupParams dup_params() const;
    static IRInst make_shrn_sat(uint16_t dest, uint16_t src,
                                const struct SimdShrnSatParams& p,
                                uint64_t arm_pc);
    struct SimdShrnSatParams shrn_sat_params() const;
    static IRInst make_mul_elem(uint16_t dest, uint16_t src1, uint16_t src2,
                                const struct SimdMulElemParams& p,
                                uint64_t arm_pc);
    struct SimdMulElemParams mul_elem_params() const;
    static IRInst make_bf(IROp op, uint16_t dest, uint16_t src,
                          const struct BfParams& p, uint64_t arm_pc);
    struct BfParams bf_params() const;
    static IRInst make_sext(uint16_t dest, uint16_t src, uint8_t bits,
                            uint64_t arm_pc);
    uint8_t sext_bits() const;
    static IRInst make_zext(uint16_t dest, uint16_t src, uint8_t bits,
                            uint64_t arm_pc);
    uint8_t zext_bits() const;
    static IRInst make_csel(uint16_t dest, uint16_t src1, uint16_t src2,
                            const struct CselParams& p, uint64_t arm_pc);
    struct CselParams csel_params() const;
    static IRInst make_ccmp(uint16_t src1, uint16_t src2,
                            const struct CcmpParams& p, uint64_t arm_pc);
    struct CcmpParams ccmp_params() const;
    static IRInst make_addsub(IROp op, uint16_t dest, uint16_t src1,
                              uint16_t src2, const struct AddSubParams& p,
                              uint64_t arm_pc);
    struct AddSubParams addsub_params() const;
    static IRInst make_tst(uint16_t src1, uint16_t src2,
                           const struct TstParams& p, uint64_t arm_pc);
    struct TstParams tst_params() const;
    static IRInst make_gpr_shift(IROp op, uint16_t dest, uint16_t src1,
                                 uint16_t src2, const struct GprShiftParams& p,
                                 uint64_t arm_pc);
    struct GprShiftParams gpr_shift_params() const;
    static IRInst make_clz(uint16_t dest, uint16_t src,
                           const struct ClzParams& p, uint64_t arm_pc);
    struct ClzParams clz_params() const;
    static IRInst make_rev64(uint16_t dest, uint16_t src,
                             const struct ClzParams& p, uint64_t arm_pc);
    struct ClzParams rev64_params() const;
    static IRInst make_aes(uint16_t dest, uint16_t src1, uint16_t src2,
                           const struct AesParams& p, uint64_t arm_pc);
    struct AesParams aes_params() const;
    static IRInst make_load_mem(uint16_t dest, uint16_t base,
                                const struct MemParams& p, uint64_t arm_pc);
    struct MemParams load_mem_params() const;
    static IRInst make_store_mem(uint16_t base, uint16_t value,
                                 const struct MemParams& p, uint64_t arm_pc);
    struct MemParams store_mem_params() const;
    static IRInst make_atomic(uint16_t dest, uint16_t base, uint16_t operand,
                              const struct AtomicParams& p, uint64_t arm_pc);
    struct AtomicParams atomic_params() const;
    static IRInst make_ldst(uint16_t dest, uint16_t lo, uint16_t hi_or_zero,
                            const struct LdStParams& p, uint64_t arm_pc);
    bool ldst_is_load() const;
    static IRInst make_ld16(uint16_t dest, uint16_t base,
                            const struct Ld16Params& p, uint64_t arm_pc);
    struct Ld16Params ld16_params() const;
    static IRInst make_st16(uint16_t base, uint16_t src,
                            const struct St16Params& p, uint64_t arm_pc);
    struct St16Params st16_params() const;
    static IRInst make_div(IROp op, uint16_t dest, uint16_t src1,
                           uint16_t src2, const struct DivParams& p,
                           uint64_t arm_pc);
    uint8_t div_bits() const;
    static IRInst make_brcond(const struct BrCondParams& p, uint64_t arm_pc);
    struct BrCondParams brcond_params() const;
    static IRInst make_brcond_zero(uint16_t src,
                                   const struct BrCondZeroParams& p,
                                   uint64_t arm_pc);
    struct BrCondZeroParams brcond_zero_params() const;
    static IRInst make_brcond_bit(uint16_t src,
                                  const struct BrCondBitParams& p,
                                  uint64_t arm_pc);
    struct BrCondBitParams brcond_bit_params() const;
    static IRInst make_brcond_fallthru(uint64_t target, uint64_t arm_pc);
    struct BrCondFallthruParams fallthru_params() const;
    static IRInst make_brcond_skip(uint8_t cond, uint64_t arm_pc);
    struct BrCondSkipParams brcond_skip_params() const;
    static IRInst make_bl_call(uint64_t target, uint64_t arm_pc);
    struct BlParams bl_params() const;
    static IRInst make_load_reg(uint16_t dest, uint16_t src, bool is_fp,
                                uint64_t arm_pc = 0);
    bool is_fp_load() const;
    static IRInst make_store_reg(uint16_t dest, uint16_t src, bool is_fp,
                                 uint64_t arm_pc = 0);
    bool is_fp_store() const;
    static IRInst make_imm(uint16_t dest, uint64_t value,
                           uint64_t arm_pc = 0);
    uint64_t imm_value() const;
    uint8_t llsc_width() const;
    uint8_t csin_rd() const;
    uint8_t swar_rd() const;
    static IRInst make(IROp op, uint16_t dest = 0, uint16_t src1 = 0,
                       uint16_t src2 = 0, uint16_t aux = 0,
                       uint64_t arm_pc = 0);
    static IRInst make_mrs(uint16_t dest, uint64_t sys_idx, uint64_t arm_pc);
    uint64_t mrs_idx() const;
    static IRInst make_msr(uint16_t src, uint64_t sys_idx, uint64_t arm_pc);
    uint64_t msr_idx() const;
    uint64_t branch_target() const;
    void set_skip_count(uint64_t n);
    static IRInst make_fp_binop(uint16_t dest, uint16_t src1, uint16_t src2,
                                const struct FpBinopParams& p,
                                uint64_t arm_pc);
    struct FpBinopParams fp_binop_params() const;
    static IRInst make_fp_unop(uint16_t dest, uint16_t src,
                               const struct FpUnopParams& p, uint64_t arm_pc);
    struct FpUnopParams fp_unop_params() const;
    static IRInst make_fp_mov(uint16_t dest, uint16_t src,
                              const struct FpMovParams& p, uint64_t arm_pc);
    struct FpMovParams fp_mov_params() const;
    static IRInst make_fp_cmp(uint16_t dest, uint16_t src1, uint16_t src2,
                              const struct FpCmpParams& p, uint64_t arm_pc);
    struct FpCmpParams fp_cmp_params() const;
    static IRInst make_fp_movi(uint16_t dest, const struct FpMoviParams& p,
                               uint64_t arm_pc);
    struct FpMoviParams fp_movi_params() const;
    static IRInst make_fp_csel(uint16_t dest, uint16_t src1, uint16_t src2,
                               const struct FpCselParams& p, uint64_t arm_pc);
    struct FpCselParams fp_csel_params() const;
    static IRInst make_fp_f2i(uint16_t dest, uint16_t src,
                              const struct FpF2IParams& p, uint64_t arm_pc);
    struct FpF2IParams fp_f2i_params() const;
    static IRInst make_fp_i2f(uint16_t dest, uint16_t src,
                              const struct FpI2FParams& p, uint64_t arm_pc);
    struct FpI2FParams fp_i2f_params() const;
    static IRInst make_fp_f2i_fixed(uint16_t dest, uint16_t src,
                                    const struct FpFixedParams& p,
                                    uint64_t arm_pc);
    struct FpFixedParams fp_f2i_fixed_params() const;
    static IRInst make_fp_i2f_fixed(uint16_t dest, uint16_t src,
                                    const struct FpFixedParams& p,
                                    uint64_t arm_pc);
    struct FpFixedParams fp_i2f_fixed_params() const;
    static IRInst make_fp_frint(uint16_t dest, uint16_t src,
                                const struct FpFrintParams& p,
                                uint64_t arm_pc);
    struct FpFrintParams fp_frint_params() const;
    static IRInst make_fp_fused(IROp op, uint16_t dest, uint16_t src1,
                              uint16_t src2, const struct FpFusedParams& p,
                              uint64_t arm_pc);
    struct FpFusedParams fp_fused_params() const;
    static IRInst make_cmp(uint16_t dest, uint16_t src1, uint16_t src2,
                           const struct SimdSubopParams& p, uint64_t arm_pc);
    struct SimdSubopParams cmp_params() const;
    static IRInst make_fp_arith(uint16_t dest, uint16_t src1, uint16_t src2,
                                const struct SimdSubopParams& p,
                                uint64_t arm_pc);
    struct SimdSubopParams fp_arith_params() const;
    static IRInst make_fp_fma(uint16_t dest, uint16_t src1, uint16_t src2,
                              const struct SimdSubopParams& p,
                              uint64_t arm_pc);
    struct SimdSubopParams fp_fma_params() const;
    static IRInst make_sataddsub(uint16_t dest, uint16_t src1, uint16_t src2,
                                 const struct SimdSubopParams& p,
                                 uint64_t arm_pc);
    struct SimdSubopParams sataddsub_params() const;
    static IRInst make_abdl(uint16_t dest, uint16_t src1, uint16_t src2,
                            const struct SimdSubopParams& p, uint64_t arm_pc);
    struct SimdSubopParams abdl_params() const;
    static IRInst make_abd(uint16_t dest, uint16_t src1, uint16_t src2,
                           const struct SimdSubopParams& p, uint64_t arm_pc);
    struct SimdSubopParams abd_params() const;
    static IRInst make_addw(uint16_t dest, uint16_t src1, uint16_t src2,
                            const struct SimdSubopParams& p, uint64_t arm_pc);
    struct SimdSubopParams addw_params() const;
    static IRInst make_addhn(uint16_t dest, uint16_t src1, uint16_t src2,
                             const struct SimdSubopParams& p,
                             uint64_t arm_pc);
    struct SimdSubopParams addhn_params() const;
};
// Typed parameter structs, one per migrated op. Field names match the
// IROp comments so the contract reads the same on both sides.
struct SimdTblParams {
    uint8_t nregs = 1;  // 1 = TBL1/TBX1, 2 = TBL2/TBX2 (packed into imm)
    bool is_tbx = false;// packed into flags_op bit 1
    bool q = false;     // 0 = 64-bit operand, 1 = 128-bit (flags_op bit 0)
};
struct SimdInsParams {
    uint8_t esize = 1;  // element size in bytes: 1, 2, 4, 8 (width)
    uint8_t dst_off = 0;// destination element byte offset, didx*esize (imm)
    uint16_t sidx = 0;  // source element index (aux)
    bool q = false;     // 0 = 64-bit destination, 1 = 128-bit (flags_op)
};
// Shared parameter shape for the unary vector ops SIMD_2REG (CNT/NOT/
// RBIT/ABS/NEG), SIMD_CVTF (SCVTF/UCVTF/FCVTZS/FCVTZU) and SIMD_XTN
// (XTN/SQXTUN/SQXTN/UQXTN): imm=subop, width=esize, flags_op=Q.
// One struct for the shape, one factory+reader per op so the opcode
// stays pinned at the call site and the validator can range-check
// per-op subop values.
struct SimdSubopParams {
    uint8_t subop = 0;  // operation selector (packed into imm)
    uint8_t esize = 1;  // element size in bytes (packed into width)
    bool q = false;     // 0 = 64-bit operand, 1 = 128-bit (flags_op)
};
// Same shape but with a real src2 (PERMUTE/PAIRMIN take rn AND rm).
struct SimdBinopParams {
    uint8_t subop = 0;  // operation selector (packed into imm)
    uint8_t esize = 1;  // element size in bytes (packed into width)
    bool q = false;     // 0 = 64-bit operands, 1 = 128-bit (flags_op)
};
// ADDP's only varying parameter is Q (width=1, imm=0 always).
struct SimdAddpParams {
    bool q = false;     // 0 = 64-bit operands, 1 = 128-bit (flags_op)
};
// Integer SIMD arithmetic: subop + esize + Q (flags_op).
struct SimdArithParams {
    uint8_t subop = 0;  // 0=add,1=sub,2=mul,3=umin,4=umax,5=smin,6=smax
    uint8_t esize = 1;  // element size in bytes: 1, 2, 4, 8 (width)
    bool q = false;    // Q=0 computes the low half and clears the upper half.
};
// Bitwise logical: subop only (imm); width/cond/flags_op are always 0.
struct SimdLogicParams {
    uint8_t subop = 0;  // 0=and,1=orr,2=xor,3=bic,4=orn (5=eon, no row yet)
};
// Vector shift-by-immediate (SHL/USHR/SSHR/USRA/SSRA/SLI/SRI/URSRA/
// SRSRA): imm=shift amount, width=esize, flags_op=Q. The opcode itself
// selects the shift kind, so the factory takes it as a parameter —
// callers must pass one of the nine SIMD shift ops (the translator's
// subop switch does).
struct SimdShiftParams {
    uint8_t shift = 0;  // shift amount 0..esize*8 (packed into imm)
    uint8_t esize = 1;  // element size in bytes: 1, 2, 4, 8 (width)
    bool q = false;     // 0 = 64-bit operand, 1 = 128-bit (flags_op)
};
// Vector element -> GPR (UMOV zero-extending, SMOV sign-extending):
// width=esize, imm=lane index, flags_op=Q (0=Wd, 1=Xd).
struct SimdMovParams {
    uint8_t esize = 1;  // element size in bytes: 1, 2, 4, 8 (width)
    uint8_t index = 0;  // lane index (packed into imm)
    bool q = false;     // 0 = 32-bit GPR dest, 1 = 64-bit (flags_op)
};
// ORR/BIC immediate (read-modify-write dest): 64-bit lane pattern,
// invert flag (cond), Q.
struct SimdOrrImmParams {
    uint64_t pattern = 0;  // replicated lane pattern (packed into imm)
    bool invert = false;   // 0=ORR, 1=BIC (packed into cond)
    bool q = false;        // 0 = 64-bit dest, 1 = 128-bit (flags_op)
};
// MOVI/MVNI broadcast: 64-bit lane pattern + Q.
struct SimdMoviParams {
    uint64_t pattern = 0;  // replicated lane pattern (packed into imm)
    bool q = false;        // 0 = 64-bit dest, 1 = 128-bit (flags_op)
};
// GPR->vector broadcast (DUP): element size + Q (imm is always 0).
struct SimdDupParams {
    uint8_t esize = 8;  // element size in bytes: 1, 2, 4, 8 (width)
    bool q = false;     // 0 = 64-bit dest, 1 = 128-bit (flags_op)
};
// Saturating narrowing shift (SQSHRN family): imm=subop | (shift<<8),
// width=SOURCE esize, flags_op=Q.
struct SimdShrnSatParams {
    uint8_t subop = 0;  // 0=SQSHRN,1=UQSHRN,2=SQRSHRN,3=URQSHRN,4=SQSHRUN,
                        // 5=SQRSHRUN (imm bits 7:0)
    uint8_t shift = 1;  // 1..esize*8 (imm bits 15:8)
    uint8_t esize = 2;  // SOURCE element size in bytes (width)
    bool q = false;     // dest-half select (flags_op)
};
// Indexed-element multiply family: imm=subop | (lane<<8), width=SOURCE
// esize, flags_op=Q. src2 is the 4-bit Rm (not d.rm — see translator).
struct SimdMulElemParams {
    uint8_t subop = 0;  // 0=MUL..11=SQDMULL (imm bits 7:0)
    uint8_t lane = 0;   // indexed-element lane (imm bits 15:8)
    uint8_t esize = 1;  // SOURCE element size in bytes: 1, 2, 4 (width)
    bool q = false;     // (flags_op)
};
// Bitfield ops (SBFM/UBFM): immr/imms/sf only (width is always 0 —
// emit_bf never sets it and codegen derives 32/64 from sf). The opcode
// selects signed vs unsigned, so the factory takes it (like shifts).
struct BfParams {
    uint8_t immr = 0;  // rotate amount (immr)
    uint8_t imms = 0;  // field width selector (imms)
    bool sf = false;   // 1 = 64-bit, 0 = 32-bit (sf)
};
// Integer cond/select/compare/branch/sysreg ops.
struct CselParams {
    uint8_t cond = 0;    // ARM condition code (cond)
    uint8_t rd_slot = 0;  // rd, preserved verbatim (imm; codegen ignores)
};
struct CcmpParams {
    uint8_t nzcv = 0;   // NZCV value for cond-false (packed into WIDTH —
                        // NOT a byte size!)
    uint8_t cond = 0;   // ARM condition code (cond)
    bool is_sub = true;  // 1=CCMP(sub), 0=CCMN(add) (flags_op)
    bool sf = false;    // 1 = 64-bit compare, 0 = 32-bit (sf)
};
struct AddSubParams {
    uint8_t bits = 64;  // 32 or 64 BITS, not bytes (width)
    bool is_sub = false;  // 0=add, 1=sub — C inversion (flags_op)
};
struct TstParams {
    bool sf = false;  // 1 = 64-bit TEST, 0 = 32-bit (sf)
};
// GPR shifts (SHL/SHR/SAR/ROR): width selects the x86 form — 32 means
// the 32-bit form, anything else (0 or 64) means 64-bit. Producers use
// 0 and 64 interchangeably for 64-bit (preserved verbatim; a future
// cleanup could normalize 64→0 — all readers only test `== 32`).
struct GprShiftParams {
    uint8_t width = 0;  // 32 = 32-bit form, else 64-bit (width)
};
// Count-leading-zeros / byte-reverse-64: width=32/64 BITS, imm=rd slot
// (preserved verbatim; codegen ignores it).
struct ClzParams {
    uint8_t bits = 64;    // 32 or 64 BITS, not bytes (width)
    uint8_t rd_slot = 0;  // rd (imm; codegen ignores)
};
// Crypto extensions (AESE/AESD/AESMC/AESIMC/PMULL/PMULL2): imm=subop
// only (0..5); src2 is the key (AES) or second operand (PMULL), 0 for
// the subop-0 group which derives the variant from the encoding.
struct AesParams {
    uint8_t subop = 0;  // 0=AESE-group,1=AESD,2=AESMC,3=AESIMC,4=PMULL,5=PMULL2 (imm)
};
// Guest memory access: width=1/2/4/8 bytes, imm=byte offset.
struct MemParams {
    uint8_t width = 8;    // access size in bytes: 1, 2, 4, 8 (width)
    uint64_t offset = 0;  // byte offset added to base (imm)
};
// LSE atomics: width=1/2/4/8 bytes, cond=atom op, flags=is_load,
// imm=ARM reg index (rt, or rs for CAS).
struct AtomicParams {
    uint8_t width = 8;    // access size in bytes: 1, 2, 4, 8 (width)
    uint8_t atom_op = 0;  // 0=LDADD..8=SWP, 0xC-0xF=CAS (cond)
    bool is_load = true;  // return old value (flags_op)
    uint8_t reg_idx = 0;  // ARM reg: rt (rs for CAS) (imm)
};
// 128-bit vector load/store pair helper (LDST): width=1 load / 0 store.
struct LdStParams {
    bool is_load = true;  // 1=load (width=1), 0=store (width=0)
};
// 16-byte vector load: flags_op=register count, imm=byte offset
// (always 0 from current producers; codegen supports nonzero).
struct Ld16Params {
    uint8_t count = 1;    // registers 1..4 (flags_op)
    uint64_t offset = 0;  // byte offset added to base (imm)
};
// 16-byte vector store: flags_op=register count, cond=broadcast,
// imm=byte offset (always 0 from current producers).
struct St16Params {
    uint8_t count = 1;    // registers 1..4 (flags_op)
    bool broadcast = false;  // every half stores src2 (cond)
    uint64_t offset = 0;  // byte offset added to base (imm)
};
// Native division (UDIV/SDIV): width=32/64 BITS only.
struct DivParams {
    uint8_t bits = 64;  // 32 or 64 BITS, not bytes (width)
};
struct BrCondParams {
    uint8_t cond = 0;     // ARM condition code (cond)
    uint64_t target = 0;  // branch target pc (imm)
};
struct BrCondZeroParams {
    uint8_t cond = 0;     // 0=EQ(CBZ), 1=NE(CBNZ) (cond)
    uint64_t target = 0;  // branch target pc (imm)
    bool sf = false;      // W-form tests low 32 bits only (sf)
};
struct BrCondBitParams {
    uint8_t bit = 0;      // tested bit number 0..63 (width)
    uint8_t cond = 0;     // 0=EQ(TBZ), 1=NE(TBNZ) (cond)
    uint64_t target = 0;  // branch target pc (imm)
};
struct BrCondSkipParams {
    uint8_t cond = 0;     // ARM condition code (cond)
    uint64_t count = 0;   // region op count (imm; patched post-emit)
};
// Scalar FP ops. width here is the ftype flag (0=S single, 1=D double),
// NOT byte size — kept as `ftype` in every struct below.
struct FpBinopParams {
    uint8_t opcode = 0;  // 0=mul,1=div,2=add,3=sub,4=max,5=min,6=nmul (imm)
    uint8_t ftype = 0;   // 0=S, 1=D (width); flags_op is always 0
};
struct FpUnopParams {
    uint8_t opcode = 0;  // 0=mov,1=abs,2=neg,3=sqrt (imm)
    uint8_t ftype = 0;   // 0=S, 1=D (width)
};
struct FpMovParams {
    uint8_t ftype = 0;   // 0=S, 1=D (width); the only parameter
};
struct FpCmpParams {
    uint8_t ftype = 0;   // 0=S, 1=D (width)
    bool with_zero = false;  // 1=FCMP Dn,#0.0 form (imm)
};
struct FpMoviParams {
    uint8_t ftype = 0;   // 0=S, 1=D (width)
    uint64_t bits = 0;   // decoded FP immediate bits (imm)
};
struct FpCselParams {
    uint8_t ftype = 0;   // 0=S, 1=D (width)
    uint8_t cond = 0;    // ARM condition code (cond)
};
struct FpF2IParams {
    uint8_t ftype = 0;      // 0=S, 1=D (width)
    uint8_t rounding = 3;   // (is_away<<2)|rmode (cond)
    bool sf = false;        // 64-bit dest (flags_op)
    bool is_unsigned = false;  // (imm)
};
struct FpI2FParams {
    uint8_t ftype = 0;      // 0=S, 1=D (width)
    bool sf = false;        // 64-bit GPR source (flags_op)
    bool is_unsigned = false;  // (imm)
};
struct FpFixedParams {
    uint8_t w = 0;          // width as passed by the translator (width)
    bool is_unsigned = false;  // (imm)
    bool sf = false;        // (flags_op)
    uint8_t fbits = 0;      // scale: 0=integer form, else 64-scale (immr)
    bool fp_reg = false;    // FP-reg source/dest (imms)
};
struct FpFrintParams {
    uint8_t bits = 32;  // 32 or 64 BITS, not bytes (width)
    uint8_t mode = 0;   // 0=N,1=P,2=M,3=Z,4=I,5=X (imm)
};
// Fused multiply-add family (FMADD/FMSUB/FNMADD/FNMSUB): width=32/64
// BITS, imm=accumulator FP-reg index (Va). The opcode selects the form.
struct FpFusedParams {
    uint8_t bits = 32;  // 32 or 64 BITS, not bytes (width)
    uint8_t acc = 0;    // accumulator FP register index (imm)
};
inline IRInst IRInst::make_tbl(uint16_t dest, uint16_t table, uint16_t index,
                               const SimdTblParams& p, uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::SIMD_TBL;
    inst.dest = dest;
    inst.src1 = table;
    inst.src2 = index;
    inst.flags_op = static_cast<uint8_t>((p.is_tbx ? 2 : 0) | (p.q ? 1 : 0));
    inst.imm = p.nregs;
    inst.arm_pc = arm_pc;
    return inst;
}
inline SimdTblParams IRInst::tbl_params() const {
    SimdTblParams p;
    p.is_tbx = (flags_op & 2) != 0;
    p.q = (flags_op & 1) != 0;
    p.nregs = static_cast<uint8_t>(imm);
    return p;
}
inline IRInst IRInst::make_ins(uint16_t dest, uint16_t src_vec,
                               uint16_t rmw_dest, const SimdInsParams& p,
                               uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::SIMD_INS;
    inst.dest = dest;
    inst.src1 = src_vec;
    inst.src2 = rmw_dest;
    inst.width = p.esize;
    inst.imm = p.dst_off;
    inst.aux = p.sidx;
    inst.flags_op = p.q ? 1 : 0;
    inst.arm_pc = arm_pc;
    return inst;
}
inline SimdInsParams IRInst::ins_params() const {
    SimdInsParams p;
    p.esize = width;
    p.dst_off = static_cast<uint8_t>(imm);
    p.sidx = aux;
    p.q = (flags_op & 1) != 0;
    return p;
}
// Shared shape for the unary vector ops (2REG/CVTF/XTN): imm=subop,
// width=esize, flags_op=Q. One struct, one factory+reader per op so the
// opcode stays pinned at the call site.
inline IRInst IRInst::make_2reg(uint16_t dest, uint16_t src,
                                const SimdSubopParams& p, uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::SIMD_2REG;
    inst.dest = dest;
    inst.src1 = src;
    inst.width = p.esize;
    inst.flags_op = p.q ? 1 : 0;
    inst.imm = p.subop;
    inst.arm_pc = arm_pc;
    return inst;
}
inline SimdSubopParams IRInst::p2reg_params() const {
    return SimdSubopParams{static_cast<uint8_t>(imm), width,
                           (flags_op & 1) != 0};
}
inline IRInst IRInst::make_cvtf(uint16_t dest, uint16_t src,
                                const SimdSubopParams& p, uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::SIMD_CVTF;
    inst.dest = dest;
    inst.src1 = src;
    inst.width = p.esize;
    inst.flags_op = p.q ? 1 : 0;
    inst.imm = p.subop;
    inst.arm_pc = arm_pc;
    return inst;
}
inline SimdSubopParams IRInst::cvtf_params() const {
    return SimdSubopParams{static_cast<uint8_t>(imm), width,
                           (flags_op & 1) != 0};
}
inline IRInst IRInst::make_xtn(uint16_t dest, uint16_t src,
                               const SimdSubopParams& p, uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::SIMD_XTN;
    inst.dest = dest;
    inst.src1 = src;
    inst.width = p.esize;
    inst.flags_op = p.q ? 1 : 0;
    inst.imm = p.subop;
    inst.arm_pc = arm_pc;
    return inst;
}
inline SimdSubopParams IRInst::xtn_params() const {
    return SimdSubopParams{static_cast<uint8_t>(imm), width,
                           (flags_op & 1) != 0};
}
// Binary-vector variant of the subop shape (PERMUTE/PAIRMIN): like
// SimdSubopParams but with a real src2 (rm) instead of src2=0.
inline IRInst IRInst::make_permute(uint16_t dest, uint16_t src1,
                                   uint16_t src2, const SimdBinopParams& p,
                                   uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::SIMD_PERMUTE;
    inst.dest = dest;
    inst.src1 = src1;
    inst.src2 = src2;
    inst.width = p.esize;
    inst.flags_op = p.q ? 1 : 0;
    inst.imm = p.subop;
    inst.arm_pc = arm_pc;
    return inst;
}
inline SimdBinopParams IRInst::permute_params() const {
    return SimdBinopParams{static_cast<uint8_t>(imm), width,
                           (flags_op & 1) != 0};
}
inline IRInst IRInst::make_pairmin(uint16_t dest, uint16_t src1,
                                   uint16_t src2, const SimdBinopParams& p,
                                   uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::SIMD_PAIRMIN;
    inst.dest = dest;
    inst.src1 = src1;
    inst.src2 = src2;
    inst.width = p.esize;
    inst.flags_op = p.q ? 1 : 0;
    inst.imm = p.subop;
    inst.arm_pc = arm_pc;
    return inst;
}
inline SimdBinopParams IRInst::pairmin_params() const {
    return SimdBinopParams{static_cast<uint8_t>(imm), width,
                           (flags_op & 1) != 0};
}
// ADDP (byte-pair pairwise add) has no subop or esize: width is always 1,
// imm is always 0, only Q varies. The factory hardcodes the constants so
// no call site can get them wrong.
inline IRInst IRInst::make_addp(uint16_t dest, uint16_t src1, uint16_t src2,
                                const SimdAddpParams& p, uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::SIMD_ADDP;
    inst.dest = dest;
    inst.src1 = src1;
    inst.src2 = src2;
    inst.width = 1;
    inst.flags_op = p.q ? 1 : 0;
    inst.imm = 0;
    inst.arm_pc = arm_pc;
    return inst;
}
inline SimdAddpParams IRInst::addp_params() const {
    return SimdAddpParams{(flags_op & 1) != 0};
}
// Integer SIMD arithmetic: imm=subop, width=esize, flags_op=Q.
inline IRInst IRInst::make_arith(uint16_t dest, uint16_t src1, uint16_t src2,
                                 const SimdArithParams& p, uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::SIMD_ARITH;
    inst.dest = dest;
    inst.src1 = src1;
    inst.src2 = src2;
    inst.width = p.esize;
    inst.flags_op = p.q ? 1 : 0;
    inst.imm = p.subop;
    inst.arm_pc = arm_pc;
    return inst;
}
inline SimdArithParams IRInst::arith_params() const {
    return SimdArithParams{static_cast<uint8_t>(imm), width, (flags_op & 1) != 0};
}
// Bitwise logical: ONLY subop varies (imm); width/cond/flags_op are
// always 0. The factory hardcodes the constants.
inline IRInst IRInst::make_logical(uint16_t dest, uint16_t src1,
                                   uint16_t src2, const SimdLogicParams& p,
                                   uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::SIMD_LOGICAL;
    inst.dest = dest;
    inst.src1 = src1;
    inst.src2 = src2;
    inst.width = 0;
    inst.cond = 0;
    inst.flags_op = 0;
    inst.imm = p.subop;
    inst.arm_pc = arm_pc;
    return inst;
}
inline SimdLogicParams IRInst::logical_params() const {
    return SimdLogicParams{static_cast<uint8_t>(imm)};
}
inline IRInst IRInst::make_shift(IROp op, uint16_t dest, uint16_t src,
                                 const SimdShiftParams& p, uint64_t arm_pc) {
    IRInst inst{};
    inst.op = op;
    inst.dest = dest;
    inst.src1 = src;
    inst.width = p.esize;
    inst.flags_op = p.q ? 1 : 0;
    inst.imm = p.shift;
    inst.arm_pc = arm_pc;
    return inst;
}
inline SimdShiftParams IRInst::shift_params() const {
    return SimdShiftParams{static_cast<uint8_t>(imm), width,
                           (flags_op & 1) != 0};
}
// Vector element -> GPR (UMOV/SMOV): width=esize, imm=lane index,
// flags_op=Q. dest is the GPR (rd), src1 the vector (rn).
inline IRInst IRInst::make_umov(uint16_t dest, uint16_t src_vec,
                                const SimdMovParams& p, uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::SIMD_UMOV;
    inst.dest = dest;
    inst.src1 = src_vec;
    inst.width = p.esize;
    inst.flags_op = p.q ? 1 : 0;
    inst.imm = p.index;
    inst.arm_pc = arm_pc;
    return inst;
}
inline SimdMovParams IRInst::umov_params() const {
    return SimdMovParams{width, static_cast<uint8_t>(imm),
                         (flags_op & 1) != 0};
}
inline IRInst IRInst::make_smov(uint16_t dest, uint16_t src_vec,
                                const SimdMovParams& p, uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::SIMD_SMOV;
    inst.dest = dest;
    inst.src1 = src_vec;
    inst.width = p.esize;
    inst.flags_op = p.q ? 1 : 0;
    inst.imm = p.index;
    inst.arm_pc = arm_pc;
    return inst;
}
inline SimdMovParams IRInst::smov_params() const {
    return SimdMovParams{width, static_cast<uint8_t>(imm),
                         (flags_op & 1) != 0};
}
// ORR/BIC immediate (read-modify-write dest): imm=64-bit lane pattern,
// cond=invert (0=ORR,1=BIC), flags_op=Q, width is always 0.
inline IRInst IRInst::make_orrimm(uint16_t dest, const SimdOrrImmParams& p,
                                  uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::SIMD_ORRIMM;
    inst.dest = dest;
    inst.width = 0;
    inst.cond = p.invert ? 1 : 0;
    inst.flags_op = p.q ? 1 : 0;
    inst.imm = p.pattern;
    inst.arm_pc = arm_pc;
    return inst;
}
inline SimdOrrImmParams IRInst::orrimm_params() const {
    return SimdOrrImmParams{imm, (cond & 1) != 0, (flags_op & 1) != 0};
}
// MOVI/MVNI broadcast: imm=64-bit lane pattern, flags_op=Q,
// width/cond are always 0.
inline IRInst IRInst::make_movi(uint16_t dest, const SimdMoviParams& p,
                                uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::SIMD_MOVI;
    inst.dest = dest;
    inst.width = 0;
    inst.cond = 0;
    inst.flags_op = p.q ? 1 : 0;
    inst.imm = p.pattern;
    inst.arm_pc = arm_pc;
    return inst;
}
inline SimdMoviParams IRInst::movi_params() const {
    return SimdMoviParams{imm, (flags_op & 1) != 0};
}
// GPR->vector broadcast (DUP): width=esize bytes, flags_op=Q, imm is
// always 0. Both producers (2d form and general form) fit.
inline IRInst IRInst::make_dup(uint16_t dest, uint16_t src,
                               const SimdDupParams& p, uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::SIMD_DUP;
    inst.dest = dest;
    inst.src1 = src;
    inst.width = p.esize;
    inst.flags_op = p.q ? 1 : 0;
    inst.imm = 0;
    inst.arm_pc = arm_pc;
    return inst;
}
inline SimdDupParams IRInst::dup_params() const {
    return SimdDupParams{width, (flags_op & 1) != 0};
}
inline IRInst IRInst::make_shrn_sat(uint16_t dest, uint16_t src,
                                    const SimdShrnSatParams& p,
                                    uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::SIMD_SHRN_SAT;
    inst.dest = dest;
    inst.src1 = src;
    inst.width = p.esize;
    inst.flags_op = p.q ? 1 : 0;
    inst.imm = (uint64_t)p.subop | ((uint64_t)p.shift << 8);
    inst.arm_pc = arm_pc;
    return inst;
}
inline SimdShrnSatParams IRInst::shrn_sat_params() const {
    return SimdShrnSatParams{static_cast<uint8_t>(imm & 0xFF),
                             static_cast<uint8_t>((imm >> 8) & 0xFF),
                             width, (flags_op & 1) != 0};
}
inline IRInst IRInst::make_mul_elem(uint16_t dest, uint16_t src1,
                                    uint16_t src2,
                                    const SimdMulElemParams& p,
                                    uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::SIMD_MUL_ELEM;
    inst.dest = dest;
    inst.src1 = src1;
    inst.src2 = src2;
    inst.width = p.esize;
    inst.flags_op = p.q ? 1 : 0;
    inst.imm = (uint64_t)p.subop | ((uint64_t)p.lane << 8);
    inst.arm_pc = arm_pc;
    return inst;
}
inline SimdMulElemParams IRInst::mul_elem_params() const {
    return SimdMulElemParams{static_cast<uint8_t>(imm & 0xFF),
                             static_cast<uint8_t>((imm >> 8) & 0xFF),
                             width, (flags_op & 1) != 0};
}
inline IRInst IRInst::make_bf(IROp op, uint16_t dest, uint16_t src,
                              const BfParams& p, uint64_t arm_pc) {
    IRInst inst{};
    inst.op = op;
    inst.dest = dest;
    inst.src1 = src;
    inst.immr = p.immr;
    inst.imms = p.imms;
    inst.sf = p.sf ? 1 : 0;
    inst.arm_pc = arm_pc;
    return inst;
}
inline BfParams IRInst::bf_params() const {
    return BfParams{immr, imms, sf != 0};
}
inline IRInst IRInst::make_sext(uint16_t dest, uint16_t src, uint8_t bits,
                                uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::SEXT;
    inst.dest = dest;
    inst.src1 = src;
    inst.width = bits;
    inst.arm_pc = arm_pc;
    return inst;
}
inline uint8_t IRInst::sext_bits() const {
    return width;
}
inline IRInst IRInst::make_zext(uint16_t dest, uint16_t src, uint8_t bits,
                                uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::ZEXT;
    inst.dest = dest;
    inst.src1 = src;
    inst.width = bits;
    inst.arm_pc = arm_pc;
    return inst;
}
inline uint8_t IRInst::zext_bits() const {
    return width;
}
// Conditional select: cond=ARM condition, imm=rd slot (preserved verbatim;
// codegen ignores it), width/flags_op are always 0. The translator
// decomposes CSINC/CSINV/CSNEG into plain CSEL, so only CSEL is emitted.
inline IRInst IRInst::make_csel(uint16_t dest, uint16_t src1, uint16_t src2,
                                const CselParams& p, uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::CSEL;
    inst.dest = dest;
    inst.src1 = src1;
    inst.src2 = src2;
    inst.width = 0;
    inst.cond = p.cond;
    inst.flags_op = 0;
    inst.imm = p.rd_slot;
    inst.arm_pc = arm_pc;
    return inst;
}
inline CselParams IRInst::csel_params() const {
    return CselParams{cond, static_cast<uint8_t>(imm)};
}
// Conditional compare: width=NZCV value (4 bits — NOT a byte size!),
// cond=ARM condition, flags_op=is_sub (1=CCMP, 0=CCMN), imm always 0.
inline IRInst IRInst::make_ccmp(uint16_t src1, uint16_t src2,
                                const CcmpParams& p, uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::CCMP;
    inst.dest = 0;
    inst.src1 = src1;
    inst.src2 = src2;
    inst.width = p.nzcv;
    inst.cond = p.cond;
    inst.flags_op = p.is_sub ? 1 : 0;
    inst.imm = 0;
    inst.sf = p.sf ? 1 : 0;
    inst.arm_pc = arm_pc;
    return inst;
}
inline CcmpParams IRInst::ccmp_params() const {
    return CcmpParams{static_cast<uint8_t>(width & 0xF), cond,
                      (flags_op & 1) != 0, (sf & 1) != 0};
}
// Flag-setting ALU (ADDS/SUBS/ADCS/SBCS): width=32/64 BITS (not bytes!),
// flags_op=is_sub (0=add, 1=sub — controls C inversion). The opcode
// selects the operation; one factory takes it (like shifts).
inline IRInst IRInst::make_addsub(IROp op, uint16_t dest, uint16_t src1,
                                  uint16_t src2, const AddSubParams& p,
                                  uint64_t arm_pc) {
    IRInst inst{};
    inst.op = op;
    inst.dest = dest;
    inst.src1 = src1;
    inst.src2 = src2;
    inst.width = p.bits;
    inst.cond = 0;
    inst.flags_op = p.is_sub ? 1 : 0;
    inst.imm = 0;
    inst.arm_pc = arm_pc;
    return inst;
}
inline AddSubParams IRInst::addsub_params() const {
    return AddSubParams{width, (flags_op & 1) != 0};
}
// TST (ANDS flag-set, no dest): sf only. Set directly by the factory —
// the old post-hoc `insts.back().sf` patching is gone.
inline IRInst IRInst::make_tst(uint16_t src1, uint16_t src2,
                               const TstParams& p, uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::TST;
    inst.dest = 0;
    inst.src1 = src1;
    inst.src2 = src2;
    inst.sf = p.sf ? 1 : 0;
    inst.arm_pc = arm_pc;
    return inst;
}
inline TstParams IRInst::tst_params() const {
    return TstParams{(sf & 1) != 0};
}
inline IRInst IRInst::make_gpr_shift(IROp op, uint16_t dest, uint16_t src1,
                                     uint16_t src2, const GprShiftParams& p,
                                     uint64_t arm_pc) {
    IRInst inst{};
    inst.op = op;
    inst.dest = dest;
    inst.src1 = src1;
    inst.src2 = src2;
    inst.width = p.width;
    inst.arm_pc = arm_pc;
    return inst;
}
inline GprShiftParams IRInst::gpr_shift_params() const {
    return GprShiftParams{width};
}
inline IRInst IRInst::make_clz(uint16_t dest, uint16_t src,
                               const ClzParams& p, uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::CLZ;
    inst.dest = dest;
    inst.src1 = src;
    inst.width = p.bits;
    inst.imm = p.rd_slot;
    inst.arm_pc = arm_pc;
    return inst;
}
inline ClzParams IRInst::clz_params() const {
    return ClzParams{width, static_cast<uint8_t>(imm)};
}
inline IRInst IRInst::make_rev64(uint16_t dest, uint16_t src,
                                 const ClzParams& p, uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::REV64;
    inst.dest = dest;
    inst.src1 = src;
    inst.width = p.bits;
    inst.imm = p.rd_slot;
    inst.arm_pc = arm_pc;
    return inst;
}
inline ClzParams IRInst::rev64_params() const {
    return ClzParams{width, static_cast<uint8_t>(imm)};
}
inline IRInst IRInst::make_aes(uint16_t dest, uint16_t src1, uint16_t src2,
                               const AesParams& p, uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::AES_CRYPTO;
    inst.dest = dest;
    inst.src1 = src1;
    inst.src2 = src2;
    inst.imm = p.subop;
    inst.arm_pc = arm_pc;
    return inst;
}
inline AesParams IRInst::aes_params() const {
    return AesParams{static_cast<uint8_t>(imm)};
}
inline IRInst IRInst::make_load_mem(uint16_t dest, uint16_t base,
                                    const MemParams& p, uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::LOAD_MEM;
    inst.dest = dest;
    inst.src1 = base;
    inst.src2 = 0;
    inst.width = p.width;
    inst.imm = p.offset;
    inst.arm_pc = arm_pc;
    return inst;
}
inline MemParams IRInst::load_mem_params() const {
    return MemParams{width, imm};
}
inline IRInst IRInst::make_store_mem(uint16_t base, uint16_t value,
                                     const MemParams& p, uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::STORE_MEM;
    inst.dest = 0;
    inst.src1 = base;
    inst.src2 = value;
    inst.width = p.width;
    inst.imm = p.offset;
    inst.arm_pc = arm_pc;
    return inst;
}
inline MemParams IRInst::store_mem_params() const {
    return MemParams{width, imm};
}
inline IRInst IRInst::make_atomic(uint16_t dest, uint16_t base,
                                  uint16_t operand, const AtomicParams& p,
                                  uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::ATOMIC;
    inst.dest = dest;
    inst.src1 = base;
    inst.src2 = operand;
    inst.width = p.width;
    inst.cond = p.atom_op;
    inst.flags_op = p.is_load ? 1 : 0;
    inst.imm = p.reg_idx;
    inst.arm_pc = arm_pc;
    return inst;
}
inline AtomicParams IRInst::atomic_params() const {
    return AtomicParams{width, cond, (flags_op & 1) != 0,
                        static_cast<uint8_t>(imm)};
}
inline IRInst IRInst::make_ldst(uint16_t dest, uint16_t lo,
                                uint16_t hi_or_zero, const LdStParams& p,
                                uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::SIMD_LDST;
    inst.dest = dest;
    inst.src1 = lo;
    inst.src2 = hi_or_zero;
    inst.width = p.is_load ? 1 : 0;
    inst.arm_pc = arm_pc;
    return inst;
}
inline bool IRInst::ldst_is_load() const {
    return (width & 1) != 0;
}
inline IRInst IRInst::make_ld16(uint16_t dest, uint16_t base,
                                const Ld16Params& p, uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::SIMD_LD16;
    inst.dest = dest;
    inst.src1 = base;
    inst.src2 = 0;
    inst.width = 0;
    inst.cond = 0;
    inst.flags_op = p.count;
    inst.imm = p.offset;
    inst.arm_pc = arm_pc;
    return inst;
}
inline Ld16Params IRInst::ld16_params() const {
    return Ld16Params{static_cast<uint8_t>(flags_op), imm};
}
inline IRInst IRInst::make_st16(uint16_t base, uint16_t src,
                                const St16Params& p, uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::SIMD_ST16;
    inst.dest = 0;
    inst.src1 = base;
    inst.src2 = src;
    inst.width = 0;
    inst.cond = p.broadcast ? 1 : 0;
    inst.flags_op = p.count;
    inst.imm = p.offset;
    inst.arm_pc = arm_pc;
    return inst;
}
inline St16Params IRInst::st16_params() const {
    return St16Params{static_cast<uint8_t>(flags_op), (cond & 1) != 0, imm};
}
inline IRInst IRInst::make_div(IROp op, uint16_t dest, uint16_t src1,
                               uint16_t src2, const DivParams& p,
                               uint64_t arm_pc) {
    IRInst inst{};
    inst.op = op;
    inst.dest = dest;
    inst.src1 = src1;
    inst.src2 = src2;
    inst.width = p.bits;
    inst.arm_pc = arm_pc;
    return inst;
}
inline uint8_t IRInst::div_bits() const {
    return width;
}
// Conditional branch to target: cond=ARM condition, imm=target pc.
// dest/src are always 0 (no register operands).
inline IRInst IRInst::make_brcond(const BrCondParams& p, uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::BRCOND;
    inst.dest = 0;
    inst.src1 = 0;
    inst.src2 = 0;
    inst.width = 0;
    inst.cond = p.cond;
    inst.flags_op = 0;
    inst.imm = p.target;
    inst.arm_pc = arm_pc;
    return inst;
}
inline BrCondParams IRInst::brcond_params() const {
    return BrCondParams{cond, imm};
}
// CBZ/CBNZ: cond=EQ/NE, imm=target, sf=W-form flag (was post-hoc patched).
inline IRInst IRInst::make_brcond_zero(uint16_t src, const BrCondZeroParams& p,
                                       uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::BRCOND_ZERO;
    inst.dest = 0;
    inst.src1 = src;
    inst.width = 0;
    inst.cond = p.cond;
    inst.flags_op = 0;
    inst.imm = p.target;
    inst.sf = p.sf ? 1 : 0;
    inst.arm_pc = arm_pc;
    return inst;
}
inline BrCondZeroParams IRInst::brcond_zero_params() const {
    return BrCondZeroParams{cond, imm, (sf & 1) != 0};
}
// TBZ/TBNZ: width=bit number, cond=EQ/NE, imm=target.
inline IRInst IRInst::make_brcond_bit(uint16_t src, const BrCondBitParams& p,
                                      uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::BRCOND_BIT;
    inst.dest = 0;
    inst.src1 = src;
    inst.width = p.bit;
    inst.cond = p.cond;
    inst.flags_op = 0;
    inst.imm = p.target;
    inst.arm_pc = arm_pc;
    return inst;
}
inline BrCondBitParams IRInst::brcond_bit_params() const {
    return BrCondBitParams{width, cond, imm};
}
// Unconditional in-block branch (B/BL fallthrough): cond is always AL,
// imm=target.
struct BrCondFallthruParams {
    uint64_t target = 0;  // branch target pc (imm)
};
inline IRInst IRInst::make_brcond_fallthru(uint64_t target, uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::BRCOND_FALLTHRU;
    inst.dest = 0;
    inst.src1 = 0;
    inst.src2 = 0;
    inst.width = 0;
    inst.cond = 14;  // AL
    inst.flags_op = 0;
    inst.imm = target;
    inst.arm_pc = arm_pc;
    return inst;
}
inline BrCondFallthruParams IRInst::fallthru_params() const {
    return BrCondFallthruParams{imm};
}
// Mid-block conditional skip (leaf inlining): cond=ARM condition,
// imm=region op count (patched after translation via set_skip_count).
inline IRInst IRInst::make_brcond_skip(uint8_t cond, uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::BRCOND_SKIP;
    inst.dest = 0;
    inst.src1 = 0;
    inst.src2 = 0;
    inst.width = 0;
    inst.cond = cond;
    inst.flags_op = 0;
    inst.imm = 0;  // forward reference — patched by set_skip_count
    inst.arm_pc = arm_pc;
    return inst;
}
inline BrCondSkipParams IRInst::brcond_skip_params() const {
    return BrCondSkipParams{cond, imm};
}
// BL within block: imm=target pc.
struct BlParams {
    uint64_t target = 0;  // call target pc (imm)
};
inline IRInst IRInst::make_bl_call(uint64_t target, uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::BL_CALL;
    inst.dest = 0;
    inst.src1 = 0;
    inst.src2 = 0;
    inst.width = 0;
    inst.cond = 0;
    inst.flags_op = 0;
    inst.imm = target;
    inst.arm_pc = arm_pc;
    return inst;
}
inline BlParams IRInst::bl_params() const {
    return BlParams{imm};
}
// GPR/FP register file access (LOAD_REG/STORE_REG): sf selects the file
// (0=cpu.regs GPR, 1=cpu.v_lo FP). Single-bit param, bool directly.
inline IRInst IRInst::make_load_reg(uint16_t dest, uint16_t src, bool is_fp,
                                    uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::LOAD_REG;
    inst.dest = dest;
    inst.src1 = src;
    inst.sf = is_fp ? 1 : 0;
    inst.arm_pc = arm_pc;
    return inst;
}
inline bool IRInst::is_fp_load() const {
    return (sf & 1) != 0;
}
inline IRInst IRInst::make_store_reg(uint16_t dest, uint16_t src, bool is_fp,
                                     uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::STORE_REG;
    inst.dest = dest;
    inst.src1 = src;
    inst.sf = is_fp ? 1 : 0;
    inst.arm_pc = arm_pc;
    return inst;
}
inline bool IRInst::is_fp_store() const {
    return (sf & 1) != 0;
}
// Immediate constant: imm=value (the only payload).
inline IRInst IRInst::make_imm(uint16_t dest, uint64_t value,
                               uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::IMM;
    inst.dest = dest;
    inst.imm = value;
    inst.arm_pc = arm_pc;
    return inst;
}
inline uint64_t IRInst::imm_value() const {
    return imm;
}
// Narrow readers for DEFENSIVE-DEAD codegen cases (no producer emits
// these ops today; the cases exist as fallback). They need typed access
// for the privatized build; if a producer is ever added, promote the op
// to a full param struct + factory + validator case.
inline uint8_t IRInst::llsc_width() const {
    return width;  // LDXR_FAST/STXR_FAST/STLR_FAST access size
}
inline uint8_t IRInst::csin_rd() const {
    return static_cast<uint8_t>(imm);  // CSINC/CSINV/CSNEG rd slot
}
// SWAR-decomposed fallbacks (RBIT/REV16/CLS — never emitted; the
// translator decomposes them inline): imm=rd slot for the ARM reload.
inline uint8_t IRInst::swar_rd() const {
    return static_cast<uint8_t>(imm);
}
// Generic constructor for PARAMETER-LESS ops (pure dataflow: ADD/SUB/
// AND/OR/NOT/MOV/CALL_INTERP/SVC/BR/...). Sets no param fields — any op
// carrying params MUST use its typed factory above. This is what the
// old free emit() becomes after privatization.
inline IRInst IRInst::make(IROp op, uint16_t dest, uint16_t src1,
                           uint16_t src2, uint16_t aux, uint64_t arm_pc) {
    IRInst inst{};
    inst.op = op;
    inst.dest = dest;
    inst.src1 = src1;
    inst.src2 = src2;
    inst.aux = aux;
    inst.arm_pc = arm_pc;
    return inst;
}
// System register access: imm=sysreg index (up to 21 bits: op1:crn:crm:
// op2:op0 — NOT a byte; the factory takes the full value).
inline IRInst IRInst::make_mrs(uint16_t dest, uint64_t sys_idx,
                               uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::MRS;
    inst.dest = dest;
    inst.imm = sys_idx;
    inst.arm_pc = arm_pc;
    return inst;
}
inline uint64_t IRInst::mrs_idx() const {
    return imm;
}
inline IRInst IRInst::make_msr(uint16_t src, uint64_t sys_idx,
                               uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::MSR;
    inst.dest = 0;
    inst.src1 = src;
    inst.imm = sys_idx;
    inst.arm_pc = arm_pc;
    return inst;
}
inline uint64_t IRInst::msr_idx() const {
    return imm;
}
// Block-ending branch target for dispatch logic (self-loop detection).
// Covers every op that carries a target pc in imm; returns 0 otherwise.
// NOTE: BRCOND_SKIP is deliberately excluded — its imm is a region op
// count, not a pc (the sole caller is gated to BRCOND/ZERO/BIT anyway).
inline uint64_t IRInst::branch_target() const {
    switch (op) {
        case IROp::BRCOND:
        case IROp::BRCOND_ZERO:
        case IROp::BRCOND_BIT:
        case IROp::BRCOND_FALLTHRU:
        case IROp::BL_CALL:
            return imm;
        default:
            return 0;
    }
}
// Forward-reference patch for BRCOND_SKIP's region op count. The ONLY
// sanctioned post-emit mutation (the count is unknowable at emit time).
inline void IRInst::set_skip_count(uint64_t n) {
    imm = n;
}
// Scalar FP arithmetic (2-source): imm=opcode, width=ftype (0=S,1=D),
// flags_op is always 0.
inline IRInst IRInst::make_fp_binop(uint16_t dest, uint16_t src1,
                                    uint16_t src2, const FpBinopParams& p,
                                    uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::FP_BINOP;
    inst.dest = dest;
    inst.src1 = src1;
    inst.src2 = src2;
    inst.width = p.ftype;
    inst.flags_op = 0;
    inst.imm = p.opcode;
    inst.arm_pc = arm_pc;
    return inst;
}
inline FpBinopParams IRInst::fp_binop_params() const {
    return FpBinopParams{static_cast<uint8_t>(imm), width};
}
// Scalar FP unary: imm=opcode, width=ftype, rest 0.
inline IRInst IRInst::make_fp_unop(uint16_t dest, uint16_t src,
                                   const FpUnopParams& p, uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::FP_UNOP;
    inst.dest = dest;
    inst.src1 = src;
    inst.width = p.ftype;
    inst.imm = p.opcode;
    inst.arm_pc = arm_pc;
    return inst;
}
inline FpUnopParams IRInst::fp_unop_params() const {
    return FpUnopParams{static_cast<uint8_t>(imm), width};
}
// FP register move: width=ftype only.
inline IRInst IRInst::make_fp_mov(uint16_t dest, uint16_t src,
                                  const FpMovParams& p, uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::FP_MOV;
    inst.dest = dest;
    inst.src1 = src;
    inst.width = p.ftype;
    inst.arm_pc = arm_pc;
    return inst;
}
inline FpMovParams IRInst::fp_mov_params() const {
    return FpMovParams{width};
}
// FP compare: width=ftype, imm=with_zero (1=FCMP Dn,#0.0 form).
inline IRInst IRInst::make_fp_cmp(uint16_t dest, uint16_t src1, uint16_t src2,
                                  const FpCmpParams& p, uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::FP_CMP;
    inst.dest = dest;
    inst.src1 = src1;
    inst.src2 = src2;
    inst.width = p.ftype;
    inst.imm = p.with_zero ? 1 : 0;
    inst.arm_pc = arm_pc;
    return inst;
}
inline FpCmpParams IRInst::fp_cmp_params() const {
    return FpCmpParams{width, (imm & 1) != 0};
}
// FP immediate: width=ftype, imm=decoded bits.
inline IRInst IRInst::make_fp_movi(uint16_t dest, const FpMoviParams& p,
                                   uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::FP_MOVI;
    inst.dest = dest;
    inst.width = p.ftype;
    inst.imm = p.bits;
    inst.arm_pc = arm_pc;
    return inst;
}
inline FpMoviParams IRInst::fp_movi_params() const {
    return FpMoviParams{width, imm};
}
// FP conditional select: width=ftype, cond=ARM condition.
inline IRInst IRInst::make_fp_csel(uint16_t dest, uint16_t src1,
                                   uint16_t src2, const FpCselParams& p,
                                   uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::FP_CSEL;
    inst.dest = dest;
    inst.src1 = src1;
    inst.src2 = src2;
    inst.width = p.ftype;
    inst.cond = p.cond;
    inst.arm_pc = arm_pc;
    return inst;
}
inline FpCselParams IRInst::fp_csel_params() const {
    return FpCselParams{width, cond};
}
// FP->int (rounding variants): width=ftype, cond=rounding mode
// ((is_away<<2)|rmode), flags_op=sf, imm=is_unsigned.
inline IRInst IRInst::make_fp_f2i(uint16_t dest, uint16_t src,
                                  const FpF2IParams& p, uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::FP_F2I;
    inst.dest = dest;
    inst.src1 = src;
    inst.width = p.ftype;
    inst.cond = p.rounding;
    inst.flags_op = p.sf ? 1 : 0;
    inst.imm = p.is_unsigned ? 1 : 0;
    inst.arm_pc = arm_pc;
    return inst;
}
inline FpF2IParams IRInst::fp_f2i_params() const {
    return FpF2IParams{width, cond, (flags_op & 1) != 0, (imm & 1) != 0};
}
// int->FP: width=ftype, flags_op=sf, imm=is_unsigned.
inline IRInst IRInst::make_fp_i2f(uint16_t dest, uint16_t src,
                                  const FpI2FParams& p, uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::FP_I2F;
    inst.dest = dest;
    inst.src1 = src;
    inst.width = p.ftype;
    inst.flags_op = p.sf ? 1 : 0;
    inst.imm = p.is_unsigned ? 1 : 0;
    inst.arm_pc = arm_pc;
    return inst;
}
inline FpI2FParams IRInst::fp_i2f_params() const {
    return FpI2FParams{width, (flags_op & 1) != 0, (imm & 1) != 0};
}
// Fixed-point conversions: width=w, flags_op=sf, imm=is_unsigned,
// immr=fbits, imms=fp-reg flag. Set DIRECTLY by the factory — the old
// post-hoc `insts.back().immr/imms` patching is gone.
inline IRInst IRInst::make_fp_f2i_fixed(uint16_t dest, uint16_t src,
                                        const FpFixedParams& p,
                                        uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::FP_F2I_FIXED;
    inst.dest = dest;
    inst.src1 = src;
    inst.width = p.w;
    inst.flags_op = p.sf ? 1 : 0;
    inst.imm = p.is_unsigned ? 1 : 0;
    inst.immr = p.fbits;
    inst.imms = p.fp_reg ? 1 : 0;
    inst.arm_pc = arm_pc;
    return inst;
}
inline FpFixedParams IRInst::fp_f2i_fixed_params() const {
    return FpFixedParams{width, (imm & 1) != 0, (flags_op & 1) != 0,
                         immr, (imms & 1) != 0};
}
inline IRInst IRInst::make_fp_i2f_fixed(uint16_t dest, uint16_t src,
                                        const FpFixedParams& p,
                                        uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::FP_I2F_FIXED;
    inst.dest = dest;
    inst.src1 = src;
    inst.width = p.w;
    inst.flags_op = p.sf ? 1 : 0;
    inst.imm = p.is_unsigned ? 1 : 0;
    inst.immr = p.fbits;
    inst.imms = p.fp_reg ? 1 : 0;
    inst.arm_pc = arm_pc;
    return inst;
}
inline FpFixedParams IRInst::fp_i2f_fixed_params() const {
    return FpFixedParams{width, (imm & 1) != 0, (flags_op & 1) != 0,
                         immr, (imms & 1) != 0};
}
// FP round-to-integer: width=32/64 BITS (not bytes!), imm=rounding mode.
inline IRInst IRInst::make_fp_frint(uint16_t dest, uint16_t src,
                                    const FpFrintParams& p, uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::FRINT;
    inst.dest = dest;
    inst.src1 = src;
    inst.width = p.bits;
    inst.imm = p.mode;
    inst.arm_pc = arm_pc;
    return inst;
}
inline FpFrintParams IRInst::fp_frint_params() const {
    return FpFrintParams{width, static_cast<uint8_t>(imm)};
}
inline IRInst IRInst::make_fp_fused(IROp op, uint16_t dest, uint16_t src1,
                                  uint16_t src2, const FpFusedParams& p,
                                  uint64_t arm_pc) {
    IRInst inst{};
    inst.op = op;
    inst.dest = dest;
    inst.src1 = src1;
    inst.src2 = src2;
    inst.width = p.bits;
    inst.imm = p.acc;
    inst.arm_pc = arm_pc;
    return inst;
}
inline FpFusedParams IRInst::fp_fused_params() const {
    return FpFusedParams{width, static_cast<uint8_t>(imm)};
}
// The remaining unary/binary vector ops share the SimdSubopParams shape
// (imm=subop, width=esize, flags_op=Q): one factory+reader per op so the
// opcode stays pinned at the call site.
inline IRInst IRInst::make_cmp(uint16_t dest, uint16_t src1, uint16_t src2,
                               const SimdSubopParams& p, uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::SIMD_CMP;
    inst.dest = dest;
    inst.src1 = src1;
    inst.src2 = src2;
    inst.width = p.esize;
    inst.flags_op = p.q ? 1 : 0;
    inst.imm = p.subop;
    inst.arm_pc = arm_pc;
    return inst;
}
inline SimdSubopParams IRInst::cmp_params() const {
    return SimdSubopParams{static_cast<uint8_t>(imm), width,
                           (flags_op & 1) != 0};
}
inline IRInst IRInst::make_fp_arith(uint16_t dest, uint16_t src1,
                                    uint16_t src2, const SimdSubopParams& p,
                                    uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::SIMD_FP_ARITH;
    inst.dest = dest;
    inst.src1 = src1;
    inst.src2 = src2;
    inst.width = p.esize;
    inst.flags_op = p.q ? 1 : 0;
    inst.imm = p.subop;
    inst.arm_pc = arm_pc;
    return inst;
}
inline SimdSubopParams IRInst::fp_arith_params() const {
    return SimdSubopParams{static_cast<uint8_t>(imm), width,
                           (flags_op & 1) != 0};
}
inline IRInst IRInst::make_fp_fma(uint16_t dest, uint16_t src1, uint16_t src2,
                                  const SimdSubopParams& p, uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::SIMD_FP_FMA;
    inst.dest = dest;
    inst.src1 = src1;
    inst.src2 = src2;
    inst.width = p.esize;
    inst.flags_op = p.q ? 1 : 0;
    inst.imm = p.subop;
    inst.arm_pc = arm_pc;
    return inst;
}
inline SimdSubopParams IRInst::fp_fma_params() const {
    return SimdSubopParams{static_cast<uint8_t>(imm), width,
                           (flags_op & 1) != 0};
}
inline IRInst IRInst::make_sataddsub(uint16_t dest, uint16_t src1,
                                     uint16_t src2, const SimdSubopParams& p,
                                     uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::SIMD_SATADDSUB;
    inst.dest = dest;
    inst.src1 = src1;
    inst.src2 = src2;
    inst.width = p.esize;
    inst.flags_op = p.q ? 1 : 0;
    inst.imm = p.subop;
    inst.arm_pc = arm_pc;
    return inst;
}
inline SimdSubopParams IRInst::sataddsub_params() const {
    return SimdSubopParams{static_cast<uint8_t>(imm), width,
                           (flags_op & 1) != 0};
}
inline IRInst IRInst::make_abdl(uint16_t dest, uint16_t src1, uint16_t src2,
                                const SimdSubopParams& p, uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::SIMD_ABDL;
    inst.dest = dest;
    inst.src1 = src1;
    inst.src2 = src2;
    inst.width = p.esize;
    inst.flags_op = p.q ? 1 : 0;
    inst.imm = p.subop;
    inst.arm_pc = arm_pc;
    return inst;
}
inline SimdSubopParams IRInst::abdl_params() const {
    return SimdSubopParams{static_cast<uint8_t>(imm), width,
                           (flags_op & 1) != 0};
}
inline IRInst IRInst::make_abd(uint16_t dest, uint16_t src1, uint16_t src2,
                               const SimdSubopParams& p, uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::SIMD_ABD;
    inst.dest = dest;
    inst.src1 = src1;
    inst.src2 = src2;
    inst.width = p.esize;
    inst.flags_op = p.q ? 1 : 0;
    inst.imm = p.subop;
    inst.arm_pc = arm_pc;
    return inst;
}
inline SimdSubopParams IRInst::abd_params() const {
    return SimdSubopParams{static_cast<uint8_t>(imm), width,
                           (flags_op & 1) != 0};
}
inline IRInst IRInst::make_addw(uint16_t dest, uint16_t src1, uint16_t src2,
                                const SimdSubopParams& p, uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::SIMD_ADDW;
    inst.dest = dest;
    inst.src1 = src1;
    inst.src2 = src2;
    inst.width = p.esize;
    inst.flags_op = p.q ? 1 : 0;
    inst.imm = p.subop;
    inst.arm_pc = arm_pc;
    return inst;
}
inline SimdSubopParams IRInst::addw_params() const {
    return SimdSubopParams{static_cast<uint8_t>(imm), width,
                           (flags_op & 1) != 0};
}
inline IRInst IRInst::make_addhn(uint16_t dest, uint16_t src1, uint16_t src2,
                                 const SimdSubopParams& p, uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::SIMD_ADDHN;
    inst.dest = dest;
    inst.src1 = src1;
    inst.src2 = src2;
    inst.width = p.esize;
    inst.flags_op = p.q ? 1 : 0;
    inst.imm = p.subop;
    inst.arm_pc = arm_pc;
    return inst;
}
inline SimdSubopParams IRInst::addhn_params() const {
    return SimdSubopParams{static_cast<uint8_t>(imm), width,
                           (flags_op & 1) != 0};
}
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
void optimize_ir(IRBlock& block, bool force_fwd = false);
// Validate an IR block's per-op parameter contracts (see the typed
// factories above). Only migrated ops are checked; unmigrated ops are
// skipped. Violations are reported to `out` (returns false if any found)
// but compilation continues — print-and-continue, matching the project's
// other debug diagnostics. Intended for BIFROST_IR_VALIDATE runs, not
// the hot path.
bool validate_ir_block(const IRBlock& block, FILE* out = stderr);
// Dump an IR block to stderr for debugging.
void dump_ir(const IRBlock& block, FILE* out = stderr);
} // namespace arm64emu
