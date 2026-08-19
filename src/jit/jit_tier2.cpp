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
#include <vector>
namespace arm64emu {

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
        // Cold-entry correctness: a PC that's already a standalone block (other
        // than the head, which fired as a hot head) is not traceable — stop.
        if (cur_pc != head_pc && blocks_.find(cur_pc) != blocks_.end()) {
            trace.stop_reason = "cold_entry";
            trace_ended = true;
            break;
        }
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

}  // namespace arm64emu