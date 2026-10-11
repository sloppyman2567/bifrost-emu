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
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include "System/Memory/Memory.hpp"
#include "System/Core/CPU.hpp"
#include "System/Core/DebugFlags.hpp"
#include "Bifrost/Version.hpp"  // CODENAME
#include <sys/mman.h>
#include <algorithm>
#include <cerrno>
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
    // MT promotion is driven by atomic entry counters. Read-only table hits
    // need only a shared lock; compilation still rechecks exclusively.
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
    // The generated entry counter owns hotness for eligible heads. Keep
    // publication in tier2_fire_region so all entries retain constituent
    // ranges, revoke dispatch caches and redirect incoming chains together.
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
    fprintf(stderr, "[JIT] Tier-2 measured payoff: pairs=%llu keeps=%llu rejects=%llu\n",
        (unsigned long long)tier2_measured_pairs.load(), (unsigned long long)tier2_measured_keeps.load(),
        (unsigned long long)tier2_measured_rejections.load());
    fprintf(stderr, "[JIT] MT publication: edges=%llu regions=%llu\n",
        (unsigned long long)mt_edges_published_.load(), (unsigned long long)mt_regions_published_.load());
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
// Emit an immutable load/test/jump edge. The dispatcher supplies a bounded
// lease; expiry or a host callback sentinel returns through the cold tail.
FrostJIT::MtEdge* FrostJIT::create_chain_edge(uint64_t target_pc, bool cold_entry, size_t off) {
    auto cell = std::make_unique<MtEdge>();
    cell->source_pc = current_start_pc_;
    cell->sampling_private = tier2_sampling_compile_;
    cell->cold_entry = cold_entry;
    cell->target_pc = target_pc;
    MtEdge* edge = cell.get();
    mt_edges_.push_back(std::move(cell));
    mt_edge_refs_[target_pc].push_back(edge);
    mt_source_refs_[current_start_pc_].push_back(edge);
    mt_edge_slots_[off] = edge;
    return edge;
}
size_t FrostJIT::emit_mt_chain_slot(uint64_t target_pc, bool cold_entry) {
    const size_t off = code_buf_used_;
    if (!target_pc || dbg().no_chain) return off;
    MtEdge* edge = create_chain_edge(target_pc, cold_entry, off);
    // cmp rax,[rdi+jit_stop_pc]; je cold exit
    emit_byte(0x48); emit_byte(0x3B); emit_byte(0x87);
    emit_u32(offsetof(CPU, jit_stop_pc));
    size_t stop = emit_jcc_rel32_placeholder(0x4);
    // sub dword [rdi+jit_chain_budget],1; jbe cold exit (also handles zero).
    emit_byte(0x83); emit_byte(0xAF);
    emit_u32(offsetof(CPU, jit_chain_budget)); emit_byte(1);
    size_t budget = emit_jcc_rel32_placeholder(0x6);
    emit_mov_imm64(R11, reinterpret_cast<uint64_t>(&edge->target));
    emit_load(R11, R11, 0);
    emit_byte(0x4D); emit_byte(0x85); emit_byte(0xDB); // test r11,r11
    size_t empty = emit_jcc_rel32_placeholder(0x4);
    emit_byte(0x41); emit_byte(0xFF); emit_byte(0xE3); // jmp r11
    for (size_t p : {stop, budget, empty})
        patch_jcc_rel32(p, static_cast<int32_t>(code_buf_used_ - (p + 6)));
    return off;
}
void FrostJIT::refresh_mt_edges(uint64_t pc) {
    auto refs = mt_edge_refs_.find(pc);
    if (refs == mt_edge_refs_.end()) return;
    auto block = blocks_.find(pc);
    uint64_t fn = 0;
    if (block != blocks_.end() && block->second.fn && !block->second.interp_only)
        fn = reinterpret_cast<uint64_t>(chain_skip_enabled() && block->second.chain_entry
            ? block->second.chain_entry : block->second.fn);
    for (MtEdge* edge : refs->second) {
        if (!edge->live || edge->sampling_private) continue;
        uint64_t target = edge->cold_entry && block != blocks_.end()
            ? reinterpret_cast<uint64_t>(block->second.fn) : fn;
        if (edge->target.exchange(target, std::memory_order_release) != target && target && mt_active())
            mt_edges_published_.fetch_add(1, std::memory_order_relaxed);
    }
}
void FrostJIT::retire_mt_source(uint64_t pc) {
    auto refs = mt_source_refs_.find(pc);
    if (refs == mt_source_refs_.end()) return;
    for (MtEdge* edge : refs->second) {
        edge->live = false;
        edge->target.store(0, std::memory_order_release);
    }
    mt_source_refs_.erase(refs);
}
bool FrostJIT::patch_chain(size_t chain_patch_off, const uint8_t* target_fn) {
    if (!code_buf_) return false;
    // MT publication updates only the data cell, never live instructions.
    if (mt_active()) {
        auto slot = mt_edge_slots_.find(chain_patch_off);
        if (slot == mt_edge_slots_.end() || !slot->second->live) return false;
        slot->second->target.store(reinterpret_cast<uint64_t>(target_fn), std::memory_order_release);
        mt_edges_published_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
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
    refresh_mt_edges(target_pc); // includes tracked ST region exits
    if (mt_active()) return;
    // Legacy instruction-patching path below is single-thread only.
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
    // No entry means no block's chain slot targets target_pc: every slot is
    // registered at translate time and add_back_ref never drops a live one.
    // The old fallback scanned ALL blocks here whenever the entry was
    // missing, which is the normal case for a fresh call or return target,
    // so translation cost grew with the square of the block count (ART
    // spent ~96% of its translate time in that scan).
}
void FrostJIT::add_back_ref(uint64_t target_pc, uint64_t src_pc) {
    // Memory cap. Patched slots never need their entry again, so rebuild
    // from the slots that are still unpatched instead of clearing.
    constexpr size_t kBackRefCap = 1000000;
    if (back_refs_.size() >= kBackRefCap) {
        back_refs_.clear();
        for (const auto& kv : blocks_) {
            const BlockEntry& e = kv.second;
            if (!e.chained && e.chain_target_pc != 0)
                back_refs_[e.chain_target_pc].push_back(kv.first);
            if (!e.taken_chained && e.has_taken_chain_slot &&
                e.taken_chain_target_pc != 0 &&
                e.taken_chain_target_pc != e.chain_target_pc)
                back_refs_[e.taken_chain_target_pc].push_back(kv.first);
        }
    }
    back_refs_[target_pc].push_back(src_pc);
}
void FrostJIT::remember_code_pages(uint64_t pc, const BlockEntry& entry) {
    // Called under the compiler lock before publishing any public entry.
    auto remember = [&](uint64_t start, int count) {
        const uint64_t bytes = static_cast<uint64_t>(std::max(count, 1)) * 4;
        uint64_t last;
        if (__builtin_add_overflow(start, bytes - 1, &last)) last = UINT64_MAX;
        last &= ~Memory::PAGE_MASK;
        for (uint64_t page = start & ~Memory::PAGE_MASK;; page += Memory::PAGE_SIZE) {
            code_pages_.insert(page);
            if (page == last) break;
        }
    };
    remember(pc, entry.instr_count);
    for (const auto& range : entry.inlined_ranges_) remember(range.first, range.second);
}

void FrostJIT::publish_block(uint64_t pc, const BlockEntry& entry) {
    remember_code_pages(pc, entry);
    blocks_[pc] = entry;
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
    std::unique_lock<std::shared_mutex> g(blocks_mutex_);
    // mmap/munmap/mprotect and ART GC change data mappings much more often
    // than code mappings. A range with no published code cannot have victims.
    // lower_bound also handles enormous ranges without walking every page.
    if (!dbg().no_code_page_filter) {
        const auto page = code_pages_.lower_bound(lo);
        if (page == code_pages_.end() || *page >= hi) return;
    }
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
    invalidate_tier2_profiles(lo, hi);
    retire_blocks_locked(victims);
}

void FrostJIT::retire_blocks_locked(const std::vector<uint64_t>& victims) {
    // Caller holds the compiler lock and has selected exact victims.
    if (victims.empty()) return;
    const uint8_t unpatched = chain_skip_enabled() ? 0x90 : 0xC3;
    invalidate_dispatch_cache();
    auto unpatch_slot = [&](size_t off) {
        if (mt_active()) {
            auto slot = mt_edge_slots_.find(off);
            if (slot != mt_edge_slots_.end()) slot->second->target.store(0, std::memory_order_release);
            return;
        }
        if (off + 5 > CODE_BUF_SIZE) return;
        if (code_buf_[off] != 0xE9) return;  // not patched — leave alone
        // Chain-skip falls through all five bytes. Leaving the old rel32
        // behind would execute displacement bytes as instructions.
        if (chain_skip_enabled()) std::memset(code_buf_ + off + 1, 0x90, 4);
        code_buf_[off] = unpatched;
    };
    if (!mt_active()) make_writable();
    for (uint64_t vpc : victims) {
        auto victim_entry = blocks_.find(vpc);
        if (victim_entry != blocks_.end() && victim_entry->second.tier2_monitor)
            victim_entry->second.tier2_monitor->enabled.store(0, std::memory_order_release);
        for (auto& trial : tier2_trials_)
            if (!trial->monitor->enabled.load(std::memory_order_acquire))
                revoke_tier2_samples_locked(*trial);
        retire_mt_source(vpc);
        auto refs = mt_edge_refs_.find(vpc);
        if (refs != mt_edge_refs_.end())
            for (MtEdge* edge : refs->second) edge->target.store(0, std::memory_order_release);
        if (mt_active() && victim_entry != blocks_.end())
            tier2_counter_disable(victim_entry->second.tier2_counter_off, victim_entry->second.tier2_counter_len);
        // 2. Unpatch predecessors chained to this victim.
        auto bref = back_refs_.find(vpc);
        if (bref != back_refs_.end()) {
            for (uint64_t src_pc : bref->second) {
                auto sit = blocks_.find(src_pc);
                if (sit == blocks_.end()) continue;
                BlockEntry& published = sit->second;
                BlockEntry& se = published.is_region && published.tier2_monitor &&
                    published.tier2_monitor->trial && published.tier2_monitor->trial->decision.load() == 0
                    ? published.tier2_monitor->trial->baseline : published;
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
    if (!mt_active()) make_executable();
}
void FrostJIT::invalidate_all() {
    const uint8_t unpatched = chain_skip_enabled() ? 0x90 : 0xC3;
    std::unique_lock<std::shared_mutex> g(blocks_mutex_);
    invalidate_dispatch_cache();
    code_pages_.clear();
    clear_tier2_profiles();
    tier2_retrains_.clear(); // a full code epoch can reuse addresses for different loops
    for (auto& monitor : tier2_region_monitors_)
        monitor->enabled.store(0, std::memory_order_release);
    for (auto& edge : mt_edges_) {
        edge->live = false;
        edge->target.store(0, std::memory_order_release);
    }
    mt_source_refs_.clear();
    for (auto& trial : tier2_trials_) revoke_tier2_samples_locked(*trial);
    if (blocks_.empty()) return;
    if (mt_active()) {
        for (auto& counter : mt_hot_counters_) counter->enabled.store(0, std::memory_order_release);
        blocks_.clear(); back_refs_.clear();
        tls_last_block_ = LastBlockCache{};
        for (auto& set : tls_inline_cache_) set = InlineCacheSet{};
        return;
    }
    make_writable();
    for (auto& kv : blocks_) {
        BlockEntry& e = kv.second;
        if (e.chained && e.chain_patch_off + 5 <= CODE_BUF_SIZE
            && code_buf_[e.chain_patch_off] == 0xE9) {
            if (chain_skip_enabled()) std::memset(code_buf_ + e.chain_patch_off + 1, 0x90, 4);
            code_buf_[e.chain_patch_off] = unpatched;
            e.chained = false;
        }
        if (e.taken_chained && e.has_taken_chain_slot
            && e.taken_chain_patch_off + 5 <= CODE_BUF_SIZE
            && code_buf_[e.taken_chain_patch_off] == 0xE9) {
            if (chain_skip_enabled()) std::memset(code_buf_ + e.taken_chain_patch_off + 1, 0x90, 4);
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
    // No second vCPU exists yet. Retire legacy ST instruction patches once,
    // including frames retained by a guest call that created the thread.
    std::unique_lock<std::shared_mutex> lock(blocks_mutex_);
    make_writable();
    clear_tier2_profiles();
    for (auto& trial : tier2_trials_) revoke_tier2_samples_locked(*trial);
    for (auto& monitor : tier2_region_monitors_)
        if (monitor->trial && monitor->trial->decision.load(std::memory_order_acquire) == 0)
            monitor->enabled.store(0, std::memory_order_release);
    for (auto& edge : mt_edges_) {
        edge->live = false;
        edge->target.store(0, std::memory_order_release);
    }
    mt_source_refs_.clear();
    const uint8_t unpatched = chain_skip_enabled() ? 0x90 : 0xC3;
    for (auto& kv : blocks_) {
        auto& entry = kv.second;
        for (size_t off : {entry.chain_patch_off, entry.taken_chain_patch_off})
            if (off && off + 5 <= CODE_BUF_SIZE && code_buf_[off] == 0xE9) {
                if (chain_skip_enabled()) std::memset(code_buf_ + off + 1, 0x90, 4);
                code_buf_[off] = unpatched;
            }
        tier2_counter_disable(entry.tier2_counter_off, entry.tier2_counter_len, true);
    }
    make_executable();
    // MT publication appends code while other host threads execute from
    // this buffer, and they invalidate it. A W^X toggle (per-thread JIT
    // mode keeps W^X on) flips the WHOLE buffer to RW under them: SIGSEGV
    // ACCERR on their next fetch. MT code stays RWX, like the shared JIT.
    if (wex_enabled_) {
        if (mprotect(code_buf_, code_buf_limit_, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
            wex_enabled_ = false;
        } else {
            jit_disabled_.store(true, std::memory_order_relaxed);
            fprintf(stderr, "[%s] frostJIT: cannot make code buffer RWX for threads: %s\n",
                    CODENAME, strerror(errno));
        }
    }
    invalidate_dispatch_cache();
    blocks_.clear(); back_refs_.clear();
    code_pages_.clear();
}
} // namespace arm64emu
