// ir/ir_validate.cpp — per-op IR parameter contract checker.
//
// The raw IR parameter fields (width/cond/flags_op/imm/immr/imms/sf) carry
// a different contract per op, and past miscompiles came from emit and
// codegen disagreeing about the packing (e.g. SIMD_TBL read as TBX1 with
// Q=0). validate_ir_block() range-checks every *migrated* op (one with a
// typed factory/reader in ir.hpp) so such drift fails loud instead of
// miscompiling silently. Unmigrated ops are skipped.
//
// Wire-in: jit_translate.cpp runs this after optimize_ir() when
// BIFROST_IR_VALIDATE=1 (see debug_flags.h). Tier-2 region blocks go
// through the same optimize_ir() but are not validated yet — follow-up.
#include "ir/ir.hpp"

namespace arm64emu {

bool validate_ir_block(const IRBlock& block, FILE* out) {
    bool ok = true;
    for (size_t i = 0; i < block.insts.size(); i++) {
        const IRInst& inst = block.insts[i];
        switch (inst.op) {
            case IROp::SIMD_TBL: {
                // Check the RAW packed fields (not the reader): the point
                // is to catch emit/codegen disagreeing about the layout.
                // nregs comes from ((op >> 13) & 0x3) + 1, but only the
                // 1- and 2-register forms classify to Family::TBL
                // (TBL3/TBL4 stay on the interpreter); codegen falls back
                // to interp for > 2.
                if (inst.imm < 1 || inst.imm > 2) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_TBL "
                        "nregs=%lu out of range (want 1..2, arm_pc=0x%lx)\n",
                        block.start_pc, i, (unsigned long)inst.imm, inst.arm_pc);
                    ok = false;
                }
                // Only bits 0 (Q) and 1 (is_tbx) are assigned; anything else
                // means the emitter stuffed an unrelated value in flags_op.
                if (inst.flags_op & ~0x3u) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_TBL "
                        "flags_op=0x%x has unknown bits (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.flags_op, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::SIMD_INS: {
                // Raw packed fields: width=esize, imm=dest byte offset,
                // aux=source element index, flags_op=Q.
                const uint8_t esize = inst.width;
                const bool q = (inst.flags_op & 1) != 0;
                if (esize != 1 && esize != 2 && esize != 4 && esize != 8) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_INS "
                        "esize=%u invalid (want 1/2/4/8, arm_pc=0x%lx)\n",
                        block.start_pc, i, esize, inst.arm_pc);
                    ok = false;
                    break;
                }
                // Dest offset is didx*esize: esize-aligned and inside the
                // (Q ? 16 : 8)-byte destination.
                const uint64_t dlimit = q ? 16 : 8;
                if (inst.imm % esize != 0 || inst.imm + esize > dlimit) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_INS "
                        "dst_off=%lu out of range (esize=%u Q=%d, arm_pc=0x%lx)\n",
                        block.start_pc, i, (unsigned long)inst.imm,
                        esize, q ? 1 : 0, inst.arm_pc);
                    ok = false;
                }
                // Source index must land inside the 128-bit source vector.
                if ((uint64_t)inst.aux * esize >= 16) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_INS "
                        "sidx=%u out of range (esize=%u, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.aux, esize, inst.arm_pc);
                    ok = false;
                }
                if (inst.flags_op & ~0x1u) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_INS "
                        "flags_op=0x%x has unknown bits (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.flags_op, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::SIMD_2REG: {
                // Raw packed fields: imm=subop (0=CNT,1=NOT,2=RBIT,3=ABS,
                // 4=NEG), width=esize, flags_op=Q.
                if (inst.imm > 4) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_2REG "
                        "subop=%lu out of range (want 0..4, arm_pc=0x%lx)\n",
                        block.start_pc, i, (unsigned long)inst.imm,
                        inst.arm_pc);
                    ok = false;
                }
                // Reachable esize: CNT/NOT pin size=0 (1 byte), RBIT pins
                // size=1 (2 bytes), ABS/NEG accept any size (1/2/4/8).
                if (inst.width != 1 && inst.width != 2 &&
                    inst.width != 4 && inst.width != 8) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_2REG "
                        "esize=%u invalid (want 1/2/4/8, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.width, inst.arm_pc);
                    ok = false;
                }
                if (inst.flags_op & ~0x1u) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_2REG "
                        "flags_op=0x%x has unknown bits (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.flags_op, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::SIMD_CVTF: {
                // Raw packed fields: imm=subop (0=SCVTF,1=UCVTF,2=FCVTZS,
                // 3=FCVTZU), width=always 4, flags_op=Q.
                if (inst.imm > 3) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_CVTF "
                        "subop=%lu out of range (want 0..3, arm_pc=0x%lx)\n",
                        block.start_pc, i, (unsigned long)inst.imm,
                        inst.arm_pc);
                    ok = false;
                }
                if (inst.width != 4) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_CVTF "
                        "width=%u invalid (want 4, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.width, inst.arm_pc);
                    ok = false;
                }
                if (inst.flags_op & ~0x1u) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_CVTF "
                        "flags_op=0x%x has unknown bits (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.flags_op, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::SIMD_XTN: {
                // Raw packed fields: imm=subop (0=XTN,1=SQXTUN,2=SQXTN,
                // 3=UQXTN), width=SOURCE esize, flags_op=Q.
                if (inst.imm > 3) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_XTN "
                        "subop=%lu out of range (want 0..3, arm_pc=0x%lx)\n",
                        block.start_pc, i, (unsigned long)inst.imm,
                        inst.arm_pc);
                    ok = false;
                }
                // Table guard `size < 3` pins source esize to 2/4/8.
                if (inst.width != 2 && inst.width != 4 && inst.width != 8) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_XTN "
                        "esize=%u invalid (want 2/4/8, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.width, inst.arm_pc);
                    ok = false;
                }
                if (inst.flags_op & ~0x1u) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_XTN "
                        "flags_op=0x%x has unknown bits (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.flags_op, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::SIMD_PERMUTE: {
                // Raw packed fields: imm=opc6 subop, width=esize,
                // flags_op=Q. Table rows pin opc6 to the six permutes.
                const uint8_t subop = static_cast<uint8_t>(inst.imm);
                if (subop != 0x06 && subop != 0x0A && subop != 0x0E &&
                    subop != 0x16 && subop != 0x1A && subop != 0x1E) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_PERMUTE "
                        "subop=0x%x unknown (want opc6, arm_pc=0x%lx)\n",
                        block.start_pc, i, subop, inst.arm_pc);
                    ok = false;
                }
                if (inst.width != 1 && inst.width != 2 &&
                    inst.width != 4 && inst.width != 8) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_PERMUTE "
                        "esize=%u invalid (want 1/2/4/8, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.width, inst.arm_pc);
                    ok = false;
                }
                // Table guard `Q || size != 3`: Q=0 with esize=8 (the
                // degenerate 1D form) never reaches the emitter.
                if ((inst.flags_op & 1) == 0 && inst.width == 8) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_PERMUTE "
                        "Q=0 with esize=8 is degenerate (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.arm_pc);
                    ok = false;
                }
                if (inst.flags_op & ~0x1u) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_PERMUTE "
                        "flags_op=0x%x has unknown bits (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.flags_op, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::SIMD_PAIRMIN: {
                // Raw packed fields: imm=subop (0=SMAXP,1=SMINP,2=UMAXP,
                // 3=UMINP), width=esize, flags_op=Q. size==3 has no valid
                // encoding, so esize is always 1/2/4.
                if (inst.imm > 3) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_PAIRMIN "
                        "subop=%lu out of range (want 0..3, arm_pc=0x%lx)\n",
                        block.start_pc, i, (unsigned long)inst.imm,
                        inst.arm_pc);
                    ok = false;
                }
                if (inst.width != 1 && inst.width != 2 && inst.width != 4) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_PAIRMIN "
                        "esize=%u invalid (want 1/2/4, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.width, inst.arm_pc);
                    ok = false;
                }
                if (inst.flags_op & ~0x1u) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_PAIRMIN "
                        "flags_op=0x%x has unknown bits (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.flags_op, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::SIMD_ADDP: {
                // ADDP has no subop/esize: width is always 1, imm is
                // always 0, only Q varies.
                if (inst.width != 1) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_ADDP "
                        "width=%u invalid (want 1, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.width, inst.arm_pc);
                    ok = false;
                }
                if (inst.imm != 0) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_ADDP "
                        "imm=%lu invalid (want 0, arm_pc=0x%lx)\n",
                        block.start_pc, i, (unsigned long)inst.imm,
                        inst.arm_pc);
                    ok = false;
                }
                if (inst.flags_op & ~0x1u) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_ADDP "
                        "flags_op=0x%x has unknown bits (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.flags_op, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::SIMD_ARITH: {
                // imm=subop (0..6 from table rows; 7/8 orr_imm have no
                // rows yet), width=esize, flags_op is always 0.
                if (inst.imm > 6) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_ARITH "
                        "subop=%lu out of range (want 0..6, arm_pc=0x%lx)\n",
                        block.start_pc, i, (unsigned long)inst.imm,
                        inst.arm_pc);
                    ok = false;
                }
                if (inst.width != 1 && inst.width != 2 &&
                    inst.width != 4 && inst.width != 8) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_ARITH "
                        "esize=%u invalid (want 1/2/4/8, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.width, inst.arm_pc);
                    ok = false;
                }
                // MUL 64-bit (subop 2, esize 8) is excluded by the table
                // guard (no SSE2 pmulq); codegen would interp-fallback.
                if (inst.imm == 2 && inst.width == 8) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_ARITH "
                        "MUL with esize=8 is table-excluded (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.arm_pc);
                    ok = false;
                }
                if (inst.flags_op != 0) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_ARITH "
                        "flags_op=0x%x must be 0 (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.flags_op, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::SIMD_LOGICAL: {
                // Only subop varies (imm 0..4 from rows; 5=EON reachable
                // via the InstClass path); width/cond/flags_op are 0.
                if (inst.imm > 5) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_LOGICAL "
                        "subop=%lu out of range (want 0..5, arm_pc=0x%lx)\n",
                        block.start_pc, i, (unsigned long)inst.imm,
                        inst.arm_pc);
                    ok = false;
                }
                if (inst.width != 0 || inst.cond != 0 ||
                    inst.flags_op != 0) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_LOGICAL "
                        "width/cond/flags must be 0 (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::SIMD_CMP: {
                // imm=subop (0=CMEQ,1=CMGT,2=CMGE,3=CMHI,4=CMHS),
                // width=esize, flags_op=Q.
                if (inst.imm > 4) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_CMP "
                        "subop=%lu out of range (want 0..4, arm_pc=0x%lx)\n",
                        block.start_pc, i, (unsigned long)inst.imm,
                        inst.arm_pc);
                    ok = false;
                }
                if (inst.width != 1 && inst.width != 2 &&
                    inst.width != 4 && inst.width != 8) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_CMP "
                        "esize=%u invalid (want 1/2/4/8, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.width, inst.arm_pc);
                    ok = false;
                }
                if (inst.flags_op & ~0x1u) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_CMP "
                        "flags_op=0x%x has unknown bits (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.flags_op, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::SIMD_FP_ARITH: {
                // imm=subop (0..7, 0xB FMULX, 0xD FABD), width=4/8,
                // flags_op=Q.
                const uint8_t opc = static_cast<uint8_t>(inst.imm);
                if (opc > 7 && opc != 0xB && opc != 0xD) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_FP_ARITH "
                        "subop=0x%x unknown (arm_pc=0x%lx)\n",
                        block.start_pc, i, opc, inst.arm_pc);
                    ok = false;
                }
                if (inst.width != 4 && inst.width != 8) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_FP_ARITH "
                        "esize=%u invalid (want 4/8, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.width, inst.arm_pc);
                    ok = false;
                }
                if (inst.flags_op & ~0x1u) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_FP_ARITH "
                        "flags_op=0x%x has unknown bits (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.flags_op, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::SIMD_FP_FMA: {
                // imm=subop (0=FMLA,1=FMLS), width=4/8, flags_op=Q.
                if (inst.imm > 1) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_FP_FMA "
                        "subop=%lu out of range (want 0..1, arm_pc=0x%lx)\n",
                        block.start_pc, i, (unsigned long)inst.imm,
                        inst.arm_pc);
                    ok = false;
                }
                if (inst.width != 4 && inst.width != 8) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_FP_FMA "
                        "esize=%u invalid (want 4/8, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.width, inst.arm_pc);
                    ok = false;
                }
                if (inst.flags_op & ~0x1u) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_FP_FMA "
                        "flags_op=0x%x has unknown bits (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.flags_op, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::SIMD_SATADDSUB: {
                // imm=subop 0..3, width=esize 1/2 (guard `size < 2`),
                // flags_op=Q.
                if (inst.imm > 3) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_SATADDSUB "
                        "subop=%lu out of range (want 0..3, arm_pc=0x%lx)\n",
                        block.start_pc, i, (unsigned long)inst.imm,
                        inst.arm_pc);
                    ok = false;
                }
                if (inst.width != 1 && inst.width != 2) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_SATADDSUB "
                        "esize=%u invalid (want 1/2, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.width, inst.arm_pc);
                    ok = false;
                }
                if (inst.flags_op & ~0x1u) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_SATADDSUB "
                        "flags_op=0x%x has unknown bits (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.flags_op, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::SIMD_ABDL: {
                // imm=subop 0..3, width=SOURCE esize (no table guard:
                // 1/2/4/8 all reachable, 8 falls back in codegen),
                // flags_op=Q.
                if (inst.imm > 3) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_ABDL "
                        "subop=%lu out of range (want 0..3, arm_pc=0x%lx)\n",
                        block.start_pc, i, (unsigned long)inst.imm,
                        inst.arm_pc);
                    ok = false;
                }
                if (inst.width != 1 && inst.width != 2 &&
                    inst.width != 4 && inst.width != 8) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_ABDL "
                        "esize=%u invalid (want 1/2/4/8, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.width, inst.arm_pc);
                    ok = false;
                }
                if (inst.flags_op & ~0x1u) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_ABDL "
                        "flags_op=0x%x has unknown bits (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.flags_op, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::SIMD_ABD: {
                // imm=subop (0=S,1=U; codegen ignores it — the abs trick
                // is sign-agnostic), width=esize, flags_op=Q.
                if (inst.imm > 1) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_ABD "
                        "subop=%lu out of range (want 0..1, arm_pc=0x%lx)\n",
                        block.start_pc, i, (unsigned long)inst.imm,
                        inst.arm_pc);
                    ok = false;
                }
                if (inst.width != 1 && inst.width != 2 &&
                    inst.width != 4 && inst.width != 8) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_ABD "
                        "esize=%u invalid (want 1/2/4/8, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.width, inst.arm_pc);
                    ok = false;
                }
                if (inst.flags_op & ~0x1u) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_ABD "
                        "flags_op=0x%x has unknown bits (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.flags_op, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::SIMD_ADDW: {
                // imm=subop 0..3, width=SOURCE esize 2/4
                // (guard `size != 0 && size < 3`), flags_op=Q.
                if (inst.imm > 3) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_ADDW "
                        "subop=%lu out of range (want 0..3, arm_pc=0x%lx)\n",
                        block.start_pc, i, (unsigned long)inst.imm,
                        inst.arm_pc);
                    ok = false;
                }
                if (inst.width != 2 && inst.width != 4) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_ADDW "
                        "esize=%u invalid (want 2/4, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.width, inst.arm_pc);
                    ok = false;
                }
                if (inst.flags_op & ~0x1u) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_ADDW "
                        "flags_op=0x%x has unknown bits (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.flags_op, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::SIMD_ADDHN: {
                // imm=subop 0..3, width=INPUT esize 2/4/8
                // (2<<size, guard `size < 3`), flags_op=Q (dest-half).
                if (inst.imm > 3) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_ADDHN "
                        "subop=%lu out of range (want 0..3, arm_pc=0x%lx)\n",
                        block.start_pc, i, (unsigned long)inst.imm,
                        inst.arm_pc);
                    ok = false;
                }
                if (inst.width != 2 && inst.width != 4 && inst.width != 8) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_ADDHN "
                        "esize=%u invalid (want 2/4/8, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.width, inst.arm_pc);
                    ok = false;
                }
                if (inst.flags_op & ~0x1u) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_ADDHN "
                        "flags_op=0x%x has unknown bits (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.flags_op, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::SIMD_SHL:
            case IROp::SIMD_USHR:
            case IROp::SIMD_SSHR:
            case IROp::SIMD_USRA:
            case IROp::SIMD_SSRA:
            case IROp::SIMD_URSRA:
            case IROp::SIMD_SRSRA:
            case IROp::SIMD_SLI:
            case IROp::SIMD_SRI: {
                // imm=shift amount, width=esize, flags_op=Q. The translator
                // derives esize from immh (always 1/2/4/8) and shift in
                // 0..esize*8 (sh==esize*8 is the URSRA/SRSRA top-bit edge,
                // handled in codegen).
                if (inst.width != 1 && inst.width != 2 &&
                    inst.width != 4 && inst.width != 8) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: shift "
                        "esize=%u invalid (want 1/2/4/8, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.width, inst.arm_pc);
                    ok = false;
                } else if (inst.imm > (uint64_t)inst.width * 8) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: shift "
                        "amount=%lu exceeds esize*8 (esize=%u, arm_pc=0x%lx)\n",
                        block.start_pc, i, (unsigned long)inst.imm,
                        inst.width, inst.arm_pc);
                    ok = false;
                }
                if (inst.flags_op & ~0x1u) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: shift "
                        "flags_op=0x%x has unknown bits (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.flags_op, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::SIMD_UMOV: {
                // width=esize, imm=lane index, flags_op=Q (0=Wd,1=Xd).
                // The lane must land inside the 16-byte vector.
                if (inst.width != 1 && inst.width != 2 &&
                    inst.width != 4 && inst.width != 8) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_UMOV "
                        "esize=%u invalid (want 1/2/4/8, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.width, inst.arm_pc);
                    ok = false;
                } else if (inst.imm * inst.width >= 16) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_UMOV "
                        "lane=%lu out of range (esize=%u, arm_pc=0x%lx)\n",
                        block.start_pc, i, (unsigned long)inst.imm,
                        inst.width, inst.arm_pc);
                    ok = false;
                }
                if (inst.flags_op & ~0x1u) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_UMOV "
                        "flags_op=0x%x has unknown bits (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.flags_op, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::SIMD_SMOV: {
                // Same shape as UMOV, but the .D form is UNALLOCATED
                // (table guard keeps it on the interpreter): esize 1/2/4.
                if (inst.width != 1 && inst.width != 2 && inst.width != 4) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_SMOV "
                        "esize=%u invalid (want 1/2/4, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.width, inst.arm_pc);
                    ok = false;
                } else if (inst.imm * inst.width >= 16) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_SMOV "
                        "lane=%lu out of range (esize=%u, arm_pc=0x%lx)\n",
                        block.start_pc, i, (unsigned long)inst.imm,
                        inst.width, inst.arm_pc);
                    ok = false;
                }
                if (inst.flags_op & ~0x1u) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_SMOV "
                        "flags_op=0x%x has unknown bits (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.flags_op, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::SIMD_ORRIMM: {
                // imm=64-bit pattern (any value), cond=invert bit,
                // flags_op=Q, width is always 0.
                if (inst.width != 0) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_ORRIMM "
                        "width=%u invalid (want 0, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.width, inst.arm_pc);
                    ok = false;
                }
                if (inst.cond & ~0x1u) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_ORRIMM "
                        "cond=0x%x has unknown bits (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.cond, inst.arm_pc);
                    ok = false;
                }
                if (inst.flags_op & ~0x1u) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_ORRIMM "
                        "flags_op=0x%x has unknown bits (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.flags_op, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::SIMD_MOVI: {
                // imm=64-bit pattern (any value), flags_op=Q;
                // width/cond are always 0.
                if (inst.width != 0 || inst.cond != 0) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_MOVI "
                        "width/cond must be 0 (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.arm_pc);
                    ok = false;
                }
                if (inst.flags_op & ~0x1u) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_MOVI "
                        "flags_op=0x%x has unknown bits (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.flags_op, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::SIMD_DUP: {
                // width=esize bytes, flags_op=Q, imm is always 0.
                if (inst.width != 1 && inst.width != 2 &&
                    inst.width != 4 && inst.width != 8) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_DUP "
                        "esize=%u invalid (want 1/2/4/8, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.width, inst.arm_pc);
                    ok = false;
                }
                if (inst.imm != 0) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_DUP "
                        "imm=%lu invalid (want 0, arm_pc=0x%lx)\n",
                        block.start_pc, i, (unsigned long)inst.imm,
                        inst.arm_pc);
                    ok = false;
                }
                if (inst.flags_op & ~0x1u) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_DUP "
                        "flags_op=0x%x has unknown bits (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.flags_op, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::SIMD_SHRN_SAT: {
                // imm=subop | (shift<<8), width=SOURCE esize, flags_op=Q.
                // subop 0..5 (table rows); shift is 1..esize*8 (the
                // immh!=0 guard); esize from 1<<size.
                const uint8_t subop = static_cast<uint8_t>(inst.imm & 0xFF);
                const uint8_t shift = static_cast<uint8_t>((inst.imm >> 8) & 0xFF);
                if (subop > 5) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_SHRN_SAT "
                        "subop=%u out of range (want 0..5, arm_pc=0x%lx)\n",
                        block.start_pc, i, subop, inst.arm_pc);
                    ok = false;
                }
                if (inst.width != 1 && inst.width != 2 &&
                    inst.width != 4 && inst.width != 8) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_SHRN_SAT "
                        "esize=%u invalid (want 1/2/4/8, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.width, inst.arm_pc);
                    ok = false;
                } else if (shift < 1 || shift > inst.width * 8) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_SHRN_SAT "
                        "shift=%u out of range 1..esize*8 (esize=%u, arm_pc=0x%lx)\n",
                        block.start_pc, i, shift, inst.width, inst.arm_pc);
                    ok = false;
                }
                if (inst.flags_op & ~0x1u) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_SHRN_SAT "
                        "flags_op=0x%x has unknown bits (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.flags_op, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::SIMD_MUL_ELEM: {
                // imm=subop | (lane<<8), width=SOURCE esize (guard
                // `size < 3` → 1/2/4), flags_op=Q. subop 0..11.
                const uint8_t subop = static_cast<uint8_t>(inst.imm & 0xFF);
                const uint8_t lane = static_cast<uint8_t>((inst.imm >> 8) & 0xFF);
                if (subop > 11) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_MUL_ELEM "
                        "subop=%u out of range (want 0..11, arm_pc=0x%lx)\n",
                        block.start_pc, i, subop, inst.arm_pc);
                    ok = false;
                }
                if (inst.width != 1 && inst.width != 2 && inst.width != 4) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_MUL_ELEM "
                        "esize=%u invalid (want 1/2/4, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.width, inst.arm_pc);
                    ok = false;
                }
                // Lane comes from H:L:M(:Rm) bit formulas — always ≤ 15.
                if (lane > 15) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_MUL_ELEM "
                        "lane=%u out of range (want 0..15, arm_pc=0x%lx)\n",
                        block.start_pc, i, lane, inst.arm_pc);
                    ok = false;
                }
                if (inst.flags_op & ~0x1u) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_MUL_ELEM "
                        "flags_op=0x%x has unknown bits (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.flags_op, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::FP_BINOP: {
                // imm=opcode 0..8, width=ftype 0/1, flags_op always 0.
                if (inst.imm > 8) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: FP_BINOP "
                        "opcode=%lu out of range (want 0..8, arm_pc=0x%lx)\n",
                        block.start_pc, i, (unsigned long)inst.imm,
                        inst.arm_pc);
                    ok = false;
                }
                if (inst.width > 1) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: FP_BINOP "
                        "ftype=%u invalid (want 0/1, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.width, inst.arm_pc);
                    ok = false;
                }
                if (inst.flags_op != 0) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: FP_BINOP "
                        "flags_op=0x%x must be 0 (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.flags_op, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::FP_UNOP: {
                // imm=opcode 1..3 (1=abs,2=neg,3=sqrt per the fp1_opcode
                // guard), width=ftype 0/1.
                if (inst.imm < 1 || inst.imm > 3) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: FP_UNOP "
                        "opcode=%lu out of range (want 1..3, arm_pc=0x%lx)\n",
                        block.start_pc, i, (unsigned long)inst.imm,
                        inst.arm_pc);
                    ok = false;
                }
                if (inst.width > 1) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: FP_UNOP "
                        "ftype=%u invalid (want 0/1, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.width, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::FP_MOV: {
                // width=ftype 0/1 only.
                if (inst.width > 1) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: FP_MOV "
                        "ftype=%u invalid (want 0/1, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.width, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::FP_CMP: {
                // width=ftype 0/1, imm=with_zero bit.
                if (inst.width > 1) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: FP_CMP "
                        "ftype=%u invalid (want 0/1, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.width, inst.arm_pc);
                    ok = false;
                }
                if (inst.imm > 1) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: FP_CMP "
                        "imm=%lu invalid (want 0/1, arm_pc=0x%lx)\n",
                        block.start_pc, i, (unsigned long)inst.imm,
                        inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::FP_MOVI: {
                // width=ftype 0/1, imm=decoded bits (any u64).
                if (inst.width > 1) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: FP_MOVI "
                        "ftype=%u invalid (want 0/1, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.width, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::FP_CSEL: {
                // width=ftype 0/1, cond=4-bit ARM condition.
                if (inst.width > 1) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: FP_CSEL "
                        "ftype=%u invalid (want 0/1, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.width, inst.arm_pc);
                    ok = false;
                }
                if (inst.cond > 15) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: FP_CSEL "
                        "cond=%u out of range (want 0..15, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.cond, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::FP_F2I: {
                // width=ftype 0/1, cond=rounding ((is_away<<2)|rmode —
                // is_away is guarded to the interpreter, so native sees
                // 0..3 only), flags_op=sf, imm=is_unsigned bit.
                if (inst.width > 1) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: FP_F2I "
                        "ftype=%u invalid (want 0/1, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.width, inst.arm_pc);
                    ok = false;
                }
                if (inst.cond > 3) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: FP_F2I "
                        "rounding=%u has ties-away bit (want 0..3, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.cond, inst.arm_pc);
                    ok = false;
                }
                if (inst.flags_op > 1 || inst.imm > 1) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: FP_F2I "
                        "sf/unsigned must be bits (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::FP_I2F: {
                // width=ftype 0/1, flags_op=sf, imm=is_unsigned bit.
                if (inst.width > 1) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: FP_I2F "
                        "ftype=%u invalid (want 0/1, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.width, inst.arm_pc);
                    ok = false;
                }
                if (inst.flags_op > 1 || inst.imm > 1) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: FP_I2F "
                        "sf/unsigned must be bits (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::FP_F2I_FIXED:
            case IROp::FP_I2F_FIXED: {
                // width=w (0/1 flag), imm=is_unsigned bit, flags_op=sf
                // bit, immr=fbits 0..64, imms=fp-reg bit.
                const char* name = (inst.op == IROp::FP_F2I_FIXED)
                    ? "FP_F2I_FIXED" : "FP_I2F_FIXED";
                if (inst.width > 1 || inst.imm > 1 || inst.flags_op > 1 ||
                    inst.imms > 1) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: %s "
                        "w/unsigned/sf/fp_reg must be bits (arm_pc=0x%lx)\n",
                        block.start_pc, i, name, inst.arm_pc);
                    ok = false;
                }
                if (inst.immr > 64) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: %s "
                        "fbits=%u out of range (want 0..64, arm_pc=0x%lx)\n",
                        block.start_pc, i, name, inst.immr, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::FRINT: {
                // width=32/64 BITS, imm=mode 0..6 (6=FRINTA, jit fallback).
                if (inst.width != 32 && inst.width != 64) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: FRINT "
                        "bits=%u invalid (want 32/64, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.width, inst.arm_pc);
                    ok = false;
                }
                if (inst.imm > 6) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: FRINT "
                        "mode=%lu out of range (want 0..6, arm_pc=0x%lx)\n",
                        block.start_pc, i, (unsigned long)inst.imm,
                        inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::FMADD:
            case IROp::FMSUB:
            case IROp::FNMADD:
            case IROp::FNMSUB: {
                // width=32/64 BITS, imm=accumulator FP-reg index 0..31.
                if (inst.width != 32 && inst.width != 64) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: FMA "
                        "bits=%u invalid (want 32/64, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.width, inst.arm_pc);
                    ok = false;
                }
                if (inst.imm > 31) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: FMA "
                        "acc=%lu out of range (want 0..31, arm_pc=0x%lx)\n",
                        block.start_pc, i, (unsigned long)inst.imm,
                        inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::SBFM:
            case IROp::UBFM: {
                // immr/imms/sf only; width/cond/flags_op/imm are always 0
                // (emit_bf never sets them; codegen derives 32/64 from sf).
                const char* name = (inst.op == IROp::SBFM) ? "SBFM" : "UBFM";
                if (inst.width != 0 || inst.cond != 0 ||
                    inst.flags_op != 0 || inst.imm != 0) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: %s "
                        "width/cond/flags/imm must be 0 (arm_pc=0x%lx)\n",
                        block.start_pc, i, name, inst.arm_pc);
                    ok = false;
                }
                // 6-bit fields; W-forms (sf=0) only admit 0..31.
                const uint8_t lim = (inst.sf != 0) ? 63 : 31;
                if (inst.immr > lim || inst.imms > lim) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: %s "
                        "immr/imms out of range (sf=%u, arm_pc=0x%lx)\n",
                        block.start_pc, i, name, inst.sf, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::SEXT: {
                // width=source bits 8/16/32 (SXTX needs no op).
                if (inst.width != 8 && inst.width != 16 &&
                    inst.width != 32) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SEXT "
                        "bits=%u invalid (want 8/16/32, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.width, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::ZEXT: {
                // width=8/16/32 bits (byte/halfword/word loads; the fp
                // sites pass the literal 32).
                if (inst.width != 8 && inst.width != 16 &&
                    inst.width != 32) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: ZEXT "
                        "bits=%u invalid (want 8/16/32, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.width, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::UDIV:
            case IROp::SDIV: {
                // width=32/64 BITS.
                if (inst.width != 32 && inst.width != 64) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: DIV "
                        "bits=%u invalid (want 32/64, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.width, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::CSEL: {
                // cond=ARM condition, imm=rd slot, width/flags always 0.
                if (inst.width != 0 || inst.flags_op != 0) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: CSEL "
                        "width/flags must be 0 (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.arm_pc);
                    ok = false;
                }
                if (inst.cond > 15 || inst.imm > 31) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: CSEL "
                        "cond/rd out of range (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::CCMP: {
                // width=NZCV (4 bits, NOT a size), cond=condition,
                // flags=is_sub bit, imm always 0.
                if (inst.width > 15) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: CCMP "
                        "nzcv=%u out of range (want 0..15, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.width, inst.arm_pc);
                    ok = false;
                }
                if (inst.cond > 15 || inst.flags_op > 1 || inst.imm != 0) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: CCMP "
                        "cond/flags/imm invalid (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::ADDS:
            case IROp::SUBS:
            case IROp::ADCS:
            case IROp::SBCS: {
                // width=32/64 BITS, flags=is_sub bit, cond/imm always 0.
                // (The opcode — not flags_op — selects add vs sub.)
                if (inst.width != 32 && inst.width != 64) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: ADDS-family "
                        "bits=%u invalid (want 32/64, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.width, inst.arm_pc);
                    ok = false;
                }
                if (inst.flags_op > 1 || inst.cond != 0 || inst.imm != 0) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: ADDS-family "
                        "flags/cond/imm invalid (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::BRCOND: {
                // cond=condition, imm=target (4-aligned guest pc).
                if (inst.cond > 15) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: BRCOND "
                        "cond=%u out of range (want 0..15, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.cond, inst.arm_pc);
                    ok = false;
                }
                if (inst.imm % 4 != 0) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: BRCOND "
                        "target=0x%lx misaligned (arm_pc=0x%lx)\n",
                        block.start_pc, i, (unsigned long)inst.imm,
                        inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::BRCOND_ZERO: {
                // cond=EQ/NE only, imm=target, sf=W-form bit.
                if (inst.cond > 1 || inst.flags_op != 0 || inst.width != 0) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: BRCOND_ZERO "
                        "cond/flags/width invalid (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.arm_pc);
                    ok = false;
                }
                if (inst.imm % 4 != 0) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: BRCOND_ZERO "
                        "target=0x%lx misaligned (arm_pc=0x%lx)\n",
                        block.start_pc, i, (unsigned long)inst.imm,
                        inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::BRCOND_BIT: {
                // width=bit 0..63, cond=EQ/NE, imm=target.
                if (inst.width > 63 || inst.cond > 1 || inst.flags_op != 0) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: BRCOND_BIT "
                        "bit/cond/flags invalid (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.arm_pc);
                    ok = false;
                }
                if (inst.imm % 4 != 0) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: BRCOND_BIT "
                        "target=0x%lx misaligned (arm_pc=0x%lx)\n",
                        block.start_pc, i, (unsigned long)inst.imm,
                        inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::BRCOND_FALLTHRU: {
                // cond is always AL, imm=target.
                if (inst.cond != 14) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: FALLTHRU "
                        "cond=%u invalid (want 14/AL, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.cond, inst.arm_pc);
                    ok = false;
                }
                if (inst.imm % 4 != 0) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: FALLTHRU "
                        "target=0x%lx misaligned (arm_pc=0x%lx)\n",
                        block.start_pc, i, (unsigned long)inst.imm,
                        inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::BRCOND_SKIP: {
                // cond=condition, imm=region op count (any value).
                if (inst.cond > 15) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SKIP "
                        "cond=%u out of range (want 0..15, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.cond, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::BL_CALL: {
                // imm=target (4-aligned guest pc).
                if (inst.imm % 4 != 0) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: BL_CALL "
                        "target=0x%lx misaligned (arm_pc=0x%lx)\n",
                        block.start_pc, i, (unsigned long)inst.imm,
                        inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::LOAD_MEM:
            case IROp::STORE_MEM: {
                // width=access size 1/2/4/8, imm=byte offset (any value,
                // including wrapped negatives from pre-index).
                const char* name = (inst.op == IROp::LOAD_MEM)
                    ? "LOAD_MEM" : "STORE_MEM";
                if (inst.width != 1 && inst.width != 2 &&
                    inst.width != 4 && inst.width != 8) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: %s "
                        "width=%u invalid (want 1/2/4/8, arm_pc=0x%lx)\n",
                        block.start_pc, i, name, inst.width, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::ATOMIC: {
                // width=1/2/4/8, cond=atom op 0..8/0xC..0xF, flags=is_load
                // bit, imm=ARM reg index 0..31.
                if (inst.width != 1 && inst.width != 2 &&
                    inst.width != 4 && inst.width != 8) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: ATOMIC "
                        "width=%u invalid (want 1/2/4/8, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.width, inst.arm_pc);
                    ok = false;
                }
                const uint8_t aop = inst.cond;
                if ((aop > 8 && (aop < 0xC || aop > 0xF))) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: ATOMIC "
                        "atom_op=0x%x unknown (want 0..8/0xC..0xF, arm_pc=0x%lx)\n",
                        block.start_pc, i, aop, inst.arm_pc);
                    ok = false;
                }
                if (inst.flags_op > 1 || inst.imm > 31) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: ATOMIC "
                        "is_load/reg invalid (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::SIMD_LDST: {
                // width=1 load / 0 store only.
                if (inst.width > 1) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_LDST "
                        "width=%u invalid (want 0/1, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.width, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::SIMD_LD16: {
                // flags_op=register count 1..4, imm=byte offset (any —
                // the mem producers pass mem_off, the fp site passes 0).
                if (inst.flags_op < 1 || inst.flags_op > 4) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_LD16 "
                        "count=%u invalid (want 1..4, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.flags_op, inst.arm_pc);
                    ok = false;
                }
                if (inst.width != 0 || inst.cond != 0) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_LD16 "
                        "width/cond must be 0 (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::SIMD_ST16: {
                // flags_op=register count 1..4, cond=broadcast bit,
                // imm=byte offset (any), width always 0.
                if (inst.flags_op < 1 || inst.flags_op > 4) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_ST16 "
                        "count=%u invalid (want 1..4, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.flags_op, inst.arm_pc);
                    ok = false;
                }
                if (inst.width != 0 || (inst.cond & ~0x1u)) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: SIMD_ST16 "
                        "width/cond invalid (arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::SHL:
            case IROp::SHR:
            case IROp::SAR:
            case IROp::ROR: {
                // width selects the x86 form: 32 = 32-bit, 0/64 = 64-bit
                // (producers use 0 and 64 interchangeably — preserved).
                if (inst.width != 0 && inst.width != 32 &&
                    inst.width != 64) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: GPR-SHIFT "
                        "width=%u invalid (want 0/32/64, arm_pc=0x%lx)\n",
                        block.start_pc, i, inst.width, inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::CLZ:
            case IROp::REV64: {
                // width=32/64 BITS, imm=rd slot (preserved verbatim;
                // codegen ignores it).
                const char* name = (inst.op == IROp::CLZ) ? "CLZ" : "REV64";
                if (inst.width != 32 && inst.width != 64) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: %s "
                        "bits=%u invalid (want 32/64, arm_pc=0x%lx)\n",
                        block.start_pc, i, name, inst.width, inst.arm_pc);
                    ok = false;
                }
                if (inst.imm > 31) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: %s "
                        "rd=%lu out of range (want 0..31, arm_pc=0x%lx)\n",
                        block.start_pc, i, name, (unsigned long)inst.imm,
                        inst.arm_pc);
                    ok = false;
                }
                break;
            }
            case IROp::AES_CRYPTO: {
                // imm=subop 0..5 (0=AESE-group,1=AESD,2=AESMC,3=AESIMC,
                // 4=PMULL,5=PMULL2).
                if (inst.imm > 5) {
                    std::fprintf(out,
                        "[ir-validate] block pc=0x%lx inst %zu: AES_CRYPTO "
                        "subop=%lu out of range (want 0..5, arm_pc=0x%lx)\n",
                        block.start_pc, i, (unsigned long)inst.imm,
                        inst.arm_pc);
                    ok = false;
                }
                break;
            }
            default:
                // Unmigrated op — no contract to check yet. Add a case here
                // when the op gains a typed factory/reader.
                break;
        }
    }
    return ok;
}

} // namespace arm64emu
