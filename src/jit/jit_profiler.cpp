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

#include <sys/mman.h>

namespace arm64emu {

// ── Construction ────────────────────────────────────────────────────────
FrostJIT::FrostJIT() {
    void* p = mmap(nullptr, CODE_BUF_SIZE,
                   PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p != MAP_FAILED) code_buf_ = static_cast<uint8_t*>(p);
    // Counters start at zero (declared in the public header).
    blocks_translated = 0;
    blocks_executed   = 0;
    cache_hits        = 0;
    cache_misses      = 0;
    interpreter_fallbacks = 0;
    block_chains_patched = 0;

    // Watchdog state.
    watchdog_last_pc_ = UINT64_MAX;
    watchdog_count_   = 0;
    total_blocks_executed_ = 0;
    jit_disabled_ = false;

    // Initialize vreg arrays — prev_max_vreg_ must be large enough that
    // the first translate_block() clears all 4096 entries. Without this,
    // uninitialized vreg_home_[] garbage causes load_vreg_to_reg to think
    // vregs are cached in random host regs, producing mov-from-garbage.
    prev_max_vreg_ = 4095;
    for (int i = 0; i < 4096; i++) {
        vreg_home_[i] = -1;
        vreg_dirty_[i] = false;
        vreg_slot_[i] = 0;
    }
    for (int i = 0; i < 16; i++) reg_vreg_[i] = -1;
    max_vreg_ = 0;
}

FrostJIT::~FrostJIT() {
    if (code_buf_) munmap(code_buf_, CODE_BUF_SIZE);
}

void FrostJIT::flush_cache() {
    blocks_.clear();
    back_refs_.clear();
    code_buf_used_ = 0;
}

} // namespace arm64emu
