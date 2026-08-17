// jit/jit_cache.cpp — block cache + chain patching.
//
// The block cache maps guest PC → BlockEntry (compiled native code +
// metadata). Block chaining patches a 5-byte "chain slot" at the end
// of each block (initially `ret` + 4 NOPs) to `jmp rel32` → next
// block's entry, so straight-line code skips the C dispatcher.
//
// chain_target_pc_ == 0 means "not chainable" (indirect branch, SVC,
// conditional branch — runtime-dependent next PC).
//
// Back-reference index: maps target_pc → list of source_pcs whose
// chain_target_pc equals target_pc. Maintained incrementally at
// translate-time. Lets chain_back_references run in O(k) where k is
// the number of back-refs (typically 1-3), instead of O(N) scanning
// all blocks.
//
// Methods implemented here:
//   patch_chain           — patch a block's chain slot to jmp to target_fn
//   try_chain_block       — try to chain `entry` to its translated target
//   chain_back_references — patch all blocks whose target is `target_pc`
#include "jit/frostjit.hpp"
#include <atomic>
#include <cstdint>
#include <cstring>
namespace arm64emu {
bool FrostJIT::patch_chain(size_t chain_patch_off, const uint8_t* target_fn) {
    if (!code_buf_) return false;
    if (chain_patch_off + 5 > CODE_BUF_SIZE) return false;
    // Verify the slot still contains the unpatched pattern: `ret` + NOPs
    // (default) or 5 NOPs (chain-skip lease layout). If it's already
    // patched (0xE9), don't patch again.
    if (code_buf_[chain_patch_off] != (chain_skip_enabled() ? 0x90 : 0xC3)) return false;
    // Compute the relative displacement: target - (slot + 5).
    int32_t rel = static_cast<int32_t>(target_fn
                            - (code_buf_ + chain_patch_off + 5));
    // W^X: toggle the code buffer to writable before patching. The toggle
    // is cheap if the buffer is already writable (e.g., during translate_block).
    // If W^X is disabled, this is a no-op.
    make_writable();
    // Overwrite the 5 bytes with `jmp rel32` (0xE9 + 4-byte displacement).
    code_buf_[chain_patch_off] = 0xE9;
    memcpy(code_buf_ + chain_patch_off + 1, &rel, 4);
    // Memory barrier — ensures the writer's stores are globally visible
    // before any other thread (or the same core's instruction fetch)
    // observes the patched bytes. x86 stores are already TSO, but the
    // compiler could reorder; the barrier constrains the compiler too.
    std::atomic_thread_fence(std::memory_order_release);
    // W^X: toggle back to executable so the patched block can run.
    make_executable();
    return true;
}
void FrostJIT::patch_pending_calls(uint64_t target_pc, const uint8_t* target_fn) {
    if (!code_buf_ || !target_fn) return;
    auto it = pending_call_sites_.find(target_pc);
    if (it == pending_call_sites_.end() || it->second.empty()) return;
    // W^X: writable for the duration of all slot rewrites.
    make_writable();
    for (size_t off : it->second) {
        if (off + 5 > CODE_BUF_SIZE) continue;
        // The slot must still be an unpatched `E8` call; skip already-patched
        // sites (defensive — a target translates at most once).
        if (code_buf_[off] != 0xE8) continue;
        // Compute the relative displacement: target - (slot + 5).
        int32_t rel = static_cast<int32_t>(target_fn - (code_buf_ + off + 5));
        memcpy(code_buf_ + off + 1, &rel, 4);
    }
    it->second.clear();  // patched — drop the records
    std::atomic_thread_fence(std::memory_order_release);
    make_executable();
}
void FrostJIT::try_chain_block(uint64_t /*pc*/, BlockEntry& entry) {
    // Fall-through chain slot.
    if (!entry.chained && entry.chain_target_pc != 0) {
        auto it = blocks_.find(entry.chain_target_pc);
        if (it != blocks_.end() && it->second.fn != nullptr) {
            // Chain-skip: patch to the successor's chain_entry (past its
            // prologue) so the successor reuses this block's frame.
            const uint8_t* target = (chain_skip_enabled() && it->second.chain_entry)
                ? reinterpret_cast<const uint8_t*>(it->second.chain_entry)
                : reinterpret_cast<const uint8_t*>(it->second.fn);
            if (patch_chain(entry.chain_patch_off, target)) {
                entry.chained = true;
                block_chains_patched++;
            }
        }
    }
    // Taken-path chain slot (BRCOND/CBZ/CBNZ/TBZ/TBNZ): patch the ret at
    // the end of the taken path to jmp directly to the taken target block
    // once it's translated. This skips the dispatcher on loop-back edges.
    if (!entry.taken_chained && entry.has_taken_chain_slot && entry.taken_chain_target_pc != 0) {
        auto it = blocks_.find(entry.taken_chain_target_pc);
        if (it != blocks_.end() && it->second.fn != nullptr) {
            const uint8_t* target = (chain_skip_enabled() && it->second.chain_entry)
                ? reinterpret_cast<const uint8_t*>(it->second.chain_entry)
                : reinterpret_cast<const uint8_t*>(it->second.fn);
            if (patch_chain(entry.taken_chain_patch_off, target)) {
                entry.taken_chained = true;
                block_chains_patched++;
            }
        }
    }
}
void FrostJIT::chain_back_references(uint64_t target_pc) {
    // Patch any cached block whose chain_target_pc == target_pc.
    //
    // Uses the back_refs_ index for O(k) lookup (k = number of back-
    // refs, typically 1-3). Falls back to O(N) scan if the index is
    // missing for this target_pc — defensive, shouldn't happen since
    // the index is maintained at translate-time.
    auto target_it = blocks_.find(target_pc);
    if (target_it == blocks_.end()) return;
    const uint8_t* target_fn = (chain_skip_enabled() && target_it->second.chain_entry)
        ? reinterpret_cast<const uint8_t*>(target_it->second.chain_entry)
        : reinterpret_cast<const uint8_t*>(target_it->second.fn);
    if (target_fn == nullptr) return;
    auto try_patch = [&](uint64_t src_pc) {
        auto sit = blocks_.find(src_pc);
        if (sit == blocks_.end()) return;
        BlockEntry& entry = sit->second;
        // Fall-through slot.
        if (!entry.chained && entry.chain_target_pc == target_pc) {
            if (patch_chain(entry.chain_patch_off, target_fn)) {
                entry.chained = true;
                block_chains_patched++;
            }
        }
        // Taken-path slot.
        if (!entry.taken_chained && entry.has_taken_chain_slot
            && entry.taken_chain_target_pc == target_pc) {
            if (patch_chain(entry.taken_chain_patch_off, target_fn)) {
                entry.taken_chained = true;
                block_chains_patched++;
            }
        }
    };
    auto it = back_refs_.find(target_pc);
    if (it != back_refs_.end()) {
        for (uint64_t src_pc : it->second) {
            try_patch(src_pc);
        }
    }
    // Defensive fallback: also scan all blocks in case the index missed
    // any (e.g. blocks translated before the index was added). This is
    // O(N) but only runs when back_refs_ lacks the entry, which is rare.
    // Skip the scan if the index hit covered everything — for hot loops
    // the index always hits, so this stays O(k).
    if (it == back_refs_.end()) {
        for (auto& kv : blocks_) {
            try_patch(kv.first);
        }
    }
}
} // namespace arm64emu
