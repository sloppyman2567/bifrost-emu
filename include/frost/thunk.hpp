// frost/thunk.hpp — GraphicThunk: forward guest GL/EGL/SDL2 calls to host.
//
// v1.4.5-alpha: NEW. The GraphicThunk class is forward-
// declared in frost/graphics.hpp (so the header doesn't pull in dlfcn.h
// / GL / EGL / SDL2 headers). This header provides the full class
// definition, needed by:
//   - frost_graphics/graphics.cpp (FrostGraphics destructor destroys
//     the unique_ptr<GraphicThunk> member)
//   - frost_graphics/thunk.cpp (the class implementation)
//   - src/frontend/dynamic_linker.cpp (calls GraphicThunk::resolve)
//   - src/syscalls/thunk.cpp (calls GraphicThunk::dispatch)
//
// External consumers (libbifrost users) do NOT need to include this
// header — they use FrostGraphics::thunk() which returns a
// GraphicThunk* (forward-declared).
//
// ── How it works (Turn 37 redesign) ───────────────────────────────────
// The thunk maintains a registry of (library, symbol) → (host_fn_ptr,
// guest_trampoline_addr). Each registered symbol gets a 16-byte guest
// trampoline that:
//
//   1. Loads its symbol_id into x9.
//   2. Loads __NR_bifrost_thunk into x8.
//   3. Issues `svc #0` to trap into the host.
//
// The host's syscall dispatcher recognizes __NR_bifrost_thunk and calls
// GraphicThunk::dispatch(cpu, sym_id), which:
//
//   1. Looks up the host function pointer for sym_id.
//   2. Reads the first 8 args from cpu.regs[0..7] (per AArch64 AAPCS).
//   3. Calls the host function (with optional pointer-arg translation).
//   4. Writes the return value to cpu.regs[0].
//
// This is the standard "thunk" pattern used by user-mode emulators
// (QEMU user, Rosetta 2, FEX-Emu) to forward guest calls to host
// libraries without recompiling them.
//
// ── Why not just dlsym(RTLD_DEFAULT)? ─────────────────────────────────
// The Turn 36 implementation returned host function pointers directly
// via dlsym(RTLD_DEFAULT, ...). This is fundamentally broken: those are
// x86-64 function pointers in the host's address space, and the guest
// would try to execute them as AArch64 code, crashing with SIGILL.
// The Turn 37 redesign fixes this by returning GUEST-CALLABLE
// trampoline addresses instead.
//
// ── Limitations (inherited from Turn 36) ──────────────────────────────
//   - Pointer arguments are translated assuming the guest pointer is
//     in the emulator's address space (via Memory::host_addr()). This
//     works for heap/stack pointers but NOT for pointers the guest
//     got from mmap'd host resources (rare).
//   - No state tracking: glEnable(GL_DEPTH_TEST) is forwarded but not
//     recorded. Fine for immediate-mode GL, breaks retained-mode.
//   - No shader translation: GLSL is text, identical AArch64↔x86-64.
//   - No EGL surface management: single hidden SDL2 window for EGL.
//   - No GLESv1 (fixed-function) support: only GLESv2 (programmable).
//   - Variadic functions (printf-style) are NOT supported — the thunk
//     passes exactly 8 args, ignoring variadic extras. Most GL/EGL/
//     SDL2 entry points are non-variadic so this is rarely hit.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
namespace arm64emu {
// Forward-declarations (full types defined in their own headers).
class Memory;
class CPU;
// Forward-declare the pimpl.
struct GraphicThunkImpl;
class GraphicThunk {
public:
    GraphicThunk();
    ~GraphicThunk();
    // Non-copyable, non-movable (owns guest-side trampoline page +
    // host-side GL/EGL/SDL2 state).
    GraphicThunk(const GraphicThunk&) = delete;
    GraphicThunk& operator=(const GraphicThunk&) = delete;
    // Whether thunking is enabled (BIFROST_THUNK_GRAPHICS=1 env var).
    // When false, resolve() always returns 0 and dispatch() is a no-op.
    bool enabled() const;
    // ── Lifecycle ────────────────────────────────────────────────────
    // Initialize the thunk: allocate the guest trampoline page and
    // register the known GL/EGL/SDL2 entry points. Idempotent —
    // subsequent calls are no-ops. Must be called before resolve().
    // Returns true on success, false on failure (e.g., out of memory).
    //
    // The Memory reference is stored internally; the caller must ensure
    // it outlives the thunk (typically both are owned by the Emulator).
    bool init(Memory& mem);
    // ── Symbol resolution ────────────────────────────────────────────
    // Resolve a graphic API symbol from the guest's perspective.
    // `lib` is the library basename (e.g. "libGL.so.1").
    // `sym` is the symbol name (e.g. "glClear").
    // Returns a GUEST-CALLABLE trampoline address, or 0 if not thunked.
    //
    // The returned address points into the thunk's trampoline page in
    // guest memory. Calling it triggers the __NR_bifrost_thunk syscall
    // which dispatches to the host function.
    uint64_t resolve(const std::string& lib, const std::string& sym);
    // ── Per-library symbol enumeration ───────────────────────────────
    // Enumerate all registered symbols for `lib`. Calls `cb(name, addr)`
    // for each. Used by the dynamic linker to populate its symbol table
    // for synthetic graphic-library LoadedObjects.
    //
    // If `lib` is not a known graphic library, the callback is not
    // called. Returns the number of symbols enumerated.
    size_t enumerate_symbols(const std::string& lib,
                             const std::function<void(const std::string&,
                                                       uint64_t)>& cb) const;
    // ── Dispatch (called from syscall handler) ───────────────────────
    // Dispatch a thunk call. `symbol_id` is the value in x9 when the
    // trampoline trapped. Reads args from cpu.regs[0..7], calls the
    // host function, writes the return value to cpu.regs[0].
    //
    // Returns 0 on success, -errno on failure (unknown symbol_id,
    // dispatch error). The caller (syscall handler) writes the return
    // value to cpu.regs[0]; on success it's already set by dispatch().
    int64_t dispatch(CPU& cpu, uint32_t symbol_id);
    // ── Diagnostics ──────────────────────────────────────────────────
    size_t symbol_count() const;
    uint64_t trampoline_base() const;
    // The syscall number used by trampolines to trap into the host.
    // High enough to never collide with real Linux AArch64 syscalls
    // (which go up to ~451 as of kernel 6.x).
    static constexpr uint64_t SYSCALL_NUMBER = 0x1000;
    // Trampoline layout: 4 instructions × 4 bytes = 16 bytes.
    //   movz x9, #symbol_id       ; load symbol_id
    //   movz x8, #SYSCALL_NUMBER  ; load syscall number
    //   svc #0                    ; trap to host
    //   nop                       ; pad to 16 bytes (alignment)
    static constexpr uint64_t TRAMPOLINE_SIZE  = 16;
    static constexpr uint64_t MAX_SYMBOLS      = 4096;  // 64 KiB page / 16 B
    // v1.5.0.alpha: per-thunk ID base to avoid collisions.
    // Each thunk type gets a non-overlapping range of symbol_ids.
    // The dispatch handler checks the range to route to the correct
    // thunk without trying each one sequentially.
    //   GraphicThunk: 0x0000 - 0x0FFF  (4096 symbols)
    //   AudioThunk:   0x1000 - 0x1FFF  (4096 symbols)
    //   DisplayThunk: 0x2000 - 0x2FFF  (4096 symbols)
    static constexpr uint32_t ID_BASE_GRAPHICS = 0x0000;
    static constexpr uint32_t ID_BASE_AUDIO    = 0x1000;
    static constexpr uint32_t ID_BASE_DISPLAY  = 0x2000;
    static constexpr uint32_t ID_MASK          = 0x3000;  // range selector
private:
    std::unique_ptr<GraphicThunkImpl> impl_;
    // Per-library registration helpers (defined in thunk.cpp).
    // v1.5.0.alpha: added pointer_args bitmask (bit N = arg N
    // is a pointer needing guest→host translation).
    void register_function_(const std::string& lib,
                            const std::string& sym,
                            void* host_fn,
                            uint8_t pointer_args = 0);
    void* resolve_gl_(const std::string& sym);
    void* resolve_egl_(const std::string& sym);
    void* resolve_sdl_(const std::string& sym);
    // Write a trampoline at the given guest address for the given sym_id.
    void write_trampoline_(Memory& mem, uint64_t addr, uint32_t sym_id);
    // Register the known GL/EGL/SDL2 entry points. Called once by init().
    void register_known_symbols_();
};
} // namespace arm64emu
