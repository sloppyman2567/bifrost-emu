// Tier-2 bounded loop discovery, region optimization and publication.
// Conditional branches follow measured hot successors; direct jumps and capped
// straight-line blocks continue inline. Collection emits no host code.
// Unsupported instructions stop before their block; complete prefixes remain
// eligible without crossing a fault/call boundary.
//
// Regalloc contract (IMPORTANT): translate_to_ir allocates scratch vregs from
// the thread_local g_alloc (vregs 0-32 = arch regs, scratch >= 33).
// translate_block resets it per block via ir_reset_vreg_alloc(); this walker
// does the same, so collection never consumes the global allocator. Each
// block's IRBlock.insts is self-contained (scratch numbering reused across
// blocks is fine — the region compiler remaps/re-allocates later). A guard
// before each instruction aborts the trace if the current block's allocation
// approaches the 4095-entry vreg table.
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

Tier2Trace FrostJIT::collect_tier2_trace(Emulator& emu, uint64_t head_pc) {
    Tier2Trace trace;
    trace.head_pc = head_pc;

    // Small hot superblocks bound compilation latency and spill pressure.
    const auto limit = tier2_block_limits_.find(head_pc);
    const unsigned MAX_TRACE_BLOCKS = limit == tier2_block_limits_.end()
        ? dbg().jit_superblock_limit : std::min(dbg().jit_superblock_limit, limit->second);
    constexpr uint64_t MAX_TRACE_INSTS = 256;
    // Vreg ceiling: guard before allocating (the per-block scratch space is
    // bounded by the 4095-entry uint16_t vreg table).
    constexpr uint16_t  VREG_LIMIT = 4000;
    // Same block-size cap as translate_block.
    const int max_block_insts = dbg().jit_block_limit;

    // Follow fall-through and direct-jump edges. Only a return to the head
    // closes a region loop; other cycles are bounded by the visited set.
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

        // Do not build overlapping regions through an already optimized
        // head. Entering its published region retains that optimization and
        // avoids spending code-cache space on rotated copies of the loop.
        const auto cached = blocks_.find(cur_pc);
        if (cur_pc != head_pc && cached != blocks_.end() && cached->second.is_region) {
            trace.stop_reason = "region_entry";
            aborted = true;
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
        bool trace_ends_after_block = false;
        uint64_t next_pc = cur_pc;

        while (!block_ended && instr_count < max_block_insts &&
               !block_translation_budget_reached(tb.ir, instr_count, g_alloc.next)) {
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
            static const bool no_callregion_ =
                (getenv("BIFROST_NO_CALLREGION") != nullptr);
            if (d.cls == InstClass::SVC || d.cls == InstClass::SVC_IMM) {
                trace.stop_reason = "svc";
                aborted = true;
                break;
            }
            if (d.cls == InstClass::BR) {
                // An indirect `br xN` is a data-dependent JUMP (RET is its own
                // InstClass) — the walker can't follow it.
                trace.stop_reason = "indirect_br";
                aborted = true;
                break;
            }
            if (d.cls == InstClass::BL) {
                if (!no_callregion_ &&
                    lookup_only_locked(ip + static_cast<uint64_t>(d.imm)) != nullptr) {
                    // Call-aware region: a BL is a direct CALL. The callee is
                    // NOT part of the trace — the region's BL_CALL codegen
                    // invokes the (already-translated) standalone callee fn
                    // via a patchable `call rel32`, and the trace continues at
                    // the caller's fall-through (ip+4). Requiring the callee
                    // pre-translated means the direct-call slot patches
                    // immediately; an untranslated callee would bounce through
                    // jit_call_helper → INCOMPLETE unwind → dispatcher on
                    // every iteration, which is worse than no region.
                    // BIFROST_NO_CALLREGION=1 restores the M1 abort.
                } else {
                    trace.stop_reason = "bl_untranslated";
                    aborted = true;
                    break;
                }
            }
            if (d.cls == InstClass::BLR && no_callregion_) {
                // BLR (function pointer): the target is dynamic, so no
                // pre-translation check is possible. BLR_CALL codegen routes
                // through jit_call_helper, which runs the whole callee and
                // returns the caller's continuation — correct from inside a
                // region, so BLR is allowed by default (Track 1; the worldgen
                // noise path is blr-heavy). BIFROST_NO_CALLREGION=1 restores
                // the M1 abort.
                trace.stop_reason = "indirect_br";
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
                if (tb.ir.insts[i].op == IROp::CALL_INTERP ||
                    tb.ir.insts[i].op == IROp::SIMD_NATIVE_STRUCT) {
                    // Structured memory helpers return a dynamic PC on a
                    // guest fault and end the Tier-1 block. Regions must
                    // not continue through their fault/signal boundary.
                    trace.stop_reason = tb.ir.insts[i].op == IROp::CALL_INTERP
                        ? "call_interp" : "structured_memory";
                    aborted = true;
                    break;
                }
            }
            if (aborted) break;

            // ── Branch handling ──────────────────────────────────────
            // Conditional branches: the taken target leaves the trace (recorded
            // as a side exit / back-edge), the fall-through (ip + 4) continues.
            // Target = pc + d.imm, mirroring the interpreter and translate_block
            // (Integer.cpp Bcond/CBZ/TBZ cases).
            bool is_cond_branch = (d.cls == InstClass::Bcond ||
                                   d.cls == InstClass::CBZ ||
                                   d.cls == InstClass::CBNZ ||
                                   d.cls == InstClass::TBZ ||
                                   d.cls == InstClass::TBNZ);
            if (is_cond_branch) {
                uint64_t target = ip + static_cast<uint64_t>(d.imm);
                uint64_t side_pc = target;
                auto profile = tier2_profiles_by_pc_.find(ip);
                uint64_t taken = 0, fallthrough = 0;
                if (profile != tier2_profiles_by_pc_.end()) {
                    taken = profile->second->taken.load(std::memory_order_relaxed);
                    fallthrough = profile->second->fallthrough.load(std::memory_order_relaxed);
                }
                const uint64_t samples = taken + fallthrough;
                const bool profiled = samples >= 16;
                trace.fully_profiled &= profiled;
                // Keep a head back-edge as the loop terminator. Otherwise
                // linearize a strongly biased taken successor and make the
                // original fall-through the cold exit by inverting the IR.
                const bool follow_taken = profiled && taken * 4 >= samples * 3 &&
                    target != head_pc && (d.cls != InstClass::Bcond || d.cond < 14) &&
                    std::find(visited.begin(), visited.end(), target) == visited.end();
                if (follow_taken) {
                    IRInst& terminal = tb.ir.insts.back();
                    if (terminal.op == IROp::BRCOND) {
                        auto p = terminal.brcond_params(); p.cond ^= 1; p.target = ip + 4;
                        terminal = IRInst::make_brcond(p, ip);
                    } else if (terminal.op == IROp::BRCOND_ZERO) {
                        auto p = terminal.brcond_zero_params(); p.cond ^= 1; p.target = ip + 4;
                        terminal = IRInst::make_brcond_zero(terminal.src1, p, ip);
                    } else {
                        auto p = terminal.brcond_bit_params(); p.cond ^= 1; p.target = ip + 4;
                        terminal = IRInst::make_brcond_bit(terminal.src1, p, ip);
                    }
                    tb.follows_taken = true;
                    tb.hot_successor = target;
                    next_pc = target;
                    side_pc = ip + 4;
                }
                if (profiled) {
                    const uint64_t continued = (follow_taken || target == head_pc)
                        ? taken : fallthrough;
                    trace.path_probability *= static_cast<double>(continued) / samples;
                }
                tb.side_exits.push_back(
                    {side_pc, static_cast<int>(tb.ir.insts.size() - 1)});
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
            // Direct jumps carry no side exit when their target is the next
            // collected block. Follow them, including non-contiguous bodies;
            // never follow a cycle that bypasses the region head.
            if (d.cls == InstClass::B) {
                uint64_t target = ip + static_cast<uint64_t>(d.imm);
                tb.side_exits.push_back(
                    {target, static_cast<int>(tb.ir.insts.size() - 1)});
                next_pc = target;
                if (target == head_pc) {
                    trace.stop_reason = "b_backedge";
                    trace_ends_after_block = true;
                } else if (std::find(visited.begin(), visited.end(), target) != visited.end()) {
                    trace.stop_reason = "revisit";
                    trace_ends_after_block = true;
                }
                block_ended = true;
                break;
            }
            // A disabled in-block call can lower to a block-ending jump.
            // Do not mistake its callee target for a straight-line cap.
            if (ends) {
                trace.stop_reason = "unsupported_terminator";
                aborted = true;
                break;
            }
            ip += 4;
        }

        if (aborted) break;
        if (instr_count == 0) break;  // nothing decoded — discard the block
        if (tb.side_exits.empty() && !trace_ends_after_block) {
            // The cap stopped before translating ip. Preserve the exact
            // continuation without pretending a guest branch was executed.
            next_pc = ip;
            emit_brcond_fallthru(tb.ir, next_pc, ip);
            tb.side_exits.push_back({next_pc, static_cast<int>(tb.ir.insts.size() - 1)});
        } else if (next_pc == cur_pc) {
            next_pc = ip + 4;
        }
        // Mirror translate_block: run the IR optimizer on each block BEFORE
        // concatenation. Without it the region IR keeps every
        // STORE_REG→LOAD_REG round-trip that optimize_ir's Pass 1 (store-load
        // forwarding) would collapse into a single vreg — an invariant chain
        // threaded through a guest GPR then stays unhoistable to LICM (the
        // STORE_REG is an arch write, the LOAD_REG reads written state), so
        // the region's preheader can never absorb it. Same guard as
        // translate_block: blocks with BRCOND_SKIP skip optimization (its DCE
        // could erase ops inside the skip region and invalidate the skip's
        // op-count imm). The term op survives optimization, so re-derive the
        // recorded side-exit op index (insts.size()-1) afterwards.
        static bool walker_no_opt_ = (getenv("BIFROST_NO_OPT") != nullptr);
        bool has_brcond_skip = false;
        for (const auto& ir_inst : tb.ir.insts) {
            if (ir_inst.op == IROp::BRCOND_SKIP) {
                has_brcond_skip = true;
                break;
            }
        }
        if (!walker_no_opt_ && !has_brcond_skip) {
            // Force FWD on for region blocks (see optimize_ir's force_fwd):
            // it collapses STORE_REG→LOAD_REG round-trips into single vregs,
            // which is what makes real guest invariant chains LICM-hoistable.
            optimize_ir(tb.ir, /*force_fwd=*/true);
            if (!tb.side_exits.empty()) {
                tb.side_exits[0].second =
                    static_cast<int>(tb.ir.insts.size() - 1);
            }
        }
        // Same per-op contract check as the tier-1 path (jit_translate):
        // runs whether or not the optimizer ran, validating what the
        // region compiler is about to consume.
        if (dbg().ir_validate) validate_ir_block(tb.ir);
        trace.total_insts += static_cast<uint64_t>(instr_count);
        trace.blocks.push_back(std::move(tb));
        if (trace_ended || trace_ends_after_block) {
            trace_ended = true;
            break;
        }
        cur_pc = next_pc;
    }

    // ── Finalize ────────────────────────────────────────────────────
    if (!aborted && !trace_ended) {
        // Exited the outer loop at the block cap.
        trace.stop_reason = "block_cap";
        trace_ended = true;
    }
    // Multi-block traces qualify for a region — and so does a 1-block
    // SELF-LOOP (its cond branch targets itself, stop=b_backedge): it gains
    // the LICM preheader + DCE + const-prop that a plain block can't have,
    // which is Phase 2's "fuse the self-loop". A 1-block LINEAR trace is
    // just the existing block JIT and stays rejected.
    bool self_loop_trace = (trace.blocks.size() == 1 &&
                            !trace.blocks.empty() &&
                            trace.blocks[0].side_exits.size() == 1 &&
                            trace.blocks[0].side_exits[0].first == head_pc);
    // An unsupported next block does not invalidate an already collected
    // prefix: its last edge exits before that instruction. Never fuse a
    // non-head cycle or overlap an existing region through this shortcut.
    const bool prefix = aborted && trace.blocks.size() >= 2 &&
        trace.stop_reason && std::strcmp(trace.stop_reason, "revisit") != 0 &&
        std::strcmp(trace.stop_reason, "region_entry") != 0;
    const bool non_head_cycle = trace.stop_reason &&
        std::strcmp(trace.stop_reason, "revisit") == 0;
    trace.ok = (!non_head_cycle && (!aborted || prefix) &&
        (trace.blocks.size() >= 2 || self_loop_trace));
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
