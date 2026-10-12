// Parameters.hpp — typed IR parameter structs. Producers pass them to the
// IRInst factories and consumers read them back through the matching
// readers (Factories.hpp); the packed IRInst fields stay private.
// Validate.cpp checks each op's parameter contract.
#pragma once
#include <cstdint>
namespace arm64emu {
// Typed parameter structs, one per migrated op. Field names match the
// IROp comments so the contract reads the same on both sides.
struct SimdTblParams {
    uint8_t nregs = 1;  // 1 = TBL1/TBX1, 2 = TBL2/TBX2 (packed into imm)
    bool is_tbx = false;// packed into flags_op bit 1
    bool q = false;     // 0 = 64-bit operand, 1 = 128-bit (flags_op bit 0)
};
struct SimdScalarDupParams {
    uint8_t esize = 1;
    uint8_t lane = 0;
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
struct SimdFpArithParams {
    uint8_t subop = 0;
    uint8_t esize = 4;
    bool q = false;
    int8_t lane = -1; // -1: vector source; otherwise broadcast indexed src2
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
    uint8_t subop = 0;  // 0=and,1=orr,2=xor,3=bic,4=orn,5=eon
    bool q = true;
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
    bool conditional = false;
    uint8_t condition = 0;
    uint8_t nzcv = 0;
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
} // namespace arm64emu
