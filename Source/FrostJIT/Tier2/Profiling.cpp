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
    emit_byte(0x41); emit_byte(0x83); emit_byte(0x3B); emit_byte(0); // cmp [r11],0
    const size_t disabled = emit_jcc_rel32_placeholder(4);
    const uint8_t offset = taken ? offsetof(Tier2BranchProfile, taken)
                                 : offsetof(Tier2BranchProfile, fallthrough);
    // Each outcome stops accumulating at 4096 (MT can overshoot by the
    // number of concurrent CPUs). No counter wraps or unbounded profiling.
    emit_byte(0x41); emit_byte(0x81); emit_byte(0x7B); emit_byte(offset);
    emit_u32(4096);
    const size_t saturated = emit_jcc_rel32_placeholder(3); // JAE
    if (mt_active()) emit_byte(0xF0);
    emit_byte(0x41); emit_byte(0xFF); emit_byte(0x43); emit_byte(offset); // inc [r11+off]
    for (size_t p : {disabled, saturated})
        patch_jcc_rel32(p, static_cast<int32_t>(code_buf_used_ - (p + 6)));
}
void FrostJIT::invalidate_tier2_profiles(uint64_t lo, uint64_t hi) {
    for (auto it = tier2_profiles_by_pc_.begin(); it != tier2_profiles_by_pc_.end();) {
        if (it->first >= lo && it->first < hi) {
            it->second->enabled.store(0, std::memory_order_release);
            it = tier2_profiles_by_pc_.erase(it);
        } else ++it;
    }
}
void FrostJIT::clear_tier2_profiles() {
    for (auto& profile : tier2_branch_profiles_)
        profile->enabled.store(0, std::memory_order_release);
    tier2_profiles_by_pc_.clear();
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
