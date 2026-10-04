// jit/jit_cache.cpp — block cache + chain patching.
//
// The block cache maps guest PC → BlockEntry (compiled native code +
// metadata). Block chaining patches a 5-byte "chain slot" at the end
// of each block (initially `ret` + 4 NOPs) to `jmp rel32` → next
// block's entry, so straight-line code skips the C dispatcher.
//
// chain_target_pc_ == 0 means "no fall-through chain" (indirect branch,
// SVC, unresolved). Conditional branches chain via their taken-path slot
// (taken_chain_target_pc_/taken_chain_patch_off_).
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
#include "core/memory.h"
#include "debug_flags.h"
#include <cstdio>
#include <atomic>
#include <cstdint>
#include <cstring>
namespace arm64emu {
std::atomic<uint64_t> FrostJIT::next_cache_generation_{1};
thread_local const FrostJIT* FrostJIT::tls_cache_owner_ = nullptr;
thread_local uint64_t FrostJIT::tls_cache_generation_ = 0;
uint64_t (*FrostJIT::lookup_or_translate(Emulator& emu, uint64_t pc))(CPU*, Emulator*) {
    {
        std::shared_lock<std::shared_mutex> lock(blocks_mutex_);
        auto it = blocks_.find(pc);
        if (it != blocks_.end()) return it->second.fn;
    }
    return translate_and_lookup(emu, pc);
}
uint64_t (*FrostJIT::lookup_only(uint64_t pc))(CPU*, Emulator*) {
    std::shared_lock<std::shared_mutex> lock(blocks_mutex_);
    return lookup_only_locked(pc);
}
uint64_t (*FrostJIT::lookup_only_locked(uint64_t pc))(CPU*, Emulator*) {
    auto it = blocks_.find(pc);
    return it == blocks_.end() ? nullptr : it->second.fn;
}
uint64_t (*FrostJIT::translate_and_lookup(Emulator& emu, uint64_t pc))(CPU*, Emulator*) {
    std::unique_lock<std::shared_mutex> lock(blocks_mutex_);
    auto it = blocks_.find(pc);
    if (it != blocks_.end()) return it->second.fn;
    return translate_block(emu, pc);
}
uint64_t (*FrostJIT::lookup_call_target_slow(Emulator& emu, uint64_t pc,
                                           int& instr_count))(CPU*, Emulator*) {
    note_table_lookup();
    // MT mode cannot promote or patch an existing block. A read-only hit
    // needs only a shared lock; compilation still rechecks exclusively.
    if (mt_active()) {
        std::shared_lock<std::shared_mutex> lock(blocks_mutex_);
        const auto it = blocks_.find(pc);
        if (it != blocks_.end()) {
            auto fn = it->second.fn;
            instr_count = fn ? it->second.instr_count : 1;
            tls_last_block_ = LastBlockCache{pc, fn, instr_count};
            inline_cache_insert(pc, fn, instr_count);
            return fn;
        }
    }
    std::unique_lock<std::shared_mutex> lock(blocks_mutex_);
    auto it = blocks_.find(pc);
    if (it == blocks_.end()) {
        translate_block(emu, pc);
        it = blocks_.find(pc);
    }
    auto fn = it == blocks_.end() ? nullptr : it->second.fn;
    int cnt = fn ? it->second.instr_count : 1;
    // Tier-2 metadata writes and publication use the exclusive table lock.
    if (fn && !mt_active() && tier2_enabled() && !it->second.tier2_hot_logged) {
        uint32_t n = ++it->second.exec_count;
        if (n >= tier2_hits_threshold()) {
            it->second.tier2_hot_logged = true;
            tier2_counter_disable(it->second.tier2_counter_off,
                                  it->second.tier2_counter_len);
            tier2_hot_heads.fetch_add(1, std::memory_order_relaxed);
            Tier2Trace trace = collect_tier2_trace(emu, pc);
            if (trace.ok) {
                uint64_t (*chain_fn)(CPU*, Emulator*) = nullptr;
                auto rfn = compile_tier2_region(emu, trace, &chain_fn);
                it = blocks_.find(pc);
                if (rfn && it != blocks_.end()) {
                    BlockEntry region;
                    region.fn = rfn;
                    if (chain_skip_enabled()) region.chain_entry = chain_fn;
                    region.instr_count = static_cast<int>(trace.total_insts);
                    region.exec_count = it->second.exec_count;
                    region.tier2_hot_logged = true;
                    region.ends_with_branch = true;
                    region.chained = region.taken_chained = true;
                    region.verified_once = true;
                    it->second = region;
                    fn = rfn;
                    cnt = region.instr_count;
                }
            }
        }
    }
    tls_last_block_ = LastBlockCache{pc, fn, cnt};
    inline_cache_insert(pc, fn, cnt);
    instr_count = cnt;
    return fn;
}
void FrostJIT::dump_code_cache_stats() {
    flush_stat_tls();
    std::shared_lock<std::shared_mutex> lock(blocks_mutex_);
    fprintf(stderr, "[JIT] code-cache: used=%zu capacity=%zu limit=%zu blocks=%zu growths=%llu overflows=%llu budget_fallbacks=%llu mt=%d\n",
            code_buf_used_, code_capacity_, code_buf_limit_, blocks_.size(),
            (unsigned long long)code_cache_growths.load(std::memory_order_relaxed),
            (unsigned long long)code_cache_overflows.load(std::memory_order_relaxed),
            (unsigned long long)code_cache_budget_fallbacks.load(std::memory_order_relaxed),
            mt_active() ? 1 : 0);
    if (dispatch_stats_enabled_) {
        fprintf(stderr, "[JIT] dispatch-cache: last=%llu way0=%llu way1=%llu table=%llu evictions=%llu sets=%d ways=2\n",
                (unsigned long long)dispatch_last_hits_.load(std::memory_order_relaxed),
                (unsigned long long)dispatch_way0_hits_.load(std::memory_order_relaxed),
                (unsigned long long)dispatch_way1_hits_.load(std::memory_order_relaxed),
                (unsigned long long)dispatch_table_lookups_.load(std::memory_order_relaxed),
                (unsigned long long)dispatch_evictions_.load(std::memory_order_relaxed),
                INLINE_CACHE_SETS);
    }
}
bool FrostJIT::patch_chain(size_t chain_patch_off, const uint8_t* target_fn) {
    if (!code_buf_) return false;
    // MT-safe mode: never modify live code (see enter_multithreaded()).
    if (mt_active_.load(std::memory_order_relaxed)) return false;
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
    // Thread-safe publication: write the 4 displacement bytes FIRST, then
    // the opcode byte LAST with release ordering. An executing thread
    // fetching these 5 bytes concurrently then observes either:
    //   - the old opcode (ret/0xC3 or NOP/0x90) → runs the unpatched
    //     slot (returns to the dispatcher — safe), or
    //   - 0xE9 with a fully-written rel32 → jumps to the target (safe).
    // A torn rel32 with a premature 0xE9 (the old order: opcode first)
    // jumps to a wild address. x86 stores are TSO-ordered and the fence
    // constrains the compiler, so the opcode store is globally observed
    // after the rel32 stores. Single-byte stores are atomic on x86.
    memcpy(code_buf_ + chain_patch_off + 1, &rel, 4);
    std::atomic_thread_fence(std::memory_order_release);
    code_buf_[chain_patch_off] = 0xE9;
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
    // MT-safe mode: the call slot already dispatches through jit_call_helper
    // (slow_path), so leaving it unpatched is correct — just slower.
    if (mt_active_.load(std::memory_order_relaxed)) return;
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
    // MT-safe mode: no runtime code writes (see enter_multithreaded()).
    if (mt_active_.load(std::memory_order_relaxed)) return;
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
    // ── 1.5.5-alpha: strip dead pstate materializes into this block ──
    // If this freshly-translated block provably never reads pstate
    // (reads_pstate_before_set == false), any already-compiled predecessor
    // that recorded a pending materialize site targeting it can drop that
    // materialize: the successor never consumes pstate, so the write is
    // dead work on every incoming edge. Patch each recorded region to a
    // 5-byte `jmp rel32` that hops past it — the jcc lands exactly on
    // code_off, and the region is followed by `mov rax, next_pc`, so a
    // jump over it is cheap and safe (cheaper than NOPing the ~25-89 bytes,
    // which would still cost fetch/decode bandwidth every iteration).
    // Mirrors patch_chain's W^X + release-fence pattern; the region is
    // never re-visited (sites are erased below), so this runs once.
    if (!target_it->second.reads_pstate_before_set) {
        auto bref = back_refs_.find(target_pc);
        if (bref != back_refs_.end()) {
            bool patched_any = false;
            for (uint64_t src_pc : bref->second) {
                auto sit = blocks_.find(src_pc);
                if (sit == blocks_.end()) continue;
                auto& mats = sit->second.pending_flag_mat_;
                for (auto itm = mats.begin(); itm != mats.end();) {
                    if (itm->target_pc != target_pc) {
                        ++itm;
                        continue;
                    }
                    if (itm->code_len >= 5 &&
                        itm->code_off + itm->code_len <= CODE_BUF_SIZE) {
                        if (!patched_any) {
                            make_writable();
                            patched_any = true;
                        }
                        int32_t rel = static_cast<int32_t>(itm->code_len - 5);
                        // Ordered publication (rel32 first, opcode last) —
                        // same torn-fetch rationale as patch_chain above.
                        memcpy(code_buf_ + itm->code_off + 1, &rel, 4);
                        std::atomic_thread_fence(std::memory_order_release);
                        code_buf_[itm->code_off] = 0xE9;  // jmp rel32
                    }
                    itm = mats.erase(itm);
                }
            }
            if (patched_any) {
                std::atomic_thread_fence(std::memory_order_release);
                make_executable();
            }
        }
    }
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
void FrostJIT::invalidate_range(uint64_t addr, uint64_t size) {
    if (size == 0) return;
    const uint64_t lo = addr & ~Memory::PAGE_MASK;
    uint64_t end;
    if (__builtin_add_overflow(addr, size, &end)) { invalidate_all(); return; }
    uint64_t hi;
    if (__builtin_add_overflow(end, Memory::PAGE_MASK, &hi))
        hi = UINT64_MAX & ~Memory::PAGE_MASK;
    else
        hi &= ~Memory::PAGE_MASK;
    if (hi <= lo) { invalidate_all(); return; }
    const uint8_t unpatched = chain_skip_enabled() ? 0x90 : 0xC3;
    std::unique_lock<std::shared_mutex> g(blocks_mutex_);
    // 1. Collect victims: blocks whose [pc, pc+instr_count*4) overlaps [lo, hi).
    std::vector<uint64_t> victims;
    victims.reserve(4);
    auto ranges_overlap = [&](uint64_t b, uint64_t e) { return b < hi && e > lo; };
    for (const auto& kv : blocks_) {
        const uint64_t b = kv.first;
        const uint64_t e = b + static_cast<uint64_t>(kv.second.instr_count) * 4;
        if (ranges_overlap(b, e)) { victims.push_back(b); continue; }
        // Inlined BL-leaf callees live outside the caller's contiguous
        // range — check those too.
        for (const auto& r : kv.second.inlined_ranges_) {
            if (ranges_overlap(r.first, r.first + static_cast<uint64_t>(r.second) * 4)) {
                victims.push_back(b);
                break;
            }
        }
    }
    if (victims.empty()) return;
    // Mapping churn in data-only pages cannot change a cached translation.
    // Revoke all CPUs' dispatch entries only when compiled code is affected.
    invalidate_dispatch_cache();
    auto unpatch_slot = [&](size_t off) {
        if (off + 5 > CODE_BUF_SIZE) return;
        if (code_buf_[off] != 0xE9) return;  // not patched — leave alone
        code_buf_[off] = unpatched;  // single-byte atomic store
    };
    make_writable();
    for (uint64_t vpc : victims) {
        // 2. Unpatch predecessors chained to this victim.
        auto bref = back_refs_.find(vpc);
        if (bref != back_refs_.end()) {
            for (uint64_t src_pc : bref->second) {
                auto sit = blocks_.find(src_pc);
                if (sit == blocks_.end()) continue;
                BlockEntry& se = sit->second;
                if (se.chained && se.chain_target_pc == vpc) {
                    unpatch_slot(se.chain_patch_off);
                    se.chained = false;
                }
                if (se.taken_chained && se.has_taken_chain_slot
                    && se.taken_chain_target_pc == vpc) {
                    unpatch_slot(se.taken_chain_patch_off);
                    se.taken_chained = false;
                }
            }
            back_refs_.erase(bref);
        }
        // 3. Erase the victim (code bytes leak — never reused, see header).
        // Keep pending_call_sites_ so a later retranslate re-patches
        // still-slow-path callers to the fresh fn.
        blocks_.erase(vpc);
        // 4. Drop this thread's fast-path entries for the victim.
        if (tls_last_block_.pc == vpc) tls_last_block_ = LastBlockCache{};
        inline_cache_erase(vpc);
    }
    std::atomic_thread_fence(std::memory_order_release);
    make_executable();
}
void FrostJIT::invalidate_all() {
    const uint8_t unpatched = chain_skip_enabled() ? 0x90 : 0xC3;
    std::unique_lock<std::shared_mutex> g(blocks_mutex_);
    invalidate_dispatch_cache();
    if (blocks_.empty()) return;
    make_writable();
    for (auto& kv : blocks_) {
        BlockEntry& e = kv.second;
        if (e.chained && e.chain_patch_off + 5 <= CODE_BUF_SIZE
            && code_buf_[e.chain_patch_off] == 0xE9) {
            code_buf_[e.chain_patch_off] = unpatched;
            e.chained = false;
        }
        if (e.taken_chained && e.has_taken_chain_slot
            && e.taken_chain_patch_off + 5 <= CODE_BUF_SIZE
            && code_buf_[e.taken_chain_patch_off] == 0xE9) {
            code_buf_[e.taken_chain_patch_off] = unpatched;
            e.taken_chained = false;
        }
    }
    blocks_.clear();
    back_refs_.clear();
    // Keep pending_call_sites_ (see invalidate_range rationale).
    tls_last_block_ = LastBlockCache{};
    for (auto& set : tls_inline_cache_) set = InlineCacheSet{};
    std::atomic_thread_fence(std::memory_order_release);
    make_executable();
}
void FrostJIT::enter_multithreaded() {
    // Sticky: only the FIRST transition into multithreaded execution does
    // the purge. After that every writer checks mt_active_ and bails.
    bool expected = false;
    if (!mt_active_.compare_exchange_strong(expected, true)) return;
    if (dbg().jit_verify)
        fprintf(stderr, "[VERIFY] suspended: concurrent guest execution requires oracle tests; shared-memory replay is unsafe\n");
    // Purge every compiled block. This drops all chain slots (patch_chain is
    // disabled from now on, so none are re-created) AND any tier-2 region:
    // a region carries an in-code hot-head counter that writes the code page
    // on every entry, which no compile-time gate can disable retroactively.
    //
    // Safe without quiescing: the spawning vCPU is the only running one (the
    // guest was single-threaded until this moment) and it is inside a
    // syscall, not executing JIT code. Blocks re-translated after this point
    // emit no chain slots and no tier-2 regions, so the RWX buffer is never
    // modified while another core executes it — the x86 cross-modifying-code
    // hazard that produced the pc=0 torn-jump abort.
    invalidate_all();
}
} // namespace arm64emu
