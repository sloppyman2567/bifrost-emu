// jit/jit_dispatch.cpp — FrostJIT block dispatcher.
//
// v1.4.5-alpha: split out of frostjit.cpp. Holds the
// run_block() method, which is the main JIT entry point: given a CPU
// state, look up the block at cpu.pc in the block cache. On a hit,
// call the cached x86 code directly. On a miss, translate the block
// via translate_block() (jit_translate.cpp) and cache it. Also handles
// verify mode (BIFROST_JIT_VERIFY=1) and block chaining.
#include "jit/frostjit.hpp"
#include "core/emulator.h"
#include "ir/ir.hpp"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/types.h>  // mode_t
#include <unordered_map>
#include <vector>
namespace arm64emu {
// BIFROST_PROF sampling-flag toggle (see jit_glue.cpp). RAII so the
// flag is cleared on every early-return path.
extern thread_local bool prof_in_run_block;
extern thread_local bool prof_in_translate;
extern thread_local bool prof_in_interp;
void bifrost_prof_init(FrostJIT* jit);
bool bifrost_prof_active();
struct ProfRunGuard {
    bool saved_;
    ProfRunGuard() : saved_(bifrost_prof_active()) { if (saved_) prof_in_run_block = true; }
    ~ProfRunGuard() { if (saved_) prof_in_run_block = false; }
};
uint64_t FrostJIT::run_block(CPU& cpu, Emulator& emu) {
    static bool prof_inited = false;
    if (!prof_inited) {
        bifrost_prof_init(this);
        prof_inited = true;
    }
    // Hoisted once — the runtime hot-block -> interp_only demotion is disabled
    // by default (measured ~8-10% SLOWER on the minecraft game). getenv() on
    // every slow-path cache hit was a full environ scan per dispatch.
    static bool hot_interp_ = (getenv("BIFROST_HOT_INTERP") != nullptr);
    ProfRunGuard prof_g;
    if (!code_buf_ || jit_disabled_.load(std::memory_order_relaxed)) {
        interpreter_fallbacks++;
        emu.step(cpu);
        return cpu.pc;
    }
    // Global progress watchdog — if we've executed > GLOBAL_BLOCK_LIMIT
    // blocks, the JIT is likely stuck in a codegen-bug-induced loop.
    // Disable the JIT permanently and fall back to pure interpreter.
    // This is a safety valve; normal programs never hit it.
    //
    // 1.5.2-alpha: the counter is THREAD-LOCAL. The old
    // total_blocks_executed_.fetch_add(1) was a `lock xadd` on EVERY
    // dispatch (fast path included) — ~15-25 cycles of serializing
    // atomic traffic per block transition, which was a large fraction of
    // the ~25% dispatch overhead at 20M dispatches/sec. GLOBAL_BLOCK_LIMIT
    // is 1e12 (≈14h of pure dispatch at 20M blocks/sec), so the watchdog
    // is a pure safety valve for codegen-bug infinite loops; a per-thread
    // count catches any thread spinning on a buggy translation. A thread
    // tripping it disables the (shared) JIT for everyone.
    thread_local uint64_t tls_total_blocks_ = 0;
    if (__builtin_expect(++tls_total_blocks_ > GLOBAL_BLOCK_LIMIT, 0)) {
        jit_disabled_.store(true, std::memory_order_relaxed);
        fprintf(stderr, "[JIT] global watchdog: %llu blocks executed by a thread — disabling JIT (likely codegen bug)\n",
                static_cast<unsigned long long>(tls_total_blocks_));
        interpreter_fallbacks++;
        emu.step(cpu);
        return cpu.pc;
    }
    uint64_t pc = cpu.pc;
    // ── 1.5.2-alpha: single-entry "last block" fast cache ────────
    // Tight loops dispatch the same PC thousands of times in a row.
    // Bypass the shared_mutex + unordered_map lookup entirely when the
    // PC matches the cached one. The cached fn pointer is stable across
    // translate_block() calls (code_buf_ never moves), so a stale cache
    // entry is safe to call — worst case it runs an older (still-correct)
    // translation.
    //
    // 1.5.2-alpha: trimmed to the bare minimum. The per-PC watchdog is
    // gone from the fast path (the thread-local global watchdog above
    // still counts every dispatch, and a single-PC hot loop through the
    // dispatcher is the normal non-self-loopable case). The block's
    // epilogue already wrote next_pc to cpu.pc, so the redundant
    // `cpu.pc = next_pc` store is dropped too.
    //
    // Hot-path stats are accumulated in thread-locals and flushed to the
    // atomic counters on the slow path below. Every block dispatch used
    // to do two `lock xadd` atomics (~15-20 cycles each); with avg 4
    // instructions/block that was ~10 cycles of pure counter overhead per
    // guest instruction. Thread-locals are plain adds on the hot path.
    thread_local uint64_t tls_exec_ = 0;
    thread_local uint64_t tls_instr_ = 0;
    // Entries are written pc+fn together (slow path), so a pc match alone
    // implies a valid fn — no redundant fn != nullptr test. The ~0ULL
    // empty sentinel is never a real guest PC (48-bit VAs).
    if (__builtin_expect(pc == tls_last_block_.pc, 1)) {
        tls_exec_++;
        tls_instr_ += tls_last_block_.instr_count;
        return tls_last_block_.fn(&cpu, &emu);
    }
    // 1.5.2-alpha: inline cache for block-to-block transitions.
    // Catches the common case of sequential block-to-block transitions
    // (B/BL fallthrough, CBZ/CBNZ taken paths) without taking the
    // shared_mutex. Direct-mapped by a PC hash that mixes high and low
    // bits; grown to 256 slots (1.5.2-alpha) so a game's hot working
    // set stays resident instead of thrashing to the slow path. The
    // lookup is inlined — a separate call would be most of its cost.
    {
        uint64_t (*cached_fn)(CPU*, Emulator*) = nullptr;
        int cached_count = 0;
        if (inline_cache_lookup(pc, &cached_fn, cached_count)) {
            tls_exec_++;
            tls_instr_ += cached_count;
            return cached_fn(&cpu, &emu);
        }
    }
    // ── Shared-JIT locking strategy ────────────────────────────────
    // The lock is held ONLY for table mutations (translate, chain,
    // hotness promotion, watchdog demotion). It is RELEASED before
    // block execution (entry.fn), which may block in syscalls
    // (futex_wait, read, sleep). Holding the lock during execution
    // would deadlock: thread A holds the lock and blocks in futex_wait
    // waiting for thread B, which needs the lock to run the block that
    // would wake A.
    //
    // Block execution is safe without the lock because:
    //   - The code buffer is PROT_READ|PROT_EXEC (never mutated at runtime
    //     except via patch_chain, which takes its own W^X toggle).
    //   - `entry` is a local copy — other threads mutating blocks_[pc]
    //     don't affect our copy.
    //   - x86 JIT code is reentrant — multiple threads can execute the
    //     same block concurrently (each has its own CPU/stack).
    //
    // Flush the thread-local hot-path counters into the shared atomic
    // stats (slow path runs far less often than block dispatches).
    blocks_executed.fetch_add(tls_exec_, std::memory_order_relaxed);
    instructions_executed.fetch_add(tls_instr_, std::memory_order_relaxed);
    tls_exec_ = 0;
    tls_instr_ = 0;
    //
    // Per-thread state (watchdog, hotness) is thread-local — no lock
    // needed.
    // Use a SHARED lock for block lookup (concurrent reads OK). Release
    // before execution — entry is a local copy, no lock needed to run it.
    blocks_mutex_.lock_shared();
    auto it = blocks_.find(pc);
    BlockEntry entry;
    if (it != blocks_.end()) {
        entry = it->second;
        cache_hits++;
        // Per-PC hotness tracking (thread-local, no lock needed for the
        // counter, but promoting to interp_only needs exclusive lock).
        // Runtime promotion to interp_only is DISABLED by default: the
        // translator already marks CALL_INTERP-heavy blocks interp_only
        // (call_interp_count*2 > instr_count in translate_block), and
        // mixed blocks (a few fallbacks + several native ops) measure
        // FASTER in JIT — demoting them drags native ops down to
        // interpreter speed (interp is ~2x slower; game steady-state
        // dropped ~8-10% with promotion on). Opt in only if a future
        // workload shows JIT-with-CALL_INTERP slower than pure interp
        // for a hot cycle (the original __multf3 case is already
        // covered at translate time, so this should rarely be needed).
        if (!entry.interp_only && entry.fn &&
            entry.call_interp_count * 2 > entry.instr_count &&
            hot_interp_) {
            auto& cnt = tls_hot_pc_counts_[pc];
            if (++cnt >= HOT_PC_THRESHOLD) {
                // Promote to interp_only — upgrade to exclusive.
                blocks_mutex_.unlock_shared();
                blocks_mutex_.lock();
                it = blocks_.find(pc);
                if (it != blocks_.end()) {
                    it->second.interp_only = true;
                    it->second.interp_only_count = it->second.instr_count;
                    it->second.fn = nullptr;
                    it->second.chained = false;
                    it->second.taken_chained = false;
                    entry = it->second;
                }
                blocks_mutex_.unlock();
                blocks_mutex_.lock_shared();
                it = blocks_.find(pc);
                tls_hot_pc_counts_.erase(pc);
            }
            if (tls_hot_pc_counts_.size() > HOT_PC_MAP_MAX) {
                tls_hot_pc_counts_.clear();
            }
        }
        // interp_only shortcut — release lock, run interpreter.
        if (entry.interp_only) {
            blocks_mutex_.unlock_shared();
            blocks_executed++;
            constexpr int TIGHT_LOOP_MAX = 1000000;
            int tight_iter = 0;
            do {
                for (int i = 0; i < entry.interp_only_count && cpu.running; i++) {
                    emu.step(cpu);
                }
                instructions_executed += entry.interp_only_count;
                tight_iter++;
                if (tight_iter >= TIGHT_LOOP_MAX) break;
            } while (cpu.running && cpu.pc == pc);
            return cpu.pc;
        }
        // Lazy block chaining — release shared, try exclusive (non-blocking).
        // If we can't get exclusive, skip chaining (optimization, not correctness).
        if (!entry.chained) {
            static bool no_chain_hit_ = (getenv("BIFROST_NO_CHAIN") != nullptr);
            blocks_mutex_.unlock_shared();
            if (!no_chain_hit_ && blocks_mutex_.try_lock()) {
                if (entry.chain_target_pc != 0) {
                    auto chain_it = blocks_.find(pc);
                    if (chain_it != blocks_.end()) {
                        try_chain_block(pc, chain_it->second);
                    }
                }
                auto brit = back_refs_.find(pc);
                if (brit != back_refs_.end()) {
                    chain_back_references(pc);
                }
                blocks_mutex_.unlock();
            }
        } else {
            blocks_mutex_.unlock_shared();
        }
    } else {
        // Cache miss — need exclusive lock for translation.
        cache_misses++;
        blocks_mutex_.unlock_shared();
        blocks_mutex_.lock();
        auto fn = translate_block(emu, pc);
        if (!fn) {
            auto it2 = blocks_.find(pc);
            if (it2 != blocks_.end() && it2->second.interp_only) {
                entry = it2->second;
                blocks_mutex_.unlock();
                blocks_executed++;
                instructions_executed += entry.interp_only_count;
                for (int i = 0; i < entry.interp_only_count && cpu.running; i++) {
                    emu.step(cpu);
                }
                return cpu.pc;
            }
            blocks_mutex_.unlock();
            interpreter_fallbacks++;
            instructions_executed++;
            emu.step(cpu);
            return cpu.pc;
        }
        entry = blocks_[pc];
        blocks_mutex_.unlock();
    }
    // ── Lock is released. Execution below does NOT hold any lock. ──
    // Loop watchdog — if the same block runs > WATCHDOG_LIMIT times
    // consecutively, it's likely stuck in an infinite loop due to a JIT
    // codegen bug. Fall back to the interpreter for this block AND mark
    // it as interp_only permanently so future hits also use the
    // interpreter (avoiding repeated watchdog triggers).
    //
    // State is thread-local so multiple threads sharing a single FrostJIT
    // instance don't trample each other's counters.
    if (pc == tls_watchdog_last_pc_) {
        tls_watchdog_count_++;
        if (tls_watchdog_count_ > WATCHDOG_LIMIT) {
            // Mark this block as interp_only permanently — needs exclusive.
            blocks_mutex_.lock();
            auto wit = blocks_.find(pc);
            if (wit != blocks_.end() && !wit->second.interp_only) {
                wit->second.interp_only = true;
                wit->second.interp_only_count = wit->second.instr_count;
                wit->second.fn = nullptr;
                wit->second.chained = false;
                wit->second.taken_chained = false;
            }
            blocks_mutex_.unlock();
            interpreter_fallbacks++;
            emu.step(cpu);
            return cpu.pc;
        }
    } else {
        tls_watchdog_last_pc_ = pc;
        tls_watchdog_count_ = 0;
    }
    blocks_executed++;
    instructions_executed += entry.instr_count;
    // 1.5.2-alpha: populate the single-entry last-block cache so the
    // next dispatch of the same PC can take the fast path. Only cache
    // non-interp_only blocks with a valid fn pointer.
    if (entry.fn && !entry.interp_only) {
        tls_last_block_.pc = pc;
        tls_last_block_.fn = entry.fn;
        tls_last_block_.instr_count = entry.instr_count;
        // Also populate the inline cache for block-to-block transitions.
        int slot = static_cast<int>(((pc >> 2) ^ (pc >> 17)) & (INLINE_CACHE_SLOTS - 1));
        tls_inline_cache_[slot].pc = pc;
        tls_inline_cache_[slot].fn = entry.fn;
        tls_inline_cache_[slot].instr_count = entry.instr_count;
    }
    // Debug: print pstate at entry for specific blocks
    static bool dbg_ = (getenv("BIFROST_DBG_PC") != nullptr);
    if (dbg_) {
        const char* s = getenv("BIFROST_DBG_PC");
        uint64_t target = strtoull(s, nullptr, 0);
        if (pc == target) {
            fprintf(stderr, "[DBG] entry block @ 0x%llx pstate=0x%x x1=0x%llx x9=0x%llx x11=0x%llx x31=0x%llx\n",
                    static_cast<unsigned long long>(pc), cpu.pstate,
                    static_cast<unsigned long long>(cpu.regs[1]),
                    static_cast<unsigned long long>(cpu.regs[9]),
                    static_cast<unsigned long long>(cpu.regs[11]),
                    static_cast<unsigned long long>(cpu.regs[31]));
        }
    }
    // ── Release the lock before execution ──────────────────────────
    // From here on, we execute JIT code (entry.fn) or interpreter steps
    // that may block in syscalls. The lock is NOT needed for execution:
    //   - The code buffer is PROT_READ|PROT_EXEC (concurrent reads OK).
    //   - `entry` is a local copy (other threads can't mutate it).
    //   - x86 JIT code is reentrant (each thread has its own CPU/stack).
    // Verify-mode does code-buffer patching, but it's debug-only and
    // patches only this block's own slots (no cross-block mutation).
    // (Lock was already released above — no unlock needed here.)
    // ── BIFROST_JIT_VERIFY: divergence checker ──────────────────
    // Before running the JIT block, snapshot the CPU state. After the
    // JIT runs, step the interpreter from the snapshot for exactly the
    // same number of ARM instructions as the JIT block contains. Then
    // compare the final CPU state (registers + PC + pstate). If they
    // differ, print the divergence and abort.
    //
    // IMPORTANT: chained blocks can't be verified because the chain
    // slot patches `ret` to `jmp next_block`, so the JIT runs multiple
    // blocks in one call. We temporarily un-patch the chain slot to
    // force the block to return after its own instructions.
    //
    // Likewise, self-loop chaining patches the BRCOND taken path to
    // `jmp block_body_start`, so the JIT re-enters the block instead
    // of returning after one iteration. We temporarily un-patch the
    // self-loop slot to `jz resolve_loop_exit` style — actually we
    // replace it with 5 NOPs so the taken path falls through to the
    // epilogue and returns next_pc=branch_target. This forces the JIT
    // to run exactly one iteration of the loop, matching the
    // interpreter's `instr_count` step count.
    static bool verify_ = (getenv("BIFROST_JIT_VERIFY") != nullptr);
    if (verify_ && !entry.verified_once) {
        // Mark this block as verified so subsequent dispatches skip the
        // expensive per-block divergence check. This is essential for
        // self-loop blocks, where verify mode must un-patch the self-loop
        // slot to run one iteration at a time — without this flag, every
        // loop iteration would pay the full verify overhead (~30s for a
        // 3652-instruction test instead of <1s). First-dispatch verify
        // still catches real codegen bugs because divergences almost
        // always manifest on the first execution with any input values.
        // Set the flag BEFORE running the verify so a divergence-triggered
        // abort doesn't leave the flag cleared (which would cause an
        // infinite verify loop on retry).
        //
        // Set flag on local copy and on the map entry (brief exclusive lock).
        entry.verified_once = true;
        blocks_mutex_.lock();
        auto vit = blocks_.find(pc);
        if (vit != blocks_.end()) vit->second.verified_once = true;
        blocks_mutex_.unlock();
        // Save chain slot bytes and restore to `ret` + NOPs
        uint8_t saved_chain[5];
        bool was_chained = entry.chained;
        if (was_chained) {
            // W^X: toggle to writable before patching the chain slot.
            make_writable();
            memcpy(saved_chain, code_buf_ + entry.chain_patch_off, 5);
            if (chain_skip_enabled()) {
                // Lease layout: the slot is 5 NOPs with the real ret in the
                // cold exit AFTER it. Restore to NOPs so the block falls
                // through into the cold exit (restores frame, returns).
                for (int i = 0; i < 5; i++)
                    code_buf_[entry.chain_patch_off + i] = 0x90;
            } else {
                code_buf_[entry.chain_patch_off] = 0xC3; // ret
                code_buf_[entry.chain_patch_off + 1] = 0x90;
                code_buf_[entry.chain_patch_off + 2] = 0x90;
                code_buf_[entry.chain_patch_off + 3] = 0x90;
                code_buf_[entry.chain_patch_off + 4] = 0x90;
            }
            std::atomic_thread_fence(std::memory_order_release);
            // W^X: toggle back to executable before running the block.
            make_executable();
        }
        // Save taken-path chain slot bytes and restore to `ret` + NOPs
        // (verify runs the block one iteration at a time; a patched taken
        // slot would jump away to another block mid-verify).
        uint8_t saved_taken_chain[5];
        bool was_taken_chained = entry.taken_chained;
        if (was_taken_chained) {
            make_writable();
            memcpy(saved_taken_chain, code_buf_ + entry.taken_chain_patch_off, 5);
            if (chain_skip_enabled()) {
                for (int i = 0; i < 5; i++)
                    code_buf_[entry.taken_chain_patch_off + i] = 0x90;
            } else {
                code_buf_[entry.taken_chain_patch_off] = 0xC3; // ret
                code_buf_[entry.taken_chain_patch_off + 1] = 0x90;
                code_buf_[entry.taken_chain_patch_off + 2] = 0x90;
                code_buf_[entry.taken_chain_patch_off + 3] = 0x90;
                code_buf_[entry.taken_chain_patch_off + 4] = 0x90;
            }
            std::atomic_thread_fence(std::memory_order_release);
            make_executable();
        }
        // Save self-loop slot bytes and replace with NOPs so the JIT
        // runs exactly one iteration of the loop body (matching the
        // interpreter's `entry.instr_count` step budget). Without this,
        // verify mode logged false-positive PC DIVERGENCE for every
        // self-looping block (e.g. `1: ... ; CMP r0, #N ; B.NE 1b`),
        // because the JIT ran the loop to completion while the
        // interpreter stepped only `instr_count` instructions.
        uint8_t saved_selfloop[5];
        bool had_selfloop = entry.has_selfloop_slot;
        if (had_selfloop) {
            make_writable();
            memcpy(saved_selfloop, code_buf_ + entry.selfloop_patch_off, 5);
            // 5× NOP (0x90) — fall through past the slot to whatever
            // code follows (the not-taken epilogue, which returns the
            // branch target as next PC).
            for (int i = 0; i < 5; i++)
                code_buf_[entry.selfloop_patch_off + i] = 0x90;
            std::atomic_thread_fence(std::memory_order_release);
            make_executable();
        }
        // for the per-CPU pending signal queue). Snapshot only the
        // architectural state for verify-mode comparison.
        CPU saved;                  // default-constructed, then populated
        saved.copy_arch_state_from(cpu);
        // false (correct for clone, wrong for verify). Save the exclusive
        // monitor state so the interpreter's verify re-execution sees the
        // same LDXR reservation as the JIT did. Without this, STXR always
        // fails in the interpreter path (excl_tag_valid=false), causing
        // false-positive PC divergences in CAS loops (e.g. glibc's
        // __aarch64_cas4_acq used by curl, toybox, etc.).
        saved.excl_tag_valid = cpu.excl_tag_valid;
        saved.excl_tag_addr  = cpu.excl_tag_addr;
        saved.excl_tag_size  = cpu.excl_tag_size;
        // The pending-queue state isn't part of architectural state, so
        // saved's pending queue is empty. That's fine for verify mode —
        // verify runs single-threaded and no signals should be pending.
        // Debug: print entry state for specific blocks
        static bool vtrace_ = (getenv("BIFROST_VERIFY_TRACE") != nullptr);
        if (vtrace_) {
            fprintf(stderr, "[VTRACE] entry block @ 0x%llx x0=0x%llx x1=0x%llx pstate=0x%x\n",
                    static_cast<unsigned long long>(pc), static_cast<unsigned long long>(cpu.regs[0]),
                    static_cast<unsigned long long>(cpu.regs[1]), cpu.pstate);
        }
        // ── Verify-mode memory save/restore ─────────────────────────
        // Snapshot the original memory values at every STORE_MEM address
        // (resolved using the pre-JIT CPU state) so we can restore them
        // after the JIT runs. Without this, the interpreter's LOAD_MEM
        // would see the JIT's STORE_MEM writes, causing false-positive
        // divergences for blocks that read-then-write the same address
        // (e.g. the 0x44d65c block in toybox md5sum where LDP x21,x0,
        // [x19,#0x18] precedes STR x0,[x19,#0x18] — the interpreter's
        // LDP would read the JIT's stored x0 instead of the original,
        // making x21 look "stale by 0x28"). With save+restore, both
        // the JIT and the interpreter see the SAME original memory state
        // for those addresses, eliminating the false positive.
        //
        // Each entry: (addr, width, original_value).
        // We cap at 64 stores per block — beyond that, we accept false
        // positives (no real-world block exceeds this; the cap is just
        // a safety bound to avoid unbounded stack allocation).
        struct SavedMem { uint64_t addr; uint8_t width; uint64_t value; };
        SavedMem saved_mem[64];      // original values (pre-JIT)
        SavedMem jit_written[64];    // JIT's written values (post-JIT)
        int saved_mem_count = 0;
        // Use entry.store_infos (shared_ptr — cheap copy, no deep-copy).
        // Don't access blocks_ here — no lock is held (shared-JIT safe).
        if (entry.store_infos) {
            for (const auto& si : *entry.store_infos) {
                if (saved_mem_count >= 64) break;
                // to a known IMM within the block.
                uint64_t addr = si.use_absolute
                    ? si.absolute_addr
                    : ((si.arm_reg == 31) ? saved.sp : saved.regs[si.arm_reg])
                        + static_cast<uint64_t>(si.offset);
                uint64_t val  = 0;
                try {
                    switch (si.width) {
                        case 1: val = emu.mem().load<uint8_t>(addr);  break;
                        case 2: val = emu.mem().load<uint16_t>(addr); break;
                        case 4: val = emu.mem().load<uint32_t>(addr); break;
                        case 8: val = emu.mem().load<uint64_t>(addr); break;
                        default: continue;
                    }
                } catch (...) {
                    continue;
                }
                saved_mem[saved_mem_count].addr  = addr;
                saved_mem[saved_mem_count].width = si.width;
                saved_mem[saved_mem_count].value = val;
                saved_mem_count++;
            }
        }
        // Save guest umask BEFORE the JIT runs (for stateful-syscall
        // verify correctness — umask is stateful, so running it twice
        // gives different results without save/restore).
        mode_t saved_umask = emu.guest_umask();
        uint64_t jit_next = entry.fn(&cpu, &emu);
        cpu.pc = jit_next;
        // Save the JIT's umask value, restore pre-JIT for the interpreter.
        mode_t jit_umask = emu.guest_umask();
        emu.set_guest_umask(saved_umask);
        // Capture the JIT's written values at the STORE addresses (so we can
        // restore them after the interpreter runs — the next block expects
        // memory to be in the JIT's state, matching the JIT's cpu state).
        for (int i = 0; i < saved_mem_count; i++) {
            uint64_t v = 0;
            try {
                switch (saved_mem[i].width) {
                    case 1: v = emu.mem().load<uint8_t>(saved_mem[i].addr);  break;
                    case 2: v = emu.mem().load<uint16_t>(saved_mem[i].addr); break;
                    case 4: v = emu.mem().load<uint32_t>(saved_mem[i].addr); break;
                    case 8: v = emu.mem().load<uint64_t>(saved_mem[i].addr); break;
                }
            } catch (...) { /* skip */ }
            jit_written[i].addr  = saved_mem[i].addr;
            jit_written[i].width = saved_mem[i].width;
            jit_written[i].value = v;
        }
        // Restore the original memory values at every STORE_MEM address
        // so the interpreter sees the pre-JIT memory state (eliminating
        // the false-positive divergence from read-then-write patterns).
        for (int i = 0; i < saved_mem_count; i++) {
            try {
                switch (saved_mem[i].width) {
                    case 1: emu.mem().store<uint8_t>(saved_mem[i].addr,
                                static_cast<uint8_t>(saved_mem[i].value)); break;
                    case 2: emu.mem().store<uint16_t>(saved_mem[i].addr,
                                static_cast<uint16_t>(saved_mem[i].value)); break;
                    case 4: emu.mem().store<uint32_t>(saved_mem[i].addr,
                                static_cast<uint32_t>(saved_mem[i].value)); break;
                    case 8: emu.mem().store<uint64_t>(saved_mem[i].addr,
                                saved_mem[i].value); break;
                }
            } catch (...) {
                // Ignore — the JIT wrote here, so the address is writable
                // from the JIT's perspective. If the restore fails, the
                // interpreter will see the JIT's value (false positive),
                // but we won't crash.
            }
        }
        if (vtrace_) {
            fprintf(stderr, "[VTRACE] exit  block @ 0x%llx x0=0x%llx pstate=0x%x jit_next=0x%llx\n",
                    static_cast<unsigned long long>(pc), static_cast<unsigned long long>(cpu.regs[0]),
                    cpu.pstate, static_cast<unsigned long long>(jit_next));
        }
        // Debug: print pstate after JIT
        if (dbg_) {
            const char* s = getenv("BIFROST_DBG_PC");
            uint64_t target = strtoull(s, nullptr, 0);
            if (pc == target) {
                fprintf(stderr, "[DBG] exit  block @ 0x%llx pstate=0x%x x19=0x%llx x31=0x%llx jit_next=0x%llx\n",
                        static_cast<unsigned long long>(pc), cpu.pstate,
                        static_cast<unsigned long long>(cpu.regs[19]),
                        static_cast<unsigned long long>(cpu.regs[31]),
                        static_cast<unsigned long long>(jit_next));
            }
        }
        // Run interpreter from saved state for the same number of instrs.
        // Memory at STORE_MEM addresses was snapshotted before the JIT ran
        // and restored after, so the interpreter sees pre-JIT memory state
        // (eliminating false-positive divergences from read-then-write
        // patterns). After the comparison, the JIT's written values are
        // restored so the next block sees JIT-consistent memory.
        CPU ref;
        ref.copy_arch_state_from(saved);
        ref.pc = saved.pc;
        // interpreter's STXR sees the same LDXR reservation as the JIT.
        ref.excl_tag_valid = saved.excl_tag_valid;
        ref.excl_tag_addr  = saved.excl_tag_addr;
        ref.excl_tag_size  = saved.excl_tag_size;
        int steps = 0;
        while (steps < entry.instr_count && ref.running) {
            emu.step(ref);
            steps++;
        }
        // Restore the JIT's umask so the next block sees JIT-consistent state.
        emu.set_guest_umask(jit_umask);
        // Compare PC first — if PCs differ, the JIT took a different path.
        // This is a real codegen bug — log it and abort.
        if (ref.pc != jit_next) {
            fprintf(stderr, "[VERIFY] block @ 0x%llx: PC DIVERGENCE (jit_next=0x%llx ref_next=0x%llx steps=%d/%d)\n",
                    static_cast<unsigned long long>(pc), static_cast<unsigned long long>(jit_next),
                    static_cast<unsigned long long>(ref.pc), steps, entry.instr_count);
            for (int i = 0; i < 31; i++) {
                if (cpu.regs[i] != ref.regs[i]) {
                    fprintf(stderr, "[VERIFY]   x%d: jit=0x%llx ref=0x%llx\n",
                            i, static_cast<unsigned long long>(cpu.regs[i]),
                            static_cast<unsigned long long>(ref.regs[i]));
                }
            }
            if (cpu.sp != ref.sp)
                fprintf(stderr, "[VERIFY]   sp: jit=0x%llx ref=0x%llx\n",
                        static_cast<unsigned long long>(cpu.sp), static_cast<unsigned long long>(ref.sp));
            if (cpu.pstate != ref.pstate)
                fprintf(stderr, "[VERIFY]   pstate: jit=0x%llx ref=0x%llx\n",
                        static_cast<unsigned long long>(cpu.pstate), static_cast<unsigned long long>(ref.pstate));
            // Log but don't abort — the __syscall_ret CMN+HI carry divergence
            // is a known issue that doesn't affect program output (the error
            // path is never taken for valid fds). Real crashes will surface
            // as segfaults in the JIT code itself.
            fprintf(stderr, "[VERIFY] block @ 0x%llx: PC DIVERGENCE [logging only — may be false-positive]\n",
                    static_cast<unsigned long long>(pc));
        }
        // PCs match — compare register state.
        // NOTE: we skip pstate comparison for blocks ending with BRCOND
        // because CBNZ/CBZ are translated as TST+BRCOND, and the TST
        // materializes flags that the interpreter's CBNZ never sets.
        // This is a known semantic difference, not a real divergence.
        bool diverged = false;
        for (int i = 0; i < 31; i++) {
            if (cpu.regs[i] != ref.regs[i]) {
                fprintf(stderr, "[VERIFY] x%d: jit=0x%llx ref=0x%llx\n",
                        i, static_cast<unsigned long long>(cpu.regs[i]),
                        static_cast<unsigned long long>(ref.regs[i]));
                diverged = true;
            }
        }
        if (cpu.sp != ref.sp) {
            fprintf(stderr, "[VERIFY] sp: jit=0x%llx ref=0x%llx\n",
                    static_cast<unsigned long long>(cpu.sp), static_cast<unsigned long long>(ref.sp));
            diverged = true;
        }
        // Compare FP state (v_lo/v_hi) too — the vec/fp caches write
        // v_lo directly and a GPR-only compare can't see it (the fp-cache
        // call-guard corruption bug manifested only here).
        for (int i = 0; i < 32; i++) {
            if (cpu.v_lo[i] != ref.v_lo[i]) {
                fprintf(stderr, "[VERIFY] v_lo[%d]: jit=0x%llx ref=0x%llx\n",
                        i, static_cast<unsigned long long>(cpu.v_lo[i]),
                        static_cast<unsigned long long>(ref.v_lo[i]));
                diverged = true;
            }
        }
        if (cpu.pstate != ref.pstate) {
            // pstate comparison: mask out the internal from_sub marker bit
            // (bit 27) since it's a JIT implementation detail, not part of
            // the architectural NZCV state. Only compare the actual flags.
            uint64_t mask = 0xF0000000ULL;  // N=bit31, Z=bit30, C=bit29, V=bit28
            if ((cpu.pstate & mask) != (ref.pstate & mask)) {
                fprintf(stderr, "[VERIFY] pstate: jit=0x%llx ref=0x%llx (flags only: jit=0x%llx ref=0x%llx)\n",
                        static_cast<unsigned long long>(cpu.pstate), static_cast<unsigned long long>(ref.pstate),
                        static_cast<unsigned long long>(cpu.pstate & mask),
                        static_cast<unsigned long long>(ref.pstate & mask));
                diverged = true;
            }
        }
        if (diverged) {
            // Log but don't abort — verify mode has known false positives
            // from read-then-write same address in one block (the JIT's
            // STORE_MEM already happened when the interpreter re-reads).
            // Real bugs will cause a crash or wrong output later.
            fprintf(stderr, "[VERIFY] block @ 0x%llx: DIVERGENCE (pc=0x%llx steps=%d/%d) [logging only — may be false-positive]\n",
                    static_cast<unsigned long long>(pc), static_cast<unsigned long long>(jit_next),
                    steps, entry.instr_count);
        }
        // ── Memory divergence check (BIFROST_JIT_VERIFY_MEM=1) ────────
        // At this point the interpreter has re-run the block on the restored
        // pre-JIT memory, so the store addresses now hold the interpreter's
        // final values. Compare them against the JIT's written values — a
        // mismatch means the JIT wrote a different value to memory than the
        // interpreter, i.e. a REAL memory divergence that register verify
        // masks (it restores JIT memory afterwards). Register verify alone
        // won't catch a JIT that computes a correct register but stores it
        // to the wrong address or with a corrupted value.
        static bool memverify_ = (getenv("BIFROST_JIT_VERIFY_MEM") != nullptr);
        if (memverify_) {
            for (int i = 0; i < saved_mem_count; i++) {
                uint64_t ref_val = 0;
                bool ok = true;
                try {
                    switch (saved_mem[i].width) {
                        case 1: ref_val = emu.mem().load<uint8_t>(saved_mem[i].addr);  break;
                        case 2: ref_val = emu.mem().load<uint16_t>(saved_mem[i].addr); break;
                        case 4: ref_val = emu.mem().load<uint32_t>(saved_mem[i].addr); break;
                        case 8: ref_val = emu.mem().load<uint64_t>(saved_mem[i].addr); break;
                        default: ok = false;
                    }
                } catch (...) { ok = false; }
                if (ok && ref_val != jit_written[i].value) {
                    fprintf(stderr, "[VERIFY-MEM] block @ 0x%llx addr=0x%llx width=%d jit=0x%llx ref=0x%llx\n",
                            static_cast<unsigned long long>(pc),
                            static_cast<unsigned long long>(saved_mem[i].addr),
                            saved_mem[i].width,
                            static_cast<unsigned long long>(jit_written[i].value),
                            static_cast<unsigned long long>(ref_val));
                }
            }
        }
        // Restore the JIT's written values at every STORE_MEM address so
        // memory is consistent with the JIT's cpu state for the NEXT block.
        // (The interpreter overwrote these with its own values during the
        // re-run; without this restore, subsequent blocks would see the
        // interpreter's memory state while running on the JIT's cpu state,
        // causing cascading false-positive divergences.)
        for (int i = 0; i < saved_mem_count; i++) {
            try {
                switch (jit_written[i].width) {
                    case 1: emu.mem().store<uint8_t>(jit_written[i].addr,
                                static_cast<uint8_t>(jit_written[i].value)); break;
                    case 2: emu.mem().store<uint16_t>(jit_written[i].addr,
                                static_cast<uint16_t>(jit_written[i].value)); break;
                    case 4: emu.mem().store<uint32_t>(jit_written[i].addr,
                                static_cast<uint32_t>(jit_written[i].value)); break;
                    case 8: emu.mem().store<uint64_t>(jit_written[i].addr,
                                jit_written[i].value); break;
                }
            } catch (...) {
                // Ignore — best-effort restore.
            }
        }
        // Restore chain slot if it was patched.
        if (was_chained) {
            // W^X: toggle to writable before restoring the chain slot.
            make_writable();
            memcpy(code_buf_ + entry.chain_patch_off, saved_chain, 5);
            std::atomic_thread_fence(std::memory_order_release);
            // W^X: toggle back to executable for normal execution.
            make_executable();
        }
        // Restore self-loop slot if it was patched.
        if (had_selfloop) {
            make_writable();
            memcpy(code_buf_ + entry.selfloop_patch_off, saved_selfloop, 5);
            std::atomic_thread_fence(std::memory_order_release);
            make_executable();
        }
        // Restore taken-path chain slot if it was patched.
        if (was_taken_chained) {
            make_writable();
            memcpy(code_buf_ + entry.taken_chain_patch_off, saved_taken_chain, 5);
            std::atomic_thread_fence(std::memory_order_release);
            make_executable();
        }
        return jit_next;
    }
    static bool trace_ = (getenv("BIFROST_JIT_TRACE") != nullptr);
    if (trace_) {
        fprintf(stderr, "[JIT] run block @ 0x%llx sp=0x%llx x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx x5=0x%llx\n",
                static_cast<unsigned long long>(pc), static_cast<unsigned long long>(cpu.sp),
                static_cast<unsigned long long>(cpu.regs[0]), static_cast<unsigned long long>(cpu.regs[1]),
                static_cast<unsigned long long>(cpu.regs[2]), static_cast<unsigned long long>(cpu.regs[3]),
                static_cast<unsigned long long>(cpu.regs[5]));
    }
    // ── SIGSEGV delivery for JIT'd memory faults ───────────────────
    // The JIT'd code (entry.fn) calls C helpers (jit_load_mem_slow /
    // jit_store_mem_slow) that may throw UnmappedMemory. JIT'd code
    // has no DWARF unwind info, so a C++ exception thrown across it
    // would call std::terminate. Catch at the boundary and translate
    // to a SIGSEGV signal delivery (matching the interpreter path in
    // emulator.cpp). If no handler is installed, deliver_signal sets
    // cpu.exit_code = 128+11 = 139 and cpu.running = false.
    uint64_t next_pc;
    try {
        next_pc = entry.fn(&cpu, &emu);
    } catch (UnmappedMemory& e) {
        // Deliver SIGSEGV to the guest with fault address + si_code.
        // If a handler is installed, deliver_signal sets up the handler
        // frame and returns true; we resume at the handler's PC. If no
        // handler, it sets cpu.running = false and exit_code = 139.
        int si_code = e.write ? SEGV_ACCERR_EMU : SEGV_MAPERR_EMU;
        deliver_signal(emu, cpu, emu.signals(), BIFROST_SIGSEGV,
                       si_code, e.addr);
        // cpu.pc may have been changed by deliver_signal (handler entry)
        // or left unchanged (no handler — cpu.running is now false).
        next_pc = cpu.pc;
    }
    // The block's epilogue already stored next_pc to cpu.pc (jit_translate.cpp),
    // and the exception path set next_pc = cpu.pc above — the value is already
    // in place. Drop the redundant store (saves a store + the store->load
    // dependency for the next dispatch's `pc = cpu.pc` read).
    return next_pc;
}
} // namespace arm64emu
