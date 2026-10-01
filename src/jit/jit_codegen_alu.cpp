// jit/jit_codegen_alu.cpp — FrostJIT ALU/arithmetic IR-op codegen.
//
// v1.4.5-alpha: split out of frostjit.cpp. This file holds the
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
                if (inst.dest > 32 && inst.dest < 4096)
                    jit_consts_[inst.dest] = inst.imm_value();
                // Fold lookahead: when the immediately-following op folds
                // this constant into an x86 immediate form (dead src2), the
                // mov we'd emit here is dead code — skip it. The pre-scan
                // (jit_translate.cpp) only marks IMMs whose dest's ONLY read
                // is that foldable consumer, so the conditions here match
                // the consumer's fold guards EXACTLY (including the imm32
                // sign-extension fit for the ALU class): a skipped mov whose
                // consumer then did NOT fold would leave the vreg unmapped
                // and the consumer's ensure_vreg would reload garbage.
                if (cur_op_index_ < fold_ahead_kind_.size()) {
                    uint8_t fa = fold_ahead_kind_[cur_op_index_];
                    if (fa == 1) {
                        int64_t c = static_cast<int64_t>(inst.imm_value());
                        if (static_cast<int64_t>(static_cast<int32_t>(c)) == c)
                            return 0;
                    } else if (fa == 2) {
                        return 0;
                    }
                }
                int d = alloc_reg_for(inst.dest, -1);
                if (inst.imm_value() <= 0xFFFFFFFFULL) {
                    emit_mov_imm32_zext(d, static_cast<uint32_t>(inst.imm_value()));
                } else {
                    emit_mov_imm64(d, inst.imm_value());
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
            // ── Constant-src2 immediate folding ──
            // If src2 is a block-local IMM constant (dead after this op and
            // not the dest), fold it into an x86 immediate form. The JIT ALU
            // ops are always 64-bit (guest W-reg results get a separate ZEXT
            // from the translator), so only constants that survive imm32
            // sign-extension fold: (int64)c == (int64)(int32)c. This covers
            // 12-bit add/sub immediates, stack-frame offsets, small masks
            // (0xFF/0x3F/...), and −1 (SUB #−1 → add r,−1).
            if (inst.op != IROp::MUL && inst.dest != inst.src2 &&
                inst.src1 != inst.src2) {
                auto cit = jit_consts_.find(inst.src2);
                if (cit != jit_consts_.end() && vreg_last_use_this_op(inst.src2)) {
                    int64_t c = static_cast<int64_t>(cit->second);
                    if (static_cast<int64_t>(static_cast<int32_t>(c)) == c) {
                        int32_t imm = static_cast<int32_t>(c);
                        int kind = (inst.op == IROp::ADD) ? 0
                                : (inst.op == IROp::OR)  ? 1
                                : (inst.op == IROp::AND) ? 4
                                : (inst.op == IROp::SUB) ? 5 : 6;
                        kill_vreg(inst.src2);  // free the dead const vreg's reg
                        int s1 = ensure_vreg(inst.src1);
                        int d;
                        if (inst.dest == inst.src1) {
                            d = s1;
                        } else {
                            d = alloc_reg_excluding(s1, -1);
                            if (d != s1) emit_mov_reg(d, s1);
                        }
                        emit_alu_imm(d, kind, imm);
                        if (inst.dest == inst.src1) {
                            vreg_dirty_[inst.dest] = true;
                            dirty_host_regs_ |= (1u << d);
                            vreg_last_use_[inst.dest] = ++regalloc_lru_counter_;
                        } else {
                            set_vreg_reg(inst.dest, d);
                        }
                        return 0;
                    }
                }
            }
            bool commutative = (inst.op != IROp::SUB);
            // Variant-2-safe operand ensure (2026-08-21 audit): the old
            // `s1 = ensure_vreg(src1); s2 = ensure_vreg(src2);` pair left
            // s1 stale whenever the second ensure Belady-evicted a
            // dead-after-this-op src1 (every alloc reg occupied) — the
            // emitter then read src2's register as src1. ensure_two_vregs
            // re-reads both homes after the last evicting call.
            int s1, s2;
            ensure_two_vregs(inst.src1, inst.src2, &s1, &s2);
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
                vreg_last_use_[inst.dest] = ++regalloc_lru_counter_;
            } else if (inst.dest == inst.src2 && commutative) {
                // dest == src2, commutative: compute in s2 (swap operands).
                d = s2;
                emit_alu_op(d, s1);
                vreg_dirty_[inst.dest] = true;
                dirty_host_regs_ |= (1u << d);
                vreg_last_use_[inst.dest] = ++regalloc_lru_counter_;
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
            clobber_flags();
            // ── Constant-count immediate shift ──
            // If src2 is a block-local IMM constant (dead after this op and
            // not the dest), emit `shl/shr/sar/ror r, imm8` and skip the
            // RCX/CL dance entirely. Counts are masked mod 64 (mod 32 for
            // 32-bit ROR) exactly as the CL-variable path does.
            if (inst.dest != inst.src2 && inst.src1 != inst.src2) {
                auto cit = jit_consts_.find(inst.src2);
                if (cit != jit_consts_.end() && vreg_last_use_this_op(inst.src2)) {
                    uint64_t cnt = cit->second;
                    kill_vreg(inst.src2);
                    int s1 = ensure_vreg(inst.src1);
                    int d;
                    if (inst.dest == inst.src1) {
                        d = s1;
                    } else {
                        d = alloc_reg_excluding(s1, -1);
                        if (d != s1) emit_mov_reg(d, s1);
                    }
                    bool is_32bit = (inst.gpr_shift_params().width == 32);
                    int kind = (inst.op == IROp::SHL) ? 4
                             : (inst.op == IROp::SHR) ? 5
                             : (inst.op == IROp::SAR) ? 7 : 1;
                    if (is_32bit) {
                        // 32-bit op: count mod 32 (ARM W-shift semantics),
                        // 32-bit form (no REX.W) so upper container zeroes.
                        uint8_t c = static_cast<uint8_t>(cnt & 0x1F);
                        if (c) {  // shl/shr/sar/ror r32d, imm8
                            if (d >= 8) emit_byte(0x41);
                            emit_byte(0xC1); emit_byte(modrm(3, kind, d & 7)); emit_byte(c);
                        }
                    } else {
                        emit_shift_imm8(d, kind, static_cast<uint8_t>(cnt & 0x3F));
                    }
                    if (inst.dest == inst.src1) {
                        vreg_dirty_[inst.dest] = true;
                        dirty_host_regs_ |= (1u << d);
                        vreg_last_use_[inst.dest] = ++regalloc_lru_counter_;
                    } else {
                        set_vreg_reg(inst.dest, d);
                    }
                    return 0;
                }
            }
            // x86 variable shifts use CL for the count. We force src2 into
            // RCX (clobbering its previous occupant), but leave src1 in
            // whatever reg it's cached in. dest is computed in src1's reg
            // when dest == src1, else in a fresh reg excluding src1 and RCX.
            int s1 = ensure_vreg(inst.src1);
            // If s1 is in RCX, move it elsewhere first so forcing src2 into
            // RCX doesn't lose src1.
            if (s1 == RCX) {
                // excl2 = RCX must stay intact (tmp receives s1 moved OUT of
                // RCX); excl1 = -1 (no in-place-safe operand here).
                int tmp = alloc_reg_excluding(-1, RCX);
                emit_mov_reg(tmp, RCX);
                reg_vreg_[RCX] = -1;
                vreg_home_[inst.src1] = tmp;
                reg_vreg_[tmp] = inst.src1;
                vreg_last_use_[inst.src1] = ++regalloc_lru_counter_;
                if (vreg_dirty_[inst.src1]) {
                    dirty_host_regs_ &= ~(1u << RCX);
                    dirty_host_regs_ |= (1u << tmp);
                }
                s1 = tmp;
            }
            // Force src2 into RCX (evict current occupant if any).
            force_vreg_to_reg(inst.src2, RCX);
            // x86 shifts/rotates already mask the count to 5/6 bits.
            // Preserve RCX: it still holds src2's cached, possibly live
            // value (zstd packs Huffman bits above the count in this value).
            auto emit_shift = [&](int d) {
                bool is_32bit = (inst.gpr_shift_params().width == 32);
                int kind = (inst.op == IROp::SHL) ? 4
                         : (inst.op == IROp::SHR) ? 5
                         : (inst.op == IROp::SAR) ? 7 : 1;
                if (is_32bit) {
                    // 32-bit op: ARM LSLV/LSRV/ASRV/RORV take the count mod
                    // 32, so mask CL to 5 bits and use the 32-bit form (no
                    // REX.W) — it also zeroes the upper container, matching
                    // AArch64 W-write semantics. (A 64-bit shift masked to
                    // 6 bits silently miscompiles counts >= 32: glibc
                    // _int_malloc's binmap `lsl w8,w2,w8` with bin 107
                    // produced 0 instead of 0x800 → shredded guest heap.)
                    emit_byte(rex(false, false, false, d >= 8));
                    emit_byte(0xD3);
                    emit_byte(modrm(3, kind, d & 7));
                } else {
                    emit_shift_cl(d, kind);
                }
            };
            int d;
            if (inst.dest == inst.src1) {
                d = s1;
                emit_shift(d);
                vreg_dirty_[inst.dest] = true;
                dirty_host_regs_ |= (1u << d);
                vreg_last_use_[inst.dest] = ++regalloc_lru_counter_;
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
            int bits = inst.sext_bits();
            if (bits < 64) {
                int sh = 64 - bits;
                emit_shift_imm8(d, 4, sh);
                emit_shift_imm8(d, 7, sh);
            }
            set_vreg_reg(inst.dest, d);
            return 0;
        }
        case IROp::ZEXT: {
            int bits = inst.zext_bits();
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
            // lzcnt (F3 0F BD) decodes as BSF on hosts without ABM —
            // and BSF leaves dest unchanged on zero input instead of
            // returning the full width. Fall back to the interpreter on
            // such hosts (same runtime-guard pattern as FRINT's SSE4.1).
            if (!has_lzcnt()) {
                emit_call_interp(inst.arm_pc, false);
                return 0;
            }
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
            if (inst.clz_params().bits == 32) {
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
            if (inst.rev64_params().bits == 32) {
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
            const CselParams cp = inst.csel_params();
            // Native CSEL/CSINC/CSINV/CSNEG via CMOVcc (2026-08-21).
            //
            // Semantics:
            //   CSEL  Rd = cond ? Rn : Rm
            //   CSINC Rd = cond ? Rn : (Rm + 1)
            //   CSINV Rd = cond ? Rn : ~Rm
            //   CSNEG Rd = cond ? Rn : -Rm
            //
            // Strategy (register-resident, no full flush):
            //   1. Ensure flags in host RFLAGS (targeted FLAGS3 flush when
            //      loading from pstate — preserves vregs in R8/R9/R11/R12+,
            //      BRCOND/CCMP precedent; the old flush_all_vregs+
            //      invalidate_all_vregs evicted everything and made every
            //      CSEL in a loop body nuke the register cache).
            //   2. Ensure both operands in their own registers
            //      (ensure_two_vregs — variant-2-safe) — no RAX/RCX staging.
            //   3. d = fresh reg excluding both operands; compute the ELSE
            //      value into d (mov/lea/not/neg). CSINC uses LEA (flags-
            //      free); CSINV/CSNEG pushfq/popfq around the transform.
            //   4. cmovcc d, s1 — cond TRUE selects the then-value. CMOV
            //      reads RFLAGS without modifying them.
            //   5. HI/LS carry inversion: cmc before the cmovcc and a
            //      RESTORING cmc after (the old emitter never restored CF —
            //      a later flag consumer or the epilogue materialize would
            //      see the flipped carry when flags were already in host).
            //
            // Carry polarity: arm_cond_to_x86() assumes SUB convention
            // (ARM C = NOT x86 CF). When flags came from ADD/TST
            // (carry_is_direct), CS/CC need swapped mapping, HI/LS need cmc.
            // If we loaded flags from pstate, the flags in pstate are
            // already correct and the epilogue should NOT re-materialize.
            // If we didn't load (flags were already in host), the epilogue
            // must still materialize them.
            bool loaded_from_pstate = !flags_in_host_;
            // Direct-carry HI/LS (flags from ADD/TST still in host): the
            // resolver's cmc mapping assumes the SUB convention — cmc+JA
            // would compute the INVERTED selection. The old full-flush
            // emitter got this right for free (it always reloaded flags
            // from pstate, which normalizes CF). Round-trip via pstate
            // here: materialize the in-host flags, then the normal load
            // path below re-establishes them with SUB convention.
            if (flags_in_host_ && !flags_from_sub_ &&
                (cp.cond & 0xE) == 0x8) {
                materialize_flags_to_pstate();  // sets flags_in_host_ = false
                loaded_from_pstate = true;      // pstate is now current; the
                                                // epilogue must NOT re-materialize
            }
            if (!flags_in_host_) {
                constexpr uint16_t FLAGS3 =
                    (1u << RAX) | (1u << RCX) | (1u << RDX);
                flush_dirty_host_regs(FLAGS3);
                flush_scratch_host_regs(FLAGS3);
                emit_load_flags_from_pstate();
                emit_normalize_cf_to_sub_convention();
                invalidate_host_regs(FLAGS3);
                flags_in_host_ = true;
                flags_from_sub_ = true;  // CF is now in SUB convention
            }
            // Resolve condition code. With flags_from_sub_=true (SUB convention,
            // whether originally from SUB or normalized after loading),
            // resolve_arm_cond_with_carry uses the default mapping.
            bool need_cmc = false;
            uint8_t cc = resolve_arm_cond_with_carry(cp.cond, need_cmc);
            // Operands in their own registers (XZR = vreg 32 → immediate 0).
            const bool z1 = (inst.src1 == 32);
            const bool z2 = (inst.src2 == 32);
            int s1 = -1, s2 = -1;
            if (!z2) s2 = ensure_vreg(inst.src2);
            if (!z1 && !z2) {
                ensure_two_vregs(inst.src1, inst.src2, &s1, &s2);
            } else if (!z1) {
                s1 = ensure_vreg(inst.src1);
            }
            // Fresh dest reg excluding both operand regs (variant-2-safe:
            // alloc after both ensures, afab35d contract).
            int d = alloc_reg_excluding(z1 ? -1 : s1, z2 ? -1 : s2);
            // XZR then-value needs a zero REGISTER (cmov has no immediate).
            if (z1) {
                s1 = alloc_reg_excluding(d, -1);
                emit_mov_imm32_zext(s1, 0);
            }
            // Else-value into d.
            if (z2) emit_mov_imm32_zext(d, 0);
            else if (d != s2) emit_mov_reg(d, s2);
            if (inst.op == IROp::CSINC) {
                // lea 1(d), d — flags-free increment.
                emit_byte(rex(true, d >= 8, false, d >= 8));
                emit_byte(0x8D);
                emit_modrm_disp(d, d, 1);
            } else if (inst.op == IROp::CSINV || inst.op == IROp::CSNEG) {
                emit_pushfq();
                if (inst.op == IROp::CSINV) emit_not_reg(d);
                else emit_neg_reg(d);
                emit_popfq();
            }
            if (need_cmc) emit_byte(0xF5);  // invert CF for HI/LS
            // cmovcc d, s1: REX.W + 0F 4x /r — dest d in the REG field,
            // source s1 in r/m (REX.R=d, REX.B=s1; cc same as jcc).
            emit_byte(rex(true, d >= 8, false, s1 >= 8));
            emit_byte(0x0F);
            emit_byte(0x40 + cc);
            emit_byte(modrm(3, d & 7, s1 & 7));
            if (need_cmc) emit_byte(0xF5);  // restore CF for later consumers
            // Store RDX-style tail: eager store + cache in d.
            store_reg_to_vreg(inst.dest, d);
            set_vreg_reg(inst.dest, d);
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
            const BfParams bp = inst.bf_params();
            int width = bp.sf ? 64 : 32;
            int immr = bp.immr;
            int imms = bp.imms;
            // Load src into RAX.
            // flush+invalidate FIRST so the
            // cache is empty and the subsequent memory access can't
            // interact with stale mappings. We then write the result
            // directly to the dest vreg's memory home and re-cache it.
            //
            // codegen below (shifts, ands, mov_imm64). Use targeted
            // flush+invalidate instead of the full flush_all_vregs+
            // invalidate_all_vregs — this preserves vregs cached in
            // R8/R9/R11/R12/R13/R15 across the bitfield op, eliminating
            // redundant reloads in tight loops containing bitfield ops.
            clobber_flags();  // shifts/ands clobber RFLAGS
            constexpr uint16_t BFM_CLOBBER = (1u << RAX) | (1u << RCX) | (1u << RDX);
            // Fast path: if src1 is a dead scratch vreg already cached in
            // RAX (e.g. a preceding LOAD_REG left it there), skip the
            // flush→reload sandwich entirely. Otherwise flush+invalidate
            // and load as before (also skipping the redundant reload when
            // src1 was still cached in RAX — the flush only writes memory).
            load_vreg_to_reg_fast(RAX, inst.src1, inst.dest, BFM_CLOBBER);
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
                    // SBFM's insert form (SBFIZ) sign-extends the selected
                    // low field before positioning it. UBFM (UBFIZ/LSL)
                    // keeps it zero-extended. Without this, negative
                    // 32-bit indices such as HUF's lowS == -1 become large
                    // positive offsets instead of addressing the sentinel
                    // node immediately before the table.
                    if (inst.op == IROp::SBFM) {
                        int sign_sh = width - (imms + 1);
                        if (width == 32) {
                            emit_byte(0xC1); emit_byte(modrm(3, 4, RAX & 7)); emit_byte(static_cast<uint8_t>(sign_sh));
                            emit_byte(0xC1); emit_byte(modrm(3, 7, RAX & 7)); emit_byte(static_cast<uint8_t>(sign_sh));
                        } else {
                            emit_shift_imm8(RAX, 4, sign_sh);
                            emit_shift_imm8(RAX, 7, sign_sh);
                        }
                    }
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
                // Shift the sign bit up to bit 63 so the 64-bit SAR below
                // sign-extends correctly. For 32-bit ops (sxtb/sxth/sbfx W)
                // `width - field_width` puts the sign bit at bit 31, but a
                // 64-bit SAR reads bit 63 — a negative byte (0xf8) would
                // come back as 248 instead of 0xfffffff8. The trailing
                // `mov %eax,%eax` truncates the 64-bit sign-extended result
                // to the 32-bit W container.
                int sh = 64 - field_width;
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
                int rd = static_cast<int>(inst.swar_rd());
                emit_load_arm(d, rd);
                set_vreg_reg(inst.dest, d);
            }
            return 0;
        case IROp::CCMP: {
            // CCMP/CCMN: if cond then set flags from (rn - rm) [CCMP]
            //            or (rn + rm) [CCMN]; else set flags to imm nzcv.
            // inst.width = nzcv field (4 bits), inst.cond = ARM cond,
            // inst.flags_op = 1 for CCMP (sub), 0 for CCMN (add).
            const CcmpParams cp = inst.ccmp_params();
            bool is_sub = cp.is_sub;
            uint8_t nzcv = cp.nzcv & 0xF;
            // Compute x86 cc (true when ARM cond is TRUE).
            bool need_cmc = false;
            uint8_t cc = resolve_arm_cond_with_carry(cp.cond, need_cmc);
            // Ensure flags in host.
            if (!flags_in_host_) {
                // emit_normalize_cf_to_sub_convention only clobber
                // RAX/RCX/RDX. Use targeted flush+invalidate to preserve
                // vregs cached in R8/R9/R11/R12/R13/R15.
                // No pushfq/popfq: see BRCOND comment for rationale.
                constexpr uint16_t FLAGS3 = (1u << RAX) | (1u << RCX) | (1u << RDX);
                flush_dirty_host_regs(FLAGS3);
                flush_scratch_host_regs(FLAGS3);
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
                invalidate_host_regs(FLAGS3);
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
            // The else path sets NZCV directly from the instruction's nzcv
            // immediate — there's no subtraction, so C is NOT in SUB
            // convention (inverted). Setting from_sub=1 would cause the
            // flag loader to invert C, producing wrong flags. This was the
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
            // which computes the x86 Sign Flag from bit 63 instead of
            // bit 31. For 32-bit operations like `ccmp w3, #2`, if the
            // result is e.g. 0xFFFFFFFD (w3=0xFFFFFFFF, w3-2), the 64-bit
            // sub gives SF=0 (positive) while the 32-bit sub gives SF=1
            // (negative). This caused the ARM N flag to be wrong, leading
            // to incorrect conditional branches and eventually crashes
            // in programs that use 32-bit ccmp (e.g., curl --version).
            bool is_32bit_ccmp = !cp.sf;
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
            const uint8_t div_bits = inst.div_bits();
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
            if (div_bits == 32) {
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
                if (div_bits == 64) {
                    emit_byte(0x48); emit_byte(0x83); emit_byte(0xF9); emit_byte(0xFF);
                } else {
                    emit_byte(0x83);  emit_byte(0xF9); emit_byte(0xFF);
                }
                // jne skip_ovfl
                size_t jne1_patch = emit_jcc_rel32_placeholder(5);  // JNE
                // Compare dividend to INT_MIN. We use RDX as scratch since
                // it is already invalidated above and idiv clobbers it anyway.
                if (div_bits == 64) {
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
                if (div_bits == 64) {
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
            if (div_bits == 32) {
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
