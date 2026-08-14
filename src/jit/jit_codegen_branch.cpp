// jit/jit_codegen_branch.cpp — FrostJIT branch/call IR-op codegen.
//
// v1.4.5-alpha: split out of frostjit.cpp. This file holds the
// BRCOND_ZERO / BRCOND_BIT / BRCOND / BRCOND_FALLTHRU / CALL_INTERP /
// SVC case bodies of the IR-op switch, extracted into a separate method
// (compile_ir_branch) for readability. The main switch in frostjit.cpp
// dispatches to this method before its residual cases.
//
// No behavior change — pure file split. The method is a member of
// FrostJIT (declared in include/jit/frostjit.hpp) so it has full access
// to the JIT's emit_*, alloc_*, flush_*, etc. helpers.
//
// Return value (int — see frostjit.hpp):
//   -1 = op not handled here (caller falls through to next dispatcher)
//    0 = op handled, does NOT end the block
//    1 = op handled AND ends the block
// All branch/call ops in this file return 1 except CALL_INTERP, which
// returns 0 (it doesn't end the block).
#include "jit/frostjit.hpp"
#include "core/emulator.h"
#include "ir/ir.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdlib>  // getenv (BIFROST_NO_SELFLOOP)
namespace arm64emu {
// ── FrostJIT::emit_taken_path_epilogue ─────────────────────────────────
// Emits the conditional-branch TAKEN-path tail. RAX must hold the taken
// next-PC. Writes back dirty cached vectors, stores the PC, sets RDI/RSI
// for the next block's prologue, restores callee-saved regs, then emits
// `ret` + 4 NOPs as a SECOND chain slot. try_chain_block() patches the
// ret to `jmp rel32 → target block entry` once the taken target is
// translated, so the loop-back edge of a hot conditional loop skips the
// C++ dispatcher entirely (the dispatcher only chained the fall-through
// edge before this).
void FrostJIT::emit_taken_path_epilogue() {
    // Write back dirty cached vectors first (see BRCOND_ZERO above).
    vec_cache_writeback_all();
    emit_store(CPU_REG, PC_OFF, RAX);
    emit_mov_reg(RDI, CPU_REG);   // mov rdi, rbx (for dispatcher OR chain target)
    emit_mov_reg(RSI, EMU_REG);   // mov rsi, r14
    emit_byte(0x48); emit_byte(0x89); emit_byte(0xEC); // mov rsp, rbp
    emit_pop(R15); emit_pop(R14); emit_pop(R13);
    emit_pop(R12); emit_pop(RBP); emit_pop(RBX);
    // ── Taken-path chain slot ──
    // 5 bytes reserved at the end of the taken path: `ret` + 4 NOPs.
    // Identical layout to the shared epilogue's chain slot so
    // patch_chain() (which guards on the slot starting with 0xC3) can
    // patch either one. Until patched, the taken path returns to the
    // dispatcher as before.
    taken_chain_patch_off_ = code_buf_used_;
    has_taken_chain_slot_ = true;
    emit_ret();                                  // 0xC3
    emit_nop(); emit_nop(); emit_nop(); emit_nop();  // 4 × 0x90
}
// ── FrostJIT::compile_ir_branch ────────────────────────────────────────
// Handles conditional/unconditional branch ops and supervisor calls.
// All of these set rax_holds_next_pc_=true and return 1 (ends block)
// except CALL_INTERP, which returns 0.
int FrostJIT::compile_ir_branch(const IRInst& inst) {
    switch (inst.op) {
        case IROp::BRCOND_ZERO: {
            // CBZ/CBNZ: branch on (val == 0) without touching flags.
            // cond=0 (EQ) → branch if val == 0
            // cond=1 (NE) → branch if val != 0
            // We emit: test val, val; jcc (JE for EQ, JNE for NE)
            // The test sets ZF but we don't materialize flags (CBZ/CBNZ
            // don't modify architectural flags). We save/restore RFLAGS
            // around the test to avoid clobbering pending flags.
            //
            // the previous code did
            //   int s1 = ensure_vreg(inst.src1, RAX);
            //   if (s1 != RAX) emit_mov_reg(RAX, s1);
            // which would overwrite RAX without evicting whatever dirty
            // vreg was cached there — typically the value computed by the
            // immediately preceding SBFM/UBFM/ADDS that wrote to an
            // architectural reg. The epilogue's flush_all_vregs() would
            // then write the next-PC value (left in RAX by this branch)
            // to that architectural reg, corrupting it.
            //
            // Fix: evict any dirty vreg in RAX BEFORE loading the test
            // value, and drop RAX's cache mapping so flush_all_vregs
            // can't miswrite it.
            clobber_flags();  // materialize any pending flags first
            // use drop_vreg — evicts if dirty, drops if not.
            if (reg_vreg_[RAX] >= 0) {
                drop_vreg(reg_vreg_[RAX]);
            }
            int s1 = ensure_vreg(inst.src1, RAX);
            if (s1 != RAX) emit_mov_reg(RAX, s1);
            // RAX now holds the test value. Drop RAX's cache mapping so
            // the upcoming `mov eax, <pc>` doesn't corrupt any vreg.
            // use drop_vreg — if RAX holds a dirty vreg,
            // evict it to memory first so the value is preserved.
            if (reg_vreg_[RAX] >= 0) {
                drop_vreg(reg_vreg_[RAX]);
            }
            // Save RFLAGS (CBZ/CBNZ don't modify architectural flags).
            emit_pushfq();
            // test rax, rax
            emit_test_reg(RAX, RAX);
            // jcc to taken target
            uint8_t cc = (inst.cond == 0) ? 4 /*JE*/ : 5 /*JNE*/;
            size_t jcc_patch = emit_jcc_rel32_placeholder(cc);
            // Not taken: RAX = fall-through.
            uint64_t fall = inst.arm_pc + 4;
            emit_mov_imm_to_rax(fall);
            // Restore RFLAGS before jumping to epilogue
            emit_popfq();
            size_t jmp_to_epilogue = emit_jmp_rel32_placeholder();
            branch_target_patches_.push_back({jmp_to_epilogue, 0});
            // Taken: patch jcc to here.
            int32_t taken_rel = static_cast<int32_t>(code_buf_used_ - (jcc_patch + 6));
            patch_jcc_rel32(jcc_patch, taken_rel);
            // Restore RFLAGS (CBZ/CBNZ don't modify flags)
            emit_popfq();
            emit_mov_imm_to_rax(inst.imm);
            rax_holds_next_pc_ = true;
            chain_target_pc_ = inst.arm_pc + 4;  // fall-through PC
            // Taken path: store PC, restore regs, ret-with-chain-slot.
            // The ret is a chain slot patched to `jmp taken_target` once the
            // taken target is translated (loop-back edges skip the dispatcher).
            taken_chain_target_pc_ = inst.imm;
            emit_taken_path_epilogue();
            return 1;
        }
        case IROp::BRCOND_BIT: {
            // TBZ/TBNZ: branch on ((val >> bit) & 1) without touching flags.
            // cond=0 (EQ) → branch if bit == 0 (TBZ)
            // cond=1 (NE) → branch if bit == 1 (TBNZ)
            // We emit: bt rax, bit; jcc (JNC for bit==0, JC for bit==1)
            // BT sets CF = (val >> bit) & 1. We save/restore RFLAGS.
            // same RAX eviction as BRCOND_ZERO —
            // see the comment there for the rationale.
            clobber_flags();
            // use drop_vreg — evicts if dirty, drops if not.
            if (reg_vreg_[RAX] >= 0) {
                drop_vreg(reg_vreg_[RAX]);
            }
            int s1 = ensure_vreg(inst.src1, RAX);
            if (s1 != RAX) emit_mov_reg(RAX, s1);
            // use drop_vreg — if RAX holds a dirty vreg,
            // evict it to memory first so the value is preserved.
            if (reg_vreg_[RAX] >= 0) {
                drop_vreg(reg_vreg_[RAX]);
            }
            emit_pushfq();
            // bt rax, imm8  — 0x48 0x0F 0xBA /5 r/m, imm8
            emit_byte(rex(true, false, false, RAX >= 8));
            emit_byte(0x0F); emit_byte(0xBA);
            emit_byte(modrm(3, 5, RAX & 7));
            emit_byte(inst.width);  // bit number
            // jcc: TBZ (cond=0, EQ) → JNC (bit==0, CF=0) → JAE (cc=3)
            //      TBNZ (cond=1, NE) → JC (bit==1, CF=1) → JB (cc=2)
            uint8_t cc = (inst.cond == 0) ? 3 /*JNC/JAE*/ : 2 /*JC/JB*/;
            size_t jcc_patch = emit_jcc_rel32_placeholder(cc);
            // Not taken: RAX = fall-through.
            uint64_t fall = inst.arm_pc + 4;
            emit_mov_imm_to_rax(fall);
            emit_popfq();
            size_t jmp_to_epilogue = emit_jmp_rel32_placeholder();
            branch_target_patches_.push_back({jmp_to_epilogue, 0});
            // Taken: patch jcc to here.
            int32_t taken_rel = static_cast<int32_t>(code_buf_used_ - (jcc_patch + 6));
            patch_jcc_rel32(jcc_patch, taken_rel);
            emit_popfq();
            emit_mov_imm_to_rax(inst.imm);
            rax_holds_next_pc_ = true;
            chain_target_pc_ = inst.arm_pc + 4;  // fall-through PC
            // Taken path: store PC, restore regs, ret-with-chain-slot.
            taken_chain_target_pc_ = inst.imm;
            emit_taken_path_epilogue();
            return 1;
        }
        case IROp::BRCOND: {
            if (!flags_in_host_) {
                // emit_normalize_cf_to_sub_convention only clobber
                // RAX/RCX/RDX. Use targeted flush+invalidate to preserve
                // vregs cached in R8/R9/R11/R12/R13/R15 across the flag
                // load. This is a big win in branch-heavy code (loops with
                // many live variables).
                // No pushfq/popfq: the goal is to LOAD flags, and
                // popfq would restore the pre-load (garbage) flags.
                constexpr uint16_t FLAGS3 = (1u << RAX) | (1u << RCX) | (1u << RDX);
                flush_dirty_host_regs(FLAGS3);
                flush_scratch_host_regs(FLAGS3);
                emit_load_flags_from_pstate();
                // Normalize CF to SUB convention so the default
                // arm_cond_to_x86() mapping works for all conditions.
                emit_normalize_cf_to_sub_convention();
                invalidate_host_regs(FLAGS3);
                flags_from_sub_ = true;  // CF is now in SUB convention
                flags_in_host_ = true;
            }
            // Resolve condition code, handling carry polarity (ADD/TST vs SUB).
            // With flags_from_sub_=true (SUB convention, whether originally
            // from SUB or normalized), the default mapping is used.
            bool need_cmc_for_hi_ls = false;
            uint8_t cc = resolve_arm_cond_with_carry(inst.cond, need_cmc_for_hi_ls);
            // Emit the JCC first — it consumes host RFLAGS directly, so no
            // flag materialization is needed before it. Flags are materialized
            // to pstate on each path separately (the next block may read pstate).
            // Stores don't clobber RFLAGS, so flush_all_vregs needs no
            // pushfq/popfq wrapper here.
            flush_all_vregs();
            if (need_cmc_for_hi_ls) {
                emit_byte(0xF5);  // cmc — invert CF for HI/LS after ADD/TST
            }
            size_t jcc_patch = emit_jcc_rel32_placeholder(cc);
            // ── Fall-through (not-taken) path: materialize flags, set RAX = fall-through PC ──
            // If CMC was emitted (for HI/LS after ADD/TST), re-invert CF so
            // materialize_flags_to_pstate sees the original carry flag.
            if (need_cmc_for_hi_ls) {
                emit_byte(0xF5);  // cmc — restore CF to original
            }
            materialize_flags_to_pstate();
            emit_mov_imm_to_rax(inst.arm_pc + 4);
            size_t jmp_to_epilogue = emit_jmp_rel32_placeholder();
            branch_target_patches_.push_back({jmp_to_epilogue, 0});
            // ── Taken path ──
            int32_t taken_rel = static_cast<int32_t>(code_buf_used_ - (jcc_patch + 6));
            patch_jcc_rel32(jcc_patch, taken_rel);
            // If CMC was emitted, re-invert CF before materializing flags.
            if (need_cmc_for_hi_ls) {
                emit_byte(0xF5);  // cmc — restore CF to original
            }
            // Materialize flags to pstate (the taken-target block may read them).
            materialize_flags_to_pstate();
            // ── Self-loop chaining ──
            // If the taken target is the block's own start PC, emit a 5-byte
            // `jmp rel32` placeholder. After the block is fully compiled,
            // translate_block patches this slot to jump directly to the block
            // body start, creating a tight loop that skips the epilogue,
            // dispatcher, and prologue. This is the single biggest win for
            // tight loops (e.g. bench_mips: 7.4s → 1.4s).
            //
            // Disable with BIFROST_NO_SELFLOOP=1 for debugging.
            static bool no_selfloop_ = (getenv("BIFROST_NO_SELFLOOP") != nullptr);
            bool is_selfloop = (inst.imm == current_start_pc_);
            if (is_selfloop && !no_selfloop_) {
                has_selfloop_slot_ = true;
                selfloop_patch_off_ = code_buf_used_;
                emit_byte(0xE9); emit_u32(0);  // jmp rel32 placeholder
            }
            emit_mov_imm_to_rax(inst.imm);
            rax_holds_next_pc_ = true;
            flags_in_host_ = false;
            //
            // OLD: unchainable_end_ = true (both paths return to dispatcher)
            // NEW: The fall-through (not-taken) path chains to the block at
            //      pc+4 via the shared epilogue's chain slot. The taken path
            //      emits its own "store PC + restore regs + ret" to return
            //      to the dispatcher independently.
            //
            // This is the biggest perf win for call-heavy code (fib, qsort).
            // Before: every conditional branch went through the C dispatcher
            // on BOTH paths. Now: the common (not-taken) path chains directly
            // to the next block, skipping the dispatcher. Only the taken path
            // (typically the less-common branch, e.g., loop exit, function
            // return) goes through the dispatcher.
            //
            // The taken path emits a complete epilogue (store PC, restore
            // callee-saved regs, ret) BEFORE the shared epilogue. This
            // duplicates ~10 instructions of epilogue code per conditional
            // branch block, but the code size increase is negligible (<1%
            // of the 64MB code buffer for typical programs).
            chain_target_pc_ = inst.arm_pc + 4;  // fall-through PC
            // 1.5.2-alpha: emit the taken-path epilogue with a SECOND chain
            // slot so the taken edge (typically the loop-back of a hot
            // conditional loop) also skips the dispatcher. Skipped when a
            // self-loop slot was emitted — that jmp already goes straight
            // to the block body, and the epilogue after it is dead code.
            if (!has_selfloop_slot_) {
                taken_chain_target_pc_ = inst.imm;
                emit_taken_path_epilogue();
            } else {
                // Self-loop: keep the original dead epilogue (store PC,
                // restore regs, ret) — harmless, never executed.
                emit_store(CPU_REG, PC_OFF, RAX);
                emit_mov_reg(RDI, CPU_REG);
                emit_mov_reg(RSI, EMU_REG);
                emit_byte(0x48); emit_byte(0x89); emit_byte(0xEC); // mov rsp, rbp
                emit_pop(R15); emit_pop(R14); emit_pop(R13);
                emit_pop(R12); emit_pop(RBP); emit_pop(RBX);
                emit_ret();
            }
            return 1;
        }
        case IROp::BRCOND_FALLTHRU: {
            flush_all_vregs();
            emit_mov_imm_to_rax(inst.imm);
            rax_holds_next_pc_ = true;
            // Unconditional branch with statically-known target — record
            // it for block chaining. try_chain_block() will patch the
            // epilogue's chain slot to jmp directly to the target block
            // once it has been translated.
            chain_target_pc_ = inst.imm;
            return 1;
        }
        case IROp::CALL_INTERP:
            emit_call_interp(inst.arm_pc, false);
            return 0;
        case IROp::SVC:
            // 1.5.2-alpha: vDSO clock fast path — SVCs translated from the
            // vDSO clock stubs emit a native call (jit_vdso_clock_svc) that
            // reads the host clock directly, skipping the interpreter +
            // syscall dispatch. Other SVCs go through the normal interp step.
            if (in_vdso(inst.arm_pc)) {
                emit_call_vdso_clock(inst.arm_pc);
            } else {
                emit_call_interp(inst.arm_pc, true);
            }
            rax_holds_next_pc_ = true;
            unchainable_end_ = true;  // syscall may modify PC
            return 1;
        default:
            return -1;  // not handled — caller falls through
    }
}
} // namespace arm64emu
