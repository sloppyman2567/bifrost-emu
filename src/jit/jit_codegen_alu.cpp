// jit/jit_codegen_alu.cpp — FrostJIT ALU/arithmetic IR-op codegen.
//
// v1.4.5-alpha (Turn 37): split out of frostjit.cpp. This file holds the
// arithmetic / logic / bitfield case bodies of the IR-op switch,
// extracted into a separate method (compile_ir_alu) for readability.
// The main switch in frostjit.cpp dispatches to this method before its
// residual cases.
//
// No behavior change — pure file split. The method is a member of
// FrostJIT (declared in include/jit/frostjit.hpp) so it has full access
// to the JIT's emit_*, alloc_*, flush_*, etc. helpers.
//
// Return value (int — see frostjit.hpp):
//   -1 = op not handled here (caller falls through to next dispatcher)
//    0 = op handled, does NOT end the block
//    1 = op handled AND ends the block
// All ALU ops in this file return 0 (handled, does not end block).
//
// Cases handled:
//   IMM, MOV, ADD, SUB, AND, OR, XOR, MUL, SHL, SHR, SAR, ROR,
//   NOT, NEG, SEXT, ZEXT, CLZ, REV64, CSEL,
//   SBFM, UBFM, BFM, EXTR, RBIT, CLS, REV16, REV32,
//   CCMP, UDIV, SDIV,
//   SMADDL, UMADDL, SMSUBL, UMSUBL
#include "jit/frostjit.hpp"
#include "core/emulator.h"
#include "ir/ir.hpp"

#include <cstddef>
#include <cstdint>

namespace arm64emu {

// ── FrostJIT::compile_ir_alu ───────────────────────────────────────────
int FrostJIT::compile_ir_alu(const IRInst& inst) {
    switch (inst.op) {
        case IROp::IMM:
            if (inst.dest) {
                int d = alloc_reg_for(inst.dest, -1);
                if (inst.imm <= 0xFFFFFFFFULL) {
                    emit_mov_imm32_zext(d, static_cast<uint32_t>(inst.imm));
                } else {
                    emit_mov_imm64(d, inst.imm);
                }
            }
            return 0;

        case IROp::MOV:
            if (inst.dest) {
                int s = ensure_vreg(inst.src1);
                int d = alloc_reg(s);
                if (d != s) {
                    emit_mov_reg(d, s);
                }
                set_vreg_reg(inst.dest, d);
            }
            return 0;

        // ── Binary ALU ops ──
        // Use src1 and src2 in whatever host regs they're already cached in.
        // Only allocate a fresh reg for dest when dest != src1 && dest != src2.
        // This avoids the old "force everything into RAX/RCX" pattern that
        // caused massive stack spilling — values now stay in their host regs
        // across ALU ops, and the register allocator's caching actually pays
        // off.
        //
        // Commutative ops (ADD/AND/OR/XOR/MUL) can swap operands, so
        // dest == src2 is handled by computing in src2's reg. Non-commutative
        // ops (SUB) need a fresh reg when dest == src2 (because we'd lose
        // src2 before the subtraction).
        case IROp::ADD: case IROp::SUB: case IROp::AND:
        case IROp::OR:  case IROp::XOR: case IROp::MUL: {
            clobber_flags();
            bool commutative = (inst.op != IROp::SUB);
            int s1 = ensure_vreg(inst.src1);
            int s2 = ensure_vreg(inst.src2);
            int d;

            // emit_alu_op: emit `d = d op src` for the current inst.op.
            auto emit_alu_op = [&](int d, int src) {
                switch (inst.op) {
                    case IROp::ADD: emit_add_reg(d, src); break;
                    case IROp::SUB: emit_sub_reg(d, src); break;
                    case IROp::AND: emit_and_reg(d, src); break;
                    case IROp::OR:  emit_or_reg(d, src);  break;
                    case IROp::XOR: emit_xor_reg(d, src); break;
                    case IROp::MUL: emit_imul_reg(d, src); break;
                    default: break;
                }
            };

            if (inst.dest == inst.src1) {
                // dest == src1: compute in s1 (in-place modify).
                d = s1;
                emit_alu_op(d, s2);
                vreg_dirty_[inst.dest] = true;
                dirty_host_regs_ |= (1u << d);
            } else if (inst.dest == inst.src2 && commutative) {
                // dest == src2, commutative: compute in s2 (swap operands).
                d = s2;
                emit_alu_op(d, s1);
                vreg_dirty_[inst.dest] = true;
                dirty_host_regs_ |= (1u << d);
            } else {
                // dest != src1 (and not the commutative src2 case):
                // allocate a fresh reg for dest that doesn't collide with
                // s1 or s2, then mov src1 and op src2.
                d = alloc_reg_excluding(s1, s2);
                if (d != s1) emit_mov_reg(d, s1);
                emit_alu_op(d, s2);
                set_vreg_reg(inst.dest, d);
            }
            return 0;
        }

        case IROp::SHL: case IROp::SHR:
        case IROp::SAR: case IROp::ROR: {
            // x86 variable shifts use CL for the count. We force src2 into
            // RCX (clobbering its previous occupant), but leave src1 in
            // whatever reg it's cached in. dest is computed in src1's reg
            // when dest == src1, else in a fresh reg excluding src1 and RCX.
            clobber_flags();
            int s1 = ensure_vreg(inst.src1);
            // If s1 is in RCX, move it elsewhere first so forcing src2 into
            // RCX doesn't lose src1.
            if (s1 == RCX) {
                int tmp = alloc_reg_excluding(RCX, -1);
                emit_mov_reg(tmp, RCX);
                reg_vreg_[RCX] = -1;
                vreg_home_[inst.src1] = tmp;
                reg_vreg_[tmp] = inst.src1;
                if (vreg_dirty_[inst.src1]) {
                    dirty_host_regs_ &= ~(1u << RCX);
                    dirty_host_regs_ |= (1u << tmp);
                }
                s1 = tmp;
            }
            // Force src2 into RCX (evict current occupant if any).
            force_vreg_to_reg(inst.src2, RCX);

            // emit_shift: mask CL to 6 bits (x86 shift counts are mod 64)
            // and emit `d = d shift_cl` for the current inst.op.
            auto emit_shift = [&](int d) {
                // For 32-bit ROR: mask CL to 5 bits and use 32-bit ROR
                // (no REX.W) so rotation stays within the lower 32 bits.
                // 64-bit ROR on a zero-extended 32-bit value loses wrap bits.
                bool is_32bit_ror = (inst.op == IROp::ROR && inst.width == 32);
                if (is_32bit_ror) {
                    emit_and_cl_imm8(0x1F);  // mask to 5 bits for 32-bit
                    // Emit 32-bit ROR: no REX.W prefix.
                    emit_byte(rex(false, false, false, d >= 8));
                    emit_byte(0xD3);
                    emit_byte(modrm(3, 1, d & 7));
                } else {
                    emit_and_cl_imm8(0x3F);
                    int kind = (inst.op == IROp::SHL) ? 4
                             : (inst.op == IROp::SHR) ? 5
                             : (inst.op == IROp::SAR) ? 7 : 1;
                    emit_shift_cl(d, kind);
                }
            };

            int d;
            if (inst.dest == inst.src1) {
                d = s1;
                emit_shift(d);
                vreg_dirty_[inst.dest] = true;
                dirty_host_regs_ |= (1u << d);
            } else {
                d = alloc_reg_excluding(s1, RCX);
                if (d != s1) emit_mov_reg(d, s1);
                emit_shift(d);
                set_vreg_reg(inst.dest, d);
            }
            return 0;
        }

        case IROp::NOT: {
            clobber_flags();
            int s = ensure_vreg(inst.src1);
            int d = alloc_reg(s);
            if (d != s) emit_mov_reg(d, s);
            emit_not_reg(d);
            set_vreg_reg(inst.dest, d);
            return 0;
        }

        case IROp::NEG: {
            clobber_flags();
            int s = ensure_vreg(inst.src1);
            int d = alloc_reg(s);
            if (d != s) emit_mov_reg(d, s);
            emit_neg_reg(d);
            set_vreg_reg(inst.dest, d);
            return 0;
        }

        case IROp::SEXT: {
            clobber_flags();  // shifts clobber RFLAGS
            int s = ensure_vreg(inst.src1);
            int d = alloc_reg(s);
            if (d != s) emit_mov_reg(d, s);
            int bits = inst.width;
            if (bits < 64) {
                int sh = 64 - bits;
                emit_shift_imm8(d, 4, sh);
                emit_shift_imm8(d, 7, sh);
            }
            set_vreg_reg(inst.dest, d);
            return 0;
        }

        case IROp::ZEXT: {
            int bits = inst.width;
            int s = ensure_vreg(inst.src1);
            int d = alloc_reg(s);
            if (d != s) emit_mov_reg(d, s);
            if (bits < 64) {
                if (bits == 32) {
                    // mov e_d, e_d (zero-extends to 64 bits).
                    // MUST emit REX prefix if d >= R8 (R8-R15 need REX.B
                    // for both reg and rm fields, since they share the
                    // same register).
                    // NOTE: mov r32, r32 does NOT clobber RFLAGS.
                    if (d >= 8) {
                        emit_byte(0x45);
                    }
                    emit_byte(0x89); emit_byte(modrm(3, d&7, d&7));
                } else if (bits == 16) {
                    clobber_flags();  // AND clobbers RFLAGS
                    emit_byte(rex(true,false,false,d>=8));
                    emit_byte(0x81); emit_byte(modrm(3,4,d&7)); emit_u32(0x0000FFFF);
                } else if (bits == 8) {
                    clobber_flags();  // AND clobbers RFLAGS
                    emit_byte(rex(true,false,false,d>=8));
                    emit_byte(0x81); emit_byte(modrm(3,4,d&7)); emit_u32(0x000000FF);
                } else {
                    clobber_flags();  // AND clobbers RFLAGS
                    // Use RCX as scratch for the mask.
                    int tmp = alloc_reg(d);
                    emit_mov_imm64(tmp, (1ULL << bits) - 1);
                    emit_and_reg(d, tmp);
                }
            }
            set_vreg_reg(inst.dest, d);
            return 0;
        }

        case IROp::CLZ: {
            // lzcnt rax, rax overwrites RAX, destroying
            // src1's cached value. If src1 is a scratch vreg holding a snapshot
            // of an arch reg (from LOAD_REG), later readers would reload from
            // an uninitialized stack slot. Use the same fix as REV64: allocate
            // a separate dest reg and copy src1 there BEFORE lzcnt.
            //
            // The previous code did `vreg_home_[inst.src1] = -1; vreg_dirty_[inst.src1] = false`
            // which silently dropped a dirty src1 — same bug class as REV64.
            clobber_flags();  // lzcnt doesn't clobber flags, but sub does (32-bit path)
            force_vreg_to_reg(inst.src1, RAX);
            int d = alloc_reg_for(inst.dest, RAX);
            if (d != RAX) {
                emit_mov_reg(d, RAX);  // copy src1 to d, preserving src1 in RAX
            }
            emit_lzcnt_reg(d, d);  // lzcnt d, d (in-place on d)
            // For 32-bit CLZ: x86 LZCNT counts 64-bit leading zeros.
            // ARM 32-bit CLZ should only count the lower 32 bits.
            // Subtract 32 to account for the upper 32 zero bits.
            if (inst.width == 32) {
                // sub d, 32 (use the right encoding for d >= R8)
                if (d >= 8) emit_byte(0x49); else emit_byte(0x48);
                emit_byte(0x83); emit_byte(0xE8 | (d & 7)); emit_byte(0x20);
                // mov e_d, e_d (zero-extend to 64 bits)
                if (d >= 8) emit_byte(0x45);
                emit_byte(0x89); emit_byte(modrm(3, d&7, d&7));
            }
            // dest is already cached in d (via alloc_reg_for) and marked dirty.
            return 0;
        }

        case IROp::REV64: {
            // Force src1 into RAX (properly evicts old RAX occupant).
            force_vreg_to_reg(inst.src1, RAX);
            // bswap modifies RAX in place, which
            // destroys v(src1)'s value. If dest != src1, we must preserve
            // src1's value for potential later readers. Allocate a separate
            // dest reg and copy src1 there BEFORE bswap, so src1 stays
            // cached in RAX (or gets reloaded from cpu.regs[]/stack later).
            //
            // The previous code did `bswap eax; store_vreg(dest, RAX)` which
            // silently dropped src1's value if src1 was a scratch vreg
            // (v > 31) — store_vreg cleared src1's dirty flag without
            // spilling, and a later force_vreg_to_reg(src1) loaded from an
            // uninitialized stack slot. This caused jit_simd.elf's
            // `cmp w0, w5` to compute wrong flags and crash.
            int d = alloc_reg_for(inst.dest, RAX);
            if (d != RAX) {
                // Copy src1 to d, then bswap d (preserving src1 in RAX).
                emit_mov_reg(d, RAX);
            }
            // bswap d (in-place if d == RAX, or the copy if d != RAX).
            if (inst.width == 32) {
                // 32-bit bswap: 0F C8+r (no REX.W). REX.B if d >= 8.
                if (d >= 8) emit_byte(0x41);
                emit_byte(0x0F); emit_byte(0xC8 + (d & 7));
                // Zero-extend 32-bit result to 64 bits.
                emit_byte(rex(false, d>=8, false, d>=8));
                emit_byte(0x89); emit_byte(modrm(3, d&7, d&7));
            } else {
                // 64-bit bswap: REX.W 0F C8+r.
                emit_bswap_reg(d);
            }
            // dest is already cached in d (via alloc_reg_for) and marked dirty.
            return 0;
        }

        case IROp::CSEL: {
            // Native CSEL/CSINC/CSINV/CSNEG via jcc+mov.
            //
            // Semantics:
            //   CSEL  Rd = cond ? Rn : Rm
            //   CSINC Rd = cond ? Rn : (Rm + 1)
            //   CSINV Rd = cond ? Rn : ~Rm
            //   CSNEG Rd = cond ? Rn : -Rm
            //
            // Strategy:
            //   1. Ensure flags in host RFLAGS.
            //   2. Flush all vregs (save/restore flags around flush).
            //   3. Load src1 → RAX, src2 → RCX (with XZR special case).
            //   4. For CSINC/CSINV/CSNEG: transform RCX (pushfq/popfq
            //      to preserve flags). For 32-bit ops, zero-extend RCX
            //      after the transform.
            //   5. RDX = RAX (d = src1).
            //   6. jcc skip (if cond TRUE, keep src1); else mov rdx, rcx.
            //   7. For 32-bit ops, zero-extend RDX.
            //   8. Store RDX to dest.
            //
            // Carry polarity: arm_cond_to_x86() assumes SUB convention
            // (ARM C = NOT x86 CF). When flags came from ADD/TST
            // (carry_is_direct), CS/CC need swapped mapping, HI/LS need cmc.

            // Track whether we loaded flags from pstate. If we did, the
            // flags in pstate are already correct and the epilogue should
            // NOT re-materialize (the loaded x86 flags have inverted CF,
            // and materialize(false) would corrupt the C flag).
            // If we didn't load (flags were already in host), the epilogue
            // must still materialize them.
            bool loaded_from_pstate = !flags_in_host_;

            // Ensure flags in host.
            if (!flags_in_host_) {
                flush_all_vregs();
                emit_load_flags_from_pstate();
                // emit_load_flags_from_pstate sets x86 CF = ARM C XOR from_sub.
                // Normalize to SUB convention (x86 CF = NOT ARM C) so the
                // default arm_cond_to_x86() mapping works correctly for ALL
                // conditions (CS/CC/HI/LS included) regardless of whether
                // the flags originally came from ADD or SUB.
                emit_normalize_cf_to_sub_convention();
                // Drop all cache mappings but DON'T clear flags_in_host_.
                bool saved_fih2 = flags_in_host_;
                invalidate_all_vregs();
                flags_in_host_ = saved_fih2;
                flags_in_host_ = true;
                flags_from_sub_ = true;  // CF is now in SUB convention
            }
            // Resolve condition code. With flags_from_sub_=true (SUB convention,
            // whether originally from SUB or normalized after loading),
            // resolve_arm_cond_with_carry uses the default mapping.
            bool need_cmc = false;
            uint8_t cc = resolve_arm_cond_with_carry(inst.cond, need_cmc);
            // Save flags, flush vregs, restore flags. CRITICAL: preserve
            // flags_in_host_ — invalidate_all_vregs would clear it, but
            // pushfq/popfq preserves the actual flags.
            bool saved_fih = flags_in_host_;
            bool saved_ffs = flags_from_sub_;
            emit_pushfq();
            flush_all_vregs();
            invalidate_all_vregs();
            emit_popfq();
            flags_in_host_ = saved_fih;
            flags_from_sub_ = saved_ffs;
            if (need_cmc) emit_byte(0xF5);  // cmc

            // Load src1 → RAX, src2 → RCX.
            if (inst.src1 == 32) emit_mov_imm32_zext(RAX, 0);
            else load_vreg_to_reg(RAX, inst.src1);
            if (inst.src2 == 32) emit_mov_imm32_zext(RCX, 0);
            else load_vreg_to_reg(RCX, inst.src2);

            // For CSINC/CSINV/CSNEG, transform RCX (the "else" value).
            if (inst.op != IROp::CSEL) {
                emit_pushfq();
                if (inst.op == IROp::CSINC) {
                    // add rcx, 1
                    emit_byte(0x48); emit_byte(0x83); emit_byte(0xC1); emit_byte(0x01);
                } else if (inst.op == IROp::CSINV) {
                    emit_not_reg(RCX);
                } else {  // CSNEG
                    emit_neg_reg(RCX);
                }
                emit_popfq();
            }

            // RDX = RAX (d = src1).
            emit_mov_reg(RDX, RAX);
            // jcc skip (if cond TRUE, keep src1 in RDX).
            // Use placeholder+patch instead of hardcoded offset.
            size_t jcc_off = emit_jcc_rel8_placeholder(cc);
            // mov rdx, rcx (cond FALSE: rdx = src2)
            emit_byte(0x48); emit_byte(0x89); emit_byte(0xCA);
            // Patch jcc to skip over the 3-byte mov.
            patch_jcc_rel8(jcc_off, 3);

            // Store RDX to dest, then cache it in RDX.
            store_reg_to_vreg(inst.dest, RDX);
            set_vreg_reg(inst.dest, RDX);
            // If we loaded flags from pstate, clear flags_in_host_ so the
            // epilogue doesn't re-materialize. The flags in pstate are
            // already correct (CSEL doesn't modify flags). Re-materializing
            // with the loaded x86 flags would corrupt the C flag: the load
            // inverted CF based on the from_sub bit, and materialize(false)
            // would set ARM C = x86 CF (the inverted value), losing the C.
            // If we loaded flags from pstate, clear flags_in_host_ so the
            // epilogue doesn't re-materialize. The flags in pstate are
            // already correct (CSEL doesn't modify flags). Re-materializing
            // with the loaded x86 flags would corrupt the C flag: the load
            // inverted CF based on the from_sub bit, and materialize(false)
            // would set ARM C = x86 CF (the inverted value), losing the C.
            // If flags were already in host (not loaded), keep flags_in_host_
            // so the epilogue materializes them normally.
            if (loaded_from_pstate) {
                flags_in_host_ = false;
            }
            return 0;
        }

        // These are very common (SXTB/SXTH/SXTW/UXTB/UXTH/UXTW/LSL/LSR/
        // ASR/SBFIZ/UBFIZ/BFI/BFXIL) and falling back to CALL_INTERP
        // for each one is both slow and a source of correctness bugs
        // (the interp call's PC-change check can cause spurious block
        // exits). Implement them directly.
        //
        // ARM64 bitfield semantics (width W = 32 or 64):
        //   SBFM Rd, Rn, #immr, #imms:
        //     R = ROR(Rn, immr)  (rotate right by immr)
        //     if imms < immr:  Rd = sign_extend(R[W-1:imms], imms+1 bits)
        //     else:            Rd = sign_extend(R[imms:0], imms-immr+1 bits)
        //   UBFM Rd, Rn, #immr, #imms:
        //     R = ROR(Rn, immr)
        //     if imms < immr:  Rd = zero_extend(R[imms:0], imms+1 bits)
        //     else:            Rd = R[imms:0] zero-extended (i.e. extract)
        //   BFM Rd, Rn, #immr, #imms:
        //     inserts Rn's bits into Rd (preserve outside bits)
        //   EXTR Rd, Rn, Rm, #imms:
        //     Rd = (Rn:Rm) >> imms
        case IROp::SBFM: case IROp::UBFM: {
            int width = inst.sf ? 64 : 32;
            int immr = inst.immr;
            int imms = inst.imms;
            // Load src into RAX.
            // flush+invalidate FIRST so the
            // cache is empty and the subsequent memory access can't
            // interact with stale mappings. We then write the result
            // directly to the dest vreg's memory home and re-cache it.
            clobber_flags();  // shifts/ands clobber RFLAGS
            flush_all_vregs();
            invalidate_all_vregs();
            load_vreg_to_reg(RAX, inst.src1);

            // Handle common aliases efficiently:
            // - LSL (imms < immr): shift left by (width - immr)
            // - LSR (imms == width-1, UBFM): shift right by immr
            // - ASR (imms == width-1, SBFM): arithmetic shift right by immr

            // LSL: imms < immr (e.g. lsl w0, w0, #2 = UBFM w0, w0, #30, #31)
            // UBFM semantics for imms < immr:
            //   field = src & ((1 << (imms+1)) - 1)   [take low imms+1 bits]
            //   result = field << (width - immr)       [shift left to position]
            // the previous code did shl THEN and, which
            // zeroed the result for shift >= 32. For example, lsl x0, x0, #32
            // (immr=32, imms=31): shl rax,32 → 0x100000000, then and rax,
            // 0xFFFFFFFF → 0. The correct order is: mask FIRST, then shift.
            if (imms < immr) {
                int sh = width - immr;
                if (sh > 0 && sh < width) {
                    // Mask to imms+1 bits FIRST.
                    uint64_t mask = (1ULL << (imms + 1)) - 1;
                    emit_mov_imm64(RDX, mask);
                    emit_and_reg(RAX, RDX);
                    // THEN shift left by sh.
                    if (width == 32) {
                        emit_byte(0xC1); emit_byte(modrm(3, 4, RAX & 7)); emit_byte(static_cast<uint8_t>(sh));
                    } else {
                        emit_shift_imm8(RAX, 4, sh);
                    }
                    if (width == 32) {
                        if (RAX >= 8) emit_byte(0x45);
                        emit_byte(0x89); emit_byte(modrm(3, RAX&7, RAX&7));
                    }
                    // Write result directly to dest's memory home, then cache.
                    store_reg_to_vreg(inst.dest, RAX);
                    set_vreg_reg(inst.dest, RAX);
                    return 0;
                }
            }

            // LSR (UBFM) or ASR (SBFM): imms == width-1
            if (imms == width - 1) {
                if (immr > 0) {
                    if (width == 32) {
                        if (immr <= 31) {
                            if (inst.op == IROp::SBFM) {
                                // SAR (arithmetic)
                                emit_byte(0xC1); emit_byte(modrm(3, 7, RAX & 7)); emit_byte(static_cast<uint8_t>(immr));
                            } else {
                                // SHR (logical)
                                emit_byte(0xC1); emit_byte(modrm(3, 5, RAX & 7)); emit_byte(static_cast<uint8_t>(immr));
                            }
                        }
                    } else {
                        if (inst.op == IROp::SBFM) {
                            emit_shift_imm8(RAX, 7, immr);
                        } else {
                            emit_shift_imm8(RAX, 5, immr);
                        }
                    }
                }
                if (width == 32) {
                    if (RAX >= 8) emit_byte(0x45);
                    emit_byte(0x89); emit_byte(modrm(3, RAX&7, RAX&7));
                }
                store_reg_to_vreg(inst.dest, RAX);
                set_vreg_reg(inst.dest, RAX);
                return 0;
            }

            // General case: ROR then extract then (for SBFM) sign-extend
            if (immr != 0) {
                emit_mov_imm32_zext(RCX, immr);
                if (width == 64) {
                    emit_byte(0x48); emit_byte(0x83); emit_byte(0xE1); emit_byte(0x3F);
                    emit_byte(rex(true,false,false,false));
                    emit_byte(0xD3); emit_byte(modrm(3,1,RAX&7));
                } else {
                    emit_byte(0x48); emit_byte(0x83); emit_byte(0xE1); emit_byte(0x1F);
                    emit_byte(0xD3); emit_byte(modrm(3, 1, RAX & 7));
                    emit_byte(rex(true,false,false,false));
                    emit_byte(0x81); emit_byte(modrm(3,4,RAX&7)); emit_u32(0xFFFFFFFF);
                }
            }
            // Extract bits [imms-immr:0] from RAX (after rotate).
            // after ROR by immr, the field that was at
            // [imms:immr] in the original is now at [imms-immr:0]. So the
            // mask must be (imms-immr+1) bits wide, NOT (imms+1) bits.
            // The old code used (1<<(imms+1))-1 which extracted too many
            // bits, pulling in garbage from above the field. This broke
            // musl's get_stride (ubfx x0, x0, #6, #6) which extracts a
            // 6-bit field — the JIT returned 0x57 instead of 0x17,
            // corrupting the malloc size class lookup.
            if (imms < width - 1) {
                int field_width = imms - immr + 1;
                uint64_t mask = (field_width >= 64) ? ~0ULL : ((1ULL << field_width) - 1);
                emit_mov_imm64(RDX, mask);
                emit_and_reg(RAX, RDX);
            }
            // For SBFM: sign-extend from the field's sign bit.
            // After ROR+mask, the field occupies bits [field_width-1:0]
            // where field_width = imms - immr + 1. The sign bit is at
            // bit (imms - immr). Sign-extend by shifting left then right.
            if (inst.op == IROp::SBFM && imms < width - 1) {
                int field_width = imms - immr + 1;
                int sh = width - field_width;
                if (sh > 0) {
                    emit_shift_imm8(RAX, 4, sh);
                    emit_shift_imm8(RAX, 7, sh);
                }
            }
            // For 32-bit ops: zero-extend result to 64 bits.
            if (width == 32) {
                emit_byte(0x89); emit_byte(modrm(3, RAX&7, RAX&7));
            }
            store_reg_to_vreg(inst.dest, RAX);
            set_vreg_reg(inst.dest, RAX);
            return 0;
        }

        // Defensive fallback: BFM is normally decomposed to SHL+SHR+
        // OR+AND+OR in ir_translate.cpp. Falls back to interpreter.
        case IROp::BFM: {
            emit_call_interp(inst.arm_pc, false);
            return 0;
        }

        // Defensive fallback: EXTR is normally decomposed to SHL+SHR+OR
        // in ir_translate.cpp. Falls back to interpreter.
        case IROp::EXTR: {
            emit_call_interp(inst.arm_pc, false);
            return 0;
        }

        // Defensive fallback: RBIT/REV16/REV32 are decomposed to SWAR
        // shift/mask patterns in ir_translate.cpp, and CLS to SAR+XOR+
        // CLZ+SUB. Falls back to interpreter if re-emitted.
        case IROp::RBIT: case IROp::CLS: case IROp::REV16: case IROp::REV32:
            emit_call_interp(inst.arm_pc, false);
            kill_vreg(inst.dest);
            {
                int d = alloc_reg();
                int rd = static_cast<int>(inst.imm);
                emit_load_arm(d, rd);
                set_vreg_reg(inst.dest, d);
            }
            return 0;

        case IROp::CCMP: {
            // CCMP/CCMN: if cond then set flags from (rn - rm) [CCMP]
            //            or (rn + rm) [CCMN]; else set flags to imm nzcv.
            // inst.width = nzcv field (4 bits), inst.cond = ARM cond,
            // inst.flags_op = 1 for CCMP (sub), 0 for CCMN (add).
            bool is_sub = (inst.flags_op == 1);
            uint8_t nzcv = inst.width & 0xF;

            // Compute x86 cc (true when ARM cond is TRUE).
            bool need_cmc = false;
            uint8_t cc = resolve_arm_cond_with_carry(inst.cond, need_cmc);

            // Ensure flags in host.
            if (!flags_in_host_) {
                flush_all_vregs();
                emit_load_flags_from_pstate();
                // emit_load_flags_from_pstate sets x86 CF = ARM C XOR from_sub.
                // Normalize to SUB convention (x86 CF = NOT ARM C) so the
                // default arm_cond_to_x86() mapping works correctly for ALL
                // conditions (CS/CC/HI/LS included) regardless of whether
                // the flags originally came from ADD or SUB. Without this
                // normalization, CCMP after ADDS would use the wrong Jcc
                // for the CC/CS condition (taking the wrong branch), causing
                // pstate divergences like jit=0x8000000 ref=0x88000000
                // (JIT skipped the compare; interpreter did it).
                emit_normalize_cf_to_sub_convention();
                // Drop all cache mappings WITHOUT clearing flags_in_host_.
                // use invalidate_all_vregs but preserve flags_in_host_.
                {
                    bool saved_fih3 = flags_in_host_;
                    invalidate_all_vregs();
                    flags_in_host_ = saved_fih3;
                }
                flags_in_host_ = true;
                flags_from_sub_ = true;  // CF is now in SUB convention
            }
            if (need_cmc) emit_byte(0xF5);

            // Load src1 (rn) → RAX, src2 (rm) → RCX.
            // Use force_two_vregs_to for proper aliasing/eviction handling.
            // The old ensure_vreg + mov pattern could lose src1's value when
            // the second ensure_vreg evicted RAX under register pressure.
            if (inst.src1 == 32 && inst.src2 == 32) {
                emit_mov_imm32_zext(RAX, 0);
                emit_mov_imm32_zext(RCX, 0);
            } else if (inst.src1 == 32) {
                force_vreg_to_reg(inst.src2, RCX);
                emit_mov_imm32_zext(RAX, 0);
            } else if (inst.src2 == 32) {
                force_vreg_to_reg(inst.src1, RAX);
                emit_mov_imm32_zext(RCX, 0);
            } else {
                force_two_vregs_to(inst.src1, RAX, inst.src2, RCX);
            }

            // CCMP clobbers RAX, RCX, RDX (via emit_materialize_flags on the
            // compare path, and via emit_mov_imm32_zext(RDX,...) on the else
            // path). force_two_vregs_to handled RAX/RCX eviction, but RDX
            // may still hold a live vreg (e.g., new_sp from a prior ADD).
            // Spill it before clobbering. Without this, the vreg in RDX is
            // lost — its value is only in the host reg, and the CCMP
            // overwrites it. This was the root cause of the FWD crash on
            // `toybox ls /` (v37 = new_sp was in RDX, lost to CCMP, then
            // STORE_MEM [v37+0x40] used garbage as the base address).
            flush_invalidate_host_regs((1u << RDX) | (1u << RAX) | (1u << RCX));

            // jcc do_compare (if cond TRUE, do the compare)
            size_t jcc_to_compare = emit_jcc_rel32_placeholder(cc);
            // --- else path: cond FALSE, set pstate = nzcv ---
            // Turn 95: do NOT set from_sub (bit 27) for the else path.
            // The else path sets NZCV directly from the instruction's nzcv
            // immediate — there's no subtraction, so C is NOT in SUB
            // convention (inverted). Setting from_sub=1 would cause the
            // flag loader to invert C, producing wrong flags. This was the
            // root cause of curl's "Port number was not a decimal" error:
            // CCMP's else path set C=1 (from nzcv) but from_sub=1 caused
            // the next conditional branch to see C=0, taking the wrong path.
            uint32_t pstate_else = (static_cast<uint32_t>(nzcv) << 28);
            // from_sub (bit 27) is NOT set — C is raw, not inverted.
            emit_mov_imm32_zext(RDX, pstate_else);
            emit_store32(CPU_REG, PSTATE_OFF, RDX);
            // Jump to end.
            size_t jmp_to_end = emit_jmp_rel32_placeholder();
            // --- cond TRUE path: do the compare ---
            size_t compare_off = code_buf_used_;
            // BUGFIX (Turn 56): use 32-bit sub/add for 32-bit CCMP/CCMN.
            // The old code always used emit_sub_reg/emit_add_reg (64-bit),
            // which computes the x86 Sign Flag from bit 63 instead of
            // bit 31. For 32-bit operations like `ccmp w3, #2`, if the
            // result is e.g. 0xFFFFFFFD (w3=0xFFFFFFFF, w3-2), the 64-bit
            // sub gives SF=0 (positive) while the 32-bit sub gives SF=1
            // (negative). This caused the ARM N flag to be wrong, leading
            // to incorrect conditional branches and eventually crashes
            // in programs that use 32-bit ccmp (e.g., curl --version).
            bool is_32bit_ccmp = (inst.sf == 0);
            if (is_sub) {
                if (is_32bit_ccmp) {
                    // 32-bit: sub eax, ecx (no REX.W)
                    bool need_rex = (RAX >= 8) || (RCX >= 8);
                    if (need_rex) emit_byte(rex(false, RCX>=8, false, RAX>=8));
                    emit_byte(0x29); emit_byte(modrm(3, RCX&7, RAX&7));
                } else {
                    emit_sub_reg(RAX, RCX);
                }
            } else {
                if (is_32bit_ccmp) {
                    bool need_rex = (RAX >= 8) || (RCX >= 8);
                    if (need_rex) emit_byte(rex(false, RCX>=8, false, RAX>=8));
                    emit_byte(0x01); emit_byte(modrm(3, RCX&7, RAX&7));
                } else {
                    emit_add_reg(RAX, RCX);
                }
            }
            // Materialize flags to pstate.
            emit_materialize_flags(is_sub);
            size_t end_off = code_buf_used_;

            // Patch jumps.
            int32_t rel_compare = static_cast<int32_t>(compare_off - (jcc_to_compare + 6));
            patch_jcc_rel32(jcc_to_compare, rel_compare);
            int32_t rel_end = static_cast<int32_t>(end_off - (jmp_to_end + 5));
            patch_jmp_rel32(jmp_to_end, rel_end);

            // After both paths, RAX/RCX/RDX hold garbage (materialize_flags
            // or mov_imm32 clobbered them). Drop any stale cache mappings
            // so later instructions reload from memory instead of using
            // the clobbered host regs.
            invalidate_host_regs((1u << RAX) | (1u << RCX) | (1u << RDX));

            flags_in_host_ = false;
            return 0;
        }

        // ── UDIV / SDIV — native x86 div/idiv ────────────────────────
        case IROp::UDIV:
        case IROp::SDIV: {
            // ARM64 UDIV/SDIV by zero returns 0 (no exception).
            // x86 div/idiv by zero raises SIGFPE. We emit a test+jz
            // to skip the div and set result=0 when divisor is zero.
            //
            // ARM64 SDIV of INT_MIN / -1 returns INT_MIN (no trap); x86 idiv
            // raises #DE → SIGFPE → guest crash. We detect the (INT_MIN, -1)
            // pair and short-circuit to INT_MIN before the idiv. Same fix
            // applies to the 32-bit form (INT32_MIN / -1 → INT32_MIN).
            clobber_flags();
            // use bitmask helpers instead of open-coded loop.
            flush_invalidate_host_regs((1u<<RAX)|(1u<<RCX)|(1u<<RDX));
            load_vreg_to_reg(RAX, inst.src1);  // dividend
            load_vreg_to_reg(RCX, inst.src2);  // divisor

            // For 32-bit division, zero-extend EAX into RAX (clear upper 32).
            // The dividend must be in EAX; if we loaded a 64-bit value,
            // the upper bits would corrupt the 32-bit div.
            if (inst.width == 32) {
                // mov eax, eax (zero-extends to RAX on x86-64)
                emit_byte(0x89); emit_byte(0xC0);
                // mov ecx, ecx (zero-extends divisor)
                emit_byte(0x89); emit_byte(0xC9);
            }

            // test rcx, rcx
            emit_test_reg(RCX, RCX);
            // jz zero_div (jump to xor eax,eax if divisor == 0)
            size_t jz_patch = emit_jcc_rel32_placeholder(4);  // JE
            // --- non-zero divisor path ---

            // For SDIV, also guard the (INT_MIN, -1) case to avoid x86 #DE.
            // Layout:
            //   cmp rcx, -1           ; is divisor -1?
            //   jne skip_ovfl         ; if not, do normal idiv
            //   cmp rax, INT_MIN      ; is dividend INT_MIN?
            //   jne skip_ovfl         ; if not, do normal idiv
            //   mov rax, INT_MIN      ; short-circuit result
            //   jmp past_zero
            // skip_ovfl:
            //   <idiv>
            size_t overflow_jmp_patch = 0;
            if (inst.op == IROp::SDIV) {
                // cmp rcx, -1
                if (inst.width == 64) {
                    emit_byte(0x48); emit_byte(0x83); emit_byte(0xF9); emit_byte(0xFF);
                } else {
                    emit_byte(0x83);  emit_byte(0xF9); emit_byte(0xFF);
                }
                // jne skip_ovfl
                size_t jne1_patch = emit_jcc_rel32_placeholder(5);  // JNE
                // Compare dividend to INT_MIN. We use RDX as scratch since
                // it is already invalidated above and idiv clobbers it anyway.
                if (inst.width == 64) {
                    // mov rdx, 0x8000000000000000 (10 bytes: 48 BA <imm64>)
                    emit_byte(0x48); emit_byte(0xBA);
                    emit_u32(0x00000000); emit_u32(0x80000000);
                    // cmp rax, rdx (3 bytes: 48 39 D0)
                    emit_byte(0x48); emit_byte(0x39); emit_byte(0xD0);
                } else {
                    // 32-bit: cmp eax, 0x80000000 (5 bytes: 3D 00 00 00 80)
                    emit_byte(0x3D); emit_u32(0x80000000);
                }
                // jne skip_ovfl
                size_t jne2_patch = emit_jcc_rel32_placeholder(5);  // JNE
                // Both checks matched → result = INT_MIN, jump past div.
                if (inst.width == 64) {
                    // mov rax, 0x8000000000000000 (10 bytes)
                    emit_byte(0x48); emit_byte(0xB8);
                    emit_u32(0x00000000); emit_u32(0x80000000);
                } else {
                    // mov eax, 0x80000000 (5 bytes)
                    emit_byte(0xB8); emit_u32(0x80000000);
                }
                // jmp past_zero
                overflow_jmp_patch = emit_jmp_rel32_placeholder();
                // patch both jne to skip this overflow-short-circuit
                size_t skip_off = code_buf_used_;
                patch_jcc_rel32(jne1_patch, static_cast<int32_t>(skip_off - (jne1_patch + 6)));
                patch_jcc_rel32(jne2_patch, static_cast<int32_t>(skip_off - (jne2_patch + 6)));
                // mark RDX as invalidated (we used it as scratch)
                invalidate_host_regs(1u << RDX);
            }

            if (inst.width == 32) {
                // 32-bit division: use div/idiv on EAX.
                // xor edx, edx (clear upper for unsigned) or cdq (sign-extend)
                if (inst.op == IROp::UDIV) {
                    emit_byte(0x31); emit_byte(0xD2);  // xor edx, edx
                    emit_byte(0xF7); emit_byte(0xF1);  // div ecx
                } else {
                    emit_byte(0x99);                    // cdq
                    emit_byte(0xF7); emit_byte(0xF9);  // idiv ecx
                }
            } else {
                // 64-bit division
                if (inst.op == IROp::UDIV) {
                    emit_byte(0x48); emit_byte(0x31); emit_byte(0xD2);  // xor rdx, rdx
                    emit_byte(0x48); emit_byte(0xF7); emit_byte(0xF1);  // div rcx
                } else {
                    emit_byte(0x48); emit_byte(0x99);                    // cqo
                    emit_byte(0x48); emit_byte(0xF7); emit_byte(0xF9);  // idiv rcx
                }
            }
            // jmp past_zero
            size_t jmp_patch = emit_jmp_rel32_placeholder();
            // --- zero divisor path: result = 0 ---
            size_t zero_off = code_buf_used_;
            patch_jcc_rel32(jz_patch, static_cast<int32_t>(zero_off - (jz_patch + 6)));
            emit_byte(0x48); emit_byte(0x31); emit_byte(0xC0);  // xor rax, rax
            // --- past_zero ---
            size_t past_off = code_buf_used_;
            patch_jmp_rel32(jmp_patch, static_cast<int32_t>(past_off - (jmp_patch + 5)));
            if (inst.op == IROp::SDIV && overflow_jmp_patch != 0) {
                patch_jmp_rel32(overflow_jmp_patch,
                                static_cast<int32_t>(past_off - (overflow_jmp_patch + 5)));
            }

            // For 32-bit results, writing to EAX zero-extends to RAX.
            int d = alloc_reg_for(inst.dest, RAX);
            if (d != RAX) emit_mov_reg(d, RAX);
            set_vreg_reg(inst.dest, d);  // cache the result
            return 0;
        }

        // ── SMADDL / UMADDL — widening multiply-accumulate ──────────
        case IROp::SMADDL:
        case IROp::UMADDL: {
            // SMADDL: dest = acc + (int64)(int32)src1 * (int64)(int32)src2
            // UMADDL: dest = acc + (uint64)(uint32)src1 * (uint64)(uint32)src2
            // x86: imul rax, rcx (64-bit multiply); add rax, acc
            clobber_flags();
            // use bitmask helpers instead of open-coded loop.
            flush_invalidate_host_regs((1u<<RAX)|(1u<<RCX)|(1u<<RDX));
            // Load src1 (32-bit, sign/zero-extended) into RAX
            load_vreg_to_reg(RAX, inst.src1);
            if (inst.op == IROp::SMADDL) {
                // cdqe (sign-extend EAX into RAX)
                emit_byte(0x48); emit_byte(0x98);
            } else {
                // mov eax, eax (zero-extend)
                emit_byte(0x89); emit_byte(0xC0);
            }
            // Load src2 (32-bit, extended) into RCX
            load_vreg_to_reg(RCX, inst.src2);
            if (inst.op == IROp::SMADDL) {
                // Sign-extend ECX into RCX (cdqe on RCX isn't directly
                // available; use movsxd rcx, ecx instead).
                // 48 63 c9 = movsxd rcx, ecx
                emit_byte(0x48); emit_byte(0x63); emit_byte(0xC9);
            } else {
                // mov ecx, ecx (zext)
                emit_byte(0x89); emit_byte(0xC9);
            }
            // imul rax, rcx (64-bit multiply — result in RAX, no RDX needed)
            emit_byte(0x48); emit_byte(0x0F); emit_byte(0xAF); emit_byte(0xC1);
            // BUGFIX (Turn 66): Load accumulator vreg via the vreg cache
            // (inst.aux), not directly from cpu.regs[]. The old code read
            // cpu.regs[inst.cond] which bypassed the vreg cache and could
            // read stale values if the accumulator was modified earlier
            // in the same block.
            load_vreg_to_reg(RDX, inst.aux);
            // add rax, rdx
            emit_byte(0x48); emit_byte(0x01); emit_byte(0xD0);
            int d = alloc_reg_for(inst.dest, RAX);
            if (d != RAX) emit_mov_reg(d, RAX);
            set_vreg_reg(inst.dest, d);  // cache the result (like UDIV)
            return 0;
        }

        // ── SMSUBL / UMSUBL — widening multiply-subtract ────────────
        case IROp::SMSUBL:
        case IROp::UMSUBL: {
            // SMSUBL: dest = acc - (int64)(int32)src1 * (int32)src2
            // UMSUBL: dest = acc - (uint64)(uint32)src1 * (uint32)src2
            clobber_flags();
            // use bitmask helpers instead of open-coded loop.
            flush_invalidate_host_regs((1u<<RAX)|(1u<<RCX)|(1u<<RDX));
            load_vreg_to_reg(RAX, inst.src1);
            if (inst.op == IROp::SMSUBL) {
                emit_byte(0x48); emit_byte(0x98);  // cdqe
            } else {
                emit_byte(0x89); emit_byte(0xC0);  // mov eax, eax
            }
            load_vreg_to_reg(RCX, inst.src2);
            if (inst.op == IROp::SMSUBL) {
                emit_byte(0x48); emit_byte(0x63); emit_byte(0xC9);  // movsxd rcx, ecx
            } else {
                emit_byte(0x89); emit_byte(0xC9);  // mov ecx, ecx
            }
            emit_byte(0x48); emit_byte(0x0F); emit_byte(0xAF); emit_byte(0xC1);  // imul rax, rcx
            // BUGFIX (Turn 66): Load accumulator vreg via the vreg cache.
            load_vreg_to_reg(RDX, inst.aux);
            // sub rdx, rax (dest = acc - product)
            emit_byte(0x48); emit_byte(0x29); emit_byte(0xC2);  // sub rdx, rax
            int d = alloc_reg_for(inst.dest, RDX);
            if (d != RDX) emit_mov_reg(d, RDX);
            set_vreg_reg(inst.dest, d);
            return 0;
        }

        default:
            return -1;  // not handled — caller falls through
    }
}

} // namespace arm64emu
