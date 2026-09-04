// bifrost.h — Public C API for libbifrost.
//
// This header provides a stable C interface to the bifrost-emu library,
// allowing it to be embedded in other programs — debuggers, IDE plugins,
// test harnesses, CI runners, static analyzers, emulators, and other
// tooling — without depending on C++ ABI details.
//
// Version: 1.5.5-alpha
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
// With JIT enabled (default since 1.4.0; current release 1.5.5-alpha):
//
//     bifrost_emu_t* emu = bifrost_create();
//     bifrost_load_elf(emu, "hello.elf", argc, argv);
//     int exit_code = bifrost_run(emu);   // JIT is on by default
//     bifrost_destroy(emu);
//
// Use bifrost_set_jit(emu, 0) before bifrost_run() to force the
// interpreter.
//
// For interactive use (single-stepping, state inspection):
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
// Execute a single instruction. Returns 0 on success, 1 if a breakpoint was
// hit (see below), -1 on error.
int bifrost_step(bifrost_emu_t* emu);
// Check if the emulator is still running (guest hasn't called exit).
int bifrost_is_running(const bifrost_emu_t* emu);
// Get the current exit code (valid after bifrost_run returns).
int bifrost_get_exit_code(const bifrost_emu_t* emu);
// ── Guest function calls ────────────────────────────────────────────────
// Call a guest function at `fn` with up to 8 integer arguments (x0-x7,
// from `args`) and up to 8 floating-point arguments (d0-d7, from `fargs`).
// The function is invoked with a scratch guest stack and a sentinel LR
// (0x1000), so it must return with `ret` like any normal AArch64 function.
// All CPU state is saved and restored around the call. Returns x0 after
// the callee returns (0 if fn==0, the call threw, or it exceeded the
// 50M-instruction step limit). Useful for invoking guest callbacks, init
// functions, or exported helpers from an embedder.
uint64_t bifrost_call(bifrost_emu_t* emu, uint64_t fn,
                      const int64_t* args, size_t n_args,
                      const double* fargs, size_t n_fargs);
// Same as bifrost_call, but returns the callee's FP return value (d0) as
// a double. For a callee that returns its result in an FP register (e.g.
// a C function returning double/float), use this instead of bifrost_call
// (whose return value is x0). Returns 0.0 on error (fn==0, throw, or step
// limit).
double bifrost_call_f(bifrost_emu_t* emu, uint64_t fn,
                      const int64_t* args, size_t n_args,
                      const double* fargs, size_t n_fargs);
// Look up a symbol by name across all loaded objects (dynamic binaries
// and thunk-registered symbols). Returns the absolute guest address, or
// 0 if not found (or the binary has no dynamic symbol table).
uint64_t bifrost_lookup_symbol(bifrost_emu_t* emu, const char* name);
// ── Guest dynamic linking ─────────────────────────────────────────────
// Load a guest AArch64 shared object into the emulator at runtime (the
// equivalent of dlopen). `path` is a host filesystem path. `flags` is
// accepted for API compatibility (RTLD_LAZY/RTLD_NOW/RTLD_GLOBAL) but
// resolution is always eager and symbols are always registered globally.
// Returns the guest load address (the library handle) on success, or 0
// on failure. If the library is already loaded, the existing handle is
// returned with its refcount bumped (glibc _dl_open semantics).
uint64_t bifrost_dlopen(bifrost_emu_t* emu, const char* path, int flags);
// Resolve a symbol within a specific loaded library's scope (the
// equivalent of dlsym with a handle): the library's own .dynsym first,
// then its DT_NEEDED dependencies. Returns the absolute guest address,
// or 0 if not found.
uint64_t bifrost_dlsym(bifrost_emu_t* emu, uint64_t handle, const char* name);
// Decrement a library's refcount (the equivalent of dlclose). When the
// refcount reaches 0 the library's DT_FINI_ARRAY runs (in reverse order)
// and the library is marked unloaded. Returns 0 on success, -1 on error
// (invalid handle / library not loaded).
int bifrost_dlclose(bifrost_emu_t* emu, uint64_t handle);
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
// ── JIT configuration ────────────────────────────────────────────────
// Enable/disable the frostJIT compiler. When enabled, the emulator
// translates ARM64 basic blocks to native x86-64 code on first
// execution and caches them. Subsequent executions of the same block
// run the cached native code directly, skipping decode + interpret.
//
// JIT is the default execution mode in 1.5.5-alpha (bifrost_run enables
// it automatically). It improves performance on compute-heavy workloads
// (~6.4x over the interpreter on bench_mips). Use bifrost_set_jit_verify()
// to catch divergences during development.
//
// If called, it should be called AFTER bifrost_load_elf() and BEFORE
// bifrost_run().
void bifrost_set_jit(bifrost_emu_t* emu, int enable);
// Check if the JIT is enabled (true by default).
int bifrost_get_jit(const bifrost_emu_t* emu);
// Enable/disable JIT verification mode. When enabled, the JIT runs
// each block through both the JIT compiler AND the interpreter, then
// compares the resulting CPU state. If they differ, the emulator
// prints the divergence and aborts. This is useful for debugging
// JIT correctness issues. Has significant performance overhead.
//
// Only effective when JIT is also enabled. Must be set BEFORE
// bifrost_run() (the verify flag is read when the first block is
// dispatched).
void bifrost_set_jit_verify(bifrost_emu_t* emu, int enable);
// ── JIT statistics ────────────────────────────────────────────────────
// JIT statistics structure. Filled by bifrost_get_jit_stats().
typedef struct {
    uint64_t blocks_translated;    // total blocks compiled to native code
    uint64_t blocks_executed;      // total block executions
    uint64_t cache_hits;           // executions that hit the code cache
    uint64_t cache_misses;         // executions that required translation
    uint64_t interpreter_fallbacks; // blocks that fell back to interpreter
    uint64_t block_chains_patched; // blocks chained via direct jumps
    size_t   code_cache_used;      // bytes used in the code cache
    size_t   code_cache_size;      // total bytes in the code cache
    size_t   cache_entries;        // number of cached blocks
} bifrost_jit_stats_t;
// Get JIT statistics. Returns 0 on success, -1 if JIT is not enabled.
int bifrost_get_jit_stats(const bifrost_emu_t* emu, bifrost_jit_stats_t* stats);
// ── Bulk execution ───────────────────────────────────────────────────
// Execute up to `count` instructions. Returns 0 on success, 1 if a
// breakpoint was hit before `count` steps completed (execution stops at
// the breakpoint), -1 on error.
// Useful for host-driven step loops: run a batch of instructions, then
// inspect state. More efficient than calling bifrost_step() in a loop
// because it avoids the per-call function-call overhead.
int bifrost_step_n(bifrost_emu_t* emu, uint64_t count);
// ── FP / SIMD register access ─────────────────────────────────────────
// ARM64 has 32 FP/SIMD registers (V0-V31), each 128 bits. We expose
// the low 64 bits (v_lo) and high 64 bits (v_hi) separately for
// portability across 32-bit and 64-bit hosts.
// Get the low 64 bits of FP register `reg` (0-31).
uint64_t bifrost_get_fp_reg_lo(const bifrost_emu_t* emu, int reg);
// Get the high 64 bits of FP register `reg` (0-31).
uint64_t bifrost_get_fp_reg_hi(const bifrost_emu_t* emu, int reg);
// Set both halves of FP register `reg` (0-31) atomically.
void bifrost_set_fp_reg(bifrost_emu_t* emu, int reg,
                        uint64_t lo, uint64_t hi);
// ── PSTATE / condition flags ─────────────────────────────────────────
// NZCV flags are in bits [31:28] of PSTATE: N=31, Z=30, C=29, V=28.
// Get the full 32-bit PSTATE register.
uint32_t bifrost_get_pstate(const bifrost_emu_t* emu);
// Set the full 32-bit PSTATE register.
void bifrost_set_pstate(bifrost_emu_t* emu, uint32_t value);
// Get a single condition flag. `flag`: 0=N, 1=Z, 2=C, 3=V. Returns 0 or 1.
int bifrost_get_flag(const bifrost_emu_t* emu, int flag);
// Set a single condition flag. `flag`: 0=N, 1=Z, 2=C, 3=V. `value`: 0 or 1.
void bifrost_set_flag(bifrost_emu_t* emu, int flag, int value);
// ── FPSR / FPCR ──────────────────────────────────────────────────────
// Get the FP Status Register (exception flags: IDC, IXC, UFC, OFC, DZC, IOC).
uint32_t bifrost_get_fpsr(const bifrost_emu_t* emu);
// Set the FP Status Register.
void bifrost_set_fpsr(bifrost_emu_t* emu, uint32_t value);
// Get the FP Control Register (rounding mode, FZ, DN, exception traps).
uint32_t bifrost_get_fpcr(const bifrost_emu_t* emu);
// Set the FP Control Register.
void bifrost_set_fpcr(bifrost_emu_t* emu, uint32_t value);
// ── JIT threshold ────────────────────────────────────────────────────
// Set the JIT warmup threshold: use the interpreter for the first `n`
// instructions, then switch to JIT. This avoids JIT compilation overhead
// for short programs. Set to 0 (default) to use JIT from the start.
void bifrost_set_jit_threshold(bifrost_emu_t* emu, uint64_t n);
// ── Syscall hook ──────────────────────────────────────────────────────
// SVC hook: called for EVERY guest syscall (interpreter AND JIT native
// svc both funnel through the emulator's syscall dispatcher; the internal
// thunk fast path for num==0x1000 bypasses it). The hook runs BEFORE the
// syscall is handled, with `args` pointing at x0-x5 (the syscall arg
// registers).
//
// Return 1 to handle the syscall yourself: set *result to the value the
// guest should see in x0 and skip the emulator's own dispatch. Return 0
// to let the emulator handle it normally. The hook may also observe and
// let the emulator proceed. Set to NULL to remove.
typedef int (*bifrost_svc_hook_fn)(void* userdata, uint64_t num,
                                   const uint64_t* args, uint64_t* result);
// Install/remove the syscall hook. Pass fn=NULL to remove. Returns 0 on
// success, -1 on error (NULL emu).
int bifrost_set_svc_hook(bifrost_emu_t* emu, bifrost_svc_hook_fn fn,
                         void* userdata);
// ── Breakpoints ──────────────────────────────────────────────────────
// Breakpoints are evaluated on the interpreter path (bifrost_step() /
// bifrost_step_n()): each step stops BEFORE executing the instruction at
// a breakpoint address and returns 1 (bifrost_step_n returns 1 as soon as
// it reaches a breakpoint, even before `count` steps elapse). The main
// execution loop (bifrost_run()) does NOT honor breakpoints — it runs to
// completion. Use bifrost_step()/bifrost_step_n() for breakpoint-based
// debugging.
//
// Set a breakpoint at guest address `addr`. Returns 0 on success, -1 on
// error (NULL emu).
int bifrost_set_breakpoint(bifrost_emu_t* emu, uint64_t addr);
// Remove a breakpoint at guest address `addr`. Returns 0 on success, -1
// on error.
int bifrost_remove_breakpoint(bifrost_emu_t* emu, uint64_t addr);
// ── Error reporting ──────────────────────────────────────────────────
// Get the last error message. Returns a pointer to a static buffer valid
// until the next API call on this handle. Returns an empty string if no
// error has occurred.
const char* bifrost_get_error(const bifrost_emu_t* emu);
// ── Version ─────────────────────────────────────────────────────────────
// Get the library version string (e.g., "1.5.5-alpha").
const char* bifrost_version(void);
#ifdef __cplusplus
} // extern "C"
#endif
