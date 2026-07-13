// jit_glue.cpp — Glue between Emulator and FrostJIT.
//
// Emulator's constructor/destructor live in src/core/emulator.cpp.
// This file holds the JIT-specific Emulator methods that need to see
// FrostJIT's full definition (held via unique_ptr in Emulator).
#include "core/emulator.h"
#include "jit/frostjit.hpp"

namespace arm64emu {

void Emulator::enable_jit() {
    if (jit_) return;
    // Shared-JIT mode (default): spawned threads share the main's FrostJIT,
    // saving 64 MiB per thread. This requires the code buffer to be RWX
    // (not W^X) so translation (writes) and execution (reads/exec) can
    // happen concurrently. W^X would toggle mprotect on the WHOLE buffer,
    // crashing any thread executing JIT code.
    // We force-disable W^X by setting BIFROST_NO_WEX before the FrostJIT
    // constructor runs (it checks the env var at construction).
    // Opt out via BIFROST_NO_SHARED_JIT=1 for per-thread JIT (lock-free,
    // W^X-protected, but 64 MiB per thread).
    // Security tradeoff: acceptable — the emulator is a single-process
    // user-mode emulator; the only code in the JIT buffer is generated
    // from the trusted guest binary.
    static bool shared_jit_checked = false;
    static bool shared_jit_mode = false;
    if (!shared_jit_checked) {
        shared_jit_mode = (getenv("BIFROST_NO_SHARED_JIT") == nullptr);
        shared_jit_checked = true;
    }
    if (shared_jit_mode) {
        setenv("BIFROST_NO_WEX", "1", 1);
    }
    jit_ = std::make_unique<FrostJIT>();
    jit_enabled_ = (jit_ != nullptr);
    if (jit_enabled_) {
        jit_->set_direct_window(mem_.direct_window());
        if (verbose_) {
            const auto& cf = jit_->cpu_features();
            fprintf(stderr, "[%s] frostJIT enabled (x86 codegen, %s mode, "
                    "features: %s)\n",
                    CODENAME, shared_jit_mode ? "shared" : "per-thread",
                    cpu_features_string(cf));
        }
    }
}

void Emulator::print_jit_stats() {
    if (!jit_ || !jit_enabled_) return;
    // Aggregate main JIT + all per-thread JITs for a complete picture.
    uint64_t blocks_translated = jit_->blocks_translated;
    uint64_t blocks_executed   = jit_->blocks_executed;
    uint64_t instructions      = jit_->instructions_executed;
    uint64_t cache_hits        = jit_->cache_hits;
    uint64_t cache_misses      = jit_->cache_misses;
    uint64_t fallbacks         = jit_->interpreter_fallbacks;
    uint64_t chains            = jit_->block_chains_patched;
    size_t    code_used        = jit_->code_buf_used();
    size_t    code_size        = jit_->code_buf_size();
    size_t    cache_entries    = jit_->cache_entries();
    size_t    num_jits         = 1;  // main JIT

    {
        std::lock_guard<std::mutex> g(threads_mu_);
        for (auto& gt : threads_) {
            if (gt->jit) {
                blocks_translated += gt->jit->blocks_translated;
                blocks_executed   += gt->jit->blocks_executed;
                instructions      += gt->jit->instructions_executed;
                cache_hits        += gt->jit->cache_hits;
                cache_misses      += gt->jit->cache_misses;
                fallbacks         += gt->jit->interpreter_fallbacks;
                chains            += gt->jit->block_chains_patched;
                code_used         += gt->jit->code_buf_used();
                cache_entries     += gt->jit->cache_entries();
                num_jits++;
            }
        }
    }

    fprintf(stderr, "[%s] frostJIT (%zu thread%s): %llu blocks translated, "
            "%llu executed (%llu instructions, %llu cache hits, %llu misses, "
            "%llu fallbacks, %llu chains)\n",
            CODENAME, num_jits, num_jits == 1 ? "" : "s",
            static_cast<unsigned long long>(blocks_translated),
            static_cast<unsigned long long>(blocks_executed),
            static_cast<unsigned long long>(instructions),
            static_cast<unsigned long long>(cache_hits),
            static_cast<unsigned long long>(cache_misses),
            static_cast<unsigned long long>(fallbacks),
            static_cast<unsigned long long>(chains));
    fprintf(stderr, "[%s] frostJIT: code cache %zu/%zu bytes, %zu blocks\n",
            CODENAME, code_used, code_size * num_jits, cache_entries);
    if (blocks_executed > 0) {
        double avg = static_cast<double>(instructions) /
                     static_cast<double>(blocks_executed);
        fprintf(stderr, "[%s] frostJIT: avg %.1f instructions/block\n",
                CODENAME, avg);
    }
}

void Emulator::jit_step(CPU& cpu) {
    jit_->run_block(cpu, *this);
}

// Turn 93: BL_CALL helper — called from JIT code to invoke a callee.
// Runs the callee until it returns (PC = LR). This means dispatching
// multiple blocks in a loop — the callee's first block only executes
// part of the function. We must keep dispatching until RET sets PC=LR.
extern "C" uint64_t jit_call_helper(CPU* cpu, Emulator* emu, uint64_t target_pc) {
    uint64_t return_pc = cpu->regs[30];  // LR set by BL_CALL's STORE_REG
    cpu->pc = target_pc;

    auto* jit = emu->jit();
    int steps = 0;
    while (cpu->running && cpu->pc != return_pc) {
        if (jit) {
            auto fn = jit->lookup_only(cpu->pc);
            if (!fn) fn = jit->translate_and_lookup(*emu, cpu->pc);
            if (fn) {
                cpu->pc = fn(cpu, emu);
                if (++steps > 10000000) break;
                continue;
            }
        }
        emu->step(*cpu);
        if (++steps > 10000000) break;
    }
    return cpu->pc;
}

} // namespace arm64emu
