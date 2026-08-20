// jit/jit_tier2.cpp — FrostJIT tier-2 trace collection (ROADMAP #14,
// Phase 1 step 2).
//
// Pure data collection: from a hot head (BlockEntry::exec_count crossed
// tier2_hits_threshold()), walk guest code, translate each instruction to IR
// with the SAME loop structure as translate_block, and produce a candidate
// Tier2Trace (a linear list of blocks + side exits). A LATER task compiles
// that trace as one region with one register allocation. This file emits NO
// code, never touches blocks_ beyond read-only membership checks, and never
// compiles anything.
//
// Stop classification: an ABORT (call_interp / svc / indirect_br / bl /
// decode_fail / vreg_exhaust) makes the trace unusable (ok stays false).
// A NORMAL end (cold_entry / revisit / ret / b_exit / b_backedge / block_cap
// / inst_cap) yields a valid bounded trace; ok is then decided by block count.
//
// Regalloc contract (IMPORTANT): translate_to_ir allocates scratch vregs from
// the thread_local g_alloc (vregs 0-32 = arch regs, scratch >= 33).
// translate_block resets it per block via ir_reset_vreg_alloc(); this walker
// does the same, so collection never consumes the global allocator. Each
// block's IRBlock.insts is self-contained (scratch numbering reused across
// blocks is fine — the region compiler remaps/re-allocates later). A guard
// before each instruction aborts the trace if the current block's allocation
// approaches the 4095-entry vreg table.
#include "jit/frostjit.hpp"
#include "core/emulator.h"
#include "ir/ir.hpp"
#include "ir/ir.h"
#include <algorithm>  // std::find
#include <cstdint>
#include <cstdio>
#include <cstring>  // memcpy
#include <vector>
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

Tier2Trace FrostJIT::collect_tier2_trace(Emulator& emu, uint64_t head_pc) {
    Tier2Trace trace;
    trace.head_pc = head_pc;

    // Trace caps (ROADMAP #14: "start 64 blocks or ~2048 guest instructions").
    constexpr int       MAX_TRACE_BLOCKS = 64;
    constexpr uint64_t  MAX_TRACE_INSTS  = 2048;
    // Vreg ceiling: guard before allocating (the per-block scratch space is
    // bounded by the 4095-entry uint16_t vreg table).
    constexpr uint16_t  VREG_LIMIT = 4000;
    // Same block-size cap as translate_block.
    constexpr int       MAX_BLOCK_REG_PRESSURE = 32;

    // PCs already collected. The walk only ever follows fall-through edges
    // (taken edges become side exits / back-edges), so a revisit is impossible
    // in practice — the set is a defensive guard per the tier-2 spec.
    std::vector<uint64_t> visited;

    uint64_t cur_pc = head_pc;
    bool trace_ended = false;  // normal termination (RET / B / caps / cold-entry)
    bool aborted = false;      // abort (trace unusable, ok stays false)

    while (!trace_ended && !aborted && trace.blocks.size() < MAX_TRACE_BLOCKS) {
        if (trace.total_insts >= MAX_TRACE_INSTS) {
            trace.stop_reason = "inst_cap";
            trace_ended = true;
            break;
        }
        // NOTE: there is deliberately NO "cold_entry" stop here anymore.
        // Real hot loops have ALL their blocks already translated (they ran
        // long enough to be hot), so stopping at the first cached block used
        // to abort every real trace at 1 block. The region compiler re-compiles
        // trace blocks from their IR regardless of whether a standalone block
        // exists, so walking through already-translated guest code is correct.
        // Only the head may be revisited (as a back-edge).
        if (cur_pc != head_pc &&
            std::find(visited.begin(), visited.end(), cur_pc) != visited.end()) {
            trace.stop_reason = "revisit";
            trace_ended = true;
            break;
        }

        Tier2Trace::Block tb;
        tb.pc = cur_pc;
        tb.ir.start_pc = cur_pc;
        visited.push_back(cur_pc);
        // Fresh scratch numbering per block (see the file header — collection
        // must NOT consume the global allocator).
        ir_reset_vreg_alloc();

        uint64_t ip = cur_pc;
        int instr_count = 0;
        bool block_ended = false;
        bool trace_ends_after_block = false;  // set by RET / B

        while (!block_ended && instr_count < MAX_BLOCK_REG_PRESSURE) {
            if (trace.total_insts + static_cast<uint64_t>(instr_count) >= MAX_TRACE_INSTS) {
                // Mid-block instruction cap — a normal (bounded) end.
                trace.stop_reason = "inst_cap";
                trace_ended = true;
                block_ended = true;
                break;
            }
            if (g_alloc.next > VREG_LIMIT) {
                trace.stop_reason = "vreg_exhaust";
                aborted = true;
                break;
            }

            uint32_t inst;
            try {
                inst = emu.mem().fetch_inst(ip);
            } catch (...) {
                trace.stop_reason = "decode_fail";
                aborted = true;
                break;
            }
            DecodedInst d;
            if (!decode(d, inst)) {
                trace.stop_reason = "decode_fail";
                aborted = true;
                break;
            }

            // ── Trace-aborting instruction classes (never in a region) ──
            if (d.cls == InstClass::SVC || d.cls == InstClass::SVC_IMM) {
                trace.stop_reason = "svc";
                aborted = true;
                break;
            }
            if (d.cls == InstClass::BR || d.cls == InstClass::BLR) {
                trace.stop_reason = "indirect_br";
                aborted = true;
                break;
            }
            if (d.cls == InstClass::BL) {
                // M1 keeps calls out of traces: a region must not dispatch a
                // call edge (ROADMAP #14: "BL/BLR inside a trace ends the
                // trace"). Simpler and correct.
                trace.stop_reason = "bl";
                aborted = true;
                break;
            }

            // RET: translate it (it ends the block), then end the trace.
            if (d.cls == InstClass::RET) {
                translate_to_ir(tb.ir, d, ip);
                instr_count++;
                tb.ir.count = instr_count;
                trace.stop_reason = "ret";
                trace_ends_after_block = true;
                block_ended = true;
                break;
            }

            // Translate to IR and scan the ops this instruction appended for an
            // interpreter fallback — such an instruction can't live in a region.
            size_t op_start = tb.ir.insts.size();
            bool ends = translate_to_ir(tb.ir, d, ip);
            instr_count++;
            tb.ir.count = instr_count;
            for (size_t i = op_start; i < tb.ir.insts.size(); i++) {
                if (tb.ir.insts[i].op == IROp::CALL_INTERP) {
                    trace.stop_reason = "call_interp";
                    aborted = true;
                    break;
                }
            }
            if (aborted) break;

            // ── Branch handling ──────────────────────────────────────
            // Conditional branches: the taken target leaves the trace (recorded
            // as a side exit / back-edge), the fall-through (ip + 4) continues.
            // Target = pc + d.imm, mirroring the interpreter and translate_block
            // (ir_translate.cpp Bcond/CBZ/TBZ cases).
            bool is_cond_branch = (d.cls == InstClass::Bcond ||
                                   d.cls == InstClass::CBZ ||
                                   d.cls == InstClass::CBNZ ||
                                   d.cls == InstClass::TBZ ||
                                   d.cls == InstClass::TBNZ);
            if (is_cond_branch) {
                uint64_t target = ip + static_cast<uint64_t>(d.imm);
                tb.side_exits.push_back(
                    {target, static_cast<int>(tb.ir.insts.size() - 1)});
                block_ended = true;
                // A conditional branch whose TAKEN target is the head is the
                // loop's back-edge: end the trace HERE (this block's taken edge
                // becomes the region's Lback). Without this, a natural loop
                // whose back-edge is the taken path (GCC emits `b.ne .L3` at
                // the loop bottom) would walk out of the loop on the
                // fall-through and the trace would follow the exit instead.
                if (target == head_pc) {
                    trace.stop_reason = "b_backedge";
                    trace_ends_after_block = true;
                }
                break;
            }
            // Unconditional branch: no fall-through — the trace ends here. A
            // target == head_pc is the loop back-edge (recorded, not followed).
            if (d.cls == InstClass::B) {
                uint64_t target = ip + static_cast<uint64_t>(d.imm);
                tb.side_exits.push_back(
                    {target, static_cast<int>(tb.ir.insts.size() - 1)});
                trace.stop_reason = (target == head_pc) ? "b_backedge" : "b_exit";
                trace_ends_after_block = true;
                block_ended = true;
                break;
            }
            // Any other block-ender (translate_to_ir returned true). All the
            // terminator classes above are handled explicitly, so this is a
            // defensive catch — keep the generic contract.
            if (ends) {
                block_ended = true;
                break;
            }
            ip += 4;
        }

        if (aborted) break;
        if (instr_count == 0) break;  // nothing decoded — discard the block
        trace.total_insts += static_cast<uint64_t>(instr_count);
        trace.blocks.push_back(std::move(tb));
        if (trace_ended || trace_ends_after_block) {
            trace_ended = true;
            break;
        }
        // The block ended at `ip`; its fall-through continues the trace.
        cur_pc = ip + 4;
    }

    // ── Finalize ────────────────────────────────────────────────────
    if (!aborted && !trace_ended) {
        // Exited the outer loop at the block cap.
        trace.stop_reason = "block_cap";
        trace_ended = true;
    }
    // Only multi-block traces qualify — a 1-block trace is just the existing
    // block JIT. An aborted trace is never compiled regardless of length.
    trace.ok = (!aborted && trace.blocks.size() >= 2);
    if (trace.ok) {
        tier2_traces.fetch_add(1, std::memory_order_relaxed);
    } else if (!aborted && trace.stop_reason == nullptr) {
        // No specific reason ended a short trace — it was just too short.
        trace.stop_reason = "too_short";
    }

    if (tier2_trace_enabled()) {
        fprintf(stderr, "[tier2] trace pc=0x%llx blocks=%zu insts=%llu ok=%d stop=%s\n",
                static_cast<unsigned long long>(head_pc), trace.blocks.size(),
                static_cast<unsigned long long>(trace.total_insts), static_cast<int>(trace.ok),
                trace.stop_reason ? trace.stop_reason : "-");
        for (size_t i = 0; i < trace.blocks.size(); i++) {
            fprintf(stderr, "[tier2]   block %zu pc=0x%llx insts=%d side_exits=%zu\n",
                    i, static_cast<unsigned long long>(trace.blocks[i].pc),
                    trace.blocks[i].ir.count, trace.blocks[i].side_exits.size());
        }
    }
    return trace;
}

// ─────────────────────────────────────────────────────────────────────────
// Tier-2 Phase 1 step 3 / M1: compile a collected trace as ONE x86 function
// with ONE region-wide register allocation. See the header contract
// (frostjit.hpp:479) and ROADMAP #14 for the full design; the layout:
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
uint64_t (*FrostJIT::compile_tier2_region(Emulator& emu, const Tier2Trace& trace))(CPU*, Emulator*) {
    (void)emu;  // M1 regions never call back into guest code
    if (!code_buf_ || !trace.ok) return nullptr;
    // Regions don't compose with chain-skip: helpers like emu_slot_off()
    // branch on chain_skip_enabled() and the chain frame is a fixed 32 KB
    // unified slot that conflicts with the region's per-region frame. Under
    // chain-skip, decline (the single-block JIT handles everything).
    if (chain_skip_enabled()) return nullptr;

    const auto& blocks = trace.blocks;
    const size_t nblk = blocks.size();
    if (nblk < 2) return nullptr;

    // ── M1 shape validation ──────────────────────────────────────────
    // The region is a linear fall-through chain; every block must be
    // terminated by a conditional branch (BRCOND/BRCOND_ZERO/BRCOND_BIT)
    // with exactly ONE side exit (the taken edge). The LAST block's taken
    // edge may be the loop back-edge to the head (Lback) — anything else
    // becomes an ordinary cold exit, so a straight-line (non-looping) chain
    // of cond-branch blocks is region-qualified too. A NON-last block may
    // never target the head (a mid-trace back-edge can't be represented by
    // the single-back-edge layout).
    for (size_t i = 0; i < nblk; i++) {
        const auto& b = blocks[i];
        if (b.ir.insts.empty()) return nullptr;
        if (b.side_exits.size() != 1) return nullptr;
        int term_idx = static_cast<int>(b.ir.insts.size()) - 1;
        if (b.side_exits[0].second != term_idx) return nullptr;
        IROp term_op = b.ir.insts[term_idx].op;
        if (term_op != IROp::BRCOND && term_op != IROp::BRCOND_ZERO &&
            term_op != IROp::BRCOND_BIT) {
            return nullptr;
        }
        if (i < nblk - 1 && b.side_exits[0].first == trace.head_pc) {
            return nullptr;  // mid-trace back-edge
        }
    }
    // The last block's taken edge → the loop back-edge iff it targets the head.
    bool last_is_backedge = (blocks[nblk - 1].side_exits[0].first == trace.head_pc);

    // ── Vreg remap + IR concatenation ────────────────────────────────
    // Each block's IR used its own scratch numbering (starts at 33, the
    // walker reset g_alloc per block). Remap block i's scratch vregs into a
    // disjoint range so the whole region shares one regalloc/vreg-slot space
    // (scratch vregs are block-local SSA, so the remap is a pure bijection).
    // base[i] = the offset added to block i's scratch vregs (>= 33).
    std::vector<size_t> base(nblk, 0);
    size_t cur_max = 32;  // highest remapped vreg so far
    for (size_t i = 0; i < nblk; i++) {
        base[i] = cur_max - 32;
        size_t bmax = 32;
        for (const auto& inst : blocks[i].ir.insts) {
            if (inst.dest > bmax) bmax = inst.dest;
            if (inst.src1 > bmax) bmax = inst.src1;
            if (inst.src2 > bmax) bmax = inst.src2;
            if (inst.aux  > bmax) bmax = inst.aux;
        }
        size_t remapped_max = base[i] + bmax;
        if (remapped_max >= 4096) return nullptr;
        cur_max = remapped_max;
    }
    const int region_max_vreg = static_cast<int>(cur_max);

    struct RBlock {
        size_t   start;      // first op index in region_ir
        size_t   term;       // terminating branch op index
        uint64_t side_pc;    // taken (side-exit / back-edge) target
        uint64_t pc;         // block start PC (for exit_pc)
        size_t   inst_count; // ARM instructions in this block
    };
    std::vector<RBlock> rblocks(nblk);
    std::vector<IRInst> region_ir;
    region_ir.reserve(trace.total_insts * 2 + nblk);
    for (size_t i = 0; i < nblk; i++) {
        size_t off = base[i];
        size_t start = region_ir.size();
        for (const auto& inst : blocks[i].ir.insts) {
            IRInst c = inst;
            if (c.dest > 32 && c.dest < 4096) c.dest = static_cast<uint16_t>(c.dest + off);
            if (c.src1 > 32 && c.src1 < 4096) c.src1 = static_cast<uint16_t>(c.src1 + off);
            if (c.src2 > 32 && c.src2 < 4096) c.src2 = static_cast<uint16_t>(c.src2 + off);
            if (c.aux  > 32 && c.aux  < 4096) c.aux  = static_cast<uint16_t>(c.aux  + off);
            region_ir.push_back(c);
        }
        rblocks[i] = {start, region_ir.size() - 1, blocks[i].side_exits[0].first,
                      blocks[i].pc, static_cast<size_t>(blocks[i].ir.count)};
    }

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
    pending_flag_mat_.clear();
    chain_entry_off_ = 0;
    num_stack_slots_ = 0;
    vec_cache_reset();  // regions never use the vec/fp cache (memory paths)
    regalloc_stats_reset();
    size_t block_start = code_buf_used_;
    int clear_limit = region_max_vreg + 1;
    if (clear_limit > 4096) clear_limit = 4096;
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
        vreg_uses_.assign(4096, {});
        for (size_t i = 0; i < n; i++) {
            const IRInst& inst = region_ir[i];
            if (inst.op != IROp::LOAD_REG) {
                if (inst.src1 > 31 && inst.src1 < 4096) {
                    vreg_last_use_op_[inst.src1] = static_cast<int>(i);
                    vreg_uses_[inst.src1].push_back(static_cast<uint16_t>(i));
                }
                if (inst.src2 > 31 && inst.src2 < 4096) {
                    vreg_last_use_op_[inst.src2] = static_cast<int>(i);
                    vreg_uses_[inst.src2].push_back(static_cast<uint16_t>(i));
                }
            }
            if (inst.aux > 31 && inst.aux < 4096) {
                vreg_last_use_op_[inst.aux] = static_cast<int>(i);
                vreg_uses_[inst.aux].push_back(static_cast<uint16_t>(i));
            }
        }
        kills_per_op_.assign(n, {});
        for (int v = 32; v < 4096; v++) {
            if (vreg_last_use_op_[v] >= 0) {
                kills_per_op_[vreg_last_use_op_[v]].push_back(static_cast<uint16_t>(v));
            }
        }
        // Fold-lookahead (same guards as translate_block's pre-scan — the
        // consumer folds the const into an x86 immediate and never reads the
        // vreg's host mapping, so the IMM codegen may skip its mov).
        fold_ahead_kind_.assign(n, 0);
        for (size_t i = 0; i + 1 < n; i++) {
            const IRInst& im = region_ir[i];
            if (im.op != IROp::IMM) continue;
            if (!(im.dest > 32 && im.dest < 4096)) continue;
            if (vreg_last_use_op_[im.dest] != static_cast<int>(i + 1)) continue;
            const IRInst& nx = region_ir[i + 1];
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

    // ── Phase 2 loop-carried arch GPR pinning (back-edge regions) ─────
    // When the region's last block loops back to the head (last_is_backedge),
    // the loop-carried arch GPRs — values READ before they are WRITTEN over
    // the concatenated region IR — are PINNED to fixed callee-saved regs
    // (R12-R15) for the whole loop. The prologue preloads them once (every
    // region entry goes through the prologue; the back-edge jmp to body_start
    // skips it); STORE_REG refreshes the pin; the back-edge (Lback) NO LONGER
    // flushes all vregs, so the pinned values survive the loop in registers —
    // no store→cpu.regs[]→load round trip per iteration. The flush removal
    // is safe because regions write arch GPRs eagerly (STORE_REG) and FP/
    // vector regs eagerly (no fp/vec cache), and every block's scratch vregs
    // are remapped to DISJOINT ranges (compile_tier2_region above), so at the
    // back-edge the only live state is arch GPRs + FP regs — the flush only
    // ever stored dead scratch vregs. The pins are excluded from the
    // allocator pool (pinned_host_regs_), so nothing can claim them mid-loop.
    {
        for (int i = 0; i < 32; i++) arch_pin_[i] = -1;
        pinned_host_regs_ = 0;
        keep_store_dest_ = false;
if (last_is_backedge && !getenv("BIFROST_NO_PIN")) {
            bool has_call = false;
            for (const IRInst& inst : region_ir) {
                if (inst.op == IROp::SVC || inst.op == IROp::BR ||
                    inst.op == IROp::BL_CALL || inst.op == IROp::BLR_CALL ||
                    inst.op == IROp::CALL_INTERP) {
                    has_call = true;
                    break;
                }
            }
            if (!has_call) {
                uint16_t first_write[32];
                uint16_t first_read[32];
                bool direct_write[32];
                for (int i = 0; i < 32; i++) {
                    first_write[i] = 0xFFFF;
                    first_read[i] = 0xFFFF;
                    direct_write[i] = false;
                }
                size_t n = region_ir.size();
                for (size_t i = 0; i < n; i++) {
                    const IRInst& inst = region_ir[i];
                    if (inst.op == IROp::STORE_REG) {
                        if (inst.dest <= 30 && first_write[inst.dest] == 0xFFFF)
                            first_write[inst.dest] = static_cast<uint16_t>(i);
                        if (inst.src1 <= 30 && first_read[inst.src1] == 0xFFFF)
                            first_read[inst.src1] = static_cast<uint16_t>(i);
                    } else if (inst.op == IROp::LOAD_REG) {
                        if (inst.src1 <= 30 && first_read[inst.src1] == 0xFFFF)
                            first_read[inst.src1] = static_cast<uint16_t>(i);
                    } else if (inst.dest <= 30 &&
                               (inst.op == IROp::CSEL || inst.op == IROp::CSINC ||
                                inst.op == IROp::CSINV || inst.op == IROp::CSNEG ||
                                inst.op == IROp::UBFM || inst.op == IROp::SBFM ||
                                inst.op == IROp::FP_F2I || inst.op == IROp::FP_F2I_FIXED ||
                                inst.op == IROp::FMOV_F2G || inst.op == IROp::FMOV_FHI2G ||
                                inst.op == IROp::SIMD_UMOV ||
                                inst.op == IROp::LOAD_MEM || inst.op == IROp::ATOMIC)) {
                        // Direct arch-GPR writers (store_reg_to_vreg /
                        // set_vreg_reg), bypassing STORE_REG — the pin would
                        // hold a stale value. LOAD_MEM/ATOMIC write the
                        // loaded result into the dest vreg directly
                        // (set_vreg_reg(inst.dest, RAX) / store_reg_to_vreg),
                        // so a pinned vreg's value would land in a plain
                        // scratch register that the NEXT iteration's code
                        // freely clobbers — the pin is abandoned and the
                        // loop-carried value is lost (CoreMark region
                        // corruption: wrong CRCs). EXPLICIT list, NOT a
                        // dest<=30 catch-all: branch ops carry a dummy
                        // dest=0 that would wrongly flag x0.
                        direct_write[inst.dest] = true;
                    }
                }
                int n_pins = 0;
                // Loop-carried vregs (read before first write, or loop-
                // invariant reads) get pinned — the value persists across the
                // back-edge in the register. Written-then-read vregs are NOT
                // pinned (measured ~6% slower on bench_mips: the pin occupies
                // a scratch register for the whole loop and the body's
                // LOAD_MEM needs the spare register).
                for (int a = 0; a <= 30 && n_pins < NUM_PIN_REGS; a++) {
                    if (direct_write[a]) continue;
                    if (first_read[a] == 0xFFFF) continue;  // never read — dead
                    if (first_write[a] != 0xFFFF && first_write[a] < first_read[a])
                        continue;  // written before first read — not carried
                    arch_pin_[a] = PIN_REGS[n_pins];
                    pinned_host_regs_ |= (1u << PIN_REGS[n_pins]);
                    n_pins++;
                }
                keep_store_dest_ = true;
            }
        }
    }

    // ── Prologue (mirror translate_block) ─────────────────────────────
    emit_push(RBX); emit_push(RBP); emit_push(R12);
    emit_push(R13); emit_push(R14); emit_push(R15);
    emit_byte(0x48); emit_byte(0x89); emit_byte(0xE5);  // mov rbp, rsp
    emit_byte(0x48); emit_byte(0x81); emit_byte(0xEC);
    emit_u32(stack_bytes);                              // sub rsp, stack_bytes
    emit_byte(0x48); emit_byte(0x89); emit_byte(0xFB);  // mov rbx, rdi (CPU)
    emit_store(RBP, emu_slot_off(), RSI);               // stash Emulator*
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
                default:
                    break;
            }
            if (uses_window) break;
        }
        if (window_base_ && uses_window)
            emit_mov_imm64(WIN_REG, reinterpret_cast<uint64_t>(window_base_));
    }
    // No vec/fp-cache prologue loads (regions never activate the caches).
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
    jit_consts_.clear();
    size_t blk = 0;
    for (size_t i = 0; i < region_ir.size(); i++) {
        while (blk < nblk && i > rblocks[blk].term) blk++;
        const IRInst& inst = region_ir[i];
        cur_op_index_ = i;

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
                    if (!flags_in_host_) {
                        constexpr uint16_t FLAGS3 =
                            (1u << RAX) | (1u << RCX) | (1u << RDX) | (1u << R8);
                        flush_dirty_host_regs(FLAGS3);
                        flush_scratch_host_regs(FLAGS3);
                        emit_load_flags_from_pstate();
                        emit_normalize_cf_to_sub_convention();
                        invalidate_host_regs(FLAGS3);
                        flags_from_sub_ = true;  // CF is now in SUB convention
                        flags_in_host_ = true;
                    }
                    uint8_t cc = resolve_arm_cond_with_carry(inst.cond, need_cmc);
                    if (need_cmc) emit_byte(0xF5);  // cmc — invert CF for HI/LS
                    jcc = emit_jcc_rel32_placeholder(cc);
                    break;
                }
                case IROp::BRCOND_ZERO: {
                    clobber_flags();
                    if (reg_vreg_[RAX] >= 0) drop_vreg(reg_vreg_[RAX]);
                    int s1 = ensure_vreg(inst.src1, RAX);
                    if (s1 != RAX) emit_mov_reg(RAX, s1);
                    if (reg_vreg_[RAX] >= 0) drop_vreg(reg_vreg_[RAX]);
                    emit_test_reg(RAX, RAX);
                    uint8_t cc = (inst.cond == 0) ? 4 /*JE*/ : 5 /*JNE*/;
                    jcc = emit_jcc_rel32_placeholder(cc);
                    break;
                }
                case IROp::BRCOND_BIT: {
                    clobber_flags();
                    if (reg_vreg_[RAX] >= 0) drop_vreg(reg_vreg_[RAX]);
                    int s1 = ensure_vreg(inst.src1, RAX);
                    if (s1 != RAX) emit_mov_reg(RAX, s1);
                    if (reg_vreg_[RAX] >= 0) drop_vreg(reg_vreg_[RAX]);
                    emit_byte(rex(true, false, false, RAX >= 8));
                    emit_byte(0x0F); emit_byte(0xBA);
                    emit_byte(modrm(3, 5, RAX & 7));
                    emit_byte(inst.width);  // bit number
                    uint8_t cc = (inst.cond == 0) ? 3 /*JNC*/ : 2 /*JC*/;
                    jcc = emit_jcc_rel32_placeholder(cc);
                    break;
                }
                default:
                    code_buf_used_ = block_start;
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
            // happens (CALL_INTERP / BL / SVC / BR abort the trace), so this
            // is defensive only.
            code_buf_used_ = block_start;
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
    // guest PC right after the last block (the walker stopped there).
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
        for (int a = 0; a <= 30; a++) {
            if (arch_pin_[a] >= 0) emit_store_arm(a, arch_pin_[a]);
        }
    };
    uint64_t exit_pc = rblocks[nblk - 1].pc + rblocks[nblk - 1].inst_count * 4;
    clobber_flags();
    flush_all_vregs();
    emit_flush_all_pins();
    emit_mov_imm_to_rax(exit_pc);
    emit_store(CPU_REG, PC_OFF, RAX);
    emit_mov_reg(RDI, CPU_REG);        // mov rdi, rbx
    emit_load(RSI, RBP, emu_slot_off());  // rsi = emu
    emit_byte(0x48); emit_byte(0x89); emit_byte(0xEC);  // mov rsp, rbp
    emit_pop(R15); emit_pop(R14); emit_pop(R13);
    emit_pop(R12); emit_pop(RBP); emit_pop(RBX);
    emit_ret();

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
        emit_mov_imm_to_rax(c.side_pc);
        emit_store(CPU_REG, PC_OFF, RAX);
        emit_mov_reg(RDI, CPU_REG);
        emit_load(RSI, RBP, emu_slot_off());
        emit_byte(0x48); emit_byte(0x89); emit_byte(0xEC);  // mov rsp, rbp
        emit_pop(R15); emit_pop(R14); emit_pop(R13);
        emit_pop(R12); emit_pop(RBP); emit_pop(RBX);
        emit_ret();
    }

    // ── Loop back-edge (Lback): the last block's taken edge ─────────
    // Only emitted when the last block's taken target is the head (a hot
    // loop): flush loop-carried state to cpu.regs[]/stack, then jmp to the
    // region body start. block0's body was compiled with a fresh regalloc
    // state (nothing mapped), so it reloads arch vregs from cpu.regs[] and
    // scratch vregs from their stack slots — the flush makes exactly that
    // state current. Materialize pstate on the back-edge only when the loop
    // top reads it.
    size_t lback_off = code_buf_used_;
    if (last_is_backedge && has_back_snap) {
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
        // eagerly (STORE_REG) and FP/vector regs eagerly (no fp/vec cache),
        // and each block's scratch vregs occupy DISJOINT remapped ranges, so
        // at the back-edge the only live values are the arch GPRs (pins +
        // cpu.regs[] always current) — the flush only ever stored dead
        // scratch vregs. Pinned arch vregs survive in R12-R15 and block0's
        // body (compiled with the pin mappings pre-established) reuses them.
    }
    if (last_is_backedge) {
        int32_t rel = static_cast<int32_t>(body_start - (code_buf_used_ + 5));
        emit_byte(0xE9);
        emit_u32(static_cast<uint32_t>(rel));  // jmp body_start
    }

    // ── Patch the taken JCCs to their targets ────────────────────────
    // Each block k's taken JCC was emitted as a forward placeholder
    // (0F 8x rel32, 6 bytes) at cold[k].jcc_patch (k < nblk-1), the last
    // block's last_jcc (back-edge → Lback), or the final cold entry's
    // jcc_patch (linear region). The runtime taken path jumps straight to
    // its deferred exit, skipping all the not-taken inline body between.
    for (size_t k = 0; k < nblk; k++) {
        size_t target_off;
        size_t jcc;
        if (k + 1 < nblk) {
            target_off = cold_off[k];
            jcc = cold[k].jcc_patch;
        } else if (last_is_backedge) {
            target_off = lback_off;
            jcc = last_jcc;
        } else {
            target_off = cold_off.back();  // the last block's own cold exit
            jcc = cold.back().jcc_patch;
        }
        int64_t rel = static_cast<int64_t>(code_buf_ + target_off - (code_buf_ + jcc + 6));
        patch_jcc_rel32(jcc, static_cast<int32_t>(rel));
    }

    // ── Publish the region ───────────────────────────────────────────
    size_t size = code_buf_used_ - block_start;
    make_executable();
    prev_max_vreg_ = max_vreg_;  // next translate_block's clear_limit (jit_translate.cpp:326)
    auto fn = reinterpret_cast<uint64_t (*)(CPU*, Emulator*)>(code_buf_ + block_start);
    if (code_buf_overflow_ || size == 0) return nullptr;
    tier2_regions++;
    if (tier2_trace_enabled()) {
        fprintf(stderr,
                "[tier2] region pc=0x%llx blocks=%zu insts=%llu bytes=%zu back_flags_carried=%d\n",
                static_cast<unsigned long long>(trace.head_pc), nblk,
                static_cast<unsigned long long>(trace.total_insts), size,
                static_cast<int>(region_flags_loop_carried));
    }
    return fn;
}

// ─────────────────────────────────────────────────────────────────────────
// Tier-2 Phase 1 step 4: IN-CODE hot-head firing.
//
// The dispatch-side counters (run_block slow path / lookup_call_target) can
// never fire on real workloads: hot loops run as chained jmp sequences (or
// inside jit_call_helper's dispatch loop), so only the loop's entry block
// reaches the dispatcher a couple of times and its inline-cache slot then
// self-pins. Block prologues therefore carry an execution counter that
// counts EVERY entry — cold dispatch AND chain edges — and once it crosses
// tier2_hits_threshold() calls tier2_fire_stub() from inside the running
// block. This compiles the head's trace as a region and force-patches the
// loop's back-edge taken-chain slot so the NEXT iteration jumps into the
// region instead of the old per-block functions.
void tier2_fire_stub(Emulator* emu, uint64_t pc) {
    if (!emu) return;
    FrostJIT* jit = emu->jit();
    if (!jit) return;
    jit->tier2_fire_region(*emu, pc);
}

void FrostJIT::tier2_counter_disable(size_t counter_off, size_t counter_len) {
    if (!counter_off || counter_len < 6) return;
    // Overwrite the leading `inc dword[rip+disp]` (6 bytes) with
    // `jmp rel32` over the whole counter+fire sequence. The block keeps
    // paying a single taken branch (~1-2 cycles) instead of the
    // inc+cmp+jne+fire fetch on every entry. Callers hold the exclusive
    // lock; tier-2 only runs in non-W^X mode (RWX buffer), so the write is
    // safe while other threads execute the block (a torn byte would only
    // land in the skipped region — the jmp is written atomically-ish as the
    // first byte + rel; x86 fetch of a torn JMP could theoretically see a
    // stale displacement, but the OLD bytes at the target of that jmp are
    // the dead counter sequence, and this runs once per block).
    size_t off = counter_off;
    if (off + 6 > CODE_BUF_SIZE) return;
    const int32_t rel = static_cast<int32_t>(static_cast<int64_t>(counter_len) - 5);
    code_buf_[off] = 0xE9;
    memcpy(code_buf_ + off + 1, &rel, 4);
    std::atomic_thread_fence(std::memory_order_release);
}

void FrostJIT::tier2_fire_region(Emulator& emu, uint64_t pc) {
    // Runs from INSIDE a generated block (prologue fire path). Never let an
    // exception cross the JIT frame boundary (std::terminate).
    try {
        // W^X bail: the in-code counter writes the code page (a non-W^X RWX
        // buffer), and the compile here would mprotect the page we're
        // currently executing from. Regions only exist in shared-JIT mode.
        if (wex_enabled_ || !tier2_enabled()) return;
        blocks_mutex_.lock();
        auto it = blocks_.find(pc);
        if (it == blocks_.end()) {
            blocks_mutex_.unlock();
            return;
        }
        BlockEntry old_entry = it->second;
        if (old_entry.tier2_hot_logged) {
            blocks_mutex_.unlock();
            return;
        }
        // Mark hot-logged BEFORE attempting collection/compilation: the same
        // block always produces the same trace shape, so a failed attempt
        // would fail identically on every re-fire. One shot per block.
        it->second.tier2_hot_logged = true;
        tier2_counter_disable(it->second.tier2_counter_off,
                              it->second.tier2_counter_len);
        tier2_hot_heads.fetch_add(1, std::memory_order_relaxed);

        Tier2Trace trace = collect_tier2_trace(emu, pc);  // read-only, we hold the lock
        uint64_t (*rfn)(CPU*, Emulator*) = trace.ok ? compile_tier2_region(emu, trace) : nullptr;
        if (rfn) {
            if (tier2_trace_enabled()) {
                fprintf(stderr,
                        "[tier2] in-code region fired pc=0x%llx blocks=%zu insts=%llu\n",
                        static_cast<unsigned long long>(pc), trace.blocks.size(),
                        static_cast<unsigned long long>(trace.total_insts));
            }
            BlockEntry region_entry;
            region_entry.fn = rfn;
            region_entry.instr_count = static_cast<int>(trace.total_insts);
            region_entry.exec_count = old_entry.exec_count;
            region_entry.tier2_hot_logged = true;
            region_entry.ends_with_branch = true;
            region_entry.chained = true;        // region has no chain slot
            region_entry.taken_chained = true;
            region_entry.interp_only = false;
            region_entry.verified_once = true;  // M1: no region verify
            it->second = region_entry;

            // ── Re-patch the loop back-edge into the region ──────────
            // Every trace block whose side exit targets the head is a
            // back-edge block; its taken chain slot currently `jmp`s to the
            // OLD head fn (or is still unpatched `ret`). Overwrite it with
            // `jmp region-fn` unconditionally (patch_chain refuses already-
            // patched slots — its 0xC3/0x90 guard). The taken-path epilogue
            // set RDI=cpu/RSI=emu right before the slot, and the region
            // prologue expects exactly that, so the hijack is transparent.
            // A block whose taken slot is still unpatched is left alone —
            // try_chain_block later chains it to the region (blocks_[pc].fn
            // is the region now), and until then the old head fn handles one
            // extra iteration then lands in the region via ITS back-edge.
            for (const auto& tb : trace.blocks) {
                if (tb.side_exits.empty() || tb.side_exits[0].first != pc) continue;
                auto bit = blocks_.find(tb.pc);
                if (bit == blocks_.end() || !bit->second.has_taken_chain_slot) continue;
                size_t off = bit->second.taken_chain_patch_off;
                if (off + 5 > CODE_BUF_SIZE) continue;
                make_writable();  // no-op in non-W^X shared-JIT mode (we bailed above)
                int32_t rel = static_cast<int32_t>(reinterpret_cast<const uint8_t*>(rfn) -
                                                   (code_buf_ + off + 5));
                code_buf_[off] = 0xE9;
                memcpy(code_buf_ + off + 1, &rel, 4);
                std::atomic_thread_fence(std::memory_order_release);
                make_executable();
                if (tier2_trace_enabled()) {
                    fprintf(stderr, "[tier2]   back-edge %llx → region (chain slot @0x%zx)\n",
                            static_cast<unsigned long long>(tb.pc), off);
                }
            }
        }
        blocks_mutex_.unlock();
    } catch (...) {
        // Never let a collect/compile/registration failure escape a JIT frame.
        if (blocks_mutex_.try_lock()) blocks_mutex_.unlock();
    }
}

}  // namespace arm64emu