// Tier-2 branch-edge profiles and trial work probes emitted into Tier-1 code.
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

// Emitted after all guest state is flushed at a Tier-1 conditional exit.
// R11 is free here, RAX retains next_pc, and NZCV is already materialized.
void FrostJIT::emit_tier2_edge_profile(bool taken) {
    if (!tier2_branch_profile_) return;
    emit_mov_imm64(R11, reinterpret_cast<uint64_t>(tier2_branch_profile_));
    const uint8_t offset = taken ? offsetof(Tier2BranchProfile, taken)
                                 : offsetof(Tier2BranchProfile, fallthrough);
    // Two instructions and no branch: this code runs on every execution of
    // every profiled branch until the block settles. A 64-bit counter
    // cannot wrap; a profile of a replaced translation just goes unread.
    // Not locked in MT: these counts only feed a bias heuristic, so an update
    // lost to a race changes nothing, and a locked RMW on every branch does.
    emit_byte(0x49); emit_byte(0xFF); emit_byte(0x43); emit_byte(offset); // inc qword [r11+off]
}
void FrostJIT::invalidate_tier2_profiles(uint64_t lo, uint64_t hi) {
    tier2_profiles_by_pc_.erase(tier2_profiles_by_pc_.lower_bound(lo),
                                tier2_profiles_by_pc_.lower_bound(hi));
    tier2_settled_.erase(tier2_settled_.lower_bound(lo), tier2_settled_.lower_bound(hi));
}
void FrostJIT::clear_tier2_profiles() {
    tier2_profiles_by_pc_.clear();
    tier2_settled_.clear();
}
void FrostJIT::tier2_settle_locked(uint64_t pc) {
    if (!tier2_settled_.insert(pc).second) return;
    tier2_settle_queue_.push_back(pc);
    tier2_settle_pending_.store(true, std::memory_order_release);
}
// Replace instrumented translations with clean ones. Their profiles stay, for
// regions that pass through them. Retirement unlinks every predecessor and
// forgets them, and the dispatcher only relinks a broken fall-through edge: a
// hot block left to that ran through dispatch on every entry, 4x slower.
void FrostJIT::tier2_settle_queued(Emulator& emu) {
    std::vector<uint64_t> victims, sources;
    {
        std::unique_lock<std::shared_mutex> lock(blocks_mutex_);
        tier2_settle_pending_.store(false, std::memory_order_relaxed);
        for (uint64_t pc : tier2_settle_queue_) {
            const auto block = blocks_.find(pc);
            if (block == blocks_.end() || block->second.is_region || !block->second.tier2_counter_off) continue;
            victims.push_back(pc);
            const auto refs = back_refs_.find(pc);
            if (refs != back_refs_.end()) sources.insert(sources.end(), refs->second.begin(), refs->second.end());
        }
        tier2_settle_queue_.clear();
        tier2_settling_ = true;
        retire_blocks_locked(victims);
    }
    for (uint64_t pc : victims) {
        BlockEntry entry;
        translate_entry(emu, pc, entry);
    }
    std::unique_lock<std::shared_mutex> lock(blocks_mutex_);
    tier2_settling_ = false;
    sources.insert(sources.end(), victims.begin(), victims.end());
    for (uint64_t pc : sources) {
        const auto block = blocks_.find(pc);
        if (block != blocks_.end()) try_chain_block(pc, block->second);
    }
    for (uint64_t pc : victims) chain_back_references(pc);
}

static uint64_t tier2_probe_hash(uint64_t pc) {
    pc ^= pc >> 33; pc *= 0xff51afd7ed558ccdull;
    pc ^= pc >> 33; pc *= 0xc4ceb9fe1a85ec53ull;
    pc ^= pc >> 33;
    return pc;
}

// Only private sampling copies carry probes. Public Tier-1 and selected
// region code has no dormant guard, counter, or scratch-register save.
void FrostJIT::emit_tier2_work_probe(uint64_t pc, unsigned count) {
    if (!tier2_sampling_compile_) return;
    emit_push(RAX); emit_byte(0x9C);
    emit_byte(0x81); emit_byte(0x83);
    emit_u32(offsetof(CPU, jit_tier2_trial_work)); emit_u32(count);
    emit_mov_imm_to_rax(tier2_probe_hash(pc));
    emit_byte(0x48); emit_byte(0x01); emit_byte(0x83);
    emit_u32(offsetof(CPU, jit_tier2_trial_hash));
    emit_byte(0x9D); emit_pop(RAX);
}
}  // namespace arm64emu
