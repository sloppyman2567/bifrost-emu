// Factories.hpp — inline IRInst factories (make_*) and readers (*_params)
// that pack typed parameters (Parameters.hpp) into IRInst fields and read
// them back. This is the IR parameter contract: translators use the
// factories, codegen and passes use the readers. Included by IR.hpp.
#pragma once
#include "FrostJIT/IR/IR.hpp"
#include "FrostJIT/IR/Parameters.hpp"
namespace arm64emu {
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
inline IRInst IRInst::make_native_structure(const NativeStructureParams& p, uint64_t arm_pc) {
    IRInst i{}; i.op = IROp::SIMD_NATIVE_STRUCT; i.imm = p.pack(); i.arm_pc = arm_pc; return i;
}
inline IRInst IRInst::make_native_simd(uint32_t opcode, uint64_t arm_pc) {
    IRInst i{}; i.op = IROp::SIMD_NATIVE_EXTRA; i.imm = opcode;
    i.arm_pc = arm_pc; return i;
}
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
    inst.flags_op = p.q;
    inst.imm = p.subop;
    inst.arm_pc = arm_pc;
    return inst;
}
inline SimdLogicParams IRInst::logical_params() const {
    return SimdLogicParams{static_cast<uint8_t>(imm), flags_op != 0};
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
// Scalar lane copy: width=element bytes, aux=source lane.
inline IRInst IRInst::make_scalar_dup(uint16_t dest, uint16_t src,
                                     const SimdScalarDupParams& p, uint64_t arm_pc) {
    IRInst i{}; i.op = IROp::SIMD_SCALAR_DUP; i.dest = dest; i.src1 = src;
    i.width = p.esize; i.aux = p.lane; i.arm_pc = arm_pc; return i;
}
inline SimdScalarDupParams IRInst::scalar_dup_params() const {
    return SimdScalarDupParams{width, static_cast<uint8_t>(aux)};
}
// GPR->vector broadcast: width=esize, flags_op=Q.
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
inline IRInst IRInst::make_llsc(IROp op, uint16_t dest, uint16_t address,
                                 uint16_t value, uint8_t bytes, bool exclusive,
                                 uint64_t arm_pc) {
    IRInst i{}; i.op = op; i.dest = dest; i.src1 = address; i.src2 = value;
    i.width = bytes; i.flags_op = exclusive; i.arm_pc = arm_pc;
    return i;
}
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
    inst.imm = (p.with_zero ? 1 : 0) | (p.conditional ? 2 : 0);
    inst.cond = p.condition;
    inst.flags_op = p.nzcv;
    inst.arm_pc = arm_pc;
    return inst;
}
inline FpCmpParams IRInst::fp_cmp_params() const {
    return FpCmpParams{width, (imm & 1) != 0, (imm & 2) != 0, cond, flags_op};
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
                                    uint16_t src2, const SimdFpArithParams& p,
                                    uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::SIMD_FP_ARITH;
    inst.dest = dest;
    inst.src1 = src1;
    inst.src2 = src2;
    inst.width = p.esize;
    inst.flags_op = p.q ? 1 : 0;
    inst.imm = p.subop;
    inst.aux = static_cast<uint64_t>(p.lane + 1);
    inst.arm_pc = arm_pc;
    return inst;
}
inline SimdFpArithParams IRInst::fp_arith_params() const {
    return SimdFpArithParams{static_cast<uint8_t>(imm), width,
                           (flags_op & 1) != 0, aux ? static_cast<int8_t>(aux - 1) : int8_t(-1)};
}
inline IRInst IRInst::make_fp_unary(uint16_t dest, uint16_t src1,
                                    const SimdSubopParams& p, uint64_t arm_pc) {
    IRInst inst{};
    inst.op = IROp::SIMD_FP_UNARY;
    inst.dest = dest; inst.src1 = src1;
    inst.width = p.esize; inst.flags_op = p.q ? 1 : 0;
    inst.imm = p.subop; inst.arm_pc = arm_pc;
    return inst;
}
inline SimdSubopParams IRInst::fp_unary_params() const {
    return SimdSubopParams{static_cast<uint8_t>(imm), width, (flags_op & 1) != 0};
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
} // namespace arm64emu
