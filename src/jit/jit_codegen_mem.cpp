// jit/jit_codegen_mem.cpp — FrostJIT memory IR-op codegen.
//
// v1.4.5-alpha (Turn 37): split out of frostjit.cpp. This file holds the
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
            // BUGFIX (Turn 57): when inst.sf=1 (is_fp), load from cpu.v_lo[]
            // instead of cpu.regs[]. This is used for FP LDR/STR.
            {
                kill_vreg(inst.dest);
                int d;
                if (inst.src1 <= 31 && vreg_home_[inst.src1] >= 0) {
                    int s = vreg_home_[inst.src1];
                    d = alloc_reg_excluding(s, -1);
                    emit_mov_reg(d, s);
                } else if (inst.sf == 1) {
                    // FP register: load from cpu.v_lo[src1]
                    d = alloc_reg();
                    emit_load(d, CPU_REG, V_LO_OFF + 8 * inst.src1);
                } else {
                    d = alloc_reg();
                    emit_load_arm(d, inst.src1);
                }
                set_vreg_reg(inst.dest, d);
            }
            return 0;

        case IROp::STORE_REG:
            // arm64_reg[dest] = src1. Write to cpu.regs[dest] (or v_lo if is_fp).
            // DON'T cache dest — leave it uncached so it reloads from
            // cpu.regs[dest] if needed (correct value, just written).
            // DON'T touch src1 — it stays cached in its reg.
            // This avoids all aliasing problems and eliminates spills.
            //
            // BUGFIX (Turn 57): when inst.sf=1 (is_fp), write to cpu.v_lo[]
            // instead of cpu.regs[]. This is used for FP LDR/STR.
            {
                int s = ensure_vreg(inst.src1);
                if (inst.sf == 1 && inst.dest <= 30) {
                    // FP register: store to cpu.v_lo[dest]
                    emit_store(CPU_REG, V_LO_OFF + 8 * inst.dest, s);
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
            flush_invalidate_host_regs(MEM_CLOBBER);
            load_vreg_to_reg(RAX, inst.src1);
            emit_load_mem(RAX, RAX, static_cast<int32_t>(inst.imm), inst.width, false);
            store_reg_to_vreg(inst.dest, RAX);
            return 0;
        }

        case IROp::STORE_MEM: {
            // Same as LOAD_MEM: only flush caller-saved dirty vregs.
            clobber_flags();
            constexpr uint16_t MEM_CLOBBER =
                (1u << RAX) | (1u << RCX) | (1u << RDX) |
                (1u << R8)  | (1u << R9)  | (1u << R11);
            flush_invalidate_host_regs(MEM_CLOBBER);
            load_vreg_to_reg(RAX, inst.src1);
            load_vreg_to_reg(RCX, inst.src2);
            emit_store_mem(RAX, static_cast<int32_t>(inst.imm), RCX, inst.width);
            return 0;
        }

        default:
            return -1;  // not handled — caller falls through
    }
}

} // namespace arm64emu
