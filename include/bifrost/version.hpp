// bifrost/version.hpp — version constants for bifrost-emu.
//
// Public header: safe to include from any consumer (C++ or C-binding).
#pragma once

namespace arm64emu {

// ── Version ────────────────────────────────────────────────────────────
// — directory-layout overhaul + VFS abstraction.
//   * arm64_emu.hpp "god header" split into include/bifrost/{types,emulator,
//     version}.hpp + src/core/{cpu,memory,emulator,signal}.h/.cpp.
//   * frostjit.cpp (3671 LOC) split into src/jit/{frostjit, jit_cache,
//     jit_profiler, x86_backend, x86_regalloc}.cpp + private headers.
//   * ir.cpp (1515 LOC) split into src/ir/{ir_builder, ir_translate,
//     ir_lower, ir_optimize, ops}.cpp.
//   * syscalls.cpp (1889 LOC) split into src/syscalls/{syscalls, fs,
//     mem, threads, time, ioctls}.cpp.
//   * VFS overhaul: new src/vfs/ module with VNode base + FdTable.
//   * Dead code removed (track_allocation, map_direct, three DEAD
//     frostjit cases, never-defined Emulator_step/syscall/execute
//     friend wrappers).
constexpr const char* VERSION  = "1.4.0-beta.3";
constexpr const char* CODENAME = "bifrost-emu";

} // namespace arm64emu
