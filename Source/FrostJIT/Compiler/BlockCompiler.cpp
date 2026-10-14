// BlockCompiler.cpp — FrostJIT block translation.
//
// split out of FrostJIT.cpp. Holds the
// translate_block() method, which translates one ARM64 basic block to
// x86-64 code. The method:
//   1. Decodes ARM64 instructions via the shared decoder.
//   2. Translates each to IR ops (Integer.cpp).
//   3. Optimizes the IR (Optimize.cpp).
//   4. Compiles each IR op to x86 via compile_ir_inst() (FrostJIT.cpp
//      + Dispatch.cpp).
//   5. Patches branch targets and block-chaining slots.
//   6. Returns a function pointer to the compiled block.
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include "FrostJIT/Compiler/BlockAnalysis.hpp"
#include "System/Core/Emulator.hpp"
#include "System/Core/DebugFlags.hpp"
#include "System/Loader/DynamicLinker.hpp"
#include "FrostJIT/IR/IR.hpp"
#include "FrostJIT/IR/Builder.hpp"
#include "FrostJIT/CodeGen/Native/NativeSIMD.hpp"
#include "Generated/SIMDTable.hpp"
#include "Generated/FixedPointTable.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>
namespace arm64emu {
// BIFROST_PROF sampling-flag toggle (see EmulatorBridge.cpp).
extern thread_local bool prof_in_translate;
bool bifrost_prof_active();
struct ProfTranslateGuard {
    bool active_, saved_;
    ProfTranslateGuard() : active_(bifrost_prof_active()), saved_(prof_in_translate) {
        if (active_) prof_in_translate = true;
    }
    ~ProfTranslateGuard() { if (active_) prof_in_translate = saved_; }
};
// (end block) instead of BL_CALL (call within block).
extern thread_local bool bl_call_disabled_;
extern thread_local bool blr_call_disabled_;
// ── translate_block ───────────────────────────────────────────────
uint64_t (*FrostJIT::translate_block(Emulator& emu, uint64_t start_pc, BlockEntry* private_entry))(CPU*, Emulator*) {
    ProfTranslateGuard prof_g;
    // Public Tier-1 compile time; private Tier-2 sampling copies count as
    // Tier-2 compilation instead.
    struct CompileTimer {
        FrostJIT* jit;
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        ~CompileTimer() {
            if (!jit) return;
            const uint64_t ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start).count());
            jit->tier1_compile_ns.fetch_add(ns, std::memory_order_relaxed);
            uint64_t max = jit->tier1_compile_max_ns.load(std::memory_order_relaxed);
            while (ns > max && !jit->tier1_compile_max_ns.compare_exchange_weak(
                       max, ns, std::memory_order_relaxed)) {}
        }
    } compile_timer{private_entry ? nullptr : this};
    block_words_.clear();
    if (!code_buf_) return nullptr;
    if (code_budget_exhausted_) {
        BlockEntry fallback;
        fallback.interp_only = true;
        fallback.interp_only_count = fallback.instr_count = 1;
        fallback.verified_once = true;
        if (!private_entry) publish_or_stage(start_pc, fallback);
        code_cache_budget_fallbacks.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    // BL_CALL (native call-within-block) does NOT compose with chain-skip:
    // a callee block entered at fn() inside jit_call_helper eventually
    // reaches the chain-skip cold exit (`mov rsp,rbp; pop×6; ret`), which
    // resets rsp to the chain root's frame and discards the BL_CALL /
    // jit_call_helper return address on the stack — corrupting the host
    // call chain (busybox id hung in do_wait after printing its output).
    // Chain-skip is opt-in (the game doesn't use it), so under chain-skip
    // fall back to the block-end-at-BL behavior (BL ends the block, the
    // call edge dispatches via chain slots — the pre-1.5.3 path).
    bl_call_disabled_ = chain_skip_enabled();
    // Bisection gates: BIFROST_NO_BL_CALL / BIFROST_NO_BLR_CALL disable the
    // native call-within-block for that form only (BL/BLR falls back to a
    // plain block-end dispatch, or CALL_INTERP for BLR).
    static const bool no_bl_call_ = (getenv("BIFROST_NO_BL_CALL") != nullptr);
    static const bool no_blr_call_ = (getenv("BIFROST_NO_BLR_CALL") != nullptr);
    bl_call_disabled_ = bl_call_disabled_ || no_bl_call_;
    blr_call_disabled_ = no_blr_call_;
    // W^X: toggle the code buffer to writable before emitting x86 code.
    // (No-op if W^X is disabled or the buffer is already writable.)
    make_writable();
    current_start_pc_ = start_pc;
    code_buf_overflow_ = false;
    call_interp_branch_patches_.clear();
    branch_target_patches_.clear();
    skip_fixups_.clear();
    rax_holds_next_pc_ = false;
    flags_in_host_ = false;
    flags_from_sub_ = false;
    chain_target_pc_ = 0;
    unchainable_end_ = false;
    indirect_exit_ = false;
    has_selfloop_slot_ = false;
    selfloop_patch_off_ = 0;
    tier2_counter_off_ = 0;
    tier2_counter_len_ = 0;
    tier2_branch_profile_ = nullptr;
    has_taken_chain_slot_ = false;
    taken_chain_patch_off_ = 0;
    taken_chain_target_pc_ = 0;
    inlined_ranges_.clear();
    chain_entry_off_ = 0;
    num_stack_slots_ = 0;
    vec_cache_reset();
    regalloc_stats_reset();
    // Only clear the vreg arrays up to the previous block's max_vreg_+1,
    // not all 4096 entries. This saves ~12KB of writes per block
    // translation when blocks are small (typical: max_vreg_ ≈ 33-100).
    int clear_limit = prev_max_vreg_ + 1;
    if (clear_limit > 4096) clear_limit = 4096;
    for (int i = 0; i < clear_limit; i++) {
        vreg_home_[i] = -1;
        vreg_dirty_[i] = false;
        vreg_slot_[i] = 0;
        vreg_last_use_[i] = 0;  // reset LRU timestamps
    }
    for (int i = 0; i < NUM_HOST_REGS; i++) reg_vreg_[i] = -1;
    max_vreg_ = 0;
    dirty_host_regs_ = 0;  // reset dirty-bitmask
    // Tier-2 in-code hot-head counter (opt-in; BIFROST_TIER2=1 enables):
    // 8 bytes of counter state immediately BEFORE the fn entry (unreachable
    // data — the fn starts at block_start below, so the entry point stays the
    // prologue). The prologue emits the RIP-relative increment against this
    // slot; the counter counts EVERY entry into the block (cold dispatch AND
    // chain edges — both run the prologue) so CHAINED hot loops fire, which
    // the dispatch-side counters (run_block slow path / lookup_call_target)
    // can never see. Gated on !wex_enabled_ only (the counter writes the code
    // page — a W^X buffer would fault on the `inc`). Chain-skip COMPOSES with
    // tier-2: the reservation MUST stay active there or the
    // counter's RIP-relative inc would read/write the PREVIOUS block's last
    // 8 code bytes (layout-dependent corruption → SIGSEGV). Self-loops are
    // naturally excluded: their loop-back jumps to block_body_start_off_
    // (past the prologue), so only cold entries count (~1).
    if (tier2_enabled() && !wex_enabled_ && !mt_active_)
        emit_u64(0);  // uint32 counter + 4 bytes padding (RIP-relative inc)
    size_t block_start = code_buf_used_;
    // ── Translate ARM64 → IR ─────────────────────────────────────
    IRBlock ir_block;
    ir_block.start_pc = start_pc;
    ir_reset_vreg_alloc();
    // Grow simple blocks beyond the old fixed cap, but stop dense IR
    // before scratch/frame growth becomes excessive. Calls retain their
    // independent limits. The diagnostic cap permits matched A/B runs.
    const int max_block_insts = dbg().jit_block_limit;
    // limit the number of CALL_INTERP fallbacks per
    // block. Each CALL_INTERP invalidates all cached vregs, and each
    // subsequent clobber_flags() drops non-dirty vregs from RAX/RCX/RDX.
    // In long blocks with many CALL_INTERPs (e.g., __multf3's 82-instr
    // multiply block with ~10 CSINC/CCMP/CSEL fallbacks), this creates
    // a cascade of stale reloads that corrupt register values.
    //
    // Fix: after MAX_CALL_INTERP_PER_BLOCK interpreter fallbacks, force
    // a block boundary. The next instruction becomes the start of a new
    // block, which gets a fresh register allocator state. This trades a
    // small perf cost (one extra block dispatch per split) for
    // correctness in complex blocks.
    constexpr int MAX_CALL_INTERP_PER_BLOCK = 2;
    // invalidates all cache mappings, so too many in one block kills perf.
    // Cap at 2 — enough for small function call sequences, low enough to
    // keep register pressure manageable and avoid excessive vreg flushes.
    constexpr int MAX_BL_CALL_PER_BLOCK = 2;
    int call_interp_count = 0;
    int bl_call_count = 0;
    uint64_t cur_pc = start_pc;
    int instr_count = 0;
    bool block_ended = false;
    // leaf inlining. When a BL targets a small leaf (no calls/
    // memory/stack, ≤16 instrs), its body is translated inline instead of
    // emitting a BL_CALL — the caller block continues past the leaf's RET.
    // The inlined body may contain ONE forward conditional branch (flattened
    // via BRCOND_SKIP).
    //
    // DISABLED BY DEFAULT (opt-in via BIFROST_ENABLE_LEAF_INLINE=1): the
    // 2026-08-15 measurement was a NET LOSS. Inlining the 15-instr grad3 leaf
    // into the minecraft game's noise3 caller blocks grew the x86 ~2x (7370
    // vs 3541 bytes for the same loop region) because the leaf's FP regs
    // (s0-s3) contend with the caller's live set inside the 10-host-reg pool —
    // a BL_CALL flushes once around the call, but an inlined body spills
    // throughout. Game heightmap: 26.5ms/column → 37.4ms/column (-41%). Even
    // the ideal mirror workload (pure-ALU leaf, dedicated loop) was ~6%
    // slower. The correctness is proven (BRCOND_SKIP + the cur_pc
    // next_pc/verify/interp_only accounting), so it stays available for a
    // future regalloc change that can run the leaf with a fresh register
    // context.
    static const bool leaf_inline_enabled_ = (getenv("BIFROST_ENABLE_LEAF_INLINE") != nullptr);
    bool has_inlined_leaf = false;  // cannot use contiguous interpreter step counts
    bool has_brcond_skip = false;   // blocks with BRCOND_SKIP skip optimize_ir (op-count stability)
    uint64_t loop_tail_end = 0;
    while (!block_ended && instr_count < max_block_insts &&
           !block_translation_budget_reached(ir_block, instr_count, g_alloc.next)) {
        // ── Block splitting at known entry points ──────────────────
        if (instr_count > 0 && cur_pc >= loop_tail_end) {
            const auto known = blocks_.find(cur_pc);
            if (known != blocks_.end()) {
                const BlockEntry& tail = known->second;
                // A previously compiled tail can make a short loop depend
                // on entry order: splitting here hides its back-edge from
                // emit_selfloop_backedge. Use metadata only as a growth
                // hint; translate the actual words normally, preserving
                // code_words_, instruction counts and all complexity caps.
                // No speculative guest reads become cache dependencies.
                const bool back_to_start = tail.chain_target_pc == start_pc ||
                    (tail.has_taken_chain_slot && tail.taken_chain_target_pc == start_pc);
                if (tail.fn && !tail.interp_only && !tail.has_call && !tail.has_svc &&
                    tail.ends_with_branch && back_to_start && tail.instr_count > 0 &&
                    tail.instr_count <= max_block_insts - instr_count) {
                    loop_tail_end = cur_pc + static_cast<uint64_t>(tail.instr_count) * 4;
                } else {
                    block_profile.entry_point++;
                    chain_target_pc_ = cur_pc;
                    break;
                }
            }
        }
        uint32_t inst;
        try {
            inst = emu.mem().fetch_inst(cur_pc);
        } catch (...) { block_profile.decode_fail++; break; }
        DecodedInst d;
        if (!decode(d, inst)) { block_profile.decode_fail++; break; }
        // ── Leaf inlining (before the normal BL translation) ───────
        if (d.cls == InstClass::BL && leaf_inline_enabled_ && !bl_call_disabled_) {
            uint64_t target = cur_pc + static_cast<uint64_t>(d.imm);
            jit_analysis::LeafScanResult scan = jit_analysis::leaf_scan(emu, target);
            if (getenv("BIFROST_DEBUG_INLINE")) {
                fprintf(stderr, "[leaf] pc=0x%llx bl->0x%llx scan.size=%d has_branch=%d\n",
                        (unsigned long long)cur_pc, (unsigned long long)target,
                        scan.size, (int)scan.has_branch);
            }
            if (scan.size > 0) {
                // Translate the leaf body into a temp block first; splice
                // into ir_block only on success (keeps the LR store + partial
                // leaf out of the block if anything goes wrong).
                IRBlock leaf_block;
                leaf_block.start_pc = target;
                uint64_t lp = target;
                uint64_t lend = target + static_cast<uint64_t>(scan.size) * 4;
                int leaf_count = 0;
                bool leaf_ok = true;
                while (lp < lend && leaf_ok) {
                    uint32_t li;
                    try {
                        li = emu.mem().fetch_inst(lp);
                    } catch (...) { leaf_ok = false; break; }
                    DecodedInst ld;
                    if (!decode(ld, li)) { leaf_ok = false; break; }
                    if (ld.cls == InstClass::RET) break;  // leaf end (not translated)
                    if (ld.cls == InstClass::Bcond) {
                        // d.imm is the full relative offset in bytes
                        // (target = pc + imm, mirroring the interp).
                        uint64_t bt = lp + static_cast<uint64_t>(ld.imm);
                        size_t skip_idx = leaf_block.insts.size();
                        size_t region_start = skip_idx + 1;
                        emit_brcond_skip(leaf_block,
                                           static_cast<uint8_t>(ld.cond & 0xF),
                                           lp);
                        has_brcond_skip = true;
                        // Translate the not-taken region [lp+4, bt) inline.
                        uint64_t rp = lp + 4;
                        while (rp < bt && leaf_ok) {
                            uint32_t ri;
                            try {
                                ri = emu.mem().fetch_inst(rp);
                            } catch (...) { leaf_ok = false; break; }
                            DecodedInst rd;
                            if (!decode(rd, ri)) { leaf_ok = false; break; }
                            translate_to_ir(leaf_block, rd, rp);
                            leaf_count++;
                            rp += 4;
                        }
                        if (!leaf_ok) break;
                        // Patch the skip's region size (in IR ops) now that
                        // the region has been translated.
                        uint64_t region_ops = leaf_block.insts.size() - region_start;
                        leaf_block.insts[skip_idx].set_skip_count(region_ops);
                        leaf_count++;  // the branch instruction itself
                        lp = bt;
                        continue;
                    }
                    translate_to_ir(leaf_block, ld, lp);
                    leaf_count++;
                    lp += 4;
                }
                if (leaf_ok && leaf_count == scan.size) {
                    // Inline: LR = bl_pc+4, then the leaf body, then continue
                    // the caller at cur_pc+4.
                    uint16_t lr = load_imm(ir_block, cur_pc + 4);
                    store_arm_reg(ir_block, 30, lr);
                    ir_block.insts.insert(ir_block.insts.end(),
                                          leaf_block.insts.begin(),
                                          leaf_block.insts.end());
                    instr_count += leaf_count;
                    ir_block.count = instr_count;
                    has_inlined_leaf = true;
                    inlined_ranges_.emplace_back(target, leaf_count);
                    block_profile.leaf_inlined++;
                    if (getenv("BIFROST_DEBUG_INLINE"))
                        fprintf(stderr, "[leaf] INLINED bl@0x%llx -> 0x%llx (%d instrs)\n",
                                (unsigned long long)cur_pc, (unsigned long long)target,
                                leaf_count);
                    cur_pc += 4;  // caller continuation after the BL
                    continue;
                }
                if (getenv("BIFROST_DEBUG_INLINE"))
                    fprintf(stderr, "[leaf] REJECTED bl@0x%llx -> 0x%llx leaf_ok=%d count=%d/%d\n",
                            (unsigned long long)cur_pc, (unsigned long long)target,
                            (int)leaf_ok, leaf_count, scan.size);
                // Fall through to the normal BL_CALL path (partial leaf
                // IR is discarded — it lived only in leaf_block).
            }
        }
        bool will_call_interp = jit_analysis::instr_will_call_interp(d);
        if (will_call_interp && call_interp_count >= MAX_CALL_INTERP_PER_BLOCK && instr_count > 0) {
            // Split here — the next instruction starts a new block.
            block_profile.call_interp_cap++;
            chain_target_pc_ = cur_pc;
            break;
        }
        bool ends = translate_to_ir(ir_block, d, cur_pc);
        if (will_call_interp) call_interp_count++;
        // Track BL_CALL count for block splitting (BLR_CALL too — both
        // keep the block alive past the call and grow register pressure).
        if (d.cls == InstClass::BL || d.cls == InstClass::BLR) bl_call_count++;
        block_words_.push_back(inst);
        instr_count++;
        ir_block.count = instr_count;
        if (ends) {
            block_profile.natural_branch++;
            block_ended = true;
        }
        else {
            if (bl_call_count >= MAX_BL_CALL_PER_BLOCK) {
                block_profile.bl_call_cap++;
                chain_target_pc_ = cur_pc + 4;
                break;
            }
            cur_pc += 4;
        }
    }
    const bool size_limited = instr_count >= max_block_insts ||
        block_translation_budget_reached(ir_block, instr_count, g_alloc.next);
    if (size_limited) block_profile.max_size++;
    if (instr_count == 0) {
        make_executable();  // W^X: balance the make_writable() at entry
        return nullptr;
    }
    // ── glibc malloc/free freelist quarantine (Track A) ────────────
    // The JIT has a known register-state corruption entering glibc's
    // malloc/free safe-linked freelist code (PROTECT_PTR head reads as 0,
    // corrupting the heap). Blocks inside glibc's allocator hot region
    // run via the interpreter, which is the correct baseline. The region
    // is matched by name + file offset so it survives ASLR.
    static const bool qt_quarantine_ = (getenv("BIFROST_MALLOC_INTERP") != nullptr);
    if (qt_quarantine_) {
        if (emu.dyn_linker()) {
            uint64_t off = emu.dyn_linker()->object_relative_offset(start_pc, "libc");
            // malloc/free/calloc/realloc + internal _int_malloc/_int_free.
            if (off != ~uint64_t(0) && off >= 0x90480 && off <= 0x92800) {
                BlockEntry entry;
                entry.fn = nullptr;
                entry.interp_only = true;
                entry.interp_only_count = instr_count;
                entry.ends_with_branch = ir_block.ends_with_branch;
                entry.chain_target_pc = 0;
                entry.chained = false;
                entry.instr_count = instr_count;
                entry.verified_once = true;  // interp-only: no verify
                attach_code_words(entry, !has_inlined_leaf);
                if (!private_entry) publish_or_stage(start_pc, entry);
                blocks_translated++;
                block_profile.interp_only++;
                make_executable();  // W^X: balance the make_writable() at entry
                return nullptr;
            }
        }
    }
    // ── Heuristic: skip JIT for CALL_INTERP-heavy blocks ──────────
    // The JIT's per-CALL_INTERP overhead (flush all vregs + push 2 regs +
    // call interpreter + pop 2 regs + reload) is ~20 instructions. For
    // blocks where CALL_INTERP dominates (more than half the instructions
    // are interpreter fallbacks), the pure interpreter is faster — it
    // skips the prologue/epilogue/dispatch entirely.
    //
    // refined from "any CALL_INTERP → interp_only" to
    // "CALL_INTERP-heavy → interp_only". The old heuristic was too
    // aggressive — a block with 10 native ops and 1 CALL_INTERP would
    // skip JIT entirely, losing the 10 native ops' speedup. Now we only
    // fall back to interpreter when CALL_INTERP is the majority.
    //
    // This fixes the long-double multiply/divide hang: __multf3/__divtf3
    // are ~82-instruction soft-float routines split into ~20 tiny blocks
    // by the MAX_CALL_INTERP_PER_BLOCK=2 splitter. The JIT was 100-200x
    // slower than the interpreter for these, causing effective hangs on
    // jit_block_split.elf and jit_fp_scalar.elf with --jit.
    //
    // Interp-only blocks are cached (so we skip the re-decode cost on
    // cache hits) and run exactly instr_count interpreter steps.
    // ── interp_only decision ────────────────────────────────────────
    // Blocks whose instructions are mostly CALL_INTERP fallbacks
    // run entirely via the interpreter — cached, so the
    // decode cost is skipped on hits. The heuristic call_interp_count
    // OVER-predicts (it fires for FP_SCALAR/SIMD_DP ops that the IR
    // translator actually handles natively, e.g. FMOV/FCVT/FADD), which
    // demoted tiny native FP blocks to the interpreter (~2x slower) and
    // split FP-heavy blocks every 2 instructions. base the
    // decision on the ACTUAL number of CALL_INTERP ops in the generated
    // IR instead — only blocks that genuinely run the interpreter get
    // demoted. Native FP blocks now stay in the JIT.
    int actual_call_interp = 0;
    for (auto& ir_inst : ir_block.insts) {
        if (ir_inst.op == IROp::CALL_INTERP) actual_call_interp++;
    }
    // Inlined-leaf blocks stay in the JIT: their instr_count is NOT a
// contiguous guest-pc count (the inlined leaf body + implicit BL/RET
// break the start_pc + count*4 model), so the interp_only step-count
// semantics don't apply. The
// inlined body is native by construction (leaf scan rejects memory,
// calls, stack, and system ops); CALL_INTERP-heavy caller code still
// splits naturally via MAX_CALL_INTERP_PER_BLOCK. If the block is
// nonetheless demoted (defensive), the count below would run the wrong
// number of interpreter steps — so never demote inline blocks.
bool demote_interp = false;
if (!has_inlined_leaf) {
    demote_interp = actual_call_interp > 0 && actual_call_interp * 2 > instr_count;
}
if (demote_interp) {
        if (getenv("BIFROST_CLASS_PROF")) {
            static uint64_t dump_ct = 0;
            if (++dump_ct <= 300) {
                std::string cls_list;
                for (int k = 0; k < instr_count; k++) {
                    try {
                        uint32_t raw = emu.mem().fetch_inst(start_pc + 4ull * k);
                        DecodedInst dd;
                        decode(dd, raw);
                        char buf[64];
                        snprintf(buf, sizeof(buf), " %d:%d/%08x", k, (int)dd.cls, raw);
                        cls_list += buf;
                    } catch (...) {
                        cls_list += " X";
                    }
                }
                fprintf(stderr, "[interp_only] pc=0x%llx n=%d aci=%d heur=%d%s\n",
                        (unsigned long long)start_pc, instr_count, actual_call_interp,
                        call_interp_count, cls_list.c_str());
            }
        }
        BlockEntry entry;
        entry.fn = nullptr;
        entry.interp_only = true;
        entry.interp_only_count = instr_count;
        entry.ends_with_branch = ir_block.ends_with_branch;
        entry.chain_target_pc = 0;
        entry.chained = false;
        entry.instr_count = instr_count;
        entry.verified_once = true;  // skip verify for interp-only
        if (!private_entry) {
            attach_code_words(entry, !has_inlined_leaf);
            publish_or_stage(start_pc, entry);
        }
        blocks_translated++;
        block_profile.interp_only++;
        make_executable();  // W^X: balance the make_writable() at entry
        return nullptr;
    }
    // mode (which re-runs the interpreter for instr_count steps). With
    // BL_CALL, the JIT block executes MORE ARM instructions than
    // instr_count (the called functions run inside jit_call_helper).
    // The interpreter re-run would only do instr_count steps, missing
    // the called functions, causing false divergences. Mark as
    // verified_once to skip verify for BL_CALL blocks.
    // IR ops (not just BL instructions). When BL_CALL is disabled,
    // BL ends the block (like before), so verify is safe.
    // CALL_INTERP blocks can't be verified because the JIT flushes vregs
    // to cpu.regs[] before calling the interpreter, but the verify mode
    // restores the pre-JIT state for the interpreter re-run. This causes
    // false-positive divergences that compound and break programs like
    // curl. The JIT's output is correct — the verify mode's re-run is wrong.
    bool has_bl_call = false;
    bool has_call_interp = false;
    bool has_svc = false;
    for (auto& ir_inst : ir_block.insts) {
        if (ir_inst.op == IROp::BL_CALL || ir_inst.op == IROp::BLR_CALL) { has_bl_call = true; break; }
        if (ir_inst.op == IROp::CALL_INTERP) { has_call_interp = true; }
    }
    // SVC must be scanned in its own loop: the loop above breaks on the first
    // BL_CALL/BLR_CALL, so a block that ends `bl foo; ...; svc #0` (BL_CALL
    // does not end the block, SVC does) would leave has_svc false and let
    // verify re-execute a non-idempotent syscall on the interp re-run.
    for (auto& ir_inst : ir_block.insts) {
        if (ir_inst.op == IROp::SVC) { has_svc = true; break; }
    }
    // ── Optimize the IR ──────────────────────────────────────────
    // Skip optimization for blocks containing BRCOND_SKIP: optimize_ir's
    // DCE could erase pure-dead ops inside the skip region, which would
    // invalidate the region's op count stored in the BRCOND_SKIP imm (the
    // compile-loop fixup target is an IR op index). The inlined bodies are
    // already tight native FP/GPR code; skipping the pass costs little.
    static bool no_opt_ = (getenv("BIFROST_NO_OPT") != nullptr);
    if (!no_opt_ && !has_brcond_skip) optimize_ir(ir_block);
    // Per-op IR parameter contract check (only migrated ops; see IR.hpp).
    // Validates what codegen is about to consume, after the optimizer.
    if (dbg().ir_validate) validate_ir_block(ir_block);
    static bool dump_ir_ = (getenv("BIFROST_JIT_DUMP") != nullptr);
    // BIFROST_DUMP_PC=0x... restricts the IR dump to a single block pc
    // (useful when BIFROST_JIT_DUMP would flood with thousands of blocks).
    static uint64_t dump_pc_ = []() -> uint64_t {
        const char* s = getenv("BIFROST_DUMP_PC");
        return s ? strtoull(s, nullptr, 0) : 0;
    }();
    if ((dump_ir_ && (!dump_pc_ || start_pc == dump_pc_)) || start_pc == force_dump_pc_.load()) {
        fprintf(stderr, "══ Block @ 0x%llx (%d ARM instrs) ══\n",
                static_cast<unsigned long long>(start_pc), instr_count);
        dump_ir(ir_block);
    }
    // ── Compute stack size and pre-allocate vreg slots ────────────
    // Pre-scan IR to find all scratch vregs (33+) and assign each a
    // fixed stack slot. This avoids the lazy allocation mismatch between
    // the pre-computed stack size and the runtime slot counter.
    //
    // the previous code limited max_vreg to < 200,
    // but blocks with many ARM instructions (e.g., __multf3's 82-instr
    // block) can have vregs up to 317+. Vregs >= 200 would get stack
    // slots via lazy allocation that extend BEYOND the pre-allocated
    // stack frame, causing stack corruption.
    int max_vreg = 33;
    for (auto& inst : ir_block.insts) {
        if (inst.dest > max_vreg) max_vreg = inst.dest;
        if (inst.src1 > max_vreg) max_vreg = inst.src1;
        if (inst.src2 > max_vreg) max_vreg = inst.src2;
        // aux is the SMADDL/SMSUBL accumulator vreg. Today it is always
        // allocated BEFORE inst.dest (load_arm_reg(ra) then g_alloc.alloc()),
        // so aux < dest for the same op — but copy-substitution in
        // optimize_ir can rewrite aux, and any vreg the pre-scan misses gets
        // a LAZY slot (-8 * num_stack_slots_++) past the pre-allocated frame
        // (vreg_stack_slot in RegisterAllocator.cpp). Under chain-skip's fixed
        // 32 KB frame that writes past the frame. Defensive: cover it.
        if (inst.aux > max_vreg) max_vreg = inst.aux;
    }
    // Bounds-check: vreg arrays are fixed-size (4096). A pathological block
    // could exceed this, silently overflowing vreg_slot_[] / vreg_home_[] /
    // vreg_dirty_[]. Cap max_vreg and emit a warning to stderr the first
    // time this happens (the block will still run via CALL_INTERP fallback
    // for the overflowed vregs, which is correct but slow).
    static bool vreg_overflow_warned_ = false;
    if (max_vreg >= 4096) {
        if (!vreg_overflow_warned_) {
            fprintf(stderr, "[%s] frostJIT: vreg overflow (max=%d >= 4096) — "
                    "block will use interpreter fallback for overflowed vregs. "
                    "This is a bug; please report the guest binary.\n",
                    CODENAME, max_vreg);
            vreg_overflow_warned_ = true;
        }
        max_vreg = 4095;
    }
    // Pre-assign stack slots: vreg 33 → slot -8, vreg 34 → slot -16, etc.
    for (int v = 33; v <= max_vreg; v++) {
        vreg_slot_[v] = -8 * (v - 32);
    }
    num_stack_slots_ = max_vreg - 32;
    if (num_stack_slots_ < 1) num_stack_slots_ = 1;
    uint32_t stack_bytes = static_cast<uint32_t>(num_stack_slots_ * 8 + 64) & ~15U;
    // ── Vector register cache pre-scan ─────────────────────────────
    // Enable the XMM vector cache only if EVERY vector-touching op in the
    // block is cache-aware (SIMD_FP_FMA / SIMD_FP_ARITH) plus GPR-only ops.
    // The cache's correctness relies on never flushing mid-block, which the
    // all-or-nothing scan guarantees.
    vec_cache_may_enable(ir_block);
    // Scalar-FP cache (lo-only): only when the vec cache is NOT active for
    // this block — the two are exclusive (both pin into XMM3-15 and share
    // cpu.v_lo, so a block is either vector-cached or scalar-FP-cached).
    if (!vec_cache_active_) fp_cache_may_enable(ir_block);
    // ── Prologue ─────────────────────────────────────────────────
    emit_push(RBX); emit_push(RBP); emit_push(R12);
    emit_push(R13); emit_push(R14); emit_push(R15);
    emit_byte(0x48); emit_byte(0x89); emit_byte(0xE5); // mov rbp, rsp
emit_byte(0x48); emit_byte(0x81); emit_byte(0xEC);
    // Chain-skip (BIFROST_CHAIN_SKIP=1): allocate ONE unified 32 KB frame
    // instead of a per-block stack_bytes frame. Chain successors jump to
    // this block's body WITHOUT pushing/allocating, so they reuse this
    // block's frame — and this block may itself have been chained into
    // from a predecessor whose frame is identical. The 32 KB ceiling
    // (max_vreg 4095 × 8 B) is never exceeded: vreg_slot_ offsets for a
    // block are always ≤ its own max_vreg, and the pre-scan below caps at
    // 4095. Disabled mode keeps the old per-block frame.
    emit_u32(chain_skip_enabled() ? kChainSkipFrameBytes : stack_bytes);
    emit_byte(0x48); emit_byte(0x89); emit_byte(0xFB); // mov rbx, rdi
    // Stash the Emulator* into the frame slot (emu_slot_off(), below the
    // block's vreg slots / the chain-skip frame ceiling). R14 is then FREE
    // for vreg allocation in every block — all emu-consuming codegen sites
    // load emu from this slot instead of R14. RSI still holds
    // emu at this point (entry arg), untouched since the prologue started.
    emit_store(RBP, emu_slot_off(), RSI);
    // Chain-skip entry (BIFROST_CHAIN_SKIP=1): recorded here, BEFORE the
    // window load and the vector-cache prologue loads. Chain edges jump to
    // this offset, skipping the predecessor's frame teardown and THIS
    // block's push/frame/reg-setup (~17 instructions) while still
    // re-running the R10 window load and the vec loads. Both are required:
    // R10 is only loaded by window-using blocks, so a chain into a window
    // block from a non-window predecessor carries stale R10; and a chain
    // successor must pin the CURRENT guest vectors (the predecessor's
    // epilogue wrote its dirty vectors back to cpu.v_lo/v_hi). RBX needs no
    // setup — it holds cpu persistently across the chain edge (reserved
    // reg). R14 is allocatable, so emu does NOT flow through
    // a register: the predecessor's epilogue set RSI=emu from the shared
    // chain frame's fixed emu slot (emu_slot_off()), and a successor that
    // skips its own prologue still reads that slot. Disabled mode leaves
    // chain_entry_off_ unused (chain slots patch to fn instead).
    chain_entry_off_ = code_buf_used_;
    // BIFROST_BLOCK_TRACE: log every cold and chained entry. Like the Tier-2
    // counter below, it runs before anything is cached, so the caller-saved
    // registers are free. Self-loop iterations re-enter past the prologue
    // and are not logged.
    if (bifrost::DebugFlags::in_ranges(dbg().block_trace, start_pc)) {
        emit_mov_reg(RDI, CPU_REG);
        emit_mov_imm64(RSI, start_pc);
        emit_mov_imm64(RDX, ir_block.fwd_enabled ? 1 : 0);
        emit_call_aligned(&jit_trace_block_entry, 0);
    }
    // ── Tier-2 in-code hot-head counter (opt-in; BIFROST_TIER2=1 enables) ──
    // Emitted here (after chain_entry_off_, before the R10 window load and
    // vec prologue loads) so BOTH cold entries (falling through the prologue)
    // and chain-skip entries (jumping to chain_entry_off_) hit it — every
    // execution of the block is counted. At this point RBX=cpu/RBP=frame are
    // set and RDI/RSI are still the entry args (already stashed), so the fire
    // path can clobber the caller-saved regs freely (the window load and vec
    // loads that follow use no GPR scratch). The counter data lives 8 bytes
    // before block_start (see above); the RIP-relative disp32 is known here
    // because that data was emitted first.
    //
    // Native conditional/direct-branch and capped straight-line heads can
    // participate in a bounded loop region. Calls and fault boundaries are
    // excluded from entry instrumentation; the collector validates the body.
    bool tier2_head_eligible = false;
    // Chain-skip composes with tier-2: the counter sits AFTER
    // chain_entry_off_, so chain edges (bare jmps to chain_entry) count too.
    // Regions entered via a chain edge use the region's own chain-skip entry
    // (compile_tier2_region's chain_out); the unified 32 KB frame makes the
    // vreg-slot layouts compatible.
    if (tier2_enabled() && !wex_enabled_ && !private_entry) {
        if (!ir_block.insts.empty()) {
            IROp last_op = ir_block.insts.back().op;
            if (last_op == IROp::BRCOND || last_op == IROp::BRCOND_ZERO ||
                last_op == IROp::BRCOND_BIT || last_op == IROp::BRCOND_FALLTHRU ||
                (!block_ended && size_limited)) {
                tier2_head_eligible = true;
                for (const auto& inst : ir_block.insts) {
                    if (inst.op == IROp::SVC || inst.op == IROp::BR ||
                        inst.op == IROp::BL_CALL || inst.op == IROp::BLR_CALL ||
                        inst.op == IROp::CALL_INTERP || inst.op == IROp::SIMD_NATIVE_STRUCT) {
                        tier2_head_eligible = false;
                        break;
                    }
                }
            }
        }
    }
    if (tier2_head_eligible) {
        const IRInst& terminal = ir_block.insts.back();
        if (terminal.op == IROp::BRCOND || terminal.op == IROp::BRCOND_ZERO ||
            terminal.op == IROp::BRCOND_BIT) {
            auto profile = std::make_unique<Tier2BranchProfile>();
            tier2_branch_profile_ = profile.get();
            auto old = tier2_profiles_by_pc_.find(terminal.arm_pc);
            if (old != tier2_profiles_by_pc_.end())
                old->second->enabled.store(0, std::memory_order_release);
            tier2_profiles_by_pc_[terminal.arm_pc] = profile.get();
            tier2_branch_profiles_.push_back(std::move(profile));
        }
        size_t mt_counter_disabled_patch = 0;
        tier2_counter_off_ = code_buf_used_;
        if (mt_active()) {
            auto counter = std::make_unique<MtHotCounter>();
            MtHotCounter* state = counter.get();
            mt_hot_counters_.push_back(std::move(counter));
            mt_counter_sites_[tier2_counter_off_] = state;
            emit_mov_imm64(RAX, reinterpret_cast<uint64_t>(state));
            // cmp dword [rax],0; je past counter/fire (patched below)
            emit_byte(0x83); emit_byte(0x38); emit_byte(0);
            size_t disabled = emit_jcc_rel32_placeholder(0x4);
            emit_mov_imm64(RCX, 1);
            // lock xadd dword [rax+4],ecx: each entry obtains a unique hit.
            emit_byte(0xF0); emit_byte(0x0F); emit_byte(0xC1);
            emit_byte(0x48); emit_byte(4);
            emit_byte(0x81); emit_byte(0xF9);
            emit_u32(std::max(uint32_t{1}, tier2_hits_threshold()) - 1);
            // The disabled branch lands at the same skip as the JNE below.
            // Keep the disabled-counter fixup local to this prologue.
            mt_counter_disabled_patch = disabled;
        } else {
            const size_t data_off = block_start - 8;
            const uint32_t threshold = tier2_hits_threshold();
            // inc dword [rip+disp]         (FF 05 disp32)
            size_t inc_off = code_buf_used_;
            emit_byte(0xFF); emit_byte(0x05);
            emit_u32(static_cast<uint32_t>(static_cast<int32_t>(data_off - (inc_off + 6))));
            // cmp dword [rip+disp], imm32  (81 3D disp32 imm32)
            size_t cmp_off = code_buf_used_;
            emit_byte(0x81); emit_byte(0x3D);
            emit_u32(static_cast<uint32_t>(static_cast<int32_t>(data_off - (cmp_off + 10))));
            emit_u32(threshold);
        }
        // jne short skip                (75 rel8 — patched after the call)
        size_t jne_off = code_buf_used_;
        emit_byte(0x75); emit_byte(0x00);
        // Fire path: rdi = emu (frame slot), rsi = head pc, call the stub.
        // emit_call_aligned(…, 0) wraps emit_call_abs (movabs+call, so the
        // target is reachable regardless of the code_buf_ ↔ binary text
        // distance) in pushfq/popfq, preserving RFLAGS AND restoring the
        // ABI stack alignment for the stub (JIT body RSP%16==8 at this
        // point; pushfq makes it 16-aligned for the `call`).
        emit_load(RDI, RBP, emu_slot_off());
        emit_mov_imm64(RSI, start_pc);
        emit_mov_imm64(RDX, tier2_counter_off_);
        emit_call_aligned(reinterpret_cast<void*>(&tier2_fire_stub), 0);
        // Patch the jne to skip the fire path (land right after the call).
        const int8_t rel = static_cast<int8_t>(code_buf_used_ - (jne_off + 2));
        patch_jcc_rel8(jne_off, rel);
        if (mt_counter_disabled_patch)
            patch_jcc_rel32(mt_counter_disabled_patch, static_cast<int32_t>(code_buf_used_ - (mt_counter_disabled_patch + 6)));
        tier2_counter_len_ = code_buf_used_ - tier2_counter_off_;
    }
    // load the reservation base into R10 ONLY if the
    // block actually touches guest memory inline.
    // Previously every block paid a 10-byte movabs r10, imm64 in its
    // prologue — pure overhead for the many tiny ALU/FP/vector blocks
    // that never load or store guest memory (avg 3.5 guest instrs/block,
    // ~20M block entries/sec). The window-using IR ops are exactly:
    //   LOAD_MEM / STORE_MEM            (emit_load_mem / emit_store_mem)
    //   ATOMIC                          (direct-window lock fast path)
    //   SIMD_LD16 / SIMD_ST16           (16-byte vector memory access)
    // Everything else either never reads guest memory or calls a C
    // helper (which goes through Memory, not R10).
    {
        bool uses_window = false;
        for (const auto& inst : ir_block.insts) {
            switch (inst.op) {
                case IROp::LOAD_MEM:
                case IROp::STORE_MEM:
                case IROp::ATOMIC:
                case IROp::SIMD_LD16:
                case IROp::SIMD_ST16:
                    uses_window = true;
                    break;
                case IROp::LDXR_FAST:
                    uses_window = !inst.llsc_exclusive(); // native LDAR
                    break;
                default:
                    break;
            }
            if (uses_window) break;
        }
        if (window_base_ && uses_window)
            emit_mov_imm64(WIN_REG, reinterpret_cast<uint64_t>(window_base_));
    }
    // Vector cache prologue loads MUST be emitted before block_body_start_off_
    // is recorded: self-loop re-entry jumps directly to the body start, so
    // these loads run only on cold entry (dispatcher / chain entry) and the
    // pinned XMM regs persist as loop-carried state across iterations.
    vec_emit_prologue_loads();
    // ── Phase 2 loop-carried arch GPR pinning (self-loop blocks) ──────
    // For a self-loop block (last op BRCOND/BRCOND_ZERO/BRCOND_BIT taken to
    // our own start_pc), the loop-carried arch GPRs — values READ before they
    // are WRITTEN in each iteration — are PINNED to fixed callee-saved regs
    // (R12-R15) for the whole loop. The prologue preloads them from
    // cpu.regs[] once (cold/chain entry); STORE_REG refreshes the pin each
    // iteration; the self-loop back-edge jumps to block_body_start_off_ PAST
    // these preloads, so the pinned values survive in the registers —
    // eliminating the store→cpu.regs[]→load round trip every iteration pays
    // for its loop-carried state (bench_mips: x1/x2/x3/x4). The pinned regs
    // are excluded from the allocator pool (pinned_host_regs_), so nothing
    // else can claim them mid-loop.
    //
    // Eligibility (conservative — an un-pinned vreg is just a missed
    // optimization; an incorrectly pinned one is a miscompile):
    //   * the block actually gets the self-loop slot (is_selfloop &&
    //     !BIFROST_NO_SELFLOOP, plus chaining enabled in MT — mirrors Branch.cpp).
    //   * no call-like ops (CALL_INTERP/BL_CALL/BLR_CALL/SVC/BR): those run
    //     interpreter/guest code that writes cpu.regs[] DIRECTLY without a
    //     STORE_REG, so the pin would hold a stale value and a later
    //     LOAD_REG would read it.
    //   * the arch vreg is written ONLY via STORE_REG (FP_F2I_FIXED /
    //     FMOV_F2G write arch dests directly through store_reg_to_vreg,
    //     bypassing pin maintenance).
    //   * the arch vreg is READ (LOAD_REG src1 or STORE_REG src1) before its
    //     first write (loop-carried), or never written (loop-invariant).
    {
        for (int i = 0; i < 32; i++) arch_pin_[i] = -1;
        pinned_host_regs_ = 0;
        keep_store_dest_ = false;
        bool no_selfloop = (mt_active() && dbg().no_chain) ||
            (getenv("BIFROST_NO_SELFLOOP") != nullptr);
        bool is_selfloop = false;
        if (!ir_block.insts.empty()) {
            const IRInst& last = ir_block.insts.back();
            is_selfloop = (last.op == IROp::BRCOND || last.op == IROp::BRCOND_ZERO ||
                           last.op == IROp::BRCOND_BIT) && last.branch_target() == start_pc;
        }
        if (is_selfloop && !no_selfloop && !getenv("BIFROST_NO_PIN") &&
            !getenv("BIFROST_JIT_VERIFY")) {
            // Pinning is disabled under BIFROST_JIT_VERIFY: verify mode
            // un-patches the self-loop slot, so the taken back-edge returns
            // to the dispatcher and re-runs the prologue — whose preloads
            // would read STALE cpu.regs[] for the DEFERRED pins (the
            // deferral assumes the tight jmp-to-body loop). Compiling the
            // block without pins restores the eager-store behavior the
            // verifier compares against.
            bool has_call = false;
            for (const IRInst& inst : ir_block.insts) {
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
                size_t n = ir_block.insts.size();
                for (size_t i = 0; i < n; i++) {
                    const IRInst& inst = ir_block.insts[i];
                    if (inst.op == IROp::STORE_REG) {
                        if (inst.dest <= 30 && first_write[inst.dest] == 0xFFFF)
                            first_write[inst.dest] = static_cast<uint16_t>(i);
                        // src1 is the source VALUE vreg; if it names an arch
                        // reg (mov xN, xA) that is a READ of arch reg src1.
                        if (inst.src1 <= 30 && first_read[inst.src1] == 0xFFFF)
                            first_read[inst.src1] = static_cast<uint16_t>(i);
                    } else if (inst.op == IROp::LOAD_REG) {
                        if (inst.src1 <= 30 && first_read[inst.src1] == 0xFFFF)
                            first_read[inst.src1] = static_cast<uint16_t>(i);
                    } else if (writes_arch_gpr_directly(inst)) {
                        // Bypasses STORE_REG: never pin it (see the helper).
                        direct_write[inst.dest] = true;
                    }
                }
                int n_pins = 0;
                // Loop-carried vregs (read before first write, or loop-
                // invariant reads) get pinned — the value persists across the
                // back-edge in the register (bench_mips x0/x2). Written-then-
                // read vregs are NOT pinned: pinning them occupies a scratch
                // register for the whole loop and measured ~6% SLOWER on
                // bench_mips (the body's LOAD_MEM needs the spare register;
                // a single extra spill per iteration costs more than the
                // store→load round trip it removes).
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
    // Emit the pin preloads (cpu.regs[a] → pin reg) right after the vec
    // prologue loads — after chain_entry_off_ (so chain entries run them)
    // but before block_body_start_off_ (the self-loop back-edge jumps there,
    // preserving the pins as loop-carried state). Register the mapping so
    // body reads of the pinned vreg (LOAD_REG/STORE_REG src1) reuse the pin
    // directly. The value is CLEAN: it mirrors cpu.regs[a] exactly.
    for (int a = 0; a <= 30; a++) {
        if (arch_pin_[a] >= 0) {
            emit_load_arm(arch_pin_[a], a);
            vreg_home_[a] = arch_pin_[a];
            reg_vreg_[arch_pin_[a]] = a;
            vreg_dirty_[a] = false;
            vreg_last_use_[a] = ++regalloc_lru_counter_;
        }
    }
    // Record the block body start offset (after prologue). Used for
    // self-loop chaining: the selfloop slot is patched to jmp here.
    block_body_start_off_ = code_buf_used_;
    emit_tier2_work_probe(start_pc, ir_block.count);
    // ── Compute liveness: for each vreg, find the last op that uses it ──
    // This lets us free host regs of dead vregs immediately after their
    // last use, instead of keeping them cached until eviction. Without
    // this, the register allocator treats all vregs as live until the end
    // of the block, causing unnecessary spills when register pressure is
    // high (e.g., 5+ live vregs but 10 host regs, with 4+ dead vregs
    // occupying the other regs).
    //
    // Only scratch vregs (33+) are tracked for killing. ARM reg vregs
    // (0-31) represent architectural state that must be flushed to
    // cpu.regs[] at the epilogue — killing a dirty ARM reg vreg early
    // would lose its value before flush_all_vregs can write it back.
    //
    // Special cases for the use-scan:
    //   - LOAD_REG: src1 is the ARM reg index (0-31), NOT a vreg. Don't
    //     treat it as a use.
    //   - STORE_REG: dest is the ARM reg index, src1 is the vreg being
    //     stored. src1 IS a use.
    //   - dest of any op is a definition (not a use). If dest == src1
    //     (in-place modify), the vreg is still live after this op (as
    //     dest), so we don't kill it (handled in the compile loop below).
    {
        size_t n = ir_block.insts.size();
        vreg_last_use_op_.assign(4096, -1);
        reset_vreg_uses();
        for (size_t i = 0; i < n; i++) {
            const IRInst& inst = ir_block.insts[i];
            if (inst.op != IROp::LOAD_REG) {
                // src1/src2 are vregs (for LOAD_REG, src1 is ARM reg index).
                // Bounds-check: vreg space is 0-4095. A block with vregs
                // >=4096 indicates a translator bug; cap to avoid OOB.
                if (inst.src1 > 31 && inst.src1 < 4096) {
                    vreg_last_use_op_[inst.src1] = static_cast<int>(i);
                    note_vreg_use(inst.src1, static_cast<uint16_t>(i));
                }
                if (inst.src2 > 31 && inst.src2 < 4096) {
                    vreg_last_use_op_[inst.src2] = static_cast<int>(i);
                    note_vreg_use(inst.src2, static_cast<uint16_t>(i));
                }
            }
            // aux is a vreg for SMADDL/UMADDL/SMSUBL/UMSUBL (accumulator);
            // SIMD_INS stores a small element index (< 33, filtered out).
            if (inst.aux > 31 && inst.aux < 4096) {
                vreg_last_use_op_[inst.aux] = static_cast<int>(i);
                note_vreg_use(inst.aux, static_cast<uint16_t>(i));
            }
        }
        // Build kills_per_op_: for each op i, the list of scratch vregs
        // whose last use is i (dest is excluded at kill time in the loop).
        kills_per_op_.assign(n, {});
        for (int v = 32; v < 4096; v++) {
            if (vreg_last_use_op_[v] >= 0) {
                kills_per_op_[vreg_last_use_op_[v]].push_back(static_cast<uint16_t>(v));
            }
        }
        // ── Fold-lookahead pre-scan ─────────────────────────────────
        // For each IMM whose dest's ONLY read is the immediately-following
        // op (and that op is a foldable ALU/shift consuming the dest as a
        // dead src2), classify it so the IMM codegen can skip emitting the
        // mov entirely — the consumer folds the const into an x86
        // immediate and never reads the vreg's host mapping or stack slot.
        // The conditions mirror the consumer's fold guards EXACTLY
        // (Arithmetic.cpp): the IMM case additionally verifies the
        // imm32 sign-extension fit for the ALU class before skipping.
        fold_ahead_kind_.assign(n, 0);
        for (size_t i = 0; i + 1 < n; i++) {
            const IRInst& im = ir_block.insts[i];
            if (im.op != IROp::IMM) continue;
            if (!(im.dest > 32 && im.dest < 4096)) continue;
            if (vreg_last_use_op_[im.dest] != static_cast<int>(i + 1)) continue;
            const IRInst& nx = ir_block.insts[i + 1];
            if (nx.src2 != im.dest || nx.dest == nx.src2 || nx.src1 == nx.src2) continue;
            switch (nx.op) {
                case IROp::ADD: case IROp::SUB: case IROp::AND:
                case IROp::OR:  case IROp::XOR: case IROp::MUL:
                    fold_ahead_kind_[i] = 1;  // ALU fold — const must survive imm32 sign-extend
                    break;
                case IROp::SHL: case IROp::SHR: case IROp::SAR: case IROp::ROR:
                    fold_ahead_kind_[i] = 2;  // shift fold — any count folds
                    break;
                default:
                    break;
            }
        }
        // W-form fusion: the translator follows each 32-bit ALU result with
        // ZEXT 32. When that ZEXT is the result's only reader, the op can
        // write the 32-bit x86 form instead (low 32 bits are identical for
        // ADD/SUB/AND/OR/XOR/IMUL, and the 32-bit write clears bits 63:32).
        zext32_fused_.assign(n, 0);
        for (size_t i = 0; i + 1 < n; i++) {
            const IRInst& op = ir_block.insts[i];
            switch (op.op) {
                case IROp::ADD: case IROp::SUB: case IROp::AND:
                case IROp::OR:  case IROp::XOR: case IROp::MUL: break;
                // MOVSX r32 sign-extends into the low 32 bits only.
                case IROp::SEXT:
                    if (op.sext_bits() != 8 && op.sext_bits() != 16) continue;
                    break;
                default: continue;
            }
            if (!(op.dest > 32 && op.dest < 4096)) continue;
            if (vreg_last_use_op_[op.dest] != static_cast<int>(i + 1)) continue;
            const IRInst& nx = ir_block.insts[i + 1];
            if (nx.op == IROp::ZEXT && nx.src1 == op.dest && nx.zext_bits() == 32)
                zext32_fused_[i] = 1;
        }
    }
    // ── Flag-loop-carry pre-scan ────────────────────────────────────
    // Decides whether a self-loop back-edge may skip the taken-path
    // pstate materialization (Branch.cpp BRCOND case). A
    // block is "flag loop-carried" if any flag-CONSUMING op (one that
    // reads guest NZCV) appears before the first flag-SETTING op. If a
    // consumer runs first, the flags it reads come from the PREVIOUS
    // iteration, so the loop-back edge must still materialize host flags
    // to pstate for the loop top to reload. Consumers are checked BEFORE
    // the setter mark for ops that are both (ADCS/SBCS read C, CCMP reads
    // cond): an ADCS at the block head reads loop-carried C.
    flags_loop_carried_ = false;
    {
        bool flag_setter_seen = false;
        for (const IRInst& inst : ir_block.insts) {
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
            if (consumer && !flag_setter_seen) {
                flags_loop_carried_ = true;
                break;
            }
            switch (inst.op) {
                case IROp::ADDS: case IROp::SUBS:
                case IROp::ADCS: case IROp::SBCS:
                case IROp::TST: case IROp::TST_ZERO:
                case IROp::CCMP: case IROp::FP_CMP:
                    flag_setter_seen = true;
                    break;
                default:
                    break;
            }
        }
    }
    // ── Compile IR ───────────────────────────────────────────────
    jit_consts_.clear();
    // Guarded-access cold paths are emitted after the block (see below).
    deferred_guards_.clear();
    defer_guards_ = true;
    for (size_t i = 0; i < ir_block.insts.size(); i++) {
        const IRInst& inst = ir_block.insts[i];
        cur_op_index_ = i;
        // Patch pending BRCOND_SKIP forward jumps: when the op AFTER the
        // skip region is reached, its code starts at the current offset, so
        // a region's jcc (emitted at the branch's position) must target it.
        // The fixup fires exactly once per skip (single-pass match).
        for (auto& f : skip_fixups_) {
            if (f.target_op == static_cast<int>(i)) {
                int32_t rel = static_cast<int32_t>(code_buf_used_ - (f.jcc_offset + 6));
                patch_jcc_rel32(f.jcc_offset, rel);
            }
        }
        if (compile_ir_inst(inst)) break;
        // A non-IMM op defining a vreg invalidates any const entry for it
        // (defensive; the monotonic allocator makes collisions impossible).
        if (inst.op != IROp::IMM) jit_consts_.erase(inst.dest);
        // Free host regs of vregs whose last use was this op. Dead vregs
        // are freed immediately, making room for new vregs without eviction.
        if (i < kills_per_op_.size()) {
            for (uint16_t v : kills_per_op_[i]) {
                // Don't kill the dest of this op — it's a new definition
                // and still live (handles dest == src1 in-place modify).
                if (v != inst.dest) {
                    kill_vreg(v);
                }
            }
        }
        // Dead-scratch-dest drop: if THIS op's dest is a scratch vreg
        // (33+) whose last use is at-or-before this op — never read as a
        // source, or read only in-place (dest == src1) — the value is dead
        // once the op has been emitted. Drop its host mapping NOW (no
        // spill) so it neither occupies a host reg for the rest of the
        // block nor gets written back by the epilogue's flush. Exact
        // because vreg numbers are block-local unique-per-definition
        // (VregAlloc monotonic) and the use-scan covers src1/src2/aux.
        if (inst.dest > 32 && inst.dest < 4096 &&
            vreg_last_use_op_[inst.dest] <= static_cast<int>(i)) {
            kill_vreg(inst.dest);
        }
    }
    kills_per_op_.clear();
    fold_ahead_kind_.clear();
    zext32_fused_.clear();
    vreg_last_use_op_.clear();
    reset_vreg_uses();
    // ── Regalloc bloat diagnostic (BIFROST_REGALLOC_STATS=1) ─────
    // Counts the spill/reload instruction density of THIS block. A high
    // reload:spill ratio for the same vreg within a block is the bloat
    // signal (stack round-trips the allocator should have avoided).
    if (dbg().regalloc_stats) {
        const auto& rs = regalloc_stats_get();
        uint64_t spills = rs.spill_stack + rs.spill_arm;
        uint64_t reloads = rs.reload_stack + rs.reload_arm;
        fprintf(stderr, "[regalloc] pc=%#llx insts=%zu spill=%llu(%llu/arm %llu) reload=%llu(%llu/arm %llu) evict=%llu\n",
                (unsigned long long)start_pc, ir_block.insts.size(),
                (unsigned long long)spills, (unsigned long long)rs.spill_stack,
                (unsigned long long)rs.spill_arm, (unsigned long long)reloads,
                (unsigned long long)rs.reload_stack, (unsigned long long)rs.reload_arm,
                (unsigned long long)rs.evicts);
    }
    // ── Epilogue ─────────────────────────────────────────────────
    size_t epilogue_off = code_buf_used_;
    // Materialize pending host flags to cpu.pstate before returning.
    // If a flag-setting op (ADDS/SUBS/TST) was the last to touch flags
    // and no subsequent BRCOND consumed them, the flags are still in
    // the host CPU's RFLAGS but haven't been written to pstate. The
    // next block (or the interpreter) would see stale pstate.
    // clobber_flags() handles the flush+materialize+invalidate pattern.
    clobber_flags();
    // Write back dirty architectural vregs before returning (so cpu.regs[]
    // is up to date); dead scratch values need no stack store at the exit.
    flush_all_vregs_exit();
    // Write back dirty cached vectors to cpu.v_lo/v_hi. Self-loop re-entry
    // skips the epilogue, so loop-carried accumulators stay in XMM; this
    // runs only when the loop exits (or the block is entered from another
    // block / the dispatcher).
    vec_cache_writeback_all();
    if (!rax_holds_next_pc_) {
        // inline blocks' instr_count is not a contiguous
        // guest-pc count (the inlined leaf body + implicit BL/RET break
        // the linear model), so the fall-through next_pc is the builder's
        // cur_pc — the first untranslated instruction (BL+4 or the cap
        // point). Blocks without inlined leaves keep the cheap formula.
        uint64_t next_pc = has_inlined_leaf ? cur_pc : start_pc + ir_block.count * 4;
        emit_mov_imm_to_rax(next_pc);
        // Fall-through (MAX_BLOCK hit before any block-ender): the next
        // PC is statically known, so this block is chainable to it.
        if (!unchainable_end_ && chain_target_pc_ == 0) {
            chain_target_pc_ = next_pc;
        }
    }
    // ── PC store point ───────────────────────────────────────────
    // At this point, flags are materialized and vregs are flushed.
    // RAX holds the next PC (either from rax_holds_next_pc_ or the
    // static fall-through).
    emit_store(CPU_REG, PC_OFF, RAX);
    emit_tier2_edge_profile(false);
    // ── Chain-capable epilogue ────────────────────────────────────
    // For block chaining we jump directly from one block's epilogue to
    // the next block's prologue. The next prologue reloads RBX from
    // RDI (the System-V arg register) and stashes emu from RSI into its
    // frame slot, so before restoring our own callee-saved regs we copy
    // the live CPU/EMU pointers into RDI/RSI (emu re-read from the frame
    // slot — R14 is a free vreg register now).
    // When the block returns (unchained `ret`), clobbering RDI/RSI is
    // fine — they're caller-saved and the dispatcher doesn't read them
    // after the call. When the block is chained, the next prologue sees
    // the correct RDI=cpu / RSI=emu.
    emit_mov_reg(RDI, CPU_REG);   // mov rdi, rbx
    emit_load(RSI, RBP, emu_slot_off());  // rsi = emu (from the frame slot; R14 is a free vreg reg now)
    size_t chain_patch_off;
    if (chain_skip_enabled()) {
        // ── Chain-skip epilogue ─────────────────────────────────────
        // Chain edge layout: [slot] [cold exit]. The 5-byte slot is all
        // NOPs initially; patch_chain() overwrites it with `jmp rel32` →
        // successor's chain_entry (past its push/frame/reg-setup). The
        // successor reuses THIS block's 32 KB frame, so the callee-saved
        // regs are NOT restored on the chain edge — the cold exit
        // (`mov rsp,rbp; pop×6; ret`) is only reached when the slot is
        // still unpatched, at which point it restores the frame exactly as
        // the old `ret`+NOPs layout did. RBX/R14 already went to RDI/RSI
        // for the successor's prologue-agnostic body (it reads cpu/emu
        // from RDI/RSI at chain_entry just like fn does).
        chain_patch_off = code_buf_used_;
        if (mt_active()) emit_mt_chain_slot(unchainable_end_ ? 0 : chain_target_pc_);
        else emit_slot_nop5();
        // A register branch's slot is never patched (dynamic target); try
        // the indirect-branch table instead of returning to the dispatcher.
        if (indirect_exit_ && !dbg().no_chain) emit_indirect_branch_probe();
        // Cold exit (only reached when the slot above is unpatched).
        emit_byte(0x48); emit_byte(0x89); emit_byte(0xEC); // mov rsp, rbp
        emit_pop(R15); emit_pop(R14); emit_pop(R13);
        emit_pop(R12); emit_pop(RBP); emit_pop(RBX);
        emit_ret();
    } else {
        // ── Chain-capable epilogue (original layout) ────────────────
        emit_byte(0x48); emit_byte(0x89); emit_byte(0xEC); // mov rsp, rbp
        emit_pop(R15); emit_pop(R14); emit_pop(R13);
        emit_pop(R12); emit_pop(RBP); emit_pop(RBX);
        // ── Chain slot ──
        // 5 bytes reserved at the end of every block. Initially `ret` + 4
        // NOPs (acts as a plain return to the C dispatcher). When the
        // block's chain target has been translated, patch_chain() overwrites
        // all 5 bytes with `jmp rel32` → target block's entry, skipping the
        // dispatcher entirely for straight-line / unconditional-branch code.
        chain_patch_off = code_buf_used_;
        if (mt_active()) emit_mt_chain_slot(unchainable_end_ ? 0 : chain_target_pc_);
        emit_ret();                                  // 0xC3
        emit_nop(); emit_nop(); emit_nop(); emit_nop();  // 4 × 0x90
    }
    // Patch branch targets to epilogue.
    for (auto& p : branch_target_patches_) {
        int32_t rel = static_cast<int32_t>(epilogue_off - (p.patch_off + 5));
        patch_jmp_rel32(p.patch_off, rel);
    }
    if (!call_interp_branch_patches_.empty()) {
        // ── Dynamic CALL_INTERP exit ────────────────────────────────
        // The interpreter changed the PC (signal delivery, exception
        // return, ...): RAX holds that dynamic PC. It must return to the
        // dispatcher, never pass through the chain slot above — once
        // patched (or in MT, once its target cell is set) that slot jumps
        // to the STATIC successor and drops the new PC. emit_call_interp
        // already materialized flags and flushed vregs before the JNE.
        size_t dyn_exit_off = code_buf_used_;
        emit_store(CPU_REG, PC_OFF, RAX);
        emit_byte(0x48); emit_byte(0x89); emit_byte(0xEC); // mov rsp, rbp
        emit_pop(R15); emit_pop(R14); emit_pop(R13);
        emit_pop(R12); emit_pop(RBP); emit_pop(RBX);
        emit_ret();
        for (size_t off : call_interp_branch_patches_) {
            int32_t rel = static_cast<int32_t>(dyn_exit_off - (off + 6));
            patch_jcc_rel32(off, rel);
        }
    }
    // Cold software page checks and fault exits of guarded accesses, after
    // all hot code; their registered fault sites get the real stub offsets.
    flush_deferred_guards();
    defer_guards_ = false;
    if (code_buf_overflow_) {
        rewind_codegen(block_start);
        // The budget is fixed for this JIT. Cache the failure so every
        // subsequent call does not repeat decode/IR/codegen before stepping.
        BlockEntry fallback;
        fallback.interp_only = true;
        fallback.interp_only_count = 1;
        fallback.instr_count = 1;
        fallback.verified_once = true;
        if (!private_entry) publish_or_stage(start_pc, fallback);
        code_cache_overflows.fetch_add(1, std::memory_order_relaxed);
        code_budget_exhausted_ = true;
        // W^X: make the buffer executable again before returning (we may
        // have written partial code before the overflow was detected).
        make_executable();
        return nullptr;
    }
    if (dump_ir_ || start_pc == force_dump_pc_.load()) {
        size_t code_len = code_buf_used_ - block_start;
        fprintf(stderr, "  → %zu bytes of x86 code @ %p:\n    ",
                code_len, static_cast<void*>(code_buf_ + block_start));
        for (size_t i = 0; i < code_len; i++) {
            fprintf(stderr, "%02x ", code_buf_[block_start + i]);
            if ((i & 31) == 31 && i + 1 < code_len) fprintf(stderr, "\n    ");
        }
        fprintf(stderr, "\n");
    }
    auto fn = (uint64_t(*)(CPU*, Emulator*))(code_buf_ + block_start);
    // If the block ended with an unchainable op (BR/BRCOND/SVC), force
    // chain_target_pc_ to 0 so try_chain_block() skips it.
    if (unchainable_end_) chain_target_pc_ = 0;
    BlockEntry entry;
    entry.fn = fn;
    entry.emitted_bytes = code_buf_used_ - block_start;
    // Chain-skip entry point (BIFROST_CHAIN_SKIP=1): chain slots patch to
    // the body start past the push/frame/reg-setup prologue instead of fn.
    // Only set when the gate is on; otherwise chain slots target fn and
    // try_chain_block ignores this field.
    if (chain_skip_enabled())
        entry.chain_entry = (uint64_t(*)(CPU*, Emulator*))(code_buf_ + chain_entry_off_);
    entry.ends_with_branch = ir_block.ends_with_branch;
    entry.chain_patch_off = chain_patch_off;
    entry.chain_target_pc = chain_target_pc_;
    entry.chained = false;
    // Taken-path chain info (BRCOND/CBZ/CBNZ/TBZ/TBNZ emit a second chain
    // slot on the taken path; see emit_taken_path_epilogue).
    entry.has_taken_chain_slot = has_taken_chain_slot_;
    entry.taken_chain_patch_off = taken_chain_patch_off_;
    entry.taken_chain_target_pc = taken_chain_target_pc_;
    entry.taken_chained = false;
    entry.tier2_counter_off = tier2_counter_off_;
    entry.tier2_counter_len = tier2_counter_len_;
    entry.instr_count = instr_count;
    // Runtime hot-interp promotion uses the same post-optimization IR count
    // as the translate-time interp_only decision. The pre-translation
    // predictor is intentionally only a block-splitting heuristic and may
    // overestimate native FP/SIMD instructions.
    entry.call_interp_count = actual_call_interp;
    entry.verified_once = has_bl_call || has_call_interp || has_inlined_leaf;
    // SVC presence: the verify-mode interp re-run re-executes the syscall —
    // non-idempotent syscalls (read/poll/...) return different values the
    // second time, so verify quarantines SVC blocks (see run_block gate).
    entry.has_svc = has_svc;
    // Calls mid-block break the fixed-step re-run the same way (it steps
    // INTO the callee while the JIT called-and-continued), so they share
    // the quarantine.
    entry.has_call = has_bl_call || has_call_interp;
    // Local input analysis is used only to limit verification comparisons;
    // cross-block exits always preserve NZCV for transitive consumers.
    entry.reads_pstate_before_set = flags_loop_carried_;
    entry.inlined_ranges_ = std::move(inlined_ranges_);
    inlined_ranges_.clear();
    // Record self-loop info: if the block has a selfloop slot, patch it
    // to jump back to the block body start (skipping epilogue+dispatcher+
    // prologue). This is the single biggest win for tight loops.
    if (has_selfloop_slot_) {
        entry.has_selfloop_slot = true;
        entry.selfloop_patch_off = selfloop_patch_off_;
        // MT body edges were emitted with a revocable data cell. Never
        // patch their instructions or redirect them to a different prologue.
        if (!mt_active()) {
            // Patch the selfloop slot: `jmp rel32` → block body start.
            size_t body_off = block_body_start_off_;
            int32_t self_rel = static_cast<int32_t>(body_off - (selfloop_patch_off_ + 5));
            code_buf_[selfloop_patch_off_] = 0xE9;  // jmp rel32
            code_buf_[selfloop_patch_off_ + 1] = self_rel & 0xFF;
            code_buf_[selfloop_patch_off_ + 2] = (self_rel >> 8) & 0xFF;
            code_buf_[selfloop_patch_off_ + 3] = (self_rel >> 16) & 0xFF;
            code_buf_[selfloop_patch_off_ + 4] = (self_rel >> 24) & 0xFF;
        }
    }
    if (private_entry) {
        *private_entry = std::move(entry);
        prev_max_vreg_ = max_vreg_;
        make_executable();
        return fn;
    }
    record_verification_stores(ir_block, start_pc, entry);
    attach_code_words(entry, !has_inlined_leaf);
    // Save max_vreg_ so the next translate_block only clears what's needed.
    prev_max_vreg_ = max_vreg_;
    // Verify dirty_host_regs_ invariant (active in debug or with
    // BIFROST_REGALLOC_CHECK=1 — catches regalloc maintenance bugs).
    verify_dirty_host_regs_();
    if (staging_) {
        staged_entry_ = std::move(entry);
        staged_ = staged_linked_ = true;
    } else {
        publish_translation(start_pc, entry);
    }
    // W^X: toggle the code buffer back to executable before returning.
    // The block is fully emitted and patched; execution will read from it.
    make_executable();
    return fn;
}
// Publishes a native Tier-1 translation and links it with the table: back
// references, chaining in both directions and pending direct calls. The
// caller holds blocks_mutex_ exclusively.
void FrostJIT::publish_translation(uint64_t start_pc, const BlockEntry& entry) {
    publish_block(start_pc, entry);
    blocks_translated++;
    // Maintain the back-reference index: this block's fall-through and
    // taken slots target T, so chain_back_references(T) finds it in O(k)
    // when T is translated later (e.g. a loop whose back-edge target is
    // only translated on the second iteration).
    const uint64_t target = entry.chain_target_pc, taken = entry.taken_chain_target_pc;
    if (target != 0) add_back_ref(target, start_pc);
    if (taken != 0 && taken != target) add_back_ref(taken, start_pc);
    // Try to chain this block to its already-translated target, and
    // also patch any existing blocks whose chain target is this block.
    // BIFROST_NO_CHAIN disables chaining for debugging.
    if (!dbg().no_chain) {
        try_chain_block(start_pc, blocks_[start_pc]);
        chain_back_references(start_pc);
    }
    // Patch any direct BL call slots (from already-translated callers)
    // that target this block to call `fn` directly.
    if (direct_call_enabled())
        patch_pending_calls(start_pc, reinterpret_cast<const uint8_t*>(entry.fn));
}
void FrostJIT::publish_or_stage(uint64_t pc, const BlockEntry& entry) {
    if (!staging_) { publish_block(pc, entry); return; }
    staged_entry_ = entry;
    staged_ = true;
    staged_linked_ = false;
}
} // namespace arm64emu
