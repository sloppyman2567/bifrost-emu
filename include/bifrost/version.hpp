// bifrost/version.hpp — version constants for bifrost-emu.
//
// Public header: safe to include from any consumer (C++ or C-binding).
#pragma once

namespace arm64emu {

// ── Version ────────────────────────────────────────────────────────────
// 1.4.0-rc.0 (2026-06-26): Release candidate. JIT is the default
// execution mode. 37/37 JIT tests pass; the ctest/jit_*.elf regression
// suite also passes under the interpreter (--no-jit) to catch decoder
// drift. Key changes since beta.3:
//   - SIGSEGV delivery for JIT'd memory faults: exceptions thrown from
//     JIT'd code (which has no DWARF unwind info) are now caught at the
//     C-helper boundary (jit_load_mem_slow / jit_store_mem_slow /
//     jit_interp_step) and translated to SIGSEGV signal delivery,
//     matching the interpreter path. Previously these faults called
//     std::terminate (SIGABRT, rc=134); now they exit cleanly with
//     rc=139 (or invoke the guest's SIGSEGV handler if installed).
//   - toybox sh -c regression documented (exits 139 instead of 134).
//   - Docs/Makefile cleanup: test target no longer uses --jit, skips
//     interactive programs, redirects stdin from /dev/null.
constexpr const char* VERSION  = "1.4.0-rc.0";
constexpr const char* CODENAME = "bifrost-emu";

} // namespace arm64emu
