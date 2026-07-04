// jit/jit_flags.cpp — FrostJIT flag materialization helpers.
//
// v1.4.5-alpha (Turn 36): split out of frostjit.cpp. Holds the
// clobber_flags() and materialize_flags_to_pstate() methods, which
// manage the lazy materialization of guest NZCV flags from host RFLAGS
// to cpu.pstate. These are called by compile_ir_inst (in frostjit.cpp
// and jit_codegen_fp.cpp) before any instruction that would clobber
// RFLAGS or at block exits.
#include "jit/frostjit.hpp"
#include "core/emulator.h"

#include <cstdint>

namespace arm64emu {
// ── clobber_flags ───────────────────────────────────────────────────────
// If the host RFLAGS currently hold valid guest NZCV (flags_in_host_),
// materialize them to cpu.pstate BEFORE a flag-clobbering instruction
// overwrites them. Without this, any ALU op after ADDS/SUBS/TST would
// lose the flags, causing wrong branch decisions downstream.
void FrostJIT::clobber_flags() {
    if (flags_in_host_) {
        // emit_materialize_flags clobbers RAX, RCX, RDX.
        // Spill dirty vregs AND non-dirty scratch vregs BEFORE the
        // materialize — otherwise non-dirty scratch vregs cached in
        // RAX/RCX/RDX are lost (their value is only in the host reg,
        // and materialize overwrites it before invalidate_host_regs
        // can drop the mapping). This was the root cause of the FWD
        // crash on `toybox ls /` (x30 store using a stale/garbage value).
        constexpr uint16_t FLAGS_CLOBBER =
            (1u << RAX) | (1u << RCX) | (1u << RDX);
        flush_dirty_host_regs(FLAGS_CLOBBER);
        flush_scratch_host_regs(FLAGS_CLOBBER);
        emit_materialize_flags(flags_from_sub_);
        flags_in_host_ = false;
        // Drop cache mappings for RAX/RCX/RDX (values were spilled above).
        invalidate_host_regs(FLAGS_CLOBBER);
    }
}

// Materialize host flags to pstate, preserving RFLAGS around the materialize
// (which clobbers them). Used at block exits (BRCOND fall-through and taken
// paths) where the next block may read pstate. No-op if flags aren't
// currently in host. Does NOT clear flags_in_host_ — the caller manages that,
// since BRCOND emits this on both paths before clearing flags_in_host_ at the
// end.
void FrostJIT::materialize_flags_to_pstate() {
    if (!flags_in_host_) return;
    constexpr uint16_t FLAGS3 = (1u << RAX) | (1u << RCX) | (1u << RDX);
    emit_pushfq();  // save RFLAGS (materialize clobbers them)
    flush_dirty_host_regs(FLAGS3);
    flush_scratch_host_regs(FLAGS3);
    emit_materialize_flags(flags_from_sub_);
    emit_popfq();
    invalidate_host_regs(FLAGS3);
}

// ── Block chaining helpers ──────────────────────────────────────────────
// Patch a block's 5-byte chain slot (originally `ret` + 4 NOPs) in place
// to `jmp rel32` → target_fn. x86 is icache-coherent, so no explicit
// cache flush is needed, but we emit a memory barrier to ensure the
// patched bytes are visible to any in-flight execution on the same core.

// single source of truth for "will this instruction route
// to CALL_INTERP in the IR translator?" Used by the block splitter to
// pre-scan before translating. If this list gets out of sync with
// ir_translate.cpp's default case, the splitter would misclassify
// instructions.
//
// Note: SIMD_LD1, SIMD_ST1, SIMD_DUP, and SIMD_LOGICAL have native IR
// paths (IROp::SIMD_LDST / SIMD_DUP / SIMD_LOGICAL) and do NOT route to
// CALL_INTERP for the common cases — they are deliberately NOT listed
// here so the block splitter doesn't fragment blocks around them.
// SIMD_LOGICAL falls back to CALL_INTERP only for unrecognized opcodes
// (its `default:` case), but that's rare enough to accept the risk.
//
// v1.4.5-alpha (Turn 36): instr_will_call_interp() was moved to
// jit_translate.cpp (it's only used by translate_block).

} // namespace arm64emu
