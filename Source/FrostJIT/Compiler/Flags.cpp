// Flags.cpp — FrostJIT flag materialization helpers.
//
// split out of FrostJIT.cpp. Holds the
// clobber_flags() and materialize_flags_to_pstate() methods, which
// manage the lazy materialization of guest NZCV flags from host RFLAGS
// to cpu.pstate. These are called by compile_ir_inst (in FrostJIT.cpp
// and Dispatch.cpp) before any instruction that would clobber
// RFLAGS or at block exits.
#include "FrostJIT/Compiler/BlockAnalysis.hpp"
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include "System/Core/Emulator.hpp"
#include <cstdint>
#include <cstring>
namespace arm64emu {
// ── clobber_flags ───────────────────────────────────────────────────────
// If the host RFLAGS currently hold valid guest NZCV (flags_in_host_),
// materialize them to cpu.pstate BEFORE a flag-clobbering instruction
// overwrites them. Without this, any ALU op after ADDS/SUBS/TST would
// lose the flags, causing wrong branch decisions downstream.
void FrostJIT::clobber_flags(bool dead, bool rdx_only) {
    if (flags_in_host_) {
        dead = dead || host_flags_match_pstate();
        // emit_materialize_flags overwrites RDX only (it saves and restores RAX,
        // never touches RCX). Callers that go on to pick registers through the
        // allocator can leave RAX/RCX mapped; the rest take all three, as an
        // unspecific caller may use RAX/RCX/RDX as fixed scratch next.
        const uint16_t FLAGS_CLOBBER = rdx_only
            ? static_cast<uint16_t>(1u << RDX)
            : static_cast<uint16_t>((1u << RAX) | (1u << RCX) | (1u << RDX));
        // Spill dirty vregs AND non-dirty scratch vregs BEFORE the
        // materialize — otherwise non-dirty scratch vregs cached in the
        // clobbered registers are lost (their value is only in the host reg,
        // and materialize overwrites it before invalidate_host_regs can drop
        // the mapping). This was the root cause of the FWD crash on
        // `toybox ls /` (x30 store using a stale/garbage value).
        flush_dirty_host_regs(FLAGS_CLOBBER);
        flush_scratch_host_regs(FLAGS_CLOBBER);
        if (!dead) emit_materialize_flags(flags_from_sub_);
        flags_in_host_ = false;
        // Drop cache mappings for the clobbered registers (values were spilled above).
        invalidate_host_regs(FLAGS_CLOBBER);
    }
}
// Materialize host flags to pstate. Preserve RFLAGS when a later instruction
// still consumes them; BRCOND has already consumed its condition and opts out.
// Both branch exits still commit guest NZCV. No-op if flags aren't
// currently in host. Does NOT clear flags_in_host_ — the caller manages that,
// since BRCOND emits this on both paths before clearing flags_in_host_ at the
// end.
void FrostJIT::materialize_flags_to_pstate(bool preserve_flags, bool dead) {
    if (!flags_in_host_) return;
    dead = dead || host_flags_match_pstate();
    constexpr uint16_t FLAGS3 = (1u << RAX) | (1u << RCX) | (1u << RDX);
    if (!dead && preserve_flags) emit_pushfq();
    flush_dirty_host_regs(FLAGS3);
    flush_scratch_host_regs(FLAGS3);
    if (!dead) emit_materialize_flags(flags_from_sub_);
    if (!dead && preserve_flags) emit_popfq();
    invalidate_host_regs(FLAGS3);
}

bool FrostJIT::exit_flags_dead(uint64_t pc) {
    if (!flag_scan_active_ || !window_base_ || !guest_memory_) return false;
    for (const auto& dep : flag_deps_scratch_)
        if (dep.pc == pc) return true;  // only dead verdicts are recorded
    // Read the words directly, only from pages that are plainly mapped and
    // executable: this is a prediction about code that may never run, so it
    // must not populate a missing page or raise a fault.
    constexpr int kWords = jit_analysis::kNzcvScanWords;
    constexpr uint64_t kBytes = kWords * 4;
    const uint64_t window = guest_memory_->window_size();
    if ((pc & 3) || pc > window || kBytes > window - pc) return false;
    const auto* flags = guest_memory_->direct_page_flags();
    constexpr uint8_t need = Memory::GUEST_PAGE_MAPPED | Memory::GUEST_PROT_EXEC;
    for (uint64_t p = pc / Memory::PAGE_SIZE; p <= (pc + kBytes - 1) / Memory::PAGE_SIZE; ++p) {
        const uint8_t f = flags[p].load(std::memory_order_acquire);
        if ((f & need) != need || (f & Memory::GUEST_PAGE_UFFD_MISSING)) return false;
    }
    uint32_t words[kWords];
    std::memcpy(words, window_base_ + pc, sizeof words);
    int used = 0;
    if (!jit_analysis::nzcv_dead_in(words, kWords, used) || used <= 0) return false;
    flag_deps_scratch_.push_back({pc, std::vector<uint32_t>(words, words + used)});
    flag_store_skipped_ = true;
    return true;
}
// ── Block chaining helpers ──────────────────────────────────────────────
// Patch a block's 5-byte chain slot (originally `ret` + 4 NOPs) in place
// to `jmp rel32` → target_fn. x86 is icache-coherent, so no explicit
// cache flush is needed, but we emit a memory barrier to ensure the
// patched bytes are visible to any in-flight execution on the same core.
// single source of truth for "will this instruction route
// to CALL_INTERP in the IR translator?" Used by the block splitter to
// pre-scan before translating. If this list gets out of sync with
// Integer.cpp's default case, the splitter would misclassify
// instructions.
//
// Note: SIMD_LD1, SIMD_ST1, SIMD_DUP, and SIMD_LOGICAL have native IR
// paths (IROp::SIMD_LDST / SIMD_DUP / SIMD_LOGICAL) and do NOT route to
// CALL_INTERP for the common cases — they are deliberately NOT listed
// here so the block splitter doesn't fragment blocks around them.
// SIMD_LOGICAL falls back to CALL_INTERP only for unrecognized opcodes
// (its `default:` case), but that's rare enough to accept the risk.
//
// instr_will_call_interp() was moved to
// BlockCompiler.cpp (it's only used by translate_block).
} // namespace arm64emu
