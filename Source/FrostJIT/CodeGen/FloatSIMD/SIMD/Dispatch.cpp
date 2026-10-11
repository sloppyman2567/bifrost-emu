
// ── FrostJIT::compile_ir_simd ─────────────────────────────────────────
// Handles all SIMD_* IR ops. Returns true if the op was handled, false if
// not (caller falls through to the next dispatcher or residual switch).
// The case bodies below are verbatim from Dispatch.cpp ()
// — no logic changes, just moved to a separate file/method.
#include "FrostJIT/Compiler/FrostJIT.hpp"
namespace arm64emu {
bool FrostJIT::compile_ir_simd(const IRInst& inst) {
    return compile_simd_arithmetic_(inst) || compile_simd_floating_point_(inst) ||
           compile_simd_moves_(inst) || compile_simd_memory_(inst) ||
           compile_simd_shifts_(inst) || compile_simd_elements_(inst) ||
           compile_simd_widening_(inst);
}
}  // namespace arm64emu
