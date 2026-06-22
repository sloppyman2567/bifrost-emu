// jit_glue.cpp — Glue between Emulator and FrostJIT.
#include "arm64_emu.hpp"
#include "frostjit.hpp"

namespace arm64emu {

Emulator::Emulator() = default;
Emulator::~Emulator() = default;

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
            "(%llu cache hits, %llu misses, %llu fallbacks, %llu chains)\n",
            CODENAME,
            (unsigned long long)jit_->blocks_translated,
            (unsigned long long)jit_->blocks_executed,
            (unsigned long long)jit_->cache_hits,
            (unsigned long long)jit_->cache_misses,
            (unsigned long long)jit_->interpreter_fallbacks,
            (unsigned long long)jit_->block_chains_patched);
    fprintf(stderr, "[%s] frostJIT: code cache %zu/%zu bytes, %zu blocks\n",
            CODENAME, jit_->code_buf_used(), jit_->code_buf_size(),
            jit_->cache_entries());
}

void Emulator::jit_step(CPU& cpu) {
    jit_->run_block(cpu, *this);
}

} // namespace arm64emu
