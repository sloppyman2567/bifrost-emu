// Tier-2 phase feedback: windowed side-exit rates and region retraining.
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

// Tail-called only after the generated frame has been dismantled. All guest
// state is current, so cache revocation cannot strand live register mappings.
uint64_t tier2::feedback_exit(CPU* cpu, Emulator* emu, void* monitor) noexcept {
    try { if (emu) if (FrostJIT* jit = FrostJIT::running(*emu)) jit->tier2_region_feedback(*cpu, monitor); }
    catch (...) {} // never unwind into the generated caller
    return cpu->pc;
}
void FrostJIT::tier2_region_feedback(CPU& cpu, void* opaque) {
    auto* monitor = static_cast<Tier2RegionMonitor*>(opaque);
    if (cpu.jit_tier2_trial || (monitor && monitor->trial &&
        monitor->trial->decision.load(std::memory_order_acquire) != 1)) return;
    if (!monitor || !monitor->enabled.load(std::memory_order_acquire)) return;
    // The exit stub's rule (RegionCompiler.cpp): it comes here when the slot
    // belongs to another region, or with the strike that retires.
    auto& sample = cpu.jit_tier2_feedback[monitor->slot];
    if (sample.owner != monitor->identity) {
        sample.owner = monitor->identity;
        sample.pressure = sample.credit = sample.strikes = 0;
    }
    if (sample.strikes < kTier2FeedbackStrikes) {
        if (++sample.pressure < kTier2FeedbackWindowExits) return;
        const bool bad = sample.credit < monitor->min_window_iterations;
        sample.pressure = sample.credit = 0;
        sample.strikes = bad ? sample.strikes + 1 : 0;
        if (sample.strikes < kTier2FeedbackStrikes) return;
    }
    sample.strikes = 0;
    std::unique_lock<std::shared_mutex> lock(blocks_mutex_);
    auto head = blocks_.find(monitor->head);
    if (head == blocks_.end() || head->second.tier2_monitor != monitor ||
        !monitor->enabled.load(std::memory_order_relaxed)) return;
    tier2_block_limits_.erase(monitor->head); // new phase may favor a longer path
    ++tier2_retrains_[monitor->head];
    tier2_phase_retirements.fetch_add(1, std::memory_order_relaxed);
    // Version check, selection and revocation share one critical section:
    // a concurrent replacement cannot be accidentally retired afterward.
    retire_tier2_region_locked(monitor->head);
}

void FrostJIT::retire_tier2_region_locked(uint64_t pc) {
    const auto head = blocks_.find(pc);
    if (head == blocks_.end()) return;
    // Select the region and its Tier-1 constituents precisely. Instruction
    // bytes did not change, so neighboring regions need not be discarded.
    // The code index locates candidates: a rejected trial retires here, and a
    // walk of every block per retirement took 43% of an Android game's CPU.
    std::vector<uint64_t> victims{pc};
    const auto& ranges = head->second.inlined_ranges_;
    for (const auto& range : ranges) {
        const uint64_t range_end = range.first + range.second * 4ull;
        auto granule = code_index_.lower_bound(range.first & ~(CODE_INDEX_GRANULE - 1));
        for (; granule != code_index_.end() && granule->first < range_end; ++granule) {
            for (uint64_t candidate : granule->second) {
                const auto block = blocks_.find(candidate);
                if (block == blocks_.end() || block->second.is_region) continue;
                const uint64_t end = candidate + block->second.instr_count * 4ull;
                if (candidate < range_end && end > range.first) victims.push_back(candidate);
            }
        }
    }
    // A block listed on several granules or ranges is retired once.
    std::sort(victims.begin(), victims.end());
    victims.erase(std::unique(victims.begin(), victims.end()), victims.end());
    for (const auto& range : ranges)
        invalidate_tier2_profiles(range.first, range.first + range.second * 4ull);
    // Retirement forgets each block's predecessors. Left alone, they reach
    // these blocks through the dispatcher for good: 69x the dispatches on
    // CoreMark once rejected regions retried a shorter path.
    for (uint64_t victim : victims) {
        const auto refs = back_refs_.find(victim);
        if (refs != back_refs_.end())
            tier2_rebuild_sources_.insert(tier2_rebuild_sources_.end(), refs->second.begin(), refs->second.end());
    }
    tier2_rebuild_queue_.insert(tier2_rebuild_queue_.end(), victims.begin(), victims.end());
    tier2_settle_pending_.store(true, std::memory_order_release);
    retire_blocks_locked(victims);
}
}  // namespace arm64emu
