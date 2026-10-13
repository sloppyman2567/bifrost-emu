// Tier-2 region compiler: one x86 function with region-wide register allocation.
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include "System/Core/Emulator.hpp"
#include "System/Core/DebugFlags.hpp"
#include "FrostJIT/IR/IR.hpp"
#include "FrostJIT/IR/SSA/SSA.hpp"
#include "FrostJIT/IR/Builder.hpp"
#include <chrono>
#include <time.h>
#include <algorithm>  // std::find
#include <cstdint>
#include <cstdio>
#include <cstring>  // memcpy
#include <vector>
#include "FrostJIT/Tier2/Tier2Internal.hpp"
namespace arm64emu {
namespace {
// Codegen-state snapshot captured at each terminating-branch JCC so the
// deferred side-exit / loop-back-edge epilogues can be emitted LATER (after
// the whole region body) with the exact regalloc/flags state of that branch
// point. The region body is one flat code stream with NO branch-patch jumps
// between its blocks (internal fall-through edges keep the codegen state
// live), so the only state a deferred epilogue needs is the state at its own
// branch — restored here, then materialize/flush/store emit as-of that point.
struct RegionSnapshot {
    bool   flags_in_host_ = false;
    bool   flags_from_sub_ = false;
    bool   need_cmc_ = false;   // the branch needed a `cmc` before its JCC
    uint16_t dirty_host_regs_ = 0;
    int    reg_vreg_[16];
    std::vector<int>     vreg_home_;
    std::vector<uint8_t> vreg_dirty_;
};
}  // namespace

// ─────────────────────────────────────────────────────────────────────────
// Tier-2 Phase 1 step 3 / M1: compile a collected trace as ONE x86 function
// with ONE region-wide register allocation. See the header contract
// (FrostJIT.hpp:479) and ROADMAP #14 for the full design; the layout:
//
//   prologue                       (push/frame/reg-setup, R10 window if used)
//   body_start:  block0 body        (all body ops via compile_ir_inst)
//                [flag prep][cmc] jcc0 → cold exit 0 / back-edge; [cmc restore]
//                block1 body ... block(N-1) body
//                [flag prep][cmc] jccN → Lback; [cmc restore]
//   L_exit:      clobber_flags; flush_all_vregs; store exit_pc; restore; ret
//   cold exit 0  (restore snapshot 0): [cmc] materialize; flush; store pc; ret
//   ...          cold exit N-2
//   Lback:       (restore last snapshot): [cmc][materialize if loop-carried]
//                flush_all_vregs; jmp body_start
//
// Internal fall-through edges carry the flags in host RFLAGS (no per-edge
// pstate materialize — the win); only the side exits, the cold fall-through
// exit, and the back-edge (when the loop top reads pstate) materialize.
// The back-edge flushes loop-carried state to cpu.regs[]/stack and jumps to
// body_start; block0's body was compiled from a fresh regalloc state, so it
// reloads everything from memory.
//
// Called with blocks_mutex_ held EXCLUSIVE (the region fn lands in code_buf_
// alongside translate_block's output). Returns nullptr if the trace shape
// isn't region-qualified.
uint64_t (*FrostJIT::compile_tier2_region(Emulator& emu, const Tier2Trace& trace,
                                          uint64_t (**chain_out)(CPU*, Emulator*),
                                          Tier2RegionMonitor* monitor_override))(CPU*, Emulator*) {
    (void)emu;
    if (chain_out) *chain_out = nullptr;
    // Shape checks, region SSA, pin selection and DCE/LICM live in
    // RegionPlanner.cpp; this function only generates and publishes code.
    Tier2RegionPlan plan;
    if (!code_buf_ || !plan_tier2_region(trace, monitor_override, plan)) return nullptr;
    const auto& blocks = trace.blocks;
    const size_t nblk = blocks.size();
    bool last_is_backedge = plan.last_is_backedge;
    int region_max_vreg = plan.region_max_vreg;
    auto& rblocks = plan.blocks;
    auto& region_ir = plan.region_ir;
    auto& preheader = plan.preheader;
    const bool ssa_region = plan.ssa_region;
    const bool has_call = plan.has_call;

    // ── Codegen state reset (mirror translate_block's start) ─────────
    make_writable();
    current_start_pc_ = trace.head_pc;
    code_buf_overflow_ = false;
    call_interp_branch_patches_.clear();
    branch_target_patches_.clear();
    skip_fixups_.clear();
    rax_holds_next_pc_ = false;
    flags_in_host_ = false;
    flags_from_sub_ = false;
    chain_target_pc_ = 0;
    unchainable_end_ = true;  // the region never chains out
    has_selfloop_slot_ = false;
    selfloop_patch_off_ = 0;
    has_taken_chain_slot_ = false;
    taken_chain_patch_off_ = 0;
    taken_chain_target_pc_ = 0;
    chain_entry_off_ = 0;
    num_stack_slots_ = 0;
    vec_cache_reset();
    // Select one register layout for the complete fused path. Internal edges
    // retain XMM state; cold entries reload it and every external exit commits
    // the region-wide write set, including writes from a previous loop trip.
    IRBlock cache_ir;
    cache_ir.insts = region_ir;
    if (!vec_cache_may_enable(cache_ir)) fp_cache_may_enable(cache_ir);
    regalloc_stats_reset();
    size_t block_start = code_buf_used_;
    // A region can be smaller than the preceding Tier-1 block. Clearing
    // only its own vregs leaves stale homes above region_max_vreg, then
    // publishing that smaller maximum makes the next block skip them too.
    // Region compilation is rare; reset the entire table, including aborts.
    constexpr int clear_limit = 4096;
    for (int i = 0; i < clear_limit; i++) {
        vreg_home_[i] = -1;
        vreg_dirty_[i] = false;
        vreg_slot_[i] = 0;
        vreg_last_use_[i] = 0;
    }
    for (int i = 0; i < NUM_HOST_REGS; i++) reg_vreg_[i] = -1;
    max_vreg_ = region_max_vreg;
    dirty_host_regs_ = 0;
    // Pre-assign stack slots for every region scratch vreg (fixed frame).
    for (int v = 33; v <= region_max_vreg; v++) {
        vreg_slot_[v] = -8 * (v - 32);
    }
    num_stack_slots_ = region_max_vreg - 32;
    if (num_stack_slots_ < 1) num_stack_slots_ = 1;
    uint32_t stack_bytes = static_cast<uint32_t>(num_stack_slots_ * 8 + 64) & ~15U;

    // ── Liveness over the concatenated region IR (mirror translate_block) ──
    {
        size_t n = region_ir.size();
        vreg_last_use_op_.assign(4096, -1);
        reset_vreg_uses();
        for (size_t i = 0; i < n; i++) {
            const IRInst& inst = region_ir[i];
            if (inst.op != IROp::LOAD_REG) {
                if (inst.src1 > 31 && inst.src1 < 4096) {
                    vreg_last_use_op_[inst.src1] = static_cast<int>(i);
                    note_vreg_use(inst.src1, static_cast<uint16_t>(i));
                }
                if (inst.src2 > 31 && inst.src2 < 4096) {
                    vreg_last_use_op_[inst.src2] = static_cast<int>(i);
                    note_vreg_use(inst.src2, static_cast<uint16_t>(i));
                }
            }
            if (inst.aux > 31 && inst.aux < 4096) {
                vreg_last_use_op_[inst.aux] = static_cast<int>(i);
                note_vreg_use(inst.aux, static_cast<uint16_t>(i));
            }
        }
        kills_per_op_.assign(n, {});
        for (int v = 32; v < 4096; v++) {
            if (vreg_last_use_op_[v] >= 0) {
                kills_per_op_[vreg_last_use_op_[v]].push_back(static_cast<uint16_t>(v));
            }
        }
        // Fold-lookahead (M2 cross-block const-prop — supersedes translate_block's
        // i+1-adjacent rule): an IMM is fold-ahead-skippable when its dest has
        // EXACTLY ONE consumer anywhere later in the region (vreg_uses_ size
        // 1) and that consumer folds the const into an x86 immediate (ALU or
        // shift kind, same guards). The consumer never reads the vreg's host
        // mapping, so the IMM codegen may skip its mov. Safety: vregs are
        // unique per definition (the IMM is the only writer of its dest), the
        // single-use check guarantees the const vreg is dead AT the consumer
        // (vreg_last_use_this_op holds at op j), jit_consts_ is populated by
        // the IMM before any body op (the preheader's clear precedes the body
        // loop and nothing re-defines the dest between i and j), and the ALU
        // kind-1 imm32 sign-extend fit is re-checked at the IMM codegen
        // (emits the mov on mismatch, so a non-folding consumer still finds
        // the vreg mapped). The shift kind-2 fold has no extra guards.
        fold_ahead_kind_.assign(n, 0);
        for (size_t i = 0; i < n; i++) {
            const IRInst& im = region_ir[i];
            if (im.op != IROp::IMM) continue;
            if (!(im.dest > 32 && im.dest < 4096)) continue;
            if (vreg_uses_[im.dest].size() != 1) continue;  // single consumer
            if (vreg_last_use_op_[im.dest] <= static_cast<int>(i)) continue;
            const IRInst& nx = region_ir[vreg_last_use_op_[im.dest]];
            if (nx.src2 != im.dest || nx.dest == nx.src2 || nx.src1 == nx.src2) continue;
            switch (nx.op) {
                case IROp::ADD: case IROp::SUB: case IROp::AND:
                case IROp::OR:  case IROp::XOR:
                    fold_ahead_kind_[i] = 1;
                    break;
                case IROp::SHL: case IROp::SHR: case IROp::SAR: case IROp::ROR:
                    fold_ahead_kind_[i] = 2;
                    break;
                default:
                    break;
            }
        }
    }
    // Region-wide flag-loop-carry: true iff a flag CONSUMER appears before
    // the first flag SETTER over the concatenated IR. If false, the loop
    // back-edge may skip the pstate materialization (the loop top's first
    // flag-setting op re-establishes host flags before any consumer reads
    // them); if true, the back-edge must materialize (the loop top reads
    // pstate as an input — same contract as the cross-block skip).
    bool region_flags_loop_carried = false;
    {
        bool setter_seen = false;
        for (const IRInst& inst : region_ir) {
            bool consumer = false;
            switch (inst.op) {
                case IROp::FP_CMP:
                    consumer = inst.fp_cmp_params().conditional;
                    break;
                case IROp::CSEL: case IROp::CSINC: case IROp::CSINV: case IROp::CSNEG:
                case IROp::ADCS: case IROp::SBCS: case IROp::CCMP:
                case IROp::FP_CSEL: case IROp::BRCOND: case IROp::BRCOND_SKIP:
                    consumer = true;
                    break;
                default:
                    break;
            }
            if (consumer && !setter_seen) {
                region_flags_loop_carried = true;
                break;
            }
            switch (inst.op) {
                case IROp::ADDS: case IROp::SUBS:
                case IROp::ADCS: case IROp::SBCS:
                case IROp::TST: case IROp::TST_ZERO:
                case IROp::CCMP: case IROp::FP_CMP:
                    setter_seen = true;
                    break;
                default:
                    break;
            }
        }
    }

    // ── Prologue (mirror translate_block) ─────────────────────────────
    emit_push(RBX); emit_push(RBP); emit_push(R12);
    emit_push(R13); emit_push(R14); emit_push(R15);
    emit_byte(0x48); emit_byte(0x89); emit_byte(0xE5);  // mov rbp, rsp
    emit_byte(0x48); emit_byte(0x81); emit_byte(0xEC);
    // Chain-skip: allocate the SAME unified 32 KB frame as every block so a
    // chain edge into the region's chain_entry sees an identical layout
    // (vreg slots -8*(v-32) fit either way; emu_slot_off() is mode-aware).
    emit_u32(chain_skip_enabled() ? kChainSkipFrameBytes : stack_bytes);
    emit_byte(0x48); emit_byte(0x89); emit_byte(0xFB);  // mov rbx, rdi (CPU)
    emit_store(RBP, emu_slot_off(), RSI);               // stash Emulator*
    // Chain-skip entry label (mirror translate_block's chain_entry_off_):
    // recorded AFTER the emu stash, BEFORE the R10 window load / pin
    // preloads / preheader. Chain edges bare-jmp here with the ROOT's
    // rbp/rsp/rbx intact; everything from this point on (window load, pins,
    // preheader, body) re-runs per entry exactly like a chained block.
    const size_t region_chain_entry_off = code_buf_used_;
    // Direct-window base into R10 only if some region op touches the window.
    {
        bool uses_window = false;
        for (const auto& inst : region_ir) {
            switch (inst.op) {
                case IROp::LOAD_MEM:
                case IROp::STORE_MEM:
                case IROp::ATOMIC:
                case IROp::SIMD_LD16:
                case IROp::SIMD_ST16:
                    uses_window = true;
                    break;
                case IROp::LDXR_FAST:
                    uses_window = !inst.llsc_exclusive();
                    break;
                default:
                    break;
            }
            if (uses_window) break;
        }
        if (window_base_ && uses_window)
            emit_mov_imm64(WIN_REG, reinterpret_cast<uint64_t>(window_base_));
    }
    vec_emit_prologue_loads();
    // Phase 2 pin preloads (cpu.regs[a] → pin reg). Emitted after the R10
    // window load but before body_start: every region entry runs the full
    // prologue (there is no chain-entry shortcut for regions), while the
    // back-edge jmp to body_start skips these — so the pins are loop-carried.
    // The compile-time mapping is established here too, so block0's body reads
    // (LOAD_REG/STORE_REG src1) reuse the pin instead of reloading cpu.regs[].
    // Clean: each pin mirrors cpu.regs[a] exactly.
    for (int a = 0; a <= 30; a++) {
        if (arch_pin_[a] >= 0) {
            emit_load_arm(arch_pin_[a], a);
            vreg_home_[a] = arch_pin_[a];
            reg_vreg_[arch_pin_[a]] = a;
            vreg_dirty_[a] = false;
            vreg_last_use_[a] = ++regalloc_lru_counter_;
        }
    }
    // ── M2 preheader (LICM hoisted invariants) ────────────────────────
    // Emitted AFTER the pin preloads (so hoisted LOAD_REG of a pinned arch
    // reuses the pin — a clean reg→reg move) and BEFORE body_start (so the
    // Lback jumps past it). After the preheader the codegen state is
    // flush+invalidate'd: the hoisted results land in their pre-assigned
    // stack slots and block0's body (compiled below from a fresh regalloc)
    // reloads them every iteration. jit_consts_ is populated BEFORE the body
    // loop so body consumers can fold hoisted IMMs (the body loop's clear is
    // moved up here; preheader consts persist into the body).
    jit_consts_.clear();
    if (!preheader.empty()) {
        // Liveness for the preheader's OWN chain. Without this, the
        // preheader compiled with cur_op_index_ = fold_ahead_kind_.size()
        // (out of range — deliberately, so hoisted IMMs never skip their
        // mov) made EVERY vreg look dead to the Belady allocator
        // (next_use_after returned -1 for all, since the real uses lie at
        // indices < the out-of-range cursor). alloc_reg's Belady eviction
        // then scored every occupied reg equal (INT_MAX) and picked the
        // FIRST allocable reg — RAX — evicting a just-loaded operand:
        // H9's `v44 = v41 + v43` loaded v41 into RAX, then ensure_vreg(v43)
        // evicted v41 and loaded v43 into RAX, emitting `mov rax,rax; add
        // rax,rax` → v44 = 2*v43 instead of v41+v43 (m5self's acc came out
        // 0x33333333 short per call). Fix: inject SYNTHETIC preheader use
        // positions (PREHEADER_BASE + k) into vreg_uses_ so next_use_after
        // sees each preheader-defined vreg's real next read, and compile
        // preheader op k with cur_op_index_ = PREHEADER_BASE + k. Fold-ahead
        // stays off (its bounds check `cur_op_index_ < fold_ahead_kind_
        // .size()` is false for the synthetic cursor) and the body's liveness
        // is unaffected (body indices are all < PREHEADER_BASE, so
        // next_use_after in the body still returns the correct next body
        // use; the synthetic high entries are only consulted during
        // preheader compilation, when the body uses lie BELOW the cursor).
        constexpr size_t PREHEADER_BASE = 40000;
        for (size_t k = 0; k < preheader.size(); k++) {
            const IRInst& inst = preheader[k];
            if (inst.src1 > 32 && inst.src1 < 4096)
                note_vreg_use(inst.src1, static_cast<uint16_t>(PREHEADER_BASE + k));
            if (inst.src2 > 32 && inst.src2 < 4096)
                note_vreg_use(inst.src2, static_cast<uint16_t>(PREHEADER_BASE + k));
            if (inst.aux > 32 && inst.aux < 4096)
                note_vreg_use(inst.aux, static_cast<uint16_t>(PREHEADER_BASE + k));
        }
        for (size_t k = 0; k < preheader.size(); k++) {
            cur_op_index_ = PREHEADER_BASE + k;  // real-chain liveness, fold-ahead off
            const IRInst& inst = preheader[k];
            if (compile_ir_inst(inst)) {
                rewind_codegen(block_start);
                make_executable();
                return nullptr;
            }
            if (inst.op != IROp::IMM) jit_consts_.erase(inst.dest);
        }
        flush_all_vregs();
        invalidate_all_vregs();

        // ── P2-lite: register-resident LICM results across the Lback ─────
        // The hoisted invariants now live in their (just-flushed, current)
        // stack slots and block0's body reloads them on EVERY iteration.
        // Reload the hottest roots into free CALLEE-SAVED registers here —
        // emitted BEFORE body_start, so the Lback jumps past it and the
        // load runs once per region ENTRY — and pin those regs for the
        // body compile so the allocator never evicts them (same mechanism
        // as the arch GPR pins; callee-saved is required because RAX/RCX/
        // RDX/R8/R9/R11 are in the memory ops' MEM_CLOBBER set — the
        // slow-path C helper destroys caller-saved regs mid-body). The
        // keeps are read-only in the body (LICM invariants), so they stay
        // CLEAN: no exit spill, and the kill at their last body use emits
        // nothing. Blocked when has_call (callee clobbers cpu.regs[] and
        // caller-saved state — mirrors the arch-pin policy) and fenced to
        // leave the body ≥3 allocatable regs (the emitter-audit fence: a
        // steered/pinned pool under 3 regs exposes ALU-emitter variant-2).
        if (!getenv("BIFROST_NO_KEEPRES") && last_is_backedge && !has_call) {
            const size_t n_body = region_ir.size();
            struct KeepCand { uint16_t v; int uses; };
            std::vector<KeepCand> cands;
            for (const auto& inst : preheader) {
                const uint16_t v = inst.dest;
                if (!(v > 32 && v < 4096)) continue;
                int u = 0;
                for (uint16_t ui : vreg_uses_[v]) {
                    if (ui < n_body) u++;  // body uses (synthetic preheader
                                           // entries live at PREHEADER_BASE)
                }
                if (u > 0) cands.push_back({v, u});
            }
            std::sort(cands.begin(), cands.end(),
                      [](const KeepCand& a, const KeepCand& b) {
                          return a.uses > b.uses;
                      });
            int n_alloc_pinned = 0;
            for (int r : ALLOC_REGS) {
                if (pinned_host_regs_ & (1u << r)) n_alloc_pinned++;
            }
            const int max_keeps =
                std::min(NUM_PIN_REGS, NUM_ALLOC_REGS - n_alloc_pinned - 3);
            int placed = 0;
            for (const auto& c : cands) {
                if (placed >= max_keeps) break;
                int r = -1;
                for (int pr : PIN_REGS) {
                    if (!(pinned_host_regs_ & (1u << pr))) { r = pr; break; }
                }
                if (r < 0) break;  // all callee-saved regs are arch pins
                emit_load(r, RBP, vreg_slot_[c.v]);
                vreg_home_[c.v] = r;
                reg_vreg_[r] = c.v;
                vreg_dirty_[c.v] = false;  // read-only: slot stays current
                vreg_last_use_[c.v] = ++regalloc_lru_counter_;
                pinned_host_regs_ |= (1u << r);
                placed++;
            }
            if (dbg().tier2_trace_verbose) {
                fprintf(stderr, "[tier2] keepres: %d candidates, %d pinned\n",
                        static_cast<int>(cands.size()), placed);
            }
        }
    }
    // One CPU-local flag-neutral store per completed hot iteration. No
    // shared atomic increment, register clobber, or per-entry flag save.
    auto emit_completion_credit = [&] {
        if (!tier2_current_monitor_) return;
        const size_t offset = offsetof(CPU, jit_tier2_feedback) +
            tier2_current_monitor_->slot * sizeof(CPU::Tier2FeedbackSlot) +
            offsetof(CPU::Tier2FeedbackSlot, credit);
        emit_byte(0xC7); emit_byte(0x83); // mov dword [rbx+disp32],1
        emit_u32(static_cast<uint32_t>(offset)); emit_u32(1);
    };
    auto emit_trial_backedge = [&] {
        if (!tier2_sampling_compile_) return;
        // Preserve the hot path's host flags and codegen flag state. The
        // quota exit flushes loop-carried pins before returning to the timer.
        const bool saved_host = flags_in_host_, saved_sub = flags_from_sub_;
        const uint16_t saved_dirty = dirty_host_regs_;
        int saved_regs[16]; memcpy(saved_regs, reg_vreg_, sizeof(saved_regs));
        std::vector<int> saved_homes(vreg_home_, vreg_home_ + 4096);
        std::vector<bool> saved_vdirty(vreg_dirty_, vreg_dirty_ + 4096);
        emit_push(RCX);
        emit_load(RCX, CPU_REG, offsetof(CPU, jit_tier2_trial));
        emit_byte(0xE3); emit_byte(15); // inactive: skip flag save/quota check
        emit_byte(0x9C);
        emit_byte(0x83); emit_byte(0xAB);
        emit_u32(offsetof(CPU, jit_tier2_trial_remaining)); emit_byte(1);
        const size_t quota = emit_jcc_rel32_placeholder(4);
        emit_byte(0x9D);
        emit_pop(RCX);
        const size_t hot = emit_jmp_rel32_placeholder();
        const size_t cold = code_buf_used_;
        emit_byte(0x9D); emit_pop(RCX);
        materialize_flags_to_pstate();
        flush_all_vregs_keep();
        vec_cache_writeback_all_pinned(false);
        for (int a = 0; a <= 30; ++a)
            if (arch_pin_[a] >= 0) emit_store_arm(a, arch_pin_[a]);
        emit_mov_imm_to_rax(trace.head_pc);
        emit_store(CPU_REG, PC_OFF, RAX);
        emit_mov_reg(RDI, CPU_REG); emit_load(RSI, RBP, emu_slot_off());
        emit_byte(0x48); emit_byte(0x89); emit_byte(0xEC);
        emit_pop(R15); emit_pop(R14); emit_pop(R13);
        emit_pop(R12); emit_pop(RBP); emit_pop(RBX); emit_ret();
        patch_jcc_rel32(quota, static_cast<int32_t>(cold - (quota + 6)));
        patch_jmp_rel32(hot, static_cast<int32_t>(code_buf_used_ - (hot + 5)));
        flags_in_host_ = saved_host; flags_from_sub_ = saved_sub;
        dirty_host_regs_ = saved_dirty;
        memcpy(reg_vreg_, saved_regs, sizeof(saved_regs));
        for (unsigned v = 0; v < 4096; ++v) {
            vreg_home_[v] = saved_homes[v]; vreg_dirty_[v] = saved_vdirty[v];
        }
    };
    // State-phi reloads at the loop head use CPU storage for non-pinned
    // architectural slots. A deferred STORE_REG in a safe host register
    // must commit on the backedge even when that slot was statically clean
    // when the head's load was emitted. Pins carry their value directly.
    auto emit_ssa_backedge_state = [&] {
        if (!ssa_region) return;
        for (int a = 0; a < 32; ++a)
            if (arch_pin_[a] < 0 && vreg_home_[a] >= 0 && vreg_dirty_[a])
                emit_store_arm(a, vreg_home_[a]);
    };
    size_t body_start = code_buf_used_;

    // ── Compile the region body ───────────────────────────────────────
    // Body ops go through the normal compile_ir_inst path (with the per-op
    // kill/dead-scratch-drop, exactly like translate_block's compile loop).
    // The terminating conditional branch of each block is handled MANUALLY
    // (no block-ending epilogue, no chain slots, no dispatcher return): the
    // not-taken path falls through inline into the next block (flags stay in
    // host RFLAGS), and the taken path is a forward rel32 patched at the end
    // to a deferred cold exit (or to Lback for the last block).
    struct Snapshot {
        RegionSnapshot rs;
        size_t jcc_patch;
        bool   need_cmc;
        uint64_t side_pc;
    };
    std::vector<Snapshot> cold;  // per non-last block, in order
    RegionSnapshot back_snap;
    bool has_back_snap = false;
    size_t last_jcc = 0;         // last block's taken JCC (patched → Lback)
    bool last_term_uncond = false;  // last block ends in an unconditional `b`
    bool skip_exit_sections = false;  // uncond back-edge: L_exit/Lback dead
    size_t blk = 0;
    for (size_t i = 0; i < region_ir.size(); i++) {
        while (blk < nblk && i > rblocks[blk].term) blk++;
        const IRInst& inst = region_ir[i];
        cur_op_index_ = i;
        if (i == rblocks[blk].start) emit_tier2_work_probe(rblocks[blk].pc, rblocks[blk].inst_count);

        if (i == rblocks[blk].term) {
            // ── Terminating conditional branch (manual) ─────────────
            // NOTE: BRCOND_ZERO/BRCOND_BIT skip the pushfq/popfq the
            // single-block codegen wraps around their test/bt. clobber_flags()
            // has already materialized pending host flags to pstate and set
            // flags_in_host_ = false, so nothing in the region reads host
            // RFLAGS after the test — the next inline block body reloads
            // flags from pstate when it needs them.
            size_t jcc = 0;
            bool need_cmc = false;
            switch (inst.op) {
                case IROp::BRCOND: {
                    const BrCondParams bp = inst.brcond_params();
                    if (!flags_in_host_) {
                        // FLAGS3 only: the loader clobbers RAX/RCX/RDX
                        // (RDX-only C^from_sub extraction; see X86Emitter.cpp).
                        constexpr uint16_t FLAGS3 =
                            (1u << RAX) | (1u << RCX) | (1u << RDX);
                        flush_dirty_host_regs(FLAGS3);
                        flush_scratch_host_regs(FLAGS3);
                        emit_load_flags_from_pstate();
                        invalidate_host_regs(FLAGS3);
                        flags_from_sub_ = true;  // CF is now in SUB convention
                        flags_in_host_ = true;
                    }
                    uint8_t cc = resolve_arm_cond_with_carry(bp.cond, need_cmc);
                    if (need_cmc) emit_byte(0xF5);  // cmc — invert CF for HI/LS
                    jcc = emit_jcc_rel32_placeholder(cc);
                    break;
                }
                case IROp::BRCOND_ZERO: {
                    const BrCondZeroParams bp = inst.brcond_zero_params();
                    clobber_flags();
                    if (reg_vreg_[RAX] >= 0) drop_vreg(reg_vreg_[RAX]);
                    int s1 = ensure_vreg(inst.src1, RAX);
                    if (s1 != RAX) emit_mov_reg(RAX, s1);
                    if (reg_vreg_[RAX] >= 0) drop_vreg(reg_vreg_[RAX]);
                    // W-form tests only the low 32 bits (see the matching
                    // fix in Branch.cpp) — mask dirty uppers.
                    if (!bp.sf) {
                        emit_byte(0x89); emit_byte(0xC0);  // mov eax, eax
                    }
                    emit_test_reg(RAX, RAX);
                    uint8_t cc = (bp.cond == 0) ? 4 /*JE*/ : 5 /*JNE*/;
                    jcc = emit_jcc_rel32_placeholder(cc);
                    break;
                }
                case IROp::BRCOND_BIT: {
                    const BrCondBitParams bp = inst.brcond_bit_params();
                    clobber_flags();
                    if (reg_vreg_[RAX] >= 0) drop_vreg(reg_vreg_[RAX]);
                    int s1 = ensure_vreg(inst.src1, RAX);
                    if (s1 != RAX) emit_mov_reg(RAX, s1);
                    if (reg_vreg_[RAX] >= 0) drop_vreg(reg_vreg_[RAX]);
                    emit_byte(rex(true, false, false, RAX >= 8));
                    emit_byte(0x0F); emit_byte(0xBA);
                    emit_byte(modrm(3, 5, RAX & 7));
                    emit_byte(bp.bit);  // bit number
                    uint8_t cc = (bp.cond == 0) ? 3 /*JNC*/ : 2 /*JC*/;
                    jcc = emit_jcc_rel32_placeholder(cc);
                    break;
                }
                case IROp::BRCOND_FALLTHRU: {
                    // An internal direct edge is already laid out inline.
                    // Keep register/flag state live across this boundary.
                    if (blk + 1 != nblk) {
                        blk++;
                        continue;
                    }
                    last_term_uncond = true;
                    if (last_is_backedge) {
                        // The back-edge is a PLAIN jmp (no JCC): emit it here,
                        // right after the last body op, and skip the L_exit /
                        // Lback sections below (dead code — nothing falls into
                        // them). Materialize pstate first iff the loop top
                        // reads it (mirror the cond-back-edge Lback).
                        if (region_flags_loop_carried) {
                            materialize_flags_to_pstate();
                        }
                        emit_ssa_backedge_state();
                        emit_completion_credit();
                        emit_trial_backedge();
                        int32_t rel = static_cast<int32_t>(
                            body_start - (code_buf_used_ + 5));
                        emit_byte(0xE9);
                        emit_u32(static_cast<uint32_t>(rel));  // jmp body_start
                        skip_exit_sections = true;
                    }
                    blk++;
                    continue;
                }
                default:
                    rewind_codegen(block_start);
                    make_executable();
                    return nullptr;
            }
            // Snapshot the codegen state AT THE JCC (pre fall-through cmc
            // restore) so the deferred taken-edge epilogue emits as-of here.
            RegionSnapshot snap;
            snap.flags_in_host_ = flags_in_host_;
            snap.flags_from_sub_ = flags_from_sub_;
            snap.need_cmc_ = need_cmc;
            snap.dirty_host_regs_ = dirty_host_regs_;
            memcpy(snap.reg_vreg_, reg_vreg_, sizeof(reg_vreg_));
            int n = max_vreg_ + 1;
            if (n > 4096) n = 4096;
            snap.vreg_home_.resize(static_cast<size_t>(n));
            snap.vreg_dirty_.resize(static_cast<size_t>(n));
            for (int k = 0; k < n; k++) {
                snap.vreg_home_[k] = vreg_home_[k];
                snap.vreg_dirty_[k] = vreg_dirty_[k] ? 1 : 0;
            }
            if (blk + 1 < nblk) {
                // Non-last block: the taken edge is an ordinary cold exit.
                cold.push_back({std::move(snap), jcc, need_cmc, rblocks[blk].side_pc});
            } else if (last_is_backedge) {
                // Last block, back-edge: the taken edge loops to the head.
                last_jcc = jcc;
                back_snap = std::move(snap);
                has_back_snap = true;
            } else {
                // Last block, no back-edge: its taken edge is a cold exit too.
                cold.push_back({std::move(snap), jcc, need_cmc, rblocks[blk].side_pc});
            }
            // Fall-through: restore CF so the next inline block body reads the
            // original carry (the branch's cmc was only for its own JCC).
            if (need_cmc) emit_byte(0xF5);
            // Free host regs of vregs whose last use was the term op.
            if (i < kills_per_op_.size()) {
                for (uint16_t v : kills_per_op_[i]) {
                    if (v != inst.dest) kill_vreg(v);
                }
            }
            blk++;
            continue;
        }

        // ── Body op (normal compile path) ───────────────────────────
        if (compile_ir_inst(inst)) {
            // A body op ended the block. The walker guarantees this never
            // happens (CALL_INTERP / SVC / BR abort the trace; BL/BLR fuse as
            // BL_CALL/BLR_CALL which return false and do not end the block),
            // so this is defensive only.
            rewind_codegen(block_start);
            make_executable();
            return nullptr;
        }
        if (inst.op != IROp::IMM) jit_consts_.erase(inst.dest);
        if (i < kills_per_op_.size()) {
            for (uint16_t v : kills_per_op_[i]) {
                if (v != inst.dest) kill_vreg(v);
            }
        }
        if (inst.dest > 32 && inst.dest < 4096 &&
            vreg_last_use_op_[inst.dest] <= static_cast<int>(i)) {
            kill_vreg(inst.dest);
        }
    }

    // ── Cold fall-through exit (L_exit), live state ─────────────────
    // The last block's not-taken path falls through here. exit_pc is the
    // guest PC right after the last block (the walker stopped there) — or
    // the unconditional branch target when the last block is `b target`.
    // SKIPPED for an unconditional back-edge (its jmp to body_start was
    // already emitted inline at the term; nothing falls through here).
    // Flush EVERY pinned arch vreg at every exit, not just the ones dirty
    // at the snapshot: a pin can be loop-carried-deferred (block 1 — or a
    // prior Lback iteration — wrote it via STORE_REG into its pin register),
    // so a snapshot "clean" pin may still hold the current value ONLY in the
    // register while cpu.regs[] is stale. A cold exit taken at a block whose
    // IR never writes the pin would otherwise return the stale cpu.regs[]
    // value to the caller (CoreMark crclist: x2 was deferred in block 1,
    // then the "found" cold exit at block 0 returned the pre-loop pointer →
    // wrong list navigation → wrong CRC). Over-flushing is safe — pins always
    // hold the current arch value at runtime.
    auto emit_flush_all_pins = [&]() {
        // A side exit before this iteration's first vector write can still
        // observe a value carried from a later block on the previous trip.
        // Keep codegen dirtiness intact for the remaining deferred exits.
        vec_cache_writeback_all_pinned(false);
        for (int a = 0; a <= 30; a++) {
            if (arch_pin_[a] >= 0) emit_store_arm(a, arch_pin_[a]);
        }
    };
    // ── Shared exit tail (Track 3: cold-exit dedup) ──────────────────
    // Every cold exit ends with the SAME fixed sequence (store cpu.pc from
    // RAX, rdi=cpu, rsi=emu from the frame slot, frame teardown, ret) —
    // ~33 bytes duplicated per edge. Each exit now ends with `movabs rax,
    // exit_pc; jmp L_shared_exit` (5 bytes) and ONE copy of the tail is
    // emitted after all exits. Only the per-edge VARIABLE part (cmc /
    // flag materialize / flush of that branch point's dirty vregs / pin
    // stores) remains inline — it differs per edge and cannot be shared
    // without runtime metadata. Cold exits are rare (taken edges leaving
    // the trace), so the extra jmp costs nothing measurable; the win is
    // region bytes + I-cache footprint (an 11-block region saves ~300 B).
    std::vector<size_t> shared_exit_patch_offs;
    auto emit_shared_exit_jmp = [&]() {
        size_t p = emit_jmp_rel32_placeholder();
        shared_exit_patch_offs.push_back(p);
    };
    // Region exits use revocable data cells in both modes. Untracked target
    // constants can execute stale guest instructions after invalidation.
    static const bool no_exitchain_ = (getenv("BIFROST_NO_EXITCHAIN") != nullptr);
    const bool exitchain_ok = !no_exitchain_ && !dbg().jit_verify;
    std::vector<size_t> cell_exit_patches;
    auto emit_chainout_exit = [&](uint64_t pc) -> bool {
        if (pc == trace.head_pc) emit_completion_credit();
        emit_mov_imm_to_rax(pc);
        emit_store(CPU_REG, PC_OFF, RAX);
        if (!mt_active()) {
            if (!exitchain_ok || dbg().no_chain || !lookup_only_locked(pc)) return false;
            MtEdge* edge = create_chain_edge(pc, true, code_buf_used_);
            emit_mov_imm_to_rax(pc);
            emit_store(CPU_REG, PC_OFF, RAX);
            emit_mov_imm64(R11, reinterpret_cast<uint64_t>(&edge->target));
            cell_exit_patches.push_back(emit_jmp_rel32_placeholder());
            return true;
        }
        emit_mov_imm_to_rax(pc);
        emit_store(CPU_REG, PC_OFF, RAX);
        emit_mov_reg(RDI, CPU_REG);
        emit_load(RSI, RBP, emu_slot_off());
        emit_byte(0x48); emit_byte(0x89); emit_byte(0xEC);
        emit_pop(R15); emit_pop(R14); emit_pop(R13);
        emit_pop(R12); emit_pop(RBP); emit_pop(RBX);
        if (exitchain_ok) emit_mt_chain_slot(pc, true);
        emit_ret();
        return true;
    };
    if (!skip_exit_sections) {
        uint64_t exit_pc = rblocks[nblk - 1].pc + rblocks[nblk - 1].inst_count * 4;
        if (blocks.back().follows_taken) exit_pc = blocks.back().hot_successor;
        if (last_term_uncond) exit_pc = rblocks[nblk - 1].side_pc;  // the B target
        clobber_flags();
        flush_all_vregs();
        emit_flush_all_pins();
        if (exit_pc != trace.head_pc) emit_completion_credit();
        if (!emit_chainout_exit(exit_pc)) {
            emit_mov_imm_to_rax(exit_pc);
            emit_shared_exit_jmp();
        }
    }

    // ── Deferred side-exit epilogues (one per non-last block) ───────
    // Restore each block's branch-point codegen state, then emit the
    // materialize / flush / pc-store with that state. At runtime the JCC
    // jumps straight here, so the registers still hold the branch-era
    // values; the flush stores exactly those to cpu.regs[]/stack. Each cold
    // exit returns to the C dispatcher (ret) with RAX = the side-exit PC.
    std::vector<size_t> cold_off;
    cold_off.reserve(cold.size());
    for (const auto& c : cold) {
        cold_off.push_back(code_buf_used_);
        const RegionSnapshot& s = c.rs;
        flags_in_host_ = s.flags_in_host_;
        flags_from_sub_ = s.flags_from_sub_;
        dirty_host_regs_ = s.dirty_host_regs_;
        memcpy(reg_vreg_, s.reg_vreg_, sizeof(reg_vreg_));
        for (size_t k = 0; k < s.vreg_home_.size(); k++) {
            vreg_home_[k] = s.vreg_home_[k];
            vreg_dirty_[k] = s.vreg_dirty_[k] != 0;
        }
        if (s.need_cmc_) emit_byte(0xF5);  // restore CF for the materialize
        materialize_flags_to_pstate();
        flush_all_vregs();
        emit_flush_all_pins();
        if (tier2_current_monitor_ && c.side_pc != trace.head_pc) {
            emit_mov_imm_to_rax(c.side_pc);
            emit_store(CPU_REG, PC_OFF, RAX);
            emit_mov_reg(RDI, CPU_REG);
            emit_load(RSI, RBP, emu_slot_off());
            emit_mov_imm64(RDX, reinterpret_cast<uint64_t>(tier2_current_monitor_));
            emit_mov_imm64(R11, reinterpret_cast<uint64_t>(&tier2::feedback_exit));
            emit_byte(0x48); emit_byte(0x89); emit_byte(0xEC);
            emit_pop(R15); emit_pop(R14); emit_pop(R13);
            emit_pop(R12); emit_pop(RBP); emit_pop(RBX);
            emit_byte(0x41); emit_byte(0xFF); emit_byte(0xE3); // tail jmp r11
        } else if (!emit_chainout_exit(c.side_pc)) {
            emit_mov_imm_to_rax(c.side_pc);
            emit_shared_exit_jmp();
        }
    }

    if (!cell_exit_patches.empty()) {
        const size_t tail = code_buf_used_;
        emit_mov_reg(RDI, CPU_REG);
        emit_load(RSI, RBP, emu_slot_off());
        emit_byte(0x48); emit_byte(0x89); emit_byte(0xEC);
        emit_pop(R15); emit_pop(R14); emit_pop(R13);
        emit_pop(R12); emit_pop(RBP); emit_pop(RBX);
        // Full-frame targets, including chain-skip. Honor stop sentinels
        // and the dispatch lease before dereferencing the revocable cell.
        emit_byte(0x48); emit_byte(0x3B); emit_byte(0x87);
        emit_u32(offsetof(CPU, jit_stop_pc));
        const size_t stop = emit_jcc_rel32_placeholder(4);
        emit_byte(0x83); emit_byte(0xAF);
        emit_u32(offsetof(CPU, jit_chain_budget)); emit_byte(1);
        const size_t budget = emit_jcc_rel32_placeholder(6);
        emit_load(R11, R11, 0);
        emit_byte(0x4D); emit_byte(0x85); emit_byte(0xDB);
        const size_t empty = emit_jcc_rel32_placeholder(4);
        emit_byte(0x41); emit_byte(0xFF); emit_byte(0xE3);
        for (size_t p : {stop, budget, empty})
            patch_jcc_rel32(p, static_cast<int32_t>(code_buf_used_ - (p + 6)));
        emit_ret();
        for (size_t p : cell_exit_patches)
            patch_jmp_rel32(p, static_cast<int32_t>(tail - (p + 5)));
    }

    // ── Emit the shared exit tail + patch every exit's jmp ──────────
    // Skipped when no exits exist (1-block uncond-back-edge region: the
    // term's inline jmp to body_start is the only way out).
    if (!shared_exit_patch_offs.empty()) {
        size_t tail_off = code_buf_used_;
        emit_store(CPU_REG, PC_OFF, RAX);     // cpu.pc = rax (exit pc)
        emit_mov_reg(RDI, CPU_REG);           // mov rdi, rbx
        emit_load(RSI, RBP, emu_slot_off());  // rsi = emu
        emit_byte(0x48); emit_byte(0x89); emit_byte(0xEC);  // mov rsp, rbp
        emit_pop(R15); emit_pop(R14); emit_pop(R13);
        emit_pop(R12); emit_pop(RBP); emit_pop(RBX);
        emit_ret();
        for (size_t p : shared_exit_patch_offs)
            patch_jmp_rel32(p, static_cast<int32_t>(tail_off - (p + 5)));
    }

    // ── Loop back-edge (Lback): the last block's taken edge ─────────
    // Only emitted when the last block's taken target is the head (a hot
    // loop) AND the last block is a CONDITIONAL branch (an unconditional
    // back-edge emitted its jmp inline at the term, skip_exit_sections):
    // flush loop-carried state to cpu.regs[]/stack, then jmp to the
    // region body start. block0's body was compiled with a fresh regalloc
    // state (nothing mapped), so it reloads arch vregs from cpu.regs[] and
    // scratch vregs from their stack slots — the flush makes exactly that
    // state current. Materialize pstate on the back-edge only when the loop
    // top reads it.
    size_t lback_off = code_buf_used_;
    if (!skip_exit_sections && last_is_backedge && has_back_snap) {
        const RegionSnapshot& s = back_snap;
        flags_in_host_ = s.flags_in_host_;
        flags_from_sub_ = s.flags_from_sub_;
        dirty_host_regs_ = s.dirty_host_regs_;
        memcpy(reg_vreg_, s.reg_vreg_, sizeof(reg_vreg_));
        for (size_t k = 0; k < s.vreg_home_.size(); k++) {
            vreg_home_[k] = s.vreg_home_[k];
            vreg_dirty_[k] = s.vreg_dirty_[k] != 0;
        }
        if (region_flags_loop_carried) {
            if (s.need_cmc_) emit_byte(0xF5);
            materialize_flags_to_pstate();
        }
        // NO flush_all_vregs() here (Phase 2): regions write arch GPRs
        // eagerly (STORE_REG); cached FP/vector regs retain their fixed homes,
        // and each block's scratch vregs occupy DISJOINT remapped ranges, so
        // at the back-edge the only live values are the arch GPRs (pins +
        // cpu.regs[] always current) — the flush only ever stored dead
        // scratch vregs. Pinned arch vregs survive in R12-R15 and block0's
        // body (compiled with the pin mappings pre-established) reuses them.
    }
    if (!skip_exit_sections && last_is_backedge) {
        emit_ssa_backedge_state();
        emit_completion_credit();
        emit_trial_backedge();
        int32_t rel = static_cast<int32_t>(body_start - (code_buf_used_ + 5));
        emit_byte(0xE9);
        emit_u32(static_cast<uint32_t>(rel));  // jmp body_start
    }

    // Patch actual conditional exits, not block indices: internal direct
    // edges have neither a JCC nor a deferred cold snapshot.
    for (size_t k = 0; k < cold.size(); k++) {
        const size_t jcc = cold[k].jcc_patch;
        patch_jcc_rel32(jcc, static_cast<int32_t>(cold_off[k] - (jcc + 6)));
    }
    if (last_is_backedge && !last_term_uncond) {
        patch_jcc_rel32(last_jcc, static_cast<int32_t>(lback_off - (last_jcc + 6)));
    }

    // ── Publish the region ───────────────────────────────────────────
    size_t size = code_buf_used_ - block_start;
    make_executable();
    prev_max_vreg_ = max_vreg_;  // next translate_block's clear_limit (BlockCompiler.cpp:326)
    auto fn = reinterpret_cast<uint64_t (*)(CPU*, Emulator*)>(code_buf_ + block_start);
    if (code_buf_overflow_ || size == 0) return nullptr;
    if (chain_out && chain_skip_enabled())
        *chain_out = reinterpret_cast<uint64_t (*)(CPU*, Emulator*)>(
            code_buf_ + region_chain_entry_off);
    // TEMP diagnostic (BIFROST_T2_REGIONDUMP): hexdump the published region
    // for offline objdump — same gate name the P2 review session used.
    if (getenv("BIFROST_T2_REGIONDUMP")) {
        fprintf(stderr, "[t2dump] pc=0x%llx fn=%p bytes=%zu body=%zu\n",
                static_cast<unsigned long long>(trace.head_pc),
                reinterpret_cast<void*>(code_buf_ + block_start), size,
                body_start - block_start);
        for (size_t k = 0; k < size; k++) {
            fprintf(stderr, "%02x", code_buf_[block_start + k]);
            if ((k & 31) == 31) fprintf(stderr, "\n");
        }
        fprintf(stderr, "\n");
    }
    if (dbg().tier2_trace_verbose) {
        fprintf(stderr,
                "[tier2] region pc=0x%llx blocks=%zu insts=%llu bytes=%zu back_flags_carried=%d\n",
                static_cast<unsigned long long>(trace.head_pc), nblk,
                static_cast<unsigned long long>(trace.total_insts), size,
                static_cast<int>(region_flags_loop_carried));
    }
    return fn;
}
}  // namespace arm64emu
