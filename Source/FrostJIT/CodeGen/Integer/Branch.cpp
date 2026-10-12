// Branch.cpp — FrostJIT branch/call IR-op codegen.
//
// split out of FrostJIT.cpp. This file holds the
// BRCOND_ZERO / BRCOND_BIT / BRCOND / BRCOND_FALLTHRU / CALL_INTERP /
// SVC case bodies of the IR-op switch, extracted into a separate method
// (compile_ir_branch) for readability. The main switch in FrostJIT.cpp
// dispatches to this method before its residual cases.
//
// No behavior change — pure file split. The method is a member of
// FrostJIT (declared in Source/FrostJIT/Compiler/FrostJIT.hpp) so it has full access
// to the JIT's emit_*, alloc_*, flush_*, etc. helpers.
//
// Return value (int — see FrostJIT.hpp):
//   -1 = op not handled here (caller falls through to next dispatcher)
//    0 = op handled, does NOT end the block
//    1 = op handled AND ends the block
// All branch/call ops in this file return 1 except CALL_INTERP, which
// returns 0 (it doesn't end the block).
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include "System/Core/Emulator.hpp"
#include "FrostJIT/IR/IR.hpp"
#include "FrostJIT/CodeGen/Native/NativeStructure.hpp"
#include "System/Core/DebugFlags.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdlib>  // getenv (BIFROST_NO_SELFLOOP)
namespace arm64emu {
// Keep the current frame and cached loop state, but let MT invalidation
// revoke the back-edge through a data cell. Flags must be committed before
// entering this helper: its lease/sentinel checks overwrite host RFLAGS.
bool FrostJIT::emit_selfloop_backedge(uint64_t target_pc) {
    static const bool no_selfloop = getenv("BIFROST_NO_SELFLOOP") != nullptr;
    if (target_pc != current_start_pc_ || no_selfloop ||
        (mt_active() && dbg().no_chain)) return false;
    has_selfloop_slot_ = true;
    // The back-edge jumps to the body start, whose allocator state holds
    // only the loop-carried pins. Any other dirty architectural register
    // cached in a host register (e.g. x11 written by UMOV) must reach
    // cpu.regs[] first, or the next iteration's LOAD_REG reads the previous
    // iteration's value. Mapping and dirty state stay for the exits below.
    store_dirty_unpinned_arch_regs();
    if (!mt_active()) {
        selfloop_patch_off_ = code_buf_used_;
        emit_byte(0xE9); emit_u32(0);
        return true;
    }
    // R11 may hold a deferred STORE_REG value. Use only RAX, preserving
    // its old mapping before the guard and writing the next PC on cold exit.
    clobber_host_reg(RAX);
    selfloop_patch_off_ = code_buf_used_;
    MtEdge* edge = create_chain_edge(target_pc, false, selfloop_patch_off_);
    edge->internal_loop = true;
    edge->target.store(reinterpret_cast<uint64_t>(code_buf_ + block_body_start_off_),
                       std::memory_order_relaxed);
    mt_edges_published_.fetch_add(1, std::memory_order_relaxed);
    emit_mov_imm_to_rax(target_pc);
    // cmp rax,[rbx+jit_stop_pc]; je cold exit
    emit_byte(0x48); emit_byte(0x3B); emit_byte(0x83);
    emit_u32(offsetof(CPU, jit_stop_pc));
    size_t stop = emit_jcc_rel32_placeholder(0x4);
    // Same 256-edge lease as ordinary MT chains (zero also expires).
    emit_byte(0x83); emit_byte(0xAB);
    emit_u32(offsetof(CPU, jit_chain_budget)); emit_byte(1);
    size_t budget = emit_jcc_rel32_placeholder(0x6);
    emit_mov_imm_to_rax(reinterpret_cast<uint64_t>(&edge->target));
    emit_load(RAX, RAX, 0);
    emit_test_reg(RAX, RAX);
    size_t empty = emit_jcc_rel32_placeholder(0x4);
    emit_byte(0xFF); emit_byte(0xE0); // jmp rax (body entry, same frame)
    for (size_t p : {stop, budget, empty})
        patch_jcc_rel32(p, static_cast<int32_t>(code_buf_used_ - (p + 6)));
    // The caller emits the taken cold exit, committing GPRs and vectors.
    return true;
}
void FrostJIT::emit_call_depth_guard() {
    emit_load(RCX, CPU_REG, offsetof(CPU, jit_call_depth));
    emit_alu_imm(RCX, 7, CPU::JIT_MAX_CALL_DEPTH); // CMP
    size_t enter = emit_jcc_rel32_placeholder(0x2); // JB
    emit_mov_imm64(RAX, 1);
    emit_store8(CPU_REG, offsetof(CPU, jit_dispatch_requested), RAX);
    emit_load(RAX, CPU_REG, PC_OFF);
    // Inputs, LR and target PC are committed before reaching this guard.
    // Do not run a caller continuation or write its old cached state back.
    emit_byte(0x48); emit_byte(0x89); emit_byte(0xEC);
    emit_pop(R15); emit_pop(R14); emit_pop(R13);
    emit_pop(R12); emit_pop(RBP); emit_pop(RBX);
    emit_ret();
    patch_jcc_rel32(enter, static_cast<int32_t>(code_buf_used_ - (enter + 6)));
    emit_add_reg_imm(RCX, 1);
    emit_store(CPU_REG, offsetof(CPU, jit_call_depth), RCX);
}
void FrostJIT::emit_call_depth_leave() {
    emit_load(RCX, CPU_REG, offsetof(CPU, jit_call_depth));
    emit_add_reg_imm(RCX, -1);
    emit_store(CPU_REG, offsetof(CPU, jit_call_depth), RCX);
}
// Called after a native callee, with all caller state already flushed and
// mappings invalidated. Do not write cached vectors back here: the callee's
// CPU state is authoritative. This exit is never a patchable chain slot.
void FrostJIT::emit_call_stop_guard() {
    emit_load(RAX, CPU_REG, PC_OFF);
    // Guest exit (or a stopped sibling) must unwind every native caller.
    // Continuing after exit_group executes libc's deliberate BRK fallback
    // and can overwrite the successful process status with SIGTRAP.
    emit_load8(RCX, CPU_REG, offsetof(CPU, running));
    emit_test_reg(RCX, RCX);
    size_t stopped = emit_jcc_rel32_placeholder(0x4);
    emit_load8(RCX, CPU_REG, offsetof(CPU, jit_dispatch_requested));
    emit_test_reg(RCX, RCX);
    size_t dispatch = emit_jcc_rel32_placeholder(0x5);
    emit_load(RCX, CPU_REG, offsetof(CPU, jit_stop_pc));
    emit_cmp_reg(RAX, RCX);
    size_t resume = emit_jcc_rel32_placeholder(0x5);
    patch_jcc_rel32(stopped, static_cast<int32_t>(code_buf_used_ - (stopped + 6)));
    patch_jcc_rel32(dispatch, static_cast<int32_t>(code_buf_used_ - (dispatch + 6)));
    emit_byte(0x48); emit_byte(0x89); emit_byte(0xEC); // mov rsp,rbp
    emit_pop(R15); emit_pop(R14); emit_pop(R13);
    emit_pop(R12); emit_pop(RBP); emit_pop(RBX);
    emit_ret();
    patch_jcc_rel32(resume, static_cast<int32_t>(code_buf_used_ - (resume + 6)));
}
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
    // clear_flags=false: this is the TAKEN-path exit, but the FALL-THROUGH
    // (main) epilogue is emitted LATER in codegen and must ALSO get the
    // writeback for its own exit path. vec_cache_writeback_all() clears the
    // dirty flags as a side effect, so emitting here with the default would
    // silently drop the writeback from the fall-through epilogue (the vec
    // cache's dirty state is a codegen-time concept, not a runtime one).
    vec_cache_writeback_all(false);
    // Flush dirty GPR vregs: the taken exit returns to the dispatcher
    // (or a chained successor that reloads from home slots), so every
    // value computed before the branch must be written home. Without
    // this, a dirty vreg in a non-RAX reg (e.g. umov results feeding a
    // CBZ) is silently dropped on the taken path while the fall-through
    // epilogue still writes it — zlib's inflate loop read stale x22/x19
    // and died in a null call. Cold path: loop exits and returns only.
    // KEEP (not evict): the fall-through (main) epilogue is emitted LATER
    // and needs the same dirty state for its own writeback — a consuming
    // flush here would move the stale-reg bug to the fall-through path.
    flush_all_vregs_keep();
    emit_store(CPU_REG, PC_OFF, RAX);
    emit_tier2_edge_profile(true);
    emit_mov_reg(RDI, CPU_REG);   // mov rdi, rbx (for dispatcher OR chain target)
    emit_load(RSI, RBP, emu_slot_off()); // rsi = emu
    // ── Taken-path chain slot ──
    // 5 bytes reserved at the end of the taken path. Under chain-skip the
    // slot is 5 NOPs followed by the cold exit (`mov rsp,rbp; pop×6; ret`),
    // matching the shared epilogue's lease layout so patch_chain() can
    // patch either slot to the successor's chain_entry. Otherwise it's
    // `ret` + 4 NOPs (the original layout). Until patched, the taken path
    // returns to the dispatcher as before (falling through the NOPs into
    // the cold exit under chain-skip).
    //
    // CRITICAL: taken_chain_patch_off_ must be recorded at the START of
    // the actual 5-byte slot in BOTH layouts (patch_chain() guards on the
    // slot's first byte being 0x90 / 0xC3). Under chain-skip the slot is
    // the 5 NOPs emitted first; under the default layout the slot is the
    // `ret` emitted AFTER the callee-saved restore. Recording at the top of
    // the function (before the restore) left the default-mode offset 13
    // bytes early (pointing at `mov rsp,rbp`), so every default-mode
    // taken-path chain failed the 0xC3 guard and silently never patched
    // (regression introduced by the chain-skip restructure). A direct BL
    // call relies on the callee's taken-path chain, so that silent failure
    // surfaced as the callee returning after its entry block.
    if (chain_skip_enabled()) {
        taken_chain_patch_off_ = code_buf_used_;
        has_taken_chain_slot_ = true;
        if (mt_active()) emit_mt_chain_slot(taken_chain_target_pc_);
        else { emit_nop(); emit_nop(); emit_nop(); emit_nop(); emit_nop(); }
        // Cold exit (only reached when the slot above is unpatched).
        emit_byte(0x48); emit_byte(0x89); emit_byte(0xEC); // mov rsp, rbp
        emit_pop(R15); emit_pop(R14); emit_pop(R13);
        emit_pop(R12); emit_pop(RBP); emit_pop(RBX);
        emit_ret();
    } else {
        // Default layout (original): restore the callee-saved regs FIRST,
        // then the slot is a bare `ret` + 4 NOPs. When patched, the jmp
        // skips the restore (the target's prologue re-pushes); when
        // unpatched, the ret returns to the dispatcher with a balanced
        // stack. The restore MUST precede the slot — a `ret` before it
        // would pop the dispatcher's return address off the block's own
        // frame and jump to garbage.
        emit_byte(0x48); emit_byte(0x89); emit_byte(0xEC); // mov rsp, rbp
        emit_pop(R15); emit_pop(R14); emit_pop(R13);
        emit_pop(R12); emit_pop(RBP); emit_pop(RBX);
        taken_chain_patch_off_ = code_buf_used_;
        has_taken_chain_slot_ = true;
        if (mt_active()) emit_mt_chain_slot(taken_chain_target_pc_);
        emit_ret();                                  // 0xC3
        emit_nop(); emit_nop(); emit_nop(); emit_nop();  // 4 × 0x90
    }
}
// ── FrostJIT::compile_ir_branch ────────────────────────────────────────
// Handles conditional/unconditional branch ops and supervisor calls.
// All of these set rax_holds_next_pc_=true and return 1 (ends block)
// except CALL_INTERP, which returns 0.
int FrostJIT::compile_ir_branch(const IRInst& inst) {
    switch (inst.op) {
        case IROp::BRCOND_ZERO: {
            const BrCondZeroParams bp = inst.brcond_zero_params();
            // NZCV is authoritative in pstate after clobber_flags(). TEST
            // may overwrite host flags; neither edge needs PUSHFQ/POPFQ.
            // Test the allocator's existing home without moving the operand
            // into RAX or destructively clearing its high half for W-form.
            clobber_flags();
            int s1 = ensure_vreg(inst.src1);
            emit_byte(rex(bp.sf, s1 >= 8, false, s1 >= 8));
            emit_byte(0x85); emit_byte(modrm(3, s1 & 7, s1 & 7));
            // RAX will hold the next PC. Spill its occupant before the MOV;
            // spills are flag-neutral, so they preserve TEST's branch result.
            clobber_host_reg(RAX);
            // jcc to taken target
            uint8_t cc = (bp.cond == 0) ? 4 /*JE*/ : 5 /*JNE*/;
            size_t jcc_patch = emit_jcc_rel32_placeholder(cc);
            // Not taken: RAX = fall-through.
            uint64_t fall = inst.arm_pc + 4;
            emit_mov_imm_to_rax(fall);
            size_t jmp_to_epilogue = emit_jmp_rel32_placeholder();
            branch_target_patches_.push_back({jmp_to_epilogue, 0});
            // Taken: patch jcc to here.
            int32_t taken_rel = static_cast<int32_t>(code_buf_used_ - (jcc_patch + 6));
            patch_jcc_rel32(jcc_patch, taken_rel);
            // ── Self-loop chaining (mirrors BRCOND) ──
            // A CBZ/CBNZ while-loop (bench_mips's counter loop, GCC
            // vectorized memchr/strchr) gets the tight 5-byte jmp slot
            // patched to the block body start, skipping the dispatcher and
            // prologue every iteration. This is ALSO required for loop-
            // carried pinning: deferred pins only persist when the back-edge
            // jumps to the body — a taken path returning to the dispatcher
            // re-runs the prologue, whose preloads read STALE cpu.regs[]
            // for the deferred pins (infinite loop).
            emit_selfloop_backedge(bp.target);
            emit_mov_imm_to_rax(bp.target);
            rax_holds_next_pc_ = true;
            chain_target_pc_ = inst.arm_pc + 4;  // fall-through PC
            if (!has_selfloop_slot_) {
                // Taken path: store PC, restore regs, ret-with-chain-slot.
                // The ret is a chain slot patched to `jmp taken_target` once
                // the taken target is translated (loop-back edges skip the
                // dispatcher).
                taken_chain_target_pc_ = bp.target;
                emit_taken_path_epilogue();
            } else {
                // Verification or an expired/revoked MT edge reaches this exit.
                // Commit the iteration without consuming fall-through dirtiness.
                flush_all_vregs_keep();
                vec_cache_writeback_all(false);
                emit_store(CPU_REG, PC_OFF, RAX);
                emit_mov_reg(RDI, CPU_REG);
                emit_load(RSI, RBP, emu_slot_off());
                emit_byte(0x48); emit_byte(0x89); emit_byte(0xEC); // mov rsp, rbp
                emit_pop(R15); emit_pop(R14); emit_pop(R13);
                emit_pop(R12); emit_pop(RBP); emit_pop(RBX);
                emit_ret();
            }
            return 1;
        }
        case IROp::BRCOND_BIT: {
            const BrCondBitParams bp = inst.brcond_bit_params();
            // As with CBZ, guest NZCV has been committed before BT.
            // Use the operand in place and let BT overwrite dead host CF.
            clobber_flags();
            int s1 = ensure_vreg(inst.src1);
            emit_byte(rex(true, false, false, s1 >= 8));
            emit_byte(0x0F); emit_byte(0xBA);
            emit_byte(modrm(3, 4, s1 & 7)); // BT /4 (not BTS /5)
            emit_byte(bp.bit);
            clobber_host_reg(RAX);
            // jcc: TBZ (cond=0, EQ) → JNC (bit==0, CF=0) → JAE (cc=3)
            //      TBNZ (cond=1, NE) → JC (bit==1, CF=1) → JB (cc=2)
            uint8_t cc = (bp.cond == 0) ? 3 /*JNC/JAE*/ : 2 /*JC/JB*/;
            size_t jcc_patch = emit_jcc_rel32_placeholder(cc);
            // Not taken: RAX = fall-through.
            uint64_t fall = inst.arm_pc + 4;
            emit_mov_imm_to_rax(fall);
            size_t jmp_to_epilogue = emit_jmp_rel32_placeholder();
            branch_target_patches_.push_back({jmp_to_epilogue, 0});
            // Taken: patch jcc to here.
            int32_t taken_rel = static_cast<int32_t>(code_buf_used_ - (jcc_patch + 6));
            patch_jcc_rel32(jcc_patch, taken_rel);
            // ── Self-loop chaining (mirrors BRCOND/BRCOND_ZERO) ──
            // TBZ/TBNZ while-loops get the tight 5-byte jmp slot patched to
            // the block body start. Also required for loop-carried pinning:
            // deferred pins only persist across a back-edge that jumps to the
            // body, not one that re-enters the prologue (stale preloads).
            emit_selfloop_backedge(bp.target);
            emit_mov_imm_to_rax(bp.target);
            rax_holds_next_pc_ = true;
            chain_target_pc_ = inst.arm_pc + 4;  // fall-through PC
            if (!has_selfloop_slot_) {
                // Taken path: store PC, restore regs, ret-with-chain-slot.
                taken_chain_target_pc_ = bp.target;
                emit_taken_path_epilogue();
            } else {
                // Verification or an expired/revoked MT edge reaches this exit.
                // Commit the iteration without consuming fall-through dirtiness.
                flush_all_vregs_keep();
                vec_cache_writeback_all(false);
                emit_store(CPU_REG, PC_OFF, RAX);
                emit_mov_reg(RDI, CPU_REG);
                emit_load(RSI, RBP, emu_slot_off());
                emit_byte(0x48); emit_byte(0x89); emit_byte(0xEC); // mov rsp, rbp
                emit_pop(R15); emit_pop(R14); emit_pop(R13);
                emit_pop(R12); emit_pop(RBP); emit_pop(RBX);
                emit_ret();
            }
            return 1;
        }
        case IROp::BRCOND: {
            const BrCondParams bp = inst.brcond_params();
            if (!flags_in_host_) {
                // emit_load_flags_from_pstate clobbers only RAX/RCX/RDX
                // (the C^from_sub extraction is RDX-only since the R8-scratch
                // fix; see the contract comment in X86Emitter.cpp). Use
                // targeted flush+invalidate to preserve vregs cached in
                // R8/R9/R11/R12/R13/R15 across the flag load. This is a big
                // win in branch-heavy code (loops with many live variables).
                // No pushfq/popfq: the goal is to LOAD flags, and
                // popfq would restore the pre-load (garbage) flags.
                constexpr uint16_t FLAGS3 = (1u << RAX) | (1u << RCX) | (1u << RDX);
                flush_dirty_host_regs(FLAGS3);
                flush_scratch_host_regs(FLAGS3);
                emit_load_flags_from_pstate();
                // Normalize CF to SUB convention so the default
                // arm_cond_to_x86() mapping works for all conditions.
                invalidate_host_regs(FLAGS3);
                flags_from_sub_ = true;  // CF is now in SUB convention
                flags_in_host_ = true;
            }
            // Resolve condition code, handling carry polarity (ADD/TST vs SUB).
            // With flags_from_sub_=true (SUB convention, whether originally
            // from SUB or normalized), the default mapping is used.
            bool need_cmc_for_hi_ls = false;
            uint8_t cc = resolve_arm_cond_with_carry(bp.cond, need_cmc_for_hi_ls);
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
            //
            // A successor may forward NZCV to later blocks, return to a
            // caller, or be invalidated. Preserve architectural flags on
            // every cross-block edge; a local consumer scan is not liveness.
            if (need_cmc_for_hi_ls) emit_byte(0xF5);
            // JCC has consumed the host flags. Only architectural NZCV must
            // survive the exit; restoring RFLAGS here is dead work.
            materialize_flags_to_pstate(false);
            emit_mov_imm_to_rax(inst.arm_pc + 4);
            size_t jmp_to_epilogue = emit_jmp_rel32_placeholder();
            branch_target_patches_.push_back({jmp_to_epilogue, 0});
            // ── Taken path ──
            int32_t taken_rel = static_cast<int32_t>(code_buf_used_ - (jcc_patch + 6));
            patch_jcc_rel32(jcc_patch, taken_rel);
            static bool no_selfloop_ = (getenv("BIFROST_NO_SELFLOOP") != nullptr);
            bool is_selfloop = (bp.target == current_start_pc_) &&
                (!mt_active() || !dbg().no_chain);
            // On a self-loop back-edge, skip the pstate materialization
            // entirely when the block never reads loop-carried flags
            // (flags_loop_carried_, precomputed in translate_block): the
            // next iteration's first flag-setting op re-establishes host
            // flags before any consumer, so the pstate store is dead work
            // every iteration (~60 bytes of pushfq/bit-extract/store).
            bool skip_taken_materialize =
                is_selfloop && !no_selfloop_ && !flags_loop_carried_ && !mt_active();
            // If CMC was emitted, re-invert CF before materializing flags.
            // Skipped along with the materialize: the loop body re-sets CF
            // before any consumer reads it.
            if (skip_taken_materialize) {
                // This internal self-loop never
                // reads pstate — omit materialize AND cmc restore.
            } else {
                if (need_cmc_for_hi_ls) {
                    emit_byte(0xF5);  // cmc — restore CF to original
                }
                // Materialize flags to pstate (the taken-target block may read them).
                materialize_flags_to_pstate(false);

            }
            // ── Self-loop chaining ──
            // If the taken target is the block's own start PC, emit a 5-byte
            // `jmp rel32` placeholder. After the block is fully compiled,
            // translate_block patches this slot to jump directly to the block
            // body start, creating a tight loop that skips the epilogue,
            // dispatcher, and prologue. This is the single biggest win for
            // tight loops (e.g. bench_mips: 7.4s → 1.4s).
            //
            // Disable with BIFROST_NO_SELFLOOP=1 for debugging.
            emit_selfloop_backedge(bp.target);
            emit_mov_imm_to_rax(bp.target);
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
            // emit the taken-path epilogue with a SECOND chain
            // slot so the taken edge (typically the loop-back of a hot
            // conditional loop) also skips the dispatcher. Skipped when a
            // self-loop slot was emitted — that jmp already goes straight
            // to the block body, and the epilogue after it is dead code.
            if (!has_selfloop_slot_) {
                taken_chain_target_pc_ = bp.target;
                emit_taken_path_epilogue();
            } else {
                // Verification or an expired/revoked MT edge reaches this exit.
                // Commit the iteration without consuming fall-through dirtiness.
                flush_all_vregs_keep();
                vec_cache_writeback_all(false);
                emit_store(CPU_REG, PC_OFF, RAX);
                emit_mov_reg(RDI, CPU_REG);
                emit_load(RSI, RBP, emu_slot_off());
                emit_byte(0x48); emit_byte(0x89); emit_byte(0xEC); // mov rsp, rbp
                emit_pop(R15); emit_pop(R14); emit_pop(R13);
                emit_pop(R12); emit_pop(RBP); emit_pop(RBX);
                emit_ret();
            }
            return 1;
        }
        case IROp::BRCOND_FALLTHRU: {
            const BrCondFallthruParams bp = inst.fallthru_params();
            flush_all_vregs();
            emit_mov_imm_to_rax(bp.target);
            rax_holds_next_pc_ = true;
            // Unconditional branch with statically-known target — record
            // it for block chaining. try_chain_block() will patch the
            // epilogue's chain slot to jmp directly to the target block
            // once it has been translated.
            chain_target_pc_ = bp.target;
            return 1;
        }
        case IROp::CALL_INTERP:
            emit_call_interp(inst.arm_pc, false);
            return 0;
        case IROp::BRCOND_SKIP: {
            const BrCondSkipParams bp = inst.brcond_skip_params();
            // mid-block conditional skip (leaf inlining). If
            // cond(imm) holds, jump over the next `imm` IR ops (the skip
            // region). Unlike BRCOND this does NOT end the block and does
            // NOT touch PC or materialize flags — both paths stay inline.
            // The flags come from the preceding flag-setting op (the branch
            // in the leaf follows a tst/cmp), which set host RFLAGS.
            if (!flags_in_host_) {
                constexpr uint16_t FLAGS3 = (1u << RAX) | (1u << RCX) | (1u << RDX);
                flush_dirty_host_regs(FLAGS3);
                flush_scratch_host_regs(FLAGS3);
                emit_load_flags_from_pstate();
                invalidate_host_regs(FLAGS3);
                flags_from_sub_ = true;  // CF is now in SUB convention
                flags_in_host_ = true;
            }
            bool need_cmc_for_hi_ls = false;
            uint8_t cc = resolve_arm_cond_with_carry(bp.cond, need_cmc_for_hi_ls);
            // The leaf scan only accepts non-carry conditions (EQ/NE/MI/PL/
            // VS/VC/GE/LT/GT/LE), so the cmc dance never fires here — kept
            // for symmetry with BRCOND and to be safe if that list grows.
            if (need_cmc_for_hi_ls) emit_byte(0xF5);  // cmc — invert CF
            size_t jcc_patch = emit_jcc_rel32_placeholder(cc);
            if (need_cmc_for_hi_ls) emit_byte(0xF5);  // cmc — restore CF
            // Record the fixup: translate_block's compile loop patches the
            // rel32 when it reaches the op AFTER the region (index = this
            // op + 1 + region size). Flags stay in host; the region's own
            // ops clobber them as the guest requires on the not-taken path.
            skip_fixups_.push_back(
                {jcc_patch, static_cast<int>(cur_op_index_ + 1 + bp.count)});
            return 0;
        }
        case IROp::SIMD_NATIVE_STRUCT: {
            clobber_flags(); flush_all_vregs();
            emit_mov_imm64(RAX, inst.arm_pc); emit_store(CPU_REG, PC_OFF, RAX);
            emit_load(RDI, RBP, emu_slot_off()); emit_mov_reg(RSI, CPU_REG);
            emit_mov_imm64(RDX, inst.native_structure_params().pack());
            emit_push(WIN_REG);
            emit_call_aligned(&native_structure, 1);
            emit_pop(WIN_REG);
            invalidate_all_vregs();
            rax_holds_next_pc_=true; unchainable_end_=true;
            return 1;
        }
        case IROp::SVC:
            // Native syscall dispatch. vDSO clock stubs keep their fast
            // path (jit_vdso_clock_svc reads the host clock directly,
            // skipping syscall dispatch). ALL other SVCs go through
            // jit_native_svc, which advances pc to svc_pc+4 and calls
            // Emulator::syscall() directly — no interpreter round-trip.
            // The interpreter's SVC_IMM pc-advance semantics are mirrored
            // so signal frames and the execve/rt_sigreturn pc-change check
            // stay consistent.
            if (in_vdso(inst.arm_pc)) {
                emit_call_vdso_clock(inst.arm_pc);
            } else {
                emit_call_native_svc(inst.arm_pc);
            }
            rax_holds_next_pc_ = true;
            unchainable_end_ = true;  // syscall may modify PC
            return 1;
        default:
            return -1;  // not handled — caller falls through
    }
}
} // namespace arm64emu
