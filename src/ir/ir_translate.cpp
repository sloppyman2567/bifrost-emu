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

// ── Translator ──────────────────────────────────────────────────────────
bool translate_to_ir(IRBlock& block, const DecodedInst& d, uint64_t cur_pc) {
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
            // For immediate forms with !set_flags, rn=31 reads SP.
            // But ADDS/SUBS always set flags (set_flags=true), so rn=31 = XZR.
            // (CMP = SUBS XZR — rn=31 = XZR here too.)
            uint16_t a = load_arm_reg(block, d.rn, false);
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
            uint16_t r = g_alloc.alloc();
            emit(block, op, r, a, s);
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
            // 32-bit CLS: ZEXT the input first so the high bits are 0
            // (SAR by 31 will then correctly sign-extend within the
            // low 32 bits, and CLZ with width=32 will count only the
            // low 32 bits).
            if (!d.sf) {
                uint16_t z = g_alloc.alloc();
                emit(block, IROp::ZEXT, z, v, 0, 32);
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

        // ── LDR/STR (all forms) ──────────────────────────────────────
        case InstClass::LDR_IMM: case InstClass::LDR_UNS: case InstClass::LDR_REG:
        case InstClass::LDRSW: case InstClass::LDRSB: case InstClass::LDRSH:
        case InstClass::STR_IMM: case InstClass::STR_UNS: case InstClass::STR_REG: {
            // vector loads/stores (LDR/STR Q/D/S/H/B with
            // is_vec=true) must fall back to the interpreter. The IR
            // translator's load/store code uses d.rt as a general-purpose
            // register index (cpu.regs[d.rt]), but for vector instructions
            // d.rt refers to a vector register (V0-V31). Treating a vector
            // load/store as an integer one corrupts the wrong register —
            // e.g. `str q0, [sp, #32]` would store X0's value instead of
            // Q0's, and `ldr q0, [sp, #32]` would load into X0 instead of
            // Q0. This broke musl's __fixunstfsi/__extenddftf2 which spill
            // 128-bit long doubles to the stack via `str q0` / `ldp x0,x1`.
            if (d.is_vec) {
                emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
                return false;
            }
            bool is_load = (d.cls == InstClass::LDR_IMM || d.cls == InstClass::LDR_UNS ||
                            d.cls == InstClass::LDR_REG || d.cls == InstClass::LDRSW ||
                            d.cls == InstClass::LDRSB || d.cls == InstClass::LDRSH);
            int width = 1 << d.size;
            uint16_t base = load_arm_reg(block, d.rn, true);  // base reg 31 = SP
            // ── Compute the load/store address ──
            // For post-index (mode=1): load/store from `base`, writeback `base + disp`.
            // For pre-index  (mode=2): load/store from `base + disp`, writeback `base + disp`.
            // For offset     (mode=0): load/store from `base + disp`, no writeback.
            uint8_t addr;
            bool post_index = (d.mode == 1 && d.writeback);
            if (d.cls == InstClass::LDR_REG || d.cls == InstClass::STR_REG) {
                // Register offset: addr = base + extend_reg(rm, option, S ? size : 0)
                uint16_t idx = load_arm_reg(block, d.rm);
                // Apply extend (UXTB..SXTX). For LDR/STR register-offset,
                // the shift is d.size if d.shift&1 is set, else 0.
                uint8_t shift_amt = (d.shift & 1) ? d.size : 0;
                uint16_t ext = apply_extend(block, idx, d.extend, shift_amt);
                addr = g_alloc.alloc();
                emit(block, IROp::ADD, addr, base, ext);
            } else if (post_index) {
                // Post-index: load/store from base (no offset).
                addr = base;
            } else {
                // for offset and pre-index
                // modes, fold the displacement into the LOAD_MEM/STORE_MEM
                // imm field instead of emitting a separate IMM+ADD. The
                // executor and JIT both handle `mem[base + imm]` directly.
                // This cuts 2 IR ops per load/store and lets the optimizer
                // skip the address computation entirely.
                addr = base;
            }
            // Compute the mem op's immediate offset.
            // For post-index: 0 (load from base, writeback handles disp).
            // For offset/pre-index: disp (load from base+disp).
            int64_t mem_off = post_index ? 0 : d.disp;
            if (is_load) {
                uint16_t val = g_alloc.alloc();
                emit(block, IROp::LOAD_MEM, val, addr, 0, static_cast<uint8_t>(width),
                     0, 0, static_cast<uint64_t>(mem_off));
                // Sign-extend check: for non-vector loads, opc_ls bit 2
                // (i.e. opc_ls & 2) indicates LDRSW/LDRSB/LDRSH (sign-
                // extending loads). The decoder does NOT set d.cls to
                // LDRSW/LDRSB/LDRSH — it leaves the class as LDR_IMM/
                // LDR_UNS/LDR_REG and uses d.opc_ls to distinguish
                // sign-extended loads. (Matching the interpreter, which
                // checks `opc_ls & 2` directly.)
                bool sign_ext = !d.is_vec && (d.opc_ls & 2);
                if (sign_ext) {
                    uint16_t ext = g_alloc.alloc();
                    emit(block, IROp::SEXT, ext, val, 0, static_cast<uint8_t>(width * 8));
                    store_arm_reg(block, d.rt, ext);
                } else if (width < 8) {
                    uint16_t ext = g_alloc.alloc();
                    emit(block, IROp::ZEXT, ext, val, 0, static_cast<uint8_t>(width * 8));
                    store_arm_reg(block, d.rt, ext);
                } else {
                    store_arm_reg(block, d.rt, val);
                }
            } else {
                uint16_t val = load_arm_reg(block, d.rt);
                emit(block, IROp::STORE_MEM, 0, addr, val, static_cast<uint8_t>(width),
                     0, 0, static_cast<uint64_t>(mem_off));
            }
            // Writeback.
            if (d.writeback) {
                // For load/store, rn=31 means SP (not XZR).
                bool rn_is_sp = (d.rn == 31);
                if (post_index) {
                    // rn = base + disp
                    uint16_t off = load_imm(block, static_cast<uint64_t>(d.disp));
                    uint16_t new_base = g_alloc.alloc();
                    emit(block, IROp::ADD, new_base, base, off);
                    store_arm_reg(block, d.rn, new_base, rn_is_sp);
                } else {
                    // Pre-index: rn = base + disp.
                    uint16_t off = load_imm(block, static_cast<uint64_t>(d.disp));
                    uint16_t new_base = g_alloc.alloc();
                    emit(block, IROp::ADD, new_base, base, off);
                    store_arm_reg(block, d.rn, new_base, rn_is_sp);
                }
            }
            return false;
        }

        // ── LDP/STP ──────────────────────────────────────────────────
        // Native IR translation for GPR pair load/store.
        // SIMD LDP/STP (is_vec=true) still falls back to interpreter.
        case InstClass::LDP: case InstClass::STP: {
            if (d.is_vec) {
                emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
                return false;
            }
            // GPR LDP/STP: decode esize from opc (bits[31:30])
            uint8_t opc = (d.raw >> 30) & 3;
            int esize = (opc == 2) ? 8 : 4;
            int width = esize;
            bool is_load = d.is_load;
            uint16_t base = load_arm_reg(block, d.rn, true);  // rn=31 → SP
            // Compute address based on addressing mode.
            // d.mode: 1=post-index, 2=signed offset, 3=pre-index
            bool post_index = (d.mode == 1);
            bool pre_index = (d.mode == 3);
            uint16_t addr;
            if (post_index) {
                // Load/store from base (no offset), writeback base+disp
                addr = base;
            } else {
                // Offset or pre-index: addr = base + disp
                // Use LOAD_MEM/STORE_MEM imm field for the displacement.
                addr = base;
            }
            int64_t mem_off = post_index ? 0 : d.disp;
            if (is_load) {
                uint16_t val1 = g_alloc.alloc();
                emit(block, IROp::LOAD_MEM, val1, addr, 0, static_cast<uint8_t>(width),
                     0, 0, static_cast<uint64_t>(mem_off));
                // Sign-extend or zero-extend if needed (for 32-bit)
                if (width < 8) {
                    uint16_t ext1 = g_alloc.alloc();
                    emit(block, IROp::ZEXT, ext1, val1, 0, static_cast<uint8_t>(width * 8));
                    store_arm_reg(block, d.rt, ext1);
                } else {
                    store_arm_reg(block, d.rt, val1);
                }
                uint16_t val2 = g_alloc.alloc();
                emit(block, IROp::LOAD_MEM, val2, addr, 0, static_cast<uint8_t>(width),
                     0, 0, static_cast<uint64_t>(mem_off + esize));
                if (width < 8) {
                    uint16_t ext2 = g_alloc.alloc();
                    emit(block, IROp::ZEXT, ext2, val2, 0, static_cast<uint8_t>(width * 8));
                    store_arm_reg(block, d.rt2, ext2);
                } else {
                    store_arm_reg(block, d.rt2, val2);
                }
            } else {
                // STP: store rt, rt2
                uint16_t val1 = load_arm_reg(block, d.rt);
                uint16_t val2 = load_arm_reg(block, d.rt2);
                emit(block, IROp::STORE_MEM, 0, addr, val1, static_cast<uint8_t>(width),
                     0, 0, static_cast<uint64_t>(mem_off));
                emit(block, IROp::STORE_MEM, 0, addr, val2, static_cast<uint8_t>(width),
                     0, 0, static_cast<uint64_t>(mem_off + esize));
            }
            // Writeback
            if (d.writeback || post_index || pre_index) {
                bool rn_is_sp = (d.rn == 31);
                uint16_t off = load_imm(block, static_cast<uint64_t>(d.disp));
                uint16_t new_base = g_alloc.alloc();
                emit(block, IROp::ADD, new_base, base, off);
                store_arm_reg(block, d.rn, new_base, rn_is_sp);
            }
            return false;
        }

        // ── B / BL ───────────────────────────────────────────────────
        case InstClass::B: case InstClass::BL: {
            if (d.cls == InstClass::BL) {
                uint16_t lr = load_imm(block, cur_pc + 4);
                store_arm_reg(block, 30, lr);
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

        // ── Atomics (LDXR/STXR/LDAR/STLR/LSE_ATOMIC) ───────────────
        case InstClass::LDXR: case InstClass::STXR:
        case InstClass::LDAXR: case InstClass::STLXR:
        case InstClass::LDAR: case InstClass::STLR:
        case InstClass::LSE_ATOMIC:
            emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
            return false;

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

        case InstClass::FMOV_VD1: {
            // FMOV Vd.D[1], Rn → v_hi[Vd] = regs[Rn]
            uint16_t val = load_arm_reg(block, d.rn);
            emit(block, IROp::FMOV_G2FHI, d.rd, val, 0, 0, 0, 0, 0, cur_pc);
            return false;
        }

        case InstClass::FMOV_RVD1: {
            // FMOV Rn, Vm.D[1] → regs[Rn] = v_hi[Vm]
            uint16_t v = g_alloc.alloc();
            emit(block, IROp::FMOV_FHI2G, v, d.rn, 0, 0, 0, 0, 0, cur_pc);
            store_arm_reg(block, d.rd, v);
            return false;
        }

        // ── FP_SCALAR — native FP arithmetic ────────────────────────
        // Decode specific FP op from raw bits and emit native IR ops.
        // Falls back to CALL_INTERP for ops we don't handle natively.
        case InstClass::FP_SCALAR: {
            uint32_t op = d.raw;
            uint8_t ftype = (op >> 22) & 3;
            uint8_t rd = op & 0x1F;
            uint8_t rn = (op >> 5) & 0x1F;
            uint8_t rm = (op >> 16) & 0x1F;
            uint8_t opcode = (op >> 12) & 0xF;

            // FMOV (general ↔ FP, 64-bit): may reach here via FP_SCALAR.
            // Bit[18]=1 distinguishes FMOV from SCVTF/UCVTF (bit[18]=0).
            if ((op & 0xFFE0FC00) == 0x9E600000 && (op & (1u << 18))) {
                bool to_fp = (op >> 16) & 1;
                if (to_fp) {
                    uint16_t val = load_arm_reg(block, rn);
                    emit(block, IROp::FMOV_G2F, rd, val, 0, 0, 0, 0, 0, cur_pc);
                } else {
                    uint16_t v = g_alloc.alloc();
                    emit(block, IROp::FMOV_F2G, v, rn, 0, 0, 0, 0, 0, cur_pc);
                    store_arm_reg(block, rd, v);
                }
                return false;
            }
            // FMOV (general ↔ FP, 32-bit): native path.
            // Encoding: 0x1E200000 with bit 16 = to_fp (1) or to_gpr (0).
            // Bit[18]=1 distinguishes FMOV from SCVTF/UCVTF (bit[18]=0),
            // mirroring the 64-bit check above. Without this guard,
            // `scvtf s0, w0` (0x1E220000) and `ucvtf s0, w0` (0x1E230000)
            // match this mask and get misdecoded as a raw GPR↔FP bit copy.
            // FMOV Sn, Wn → v_lo[rd] = (uint32_t)regs[rn]; v_hi[rd] = 0
            // FMOV Wd, Sn → regs[rd] = (uint32_t)v_lo[rn]
            if ((op & 0xFFE0FC00) == 0x1E200000 && (op & (1u << 18))) {
                bool to_fp = (op >> 16) & 1;
                if (to_fp) {
                    // Wn → Sn: mask GPR to 32 bits before storing to v_lo[rd].
                    uint16_t val = load_arm_reg(block, rn);
                    uint16_t mask = load_imm(block, 0xFFFFFFFFULL);
                    uint16_t masked = g_alloc.alloc();
                    emit(block, IROp::AND, masked, val, mask);
                    emit(block, IROp::FMOV_G2F, rd, masked, 0, 0, 0, 0, 0, cur_pc);
                } else {
                    // Sn → Wd: load v_lo[rn] (full 64 bits), mask to 32 bits.
                    uint16_t v = g_alloc.alloc();
                    emit(block, IROp::FMOV_F2G, v, rn, 0, 0, 0, 0, 0, cur_pc);
                    uint16_t mask = load_imm(block, 0xFFFFFFFFULL);
                    uint16_t masked = g_alloc.alloc();
                    emit(block, IROp::AND, masked, v, mask);
                    store_arm_reg(block, rd, masked);
                }
                return false;
            }
            // FMOV (FP↔FP register): native path.
            // Encoding: 0x1E604000 (double) or 0x1E204000 (single).
            // FMOV Dd, Dn → v_lo[rd] = v_lo[rn]; v_hi[rd] = v_hi[rn]
            // FMOV Sd, Sn → v_lo[rd] = v_lo[rn] (low 32 bits); v_hi[rd] = 0
            // We use FMOV_G2F/F2G via a GPR scratch to avoid adding a new IR op.
            // For double: load v_lo[rn] into GPR, store to v_lo[rd]; same for v_hi.
            // For single: load v_lo[rn] into GPR, mask to 32 bits, store to v_lo[rd]; v_hi[rd] = 0.
            if ((op & 0xFFFFFC00) == 0x1E604000) {
                // Double-precision FP register move.
                uint16_t lo = g_alloc.alloc();
                emit(block, IROp::FMOV_F2G, lo, rn, 0, 0, 0, 0, 0, cur_pc);
                emit(block, IROp::FMOV_G2F, rd, lo, 0, 0, 0, 0, 0, cur_pc);
                uint16_t hi = g_alloc.alloc();
                emit(block, IROp::FMOV_FHI2G, hi, rn, 0, 0, 0, 0, 0, cur_pc);
                emit(block, IROp::FMOV_G2FHI, rd, hi, 0, 0, 0, 0, 0, cur_pc);
                return false;
            }
            if ((op & 0xFFFFFC00) == 0x1E204000) {
                // Single-precision FP register move (zeroes upper bits).
                uint16_t lo = g_alloc.alloc();
                emit(block, IROp::FMOV_F2G, lo, rn, 0, 0, 0, 0, 0, cur_pc);
                uint16_t mask = load_imm(block, 0xFFFFFFFFULL);
                uint16_t masked = g_alloc.alloc();
                emit(block, IROp::AND, masked, lo, mask);
                emit(block, IROp::FMOV_G2F, rd, masked, 0, 0, 0, 0, 0, cur_pc);
                return false;
            }

            // FCMP/FCMPE — uses shared fp_decode helper.
            //
            // The #0.0 form vs register form is distinguished by bits[4:0]:
            //   #0.0 form:    bits[4:0] = 0b01000
            //   register form: bits[4:0] = 0b00000, rm in bits[20:16]
            //
            // We pass src2 = rm for the register form (including rm == 0,
            // which means "compare against d0"), and src2 = 0 plus a
            // sentinel bit in the `imm` field (bit 0) for the #0.0 form.
            // The JIT checks `inst.imm & 1` to distinguish the two cases;
            // without this sentinel, FCMP Dn, D0 would be confused with
            // FCMP Dn, #0.0 because both have IR src2 == 0.
            if (fp_decode::is_fcmp(op)) {
                bool with_zero = fp_decode::fcmp_with_zero(op);
                if (with_zero) {
                    emit(block, IROp::FP_CMP, 0, rn, 0, ftype,
                         0, 0, /*imm=*/1, cur_pc);
                } else {
                    emit(block, IROp::FP_CMP, 0, rn, rm, ftype,
                         0, 0, /*imm=*/0, cur_pc);
                }
                return false;
            }
            // FP arithmetic (2-source): bit[21]=1, bits[11:10]=0b10.
            //
            // The ARM ARM distinguishes FP 2-source (FMUL/FADD/etc.) from
            // FP→int (FCVTZS/FCVTZU) and int→FP (SCVTF/UCVTF) conversions
            // by bits[11:10]: 2-source ops have bits[11:10]=0b10, while
            // conversions have bits[11:10]=0b00 (with bits[15:10]=0b000000).
            //
            // The old check only excluded bits[15:10] in {0x04, 0x08, 0x10,
            // 0x14} (FMOV imm / FCMP / FP 1-source / FMOV imm alternate) —
            // but those all have bit[21]=0, so the bit[21]=1 check already
            // excluded them. The real collision was with FCVTZS/SCVTF,
            // which have bit[21]=1 AND bits[15:10]=0x00, matching the old
            // check and causing `fcvtzs w1, d0` (0x1e780001) to be
            // misdecoded as `fmul d1, d0, d24`. The fix is to require
            // bits[11:10]=0b10, which is the architectural encoding for
            // 2-source ops.
            if (((op >> 21) & 1) == 1 && ((op >> 10) & 0x3) == 0b10) {
                // FADD=0x2, FSUB=0x3, FMUL=0x0, FDIV=0x1, FMAX=0x4, FMIN=0x5, FNMUL=0x6
                if (opcode <= 6 && ftype <= 1) {
                    emit(block, IROp::FP_BINOP, rd, rn, rm, ftype, 0, 0, opcode, cur_pc);
                    return false;
                }
            }
            // FP 1-source: uses shared fp_decode helper.
            // FABS=1, FNEG=2, FSQRT=3. FRINT* (4+) is handled by the
            // dedicated FRINT block further below.
            if (fp_decode::is_fp_1source(op)) {
                uint8_t fp1_opcode = fp_decode::fp_1source_opcode(op);
                if (fp1_opcode >= 1 && fp1_opcode <= 3 && ftype <= 1) {
                    emit(block, IROp::FP_UNOP, rd, rn, 0, ftype,
                         0, 0, fp1_opcode, cur_pc);
                    return false;
                }
            }
            // FMOV (scalar, immediate) — uses shared fp_decode helper.
            //
            // MUST be checked BEFORE FCVTZS/SCVTF — the SCVTF mask
            // 0x7F3F0000 also matches FMOV imm (since both have bit[21]=1
            // and similar high bits), causing FMOV imm to be misdecoded
            // as SCVTF (int→FP conversion). This bug caused `fmov d1, #5.0`
            // to be treated as `scvtf d1, x0` (reading garbage from x0),
            // which made every subsequent FP comparison against an
            // immediate-loaded register fail.
            if (fp_decode::is_fmov_imm(op)) {
                uint8_t imm8 = (op >> 13) & 0xFF;
                uint64_t bits = fp_decode::vfp_expand_imm(imm8, ftype);
                emit(block, IROp::FP_MOVI, rd, 0, 0, ftype, 0, 0, bits, cur_pc);
                return false;
            }
            // FCVTZS/FCVTZU: FP→int (toward zero)
            // Encoding: (op & 0x7F3E0000) == 0x1E380000, rmode=3 (toward zero)
            // Mask 0x7F3E0000 excludes bit 16 (U/S selector) so both
            // FCVTZS and FCVTZU match. The previous mask 0x7F3F0000
            // included bit 16, so FCVTZU fell through to CALL_INTERP
            // (interpreter) which has the same mask bug — resulting in
            // a silent NOP for every unsigned float→int conversion.
            // We pass sf (bit 31 of the opcode) via flags_op so the JIT
            // can choose between 32-bit and 64-bit CVTTSD2SI.
            //
            // For 32-bit dest (sf=0), the JIT's CVTTSD2SI produces a
            // 64-bit result. AArch64 32-bit register writes must zero
            // the upper 32 bits — otherwise a subsequent 64-bit read of
            // Xd would see sign-extension instead of zero-extension,
            // breaking code that reuses the register as a 64-bit value.
            // We emit a ZEXT after FP_F2I when sf=0 to enforce this.
            if ((op & 0x7F3E0000) == 0x1E380000) {
                bool is_unsigned = (op >> 16) & 1;
                uint8_t sf = (op >> 31) & 1;
                if (ftype <= 1) {
                    // For 32-bit dest (sf=0), the JIT's CVTTSD2SI produces a
                    // 64-bit result. AArch64 32-bit register writes must zero
                    // the upper 32 bits — otherwise a subsequent 64-bit read
                    // of Xd would see sign-extension instead of zero-extension,
                    // and a cbz/cbnz w0 test on the 32-bit result could see
                    // stale high bits from a previous computation. We emit a
                    // ZEXT after FP_F2I when sf=0 to enforce this.
                    uint16_t tmp = g_alloc.alloc();
                    emit(block, IROp::FP_F2I, tmp, rn, 0, ftype, 0,
                         sf, is_unsigned, cur_pc);
                    if (!sf) {
                        uint16_t z = g_alloc.alloc();
                        emit(block, IROp::ZEXT, z, tmp, 0, 32);
                        store_arm_reg(block, rd, z);
                    } else {
                        store_arm_reg(block, rd, tmp);
                    }
                    return false;
                }
            }
            // SCVTF/UCVTF: int→FP
            // Encoding: (op & 0x7F3E0000) == 0x1E220000
            // Mask 0x7F3E0000 excludes bit 16 so both SCVTF (bit 16=0)
            // and UCVTF (bit 16=1) match. The previous mask 0x7F3F0000
            // included bit 16, so UCVTF (0x1E230000) did NOT match
            // 0x1E220000 and was silently NOP'd.
            // We pass sf (bit 31) via flags_op so the JIT can choose
            // between 32-bit (CVTSI2SS eax) and 64-bit (CVTSI2SS rax)
            // source forms. Without this, `scvtf s0, w0` with w0=-1
            // would convert 0x00000000FFFFFFFF (4294967295) instead of
            // -1, producing 4.29e+09 instead of -1.0f.
            // Checked AFTER FMOV imm (which has a tighter mask and must
            // match first to avoid collision).
            if ((op & 0x7F3E0000) == 0x1E220000) {
                bool is_unsigned = (op >> 16) & 1;
                uint8_t sf = (op >> 31) & 1;
                if (ftype <= 1) {
                    emit(block, IROp::FP_I2F, rd, rn, 0, ftype, 0,
                         sf, is_unsigned, cur_pc);
                    return false;
                }
            }
            // FMADD/FMSUB (FP fused multiply-add/subtract).
            // Encoding: (op & 0xFF200000) == 0x1F000000, bit 15 = sub (1=FMSUB, 0=FMADD).
            // ra = bits[14:10]. Operands: a=Vn, b=Vm, c=Va.
            if ((op & 0xFF200000) == 0x1F000000) {
                uint8_t ra = (op >> 10) & 0x1F;
                bool sub = (op >> 15) & 1;
                if (ftype <= 1) {
                    // FMADD: dest = rn * rm + ra
                    // FMSUB: dest = ra - rn * rm
                    // Pass FP register indices directly — the JIT reads
                    // operands from V_LO_OFF + idx*8. Do NOT use
                    // load_arm_reg (that loads GPRs, not FP regs).
                    emit(block, sub ? IROp::FMSUB : IROp::FMADD,
                         rd, rn, rm, ftype ? 64 : 32, 0, 0,
                         static_cast<uint64_t>(ra), cur_pc);
                    return false;
                }
            }

            // FCVT (float ↔ double conversion).
            // Encoding: 0x1E624000 (D→S) or 0x1E22C000 (S→D).
            if ((op & 0xFFFFFC00) == 0x1E624000) {
                // FCVT Sd, Dn (double → single)
                uint16_t src = load_arm_reg(block, rn);
                uint16_t r = g_alloc.alloc();
                emit(block, IROp::FCVT_D2S, r, src, 0, 0, 0, 0, 0, cur_pc);
                store_arm_reg(block, rd, r);
                return false;
            }
            if ((op & 0xFFFFFC00) == 0x1E22C000) {
                // FCVT Dd, Sn (single → double)
                uint16_t src = load_arm_reg(block, rn);
                uint16_t r = g_alloc.alloc();
                emit(block, IROp::FCVT_S2D, r, src, 0, 0, 0, 0, 0, cur_pc);
                store_arm_reg(block, rd, r);
                return false;
            }

            // FRINT (FP round to integer).
            // The FRINT* instructions have multiple encodings. The common ones:
            // FRINTN (round to nearest even): 0x1E244000 | (ftype<<22)
            // FRINTP (round toward +inf):     0x1E24C000 | (ftype<<22)
            // FRINTM (round toward -inf):     0x1E254000 | (ftype<<22)
            // FRINTZ (round toward zero):     0x1E25C000 | (ftype<<22)
            // FRINTA (round per FPCR):        0x1E264000 | (ftype<<22)
            // FRINTX (round exact):           0x1E274000 | (ftype<<22)
            // FRINTI (round per FPCR, inexact): 0x1E27C000 | (ftype<<22)
            // All have bits[21:20] = 0b11, bits[19:15] = 0b11000 | rmode.
            // rmode: 0=N, 1=P, 2=M, 3=Z (for FRINTN/P/M/Z).
            // We detect via (op & 0x7F3F0000) == 0x1E240000 (FRINT family base)
            // with rmode in bits[19:16] (0-3 = N/P/M/Z, 4=A, 5=X, 6=I, 7=I).
            if ((op & 0xFF3F0000) == 0x1E240000 && ftype <= 1) {
                uint8_t rmode = (op >> 19) & 0x7;  // bits 21:19
                // Map ARM rmode to our FRINT imm encoding:
                //   0=N(nearest), 1=P(+inf), 2=M(-inf), 3=Z(zero), 4=A(FPCR), 5=X(exact)
                uint8_t frint_mode;
                switch (rmode) {
                    case 0: frint_mode = 0; break;  // FRINTN
                    case 1: frint_mode = 1; break;  // FRINTP
                    case 2: frint_mode = 2; break;  // FRINTM
                    case 3: frint_mode = 3; break;  // FRINTZ
                    case 4: frint_mode = 4; break;  // FRINTA (FPCR)
                    case 5: frint_mode = 5; break;  // FRINTX
                    case 6: frint_mode = 4; break;  // FRINTI → treat as FPCR
                    default: frint_mode = 0; break;
                }
                uint16_t src = load_arm_reg(block, rn);
                uint16_t r = g_alloc.alloc();
                emit(block, IROp::FRINT, r, src, 0, ftype ? 64 : 32,
                     frint_mode, 0, 0, cur_pc);
                store_arm_reg(block, rd, r);
                return false;
            }

            // FCSEL (FP conditional select).
            // Encoding: (op & 0xFF200C00) == 0x1E200C00, cond in bits[15:12].
            // FCSEL Sd/Dd, Sn, Sm, cond → if cond: dest = n else dest = m.
            // We emit a CSEL-like sequence via a GPR scratch + CSEL IR op,
            // since we don't have a native FP_CSEL IR op. But actually we
            // can do this natively by loading both FP values into GPRs and
            // using CSEL.
            if ((op & 0xFF200C00) == 0x1E200C00 && ftype <= 1) {
                uint8_t cond = (op >> 12) & 0xF;
                // Load both FP values into GPR scratch vregs.
                uint16_t val_n = g_alloc.alloc();
                emit(block, IROp::FMOV_F2G, val_n, rn, 0, 0, 0, 0, 0, cur_pc);
                uint16_t val_m = g_alloc.alloc();
                emit(block, IROp::FMOV_F2G, val_m, rm, 0, 0, 0, 0, 0, cur_pc);
                // CSEL between the two GPR values.
                uint16_t selected = g_alloc.alloc();
                emit(block, IROp::CSEL, selected, val_n, val_m, 0, cond, 0, 0, cur_pc);
                // Store the selected value back to v_lo[rd].
                emit(block, IROp::FMOV_G2F, rd, selected, 0, 0, 0, 0, 0, cur_pc);
                // FMOV_G2F zeroes v_hi[rd] per ARM semantics, so no extra
                // zero-store is needed for single-precision (ftype == 0).
                return false;
            }

            // Everything else (rare FP ops) falls back to interpreter.
            emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
            return false;
        }

        // ── SIMD LOGICAL (AND/ORR/EOR/BIC/ORN/EON) — native ────────
        case InstClass::SIMD_LOGICAL: {
            uint32_t op = d.raw;
            uint8_t opcode = (op >> 12) & 0xF;
            // ARM SIMD logical opcodes: AND=3, BIC=0, ORR=1, ORN=2,
            //                           EOR=7, EON=6, BIF=8, BIT=9, BSL=10
            // Map to our SIMD_LOGICAL imm: 0=and,1=orr,2=xor,3=bic,4=orn,5=eon
            uint8_t simd_op;
            switch (opcode) {
                case 0x3: simd_op = 0; break; // AND
                case 0x1: simd_op = 1; break; // ORR
                case 0x7: simd_op = 2; break; // EOR
                case 0x0: simd_op = 3; break; // BIC
                case 0x2: simd_op = 4; break; // ORN
                case 0x6: simd_op = 5; break; // EON
                default:
                    emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
                    return false;
            }
            emit(block, IROp::SIMD_LOGICAL, d.rd, d.rn, d.rm, 0, 0, 0, simd_op, cur_pc);
            return false;
        }

        // ── SIMD DUP — native ──────────────────────────────────────
        case InstClass::SIMD_DUP: {
            // dup Vd.2d, Rn → broadcast Rn to both halves
            uint16_t val = load_arm_reg(block, d.rn);
            emit(block, IROp::SIMD_DUP, d.rd, val, 0, 0, 0, 0, 0, cur_pc);
            return false;
        }

        // ── SIMD LD1/ST1 — native (128-bit load/store) ─────────────
        // Handles multi-register forms: LD1/ST1 {Vt..Vt+n-1} stores n×16
        // bytes (n = d.simd_count, 1..4). Each register's v_lo and v_hi
        // are loaded/stored as two 8-byte memory accesses.
        case InstClass::SIMD_LD1: {
            uint16_t base = load_arm_reg(block, d.rn, true);
            for (uint8_t i = 0; i < d.simd_count; i++) {
                uint8_t reg = (d.rt + i) & 0x1F;
                uint16_t lo = g_alloc.alloc();
                emit(block, IROp::LOAD_MEM, lo, base, 0, 8, 0, 0,
                     static_cast<uint64_t>(i * 16));
                uint16_t hi = g_alloc.alloc();
                emit(block, IROp::LOAD_MEM, hi, base, 0, 8, 0, 0,
                     static_cast<uint64_t>(i * 16 + 8));
                emit(block, IROp::SIMD_LDST, reg, lo, hi, 1, 0, 0, 0, cur_pc);
            }
            return false;
        }

        case InstClass::SIMD_ST1: {
            uint16_t base = load_arm_reg(block, d.rn, true);
            for (uint8_t i = 0; i < d.simd_count; i++) {
                uint8_t reg = (d.rt + i) & 0x1F;
                uint16_t lo = g_alloc.alloc();
                uint16_t hi = g_alloc.alloc();
                emit(block, IROp::SIMD_LDST, reg, lo, hi, 0, 0, 0, 0, cur_pc);
                emit(block, IROp::STORE_MEM, 0, base, lo, 8, 0, 0,
                     static_cast<uint64_t>(i * 16));
                emit(block, IROp::STORE_MEM, 0, base, hi, 8, 0, 0,
                     static_cast<uint64_t>(i * 16 + 8));
            }
            return false;
        }

        // ── SMADDL / UMADDL (widening multiply-accumulate) ─────────
        case InstClass::SMADDL: case InstClass::UMADDL: {
            uint16_t a = load_arm_reg(block, d.rn);
            uint16_t b = load_arm_reg(block, d.rm);
            uint16_t r = g_alloc.alloc();
            // Encode the accumulator register index (d.ra) in the `cond`
            // field. The JIT loads cpu.regs[d.ra] directly. This avoids
            // creating a vreg for the accumulator that could be DCE'd by
            // the optimizer (which would leave the SMADDL with a stale
            // vreg reference). If d.ra == 31 (XZR), cond=31 and the JIT
            // uses 0 as the accumulator.
            emit(block, d.cls == InstClass::SMADDL ? IROp::SMADDL : IROp::UMADDL,
                 r, a, b, 0, d.ra & 0x1F, 0, 0, cur_pc);
            store_arm_reg(block, d.rd, r);
            return false;
        }

        // ── SMSUBL / UMSUBL / SMULH / UMULH ─────────────────────────
        case InstClass::SMSUBL:
        case InstClass::UMSUBL: {
            uint16_t a = load_arm_reg(block, d.rn);
            uint16_t b = load_arm_reg(block, d.rm);
            uint16_t r = g_alloc.alloc();
            // Accumulator register index in cond field (31=XZR → 0).
            emit(block, d.cls == InstClass::SMSUBL ? IROp::SMSUBL : IROp::UMSUBL,
                 r, a, b, 0, d.ra & 0x1F, 0, 0, cur_pc);
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

        // ── SIMD data-processing (integer add/sub/mul/min/max/cmp) ───
        // Common encodings that we can JIT natively via SIMD_ARITH /
        // SIMD_CMP:
        //   ADD (vector): 0x0E208400  (size 0/1/2/3 = 8/16/32/64-bit)
        //   SUB (vector): 0x2E208400
        //   MUL (vector): 0x0E209C00  (size 0/1/2 = 8/16/32-bit; 64-bit
        //                               not in SSE2)
        //   CMGT (signed >):   0x0E203400  (U=0, opcode=0x34)
        //   CMGE (signed >=):  0x0E203C00  (U=0, opcode=0x3C)
        //   CMEQ (==):         0x2E208C00  (U=1, opcode=0x8C)
        //   CMHI (unsigned >): 0x2E203400  (U=1, opcode=0x34)
        //   CMHS (unsigned >=):0x2E203C00  (U=1, opcode=0x3C)
        // These are the most common SIMD arithmetic and compare ops.
        // Other SIMD_DP encodings fall through to CALL_INTERP.
        case InstClass::SIMD_DP: {
            uint32_t op = d.raw;
            bool Q = (op >> 30) & 1;
            uint8_t size = (op >> 22) & 3;
            uint32_t sub3 = op & 0xFF20FC00;
            uint32_t sub3_noq = sub3 & ~(1u << 30);

            int esize = 1 << size;   // 1, 2, 4, 8

            // ── Arithmetic ops (SIMD_ARITH) ──
            uint8_t arith_op = 0xFF;
            if (sub3_noq == 0x0E208400) {
                arith_op = 0;  // ADD
            } else if (sub3_noq == 0x2E208400) {
                arith_op = 1;  // SUB
            } else if (sub3_noq == 0x0E209C00) {
                arith_op = 2;  // MUL
                if (size == 3) {
                    emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
                    return false;
                }
            }

            if (arith_op != 0xFF) {
                (void)Q;
                emit(block, IROp::SIMD_ARITH, d.rd, d.rn, d.rm, 0,
                     static_cast<uint64_t>(esize), 0, arith_op, cur_pc);
                return false;
            }

            // ── Compare ops (SIMD_CMP) ──
            // SIMD_CMP imm: 0=eq, 1=ge_u, 2=gt_u, 3=ge_s, 4=gt_s,
            //               5=hi_u, 6=hs_u
            uint8_t cmp_op = 0xFF;
            if (sub3_noq == 0x2E208C00) {
                // CMEQ (==): U=1, opcode=0x8C
                cmp_op = 0;  // eq
            }

            if (cmp_op != 0xFF) {
                (void)Q;
                emit(block, IROp::SIMD_CMP, d.rd, d.rn, d.rm, 0,
                     static_cast<uint64_t>(esize), 0, cmp_op, cur_pc);
                return false;
            }

            // ── NOT/MVN (vector) — 0x2E205800 ──
            // NOT Vd.<T>, Vn.<T> = bitwise NOT of all lanes.
            // Encoding: 1 Q 0 1 1 1 1 0 size 1 0000 0 1 0 1 1 0 Rn Rd
            // sub3_noq = 0x2E205800
            if (sub3_noq == 0x2E205800) {
                // Use SIMD_LOGICAL with opc=2 (XOR) and src2=src1
                // to compute NOT: a ^ a = 0, then we need ~a.
                // Actually NOT = a XOR all-ones. We emit CALL_INTERP
                // for now since SIMD_LOGICAL doesn't have a NOT mode.
                // But we can use BIC with src2=src1: a & ~a = 0 (wrong).
                // Let's emit a logical NOT via XOR with all-ones.
                // We don't have an all-ones register, so fall to interp.
                emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
                return false;
            }

            // ── NEG (vector) — 0x2E20B800 ──
            // NEG Vd.<T>, Vn.<T> = 0 - Vn (two's complement negate).
            // This is SUB with src1=0. We can emit SIMD_ARITH sub with
            // a zero src1, but our SIMD_ARITH reads from vregs, not XZR.
            // Fall to interp for now.
            if (sub3_noq == 0x2E20B800) {
                emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
                return false;
            }

            // ── SHL (vector, immediate) — 0x0F00A400 ──
            // SHL Vd.<T>, Vn.<T>, #shift
            // Shifts each lane left by immediate. Very common in SIMD code.
            // We don't have a native IR op for vector shift, so fall to interp.
            if ((op & 0xBF00FC00) == 0x0F00A400) {
                emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
                return false;
            }

            // ── USHR/SSHR (vector, immediate) — 0x2F000400/0x0F000400 ──
            // Very common in SIMD memset/memcpy. Fall to interp.
            if ((op & 0xBF00FC00) == 0x2F000400 ||  // USHR
                (op & 0xBF00FC00) == 0x0F000400) {  // SSHR
                emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
                return false;
            }

            // Unrecognized SIMD_DP — fall back to interpreter.
            emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
            return false;
        }

        // ── Everything else: inline interpreter call (no block split) ──
        default:
            emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
            return false;
    }
}

} // namespace arm64emu
