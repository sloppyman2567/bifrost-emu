// bifrost.h — Public C API for libbifrost.
//
// This header provides a stable C interface to the bifrost-emu library,
// allowing it to be embedded in other applications (debuggers, IDE
// plugins, test harnesses, etc.) without depending on C++.
//
// Basic usage:
//
//     #include "bifrost.h"
//
//     bifrost_emu_t* emu = bifrost_create();
//     bifrost_load_elf(emu, "hello.elf", argc, argv);
//     int exit_code = bifrost_run(emu);
//     bifrost_destroy(emu);
//
// For interactive use (single-stepping, breakpoints):
//
//     bifrost_emu_t* emu = bifrost_create();
//     bifrost_load_elf(emu, "program.elf", argc, argv);
//     bifrost_set_trace(emu, true);       // enable instruction trace
//     while (bifrost_is_running(emu)) {
//         bifrost_step(emu);              // execute one instruction
//         uint64_t pc = bifrost_get_pc(emu);
//         if (pc == breakpoint_addr) {
//             // inspect state...
//             uint64_t x0 = bifrost_get_reg(emu, 0);
//             bifrost_set_reg(emu, 0, new_x0_value);
//         }
//     }
//     bifrost_destroy(emu);
//
// Thread safety: a single bifrost_emu_t handle is NOT thread-safe.
// Different handles (different emulated processes) can be used from
// different threads.
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle to the emulator.
typedef struct bifrost_emu bifrost_emu_t;

// ── Lifecycle ───────────────────────────────────────────────────────────

// Create a new emulator instance.
// Returns NULL on failure.
bifrost_emu_t* bifrost_create(void);

// Destroy an emulator instance and free all resources.
void bifrost_destroy(bifrost_emu_t* emu);

// ── Loading ─────────────────────────────────────────────────────────────

// Load an ELF binary into the emulator.
//   path  — path to the ELF file
//   argc  — number of arguments (including argv[0] = program name)
//   argv  — array of argument strings (NULL-terminated)
// Returns 0 on success, -1 on failure.
int bifrost_load_elf(bifrost_emu_t* emu, const char* path,
                     int argc, const char* const* argv);

// ── Execution ───────────────────────────────────────────────────────────

// Run the emulator until the guest calls exit().
// Returns the guest's exit code.
int bifrost_run(bifrost_emu_t* emu);

// Execute a single instruction. Returns 0 on success, -1 on error.
int bifrost_step(bifrost_emu_t* emu);

// Check if the emulator is still running (guest hasn't called exit).
int bifrost_is_running(const bifrost_emu_t* emu);

// Get the current exit code (valid after bifrost_run returns).
int bifrost_get_exit_code(const bifrost_emu_t* emu);

// ── Register access ─────────────────────────────────────────────────────

// Get a general-purpose register (0-30). x31 reads as 0 (XZR).
uint64_t bifrost_get_reg(const bifrost_emu_t* emu, int reg);

// Set a general-purpose register (0-30). x31 is ignored (XZR).
void bifrost_set_reg(bifrost_emu_t* emu, int reg, uint64_t value);

// Get the stack pointer (SP).
uint64_t bifrost_get_sp(const bifrost_emu_t* emu);

// Set the stack pointer (SP).
void bifrost_set_sp(bifrost_emu_t* emu, uint64_t value);

// Get the program counter (PC).
uint64_t bifrost_get_pc(const bifrost_emu_t* emu);

// Set the program counter (PC).
void bifrost_set_pc(bifrost_emu_t* emu, uint64_t value);

// ── Memory access ───────────────────────────────────────────────────────

// Read `len` bytes from guest address `addr` into `buf`.
// Returns 0 on success, -1 on failure (unmapped memory).
int bifrost_read_mem(const bifrost_emu_t* emu, uint64_t addr,
                     void* buf, size_t len);

// Write `len` bytes to guest address `addr` from `buf`.
// Returns 0 on success, -1 on failure.
int bifrost_write_mem(bifrost_emu_t* emu, uint64_t addr,
                      const void* buf, size_t len);

// ── Configuration ───────────────────────────────────────────────────────

// Enable/disable instruction tracing (prints every instruction to stderr).
void bifrost_set_trace(bifrost_emu_t* emu, int enable);

// Enable/disable verbose mode (prints execution stats on exit).
void bifrost_set_verbose(bifrost_emu_t* emu, int enable);

// ── Version ─────────────────────────────────────────────────────────────

// Get the library version string (e.g., "1.1.5-alpha.1").
const char* bifrost_version(void);

#ifdef __cplusplus
} // extern "C"
#endif
