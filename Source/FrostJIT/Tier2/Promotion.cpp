// Tier-2 promotion: in-code hot-head counters, firing, trial setup and publication.
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
void tier2_fire_stub(Emulator* emu, uint64_t pc, size_t site) {
    if (!emu) return;
    FrostJIT* jit = emu->jit();
    if (!jit) return;
    jit->tier2_fire_region(*emu, pc, site);
}

void FrostJIT::tier2_counter_disable(size_t counter_off, size_t counter_len, bool legacy_transition) {
    if (!counter_off || counter_len < 6) return;
    auto mt = mt_counter_sites_.find(counter_off);
    if (mt != mt_counter_sites_.end()) {
        mt->second->enabled.store(0, std::memory_order_release);
        return;
    }
    if (mt_active() && !legacy_transition) return;
    // Only the single running vCPU may replace this legacy instruction
    // sequence. The MT transition disables it before a worker is launched.
    size_t off = counter_off;
    if (off + 6 > CODE_BUF_SIZE) return;
    const int32_t rel = static_cast<int32_t>(static_cast<int64_t>(counter_len) - 5);
    code_buf_[off] = 0xE9;
    memcpy(code_buf_ + off + 1, &rel, 4);
    std::atomic_thread_fence(std::memory_order_release);
}

void FrostJIT::tier2_fire_region(Emulator& emu, uint64_t pc, size_t site) {
    // Runs from INSIDE a generated block (prologue fire path). Never let an
    // exception cross the JIT frame boundary (std::terminate).
    try {
        // W^X bail: the in-code counter writes the code page (a non-W^X RWX
        // buffer), and the compile here would mprotect the page we're
        // currently executing from. Regions only exist in shared-JIT mode.
        if (wex_enabled_ || !tier2_enabled()) return;
        std::unique_lock<std::shared_mutex> lock(blocks_mutex_);
        auto it = blocks_.find(pc);
        if (it == blocks_.end()) {
            return;
        }
        if (site && it->second.tier2_counter_off != site) return; // retired notification
        BlockEntry old_entry = it->second;
        if (old_entry.tier2_hot_logged) {
            return;
        }
        Tier2Trace trace = collect_tier2_trace(emu, pc);
        // Samples lag the entry counter by one iteration. If a constituent
        // is still cold, retry at most three hotness windows before declining.
        if (!trace.fully_profiled && ++it->second.tier2_attempts < 3) {
            auto mt = mt_counter_sites_.find(site);
            if (mt != mt_counter_sites_.end())
                mt->second->hits.store(0, std::memory_order_relaxed);
            else if (site && old_entry.fn) {
                const uint32_t zero = 0;
                std::memcpy(reinterpret_cast<uint8_t*>(old_entry.fn) - 8, &zero, sizeof(zero));
            }
            tier2_profile_retries.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        it->second.tier2_hot_logged = true;
        tier2_counter_disable(it->second.tier2_counter_off, it->second.tier2_counter_len);
        tier2_hot_heads.fetch_add(1, std::memory_order_relaxed);
        // Require a likely complete path and actual boundary savings. MT
        // self-loops cannot retain pins/LICM across iterations and fuse no edges.
        if (!trace.fully_profiled || trace.path_probability < 0.75 ||
            (mt_active() && trace.blocks.size() < 2)) {
            tier2_profit_rejections.fetch_add(1, std::memory_order_relaxed);
            if (tier2_trace_enabled())
                std::fprintf(stderr, "[tier2] decline pc=0x%llx profiled=%d survival=%.3f blocks=%zu\n",
                    (unsigned long long)pc, trace.fully_profiled, trace.path_probability, trace.blocks.size());
            return;
        }
        // Bound total speculative compiler work and permanent code growth.
        // These are measured budgets, not a claim of predicted speedup.
        if (tier2_retrains_[pc] >= 2 ||
            tier2_code_bytes.load(std::memory_order_relaxed) >= 32ull * 1024 * 1024 ||
            tier2_compile_ns.load(std::memory_order_relaxed) >= 2000000000ull) {
            tier2_profit_rejections.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // Sampling compares exact existing Tier-1 block boundaries.
        if (trace.blocks.size() < 2) {
            tier2_profit_rejections.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        std::vector<std::pair<uint64_t, uint64_t (*)(CPU*, Emulator*)>> baseline_graph;
        uint64_t trial_lo = UINT64_MAX, trial_hi = 0;
        for (const auto& b : trace.blocks) {
            auto baseline = blocks_.find(b.pc);
            if (baseline == blocks_.end() || !baseline->second.fn || baseline->second.is_region ||
                baseline->second.has_call || baseline->second.has_svc || baseline->second.has_selfloop_slot ||
                baseline->second.instr_count != b.ir.count) {
                tier2_profit_rejections.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            baseline_graph.push_back({b.pc, baseline->second.fn});
            trial_lo = std::min(trial_lo, b.pc);
            trial_hi = std::max<uint64_t>(trial_hi, b.pc + b.ir.count * 4ull);
        }
        const auto compile_start = std::chrono::steady_clock::now();
        const size_t region_start = code_buf_used_;
        tier2_current_monitor_ = nullptr;
        const size_t old_edge_count = mt_edges_.size();
        const auto old_source_edges = mt_source_refs_[pc];
        uint64_t (*chain_fn)(CPU*, Emulator*) = nullptr;
        uint64_t (*rfn)(CPU*, Emulator*) =
            trace.ok ? compile_tier2_region(emu, trace, &chain_fn) : nullptr;
        tier2_compile_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - compile_start).count(), std::memory_order_relaxed);
        tier2_code_bytes.fetch_add(code_buf_used_ - region_start, std::memory_order_relaxed);
        const size_t region_bytes = code_buf_used_ - region_start;
        if (!rfn || !ensure_code_space(256)) {
            if (tier2_current_monitor_)
                tier2_current_monitor_->enabled.store(0, std::memory_order_release);
            // Unpublished region edges must not supersede the baseline's
            // source metadata. Their data cells stay alive but are revoked.
            for (size_t k = old_edge_count; k < mt_edges_.size(); ++k) {
                mt_edges_[k]->live = false;
                mt_edges_[k]->target.store(0, std::memory_order_release);
            }
            mt_source_refs_[pc] = old_source_edges;
            return;
        }
        if (rfn) {
            tier2_regions.fetch_add(1, std::memory_order_relaxed);
            if (tier2_trace_enabled()) {
                fprintf(stderr,
                        "[tier2] in-code region fired pc=0x%llx blocks=%zu insts=%llu\n",
                        static_cast<unsigned long long>(pc), trace.blocks.size(),
                        static_cast<unsigned long long>(trace.total_insts));
            }
            BlockEntry region_entry;
            region_entry.fn = rfn;
            region_entry.emitted_bytes = region_bytes;
            // Chain-skip: publish the post-frame-setup entry so chain edges
            // (bare jmps with the ROOT's rbp/rsp) skip the frame allocation.
            if (chain_skip_enabled() && chain_fn)
                region_entry.chain_entry = chain_fn;
            region_entry.instr_count = static_cast<int>(trace.total_insts);
            region_entry.tier2_hot_logged = true;
            region_entry.is_region = true;
            region_entry.tier2_monitor = tier2_current_monitor_;
            region_entry.ends_with_branch = true;
            region_entry.chained = true;        // region has no chain slot
            region_entry.taken_chained = true;
            region_entry.interp_only = false;
            region_entry.verified_once = true;  // M1: no region verify
            for (const auto& block : trace.blocks)
                region_entry.inlined_ranges_.push_back({block.pc, block.ir.count});
            for (const auto& block : trace.blocks) {
                auto baseline = blocks_.find(block.pc);
                if (baseline != blocks_.end()) {
                    tier2_counter_disable(baseline->second.tier2_counter_off, baseline->second.tier2_counter_len);
                    baseline->second.tier2_hot_logged = true;
                }
                const uint64_t end = block.pc + block.ir.count * 4;
                for (const auto& profile : tier2_profiles_by_pc_)
                    if (profile.first >= block.pc && profile.first < end)
                        profile.second->enabled.store(0, std::memory_order_release);
            }
            const auto wrapper_start_clock = std::chrono::steady_clock::now();
            auto trial = std::make_unique<Tier2Trial>();
            trial->monitor = tier2_current_monitor_;
            trial->baseline = old_entry;
            trial->candidate_entry = region_entry;
            trial->baseline.tier2_hot_logged = true;
            trial->candidate = rfn; trial->candidate_chain = chain_fn;
            trial->lo = trial_lo; trial->hi = trial_hi;
            trial->baseline_blocks = std::move(baseline_graph);
            trial->baseline_edges = old_source_edges;
            for (size_t k = old_edge_count; k < mt_edges_.size(); ++k)
                trial->candidate_edges.push_back(mt_edges_[k].get());
            const size_t sampling_start = code_buf_used_;
            const size_t sampling_edge_start = mt_edges_.size();
            struct SamplingCompileGuard {
                bool& flag;
                explicit SamplingCompileGuard(bool& f) : flag(f) { flag = true; }
                ~SamplingCompileGuard() { flag = false; }
            };
            bool samples_ok = true;
            {
                SamplingCompileGuard sampling(tier2_sampling_compile_);
                trial->baseline_blocks.clear();
                for (const auto& block : trace.blocks) {
                    BlockEntry copy;
                    if (!translate_block(emu, block.pc, &copy) || copy.instr_count != block.ir.count ||
                        copy.has_call || copy.has_svc || copy.has_selfloop_slot) {
                        samples_ok = false;
                        break;
                    }
                    trial->baseline_blocks.push_back({block.pc, copy.fn});
                    trial->sampling_blocks.push_back({block.pc, std::move(copy)});
                }
                if (samples_ok) {
                    trial->baseline_sample = trial->sampling_blocks.front().second.fn;
                    trial->baseline_chain_sample = trial->sampling_blocks.front().second.chain_entry;
                    trial->candidate_sample = compile_tier2_region(emu, trace,
                        &trial->candidate_chain_sample, trial->monitor);
                    samples_ok = trial->candidate_sample != nullptr;
                }
            }
            for (size_t k = sampling_edge_start; k < mt_edges_.size(); ++k)
                trial->sampling_edges.push_back(mt_edges_[k].get());
            tier2_code_bytes.fetch_add(code_buf_used_ - sampling_start, std::memory_order_relaxed);
            if (!samples_ok || !ensure_code_space(256)) {
                trial->monitor->enabled.store(0, std::memory_order_release);
                for (size_t k = old_edge_count; k < mt_edges_.size(); ++k) {
                    mt_edges_[k]->live = false;
                    mt_edges_[k]->target.store(0, std::memory_order_release);
                }
                mt_source_refs_[pc] = old_source_edges;
                return;
            }
            auto* trial_ptr = trial.get();
            tier2_current_monitor_->trial = trial_ptr;
            tier2_trials_.push_back(std::move(trial));
            // A full-frame entry selects live-work A/B runs. Backedges in an
            // active run stay entirely in native code until the quota expires.
            make_writable();
            const size_t wrapper = code_buf_used_;
            emit_mov_imm64(RDX, reinterpret_cast<uint64_t>(trial_ptr));
            emit_byte(0x48); emit_byte(0x39); emit_byte(0x97);
            emit_u32(offsetof(CPU, jit_tier2_trial)); // cmp [rdi+trial],rdx
            const size_t outside_trial = emit_jcc_rel32_placeholder(5);
            emit_byte(0x83); emit_byte(0xAF);
            emit_u32(offsetof(CPU, jit_tier2_trial_remaining)); emit_byte(1);
            const size_t quota = emit_jcc_rel32_placeholder(4);
            emit_load(R11, RDI, offsetof(CPU, jit_tier2_trial_fn));
            emit_byte(0x41); emit_byte(0xFF); emit_byte(0xE3);
            const size_t done = code_buf_used_;
            emit_load(RAX, RDI, PC_OFF); emit_ret();
            patch_jcc_rel32(quota, static_cast<int32_t>(done - (quota + 6)));
            patch_jcc_rel32(outside_trial, static_cast<int32_t>(code_buf_used_ - (outside_trial + 6)));
            emit_byte(0x48); emit_byte(0x83); emit_byte(0xBF);
            emit_u32(offsetof(CPU, jit_tier2_trial)); emit_byte(0);
            const size_t nested = emit_jcc_rel32_placeholder(5);
            patch_jcc_rel32(nested, static_cast<int32_t>(done - (nested + 6)));
            emit_mov_imm64(R11, reinterpret_cast<uint64_t>(&tier2::measured_region_entry));
            emit_byte(0x41); emit_byte(0xFF); emit_byte(0xE3);
            region_entry.fn = reinterpret_cast<uint64_t (*)(CPU*, Emulator*)>(code_buf_ + wrapper);
            if (chain_skip_enabled()) {
                const size_t chain_wrapper = code_buf_used_;
                emit_mov_imm64(RDX, reinterpret_cast<uint64_t>(trial_ptr));
                emit_byte(0x48); emit_byte(0x39); emit_byte(0x97);
                emit_u32(offsetof(CPU, jit_tier2_trial));
                const size_t cold_chain = emit_jcc_rel32_placeholder(5);
                emit_byte(0x83); emit_byte(0xAF);
                emit_u32(offsetof(CPU, jit_tier2_trial_remaining)); emit_byte(1);
                const size_t chain_quota = emit_jcc_rel32_placeholder(4);
                emit_load(R11, RDI, offsetof(CPU, jit_tier2_trial_chain_fn));
                emit_byte(0x41); emit_byte(0xFF); emit_byte(0xE3);
                const size_t cold_chain_tail = code_buf_used_;
                emit_byte(0x48); emit_byte(0x89); emit_byte(0xEC);
                emit_pop(R15); emit_pop(R14); emit_pop(R13);
                emit_pop(R12); emit_pop(RBP); emit_pop(RBX);
                const size_t jump = emit_jmp_rel32_placeholder();
                patch_jmp_rel32(jump, static_cast<int32_t>(wrapper - (jump + 5)));
                // Quota also needs the root-frame unwind; emit a separate tail.
                const size_t quota_tail = code_buf_used_;
                emit_byte(0x48); emit_byte(0x89); emit_byte(0xEC);
                emit_pop(R15); emit_pop(R14); emit_pop(R13);
                emit_pop(R12); emit_pop(RBP); emit_pop(RBX);
                emit_load(RAX, RDI, PC_OFF); emit_ret();
                patch_jcc_rel32(cold_chain, static_cast<int32_t>(cold_chain_tail - (cold_chain + 6)));
                patch_jcc_rel32(chain_quota, static_cast<int32_t>(quota_tail - (chain_quota + 6)));
                region_entry.chain_entry = reinterpret_cast<uint64_t (*)(CPU*, Emulator*)>(code_buf_ + chain_wrapper);
            }
            // Bind private chains only to this sampling graph. Cold targets
            // stay unbound and return before executing unrelated guest work.
            auto sample_target = [&](uint64_t target, bool full) -> uint64_t (*)(CPU*, Emulator*) {
                if (target == pc)
                    return !full && region_entry.chain_entry ? region_entry.chain_entry : region_entry.fn;
                for (const auto& copy : trial_ptr->sampling_blocks)
                    if (copy.first == target)
                        return !full && copy.second.chain_entry ? copy.second.chain_entry : copy.second.fn;
                return nullptr;
            };
            for (auto* edge : trial_ptr->sampling_edges)
                edge->target.store(reinterpret_cast<uint64_t>(sample_target(edge->target_pc, edge->cold_entry)),
                    std::memory_order_release);
            if (!mt_active() && !dbg().no_chain) {
                for (const auto& copy : trial_ptr->sampling_blocks) {
                    for (auto exit : {std::pair<uint64_t, size_t>{copy.second.chain_target_pc, copy.second.chain_patch_off},
                                      {copy.second.taken_chain_target_pc, copy.second.taken_chain_patch_off}}) {
                        auto target = sample_target(exit.first, false);
                        if (!target || !exit.second) continue;
                        make_writable();
                        patch_chain(exit.second, reinterpret_cast<const uint8_t*>(target));
                    }
                }
            }
            make_executable();
            const size_t wrapper_bytes = code_buf_used_ - wrapper;
            tier2_code_bytes.fetch_add(wrapper_bytes, std::memory_order_relaxed);
            tier2_compile_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - wrapper_start_clock).count(), std::memory_order_relaxed);
            region_entry.emitted_bytes += wrapper_bytes;
            remember_code_pages(pc, region_entry);
            it->second = std::move(region_entry);
            for (size_t k = old_edge_count; k < mt_edges_.size(); ++k)
                refresh_mt_edges(mt_edges_[k]->target_pc);
            if (mt_active()) mt_regions_published_.fetch_add(1, std::memory_order_relaxed);
            redirect_tier2_head_locked(pc);

        }
    } catch (...) {
        // Never let a collect/compile/registration failure escape a JIT frame.
        // The RAII compiler lock is released on every failure.
    }
}
}  // namespace arm64emu
