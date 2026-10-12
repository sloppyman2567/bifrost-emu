// Tier-2 measured A/B trials: timed windows, head redirection, keep/reject.
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

uint64_t tier2::measured_region_entry(CPU* cpu, Emulator* emu, void* trial) {
    return emu->jit()->tier2_measure(*cpu, *emu, trial);
}
static uint64_t thread_cpu_ns() {
    timespec ts{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
    return uint64_t(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

uint64_t FrostJIT::tier2_measure(CPU& cpu, Emulator& emu, void* opaque) {
    auto& trial = *static_cast<Tier2Trial*>(opaque);
    auto* monitor = trial.monitor;
    if (!monitor->enabled.load(std::memory_order_acquire)) return cpu.pc;
    unsigned decision = trial.decision.load(std::memory_order_acquire);
    if (decision) return (decision == 1 ? trial.candidate : trial.baseline.fn)(&cpu, &emu);
    auto& slot = cpu.jit_tier2_feedback[monitor->slot];
    if (slot.owner != monitor->identity) {
        slot = CPU::Tier2FeedbackSlot{};
        slot.owner = monitor->identity;
        // Linear paths pay two CPU-clock reads per traversal. Start with
        // enough guest work to qualify, then grow only when the measured
        // baseline is below 10 us, instead of timing 256 visits unconditionally.
        if (trial.linear)
            slot.sample_quota = std::max(1u, (128u + trial.candidate_entry.instr_count - 1) /
                static_cast<unsigned>(trial.candidate_entry.instr_count));
    }
    const bool candidate = slot.baseline_ns != 0;
    auto fn = candidate ? (trial.candidate_sample ? trial.candidate_sample : trial.candidate)
                        : (trial.baseline_sample ? trial.baseline_sample : trial.baseline.fn);
    // Never replay guest memory or restore architectural state: each arm runs
    // once on the live workload. Matched work/exit signatures make samples
    // comparable; CLOCK_THREAD_CPUTIME_ID excludes scheduler descheduling.
    // Timed arms stay on their private sampling chains: unbind the indirect-
    // branch table so a BR/RET cannot leave the sampled graph mid-window.
    struct Reset {
        CPU& cpu;
        void* ibl;
        ~Reset() {
            cpu.jit_tier2_trial = nullptr;
            cpu.jit_tier2_trial_fn = cpu.jit_tier2_trial_chain_fn = nullptr;
            cpu.jit_ibl_table = ibl;
        }
    } reset{cpu, cpu.jit_ibl_table};
    cpu.jit_ibl_table = nullptr;
    cpu.jit_tier2_trial = &trial;
    cpu.jit_tier2_trial_fn = fn;
    cpu.jit_tier2_trial_chain_fn = candidate
        ? (trial.candidate_chain_sample ? trial.candidate_chain_sample : trial.candidate_chain)
        : (trial.baseline_chain_sample ? trial.baseline_chain_sample : trial.baseline.chain_entry);
    cpu.jit_tier2_trial_remaining = trial.linear ? 1 : slot.sample_quota;
    cpu.jit_tier2_trial_work = cpu.jit_tier2_trial_hash = 0;
    const uint64_t start = thread_cpu_ns();
    for (unsigned dispatches = 0; dispatches < 2048; ++dispatches) {
        cpu.jit_chain_budget = 256;
        fn(&cpu, &emu);
        if (!cpu.jit_tier2_trial_remaining || !cpu.running ||
            !monitor->enabled.load(std::memory_order_acquire) || trial.decision.load(std::memory_order_acquire) || cpu.pc < trial.lo || cpu.pc >= trial.hi || cpu.pc == cpu.jit_stop_pc) break;
        // A lease can expire at a Tier-1 constituent. Resume without compile
        // work in the timed interval, using retained immutable function refs.
        fn = nullptr;
        if (cpu.pc == monitor->head) {
            // The native edge lease can expire before reaching the head's
            // trial gate. Account for that completed iteration here as well.
            if (!--cpu.jit_tier2_trial_remaining) break;
            fn = cpu.jit_tier2_trial_fn;
        }
        else for (const auto& block : trial.baseline_blocks)
            if (block.first == cpu.pc) { fn = block.second; break; }
        if (!fn) break;
    }
    const uint64_t end = thread_cpu_ns();
    uint64_t ns = end > start && start ? end - start : 0;
    uint32_t work = cpu.jit_tier2_trial_work;
    uint64_t hash = cpu.jit_tier2_trial_hash;
    const uint64_t exit = cpu.pc;
    cpu.jit_tier2_trial = nullptr;
    bool finish = false, keep = false;
    {
        std::lock_guard<std::mutex> sample_lock(trial.mutex);
        if (trial.decision.load(std::memory_order_relaxed)) return exit;
        bool complete = !cpu.jit_tier2_trial_remaining && exit == monitor->head;
        bool exhausted = false;
        if (trial.linear) {
            complete = false;
            // A linear path cannot repeat inside one timed call without
            // executing unrelated work or replaying side effects. Pool only
            // completed live traversals; both arms pay identical timer/probe
            // overhead. This is conservative for very short superblocks.
            // Bound total timer calls even when CPU-local slots collide or
            // complete, comparable windows never arrive. Unproven code stays
            // in Tier-1; eight matched pairs are still required to keep it.
            exhausted = ++trial.linear_entries >= 8192;
            if (ns && work && cpu.running && exit == trial.linear_exit &&
                monitor->enabled.load(std::memory_order_acquire)) {
                slot.window_ns += ns;
                slot.window_work += work;
                slot.window_hash += hash;
                if (++slot.window_count < slot.sample_quota && !exhausted) return exit;
                complete = slot.window_count == slot.sample_quota;
                ns = slot.window_ns; work = slot.window_work; hash = slot.window_hash;
            }
            slot.window_ns = slot.window_hash = 0;
            slot.window_work = slot.window_count = 0;
        }
        ++trial.attempts;
        // A pair belongs to one vCPU. Reject short, different-path and
        // different-work samples instead of comparing unrelated executions.
        if (monitor->enabled.load(std::memory_order_acquire) && ns &&
            (candidate || ns >= 10000) && complete && work >= 128) {
            if (!candidate) {
                slot.baseline_ns = ns; slot.baseline_exit = exit;
                slot.baseline_work = work; slot.baseline_hash = hash;
            } else if (slot.baseline_exit == exit && slot.baseline_work == work && slot.baseline_hash == hash) {
                trial.ratios.push_back(double(ns) / double(slot.baseline_ns));
                tier2_measured_pairs.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (candidate) slot.baseline_ns = 0;
        else if (ns && ns < 10000 && complete && slot.sample_quota < 4096)
            slot.sample_quota *= 2; // amortize timer cost on very fast loops
        if (trial.ratios.size() >= 8) {
            auto ratios = trial.ratios;
            std::sort(ratios.begin(), ratios.end());
            const double median = (ratios[3] + ratios[4]) / 2;
            const unsigned wins = std::count_if(ratios.begin(), ratios.end(), [](double r) { return r < 0.97; });
            keep = median < 0.97 && wins >= 6;
            finish = true;
            if (tier2_trace_enabled())
                fprintf(stderr, "[tier2] measured head=%llx pairs=%zu candidate/baseline=%.4f wins=%u keep=%d\n",
                    (unsigned long long)monitor->head, ratios.size(), median, wins, keep);
        } else if (trial.attempts >= 128 || exhausted) finish = true; // insufficient proof keeps Tier-1
        if (finish) trial.decision.store(keep ? 1 : 2, std::memory_order_release);
    }
    if (finish) finish_tier2_trial(trial, keep);
    return exit;
}

void FrostJIT::redirect_tier2_head_locked(uint64_t pc) {
    auto target = blocks_.find(pc);
    if (target == blocks_.end()) return;
    refresh_mt_edges(pc);
    invalidate_dispatch_cache();
    if (mt_active()) return;
    const auto* fn = reinterpret_cast<const uint8_t*>(chain_skip_enabled() && target->second.chain_entry
        ? target->second.chain_entry : target->second.fn);
    auto refs = back_refs_.find(pc);
    if (refs == back_refs_.end()) return;
    make_writable();
    for (uint64_t source : refs->second) {
        auto src = blocks_.find(source);
        if (src == blocks_.end() || src->second.is_region) continue;
        for (auto edge : {std::pair<uint64_t, size_t>{src->second.chain_target_pc, src->second.chain_patch_off},
                           {src->second.taken_chain_target_pc, src->second.taken_chain_patch_off}}) {
            if (edge.first != pc || !edge.second || edge.second + 5 > CODE_BUF_SIZE) continue;
            code_buf_[edge.second] = 0xE9;
            const int32_t rel = static_cast<int32_t>(fn - (code_buf_ + edge.second + 5));
            memcpy(code_buf_ + edge.second + 1, &rel, 4);
            if (edge.second == src->second.chain_patch_off) src->second.chained = true;
            if (edge.second == src->second.taken_chain_patch_off) src->second.taken_chained = true;
        }
    }
    std::atomic_thread_fence(std::memory_order_release);
    make_executable();
}

// Private ST chains are never registered in the public back-reference graph.
// Revoke them before the second CPU starts; MT copies use data cells only.
void FrostJIT::revoke_tier2_samples_locked(Tier2Trial& trial) {
    if (trial.sampling_revoked) return;
    trial.sampling_revoked = true;
    for (auto* edge : trial.sampling_edges) {
        edge->live = false;
        edge->target.store(0, std::memory_order_release);
    }
    for (auto& item : trial.sampling_blocks) {
        auto& block = item.second;
        for (size_t off : {block.chain_patch_off, block.taken_chain_patch_off}) {
            if (!off || off + 5 > CODE_BUF_SIZE || mt_edge_slots_.count(off)) continue;
            if (code_buf_[off] != 0xE9) continue;
            make_writable();
            std::memset(code_buf_ + off, 0x90, 5);
            if (!chain_skip_enabled()) code_buf_[off] = 0xC3;
            make_executable();
        }
    }
}

void FrostJIT::finish_tier2_trial(Tier2Trial& trial, bool keep) {
    std::unique_lock<std::shared_mutex> lock(blocks_mutex_);
    auto head = blocks_.find(trial.monitor->head);
    if (head == blocks_.end() || head->second.tier2_monitor != trial.monitor ||
        !trial.monitor->enabled.load(std::memory_order_acquire)) return;
    revoke_tier2_samples_locked(trial);
    if (keep) {
        head->second = trial.candidate_entry;
        for (auto* edge : trial.baseline_edges) { edge->live = false; edge->target.store(0, std::memory_order_release); }
        tier2_measured_keeps.fetch_add(1, std::memory_order_relaxed);
    } else {
        tier2_measured_rejections.fetch_add(1, std::memory_order_relaxed);
        const size_t count = trial.baseline_blocks.size();
        const unsigned shorter = count > 4 ? 4 : (count > 2 ? 2 : 0);
        // Retry only an observed loss, not a path/work mismatch or timeout.
        // Recompile later, outside the measured interval, after fresh Tier-1
        // counters and branch profiles warm up. Strictly descending caps bound
        // search without consuming the selected-region phase-change budget.
        if (shorter && trial.ratios.size() >= 8 &&
            tier2_retrains_[trial.monitor->head] < 2 &&
            tier2_code_bytes.load(std::memory_order_relaxed) < 32ull * 1024 * 1024 &&
            tier2_compile_ns.load(std::memory_order_relaxed) < 2000000000ull) {
            tier2_block_limits_[trial.monitor->head] = shorter;
            retire_tier2_region_locked(trial.monitor->head);
            return;
        }
        // Preserve the monitor on the restored baseline so in-flight trial
        // wrappers are revoked by subsequent mapping invalidation too.
        const auto dependencies = head->second.inlined_ranges_;
        head->second = trial.baseline;
        head->second.inlined_ranges_ = dependencies;
        head->second.tier2_monitor = trial.monitor;
        for (auto* edge : trial.candidate_edges) { edge->live = false; edge->target.store(0, std::memory_order_release); }
        ++tier2_retrains_[trial.monitor->head];
    }
    redirect_tier2_head_locked(trial.monitor->head);
}
}  // namespace arm64emu
