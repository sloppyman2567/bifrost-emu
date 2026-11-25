// HotSide.cpp — Carry follows the hot side of a mid-block exit.
//
// Carry formation continues a block past a forward conditional branch along its fall-through and makes the branch's taken side an exit.
// When the branch is taken more often than not, the common path leaves through an exit tail (stores, an NZCV commit, a chain slot) and
// the code it goes on to runs in a block of its own. A block that Carry formed exits in therefore starts with a short profile: a budget
// of entries (kCarryProfileBudget) and a count of how often each exit was taken. When the budget runs out the block leaves for the
// dispatcher before it runs (nothing is dirty at an entry), carry_trip picks the first exit taken more than half of the time, and the
// block is retired. The next translation follows that branch's target instead (BlockCompiler.cpp): the exit leaves on the fall-through
// side, and the hot code is copied in even where a block already starts, so the register state stays in host registers across it.
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include "System/Core/DebugFlags.hpp"
#include <algorithm>

namespace arm64emu {

extern "C" void jit_carry_trip(FrostJIT* jit, FrostJIT::CarryProfile* profile) { jit->carry_trip(profile); }

// The entry budget: `sub dword [rax], 1; jle trip`. RAX is free at an entry.
void FrostJIT::emit_carry_budget_check() {
    if (!carry_profile_) return;
    emit_mov_imm64(RAX, reinterpret_cast<uint64_t>(&carry_profile_->budget));
    emit_byte(0x83); emit_byte(0x28); emit_byte(0x01);
    carry_trip_jcc_ = emit_jcc_rel32_placeholder(0xE);
}

// The out-of-line half: the block gives up its entry, hands the profile to carry_trip and returns to the dispatcher at its own pc.
// Nothing has run, so no guest state is held in the host registers a copy does not mirror.
void FrostJIT::emit_carry_trip_stub(uint64_t start_pc) {
    if (carry_trip_jcc_ == SIZE_MAX) return;
    patch_jcc_rel32(carry_trip_jcc_, static_cast<int32_t>(code_buf_used_ - (carry_trip_jcc_ + 6)));
    const bool calls_out = block_calls_out_;  // the helper reads no cpu.pc: no entry of this block needs to store it
    emit_mov_imm64(RDI, reinterpret_cast<uint64_t>(this));
    emit_mov_imm64(RSI, reinterpret_cast<uint64_t>(carry_profile_));
    emit_call_aligned(reinterpret_cast<void*>(&jit_carry_trip), 0, false);
    block_calls_out_ = calls_out;
    emit_mov_imm_to_rax(start_pc);
    emit_store(CPU_REG, PC_OFF, RAX);
    emit_frame_return(false);
}

void FrostJIT::carry_trip(CarryProfile* p) {
    if (p->tripped.exchange(true)) return;  // another thread's entry got here first; this one just leaves
    carry_trips.fetch_add(1, std::memory_order_relaxed);
    const uint64_t entries = static_cast<uint64_t>(kCarryProfileBudget) + (p->budget < 0 ? static_cast<uint64_t>(-static_cast<int64_t>(p->budget)) : 0);
    // Exit i is evaluated only when the exits before it were not taken.
    uint64_t left = entries;
    int pick = -1;
    for (int i = 0; i < p->exits; ++i) {
        const uint64_t taken = p->taken[i];
        if (pick < 0 && taken * 2 > left) pick = i;
        left = left > taken ? left - taken : 0;
    }
    lock_table();
    std::unique_lock<std::shared_mutex> table(blocks_mutex_, std::adopt_lock);
    const uint64_t block_pc = p->block_pc;
    uint8_t& rounds = carry_rounds_[block_pc];
    ++rounds;
    bool changed = false;
    if (pick >= 0) {
        const uint64_t branch = block_pc + p->branch_off[pick];
        uint8_t& toggles = carry_toggles_[branch];
        if (toggles < 2) {
            ++toggles;
            if (!carry_follow_taken_.erase(branch)) carry_follow_taken_.insert(branch);
            carry_flips.fetch_add(1, std::memory_order_relaxed);
            changed = true;
        }
    }
    // A block whose last round changed nothing, or that had its rounds, is translated without profile code from now on.
    if (!changed || rounds >= 3) carry_done_.insert(block_pc);
    log_invalidation_for_compiles(block_pc, block_pc + 4);
    if (blocks_.count(block_pc)) {
        // Retiring unchains the predecessors and forgets them; they are chained again only if the new translation finds them in back_refs_.
        std::vector<uint64_t> predecessors;
        if (const auto refs = back_refs_.find(block_pc); refs != back_refs_.end()) predecessors = refs->second;
        retire_blocks_locked({block_pc});
        if (!predecessors.empty()) back_refs_[block_pc] = std::move(predecessors);
    }
}

}  // namespace arm64emu
