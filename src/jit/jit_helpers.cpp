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
// Validate native memory accesses before dereferencing the host window.
// The cold path commits the state BEFORE this instruction, steps the actual
// guest instruction (including signal delivery), then returns immediately.
// It never rejoins the block or enters a patched chain with handler state.
void FrostJIT::emit_memory_guard(const IRInst& inst) {
    if (!guest_memory_) return;
    uint16_t address;
    uint64_t offset = 0;
    unsigned size;
    uint8_t access;
    switch (inst.op) {
        case IROp::LOAD_MEM:
        case IROp::STORE_MEM: {
            auto p = inst.op == IROp::LOAD_MEM ? inst.load_mem_params()
                                              : inst.store_mem_params();
            address = inst.src1; offset = p.offset; size = p.width;
            access = inst.op == IROp::LOAD_MEM ? Memory::GUEST_PROT_READ
                                              : Memory::GUEST_PROT_WRITE;
            break;
        }
        case IROp::SIMD_LD16: {
            auto p = inst.ld16_params();
            address = inst.src1; offset = p.offset;
            size = 16 * (p.count ? p.count : 1);
            access = Memory::GUEST_PROT_READ;
            break;
        }
        case IROp::SIMD_ST16: {
            auto p = inst.st16_params();
            address = inst.src1; offset = p.offset;
            size = 16 * (p.count ? p.count : 1);
            access = Memory::GUEST_PROT_WRITE;
            break;
        }
        case IROp::ATOMIC:
            address = inst.src1; size = inst.atomic_params().width;
            access = Memory::GUEST_PROT_READ | Memory::GUEST_PROT_WRITE;
            break;
        case IROp::LDXR_FAST: case IROp::STXR_FAST: case IROp::STLR_FAST:
            address = inst.src1; size = inst.llsc_width();
            access = inst.op == IROp::LDXR_FAST ? Memory::GUEST_PROT_READ
                                               : Memory::GUEST_PROT_WRITE;
            break;
        default: return;
    }
    // Check a pair's entire footprint before its first load/store changes
    // any architectural register. In particular LDP may alias rt with rn.
    if (inst.arm_pc) {
        DecodedInst d;
        if (decode(d, guest_memory_->fetch_inst(inst.arm_pc)) &&
            (d.cls == InstClass::LDP || d.cls == InstClass::STP)) {
            uint64_t first_offset = d.mode == 1 ? 0 : static_cast<uint64_t>(d.disp);
            if (offset == first_offset) {
                unsigned opc = (d.raw >> 30) & 3;
                unsigned element = d.is_vec ? (4u << opc) : (opc == 2 ? 8 : 4);
                size = 2 * element;
            }
        }
    }
    clobber_flags();
    constexpr uint16_t scratch = (1u << RAX) | (1u << RCX) | (1u << RDX);
    flush_invalidate_host_regs(scratch);
    load_vreg_to_reg(RAX, address);
    // The guard's arithmetic must not leak into the memory emitter. Keep
    // the entry values on both exits: an address may still be reused by
    // the emitter, while the guard changes it into a page index below.
    emit_push(RAX); emit_push(RCX); emit_push(RDX);
    if (offset) {
        emit_mov_imm64(RDX, offset);
        emit_add_reg(RAX, RDX);
    }
    std::vector<size_t> cold;
    emit_mov_imm32_zext(RCX, static_cast<uint32_t>(Memory::DIRECT_WINDOW_SIZE - size));
    emit_cmp_reg(RAX, RCX);
    cold.push_back(emit_jcc_rel32_placeholder(7)); // JA (also catches wrap/negative addresses)
    emit_mov_reg(RCX, RAX);
    if (size > 1) emit_add_reg_imm(RCX, size - 1);
    emit_shift_imm8(RAX, 5, 12);
    emit_shift_imm8(RCX, 5, 12);
    const uint64_t flags = reinterpret_cast<uint64_t>(guest_memory_->direct_page_flags());
    const uint8_t required = Memory::GUEST_PAGE_MAPPED | access;
    emit_mov_imm64(RDX, flags);
    emit_add_reg(RDX, RAX);
    emit_load8(RAX, RDX, 0);
    emit_alu_imm(RAX, 4, required); // AND
    emit_alu_imm(RAX, 7, required); // CMP
    cold.push_back(emit_jcc_rel32_placeholder(5)); // JNE
    emit_mov_imm64(RDX, flags);
    emit_add_reg(RDX, RCX);
    emit_load8(RAX, RDX, 0);
    emit_alu_imm(RAX, 4, required);
    emit_alu_imm(RAX, 7, required);
    cold.push_back(emit_jcc_rel32_placeholder(5));
    size_t hot = emit_jmp_rel32_placeholder();
    const size_t cold_offset = code_buf_used_;
    for (size_t p : cold)
        patch_jcc_rel32(p, static_cast<int32_t>(cold_offset - (p + 6)));
    emit_pop(RDX); emit_pop(RCX); emit_pop(RAX);
    flush_all_vregs_keep();
    // A self-loop/region may carry a dirty architectural pin from a write
    // later in the previous iteration, even when it is statically clean
    // at this guard. Commit all mapped architectural registers on this exit.
    for (int v = 0; v < 32; ++v)
        if (vreg_home_[v] >= 0) emit_store_arm(v, vreg_home_[v]);
    // Include loop-carried vector writes, preserving compile-time dirtiness
    // so the normal epilogue still emits its own writeback.
    vec_cache_writeback_all_pinned(false);
    emit_mov_imm64(RAX, inst.arm_pc);
    emit_store(CPU_REG, PC_OFF, RAX);
    emit_load(RDI, RBP, emu_slot_off());
    emit_mov_reg(RSI, CPU_REG);
    emit_call_aligned(&jit_interp_step, 0);
    emit_load(RAX, CPU_REG, PC_OFF);
    emit_byte(0x48); emit_byte(0x89); emit_byte(0xEC); // mov rsp, rbp
    emit_pop(R15); emit_pop(R14); emit_pop(R13);
    emit_pop(R12); emit_pop(RBP); emit_pop(RBX);
    emit_ret();
    patch_jmp_rel32(hot, static_cast<int32_t>(code_buf_used_ - (hot + 5)));
    emit_pop(RDX); emit_pop(RCX); emit_pop(RAX);
}
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
        // ── FP-cache path (FMOV_G2F only — lo half): ────────────────
        // Store the GPR value into the pinned XMM (reg-reg) instead of
        // memory, mark it dirty, and zero v_hi in memory as usual. The
        // G2FHI form (fp_field=1) always uses the memory path — v_hi is
        // never cached in fp-cache blocks.
        if (fp_cache_active_ && fp_field == 0) {
            int xd = vec_xmm(idx);
            if (xd >= 0) {
                emit_vmovq_gpr_to_xmm(xd, s);
                vec_cache_mark_dirty(idx);
                fp_zero_hi(idx);
                // src1 stays cached in s (value not modified).
                return;
            }
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
        if (fp_cache_active_ && fp_field == 0) {
            int xs = vec_xmm(idx);
            if (xs >= 0) {
                // Pinned source: reg-reg move (no memory).
                emit_vmovq_xmm_to_gpr(d, xs);
                set_vreg_reg(dest, d);
                return;
            }
        }
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
    // ── Vector cache guard ──────────────────────────────────────────────
    // jit_interp_step is a real host function call; per the SysV ABI all
    // XMM0-15 are caller-saved and would clobber the guest vectors pinned
    // in XMM3-15. Write them back to cpu.v_lo/v_hi before the call and
    // reload them after. In a cache-active block the interp call is
    // EXPECTED to be GPR-only (the pre-scan requires all vector ops
    // cache-aware); writeback+reload defensively covers any escaped-SIMD
    // CALL_INTERP by reloading cpu.v_lo/v_hi after.
    if (vec_cache_active_) {
        vec_cache_writeback_all();
    }
    emit_push(WIN_REG);  // save R10 (caller-saved)  — 1 push
    emit_push(RAX);      // save RAX                 — 2 pushes (EVEN → no align fixup needed)
    // Set cpu.pc = arm_pc.
    emit_mov_imm_to_rax(arm_pc);
    emit_store(CPU_REG, PC_OFF, RAX);
    // Set args: RDI = emu, RSI = cpu.
    emit_load(RDI, RBP, emu_slot_off());
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
    if (vec_cache_active_) {
        vec_emit_prologue_loads();
    }
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
// ── emit_call_vdso_clock — native vDSO clock fast path ─────────────────
// Like emit_call_interp, but calls jit_vdso_clock_svc(emu, cpu, svc_pc)
// instead of stepping the interpreter. Used for SVC instructions that live
// inside the vDSO mapping: the clock stubs (gettimeofday/clock_gettime/
// clock_getres) are handled by reading the host clock directly, skipping
// step()/decode/syscall-dispatch entirely. Non-clock vDSO svcs (e.g.
// __kernel_rt_sigreturn) fall back to Emulator::syscall() inside the helper.
void FrostJIT::emit_call_vdso_clock(uint64_t arm_pc) {
    flush_all_vregs();
    if (flags_in_host_) {
        emit_materialize_flags(flags_from_sub_);
        flags_in_host_ = false;
        invalidate_host_regs((1u<<RAX)|(1u<<RCX)|(1u<<RDX));
    }
    if (vreg_home_[31] >= 0 && vreg_dirty_[31]) {
        evict_vreg(31);
    }
    emit_push(WIN_REG);  // save R10 (caller-saved)  — 1 push
    emit_push(RAX);      // save RAX                 — 2 pushes (EVEN → no align fixup)
    // Set cpu.pc = svc_pc (the helper advances it to svc_pc+4).
    emit_mov_imm_to_rax(arm_pc);
    emit_store(CPU_REG, PC_OFF, RAX);
    // Set args: RDI = emu, RSI = cpu, RDX = svc_pc.
    emit_load(RDI, RBP, emu_slot_off());
    emit_mov_reg(RSI, CPU_REG);
    emit_mov_reg(RDX, RAX);
    emit_call_aligned(&jit_vdso_clock_svc, /*num_pushed=*/2);
    emit_pop(RAX);       // restore RAX
    emit_pop(WIN_REG);   // restore WIN_REG
    // Reload PC into RAX.
    emit_load(RAX, CPU_REG, PC_OFF);
    // The helper may have touched cpu.regs[]/vregs — reload everything.
    invalidate_all_vregs();
}
// ── emit_call_native_svc — native syscall dispatch ─────────────────────
// Like emit_call_vdso_clock, but calls jit_native_svc(emu, cpu, svc_pc)
// instead of stepping the interpreter. Used for non-vDSO SVCs: the helper
// advances cpu.pc to svc_pc+4 and runs Emulator::syscall() directly,
// skipping step()/decode/syscall-dispatch. SVC never coexists with the
// vec cache (SVC disqualifies the block in vec_cache_may_enable), so no
// writeback/reload round-trip is needed here.
void FrostJIT::emit_call_native_svc(uint64_t arm_pc) {
    flush_all_vregs();
    if (flags_in_host_) {
        emit_materialize_flags(flags_from_sub_);
        flags_in_host_ = false;
        invalidate_host_regs((1u<<RAX)|(1u<<RCX)|(1u<<RDX));
    }
    if (vreg_home_[31] >= 0 && vreg_dirty_[31]) {
        evict_vreg(31);
    }
    emit_push(WIN_REG);  // save R10 (caller-saved)  — 1 push
    emit_push(RAX);      // save RAX                 — 2 pushes (EVEN → no align fixup)
    // Set cpu.pc = svc_pc (the helper advances it to svc_pc+4).
    emit_mov_imm_to_rax(arm_pc);
    emit_store(CPU_REG, PC_OFF, RAX);
    // Set args: RDI = emu, RSI = cpu, RDX = svc_pc.
    emit_load(RDI, RBP, emu_slot_off());
    emit_mov_reg(RSI, CPU_REG);
    emit_mov_reg(RDX, RAX);
    emit_call_aligned(&jit_native_svc, /*num_pushed=*/2);
    emit_pop(RAX);       // restore RAX
    emit_pop(WIN_REG);   // restore WIN_REG
    // Reload PC into RAX (the syscall may have changed cpu.pc).
    emit_load(RAX, CPU_REG, PC_OFF);
    // The syscall may have touched cpu.regs[]/vregs — reload everything.
    invalidate_all_vregs();
}
} // namespace arm64emu
