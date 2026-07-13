// ir_translate.cpp — ARM64 → IR translator.
//
// Translates each ARM64 instruction (DecodedInst) into 1+ IR micro-ops.
// The IR is then optimized (ir_optimize.cpp) and either executed by
// ops.cpp (debug) or compiled to native x86-64 code by frostjit.cpp.
//
// Vreg mapping:
//   0-30  = ARM64 X0-X30
//   31    = SP
//   32    = XZR (always 0, writes discarded)
//   33+   = scratch temporaries (allocated per-block, reset every block)
//
// Translation rules:
//   - Each ARM64 instruction emits IR ops whose net effect on the
//     architectural state (cpu.regs[], cpu.sp, cpu.pstate, memory)
//     matches the ARM64 semantics.
//   - Side-effecting ops (LOAD_MEM, STORE_MEM, CALL_INTERP, SVC, BR*)
//     are not optimized away.
//   - The translator never reads or writes cpu.regs[] directly — it
//     emits LOAD_REG / STORE_REG ops. This is what lets the optimizer
//     cache values across instructions.
//   - 32-bit ARM64 ops (sf=0) emit an explicit ZEXT after the ALU op
//     so the high 32 bits are zeroed. The optimizer peephole removes
//     redundant ZEXTs after ops that already zero-extend (ADD with
//     32-bit dest on x86, etc.) when generating x86.

#include "ir/ir.h"        // emit/load_imm/swar helpers + g_alloc
#include "ir/ir.hpp"      // public IR types
#include "core/emulator.h"  // for cond_true() (used by executor only)

namespace arm64emu {

// Turn 91: thread-local flag to disable BL_CALL during re-translation.
// When true, BL instructions use the old behavior (end block at BL)
// instead of BL_CALL (call within block). Set by translate_block when
// re-translating a block whose BL_CALL targets aren't translated yet.
thread_local bool bl_call_disabled_ = false;

// ── translate_to_ir ────────────────────────────────────────────────────

// ── Translator ──────────────────────────────────────────────────────────
bool translate_to_ir(IRBlock& block, const DecodedInst& d, uint64_t cur_pc) {
    // FP/SIMD and memory load/store cases are split into separate
    // files (ir_translate_fp.cpp, ir_translate_mem.cpp) to keep this
    // file under 1000 lines. Both helpers return `true` if they
    // handled the case (in which case the instruction does not
    // terminate the block — translate_to_ir returns `false`).
    if (translate_fp(block, d, cur_pc))  return false;
    if (translate_mem(block, d, cur_pc)) return false;

    switch (d.cls) {
        case InstClass::HINT:
            emit(block, IROp::NOP);
            return false;

        // ── MOVZ / MOVN / MOVK ───────────────────────────────────────
        case InstClass::MOVZ: {
            uint64_t val = static_cast<uint64_t>(d.imm16) << (d.hw * 16);
            if (!d.sf) val &= 0xFFFFFFFF;
            uint16_t v = load_imm(block, val);
            store_arm_reg(block, d.rd, v);
            return false;
        }
        case InstClass::MOVN: {
            uint64_t val = ~static_cast<uint64_t>(static_cast<uint64_t>(d.imm16) << (d.hw * 16));
            if (!d.sf) val &= 0xFFFFFFFF;
            uint16_t v = load_imm(block, val);
            store_arm_reg(block, d.rd, v);
            return false;
        }
        case InstClass::MOVK: {
            uint16_t cur = load_arm_reg(block, d.rd);
            uint64_t mask = ~(static_cast<uint64_t>(0xFFFF) << (d.hw * 16));
            if (!d.sf) mask |= 0xFFFFFFFF00000000ULL;
            uint16_t mv = load_imm(block, mask);
            uint16_t bits = load_imm(block, static_cast<uint64_t>(d.imm16) << (d.hw * 16));
            uint16_t masked = g_alloc.alloc();
            emit(block, IROp::AND, masked, cur, mv);
            uint16_t result = g_alloc.alloc();
            emit(block, IROp::OR, result, masked, bits);
            store_arm_reg(block, d.rd, result);
            return false;
        }

        // ── ADD/SUB (register, immediate) ────────────────────────────
        case InstClass::ADD_REG: case InstClass::ADD_IMM:
        case InstClass::SUB_REG: case InstClass::SUB_IMM: {
            // For immediate AND extended-register forms, rn=31/rd=31 can
            // mean SP (the decoder sets d.reads_sp/d.writes_sp). For the
            // shifted-register form (bit21=0), rd=31 means XZR (the
            // decoder leaves d.writes_sp=false).
            //
            // the old code only checked ADD_IMM/SUB_IMM
            // for SP mapping. ADD_REG/SUB_REG (extended register form)
            // was excluded, so `add sp, sp, x12` (very common in
            // function epilogues) was computed as `add xzr, xzr, x12`
            // and the result was discarded. This corrupted the stack
            // pointer, causing crashes in musl's mallocng during free()
            // (get_meta would see a non-16-byte-aligned pointer because
            // SP was never restored after the function's stack frame
            // allocation).
            bool rn_is_sp = (d.cls == InstClass::ADD_IMM || d.cls == InstClass::SUB_IMM ||
                             d.cls == InstClass::ADD_REG || d.cls == InstClass::SUB_REG) && d.reads_sp;
            uint16_t a = load_arm_reg(block, d.rn, rn_is_sp);
            uint8_t b;
            if (d.cls == InstClass::ADD_IMM || d.cls == InstClass::SUB_IMM) {
                // the decoder sets d.imm_u to the raw
                // 12-bit immediate and d.shift to 0 or 12 (the optional
                // left-shift by 12 for the "add/sub imm, lsl #12" form).
                // The previous code passed d.imm_u through unshifted, so
                // e.g. `add x0, x0, #0x1, lsl #12` (= add x0, x0, #0x1000)
                // was computed as `add x0, x0, #1`. This broke musl's
                // `cmn x0, #0x1, lsl #12` in __syscall_ret — the function
                // uses it to test whether a syscall return value is in
                // the [-4095, -1] error range. Without the shift, every
                // successful syscall looked like an error, and every
                // syscall was reported as -1 with bogus errno.
                b = load_imm(block, static_cast<uint64_t>(d.imm_u) << d.shift);
            } else {
                // Register form. Two sub-cases:
                //   (a) Extended register (bit21=1): apply extend type
                //       (UXTB/SXTB/UXTH/SXTH/UXTW/SXTW/UXTX/SXTX) then
                //       optional shift (0-4).
                //   (b) Shifted register (bit21=0): apply shift_type
                //       (LSL/LSR/ASR/ROR) by d.shift (0-63).
                // The decoder sets d.extend for case (a) and d.shift_type
                // for case (b). We must NOT apply extend logic in case (b)
                // even though d.shift != 0 — the shift there is a shifted-
                // register shift, not an extend shift.
                b = load_arm_reg(block, d.rm);
                if (d.extend != 0) {
                    // Extended register form — apply extend, then shift.
                    b = apply_extend(block, b, d.extend, d.shift);
                } else if (d.shift != 0 || d.shift_type != 0) {
                    b = apply_shift(block, b, d.shift_type, d.shift, d.sf);
                }
            }
            IROp op = (d.cls == InstClass::ADD_REG || d.cls == InstClass::ADD_IMM)
                      ? IROp::ADD : IROp::SUB;
            uint16_t r = g_alloc.alloc();
            emit(block, op, r, a, b);
            r = zext_if_32bit(block, r, d.sf);
            // For immediate AND extended-register forms, rd=31 writes SP
            // (when !set_flags). The decoder sets d.writes_sp accordingly.
            bool rd_is_sp = (d.cls == InstClass::ADD_IMM || d.cls == InstClass::SUB_IMM ||
                             d.cls == InstClass::ADD_REG || d.cls == InstClass::SUB_REG) && d.writes_sp;
            store_arm_reg(block, d.rd, r, rd_is_sp);
            return false;
        }

        // ── ADDS/SUBS (flag-setting) ─────────────────────────────────
        case InstClass::ADDS_REG: case InstClass::ADDS_IMM:
        case InstClass::SUBS_REG: case InstClass::SUBS_IMM: {
            // v1.5.0.alpha BUGFIX: for the EXTENDED REGISTER form of
            // ADDS/SUBS (bit21=1), Rn=31 = SP (per ARM ARM). For the
            // shifted register form (bit21=0), Rn=31 = XZR.
            // We check the raw instruction bits directly (like the
            // interpreter does) to avoid stale-field issues.
            bool rn_is_sp = false;
            if (d.cls == InstClass::ADDS_REG || d.cls == InstClass::SUBS_REG) {
                bool extended = ((d.raw & 0x1FE00000) == 0x0B200000);
                rn_is_sp = extended && (d.rn == 31);
            }
            uint16_t a = load_arm_reg(block, d.rn, rn_is_sp);
            uint8_t b;
            if (d.cls == InstClass::ADDS_IMM || d.cls == InstClass::SUBS_IMM) {
                // apply d.shift (0 or 12) to d.imm_u,
                // matching the interpreter. See ADD_IMM/SUB_IMM above
                // for the full rationale.
                b = load_imm(block, static_cast<uint64_t>(d.imm_u) << d.shift);
            } else {
                // Register form. Two sub-cases (mirrors ADD_REG/SUB_REG):
                //   (a) Extended register (bit21=1): apply extend type
                //       (UXTB/SXTB/UXTH/SXTH/UXTW/SXTW/UXTX/SXTX) then
                //       optional shift (0-4).
                //   (b) Shifted register (bit21=0): apply shift_type
                //       (LSL/LSR/ASR/ROR) by d.shift (0-63).
                //
                // the previous code ignored d.extend
                // and d.shift_type for ADDS/SUBS, so e.g.
                //   cmp x0, w24, sxtw
                // was computed as `x0 - w24` (treating w24 as unsigned
                // 32-bit, NOT sign-extended). This caused `csel x24, x0,
                //   x1, ge` in musl's vfprintf %d-zero handling to pick
                //   the wrong source, producing "" instead of "0" for
                //   printf("%d", 0). Many other CSEL-after-CMP paths in
                //   musl were similarly broken.
                b = load_arm_reg(block, d.rm);
                if (d.extend != 0) {
                    b = apply_extend(block, b, d.extend, d.shift);
                } else if (d.shift != 0 || d.shift_type != 0) {
                    b = apply_shift(block, b, d.shift_type, d.shift, d.sf);
                }
            }
            bool is_add = (d.cls == InstClass::ADDS_REG || d.cls == InstClass::ADDS_IMM);
            uint16_t r = g_alloc.alloc();
            emit(block, is_add ? IROp::ADDS : IROp::SUBS, r, a, b,
                 d.sf ? 64 : 32, 0, is_add ? 0 : 1);  // width = 32 or 64
            // CMP (SUBS XZR, ...) doesn't write Rd.
            if (d.rd != 31) {
                r = zext_if_32bit(block, r, d.sf);
                store_arm_reg(block, d.rd, r);
            }
            return false;
        }

        // ── ADC/ADCS/SBC/SBCS (with carry) ───────────────────────────
        case InstClass::ADC_REG: case InstClass::ADCS_REG:
        case InstClass::SBC_REG: case InstClass::SBCS_REG: {
            uint16_t a = load_arm_reg(block, d.rn);
            uint16_t b = load_arm_reg(block, d.rm);
            bool is_sub = (d.cls == InstClass::SBC_REG || d.cls == InstClass::SBCS_REG);
            bool set_flags = (d.cls == InstClass::ADCS_REG || d.cls == InstClass::SBCS_REG);
            uint16_t r = g_alloc.alloc();
            if (set_flags) {
                emit(block, is_sub ? IROp::SBCS : IROp::ADCS, r, a, b,
                     0, 0, is_sub ? 1 : 0);
            } else {
                // No-flag ADC/SBC: decompose into CSEL + ADD (+ NOT for SBC).
                //
                // ARM semantics:
                //   ADC: Rd = Rn + Rm + C
                //   SBC: Rd = Rn - Rm - 1 + C = Rn + ~Rm + C
                //
                // We need the C flag as a 0/1 value. CSEL with cond=CS
                // (carry set) gives us that:
                //   c_val = CSEL(1, 0, CS)  →  c_val = (C==1) ? 1 : 0
                //
                // Then:
                //   ADC:  Rd = ADD(ADD(Rn, Rm), c_val)
                //   SBC:  Rd = ADD(ADD(Rn, NOT(Rm)), c_val)
                //
                // This is 4 IR ops (1 IMM, 1 CSEL, 1 NOT for SBC, 2 ADD)
                // plus 1 IMM for the constant 0. It avoids CALL_INTERP
                // which would invalidate the entire vreg cache and force
                // a block split.
                uint16_t one  = load_imm(block, 1);
                uint16_t zero = load_imm(block, 0);
                // cond=2 (CS = carry set), flags_op=0
                uint16_t c_val = g_alloc.alloc();
                emit(block, IROp::CSEL, c_val, one, zero, 0,
                     2 /*CS*/, 0, 0, cur_pc);
                uint16_t op1;
                if (is_sub) {
                    // op1 = Rn + ~Rm
                    uint16_t not_b = g_alloc.alloc();
                    emit(block, IROp::NOT, not_b, b);
                    op1 = g_alloc.alloc();
                    emit(block, IROp::ADD, op1, a, not_b);
                } else {
                    // op1 = Rn + Rm
                    op1 = g_alloc.alloc();
                    emit(block, IROp::ADD, op1, a, b);
                }
                // r = op1 + c_val
                emit(block, IROp::ADD, r, op1, c_val);
            }
            if (d.rd != 31) {
                r = zext_if_32bit(block, r, d.sf);
                store_arm_reg(block, d.rd, r);
            }
            return false;
        }

        // ── AND/ORR/EOR/ANDS (register, immediate) ──────────────────
        // The register form also covers BIC/ORN/EON/BICS via the N bit
        // (bit 21). When N=1 the second operand is inverted before the
        // logical op. The IR translator must apply both the optional
        // shift (d.shift_type, d.shift) and the optional inversion (d.N)
        // to the register operand. Immediate forms have neither — d.N
        // there is part of the bitmask immediate encoding, not an invert.
        case InstClass::AND_REG: case InstClass::AND_IMM:
        case InstClass::ORR_REG: case InstClass::ORR_IMM:
        case InstClass::EOR_REG: case InstClass::EOR_IMM:
        case InstClass::ANDS_REG: case InstClass::ANDS_IMM: {
            uint16_t a = load_arm_reg(block, (d.rn == 31) ? 32 : d.rn);
            uint8_t b;
            bool is_imm = (d.cls == InstClass::AND_IMM || d.cls == InstClass::ORR_IMM ||
                           d.cls == InstClass::EOR_IMM || d.cls == InstClass::ANDS_IMM);
            if (is_imm) {
                b = load_imm(block, d.imm_u);
            } else {
                b = load_arm_reg(block, (d.rm == 31) ? 32 : d.rm);
                // Apply shift to the register operand.
                if (d.shift != 0 || d.shift_type != 0) {
                    b = apply_shift(block, b, d.shift_type, d.shift, d.sf);
                }
                // N=1 inverts the register operand (BIC/ORN/EON/BICS).
                if (d.N) {
                    uint16_t inverted = g_alloc.alloc();
                    emit(block, IROp::NOT, inverted, b);
                    b = inverted;
                }
            }
            IROp op;
            bool is_ands = false;
            switch (d.cls) {
                case InstClass::AND_REG: case InstClass::AND_IMM: op = IROp::AND; break;
                case InstClass::ORR_REG: case InstClass::ORR_IMM: op = IROp::OR;  break;
                case InstClass::EOR_REG: case InstClass::EOR_IMM: op = IROp::XOR; break;
                case InstClass::ANDS_REG: case InstClass::ANDS_IMM: op = IROp::AND; is_ands = true; break;
                default: op = IROp::AND; break;
            }
            if (is_ands) {
                // TST sets flags from a & b. ANDS also writes Rd.
                emit(block, IROp::TST, 0, a, b);
            }
            uint16_t r = g_alloc.alloc();
            emit(block, op, r, a, b);
            if (d.rd != 31) {
                r = zext_if_32bit(block, r, d.sf);
                store_arm_reg(block, d.rd, r);
            }
            return false;
        }

        // ── MADD / MSUB ──────────────────────────────────────────────
        case InstClass::MADD: case InstClass::MSUB: {
            uint16_t rn = load_arm_reg(block, (d.rn == 31) ? 32 : d.rn);
            uint16_t rm = load_arm_reg(block, (d.rm == 31) ? 32 : d.rm);
            uint16_t prod = g_alloc.alloc();
            emit(block, IROp::MUL, prod, rn, rm);
            uint16_t ra = load_arm_reg(block, (d.ra == 31) ? 32 : d.ra);
            uint16_t result = g_alloc.alloc();
            emit(block, d.cls == InstClass::MADD ? IROp::ADD : IROp::SUB,
                 result, ra, prod);
            result = zext_if_32bit(block, result, d.sf);
            store_arm_reg(block, d.rd, result);
            return false;
        }

        // ── LSL/LSR/ASR/ROR (register) ───────────────────────────────
        case InstClass::LSL: case InstClass::LSR:
        case InstClass::ASR: case InstClass::ROR: {
            uint16_t a = load_arm_reg(block, d.rn);
            uint16_t s = load_arm_reg(block, d.rm);
            IROp op;
            switch (d.cls) {
                case InstClass::LSL: op = IROp::SHL; break;
                case InstClass::LSR: op = IROp::SHR; break;
                case InstClass::ASR: op = IROp::SAR; break;
                case InstClass::ROR: op = IROp::ROR; break;
                default: op = IROp::SHL; break;
            }
            // 32-bit ASR: sign-extend from bit 31 before SAR, because
            // x86's 64-bit SAR looks at bit 63 (which is 0 after a
            // 32-bit register read zero-extends). Same fix as apply_shift.
            if (d.cls == InstClass::ASR && !d.sf) {
                uint16_t sext = g_alloc.alloc();
                emit(block, IROp::SEXT, sext, a, 0, 32);
                a = sext;
            }
            // 32-bit ROR: set width=32 so the JIT uses 32-bit ROR
            // (64-bit ROR on a zero-extended 32-bit value loses wrap bits).
            uint8_t width = (d.cls == InstClass::ROR && !d.sf) ? 32 : 0;
            uint16_t r = g_alloc.alloc();
            emit(block, op, r, a, s, width);
            r = zext_if_32bit(block, r, d.sf);
            store_arm_reg(block, d.rd, r);
            return false;
        }

        // ── SBFM/UBFM (bitfield extract) ───────────────────────────
        // These stay native in the JIT — pass through as single IR ops.
        // (The JIT has dedicated, well-tested codegen for them and the
        // constant folder in ir_optimize.cpp knows how to fold them
        // when the source is a known immediate.)
        //
        // Use a scratch vreg as dest (not d.rd directly) so the optimizer's
        // arm_reg_cache and liveness analysis work correctly. ARM reg
        // vregs (0-31) used as dest confuse the optimizer because they
        // represent architectural state that must be flushed via STORE_REG.
        // The store_arm_reg below emits the STORE_REG that writes the
        // result to cpu.regs[d.rd].
        case InstClass::SBFM: case InstClass::UBFM: {
            uint16_t a = load_arm_reg(block, d.rn);
            IROp op = (d.cls == InstClass::SBFM) ? IROp::SBFM
                    : IROp::UBFM;
            uint16_t r = g_alloc.alloc();
            emit_bf(block, op, r, a, 0, d.immr, d.imms, d.sf ? 1 : 0, cur_pc);
            store_arm_reg(block, d.rd, r, d.writes_sp);
            return false;
        }

        // ── EXTR (bitfield extract from concat) ────────────────────
        // EXTR Rd, Rn, Rm, #imms:
        //   Rd = (Rn:Rm) >> imms    (imms in [0, width-1])
        //
        // Decomposed into primitive IR ops the JIT already compiles
        // natively (SHL, SHR, OR), mirroring the BFM decomposition
        // strategy. This eliminates the IROp::EXTR native codegen path
        // (~45 lines of x86 in frostjit.cpp) and lets the constant
        // folder / peephole optimize the result.
        //
        //   imms == 0       → Rd = Rm                       (1 MOV)
        //   otherwise       → Rd = (Rn << (W-imms)) | (Rm >> imms)
        //                                                  (SHL+SHR+OR)
        //
        // 32-bit EXTR additionally needs a ZEXT to clear the high 32
        // bits (handled by the trailing zext_if_32bit at the end).
        case InstClass::EXTR: {
            uint16_t rn_v = load_arm_reg(block, d.rn);
            uint16_t rm_v = load_arm_reg(block, d.rm);
            int width = d.sf ? 64 : 32;
            int lsb = d.imms;  // ARM encodes the extraction point in imms

            uint16_t result;
            if (lsb == 0) {
                // Rd = Rm (low 64 bits of the concatenation).
                result = rm_v;
            } else {
                // hi_part = Rn << (width - lsb)
                uint16_t sh_hi = load_imm(block, static_cast<uint64_t>(width - lsb));
                uint16_t hi = g_alloc.alloc();
                emit(block, IROp::SHL, hi, rn_v, sh_hi);
                // lo_part = Rm >> lsb
                uint16_t sh_lo = load_imm(block, static_cast<uint64_t>(lsb));
                uint16_t lo = g_alloc.alloc();
                emit(block, IROp::SHR, lo, rm_v, sh_lo);
                // result = hi | lo
                result = g_alloc.alloc();
                emit(block, IROp::OR, result, hi, lo);
            }
            result = zext_if_32bit(block, result, d.sf);
            store_arm_reg(block, d.rd, result);
            return false;
        }

        // ── BFM (bitfield insert) ───────────────────────────────────
        // BFM Rd, Rn, #immr, #imms:
        //   mask = ROR(Ones(imms+1), immr, width)
        //   Rd = (Rd & ~mask) | (ROR(Rn, immr) & mask)
        // We compute mask and ~mask at translation time (they're immediates).
        // For the ROR(Rn, immr), we use SHL+SHR+OR which the JIT handles
        // natively for both 32 and 64-bit (avoids the ROR & 0x3F issue).
        case InstClass::BFM: {
            uint16_t rn_v = load_arm_reg(block, d.rn);
            uint16_t rd_v = load_arm_reg(block, d.rd);
            int width = d.sf ? 64 : 32;
            int immr = d.immr % width;
            int imms = d.imms;
            // Compute mask = ROR(Ones(imms+1), immr, width)
            uint64_t welem = (imms + 1 >= 64) ? ~0ULL : ((1ULL << (imms + 1)) - 1);
            if (width == 32) welem &= 0xFFFFFFFFULL;
            uint64_t mask;
            if (immr == 0) {
                mask = welem;
            } else {
                mask = (welem >> immr) | (welem << (width - immr));
                if (width == 32) mask &= 0xFFFFFFFFULL;
            }
            uint64_t notmask = ~mask & ((width == 32) ? 0xFFFFFFFFULL : ~0ULL);
            // rotated = (Rn << (width - immr)) | (Rn >> immr)
            // Use SHL and SHR with immediate amounts (JIT handles these natively).
            uint16_t rot_hi, rot_lo, rotated;
            if (immr == 0) {
                rotated = rn_v;  // no rotation needed
            } else {
                uint16_t sh_hi = load_imm(block, width - immr);
                uint16_t sh_lo = load_imm(block, immr);
                rot_hi = g_alloc.alloc();
                emit(block, IROp::SHL, rot_hi, rn_v, sh_hi);
                rot_lo = g_alloc.alloc();
                emit(block, IROp::SHR, rot_lo, rn_v, sh_lo);
                rotated = g_alloc.alloc();
                emit(block, IROp::OR, rotated, rot_hi, rot_lo);
            }
            // field = rotated & mask
            uint16_t mask_v = load_imm(block, mask);
            uint16_t field = g_alloc.alloc();
            emit(block, IROp::AND, field, rotated, mask_v);
            // cleared = Rd & ~mask
            uint16_t notmask_v = load_imm(block, notmask);
            uint16_t cleared = g_alloc.alloc();
            emit(block, IROp::AND, cleared, rd_v, notmask_v);
            // result = cleared | field
            uint16_t result = g_alloc.alloc();
            emit(block, IROp::OR, result, cleared, field);
            store_arm_reg(block, d.rd, result);
            return false;
        }

        // ── CSEL / CSINC / CSINV / CSNEG ─────────────────────────────
        // CSEL  Rd = cond ? Rn : Rm         → CSEL(Rn, Rm)
        // CSINC Rd = cond ? Rn : (Rm + 1)   → CSEL(Rn, ADD(Rm, 1))
        // CSINV Rd = cond ? Rn : ~Rm        → CSEL(Rn, NOT(Rm))
        // CSNEG Rd = cond ? Rn : -Rm        → CSEL(Rn, NEG(Rm))
        // Decompose into transform + CSEL so the JIT only needs native
        // CSEL (which it has) — no CALL_INTERP for CSINC/CSINV/CSNEG.
        case InstClass::CSEL: case InstClass::CSINC:
        case InstClass::CSINV: case InstClass::CSNEG: {
            uint16_t rn_v = load_arm_reg(block, d.rn);
            uint16_t rm_v = load_arm_reg(block, d.rm);
            uint16_t sel_src2 = rm_v;
            if (d.cls == InstClass::CSINC) {
                uint16_t one = load_imm(block, 1);
                uint16_t inc = g_alloc.alloc();
                emit(block, IROp::ADD, inc, rm_v, one);
                sel_src2 = inc;
            } else if (d.cls == InstClass::CSINV) {
                uint16_t inv = g_alloc.alloc();
                emit(block, IROp::NOT, inv, rm_v);
                sel_src2 = inv;
            } else if (d.cls == InstClass::CSNEG) {
                uint16_t neg = g_alloc.alloc();
                emit(block, IROp::NEG, neg, rm_v);
                sel_src2 = neg;
            }
            // CSEL: dest = cond ? rn : sel_src2
            uint16_t r = g_alloc.alloc();
            emit(block, IROp::CSEL, r, rn_v, sel_src2, 0, d.cond, 0, d.rd, cur_pc);
            r = zext_if_32bit(block, r, d.sf);
            store_arm_reg(block, d.rd, r);
            return false;
        }

        // ── CCMP / CCMN ──────────────────────────────────────────────
        case InstClass::CCMP: case InstClass::CCMN: {
            uint16_t rn_v = load_arm_reg(block, d.rn);
            uint8_t rm_v;
            if (d.is_register) {
                rm_v = load_arm_reg(block, d.rm);
            } else {
                rm_v = load_imm(block, d.imm_u);
            }
            bool is_sub = (d.cls == InstClass::CCMP);
            // CCMP: if cond then set flags from rn - rm else set imm nzcv.
            emit(block, IROp::CCMP, 0, rn_v, rm_v, d.nzcv_field,
                 d.cond, is_sub ? 1 : 0, 0, cur_pc);
            // BUGFIX (Turn 56): store the 32/64-bit width in the sf field
            // so the JIT can emit the correct sub/add width. Without this,
            // the JIT always uses 64-bit sub, which computes the Sign Flag
            // from bit 63 instead of bit 31 for 32-bit CCMP — causing the
            // N flag to be wrong when the 32-bit result is negative but
            // the 64-bit result is positive (e.g., w3=0xFFFFFFFF, w3-2 =
            // 0xFFFFFFFD: 32-bit N=1, 64-bit N=0). This was the root cause
            // of the curl --version crash: a 32-bit ccmp w3, #2, #0, cs
            // computed wrong N, which caused a downstream conditional
            // branch to take the wrong path, leading to a NULL deref.
            block.insts.back().sf = d.sf ? 1 : 0;
            return false;
        }

        // ── 1-source data processing: CLZ/CLS/RBIT/REV* ──────────────
        //
        // Strategy:
        //   CLZ    → native IROp::CLZ     (x86 LZCNT, 1 instr)
        //   CLS    → decompose (see below)
        //   RBIT   → decompose via SWAR (6 stages for 64-bit, 5 for 32-bit)
        //   REV16  → decompose via SWAR (1 stage: byte-swap in 16-bit lanes)
        //   REV32  → decompose via SWAR (2 stages: REV16 + halfword-swap)
        //   REV    → native IROp::REV64   (x86 BSWAP, 1 instr)
        //
        // The SWAR decompositions use only AND/OR/SHL/SHR primitives that
        // the JIT already compiles natively, eliminating the CALL_INTERP
        // fallback that previously handled RBIT/REV16/REV32. CLZ and REV64
        // stay native because x86 has single-instruction equivalents
        // (LZCNT / BSWAP) — decomposing them would be strictly worse.
        case InstClass::CLZ: {
            uint16_t a = load_arm_reg(block, d.rn);
            uint16_t r = g_alloc.alloc();
            emit(block, IROp::CLZ, r, a, 0, d.sf ? 64 : 32, 0, 0, d.rd, cur_pc);
            r = zext_if_32bit(block, r, d.sf);
            store_arm_reg(block, d.rd, r);
            return false;
        }

        case InstClass::REV: {  // REV (64-bit byte-swap) — native BSWAP
            uint16_t a = load_arm_reg(block, d.rn);
            uint16_t r = g_alloc.alloc();
            emit(block, IROp::REV64, r, a, 0, d.sf ? 64 : 32, 0, 0, d.rd, cur_pc);
            r = zext_if_32bit(block, r, d.sf);
            store_arm_reg(block, d.rd, r);
            return false;
        }

        // CLS: count leading sign bits.
        //
        // ARM semantics:
        //   CLS(v) = CLZ(v ^ SAR(v, W-1)) - 1
        //
        // Proof: SAR(v, W-1) arithmetic-shifts the sign bit into all
        // positions. So:
        //   - If v >= 0 (sign bit 0): SAR(v, W-1) = 0, and v ^ 0 = v.
        //     CLZ(v) - 1 = (leading zeros) - 1 = (leading sign bits).
        //   - If v <  0 (sign bit 1): SAR(v, W-1) = ~0 (all ones), and
        //     v ^ ~0 = ~v. CLZ(~v) - 1 = (leading ones in v) - 1.
        //   - v == 0: SAR(0, W-1) = 0, XOR = 0, CLZ(0) = W, W - 1 = W-1.
        //     ARM says CLS(0) = W; we'd return W-1. But wait — the ARM
        //     pseudocode is:
        //       CLS: result = CLZ(if v<w-1> == '1' then NOT(v) else v) - 1
        //     For v = 0: CLZ(0) - 1 = W - 1, but ARM spec says CLS(0) = W.
        //     Re-checking the ARM ARM... actually CLS(0) = W-1, not W.
        //     The pseudocode result is W-1 for v=0 (CLZ(0)=W, minus 1).
        //     My earlier reasoning was wrong. So the formula is exact.
        //   - v == ~0: SAR(~0, W-1) = ~0, XOR = 0, CLZ(0) = W, W - 1 = W-1.
        //     ARM pseudocode: v<w-1>=1 so use NOT(v)=0; CLZ(0)-1 = W-1. ✓
        //
        // Decomposition (5 IR ops including 1 IMM):
        //   sh     = IMM (W-1)
        //   sign   = SAR(v, sh)
        //   xored  = XOR(v, sign)
        //   clz    = CLZ(xored)
        //   result = SUB(clz, IMM 1)
        //
        // Uses only primitive IR ops the JIT already compiles natively
        // (SAR via shift, XOR, CLZ via LZCNT, SUB). Avoids CALL_INTERP
        // and the block-split it triggers.
        case InstClass::CLS: {
            uint16_t v = load_arm_reg(block, d.rn);
            int width = d.sf ? 64 : 32;
            // 32-bit CLS: SEXT the input first so the high bits match
            // the sign bit (SAR by 31 will then correctly produce all-1s
            // or all-0s). Using ZEXT was a bug: it clears the high bits,
            // so x86's 64-bit SAR sees bit 63=0 and treats negative
            // values as positive, breaking CLS for any value with the
            // sign bit set (e.g., CLS(-1) returned -1 instead of 31).
            if (!d.sf) {
                uint16_t z = g_alloc.alloc();
                emit(block, IROp::SEXT, z, v, 0, 32);
                v = z;
            }
            uint16_t sh = load_imm(block, static_cast<uint64_t>(width - 1));
            uint16_t sign = g_alloc.alloc();
            emit(block, IROp::SAR, sign, v, sh);
            uint16_t xored = g_alloc.alloc();
            emit(block, IROp::XOR, xored, v, sign);
            uint16_t clz = g_alloc.alloc();
            emit(block, IROp::CLZ, clz, xored, 0, static_cast<uint8_t>(width));
            uint16_t one = load_imm(block, 1);
            uint16_t r = g_alloc.alloc();
            emit(block, IROp::SUB, r, clz, one);
            r = zext_if_32bit(block, r, d.sf);
            store_arm_reg(block, d.rd, r);
            return false;
        }

        case InstClass::RBIT: {
            uint16_t a = load_arm_reg(block, d.rn);
            uint16_t r;
            if (d.sf) {
                r = rbit64_ir(block, a);
            } else {
                // 32-bit RBIT: ZEXT input to clear high bits, decompose,
                // ZEXT result.
                uint16_t z = g_alloc.alloc();
                emit(block, IROp::ZEXT, z, a, 0, 32);
                r = rbit32_ir(block, z);
                r = zext_if_32bit(block, r, /*sf=*/false);
            }
            store_arm_reg(block, d.rd, r);
            return false;
        }

        case InstClass::REV16: {
            uint16_t a = load_arm_reg(block, d.rn);
            uint16_t r = rev16_64_ir(block, a);
            r = zext_if_32bit(block, r, d.sf);
            store_arm_reg(block, d.rd, r);
            return false;
        }

        case InstClass::REV32: {
            uint16_t a = load_arm_reg(block, d.rn);
            uint16_t r = rev32_64_ir(block, a);
            r = zext_if_32bit(block, r, d.sf);
            store_arm_reg(block, d.rd, r);
            return false;
        }

        // ── UDIV / SDIV ──────────────────────────────────────────────
        case InstClass::UDIV: case InstClass::SDIV: {
            uint16_t a = load_arm_reg(block, d.rn);
            uint16_t b = load_arm_reg(block, d.rm);
            uint16_t r = g_alloc.alloc();
            emit(block, d.cls == InstClass::UDIV ? IROp::UDIV : IROp::SDIV,
                 r, a, b, d.sf ? 64 : 32, 0, 0, 0, cur_pc);
            r = zext_if_32bit(block, r, d.sf);
            store_arm_reg(block, d.rd, r);
            return false;
        }

        // ── B / BL ───────────────────────────────────────────────────
        case InstClass::B: case InstClass::BL: {
            if (d.cls == InstClass::BL) {
                // Turn 92: BL_CALL — BL within block.
                // DISABLED: translate_block during execution corrupts JIT
                // codegen state. The jit_call_helper calls translate_and_lookup
                // which calls translate_block, but translate_block uses
                // member variables (code_buf_used_, vreg_home_[], etc.)
                // that may conflict with the caller's block state.
                // Needs save/restore of JIT codegen state around the
                // translate_block call. Fall back to old behavior (end block).
                if (false) {
                    uint16_t lr = load_imm(block, cur_pc + 4);
                    store_arm_reg(block, 30, lr);
                    uint64_t target = cur_pc + d.imm;
                    emit(block, IROp::BL_CALL, 0, 0, 0, 0, 0, 0, target, cur_pc);
                    return false;
                }
                // Fallback: end block at BL (old behavior).
                uint16_t lr = load_imm(block, cur_pc + 4);
                store_arm_reg(block, 30, lr);
                uint64_t target = cur_pc + d.imm;
                emit(block, IROp::BRCOND_FALLTHRU, 0, 0, 0, 0, 14 /*AL*/, 0, target, cur_pc);
                block.ends_with_branch = true;
                return true;
            }
            uint64_t target = cur_pc + d.imm;
            // B is unconditional — encode as BRCOND with cond=AL (always).
            // Using BRCOND_FALLTHRU signals to the executor/codegen that
            // the branch is unconditional and there's no fall-through.
            emit(block, IROp::BRCOND_FALLTHRU, 0, 0, 0, 0, 14 /*AL*/, 0, target, cur_pc);
            block.ends_with_branch = true;
            return true;
        }

        // ── BR / BLR ────────────────────────────────────────────────
        case InstClass::BR: case InstClass::BLR: {
            if (d.cls == InstClass::BLR) {
                uint16_t lr = load_imm(block, cur_pc + 4);
                store_arm_reg(block, 30, lr);
            }
            uint16_t target = load_arm_reg(block, d.rn);
            emit(block, IROp::BR, 0, target);
            block.ends_with_branch = true;
            return true;
        }

        // ── RET ─────────────────────────────────────────────────────
        case InstClass::RET: {
            uint16_t target = load_arm_reg(block, d.rn);
            emit(block, IROp::BR, 0, target);
            block.ends_with_branch = true;
            return true;
        }

        // ── Bcond ───────────────────────────────────────────────────
        case InstClass::Bcond: {
            uint64_t target = cur_pc + d.imm;
            emit(block, IROp::BRCOND, 0, 0, 0, 0, d.cond, 0, target, cur_pc);
            block.ends_with_branch = true;
            return true;
        }

        // ── CBZ / CBNZ ──────────────────────────────────────────────
        // Per ARM ARM, CBZ/CBNZ do NOT modify any flags. They branch
        // based on whether the register is zero. We model this with a
        // dedicated BRCOND_ZERO op that branches directly on (val == 0)
        // without touching the flag state. cond=0 (EQ) for CBZ,
        // cond=1 (NE) for CBNZ.
        case InstClass::CBZ: case InstClass::CBNZ: {
            uint16_t val = load_arm_reg(block, d.rt);
            uint64_t target = cur_pc + d.imm;
            uint8_t cond = (d.cls == InstClass::CBZ) ? 0 /*EQ*/ : 1 /*NE*/;
            emit(block, IROp::BRCOND_ZERO, 0, val, 0, 0, cond, 0, target, cur_pc);
            block.ends_with_branch = true;
            return true;
        }

        // ── TBZ / TBNZ ──────────────────────────────────────────────
        // Per ARM ARM, TBZ/TBNZ do NOT modify flags. They branch based
        // on whether bit `bit` of `rt` is zero (TBZ) or one (TBNZ).
        // We use BRCOND_BIT which tests the bit directly without
        // touching flags. cond=0 (EQ) for TBZ, cond=1 (NE) for TBNZ.
        // imm = branch target, width = bit number (0-63).
        case InstClass::TBZ: case InstClass::TBNZ: {
            uint16_t val = load_arm_reg(block, d.rt);
            uint64_t target = cur_pc + d.imm;
            uint8_t cond = (d.cls == InstClass::TBZ) ? 0 /*EQ*/ : 1 /*NE*/;
            uint8_t bit = static_cast<uint8_t>(d.imm_u & 0x3F);
            emit(block, IROp::BRCOND_BIT, 0, val, 0, bit, cond, 0, target, cur_pc);
            block.ends_with_branch = true;
            return true;
        }

        // ── ADR / ADRP ──────────────────────────────────────────────
        case InstClass::ADR: {
            uint16_t r = load_imm(block, cur_pc + d.imm);
            store_arm_reg(block, d.rd, r);
            return false;
        }
        case InstClass::ADRP: {
            uint16_t r = load_imm(block, (cur_pc & ~0xFFFULL) + d.imm);
            store_arm_reg(block, d.rd, r);
            return false;
        }

        // ── SVC ─────────────────────────────────────────────────────
        case InstClass::SVC:
        case InstClass::SVC_IMM:
            emit(block, IROp::SVC, 0, 0, 0, 0, 0, 0, 0, cur_pc);
            return true;  // ends block (syscall may modify PC)

        // ── MSR / MRS (system reg access) ───────────────────────────
        case InstClass::MSR: case InstClass::MSR_SYS:
        case InstClass::MRS: case InstClass::MRS_SYS: {
            // Encode the system register into imm for the JIT to decode.
            // The JIT handles TPIDR_EL0, TPIDRRO_EL0, NZCV, FPCR, FPSR
            // natively; unknown registers fall back to the interpreter.
            uint64_t sys_idx = (static_cast<uint64_t>(d.sys_op1) << 16) |
                               (static_cast<uint64_t>(d.sys_crn) << 12) |
                               (static_cast<uint64_t>(d.sys_crm) << 8) |
                               (static_cast<uint64_t>(d.sys_op2) << 4) |
                               d.sys_op0;
            bool is_read = (d.cls == InstClass::MRS || d.cls == InstClass::MRS_SYS);
            if (is_read) {
                uint16_t r = g_alloc.alloc();
                emit(block, IROp::MRS, r, 0, 0, 0, 0, 0, sys_idx, cur_pc);
                store_arm_reg(block, d.rt, r);
            } else {
                uint16_t v = load_arm_reg(block, d.rt);
                emit(block, IROp::MSR, 0, v, 0, 0, 0, 0, sys_idx, cur_pc);
            }
            return false;
        }

        // ── BRK / HLT (terminators) ─────────────────────────────────
        case InstClass::BRK: case InstClass::BRK_IMM:
        case InstClass::HLT: case InstClass::HLT_IMM:
            emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
            return true;

        // ── CLREX / BARRIER ─────────────────────────────────────────
        case InstClass::CLREX: case InstClass::CLREX_INST:
        case InstClass::BARRIER:
            emit(block, IROp::NOP);
            return false;

        // ── SMADDL / UMADDL (widening multiply-accumulate) ─────────
        case InstClass::SMADDL: case InstClass::UMADDL: {
            uint16_t a = load_arm_reg(block, d.rn);
            uint16_t b = load_arm_reg(block, d.rm);
            // BUGFIX (Turn 66): load the accumulator as a vreg and pass
            // it via the `aux` field. The old approach passed the ARM
            // register index in `cond` and read cpu.regs[] directly in
            // the JIT, bypassing the vreg cache. If the accumulator
            // register was modified earlier in the same block, the new
            // value was still in a dirty vreg and cpu.regs[] held the
            // stale value, causing divergent results.
            uint16_t acc = load_arm_reg(block, d.ra);
            uint16_t r = g_alloc.alloc();
            emit_aux(block, d.cls == InstClass::SMADDL ? IROp::SMADDL : IROp::UMADDL,
                     r, a, b, acc, cur_pc);
            store_arm_reg(block, d.rd, r);
            return false;
        }

        // ── SMSUBL / UMSUBL / SMULH / UMULH ─────────────────────────
        case InstClass::SMSUBL:
        case InstClass::UMSUBL: {
            uint16_t a = load_arm_reg(block, d.rn);
            uint16_t b = load_arm_reg(block, d.rm);
            // BUGFIX (Turn 66): load accumulator as vreg via `aux` field.
            uint16_t acc = load_arm_reg(block, d.ra);
            uint16_t r = g_alloc.alloc();
            emit_aux(block, d.cls == InstClass::SMSUBL ? IROp::SMSUBL : IROp::UMSUBL,
                     r, a, b, acc, cur_pc);
            store_arm_reg(block, d.rd, r);
            return false;
        }
        case InstClass::SMULH: case InstClass::UMULH: {
            uint16_t a = load_arm_reg(block, d.rn);
            uint16_t b = load_arm_reg(block, d.rm);
            uint16_t r = g_alloc.alloc();
            emit(block, d.cls == InstClass::SMULH ? IROp::SMULH : IROp::UMULH,
                 r, a, b, 0, 0, 0, 0, cur_pc);
            store_arm_reg(block, d.rd, r);
            return false;
        }

        // ── Everything else: inline interpreter call (no block split) ──
        default:
            emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
            return false;
    }
}

} // namespace arm64emu
