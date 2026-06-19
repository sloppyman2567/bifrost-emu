// jit_glue.cpp — Glue between Emulator and FrostJIT (v1.4.0-alpha).
//
// Implements Emulator::enable_jit(), which lazily constructs the
// FrostJIT instance. Kept in a separate translation unit so that
// arm64_emu.hpp doesn't need to #include "frostjit.hpp" (which would
// pull the JIT's x86_64 emitter internals into every TU that includes
// the emulator header).

#include "arm64_emu.hpp"
#include "frostjit.hpp"

namespace arm64emu {

// Emulator's constructor/destructor are declared out-of-line here
// (in jit_glue.cpp) so that the unique_ptr<FrostJIT> member can be
// destroyed without requiring frostjit.hpp to be included by every
// TU that uses arm64_emu.hpp.
Emulator::Emulator() = default;
Emulator::~Emulator() = default;

void Emulator::enable_jit() {
    if (jit_) return;  // already enabled
    jit_ = std::make_unique<FrostJIT>();
    jit_enabled_ = (jit_ != nullptr);
    if (jit_enabled_ && verbose_) {
        fprintf(stderr, "[%s] frostJIT enabled (experimental)\n", CODENAME);
    }
}

void Emulator::print_jit_stats() {
    if (!jit_ || !jit_enabled_) return;
    fprintf(stderr, "[%s] frostJIT: %llu blocks translated, %llu executed "
            "(%llu cache hits, %llu misses, %llu interpreter fallbacks)\n",
            CODENAME,
            (unsigned long long)jit_->blocks_translated,
            (unsigned long long)jit_->blocks_executed,
            (unsigned long long)jit_->cache_hits,
            (unsigned long long)jit_->cache_misses,
            (unsigned long long)jit_->interpreter_fallbacks);
    fprintf(stderr, "[%s] frostJIT: code cache %zu/%zu bytes, %zu entries\n",
            CODENAME, jit_->code_buf_used(), jit_->code_buf_size(),
            jit_->cache_entries());
}

void Emulator::jit_step(CPU& cpu) {
    // Run one JIT-translated block. The JIT returns the next PC; if
    // translation failed for the current PC, it falls back to the
    // interpreter's step_public() for a single instruction.
    jit_->run_block(cpu, *this);
}

} // namespace arm64emu
