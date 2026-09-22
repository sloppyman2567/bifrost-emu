// jit/jit_codegen_mem.cpp — FrostJIT memory IR-op codegen.
//
// v1.4.5-alpha: split out of frostjit.cpp. This file holds the
// LOAD_MEM / STORE_MEM / LOAD_REG / STORE_REG case bodies of the IR-op
// switch, extracted into a separate method (compile_ir_mem) for
// readability. The main switch in frostjit.cpp dispatches to this method
// before its residual cases.
//
// No behavior change — pure file split. The method is a member of
// FrostJIT (declared in include/jit/frostjit.hpp) so it has full access
// to the JIT's emit_*, alloc_*, flush_*, etc. helpers.
//
// Return value (int — see frostjit.hpp):
//   -1 = op not handled here (caller falls through to next dispatcher)
//    0 = op handled, does NOT end the block
//    1 = op handled AND ends the block
// All memory ops in this file return 0 (handled, does not end block).
#include "jit/frostjit.hpp"
#include "core/emulator.h"
#include "ir/ir.hpp"
#include <cstddef>
#include <cstdint>
namespace arm64emu {
// ── FrostJIT::compile_ir_mem ───────────────────────────────────────────
// Handles LOAD_MEM, STORE_MEM, LOAD_REG, STORE_REG. These are the only
// IR ops that touch cpu.regs[]/cpu.v_lo[]/memory directly via the
// regalloc-aware helpers (load_vreg_to_reg, store_reg_to_vreg,
// emit_load_mem, emit_store_mem, emit_load_arm, emit_store_arm).
int FrostJIT::compile_ir_mem(const IRInst& inst) {
    switch (inst.op) {
        case IROp::LOAD_REG:
            // dest = arm64_reg[src1]. src1 is the ARM64 reg index.
            //
            // If the ARM reg vreg (src1, in 0..31) is already cached in a
            // host reg — e.g., because a previous FP_F2I / FP_I2F / FMOV_F2G
            // wrote directly to it via store_reg_to_vreg(src1, RAX) without
            // going through STORE_REG — reuse the cached value. Otherwise
            // we would emit a load from cpu.regs[src1], which is STALE
            // (the dirty vreg hasn't been flushed yet). This was the root
            // cause of `fcvtzs w1, d0; add w19, w19, w1` losing the
            // conversion result: FP_F2I cached vreg 1, then LOAD_REG v34, x1
            // reloaded the stale cpu.regs[1] instead of the cached vreg 1.
            //
            // instead of cpu.regs[]. This is used for FP LDR/STR.
            {
                kill_vreg(inst.dest);
                int d;
                if (inst.src1 <= 31 && vreg_home_[inst.src1] >= 0) {
                    int s = vreg_home_[inst.src1];
                    d = alloc_reg_excluding(s, -1);
                    emit_mov_reg(d, s);
                } else if (inst.is_fp_load()) {
                    // FP register: load from cpu.v_lo[src1] (or, when the fp
                    // cache pins src1, the pinned XMM — reg-reg move).
                    d = alloc_reg();
                    int xs = vec_xmm(inst.src1);
                    if (xs >= 0) {
                        emit_vmovq_xmm_to_gpr(d, xs);
                    } else {
                        emit_load(d, CPU_REG, V_LO_OFF + 8 * inst.src1);
                    }
                } else {
                    d = alloc_reg();
                    emit_load_arm(d, inst.src1);
                }
                set_vreg_reg(inst.dest, d);
            }
            return 0;
        case IROp::STORE_REG:
            // arm64_reg[dest] = src1. Write to cpu.regs[dest] (or v_lo if is_fp).
            //
            // Phase 2 (self-loop / tier-2 region): KEEP dest cached after the
            // store and DEFER the cpu.regs[] write. The value stays in a
            // register (a pinned loop-carried vreg in R12-R15, or the
            // transferred reg of a dead src1) marked DIRTY; the block's own
            // later LOAD_REG reads reuse the cached register, and the loop
            // exit (self-loop fall-through / region exit, both of which run
            // flush_all_vregs before returning OR chaining) writes it back to
            // cpu.regs[dest] ONCE. Per loop iteration this replaces a store→
            // cpu.regs→load round trip with pure register traffic.
            //
            // Deferral is safe ONLY because keep_store_dest_ is enabled
            // exclusively for self-loop blocks and back-edge regions whose
            // exits always flush: a self-loop's taken edge is the loop-back
            // (taken target == start_pc, so the exit is the fall-through →
            // shared epilogue with flush_all_vregs), and regions flush at
            // every cold/side exit. Call-like ops (interpreter, BL/BLR, SVC)
            // are excluded from pinning — they read cpu.regs[] directly and
            // would see the stale (deferred) value.
            //
            //   * Pinned arch vreg (arch_pin_[dest] >= 0): move src1 into the
            //     pin (the loop reads it from there), mark dest dirty.
            //   * Unpinned, src1 dead after this op: transfer src1's register
            //     to dest (dest stays hot for its later reads), dirty.
            //   * src1 == dest: dest's own reg already holds the value, dirty.
            //   * Otherwise (src1 still live): old behavior — eager store to
            //     cpu.regs[dest], kill the dest mapping.
            // src1 itself is never touched (stays cached in its reg).
            //
            // instead of cpu.regs[]. This is used for FP LDR/STR.
            {
                // A kept dest must live in a register the BRCOND term leaves
                // intact: the term's flag-prep + mov-imm clobber RAX/RCX/RDX/
                // R8 (FLAGS3) EVERY iteration, and a kept dest in one of
                // those would be read back garbage by the next iteration's
                // LOAD_REG (compiled as a direct reg→reg mov). Pins (R12-R15)
                // and R9/R11 are untouched by the term, so only those are
                // safe. A register that is another vreg's pin is also unsafe
                // to steal (it holds that vreg's loop-carried value).
                if (!inst.is_fp_store() && inst.dest <= 30 && keep_store_dest_) {
                    int pin = arch_pin_[inst.dest];
                    if (pin >= 0) {
                        int s = ensure_vreg(inst.src1);
                        if (s != pin) emit_mov_reg(pin, s);
                        if (vreg_home_[inst.dest] != pin) {
                            if (vreg_home_[inst.dest] >= 0) {
                                dirty_host_regs_ &= ~(1u << vreg_home_[inst.dest]);
                                reg_vreg_[vreg_home_[inst.dest]] = -1;
                            }
                            vreg_home_[inst.dest] = pin;
                            reg_vreg_[pin] = inst.dest;
                        }
                        // Deferred store: dest lives in its pin, dirty.
                        vreg_dirty_[inst.dest] = true;
                        dirty_host_regs_ |= (1u << pin);
                        vreg_last_use_[inst.dest] = ++regalloc_lru_counter_;
                        return 0;
                    }
                    int s = ensure_vreg(inst.src1);
                    const bool s_safe =
                        (s != RAX && s != RCX && s != RDX && s != R8) &&
                        !(pinned_host_regs_ & (1u << s));
                    if (s_safe && inst.src1 == inst.dest) {
                        // Self-store: dest's own reg already holds the value.
                        vreg_dirty_[inst.dest] = true;
                        dirty_host_regs_ |= (1u << s);
                        vreg_last_use_[inst.dest] = ++regalloc_lru_counter_;
                        return 0;
                    }
                    if (s_safe &&
                        vreg_last_use_op_[inst.src1] <= static_cast<int>(cur_op_index_)) {
                        // src1 dead after this op — transfer its reg to dest.
                        if (vreg_home_[inst.dest] >= 0) {
                            dirty_host_regs_ &= ~(1u << vreg_home_[inst.dest]);
                            reg_vreg_[vreg_home_[inst.dest]] = -1;
                            vreg_home_[inst.dest] = -1;
                        }
                        vreg_home_[inst.dest] = s;
                        reg_vreg_[s] = inst.dest;
                        vreg_home_[inst.src1] = -1;
                        vreg_dirty_[inst.src1] = false;
                        vreg_dirty_[inst.dest] = true;
                        dirty_host_regs_ |= (1u << s);
                        vreg_last_use_[inst.dest] = ++regalloc_lru_counter_;
                        return 0;
                    }
                    // src1 still live (or its reg is unsafe) — eager store.
                    emit_store_arm(inst.dest, s);
                    kill_vreg(inst.dest);
                    return 0;
                }
                int s = ensure_vreg(inst.src1);
                if (inst.is_fp_store() && inst.dest <= 30) {
                    // FP register: store to cpu.v_lo[dest] (or, when the fp
                    // cache pins dest, the pinned XMM — reg-reg move + dirty).
                    int xd = vec_xmm(inst.dest);
                    if (xd >= 0) {
                        emit_vmovq_gpr_to_xmm(xd, s);
                        vec_cache_mark_dirty(inst.dest);
                    } else {
                        emit_store(CPU_REG, V_LO_OFF + 8 * inst.dest, s);
                    }
                } else {
                    emit_store_arm(inst.dest, s);
                }
                // Kill any stale dest mapping (dest's value is now in cpu.regs/v_lo).
                if (inst.dest <= 31) {
                    kill_vreg(inst.dest);
                }
            }
            return 0;
        case IROp::LOAD_MEM: {
            const MemParams mp = inst.load_mem_params();
            // Memory access via emit_load_mem. The slow path calls
            // jit_load_mem_slow (clobbers caller-saved regs); the fast
            // path uses RAX/RDX/RCX/R10 internally. Now that
            // load_vreg_to_reg is cache-aware, we only need to flush
            // caller-saved dirty vregs — callee-saved vregs (R12/R13/
            // R15) survive the C call and load_vreg_to_reg will mov
            // from them correctly.
            //
            // replaced O(max_vreg_) flush + 6-reg
            // invalidate loop with two O(popcount) bitmask walks.
            clobber_flags();
            constexpr uint16_t MEM_CLOBBER =
                (1u << RAX) | (1u << RCX) | (1u << RDX) |
                (1u << R8)  | (1u << R9)  | (1u << R11);
            // Fast path: if src1 is a dead scratch vreg already cached in
            // RAX, keep it there (skip the flush→reload sandwich). The
            // address is consumed by emit_load_mem, which then OVERWRITES
            // RAX with the loaded data — so drop src1's mapping afterwards
            // (safe: src1 is dead, and its value was consumed as the base).
            //
            // Direct-index fast path (indexed addressing, 2026-08-21): the
            // emitter NEVER destroys the address register, so a src1 cached
            // in a CALLEE-SAVED reg (an arch pin — the common loop-carried
            // pointer shape) rides the SIB index directly: no staging copy
            // into RAX at all. Only taken when neither keep applies; the
            // full MEM_CLOBBER flush+invalidate still protects other live
            // vregs from the slow-path C call.
            const bool keep_rax =
                vreg_fast_keep_candidate(inst.src1, RAX, inst.dest);
            const int ahome = vreg_home_[inst.src1];
            if (!keep_rax && ahome >= R12 && ahome <= R15) {
                flush_dirty_host_regs(MEM_CLOBBER);
                flush_scratch_host_regs(MEM_CLOBBER);
                invalidate_host_regs(MEM_CLOBBER);
                emit_load_mem(RAX, ahome, static_cast<int32_t>(mp.offset),
                              mp.width, false);
            } else {
                uint16_t kept = load_vreg_to_reg_fast(RAX, inst.src1,
                                                      inst.dest, MEM_CLOBBER);
                emit_load_mem(RAX, RAX, static_cast<int32_t>(mp.offset),
                              mp.width, false);
                if (kept & (1u << RAX)) kill_vreg(inst.src1);
            }
            // Keep dest cached in RAX (set_vreg_reg) instead of storing to
            // memory: the loaded value is usually consumed immediately
            // (STORE_REG / next op), and store_reg_to_vreg would kill the
            // mapping so the consumer reloads from the stack slot — a
            // redundant sandwich. The epilogue flushes dest if the block ends
            // before it's read.
            set_vreg_reg(inst.dest, RAX);
            return 0;
        }
        case IROp::STORE_MEM: {
            const MemParams mp = inst.store_mem_params();
            // Same as LOAD_MEM: only flush caller-saved dirty vregs.
            clobber_flags();
            constexpr uint16_t MEM_CLOBBER =
                (1u << RAX) | (1u << RCX) | (1u << RDX) |
                (1u << R8)  | (1u << R9)  | (1u << R11);
            // Fast path for both operands: if src1/src2 are dead scratch
            // vregs already cached in RAX/RCX, keep them there. emit_store_mem
            // (indexed-address variant, 2026-08-21) PRESERVES both RAX (the
            // addr rides the SIB index — never copied or destroyed) and RCX
            // (pushed/popped around the slow call), so kept mappings stay
            // valid after the store. Decide BOTH operands BEFORE flushing
            // so src2's cache isn't wiped by src1's flush, then
            // flush+invalidate only the non-kept regs.
            bool keep1 = vreg_fast_keep_candidate(inst.src1, RAX, inst.dest);
            bool keep2 = vreg_fast_keep_candidate(inst.src2, RCX, inst.dest);
            // Direct-index fast path (2026-08-21): operands cached in
            // CALLEE-SAVED regs (arch pins) are passed straight to the
            // emitter — the address rides the SIB index and the value is
            // read from its own register, so the pin→RAX/RCX staging movs
            // disappear. Only when neither keep applies (a keep is cheaper
            // still); the full MEM_CLOBBER flush+invalidate still runs.
            const int ahome = vreg_home_[inst.src1];
            const int vhome = vreg_home_[inst.src2];
            if (!keep1 && !keep2 &&
                ahome >= R12 && ahome <= R15 &&
                vhome >= R12 && vhome <= R15) {
                flush_dirty_host_regs(MEM_CLOBBER);
                flush_scratch_host_regs(MEM_CLOBBER);
                invalidate_host_regs(MEM_CLOBBER);
                emit_store_mem(ahome, static_cast<int32_t>(mp.offset),
                               vhome, mp.width);
                return 0;
            }
            uint16_t kept_mask = 0;
            if (keep1) kept_mask |= (1u << RAX);
            if (keep2) kept_mask |= (1u << RCX);
            flush_dirty_host_regs(MEM_CLOBBER & ~kept_mask);
            flush_scratch_host_regs(MEM_CLOBBER & ~kept_mask);
            invalidate_host_regs(MEM_CLOBBER & ~kept_mask);
            if (!keep1) load_vreg_to_reg(RAX, inst.src1);
            if (!keep2) load_vreg_to_reg(RCX, inst.src2);
            emit_store_mem(RAX, static_cast<int32_t>(mp.offset), RCX, mp.width);
            return 0;
        }
        default:
            return -1;  // not handled — caller falls through
    }
}
} // namespace arm64emu
