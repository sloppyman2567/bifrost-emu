// Tier-2 region planning: shape checks, IR concatenation, region SSA,
// loop-carried pin selection, region DCE, LICM and dest-sourcing rename.
// The result feeds the region compiler (RegionCompiler.cpp).
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
#include <atomic>
#include <memory>
#include <string>
namespace arm64emu {
bool FrostJIT::plan_tier2_region(const Tier2Trace& trace, Tier2RegionMonitor* monitor_override,
                                 Tier2RegionPlan& plan) {
    // Call-aware regions (Track 1): a trace may now contain BL_CALL/BLR_CALL
    // body ops. Their codegen invokes guest callee fns (direct `call rel32`
    // for BL, jit_call_helper for BLR) that run with their OWN frames and
    // clobber all caller-saved state, so the region treats a call site as a
    // hard boundary: pins are disabled, LICM stops hoisting arch loads, and
    // the region's per-op regalloc reloads everything after the call (the
    // call's invalidate_all_vregs leaves the same clean state the region body
    // starts from). The region frame is laid out exactly like a standalone
    // block's (push rbx/rbp/r12-r15, mov rbp,rsp), so the BL_CALL
    // callee-completion guard's INCOMPLETE path (`mov rsp,rbp; pop×6; ret`)
    // correctly unwinds a mid-callee return to the C dispatcher.
    if (!code_buf_ || !trace.ok) return false;
    // Chain-skip COMPOSITION (1.5.4-alpha): under BIFROST_CHAIN_SKIP=1 the
    // region allocates the same unified kChainSkipFrameBytes frame as every
    // block, records a chain_entry label after the emu stash (before the R10
    // window load / pin preloads / preheader), and publishes it via
    // *chain_out. A chain edge is a bare jmp carrying the chain ROOT's
    // rbp/rsp, so it enters past the frame allocation and runs the body with
    // vreg slots relative to the root frame — legal because every
    // chain-skip frame has identical layout (vreg ceiling -32512 < 32 KiB,
    // fixed emu slot at -(kChainSkipFrameBytes-8)). Cold dispatches enter at
    // fn, which allocates the unified frame itself. Region exits keep the
    // `mov rsp,rbp; pop×6; ret` shape: with rbp = the root frame they unwind
    // the whole chain back to the dispatcher — exactly what a standalone
    // block's cold exit does when chained over.

    const auto& blocks = trace.blocks;
    const size_t nblk = blocks.size();
    if (nblk < 1) return false;
    if (nblk == 1) {
        // 1-block region = a SELF-LOOP: the single block's cond branch must
        // target the head (itself), giving the region an Lback + LICM
        // preheader. A 1-block LINEAR trace carries no back-edge and gains
        // nothing over the standalone block JIT — reject it.
        const auto& b0 = blocks[0];
        if (b0.side_exits.size() != 1 ||
            b0.side_exits[0].first != trace.head_pc) {
            return false;
        }
    }

    // A region is a linearized path with conditional side exits. Direct
    // jumps may be internal only when their target is the next block.
    for (size_t i = 0; i < nblk; i++) {
        const auto& b = blocks[i];
        if (b.ir.insts.empty() || b.side_exits.size() != 1) return false;
        const int term_idx = static_cast<int>(b.ir.insts.size()) - 1;
        if (b.side_exits[0].second != term_idx) return false;
        const IRInst& term = b.ir.insts[term_idx];
        if (term.op == IROp::BRCOND_FALLTHRU) {
            if (term.fallthru_params().target != b.side_exits[0].first) return false;
            if (i + 1 < nblk && b.side_exits[0].first != blocks[i + 1].pc)
                return false;
        } else if (term.op != IROp::BRCOND && term.op != IROp::BRCOND_ZERO &&
                   term.op != IROp::BRCOND_BIT) {
            return false;
        } else if (i + 1 < nblk &&
                   (b.follows_taken ? b.hot_successor : b.pc + b.ir.count * 4) !=
                       blocks[i + 1].pc) {
            return false;
        }
        if (i + 1 < nblk && b.side_exits[0].first == trace.head_pc) return false;
    }
    // The last block's taken edge → the loop back-edge iff it targets the head.
    bool last_is_backedge = (blocks[nblk - 1].side_exits[0].first == trace.head_pc);
    // LOOP REGIONS ONLY (default, 2026-08-20): a LINEAR region over a
    // walked-out loop body re-pays a full region entry (prologue + cold-exit
    // ret to the dispatcher) every iteration — strictly worse than the
    // chained standalone blocks it replaces. Measured on CoreMark: tier2
    // all-regions 3239 iters/s vs plain 3479 (-7%); loops-only 3602 (+3.5%).
    // Genuine loops whose back-edge is a TAKEN conditional or an
    // unconditional `b head` end their trace with stop=b_backedge and still
    // fuse; only truly linear runs (and walks that exit a loop through an
    // outer edge) are declined. BIFROST_T2_LINEAR=1 restores all-region
    // behavior for bisection.
    static const bool t2_linear_ = (getenv("BIFROST_T2_LINEAR") != nullptr);
    if (!last_is_backedge && !t2_linear_) return false;
    tier2_current_monitor_ = monitor_override;
    if (!monitor_override && tier2_retrains_[trace.head_pc] < 2) {
        auto monitor = std::make_unique<Tier2RegionMonitor>();
        monitor->head = trace.head_pc;
        static std::atomic<uint64_t> next_identity{1};
        monitor->identity = next_identity.fetch_add(1, std::memory_order_relaxed);
        monitor->slot = monitor->identity % CPU::TIER2_FEEDBACK_SLOTS;
        tier2_current_monitor_ = monitor.get();
        tier2_region_monitors_.push_back(std::move(monitor));
    }
    // MT regions fuse a whole loop iteration, then use a revocable edge
    // into the head. This bounds execution and avoids retaining registers
    // or LICM values across an invalidation/scheduling boundary.
    if (mt_active()) last_is_backedge = false;

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
        if (remapped_max >= 4096) return false;
        cur_max = remapped_max;
    }
    int region_max_vreg = static_cast<int>(cur_max);

    using RBlock = Tier2RegionPlan::Block;
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

    // Scalar loop SSA spans the entire region, including the backedge and
    // each conditional side exit. Architectural stores remain observable;
    // joins read that canonical state rather than depending on scratch homes
    // surviving the backend's loop boundary. Cross-block value numbering is
    // performed before destruction, with dominator-scoped availability.
    std::vector<IRInst> ssa_preheader;
    bool ssa_region = false;
    if (last_is_backedge && !dbg().no_ssa && !getenv("BIFROST_NO_OPT")) {
        bool conditional = false;
        for (const auto& b : rblocks)
            conditional |= region_ir[b.term].op != IROp::BRCOND_FALLTHRU;
        const size_t exit = nblk + 1;
        std::vector<SSASourceBlock> source(nblk + 1 + (conditional ? 1 : 0));
        source[0].successors = {1}; // distinct entry, outside the loop
        for (size_t b = 0; b < nblk; ++b) {
            auto& input = source[b + 1];
            input.ir.insts.assign(region_ir.begin() + rblocks[b].start,
                                  region_ir.begin() + rblocks[b].term + 1);
            input.successors = {b + 1 == nblk ? 1 : b + 2};
            if (region_ir[rblocks[b].term].op != IROp::BRCOND_FALLTHRU)
                input.successors.push_back(exit);
        }
        SSAFunction function;
        std::vector<SSASourceBlock> lowered;
        std::string error;
        if (build_ssa(source, function, &error)) {
            optimize_ssa(function);
            if (lower_ssa(function, lowered, &error, SSAPhiLowering::ArchitecturalState)) {
                ssa_preheader = std::move(lowered[0].ir.insts);
                region_ir.clear();
                for (size_t b = 0; b < nblk; ++b) {
                    rblocks[b].start = region_ir.size();
                    const auto& body = lowered[b + 1].ir.insts;
                    region_ir.insert(region_ir.end(), body.begin(), body.end());
                    rblocks[b].term = region_ir.size() - 1;
                }
                region_max_vreg = 32;
                auto scan = [&](const std::vector<IRInst>& instructions) {
                    for (const auto& i : instructions)
                        region_max_vreg = std::max<int>(region_max_vreg,
                            std::max({i.dest, i.src1, i.src2, i.aux}));
                };
                scan(ssa_preheader); scan(region_ir);
                ssa_region = true;
                if (tier2_trace_enabled()) {
                    size_t phis = 0;
                    for (const auto& b : function.blocks) phis += b.phis.size();
                    fprintf(stderr, "[tier2] SSA head=%llx blocks=%zu values=%zu phis=%zu\n",
                        (unsigned long long)trace.head_pc, nblk, function.variables.size() - 1, phis);
                }
            }
        }
    }

    // Region-wide "contains a call" flag (Track 1 call-aware regions). A
    // BL_CALL/BLR_CALL invokes guest code that can write ANY cpu.regs[] entry
    // and clobber all caller-saved host state, so:
    //   - pinning is disabled (a pin would hold a stale loop-carried value
    //     once the callee overwrites its cpu.regs[] slot — the pin scan below
    //     already refused call-containing regions; this is the same check
    //     hoisted so LICM can consult it too);
    //   - LICM must NOT hoist LOAD_REG of an arch that the REGION never writes
    //     (the callee may write it mid-loop, and a hoisted read would freeze
    //     the pre-call value). Pure scratch IMM/ALU chains stay hoistable —
    //     they live in the region's own frame slots, which the callee (its own
    //     frame) never touches.
    const bool has_call = [&]() {
        for (const IRInst& inst : region_ir) {
            if (inst.op == IROp::SVC || inst.op == IROp::BR ||
                inst.op == IROp::BL_CALL || inst.op == IROp::BLR_CALL ||
                inst.op == IROp::CALL_INTERP || inst.op == IROp::SIMD_NATIVE_EXTRA)
                return true;
        }
        return false;
    }();
    // Chain-skip composition declines CALL-CONTAINING regions: the
    // BL_CALL completion guard's INCOMPLETE unwind (`mov rsp,rbp; pop×6;
    // ret`) assumes rbp is the region's own frame, but a chain-entered
    // region inherits the chain ROOT's rbp — the unwind would discard the
    // live host return addresses (the documented busybox do_wait hang that
    // forced bl_call_disabled_ under chain-skip, BlockCompiler.cpp:283).
    // Call-free regions compose fully. (Under chain-skip the translator
    // already lowers guest BL to a block-ending BRCOND_FALLTHRU, so BL_CALL
    // cannot appear here anyway; this guards BLR_CALL/CALL_INTERP/SVC.)
    if (chain_skip_enabled() && has_call) return false;

    // ── Phase 2 loop-carried arch GPR pinning (back-edge regions) ─────
    // Runs BEFORE the LICM pass below: LICM consults arch_pin_ to avoid
    // hoisting a LOAD_REG of a pinned arch (the pin already makes the body
    // read free; hoisting it to a preheader slot would be a per-iteration
    // regression). Computing the pins on the pre-LICM IR is safe because LICM
    // only hoists LOAD_REGs of NON-pinned archs, so a pinned arch's read
    // stays in the body and the pin stays live; an unpinned arch whose read
    // gets hoisted simply has no pin. When the region's last block loops back
    // to the head (last_is_backedge), the loop-carried arch GPRs — values READ
    // before they are WRITTEN over the concatenated region IR — are PINNED to
    // fixed callee-saved regs (R12-R15) for the whole loop. The prologue
    // preloads them once (every region entry goes through the prologue; the
    // back-edge jmp to body_start skips it); STORE_REG refreshes the pin; the
    // back-edge (Lback) NO LONGER flushes all vregs, so the pinned values
    // survive the loop in registers — no store→cpu.regs[]→load round trip per
    // iteration. The flush removal is safe because regions write arch GPRs
    // eagerly (STORE_REG) and FP/vector regs eagerly (no fp/vec cache), and
    // every block's scratch vregs are remapped to DISJOINT ranges
    // (compile_tier2_region above), so at the back-edge the only live state
    // is arch GPRs + FP regs — the flush only ever stored dead scratch vregs.
    // The pins are excluded from the allocator pool (pinned_host_regs_), so
    // nothing can claim them mid-loop.
    {
        for (int i = 0; i < 32; i++) arch_pin_[i] = -1;
        pinned_host_regs_ = 0;
        keep_store_dest_ = false;
        if (last_is_backedge && !getenv("BIFROST_NO_PIN")) {
            if (has_call) {
                // Call-aware region: the callee can write any cpu.regs[], so a
                // pin (loop-carried in the register, cpu.regs[] possibly
                // stale/deferred) would hold a stale value after the call.
                // No pins.
            } else {
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
                    } else if (writes_arch_gpr_directly(inst)) {
                        // Bypasses STORE_REG: never pin it. LOAD_MEM/ATOMIC
                        // results would otherwise land in a scratch register
                        // the next iteration clobbers (CoreMark region CRCs).
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

    // ── M2 transforms: region DCE + loop-invariant code motion (LICM) ──
    // Run on the concatenated IR after the pin analysis above (which LICM
    // consults to avoid hoisting pinned-arch reads) and BEFORE the liveness/
    // fold analyses below, so those see the transformed IR. Term branches
    // (BRCOND/ZERO/BIT) carry a dummy dest=0 and are never removed; rblocks'
    // start/term indices are rebuilt after removals (side_pc/pc/inst_count are
    // untouched — DCE drops IR ops, never ARM instructions).
    //
    // Pure GPR ops: no flags, no memory, no arch writes beyond dest. The
    // dest>32 scratch guard automatically leaves all FP/SIMD state alone
    // (those write v_lo/v_hi[dest] with dest = the FP reg index 0-31) and all
    // arch writes, flag setters, memory ops, STORE_REG, calls and branches in
    // place.
    auto is_m2_pure_gpr = [](IROp op) -> bool {
        switch (op) {
            case IROp::IMM: case IROp::MOV:
            case IROp::ADD: case IROp::SUB: case IROp::MUL:
            case IROp::AND: case IROp::OR:  case IROp::XOR:
            case IROp::SHL: case IROp::SHR: case IROp::SAR: case IROp::ROR:
            case IROp::NOT: case IROp::NEG: case IROp::SEXT: case IROp::ZEXT:
            case IROp::CLZ: case IROp::CLS: case IROp::RBIT:
            case IROp::REV16: case IROp::REV32: case IROp::REV64:
            case IROp::UBFM: case IROp::SBFM: case IROp::EXTR: case IROp::BFM:
            case IROp::UDIV: case IROp::SDIV:
            case IROp::SMADDL: case IROp::UMADDL:
            case IROp::SMULH: case IROp::UMULH:
            case IROp::SMSUBL: case IROp::UMSUBL:
                return true;
            default:
                return false;
        }
    };

    // (a) Region DCE: drop pure GPR scratch ops whose dest is never read.
    // One pass is sufficient: removing an op only removes a DEFINITION, never
    // a use, so it cannot make another op newly-dead (a leftover dead op
    // merely computes an unused value — harmless).
    if (!getenv("BIFROST_NO_RDCE")) {
        std::vector<uint16_t> uses(4096, 0);
        for (const auto& inst : region_ir) {
            if (inst.op != IROp::LOAD_REG) {
                if (inst.src1 > 31 && inst.src1 < 4096) uses[inst.src1]++;
                if (inst.src2 > 31 && inst.src2 < 4096) uses[inst.src2]++;
            }
            if (inst.aux > 31 && inst.aux < 4096) uses[inst.aux]++;
        }
        region_ir.erase(
            std::remove_if(region_ir.begin(), region_ir.end(),
                           [&](const IRInst& inst) {
                               if (!(inst.dest > 32 && inst.dest < 4096)) return false;
                               if (!is_m2_pure_gpr(inst.op)) return false;
                               return uses[inst.dest] == 0;
                           }),
            region_ir.end());
    }

    // (b) LICM: for back-edge regions only, hoist loop-invariant pure GPR ops
    // and GPR LOAD_REG of never-written, non-pinned arch regs into a preheader
    // emitted once per region entry (the Lback jumps to body_start, past it).
    // All blocks of a back-edge region run every iteration, so "invariant" =
    // sources are never redefined in the region: scratch sources must be
    // defined by a hoisted op (forward pass, defs precede uses), arch sources
    // must be read-only LOAD_REG (sf=0) that is NOT pinned (arch_pin_ was
    // computed above; a pinned read is already free in the body, hoisting it
    // to a preheader slot would be a per-iteration regression). CSEL/CSINC/
    // CSINV/CSNEG read flags and are excluded (flags may be loop-carried).
    // IMM IS hoistable: chains like `add x12,x8,x8,lsl#1` lower to
    // IMM(1);SHL;ADD — excluding the IMM stranded the whole chain in the body
    // (only the bare LOAD_REGs hoisted, which measured ~1% SLOWER on m2loop3
    // because the body then reloaded them from slots). A hoisted IMM emits a
    // dead mov in the preheader (its consumers fold via jit_consts_ or load
    // the slot once per region entry) — negligible. The hoisted results live
    // in their pre-assigned stack slots; block0's body is compiled after a
    // flush+invalidate, so it reloads them from the slots each iteration.
    std::vector<IRInst> preheader = std::move(ssa_preheader);
    if (last_is_backedge && !getenv("BIFROST_NO_LICM")) {
        std::vector<bool> hoisted_vreg(4096, false);
        for (const auto& i : preheader) if (i.dest > 32) hoisted_vreg[i.dest] = true;
        std::vector<char> hoisted_op(region_ir.size(), 0);
        for (size_t i = 0; i < region_ir.size(); i++) {
            const IRInst& inst = region_ir[i];
            if (!(inst.dest > 32 && inst.dest < 4096)) continue;
            if (inst.op == IROp::CSEL || inst.op == IROp::CSINC ||
                inst.op == IROp::CSINV || inst.op == IROp::CSNEG)
                continue;
            if (inst.op != IROp::LOAD_REG && !is_m2_pure_gpr(inst.op)) continue;
            auto src_invariant = [&](uint16_t s) -> bool {
                if (s == 0) return true;  // no source
                if (s > 32 && s < 4096) return hoisted_vreg[s];
                return false;  // arch/XZR/SP sources on a pure op — never hoist
            };
            if (inst.op == IROp::LOAD_REG) {
                // A register read can reuse a dirty arch-vreg mapping that is
                // not represented by cpu.regs[] yet. Keep it in the body;
                // only pure scratch chains are safe to move to the preheader.
                continue;
            } else if (!src_invariant(inst.src1) ||
                       !src_invariant(inst.src2) ||
                       !src_invariant(inst.aux)) {
                continue;
            }
            hoisted_vreg[inst.dest] = true;
            hoisted_op[i] = 1;
        }
        // Refine: un-hoist a hoisted IMM or LOAD_REG with NO hoisted consumer.
        // A dead preheader mov is harmless, but a hoisted IMM that only fed
        // body ops (e.g. `mov wN,#imm` feeding a STORE_REG) or a hoisted
        // PINNED-arch LOAD_REG with no hoisted chain consumer would make the
        // body consumer load from a slot each iteration (slower than the
        // in-body mov / free pin read). Safe: any hoisted op that consumed
        // the IMM is itself a hoisted consumer, so this only ever un-hoists
        // ops nothing hoisted depends on.
        for (size_t i = 0; i < region_ir.size(); i++) {
            if (!hoisted_op[i]) continue;
            if (region_ir[i].op != IROp::IMM && region_ir[i].op != IROp::LOAD_REG) continue;
            uint16_t d = region_ir[i].dest;
            bool any_hoisted_consumer = false;
            for (size_t j = i + 1; j < region_ir.size(); j++) {
                if (!hoisted_op[j]) continue;
                const IRInst& c = region_ir[j];
                if (c.src1 == d || c.src2 == d || c.aux == d) {
                    any_hoisted_consumer = true;
                    break;
                }
            }
            if (!any_hoisted_consumer) {
                hoisted_op[i] = 0;
                hoisted_vreg[d] = false;
            }
        }
        std::vector<IRInst> body_ir;
        body_ir.reserve(region_ir.size());
        for (size_t i = 0; i < region_ir.size(); i++) {
            if (hoisted_op[i]) preheader.push_back(region_ir[i]);
            else body_ir.push_back(region_ir[i]);
        }
        region_ir.swap(body_ir);
    }

    // Rebuild rblocks start/term over the transformed IR (term ops are never
    // removed, one per block, in order).
    {
        size_t blk_idx = 0, start = 0;
        for (size_t i = 0; i < region_ir.size(); i++) {
            IROp op = region_ir[i].op;
            if (op == IROp::BRCOND || op == IROp::BRCOND_ZERO ||
                op == IROp::BRCOND_BIT || op == IROp::BRCOND_FALLTHRU) {
                rblocks[blk_idx].start = start;
                rblocks[blk_idx].term = i;
                blk_idx++;
                start = i + 1;
            }
        }
    }

    // ── M2 dest-sourcing rename (2026-08-21) ────────────────────────────
    // The ALU/shift emitters have an in-place fast path for dest == src1
    // (compute in src1's register, no alloc, no mov), but translated IR
    // mints a FRESH scratch dest for every op, so the general path
    // (alloc_reg_excluding + `mov d,s1` + op) fires almost always — on the
    // hot core_bench_list region, 64 of 343 dynamic instructions/iteration
    // were pure reg→reg movs, the single largest waste class. When src1 is
    // a SCRATCH vreg whose LAST USE is this op and dest is a scratch with
    // its (unique) def here and no earlier use, rewrite dest := src1 and
    // rename every later operand that referenced dest — the emitters' fast
    // path then computes destructively in src1's register. Safety: scratch
    // vregs are uniquely defined per block and remapped to disjoint ranges
    // (so dest has exactly one def — this op — and no use can precede it),
    // src1 dying here means nothing else reads the destroyed value, and
    // arch vregs (<=32) are excluded so cpu.regs[] homes and pins are
    // untouched. old_dest > 32 also makes operand renames unambiguous
    // against LOAD_REG's arch-index src1.
    if (!getenv("BIFROST_NO_DESTSRC")) {
        const size_t n = region_ir.size();
        auto is_destsrc_op = [](IROp op) -> bool {
            switch (op) {
                case IROp::ADD: case IROp::SUB: case IROp::AND:
                case IROp::OR:  case IROp::XOR: case IROp::MUL:
                case IROp::SHL: case IROp::SHR: case IROp::SAR:
                case IROp::ROR:
                    return true;
                default:
                    return false;
            }
        };
        // Last-use index per vreg and def/use counts (operands: src1 except
        // on LOAD_REG where it is the arch index, plus src2 and aux).
        std::vector<int> last_use(4096, -1);
        std::vector<int> ndefs(4096, 0);
        std::vector<int> first_use_idx(4096, -1);
        for (size_t i = 0; i < n; i++) {
            const IRInst& inst = region_ir[i];
            uint16_t ops[3] = {inst.src1, inst.src2, inst.aux};
            for (int k = 0; k < 3; k++) {
                uint16_t s = ops[k];
                if (s < 33 || s >= 4096) continue;
                if (k == 0 && inst.op == IROp::LOAD_REG) continue;  // arch index
                last_use[s] = static_cast<int>(i);
                if (first_use_idx[s] < 0) first_use_idx[s] = static_cast<int>(i);
            }
            if (inst.dest > 32 && inst.dest < 4096) ndefs[inst.dest]++;
        }
        int renamed = 0;
        for (size_t i = 0; i < n; i++) {
            IRInst& inst = region_ir[i];
            if (!is_destsrc_op(inst.op)) continue;
            if (!(inst.dest > 32 && inst.dest < 4096)) continue;
            if (!(inst.src1 > 32 && inst.src1 < 4096)) continue;
            if (inst.dest == inst.src1) continue;
            if (inst.dest == inst.src2 || inst.dest == inst.aux) continue;
            // dest: uniquely defined here, never read before its def.
            if (ndefs[inst.dest] != 1) continue;
            if (first_use_idx[inst.dest] >= 0 &&
                first_use_idx[inst.dest] < static_cast<int>(i)) continue;
            // src1 must die AT this op (it is consumed destructively).
            if (last_use[inst.src1] != static_cast<int>(i)) continue;
            const uint16_t old_dest = inst.dest;
            inst.dest = inst.src1;
            // Rename all later operand references old_dest → src1.
            for (size_t j = i + 1; j < n; j++) {
                IRInst& later = region_ir[j];
                if (later.op != IROp::LOAD_REG) {
                    if (later.src1 == old_dest) later.src1 = inst.src1;
                }
                if (later.src2 == old_dest) later.src2 = inst.src1;
                if (later.aux  == old_dest) later.aux  = inst.src1;
            }
            // src1 now inherits old_dest's readers: extend its last_use so a
            // LATER dest-sourcing rename cannot destroy the value those
            // redirected readers still need (chained-rename case).
            if (last_use[old_dest] > last_use[inst.src1]) {
                last_use[inst.src1] = last_use[old_dest];
            }
            renamed++;
        }
        if (tier2_trace_enabled() && renamed) {
            fprintf(stderr, "[tier2] destsrc: %d ops rewritten\n", renamed);
        }
    }
    plan.blocks = std::move(rblocks);
    plan.region_ir = std::move(region_ir);
    plan.preheader = std::move(preheader);
    plan.region_max_vreg = region_max_vreg;
    plan.last_is_backedge = last_is_backedge;
    plan.ssa_region = ssa_region;
    plan.has_call = has_call;
    return true;
}
}  // namespace arm64emu
