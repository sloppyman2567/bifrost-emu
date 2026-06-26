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
    jit_ = std::make_unique<FrostJIT>();
    jit_enabled_ = (jit_ != nullptr);
    if (jit_enabled_) {
        jit_->set_direct_window(mem_.direct_window());
        if (verbose_)
            fprintf(stderr, "[%s] frostJIT enabled (x86 codegen)\n", CODENAME);
    }
}

void Emulator::print_jit_stats() {
    if (!jit_ || !jit_enabled_) return;
    fprintf(stderr, "[%s] frostJIT: %llu blocks translated, %llu executed "
            "(%llu instructions, %llu cache hits, %llu misses, %llu fallbacks, "
            "%llu chains)\n",
            CODENAME,
            static_cast<unsigned long long>(jit_->blocks_translated),
            static_cast<unsigned long long>(jit_->blocks_executed),
            static_cast<unsigned long long>(jit_->instructions_executed),
            static_cast<unsigned long long>(jit_->cache_hits),
            static_cast<unsigned long long>(jit_->cache_misses),
            static_cast<unsigned long long>(jit_->interpreter_fallbacks),
            static_cast<unsigned long long>(jit_->block_chains_patched));
    fprintf(stderr, "[%s] frostJIT: code cache %zu/%zu bytes, %zu blocks\n",
            CODENAME, jit_->code_buf_used(), jit_->code_buf_size(),
            jit_->cache_entries());
    if (jit_->blocks_executed > 0) {
        double avg = static_cast<double>(jit_->instructions_executed) /
                     static_cast<double>(jit_->blocks_executed);
        fprintf(stderr, "[%s] frostJIT: avg %.1f instructions/block\n",
                CODENAME, avg);
    }
}

void Emulator::jit_step(CPU& cpu) {
    jit_->run_block(cpu, *this);
}

} // namespace arm64emu
