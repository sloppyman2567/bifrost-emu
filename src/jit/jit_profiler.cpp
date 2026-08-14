// jit/jit_profiler.cpp — FrostJIT lifecycle + statistics + watchdog.
//
// Holds construction/destruction (mmap of the code buffer), cache flush,
// and the per-instance watchdog state (reset on construction; checked
// in run_block). Statistics counters (blocks_translated, blocks_executed,
// cache_hits, cache_misses, interpreter_fallbacks, block_chains_patched)
// are public fields on FrostJIT; this file owns their initialization.
//
// Watchdog:
//   - Per-instance loop watchdog: if the same PC runs > WATCHDOG_LIMIT
//     times consecutively, fall back to the interpreter for that block
//     (and mark it interp_only permanently).
//   - Global progress watchdog: if total block executions exceed
//     GLOBAL_BLOCK_LIMIT, the JIT switches to interpreter-only mode
//     permanently (safety valve for codegen-bug-induced infinite loops).
//
// Verify mode (BIFROST_JIT_VERIFY env var) is implemented inline in
// run_block (frostjit.cpp) because it needs tight coupling with the
// block dispatch path.
#include "jit/frostjit.hpp"
#include "bifrost/version.hpp"  // CODENAME
#include <sys/mman.h>
#include <cerrno>
#include <cstring>   // memcpy
#include <cstdlib>   // getenv
#include <algorithm> // sort
#include <cstdio>    // fprintf
#include <map>
namespace arm64emu {
// ── Construction ────────────────────────────────────────────────────────
FrostJIT::FrostJIT() {
    // W^X (Write XOR Execute) protection: allocate the code buffer as
    // PROT_READ|PROT_WRITE first (for codegen), then toggle to
    // PROT_READ|PROT_EXEC before execution. This prevents the buffer
    // from being simultaneously writable and executable, mitigating
    // code-injection attacks via JIT bugs.
    //
    // We check BIFROST_NO_WEX=1 to disable (for perf-sensitive builds).
    // If the initial mprotect to RX fails (some hardened kernels reject
    // PROT_EXEC on anonymous mappings), we fall back to RWX.
    bool disable_wex = (getenv("BIFROST_NO_WEX") != nullptr);
    int initial_prot = disable_wex
        ? (PROT_READ | PROT_WRITE | PROT_EXEC)
        : (PROT_READ | PROT_WRITE);
    void* p = mmap(nullptr, CODE_BUF_SIZE, initial_prot,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "[%s] frostJIT: mmap code buffer failed (%zu bytes): %s\n",
                CODENAME, CODE_BUF_SIZE, strerror(errno));
        code_buf_ = nullptr;
    } else {
        code_buf_ = static_cast<uint8_t*>(p);
    }
    // Initialize W^X state. If we started RW (no EXEC), enable W^X and
    // mark the buffer as currently writable (since we just allocated it
    // RW and will write to it during the first translate_block).
    if (code_buf_ && !disable_wex) {
        wex_enabled_ = true;
        wex_write_depth_ = 0;  // buffer is RX (not writable) by default
    }
    // Counters start at zero (declared in the public header).
    blocks_translated = 0;
    blocks_executed   = 0;
    cache_hits        = 0;
    cache_misses      = 0;
    interpreter_fallbacks = 0;
    block_chains_patched = 0;
    // Watchdog state (thread-local — reset for this thread).
    tls_watchdog_last_pc_ = UINT64_MAX;
    tls_watchdog_count_   = 0;
    total_blocks_executed_.store(0, std::memory_order_relaxed);
    jit_disabled_.store(false, std::memory_order_relaxed);
    // Initialize vreg arrays — prev_max_vreg_ must be large enough that
    // the first translate_block() clears all 4096 entries. Without this,
    // uninitialized vreg_home_[] garbage causes load_vreg_to_reg to think
    // vregs are cached in random host regs, producing mov-from-garbage.
    prev_max_vreg_ = 4095;
    for (int i = 0; i < 4096; i++) {
        vreg_home_[i] = -1;
        vreg_dirty_[i] = false;
        vreg_slot_[i] = 0;
        vreg_last_use_[i] = 0;  // reset LRU timestamps
    }
    for (int i = 0; i < NUM_HOST_REGS; i++) reg_vreg_[i] = -1;
    max_vreg_ = 0;
    // ── FMV: detect CPU features once at construction ──────────────
    // The result is cached for the JIT's lifetime — CPU features don't
    // change at runtime. Polled by compile_ir_inst() when emitting code
    // for hot operations with multiple x86 codegen variants (currently
    // only FMADD/FMSUB/FNMADD/FNMSUB → FMA3 vs. decomposed mul+add/sub).
    //
    // BIFROST_NO_FMA3=1 forces the decomposed path even on FMA3 CPUs.
    // This is a debugging aid: it lets us A/B-test the FMA3 codegen
    // against the decomposed codegen on the same machine, and it gives
    // users a workaround if FMA3 codegen has a bug we haven't found yet.
    cpu_features_ = detect_cpu_features();
    no_fma3_ = (getenv("BIFROST_NO_FMA3") != nullptr);
    no_avx2_ = (getenv("BIFROST_NO_AVX2") != nullptr);
}
FrostJIT::~FrostJIT() {
    if (code_buf_) munmap(code_buf_, CODE_BUF_SIZE);
}
// ── W^X protection toggle (reference-counted) ──────────────────────────
// make_writable: increment the write depth. If this is the first writer
// (depth was 0), mprotect the buffer to RW. Subsequent calls are no-ops
// (the buffer is already writable). This allows nested calls like
// translate_block → patch_chain without premature make_executable.
void FrostJIT::make_writable() {
    if (!wex_enabled_) return;
    if (wex_write_depth_ == 0) {
        // First writer: toggle buffer from RX to RW.
        if (mprotect(code_buf_, CODE_BUF_SIZE, PROT_READ | PROT_WRITE) != 0) {
            // mprotect failed — disable W^X and re-mmap as RWX to avoid hang.
            wex_enabled_ = false;
            void* p = mmap(nullptr, CODE_BUF_SIZE,
                           PROT_READ | PROT_WRITE | PROT_EXEC,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (p != MAP_FAILED) {
                if (p != code_buf_) {
                    memcpy(p, code_buf_, code_buf_used_);
                    munmap(code_buf_, CODE_BUF_SIZE);
                    code_buf_ = static_cast<uint8_t*>(p);
                }
                // p == code_buf_ means the same address was reused (unlikely
                // but valid) — no copy/munmap needed.
            } else {
                // Both mprotect AND mmap failed. The code buffer is in an
                // unknown state — disable JIT to prevent silent corruption.
                fprintf(stderr, "[%s] frostJIT: W^X fallback mmap also failed: %s\n",
                        CODENAME, strerror(errno));
                code_buf_ = nullptr;
            }
            return;
        }
    }
    wex_write_depth_++;
}
// make_executable: decrement the write depth. If this is the last writer
// (depth reaches 0), mprotect the buffer to RX. No-op if other writers
// are still active (nested calls).
void FrostJIT::make_executable() {
    if (!wex_enabled_) return;
    if (wex_write_depth_ <= 0) return;  // defensive: never go negative
    wex_write_depth_--;
    if (wex_write_depth_ == 0) {
        // Last writer done: toggle buffer from RW to RX.
        mprotect(code_buf_, CODE_BUF_SIZE, PROT_READ | PROT_EXEC);
        // If mprotect fails, leave the buffer writable (better than crashing).
    }
}
void FrostJIT::flush_cache() {
    // Clear block metadata. We don't need writable access for this —
    // blocks_/back_refs_/hot_pc_counts_ are STL containers, not the code
    // buffer. The old code called make_writable() here, leaving
    // wex_write_depth_=1 (unbalanced). The next translate_block would
    // then run with the buffer in RW state until its make_executable()
    // at the end — a security hole (W^X violated during execution).
    //
    // Fix: don't touch wex_write_depth_ here. translate_block calls
    // make_writable() at entry and make_executable() at exit, keeping
    // the buffer RX whenever JIT code might run.
    blocks_.clear();
    back_refs_.clear();
    tls_hot_pc_counts_.clear();  // clear hotness tracker (thread-local)
    code_buf_used_ = 0;
}
// ── dump_pc_histogram — where the JIT spends its time ───────────────────
// The SIGPROF sampler buckets "jit" (RIP inside the code buffer) but can't
// say WHICH guest code. This resolves a ring of sampled host RIPs back to
// guest PCs using the block table: every BlockEntry.fn points into the
// code buffer, so we sort blocks by their code offset and binary-search
// each sampled RIP into the [start, next_start) range (the code buffer is
// appended contiguously per block, so the next block's start is this
// block's end). Prints the top-N hottest guest PCs as a histogram — feed
// them to aarch64 objdump to map to functions (BIFROST_PROF=1 +
// BIFROST_PC_HIST=1 enables).
void FrostJIT::dump_pc_histogram(const uint64_t* rips, uint32_t n, uint32_t top) {
    if (n == 0) {
        fprintf(stderr, "[%s] pc-hist: no jit samples\n", CODENAME);
        return;
    }
    std::shared_lock<std::shared_mutex> lock(blocks_mutex_);
    // Build sorted array of (code offset, guest pc).
    std::vector<std::pair<uint64_t, uint64_t>> blocks;
    blocks.reserve(blocks_.size());
    for (auto& kv : blocks_) {
        const BlockEntry& e = kv.second;
        if (!e.fn) continue;  // interp_only blocks have no code
        blocks.emplace_back(static_cast<uint64_t>(reinterpret_cast<const uint8_t*>(e.fn) - code_buf_),
                            kv.first);
    }
    if (blocks.empty()) {
        fprintf(stderr, "[%s] pc-hist: no translated blocks\n", CODENAME);
        return;
    }
    std::sort(blocks.begin(), blocks.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    // Accumulate per-guest-pc sample counts.
    std::map<uint64_t, uint64_t> hist;
    uint64_t base = reinterpret_cast<uint64_t>(code_buf_);
    for (uint32_t i = 0; i < n; i++) {
        if (rips[i] < base) continue;
        uint64_t off = rips[i] - base;
        // Binary search: largest block start <= off.
        auto it = std::upper_bound(
            blocks.begin(), blocks.end(), std::make_pair(off, UINT64_MAX),
            [](const auto& a, const auto& b) { return a.first < b.first; });
        if (it == blocks.begin()) continue;
        --it;
        hist[it->second]++;
    }
    // Sort by count descending, print top.
    std::vector<std::pair<uint64_t, uint64_t>> sorted(hist.begin(), hist.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    fprintf(stderr, "[%s] pc-hist: %u samples across %zu guest PCs (top %u):\n",
            CODENAME, n, hist.size(), top);
    for (uint32_t i = 0; i < top && i < sorted.size(); i++) {
        fprintf(stderr, "  pc=0x%016llx x%llu\n",
                static_cast<unsigned long long>(sorted[i].first),
                static_cast<unsigned long long>(sorted[i].second));
    }
}
} // namespace arm64emu
