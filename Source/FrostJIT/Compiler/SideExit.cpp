// SideExit.cpp — a conditional exit out of the middle of a block.
//
// A loop whose only branches besides its back edge are CBZ/CBNZ exits (a null-terminated list walk, a search that stops on a
// match) used to be split at every exit, so each iteration stored and reloaded its guest registers at the block edges. Such a
// loop is now one block: translate_block keeps going past each exit (the IR op carries aux = 1), the test and its jcc stay in the
// hot path, and the exit itself is emitted after the block from the allocator layout captured at the branch. It commits every
// mapped guest register (a loop pin may be ahead of cpu.regs[]) and the pinned vectors, then takes a chain slot to the target.
//
// Each exit's slot is published as a stub entry in blocks_ under a synthetic key (side_exit_key) whose only successor is the exit
// target, so patching, back references, MT cells, unpatching and invalidation need no second kind of slot.
#include "FrostJIT/Compiler/BlockAnalysis.hpp"
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include "System/Core/DebugFlags.hpp"
#include "System/Core/Emulator.hpp"
#include <cstdint>
#include <vector>
#include <algorithm>
#include <cstring>

namespace arm64emu {

// The extent in instructions of a loop that starts at start_pc and ends in a branch back to it, when every other branch inside is
// a CBZ/CBNZ whose target lies outside the loop; the exits are listed in `exits`. 0 for anything else, including a loop with no
// exit (selfloop_extent covers that one). Like selfloop_extent it reads plainly mapped executable pages directly and is only a
// hint: the words are translated (and recorded) by translate_block itself.
int FrostJIT::side_exit_loop_extent(uint64_t start_pc, int limit, std::vector<uint64_t>& exits) {
    exits.clear();
    if (!window_base_ || !guest_memory_ || start_pc >= (1ull << 48)) return 0;
    const uint64_t window = guest_memory_->window_size();
    const auto* flags = guest_memory_->direct_page_flags();
    constexpr uint8_t need = Memory::GUEST_PAGE_MAPPED | Memory::GUEST_PROT_EXEC;
    std::vector<std::pair<uint64_t, uint64_t>> candidates;  // exit pc, target
    uint64_t pc = start_pc;
    for (int n = 0; n < limit; ++n, pc += 4) {
        const uint64_t at = fetch_address(pc);
        if ((at & 3) || at > window - 4) return 0;
        const uint8_t f = flags[at / Memory::PAGE_SIZE].load(std::memory_order_acquire);
        if ((f & need) != need || (f & Memory::GUEST_PAGE_UFFD_MISSING)) return 0;
        uint32_t word;
        std::memcpy(&word, window_base_ + at, sizeof word);
        DecodedInst d;
        if (!decode(d, word) || jit_analysis::instr_will_call_interp(d)) return 0;
        const uint64_t target = pc + static_cast<uint64_t>(d.imm);
        switch (d.cls) {
            case InstClass::B: case InstClass::Bcond: case InstClass::CBZ: case InstClass::CBNZ:
            case InstClass::TBZ: case InstClass::TBNZ:
                if (target == start_pc) {
                    // The back edge ends the loop. Every exit must leave it: a target inside would need a label in the block.
                    const uint64_t last = pc;
                    for (const auto& [exit_pc, exit_target] : candidates)
                        if (exit_target >= start_pc && exit_target <= last) return 0;
                    for (const auto& c : candidates) exits.push_back(c.first);
                    return exits.empty() ? 0 : n + 1;
                }
                if (d.cls != InstClass::CBZ && d.cls != InstClass::CBNZ) return 0;
                candidates.push_back({pc, target});
                break;
            case InstClass::BL: case InstClass::BLR: case InstClass::BR: case InstClass::RET:
            case InstClass::SVC: case InstClass::BRK: case InstClass::UNKNOWN:
                return 0;
            default:
                break;
        }
    }
    return 0;
}

// Test the register and branch out when it hits: the same compare as BRCOND_ZERO, but the block goes on. NZCV is committed first
// so the exit (and every later one) sees it in pstate.
int FrostJIT::compile_cbz_side_exit(const IRInst& inst) {
    const BrCondZeroParams bp = inst.brcond_zero_params();
    clobber_flags(false);
    const int s1 = ensure_vreg(inst.src1);
    if (const uint8_t prefix = rex(bp.sf, s1 >= 8, false, s1 >= 8); prefix != 0x40) emit_byte(prefix);
    emit_byte(0x85); emit_byte(modrm(3, s1 & 7, s1 & 7));  // test s1, s1
    const size_t jcc = emit_jcc_rel32_placeholder(bp.cond == 0 ? 4 /*JE*/ : 5 /*JNE*/);
    deferred_side_exits_.push_back({jcc, bp.target, capture_guard_state()});
    block_has_side_exit_ = true;
    return 0;
}

// The exits, after the block. Each one runs on the layout captured at its branch: every mapped guest register is stored (a pin is
// ahead of memory and statically clean, as in emit_memory_fault_exit), the pinned vectors are written back, and the chain slot of
// emit_taken_path_epilogue takes the target. The compile-time state is swapped back afterwards.
void FrostJIT::flush_deferred_side_exits() {
    const bool had_slot = has_taken_chain_slot_;
    const size_t saved_off = taken_chain_patch_off_;
    const uint64_t saved_target = taken_chain_target_pc_;
    for (const DeferredSideExit& exit : deferred_side_exits_) {
        patch_jcc_rel32(exit.branch, static_cast<int32_t>(code_buf_used_ - (exit.branch + 6)));
        GuardState state = exit.state;
        swap_guard_state(state);
        for (int v = 0; v < 32; ++v)
            if (vreg_home_[v] >= 0) emit_store_arm(v, vreg_home_[v]);
        vec_cache_writeback_all_pinned(false);
        // RAX is about to carry the target PC: its occupant is already stored above, and must not be stored again as a PC.
        clobber_host_reg(RAX);
        taken_chain_target_pc_ = exit.target;
        edge_source_override_ = side_exit_key(current_start_pc_, compiled_side_exits_.size());
        emit_taken_path_epilogue();
        edge_source_override_ = 0;
        compiled_side_exits_.push_back({exit.target, taken_chain_patch_off_});
        swap_guard_state(state);
    }
    deferred_side_exits_.clear();
    has_taken_chain_slot_ = had_slot;
    taken_chain_patch_off_ = saved_off;
    taken_chain_target_pc_ = saved_target;
}

// Called by publish_translation: one stub entry per exit, so the existing chaining finds each slot through back_refs_.
void FrostJIT::register_side_exit_stubs(uint64_t start_pc) {
    auto it = side_exit_table_.find(start_pc);
    if (it == side_exit_table_.end()) return;
    std::vector<uint64_t> keys;
    for (size_t k = 0; k < it->second.size(); ++k) {
        const uint64_t key = side_exit_key(start_pc, k);
        BlockEntry stub;
        stub.chain_target_pc = it->second[k].target;
        stub.chain_patch_off = static_cast<uint32_t>(it->second[k].slot);
        stub.verified_once = true;
        blocks_[key] = stub;
        add_back_ref(stub.chain_target_pc, key);
        keys.push_back(key);
    }
    side_exit_table_.erase(it);
    if (!dbg().no_chain)
        for (uint64_t key : keys) try_chain_block(key, blocks_[key]);
    side_stubs_[start_pc] = std::move(keys);
}

}  // namespace arm64emu
