// bifrost/version.hpp — version constants for bifrost-emu.
//
// Public header: safe to include from any consumer (C++ or C-binding).
#pragma once

namespace arm64emu {

// ── Version ────────────────────────────────────────────────────────────
// 1.4.0-beta.3 (2026-06-26): JIT is now the default execution mode.
// 36/36 JIT tests pass; the ctest/jit_*.elf regression suite also
// passes under the interpreter (--no-jit) to catch decoder drift.
// Major work in this release: FCMP #0.0 form detection, FP 1-source
// opcode extraction (bits[20:15] not bits[15:12]), FMOV imm mask
// (now matches both single and double precision), FMOV imm vs SCVTF
// encoding collision, missing interpreter FCMP handler, 32-bit ASR
// sign-extension (interpreter + JIT), int<->FP conversion pipeline
// (SCVTF/UCVTF/FCVTZS/FCVTZU — 9 bugs), system register width (FPSR/
// FPCR 32-bit reads in JIT), and JIT performance (self-loop chaining,
// liveness-based regalloc, register-cache-aware ALU codegen) — 571
// MIPS on bench_mips (6.4x over interpreter). Added shared fp_decode
// helpers in decoder.hpp to keep the interpreter and JIT's IR
// translator in sync.
constexpr const char* VERSION  = "1.4.0-beta.3";
constexpr const char* CODENAME = "bifrost-emu";

} // namespace arm64emu
