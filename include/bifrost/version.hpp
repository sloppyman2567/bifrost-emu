// bifrost/version.hpp — version constants for bifrost-emu.
//
// Public header: safe to include from any consumer (C++ or C-binding).
#pragma once

namespace arm64emu {

// ── Version ────────────────────────────────────────────────────────────
// 1.4.0-beta.3 (2026-06-26): JIT FP correctness overhaul — 39/39 tests
// pass under both interpreter and frostJIT. Fixed FCMP #0.0 form
// detection, FP 1-source opcode extraction (bits[20:15] not bits[15:12]),
// FMOV imm mask (now matches both single and double precision), FMOV
// imm vs SCVTF encoding collision, and added missing interpreter FCMP
// handler. Added shared fp_decode helpers in decoder.hpp to keep the
// interpreter and JIT's IR translator in sync.
constexpr const char* VERSION  = "1.4.0-beta.3";
constexpr const char* CODENAME = "bifrost-emu";

} // namespace arm64emu
