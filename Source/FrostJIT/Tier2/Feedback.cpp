// Tier-2 phase feedback: cold-exit pressure and region retraining.
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
    try { if (emu && emu->jit()) emu->jit()->tier2_region_feedback(*cpu, monitor); }
    catch (...) {} // never unwind into the generated caller
    return cpu->pc;
}
void FrostJIT::tier2_region_feedback(CPU& cpu, void* opaque) {
    auto* monitor = static_cast<Tier2RegionMonitor*>(opaque);
    if (cpu.jit_tier2_trial || (monitor && monitor->trial &&
        monitor->trial->decision.load(std::memory_order_acquire) != 1)) return;
    if (!monitor || !monitor->enabled.load(std::memory_order_acquire)) return;
    auto& sample = cpu.jit_tier2_feedback[monitor->slot];
    if (sample.owner != monitor->identity) {
        sample.owner = monitor->identity;
        sample.pressure = 0;
    }
    if (sample.credit) {
        sample.credit = 0;
        if (sample.pressure) --sample.pressure;
        return;
    }
    if (++sample.pressure < 64) return;
    sample.pressure = 0;
    std::unique_lock<std::shared_mutex> lock(blocks_mutex_);
    auto head = blocks_.find(monitor->head);
    if (head == blocks_.end() || head->second.tier2_monitor != monitor ||
        !monitor->enabled.load(std::memory_order_relaxed)) return;
    // Select the region and its Tier-1 constituents precisely. Instruction
    // bytes did not change, so neighboring regions need not be discarded.
    std::vector<uint64_t> victims;
    const auto& ranges = head->second.inlined_ranges_;
    for (const auto& kv : blocks_) {
        if (kv.first == monitor->head) { victims.push_back(kv.first); continue; }
        if (kv.second.is_region) continue;
        const uint64_t end = kv.first + kv.second.instr_count * 4ull;
        for (const auto& range : ranges)
            if (kv.first < range.first + range.second * 4ull && end > range.first) {
                victims.push_back(kv.first); break;
            }
    }
    for (const auto& range : ranges)
        invalidate_tier2_profiles(range.first, range.first + range.second * 4ull);
    monitor->enabled.store(0, std::memory_order_release);
    ++tier2_retrains_[monitor->head];
    tier2_phase_retirements.fetch_add(1, std::memory_order_relaxed);
    // Version check, selection and revocation share one critical section:
    // a concurrent replacement cannot be accidentally retired afterward.
    retire_blocks_locked(victims);
}
}  // namespace arm64emu
