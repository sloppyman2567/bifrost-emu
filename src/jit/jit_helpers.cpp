// jit/jit_helpers.cpp — FrostJIT codegen helpers.
//
// v1.4.5-alpha: split out of frostjit.cpp. Holds:
//   - emit_fmov_helper  — GPR↔FP register move helper (used by FMOV ops)
//   - emit_call_interp  — emit a CALL_INTERP call site inside a block
//     (used when the JIT can't codegen an op and falls back to the
//     interpreter for one instruction)
#include "jit/frostjit.hpp"
#include "core/emulator.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
namespace arm64emu {
// ── emit_fmov_helper + emit_call_interp ─────────────────────────────────
void FrostJIT::emit_fmov_helper(int dir, int fp_field, uint16_t idx,
                                uint16_t src1, uint16_t dest) {
    int32_t fp_off = (fp_field == 0 ? V_LO_OFF : V_HI_OFF)
                   + static_cast<int>(idx) * 8;
    if (dir == 0) {
        // GPR → FP: load src1 vreg into RAX, store to fp_off.
        // We use RAX as a scratch for the memory store. src1's cached
        // value is NOT modified by the store, so we keep src1's mapping
        // intact for later readers.
        //
        // BUGFIX: if src1 is cached in a reg OTHER than RAX, the old code
        // did `emit_mov_reg(RAX, s)` which silently overwrote whatever
        // dirty vreg was in RAX — losing its value. This caused
        // printf("%f", 3.14) → "2.000000" under BIFROST_ENABLE_FWD=1
        // because v46 (the bfxil result holding x1's low 48 bits) was
        // in RAX and got clobbered by the FMOV_G2F that copies x9 to
        // v_lo[0]. Fix: spill RAX's occupant BEFORE overwriting it.
        int s = ensure_vreg(src1, RAX);
        if (s != RAX) {
            clobber_host_reg(RAX);  // spill dirty vreg in RAX before reuse
            emit_mov_reg(RAX, s);
        }
        emit_store(CPU_REG, fp_off, RAX);
        // FMOV_G2F (fp_field==0) also zeros v_hi[idx] per ARM semantics.
        // The zero-load clobbers RAX, so we must spill any dirty vreg
        // cached there BEFORE overwriting. Use a DIFFERENT reg (RCX) for
        // the zero to avoid clobbering src1 in RAX entirely.
        if (fp_field == 0) {
            int32_t vhi_off = V_HI_OFF + static_cast<int>(idx) * 8;
            // Use RCX as scratch for the zero (doesn't disturb src1 in RAX).
            // clobber_host_reg(RCX) spills any dirty vreg cached in RCX.
            clobber_host_reg(RCX);
            emit_mov_imm32_zext(RCX, 0);
            emit_store(CPU_REG, vhi_off, RCX);
        }
        // src1 stays cached in its reg (RAX or wherever ensure_vreg put it).
        // No mapping drop needed — the store didn't modify src1.
    } else {
        // FP → GPR: load fp_off into a fresh vreg for dest.
        int d = alloc_reg();
        emit_load(d, CPU_REG, fp_off);
        set_vreg_reg(dest, d);
    }
}
// ── emit_call_interp ───────────────────────────────────────────────────
void FrostJIT::emit_call_interp(uint64_t arm_pc, bool ends_block) {
    // flush ALL dirty vregs BEFORE materializing flags.
    // The previous code called emit_materialize_flags FIRST, which
    // clobbers RAX/RCX/RDX — if those held dirty vregs, their values
    // were lost before flush_all_vregs could spill them. This caused
    // state corruption in __multf3 (128-bit softfloat) blocks where
    // a dirty vreg in RAX was silently dropped, producing wrong FP
    // results (e.g. printf("%f", 3.14) → "2.000000").
    flush_all_vregs();
    // Materialize host flags to pstate if valid.
    if (flags_in_host_) {
        emit_materialize_flags(flags_from_sub_);
        flags_in_host_ = false;
        // emit_materialize_flags clobbers RAX/RCX/RDX. Drop their
        // cache mappings (values were already flushed above, so
        // dirty vregs are safe — just drop the stale associations).
        // use bitmask helper.
        invalidate_host_regs((1u<<RAX)|(1u<<RCX)|(1u<<RDX));
    }
    // flush_all_vregs already called above (before
    // materialize_flags). All dirty vregs are now in cpu.regs[]/stack.
    // SP (vreg 31) may have been flushed above, but force-check here
    // in case the materialize introduced a new dirty SP (it shouldn't).
    if (vreg_home_[31] >= 0 && vreg_dirty_[31]) {
        evict_vreg(31);
    }
    emit_push(WIN_REG);  // save R10 (caller-saved)  — 1 push
    emit_push(RAX);      // save RAX                 — 2 pushes (EVEN → no align fixup needed)
    // Set cpu.pc = arm_pc.
    emit_mov_imm_to_rax(arm_pc);
    emit_store(CPU_REG, PC_OFF, RAX);
    // Set args: RDI = emu, RSI = cpu.
    emit_mov_reg(RDI, EMU_REG);
    emit_mov_reg(RSI, CPU_REG);
    emit_call_aligned(&jit_interp_step, /*num_pushed=*/2);
    emit_pop(RAX);       // restore RAX
    emit_pop(WIN_REG);   // restore WIN_REG
    // Reload PC into RAX.
    emit_load(RAX, CPU_REG, PC_OFF);
    // invalidate ALL cache mappings after the call.
    // We can't keep callee-saved vregs cached because the interpreter
    // may have modified cpu.regs[] for registers that the JIT has
    // cached as non-dirty. A STORE_REG earlier in the block may have
    // written to cpu.regs[R], but a vreg loaded from R before that
    // STORE_REG would still hold the OLD value. After the interpreter
    // call, we must reload everything from cpu.regs[] to be safe.
    invalidate_all_vregs();
    if (!ends_block) {
        uint64_t next_pc = arm_pc + 4;
        if (next_pc <= 0xFFFFFFFFULL) {
            emit_mov_imm32_zext(RCX, static_cast<uint32_t>(next_pc));
        } else {
            emit_mov_imm64(RCX, next_pc);
        }
        emit_cmp_reg(RCX, RAX);
        size_t jne_patch = emit_jcc_rel32_placeholder(5);
        call_interp_branch_patches_.push_back(jne_patch);
    }
}
} // namespace arm64emu
