// CodeCache.cpp — block cache + chain patching.
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
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
namespace arm64emu {
std::atomic<uint64_t> FrostJIT::next_cache_generation_{1};
thread_local const FrostJIT* FrostJIT::tls_cache_owner_ = nullptr;
thread_local uint64_t FrostJIT::tls_cache_generation_ = 0;
thread_local std::unique_ptr<FrostJIT::IblTable> FrostJIT::tls_ibl_;
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
    BlockEntry entry;
    return translate_entry(emu, pc, entry) ? entry.fn : nullptr;
}
static uint64_t elapsed_ns(std::chrono::steady_clock::time_point start) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count());
}
void FrostJIT::lock_table_shared() {
    if (blocks_mutex_.try_lock_shared()) return;
    const auto start = std::chrono::steady_clock::now();
    blocks_mutex_.lock_shared();
    table_lock_wait_ns.fetch_add(elapsed_ns(start), std::memory_order_relaxed);
}
void FrostJIT::lock_table() {
    if (blocks_mutex_.try_lock()) return;
    const auto start = std::chrono::steady_clock::now();
    blocks_mutex_.lock();
    table_lock_wait_ns.fetch_add(elapsed_ns(start), std::memory_order_relaxed);
}
// Caller holds blocks_mutex_ exclusively after a shared-lock compile.
bool FrostJIT::staged_entry_invalidated(uint64_t pc) {
    std::lock_guard<std::mutex> g(compile_invalidations_mu_);
    if (compile_invalidations_overflow_) return true;
    auto touched = [&](uint64_t b, uint64_t e) {
        for (const auto& [lo, hi] : compile_invalidations_)
            if (b < hi && e > lo) return true;
        return false;
    };
    if (touched(pc, pc + static_cast<uint64_t>(staged_entry_.instr_count) * 4)) return true;
    for (const auto& r : staged_entry_.inlined_ranges_)
        if (touched(r.first, r.first + static_cast<uint64_t>(r.second) * 4)) return true;
    return false;
}
bool FrostJIT::translate_entry(Emulator& emu, uint64_t pc, BlockEntry& out) {
    auto copy_locked = [&] {
        auto it = blocks_.find(pc);
        if (it == blocks_.end()) return false;
        out = it->second;
        return true;
    };
    if (!mt_active()) {
        // ST code patches published instructions and toggles W^X; keep the
        // whole compile under the exclusive lock.
        lock_table();
        if (!copy_locked()) translate_block(emu, pc);
        const bool found = copy_locked();
        blocks_mutex_.unlock();
        return found;
    }
    // MT: one compiler at a time, while other CPUs keep reading the table.
    // Codegen only reads blocks_; the edge/counter metadata it appends is
    // read by exclusive-lock holders, which cannot run until it finishes.
    std::unique_lock<std::mutex> compiling(compile_mutex_, std::defer_lock);
    if (!compiling.try_lock()) {
        const auto start = std::chrono::steady_clock::now();
        compiling.lock();
        table_lock_wait_ns.fetch_add(elapsed_ns(start), std::memory_order_relaxed);
    }
    lock_table_shared();
    if (copy_locked()) { blocks_mutex_.unlock_shared(); return true; }
    {
        std::lock_guard<std::mutex> g(compile_invalidations_mu_);
        compile_invalidations_.clear();
        compile_invalidations_overflow_ = false;
    }
    shared_compile_active_.store(true, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);  // pairs with log_invalidation_for_compiles
    const size_t edge_mark = mt_edges_.size();
    staging_ = true;
    staged_ = false;
    translate_block(emu, pc);
    staging_ = false;
    const size_t edge_end = mt_edges_.size();
    blocks_mutex_.unlock_shared();
    tier1_shared_compiles.fetch_add(1, std::memory_order_relaxed);
    lock_table();
    shared_compile_active_.store(false, std::memory_order_relaxed);
    const bool raced = blocks_.count(pc) != 0;
    const bool stale = staged_ && staged_entry_invalidated(pc);
    if (staged_ && !raced && !stale) {
        if (staged_linked_) publish_translation(pc, staged_entry_);
        else publish_block(pc, staged_entry_);
    } else if (staged_) {
        // Never published, so nothing can reach this code except its own
        // edges; revoke them. Another translation's edges stay intact.
        auto refs = mt_source_refs_.find(pc);
        for (size_t i = edge_mark; i < edge_end; ++i) {
            MtEdge* edge = mt_edges_[i].get();
            edge->live = false;
            edge->target.store(0, std::memory_order_release);
            if (refs != mt_source_refs_.end()) {
                auto& v = refs->second;
                v.erase(std::remove(v.begin(), v.end(), edge), v.end());
            }
        }
        // Guest code changed while compiling: translate the current bytes
        // with the table held, which no invalidation can interleave.
        if (stale && !raced) {
            tier1_compile_retries.fetch_add(1, std::memory_order_relaxed);
            translate_block(emu, pc);
        }
    }
    staged_entry_ = BlockEntry{};
    const bool found = copy_locked();
    blocks_mutex_.unlock();
    return found;
}
uint64_t (*FrostJIT::lookup_call_target_slow(Emulator& emu, uint64_t pc,
                                           int& instr_count))(CPU*, Emulator*) {
    note_table_lookup();
    // MT promotion is driven by atomic entry counters. Read-only table hits
    // need only a shared lock; a miss compiles through translate_entry.
    // The dispatch generation was synced before this lookup, so a later
    // invalidation still revokes the thread-local entries inserted below.
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
    BlockEntry entry;
    auto fn = translate_entry(emu, pc, entry) ? entry.fn : nullptr;
    int cnt = fn ? entry.instr_count : 1;
    // The generated entry counter owns hotness for eligible heads. Keep
    // publication in tier2_fire_region so all entries retain constituent
    // ranges, revoke dispatch caches and redirect incoming chains together.
    tls_last_block_ = LastBlockCache{pc, fn, cnt};
    inline_cache_insert(pc, fn, cnt);
    instr_count = cnt;
    return fn;
}
void FrostJIT::dump_compile_stats() {
    // One periodic reporter thread owns these interval baselines.
    static uint64_t last_blocks, last_ns, last_recompiles, last_shared, last_retries, last_wait;
    static uint64_t last_inv[static_cast<size_t>(CodeInvalidation::Count)][3];
    const uint64_t blocks = blocks_translated.load(std::memory_order_relaxed);
    const uint64_t ns = tier1_compile_ns.load(std::memory_order_relaxed);
    const uint64_t recompiles = tier1_recompiles.load(std::memory_order_relaxed);
    const uint64_t shared = tier1_shared_compiles.load(std::memory_order_relaxed);
    const uint64_t retries = tier1_compile_retries.load(std::memory_order_relaxed);
    const uint64_t wait = table_lock_wait_ns.load(std::memory_order_relaxed);
    const uint64_t max_ns = tier1_compile_max_ns.exchange(0, std::memory_order_relaxed);
    const uint64_t d_blocks = blocks - last_blocks, d_ns = ns - last_ns;
    fprintf(stderr, "[JIT] compile: blocks=%llu recompiles=%llu ms=%.2f avg_us=%.1f max_us=%.1f "
            "shared=%llu retries=%llu lock_wait_ms=%.2f\n",
            (unsigned long long)d_blocks, (unsigned long long)(recompiles - last_recompiles),
            d_ns / 1e6, d_blocks ? d_ns / 1e3 / d_blocks : 0.0, max_ns / 1e3,
            (unsigned long long)(shared - last_shared), (unsigned long long)(retries - last_retries),
            (wait - last_wait) / 1e6);
    last_blocks = blocks; last_ns = ns; last_recompiles = recompiles;
    last_shared = shared; last_retries = retries; last_wait = wait;
    std::string line;
    for (size_t r = 0; r < static_cast<size_t>(CodeInvalidation::Count); ++r) {
        const uint64_t now[3] = {invalidation_stats[r].calls.load(std::memory_order_relaxed),
                                 invalidation_stats[r].retired.load(std::memory_order_relaxed),
                                 invalidation_stats[r].kept.load(std::memory_order_relaxed)};
        char buf[96];
        snprintf(buf, sizeof(buf), " %s=%llu/%llu/%llu", invalidation_name(static_cast<CodeInvalidation>(r)),
                 (unsigned long long)(now[0] - last_inv[r][0]), (unsigned long long)(now[1] - last_inv[r][1]),
                 (unsigned long long)(now[2] - last_inv[r][2]));
        line += buf;
        for (int k = 0; k < 3; ++k) last_inv[r][k] = now[k];
    }
    fprintf(stderr, "[JIT] invalidate (calls/retired/kept):%s\n", line.c_str());
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
// Indirect-branch fast path for a chain-skip BR/BLR/RET exit. Entered with
// RAX = guest target (already stored to cpu.pc), RDI = cpu, RSI = emu and
// every vreg/flag committed; RCX, RDX and R11 are free. Falls through to the
// caller's cold exit (dispatcher return) on any miss. Immutable code that
// reads only data, so it is valid in ST and MT publication alike.
void FrostJIT::emit_indirect_branch_probe() {
    constexpr int32_t entries_off = offsetof(IblTable, entries);
    std::vector<size_t> cold;
    // RCX = this thread's table; none bound (trial, foreign entry) → cold.
    emit_load(RCX, RDI, offsetof(CPU, jit_ibl_table));
    emit_test_reg(RCX, RCX);
    cold.push_back(emit_jcc_rel32_placeholder(0x4));             // jz
    // Table generation must equal this JIT's live dispatch generation.
    emit_load(R11, RCX, offsetof(IblTable, generation));
    emit_mov_imm64(RDX, reinterpret_cast<uint64_t>(&cache_generation_));
    emit_byte(0x4C); emit_byte(0x3B); emit_byte(0x1A);          // cmp r11,[rdx]
    cold.push_back(emit_jcc_rel32_placeholder(0x5));             // jne
    // RCX = &entries[(pc >> 2) & (IBL_ENTRIES - 1)] - entries_off
    emit_mov_reg(RDX, RAX);
    emit_shift_imm8(RDX, 5, 2);                                  // shr rdx,2
    emit_alu_imm(RDX, 4, IBL_ENTRIES - 1);                       // and rdx,mask
    emit_shift_imm8(RDX, 4, 4);                                  // shl rdx,4
    emit_add_reg(RCX, RDX);
    emit_byte(0x48); emit_byte(0x3B); emit_byte(0x81);          // cmp rax,[rcx+pc]
    emit_u32(static_cast<uint32_t>(entries_off + offsetof(IblEntry, pc)));
    cold.push_back(emit_jcc_rel32_placeholder(0x5));             // jne
    // Host-managed return boundary (SDL thread sentinel) → dispatcher.
    emit_byte(0x48); emit_byte(0x3B); emit_byte(0x87);          // cmp rax,[rdi+stop]
    emit_u32(offsetof(CPU, jit_stop_pc));
    cold.push_back(emit_jcc_rel32_placeholder(0x4));             // je
    // Inside a nested jit_call_helper the return PC must reach that loop.
    emit_byte(0x48); emit_byte(0x83); emit_byte(0xBF);          // cmp qword [rdi+depth],0
    emit_u32(offsetof(CPU, jit_call_depth)); emit_byte(0);
    cold.push_back(emit_jcc_rel32_placeholder(0x5));             // jne
    // Shared 256-edge lease: return to dispatch (signals, exit) regularly.
    emit_byte(0x83); emit_byte(0xAF);                            // sub dword [rdi+budget],1
    emit_u32(offsetof(CPU, jit_chain_budget)); emit_byte(1);
    cold.push_back(emit_jcc_rel32_placeholder(0x6));             // jbe
    emit_byte(0xFF); emit_byte(0xA1);                            // jmp [rcx+entry]
    emit_u32(static_cast<uint32_t>(entries_off + offsetof(IblEntry, entry)));
    for (size_t p : cold)
        patch_jcc_rel32(p, static_cast<int32_t>(code_buf_used_ - (p + 6)));
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
        // A body entry carries this translation's pins and frame. It must
        // never jump into another translation, including a promoted region.
        if (!edge->live || edge->sampling_private || edge->internal_loop) continue;
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
// Single-thread only (MT never patches direct calls). The caller holds the
// compiler lock with the buffer writable. Every slot that may target this
// PC's translation returns to its slow path and waits for re-patching.
void FrostJIT::unpatch_direct_calls(uint64_t target_pc) {
    auto it = direct_call_sites_.find(target_pc);
    if (it == direct_call_sites_.end()) return;
    auto& pending = pending_call_sites_[target_pc];
    pending.clear();
    for (const auto& [slot, slow] : it->second) {
        if (slot + 5 > CODE_BUF_SIZE || code_buf_[slot] != 0xE8) continue;
        int32_t rel = static_cast<int32_t>(slow - (slot + 5));
        memcpy(code_buf_ + slot + 1, &rel, 4);
        pending.push_back(slot);
    }
    std::atomic_thread_fence(std::memory_order_release);
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
void FrostJIT::remember_code_granules(uint64_t pc, const BlockEntry& entry) {
    // Called under the compiler lock before publishing any public entry.
    auto remember = [&](uint64_t start, int count) {
        const uint64_t bytes = static_cast<uint64_t>(std::max(count, 1)) * 4;
        uint64_t last;
        if (__builtin_add_overflow(start, bytes - 1, &last)) last = UINT64_MAX;
        last &= ~(CODE_INDEX_GRANULE - 1);
        for (uint64_t granule = start & ~(CODE_INDEX_GRANULE - 1);; granule += CODE_INDEX_GRANULE) {
            auto& pcs = code_index_[granule];
            if (std::find(pcs.begin(), pcs.end(), pc) == pcs.end()) pcs.push_back(pc);
            if (granule == last) break;
        }
    };
    remember(pc, entry.instr_count);
    for (const auto& range : entry.inlined_ranges_) remember(range.first, range.second);
}

// perf's JIT symbol file (tools/perf/Documentation/jit-interface.txt):
// "START SIZE name" per line. The block occupies [fn, current emission end);
// later lines win when a flushed cache reuses an address. The name gives the
// guest PC and its host-window address, which /proc/<pid>/maps resolves to
// the guest library.
void FrostJIT::write_perf_map(uint64_t pc, const BlockEntry& entry) {
    static std::mutex mu;
    static FILE* file;
    const auto* start = reinterpret_cast<const uint8_t*>(entry.fn);
    const uint8_t* end = code_buf_ + code_buf_used_;
    if (!start || start < code_buf_ || start >= end) return;
    std::lock_guard<std::mutex> lock(mu);
    if (!file) {
        char path[64];
        snprintf(path, sizeof(path), "/tmp/perf-%d.map", static_cast<int>(getpid()));
        file = fopen(path, "a");
        if (!file) return;
    }
    fprintf(file, "%llx %llx aarch64:%llx @%llx\n",
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(start)),
            static_cast<unsigned long long>(end - start),
            static_cast<unsigned long long>(pc),
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(window_base_) + pc));
    fflush(file);
}

void FrostJIT::publish_block(uint64_t pc, const BlockEntry& entry) {
    remember_code_granules(pc, entry);
    if (dbg().jit_perf_map) write_perf_map(pc, entry);
    if (!entry.is_region && retired_pcs_.erase(pc))
        tier1_recompiles.fetch_add(1, std::memory_order_relaxed);
    blocks_[pc] = entry;
}

// Appends the words fetched by the current compile when they are exactly
// the block's contiguous instruction range.
void FrostJIT::attach_code_words(BlockEntry& entry, bool contiguous) {
    const size_t count = block_words_.size();
    if (!contiguous || count == 0 || count != static_cast<size_t>(entry.instr_count) ||
        code_words_.size() > UINT32_MAX - count) return;
    entry.code_words_off = static_cast<uint32_t>(code_words_.size());
    entry.code_words_count = static_cast<uint32_t>(count);
    code_words_.insert(code_words_.end(), block_words_.begin(), block_words_.end());
}

// A translation depends only on its fetched instruction words. It stays
// valid while every word matches and its pages remain mapped executable.
// Reads page flags and the window directly: check_access would clear an
// unregistered userfaultfd-missing flag, which invalidation must not do.
bool FrostJIT::block_code_unchanged(const BlockEntry& entry, uint64_t pc) const {
    const uint64_t bytes = static_cast<uint64_t>(entry.code_words_count) * 4;
    if (!bytes || !guest_memory_ || !window_base_ || entry.is_region ||
        !entry.inlined_ranges_.empty() ||
        entry.code_words_count != static_cast<uint32_t>(entry.instr_count) ||
        pc > guest_memory_->window_size() || bytes > guest_memory_->window_size() - pc)
        return false;
    const auto* flags = guest_memory_->direct_page_flags();
    constexpr uint8_t need = Memory::GUEST_PAGE_MAPPED | Memory::GUEST_PROT_EXEC;
    for (uint64_t p = pc / Memory::PAGE_SIZE; p <= (pc + bytes - 1) / Memory::PAGE_SIZE; ++p) {
        const uint8_t f = flags[p].load(std::memory_order_acquire);
        if ((f & need) != need || (f & Memory::GUEST_PAGE_UFFD_MISSING)) return false;
    }
    return std::memcmp(window_base_ + pc, code_words_.data() + entry.code_words_off, bytes) == 0;
}

// Compiles that run under the shared table lock cannot be seen by the
// early-out below. They log every range that changes meanwhile and check it
// before publication. The fences pair with translate_entry: either the
// compile observes the change, or the change is logged.
void FrostJIT::log_invalidation_for_compiles(uint64_t lo, uint64_t hi) {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (!shared_compile_active_.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> g(compile_invalidations_mu_);
    if (compile_invalidations_.size() >= 64) compile_invalidations_overflow_ = true;
    else compile_invalidations_.emplace_back(lo, hi);
}

void FrostJIT::invalidate_range(uint64_t addr, uint64_t size, CodeInvalidation reason) {
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
    // Cache maintenance names exact lines. Mapping and protection changes
    // apply to whole pages even when the guest length is not page-aligned.
    const bool by_line = reason == CodeInvalidation::CacheMaintenance;
    const uint64_t first = by_line ? addr : lo, last = by_line ? end : hi;
    log_invalidation_for_compiles(first, last);
    auto& stats = invalidation_stats[static_cast<size_t>(reason)];
    stats.calls.fetch_add(1, std::memory_order_relaxed);
    const uint64_t glo = first & ~(CODE_INDEX_GRANULE - 1);
    uint64_t ghi;
    if (__builtin_add_overflow(last, CODE_INDEX_GRANULE - 1, &ghi)) ghi = UINT64_MAX;
    ghi &= ~(CODE_INDEX_GRANULE - 1);
    // Data-only ranges (most mappings, data-cache maintenance) touch no
    // indexed granule; answer them without blocking table readers.
    if (!dbg().no_code_page_filter) {
        std::shared_lock<std::shared_mutex> g(blocks_mutex_);
        auto granule = code_index_.lower_bound(glo);
        if (granule == code_index_.end() || granule->first >= ghi) return;
    }
    std::unique_lock<std::shared_mutex> g(blocks_mutex_);
    // 1. Collect victims: blocks whose [pc, pc+instr_count*4) or an inlined
    // BL-leaf/Tier-2 dependency range overlaps [first, last). Granules only
    // locate candidates: a 64-byte cache-line publish must neither drop nor
    // scan unrelated translations elsewhere on its page.
    std::vector<uint64_t> victims;
    victims.reserve(4);
    auto ranges_overlap = [&](uint64_t b, uint64_t e) { return b < last && e > first; };
    auto overlaps = [&](uint64_t b, const BlockEntry& entry) {
        if (ranges_overlap(b, b + static_cast<uint64_t>(entry.instr_count) * 4)) return true;
        for (const auto& r : entry.inlined_ranges_)
            if (ranges_overlap(r.first, r.first + static_cast<uint64_t>(r.second) * 4)) return true;
        return false;
    };
    if (dbg().no_code_page_filter) {
        for (const auto& kv : blocks_)
            if (overlaps(kv.first, kv.second)) victims.push_back(kv.first);
    } else {
        // mmap/munmap/mprotect and ART GC change data mappings much more often
        // than code mappings, and guest JITs issue IC IVAU per cache line.
        // Visit only blocks indexed on these granules instead of the whole
        // table. The map walk handles enormous ranges without visiting every
        // granule.
        auto granule = code_index_.lower_bound(glo);
        if (granule == code_index_.end() || granule->first >= ghi) return;
        for (; granule != code_index_.end() && granule->first < ghi;) {
            auto& pcs = granule->second;
            size_t kept = 0;
            for (uint64_t pc : pcs) {
                auto it = blocks_.find(pc);
                if (it == blocks_.end()) continue;  // retired: prune
                pcs[kept++] = pc;
                if (overlaps(pc, it->second)) victims.push_back(pc);
            }
            pcs.resize(kept);
            granule = pcs.empty() ? code_index_.erase(granule) : std::next(granule);
        }
        // A block spanning several granules in the range is listed on each.
        std::sort(victims.begin(), victims.end());
        victims.erase(std::unique(victims.begin(), victims.end()), victims.end());
    }
    // Cache maintenance and mprotect leave the bytes in place: a block whose
    // words are unchanged and still executable is as valid as a retranslation.
    if (invalidation_keeps_unchanged(reason)) {
        size_t kept = 0;
        for (uint64_t pc : victims) {
            auto it = blocks_.find(pc);
            if (it != blocks_.end() && block_code_unchanged(it->second, pc)) continue;
            victims[kept++] = pc;
        }
        stats.kept.fetch_add(victims.size() - kept, std::memory_order_relaxed);
        victims.resize(kept);
    }
    stats.retired.fetch_add(victims.size(), std::memory_order_relaxed);
    for (auto it = tier2_block_limits_.begin(); it != tier2_block_limits_.end();) {
        if (it->first >= lo && it->first < hi) it = tier2_block_limits_.erase(it);
        else ++it;
    }
    for (uint64_t pc : victims) tier2_block_limits_.erase(pc);
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
        // Direct calls into it return to their slow path and are queued in
        // pending_call_sites_, so a retranslation re-patches them.
        if (!mt_active()) unpatch_direct_calls(vpc);
        if (victim_entry != blocks_.end() && !victim_entry->second.is_region) retired_pcs_.insert(vpc);
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
    log_invalidation_for_compiles(0, UINT64_MAX);
    std::unique_lock<std::shared_mutex> g(blocks_mutex_);
    invalidate_dispatch_cache();
    code_index_.clear();
    clear_tier2_profiles();
    tier2_retrains_.clear(); // a full code epoch can reuse addresses for different loops
    tier2_block_limits_.clear();
    for (auto& monitor : tier2_region_monitors_)
        monitor->enabled.store(0, std::memory_order_release);
    for (auto& edge : mt_edges_) {
        edge->live = false;
        edge->target.store(0, std::memory_order_release);
    }
    mt_source_refs_.clear();
    for (auto& trial : tier2_trials_) revoke_tier2_samples_locked(*trial);
    if (blocks_.empty()) return;
    for (const auto& kv : blocks_)
        if (!kv.second.is_region) retired_pcs_.insert(kv.first);
    if (mt_active()) {
        for (auto& counter : mt_hot_counters_) counter->enabled.store(0, std::memory_order_release);
        blocks_.clear(); back_refs_.clear();
        tls_last_block_ = LastBlockCache{};
        clear_inline_cache();
        return;
    }
    make_writable();
    for (const auto& kv : blocks_) unpatch_direct_calls(kv.first);
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
    clear_inline_cache();
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
    for (const auto& kv : blocks_)
        if (!kv.second.is_region) retired_pcs_.insert(kv.first);
    blocks_.clear(); back_refs_.clear();
    code_index_.clear();
}
} // namespace arm64emu
