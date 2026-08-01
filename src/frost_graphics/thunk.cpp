// frost_graphics/thunk.cpp — graphic API thunking (v1.4.5-alpha).
//
// v1.5.0.alpha: also hosts the FrostGraphics::audio_thunk() and
// FrostGraphics::display_thunk() lazy-creator methods (the AudioThunk
// and DisplayThunk implementations live in their own .cpp files).
//
// ── Overview ──────────────────────────────────────────────────────────
// bifrost-emu runs AArch64 guests on x86-64 hosts. A guest that uses
// OpenGL/EGL/SDL2 normally links against the AArch64 versions of those
// libraries, which can't run on the x86-64 host. The thunk solves this
// by:
//
//   1. Maintaining a registry of (library, symbol) → (host_fn_ptr,
//      guest_trampoline_addr).
//   2. Each registered symbol gets a 16-byte guest trampoline that
//      traps to the host via the __NR_bifrost_thunk syscall.
//   3. The host's syscall dispatcher calls GraphicThunk::dispatch(),
//      which reads the symbol_id from x9, looks up the host function,
//      reads args from x0..x7, calls the host function, and writes
//      the return value to x0.
//
// This is the standard "thunk" pattern used by user-mode emulators
// (QEMU user, Rosetta 2, FEX-Emu) to forward guest calls to host
// libraries without recompiling them.
//
// ── What's supported ─────────────────────────────────────────────────
// EXPERIMENTAL. Only a small subset of GL/EGL/SDL2 entry points are
// thunked — enough to run trivial programs (clear the screen, draw a
// triangle, swap buffers). Real-world GL programs will hit unthunked
// entry points and get a stub that logs + returns 0.
//
// Supported libraries (partial):
//   - libGL.so: glClear, glClearColor, glBegin, glEnd, glVertex3f,
//     glColor3f, glFlush, glFinish, glGetError, glEnable, glDisable,
//     glViewport, glMatrixMode, glLoadIdentity, glOrtho
//   - libEGL.so: eglGetDisplay, eglInitialize, eglChooseConfig,
//     eglCreateContext, eglMakeCurrent, eglSwapBuffers, eglTerminate
//   - libSDL2.so: SDL_Init, SDL_Quit, SDL_CreateWindow,
//     SDL_GL_CreateContext, SDL_GL_SwapWindow, SDL_PollEvent,
//     SDL_GetWindowSurface, SDL_UpdateWindowSurface
//   - libGLESv2.so: glClear, glClearColor, glFlush, glGetError,
//     glEnable, glDisable, glViewport (subset of libGL)
//
// ── How to use ───────────────────────────────────────────────────────
// Set BIFROST_THUNK_GRAPHICS=1 in the environment. The emulator's
// dynamic linker (src/frontend/dynamic_linker.cpp) checks this env var
// and, when set, consults FrostGraphics::thunk() to resolve symbols
// from the graphic libraries before falling back to the host's
// dlopen/dlsym.
//
// Without BIFROST_THUNK_GRAPHICS=1, the thunk returns 0 for every
// lookup — the guest falls back to its own software rendering (or
// fails gracefully if it has no software path).
//
// ── Limitations ──────────────────────────────────────────────────────
//   - No state tracking: the thunk doesn't maintain GL state across
//     calls (e.g. glEnable(GL_DEPTH_TEST) is forwarded but not
//     recorded). This is fine for immediate-mode GL but breaks
//     retained-mode programs.
//   - No shader translation: GLSL shaders compiled by the guest are
//     passed to the host's glCompileShader verbatim. AArch64 and
//     x86-64 GLSL are identical (it's a text format), so this works.
//   - No EGL surface management: the thunk creates a single hidden
//     SDL2 window for EGL and ignores the guest's window/surface
//     requests. Real EGL programs that create multiple surfaces will
//     break.
//   - No GLESv1 (fixed-function) support: only GLESv2 (programmable).
//   - Pointer arguments are translated assuming the guest pointer is
//     in the emulator's address space (via Memory::host_addr()). This
//     works for heap/stack pointers but NOT for pointers the guest
//     got from mmap'd host resources (rare).
//   - Variadic functions (printf-style) are NOT supported — the thunk
//     passes exactly 8 args, ignoring variadic extras.
//
// See frost/thunk.hpp for the class definition and frost/graphics.hpp
// for the FrostGraphics::thunk() accessor.
#include "frost/graphics.hpp"
#include "frost/thunk.hpp"
#include "frost/audio_thunk.hpp"    // v1.5.0.alpha: AudioThunk full def
#include "frost/display_thunk.hpp"  // v1.5.0.alpha: DisplayThunk full def
#include "frost/gl_state.hpp"       // v1.5.1.alpha: GLStateTracker
#include "core/cpu.h"
#include "core/memory.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
// Host GL/EGL/SDL2 headers — optional. If not available, the thunk
// compiles but all entry points return stubs. The Makefile sets
// BIFROST_THUNK_HAVE_GL / BIFROST_THUNK_HAVE_EGL / BIFROST_THUNK_HAVE_SDL2
// when the host has the dev headers installed.
#if defined(BIFROST_THUNK_HAVE_SDL2)
#  include <SDL2/SDL.h>
#endif
#if defined(BIFROST_THUNK_HAVE_EGL)
#  include <EGL/egl.h>
#endif
#if defined(BIFROST_THUNK_HAVE_GL)
#  include <GL/gl.h>
#endif
namespace arm64emu {
// ── AArch64 instruction encodings for trampolines ─────────────────────
// We hand-encode the trampoline bytes rather than relying on an
// assembler. This keeps the thunk self-contained and lets us verify
// the encoding at compile time.
//
//   MOVZ Xd, #imm16, LSL 0  →  0xD2800000 | (imm16 << 5) | Xd
//   SVC #0                   →  0xD4000001
//   RET                      →  0xD65F03C0  (return to LR / x30)
namespace trampoline_enc {
    constexpr uint32_t MOVZ_Xd_IMM16(int Xd, uint16_t imm16) {
        return 0xD2800000u | (static_cast<uint32_t>(imm16) << 5)
                            | (static_cast<uint32_t>(Xd) & 0x1Fu);
    }
    constexpr uint32_t SVC_0   = 0xD4000001u;
    constexpr uint32_t RET     = 0xD65F03C0u;
}
// ── GraphicThunkImpl — the real implementation (pimpl) ────────────────
// The GraphicThunk class in frost/thunk.hpp exposes only void* opaque
// members to keep the header free of GL/EGL/SDL2 includes. The real
// state lives here.
struct SymbolEntry {
    std::string name;       // e.g. "glClear"
    void*       host_fn;    // host function pointer (or null if stub)
    uint64_t    guest_addr; // trampoline address in guest memory
    uint32_t    symbol_id;  // small int (0..MAX_SYMBOLS-1)
    // Bit N set ⇒ arg N is a guest pointer needing translation.
    // Covers args 0..11 (8 GPRs + up to 4 stack slots).
    uint16_t    pointer_args = 0;
    // Extra args beyond x0..x7, read from the guest stack at SP.
    uint8_t     n_stack = 0;
    // If >0, the first n_float args are IEEE-754 binary32 values in
    // v0..v{n-1} (AAPCS64 FP ABI), not in x0..x7.
    uint8_t     n_float = 0;
    // flags: bit0 = host returns const char* → copy into guest string cache
    //        bit1 = glShaderSource nested-pointer marshalling
    //        bit2 = mixed int+float ABI (n_stack = #ints in x0.., n_float in v0..)
    //        bit3 = GetProcAddress: resolve guest name → trampoline addr
    uint8_t     flags = 0;
};
static constexpr uint8_t THUNK_RET_STRING    = 1u << 0;
static constexpr uint8_t THUNK_SHADER_SOURCE = 1u << 1;
static constexpr uint8_t THUNK_MIXED_FP      = 1u << 2;
static constexpr uint8_t THUNK_GET_PROC      = 1u << 3;
struct GraphicThunkImpl {
    bool   enabled = false;
    Memory* mem    = nullptr;
    bool   initialized = false;
    // The trampoline page: a single 64 KiB region of guest memory.
    uint64_t trampoline_base = 0;
    static constexpr uint64_t TRAMPOLINE_PAGE_SIZE =
        GraphicThunk::TRAMPOLINE_SIZE * GraphicThunk::MAX_SYMBOLS;  // 64 KiB
    // Guest-visible scratch page for host→guest string returns
    // (glGetString, SDL_GetError, …). Ring-allocated.
    uint64_t string_cache_base = 0;
    static constexpr uint64_t STRING_CACHE_SIZE = 4096;
    uint32_t string_cache_off = 0;
    // Registry: (library, symbol_name) → SymbolEntry.
    // We use a flat vector per-library for cache-friendly enumeration
    // (the dynamic linker iterates all symbols when populating its
    // global symbol table for a synthetic LoadedObject).
    struct LibTable {
        std::string lib;
        std::vector<SymbolEntry> entries;
    };
    std::vector<LibTable> libs_;
    // Reverse lookup: symbol_id → (lib index, entry index).
    std::vector<std::pair<uint32_t, uint32_t>> id_to_idx_;
    // SDL2-side state (only when BIFROST_THUNK_HAVE_SDL2 is defined).
#if defined(BIFROST_THUNK_HAVE_SDL2)
    bool          sdl_init_done = false;
    SDL_Window*   sdl_window    = nullptr;
    SDL_GLContext sdl_gl_ctx    = nullptr;
#endif
    // EGL-side state (only when BIFROST_THUNK_HAVE_EGL is defined).
#if defined(BIFROST_THUNK_HAVE_EGL)
    EGLDisplay    egl_display   = nullptr;
    EGLContext    egl_context   = nullptr;
#endif
    // Mutex protecting the registry (init() registers symbols, resolve()
    // reads them; concurrent calls from multiple guest threads must be
    // safe). The dispatch() path is lock-free after init() — it only
    // reads id_to_idx_, which is set once and never resized.
    std::mutex mu;
    // v1.5.1.alpha: GL state tracker for consistent query results.
    std::unique_ptr<GLStateTracker> gl_state_tracker_;
    // Find or create the LibTable for `lib`. Returns pointer into libs_.
    LibTable* find_or_create_lib_(const std::string& lib) {
        for (auto& l : libs_) {
            if (l.lib == lib) return &l;
        }
        libs_.push_back({lib, {}});
        return &libs_.back();
    }
    LibTable* find_lib_(const std::string& lib) {
        for (auto& l : libs_) {
            if (l.lib == lib) return &l;
        }
        return nullptr;
    }
    uint64_t cache_host_string_(const char* host_str) {
        if (!mem || !string_cache_base || !host_str) return 0;
        size_t len = std::strlen(host_str) + 1;
        if (len > STRING_CACHE_SIZE) len = STRING_CACHE_SIZE;
        if (string_cache_off + len > STRING_CACHE_SIZE)
            string_cache_off = 0;
        uint64_t guest = string_cache_base + string_cache_off;
        mem->write(guest, host_str, len);
        string_cache_off = static_cast<uint32_t>(
            (string_cache_off + len + 7u) & ~7u);
        return guest;
    }
};
// ── GraphicThunk method implementations ───────────────────────────────
GraphicThunk::GraphicThunk() {
    impl_ = std::make_unique<GraphicThunkImpl>();
    // v1.5.0.alpha: thunking is now ENABLED BY DEFAULT.
    // Previously required BIFROST_THUNK_GRAPHICS=1. Now we always try
    // to thunk graphic calls; if the host doesn't have GL/EGL/SDL2
    // libraries, the symbols resolve to stubs that return 0 (safe
    // fallback). Set BIFROST_NO_THUNK_GRAPHICS=1 to disable.
    //
    // This makes graphics "just work" for programs that use GL/EGL/SDL2
    // when the host has the libraries, and degrade gracefully (software
    // rendering or no-op) when it doesn't.
    const char* disable = getenv("BIFROST_NO_THUNK_GRAPHICS");
    impl_->enabled = !(disable && disable[0] != '0');
    if (impl_->enabled) {
        // Only print if verbose or trace — don't clutter default output
        if (getenv("BIFROST_THUNK_TRACE") || getenv("BIFROST_VERBOSE")) {
            fprintf(stderr, "[thunk] graphic API thunking enabled "
                    "(GL/EGL/SDL2 → host, with emulation fallback)\n");
        }
    }
}
GraphicThunk::~GraphicThunk() {
#if defined(BIFROST_THUNK_HAVE_SDL2)
    if (impl_ && impl_->sdl_window) {
        SDL_DestroyWindow(impl_->sdl_window);
    }
    if (impl_ && impl_->sdl_init_done) {
        SDL_Quit();
    }
#endif
}
bool GraphicThunk::enabled() const { return impl_ && impl_->enabled; }
// ── init() — allocate trampoline page, register known symbols ─────────
bool GraphicThunk::init(Memory& mem) {
    if (!impl_->enabled) return false;
    if (impl_->initialized) return true;
    std::lock_guard<std::mutex> g(impl_->mu);
    impl_->mem = &mem;
    // Allocate a 64 KiB page for trampolines. mmap_alloc returns a
    // fresh, zeroed region. The page is mapped RWX in guest memory
    // (the JIT will execute the trampolines directly).
    impl_->trampoline_base = mem.mmap_alloc(GraphicThunkImpl::TRAMPOLINE_PAGE_SIZE);
    if (impl_->trampoline_base == 0) {
        fprintf(stderr, "[thunk] init: failed to allocate trampoline page\n");
        return false;
    }
    impl_->string_cache_base = mem.mmap_alloc(GraphicThunkImpl::STRING_CACHE_SIZE);
    if (impl_->string_cache_base == 0) {
        fprintf(stderr, "[thunk] init: failed to allocate string cache\n");
        return false;
    }
    impl_->string_cache_off = 0;
    // Register the known GL/EGL/SDL2 entry points.
    register_known_symbols_();
    impl_->initialized = true;
    // v1.5.1.alpha: initialize GL state tracker.
    impl_->gl_state_tracker_ = std::make_unique<GLStateTracker>();
    if (getenv("BIFROST_THUNK_TRACE")) {
        fprintf(stderr, "[thunk] init: %zu symbols registered, "
                "trampoline_base=0x%llx\n",
                impl_->id_to_idx_.size(),
                static_cast<unsigned long long>(impl_->trampoline_base));
    }
    return true;
}
// ── register_function_ — add a (lib, sym) → host_fn mapping ───────────
// Allocates a symbol_id, writes the trampoline into guest memory, and
// stores the entry. Thread-safe (called from init() under lock).
void GraphicThunk::register_function_(const std::string& lib,
                                       const std::string& sym,
                                       void* host_fn,
                                       uint16_t pointer_args,
                                       uint8_t n_stack,
                                       uint8_t n_float,
                                       uint8_t flags) {
    auto* lt = impl_->find_or_create_lib_(lib);
    // Check if already registered (idempotent).
    for (const auto& e : lt->entries) {
        if (e.name == sym) return;
    }
    // v1.5.0.alpha: symbol_id includes ID_BASE_GRAPHICS to
    // avoid collisions with AudioThunk/DisplayThunk IDs.
    uint32_t local_id = static_cast<uint32_t>(impl_->id_to_idx_.size());
    if (local_id >= GraphicThunk::MAX_SYMBOLS) {
        fprintf(stderr, "[thunk] register: symbol table full (%zu)\n",
                impl_->id_to_idx_.size());
        return;
    }
    uint32_t sym_id = GraphicThunk::ID_BASE_GRAPHICS + local_id;
    uint64_t addr = impl_->trampoline_base + local_id * GraphicThunk::TRAMPOLINE_SIZE;
    write_trampoline_(*impl_->mem, addr, sym_id);
    lt->entries.push_back({sym, host_fn, addr, sym_id, pointer_args,
                           n_stack, n_float, flags});
    impl_->id_to_idx_.push_back({
        static_cast<uint32_t>(std::distance(impl_->libs_.data(), lt)),
        static_cast<uint32_t>(lt->entries.size() - 1)
    });
    if (getenv("BIFROST_THUNK_TRACE")) {
        fprintf(stderr, "[thunk] registered %s:%s -> 0x%llx "
                "(id=%u ptrs=0x%x stack=%u fp=%u flags=0x%x)\n",
                lib.c_str(), sym.c_str(),
                static_cast<unsigned long long>(addr), sym_id,
                pointer_args, n_stack, n_float, flags);
    }
}
// ── write_trampoline_ — emit 16-byte AArch64 trampoline ────────────────
// Layout (little-endian, each instruction is 4 bytes):
//   movz x9, #sym_id        ; load symbol_id into x9
//   movz x8, #SYSCALL_NUM   ; load __NR_bifrost_thunk into x8
//   svc #0                  ; trap to host
//   nop                     ; pad to 16 bytes (alignment)
void GraphicThunk::write_trampoline_(Memory& mem, uint64_t addr, uint32_t sym_id) {
    // Truncation: sym_id is bounded by MAX_SYMBOLS (4096), so it always
    // fits in 16 bits.
    uint32_t buf[4];
    buf[0] = trampoline_enc::MOVZ_Xd_IMM16(9, static_cast<uint16_t>(sym_id));
    buf[1] = trampoline_enc::MOVZ_Xd_IMM16(8,
                static_cast<uint16_t>(GraphicThunk::SYSCALL_NUMBER));
    buf[2] = trampoline_enc::SVC_0;
    buf[3] = trampoline_enc::RET;  // return to guest caller (x30)
    mem.write(addr, buf, sizeof(buf));
}
// ── resolve() — look up a (lib, sym) and return guest trampoline addr ─
uint64_t GraphicThunk::resolve(const std::string& lib, const std::string& sym) {
    if (!impl_ || !impl_->enabled || !impl_->initialized) return 0;
    std::lock_guard<std::mutex> g(impl_->mu);
    auto* lt = impl_->find_lib_(lib);
    if (!lt) return 0;
    for (const auto& e : lt->entries) {
        if (e.name == sym) return e.guest_addr;
    }
    return 0;
}
// ── enumerate_symbols() — list all (name, addr) pairs for `lib` ────────
size_t GraphicThunk::enumerate_symbols(const std::string& lib,
    const std::function<void(const std::string&, uint64_t)>& cb) const {
    if (!impl_ || !impl_->enabled || !impl_->initialized) return 0;
    std::lock_guard<std::mutex> g(impl_->mu);
    auto* lt = impl_->find_lib_(lib);
    if (!lt) return 0;
    for (const auto& e : lt->entries) {
        cb(e.name, e.guest_addr);
    }
    return lt->entries.size();
}
// ── dispatch() — call the host function for a given symbol_id ──────────
// Called from the syscall handler when a trampoline traps. Reads args
// from cpu.regs[0..7], calls the host function, writes the return
// value to cpu.regs[0].
//
// IMPORTANT: this function is on the hot path for thunked calls. Keep
// it lock-free — the id_to_idx_ vector is set once during init() and
// never resized afterwards, so we can read it without the mutex.
int64_t GraphicThunk::dispatch(CPU& cpu, uint32_t symbol_id) {
    if (!impl_ || !impl_->enabled || !impl_->initialized) {
        return -ENOSYS;
    }
    if ((symbol_id & GraphicThunk::ID_MASK) != GraphicThunk::ID_BASE_GRAPHICS) {
        return -ENOENT;
    }
    uint32_t local_id = symbol_id - GraphicThunk::ID_BASE_GRAPHICS;
    if (local_id >= impl_->id_to_idx_.size()) {
        if (getenv("BIFROST_THUNK_TRACE")) {
            fprintf(stderr, "[thunk] dispatch: unknown symbol_id=%u\n", symbol_id);
        }
        return -ENOENT;
    }
    auto [lib_idx, ent_idx] = impl_->id_to_idx_[local_id];
    const auto& entry = impl_->libs_[lib_idx].entries[ent_idx];
    if (!entry.host_fn) {
        if (getenv("BIFROST_THUNK_TRACE")) {
            fprintf(stderr, "[thunk] dispatch: %s (stub, returns 0)\n",
                    entry.name.c_str());
        }
        cpu.regs[0] = 0;
        return 0;
    }

    // ── Float-only AAPCS64 path (glClearColor, glVertex3f, …) ────────
    if (entry.n_float > 0 && !(entry.flags & THUNK_MIXED_FP)
        && !(entry.flags & THUNK_GET_PROC)) {
        float fv[8] = {0};
        for (uint8_t i = 0; i < entry.n_float && i < 8; i++) {
            std::memcpy(&fv[i], &cpu.v_lo[i], sizeof(float));
        }
        uint64_t local_args[12] = {0};
        for (int i = 0; i < 8; i++) local_args[i] = cpu.regs[i];
        if (getenv("BIFROST_THUNK_TRACE")) {
            fprintf(stderr, "[thunk] dispatch: %s (fp×%u) f0=%g f1=%g f2=%g f3=%g\n",
                    entry.name.c_str(), entry.n_float,
                    fv[0], fv[1], fv[2], fv[3]);
        }
        switch (entry.n_float) {
        case 1: {
            using Fn = void (*)(float);
            reinterpret_cast<Fn>(entry.host_fn)(fv[0]);
            break;
        }
        case 2: {
            using Fn = void (*)(float, float);
            reinterpret_cast<Fn>(entry.host_fn)(fv[0], fv[1]);
            break;
        }
        case 3: {
            using Fn = void (*)(float, float, float);
            reinterpret_cast<Fn>(entry.host_fn)(fv[0], fv[1], fv[2]);
            break;
        }
        default: {
            using Fn = void (*)(float, float, float, float);
            reinterpret_cast<Fn>(entry.host_fn)(fv[0], fv[1], fv[2], fv[3]);
            break;
        }
        }
        cpu.regs[0] = 0;
        if (impl_->gl_state_tracker_) {
            impl_->gl_state_tracker_->track_state_change(entry.name, local_args, fv, entry.n_float);
        }
        return 0;
    }

    // ── Mixed int + float (glUniform*f, glTexParameterf, …) ─────────
    // n_stack holds the integer arity (x0..); n_float holds float arity
    // (v0..). Used only when THUNK_MIXED_FP is set.
    if (entry.flags & THUNK_MIXED_FP) {
        uint64_t iv[4] = {0};
        float fv[4] = {0};
        uint8_t ni = entry.n_stack;
        if (ni > 4) ni = 4;
        for (uint8_t i = 0; i < ni; i++) iv[i] = cpu.regs[i];
        for (uint8_t i = 0; i < entry.n_float && i < 4; i++) {
            std::memcpy(&fv[i], &cpu.v_lo[i], sizeof(float));
        }
        if (getenv("BIFROST_THUNK_TRACE")) {
            fprintf(stderr, "[thunk] dispatch: %s (mixed int×%u fp×%u) "
                    "i0=%lld f0=%g\n",
                    entry.name.c_str(), ni, entry.n_float,
                    static_cast<long long>(iv[0]), fv[0]);
        }
        if (ni == 1 && entry.n_float == 1) {
            using Fn = void (*)(int32_t, float);
            reinterpret_cast<Fn>(entry.host_fn)(
                static_cast<int32_t>(iv[0]), fv[0]);
        } else if (ni == 1 && entry.n_float == 2) {
            using Fn = void (*)(int32_t, float, float);
            reinterpret_cast<Fn>(entry.host_fn)(
                static_cast<int32_t>(iv[0]), fv[0], fv[1]);
        } else if (ni == 1 && entry.n_float == 3) {
            using Fn = void (*)(int32_t, float, float, float);
            reinterpret_cast<Fn>(entry.host_fn)(
                static_cast<int32_t>(iv[0]), fv[0], fv[1], fv[2]);
        } else if (ni == 1 && entry.n_float >= 4) {
            using Fn = void (*)(int32_t, float, float, float, float);
            reinterpret_cast<Fn>(entry.host_fn)(
                static_cast<int32_t>(iv[0]), fv[0], fv[1], fv[2], fv[3]);
        } else if (ni == 2 && entry.n_float == 1) {
            using Fn = void (*)(uint32_t, uint32_t, float);
            reinterpret_cast<Fn>(entry.host_fn)(
                static_cast<uint32_t>(iv[0]),
                static_cast<uint32_t>(iv[1]), fv[0]);
        } else {
            // Unsupported mixed shape — no-op rather than corrupt.
            if (getenv("BIFROST_THUNK_TRACE")) {
                fprintf(stderr, "[thunk] mixed FP shape unsupported for %s\n",
                        entry.name.c_str());
            }
        }
        cpu.regs[0] = 0;
        if (impl_->gl_state_tracker_) {
            uint64_t mixed_args[12] = {0};
            mixed_args[0] = iv[0];
            impl_->gl_state_tracker_->track_state_change(entry.name, mixed_args, fv, entry.n_float);
        }
        return 0;
    }

    // ── Integer/pointer path with optional stack args ────────────────
    constexpr int kMaxArgs = 12;
    uint64_t args[kMaxArgs] = {0};
    for (int i = 0; i < 8; i++) args[i] = cpu.regs[i];
    // AAPCS64: args 8+ live on the guest stack at SP, 8-byte slots.
    if (entry.n_stack && impl_->mem) {
        for (uint8_t i = 0; i < entry.n_stack && (8 + i) < kMaxArgs; i++) {
            uint64_t slot = cpu.sp + static_cast<uint64_t>(i) * 8ull;
            impl_->mem->read(slot, &args[8 + i], sizeof(uint64_t));
        }
    }

    // GetProcAddress(name): return guest trampoline for a registered
    // GL/EGL/SDL symbol, or 0 if unknown. Games resolve most GL via this.
    if (entry.flags & THUNK_GET_PROC) {
        char namebuf[256];
        const char* name = nullptr;
        if (args[0] && impl_->mem) {
            uint8_t* hp = impl_->mem->guest_to_host_ptr(args[0]);
            if (hp) {
                name = reinterpret_cast<const char*>(hp);
            } else {
                size_t n = 0;
                for (; n + 1 < sizeof(namebuf); n++) {
                    uint8_t c = 0;
                    try { impl_->mem->read(args[0] + n, &c, 1); }
                    catch (...) { break; }
                    namebuf[n] = static_cast<char>(c);
                    if (c == 0) break;
                }
                namebuf[sizeof(namebuf) - 1] = 0;
                name = namebuf;
            }
        }
        uint64_t found = 0;
        if (name && name[0]) {
            for (const auto& lib : impl_->libs_) {
                for (const auto& e : lib.entries) {
                    if (e.name == name) {
                        found = e.guest_addr;
                        break;
                    }
                }
                if (found) break;
            }
        }
        if (getenv("BIFROST_THUNK_TRACE")) {
            fprintf(stderr, "[thunk] GetProcAddress('%s') → 0x%llx\n",
                    name ? name : "(null)",
                    static_cast<unsigned long long>(found));
        }
        cpu.regs[0] = found;
        return 0;
    }

    auto translate_ptr = [&](uint64_t& a, int idx,
                             std::vector<uint8_t>* bounce,
                             uint64_t* guest_orig,
                             bool* need_wb) {
        if (a == 0 || !impl_->mem) return;
        uint8_t* host_ptr = impl_->mem->guest_to_host_ptr(a);
        if (host_ptr) {
            a = reinterpret_cast<uint64_t>(host_ptr);
            return;
        }
        // High-stack / sparse-page pointer: bounce through a host buffer.
        // Default 64 KiB covers modest textures/VBO uploads; for
        // glBufferData use the size arg when it fits.
        size_t kBounce = 65536;
        if (entry.name == "glBufferData" && idx == 2) {
            uint64_t sz = args[1];
            if (sz > 0 && sz < (16ull << 20)) kBounce = static_cast<size_t>(sz);
        } else if (entry.name == "glBufferSubData" && idx == 3) {
            uint64_t sz = args[2];
            if (sz > 0 && sz < (16ull << 20)) kBounce = static_cast<size_t>(sz);
        }
        bounce->resize(kBounce);
        try {
            impl_->mem->read(a, bounce->data(), kBounce);
        } catch (...) {
            bounce->assign(kBounce, 0);
        }
        *guest_orig = a;
        *need_wb = true;
        a = reinterpret_cast<uint64_t>(bounce->data());
        (void)idx;
    };

    std::vector<uint8_t> bounce_bufs[kMaxArgs];
    uint64_t bounce_guest[kMaxArgs] = {0};
    bool bounce_wb[kMaxArgs] = {false};

    if (entry.pointer_args && impl_->mem) {
        for (int i = 0; i < kMaxArgs; i++) {
            if (entry.pointer_args & (1u << i)) {
                translate_ptr(args[i], i, &bounce_bufs[i],
                              &bounce_guest[i], &bounce_wb[i]);
            }
        }
    }

    // glShaderSource(shader, count, const char* const* strings, const int* len)
    if (entry.flags & THUNK_SHADER_SOURCE) {
        int count = static_cast<int>(args[1]);
        if (count < 0) count = 0;
        if (count > 64) count = 64;
        const char* host_strs[64];
        // args[2] already translated to host pointer to guest pointer array
        const uint64_t* guest_arr = reinterpret_cast<const uint64_t*>(args[2]);
        for (int i = 0; i < count; i++) {
            uint64_t gp = guest_arr ? guest_arr[i] : 0;
            uint8_t* hp = (gp && impl_->mem) ? impl_->mem->guest_to_host_ptr(gp)
                                             : nullptr;
            host_strs[i] = hp ? reinterpret_cast<const char*>(hp) : "";
        }
        using Fn = void (*)(uint64_t, int, const char* const*, const int*);
        reinterpret_cast<Fn>(entry.host_fn)(
            args[0], count, host_strs,
            args[3] ? reinterpret_cast<const int*>(args[3]) : nullptr);
        cpu.regs[0] = 0;
        return 0;
    }

    if (getenv("BIFROST_THUNK_TRACE")) {
        fprintf(stderr, "[thunk] dispatch: %s (host_fn=%p) "
                "a0=0x%llx a1=0x%llx a2=0x%llx a3=0x%llx "
                "a8=0x%llx ptrs=0x%x stack=%u\n",
                entry.name.c_str(), entry.host_fn,
                static_cast<unsigned long long>(args[0]),
                static_cast<unsigned long long>(args[1]),
                static_cast<unsigned long long>(args[2]),
                static_cast<unsigned long long>(args[3]),
                static_cast<unsigned long long>(args[8]),
                entry.pointer_args, entry.n_stack);
    }

    // v1.5.1.alpha: GL state query interception — after pointer translation
    // so that pointer args in queries (glGetIntegerv, etc.) point to host
    // memory. Only intercept actual query functions.
    bool is_gl_query = (entry.name == "glIsEnabled" ||
                        entry.name == "glGetBooleanv" ||
                        entry.name == "glGetIntegerv" ||
                        entry.name == "glGetFloatv");
    bool handled_by_tracker = false;
    if (is_gl_query && impl_->gl_state_tracker_) {
        // Pass the post-translation `args` so the tracker writes query
        // results into the host buffer (direct-window alias or bounce)
        // that dispatch() will write back to guest memory.
        handled_by_tracker = impl_->gl_state_tracker_->try_handle_query(entry.name, args, cpu);
    }
    if (handled_by_tracker) {
        // Skip host call, but still write back any bounced pointer args
        // so the guest sees the query result.
        if (impl_->mem) {
            for (int i = 0; i < kMaxArgs; i++) {
                if (bounce_wb[i] && bounce_guest[i]) {
                    impl_->mem->write(bounce_guest[i], bounce_bufs[i].data(),
                                      bounce_bufs[i].size());
                }
            }
        }
        if (getenv("BIFROST_THUNK_TRACE")) {
            fprintf(stderr, "[thunk] dispatch: %s handled by GL state tracker\n",
                    entry.name.c_str());
        }
        return 0;
    }

    uint64_t ret = 0;
    if (entry.n_stack >= 1) {
        using Fn9 = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t,
                                  uint64_t, uint64_t, uint64_t, uint64_t,
                                  uint64_t);
        ret = reinterpret_cast<Fn9>(entry.host_fn)(
            args[0], args[1], args[2], args[3],
            args[4], args[5], args[6], args[7], args[8]);
    } else {
        using Fn8 = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t,
                                  uint64_t, uint64_t, uint64_t, uint64_t);
        ret = reinterpret_cast<Fn8>(entry.host_fn)(
            args[0], args[1], args[2], args[3],
            args[4], args[5], args[6], args[7]);
    }

    // Write bounced pointer args back into guest memory.
    if (impl_->mem) {
        for (int i = 0; i < kMaxArgs; i++) {
            if (bounce_wb[i] && bounce_guest[i]) {
                impl_->mem->write(bounce_guest[i], bounce_bufs[i].data(),
                                  bounce_bufs[i].size());
            }
        }
    }

    if (entry.flags & THUNK_RET_STRING) {
        ret = impl_->cache_host_string_(reinterpret_cast<const char*>(ret));
    }
    cpu.regs[0] = ret;
    if (impl_->gl_state_tracker_) {
        impl_->gl_state_tracker_->track_state_change(entry.name, args, nullptr, 0);
    }
    return 0;
}
// ── Diagnostics ────────────────────────────────────────────────────────
size_t GraphicThunk::symbol_count() const {
    if (!impl_) return 0;
    return impl_->id_to_idx_.size();
}
uint64_t GraphicThunk::trampoline_base() const {
    if (!impl_) return 0;
    return impl_->trampoline_base;
}
// ── register_known_symbols_ — populate the registry ────────────────────
// Called once by init(). Each entry maps a (library, symbol) pair to
// the host function pointer (when the host has the dev headers) or to
// a null stub (when the host doesn't have the headers, but we still
// want the symbol to resolve so the guest doesn't fail at dlsym time).
void GraphicThunk::register_known_symbols_() {
    // ── libGL.so / libGL.so.1 ──────────────────────────────────────
    const char* gl_libs[] = {"libGL.so", "libGL.so.1"};
    // v1.5.0.alpha: REG_GL_PTR marks which args are pointers.
    // The pointer_args bitmask is passed to register_function_ so the
    // dispatcher can translate guest pointers to host pointers.
    // Bit N (0-indexed) set = arg N is a pointer.
    //   glVertex3fv(v)           → arg 0 is pointer  → 0x01
    //   glVertexPointer(size, type, stride, ptr) → arg 3 is pointer → 0x08
    //   glTexImage2D(target, level, internalformat, w, h, border, format, type, data)
    //     → arg 8 is pointer, but we only support 8 args (0-7), so data
    //       (arg 8) can't be marked. This is a known limitation.
#if defined(BIFROST_THUNK_HAVE_GL)
    #define REG_GL(name) do { \
        void* p = dlsym(RTLD_DEFAULT, #name); \
        for (const char* L : gl_libs) register_function_(L, #name, p); \
    } while(0)
    #define REG_GL_PTR(name, ptrs) do { \
        void* p = dlsym(RTLD_DEFAULT, #name); \
        for (const char* L : gl_libs) register_function_(L, #name, p, ptrs); \
    } while(0)
    #define REG_GL_FP(name, nf) do { \
        void* p = dlsym(RTLD_DEFAULT, #name); \
        for (const char* L : gl_libs) register_function_(L, #name, p, 0, 0, nf); \
    } while(0)
    #define REG_GL_EX(name, ptrs, nstack, nfloat, fl) do { \
        void* p = dlsym(RTLD_DEFAULT, #name); \
        for (const char* L : gl_libs) \
            register_function_(L, #name, p, ptrs, nstack, nfloat, fl); \
    } while(0)
#else
    #define REG_GL(name) do { \
        for (const char* L : gl_libs) register_function_(L, #name, nullptr); \
    } while(0)
    #define REG_GL_PTR(name, ptrs) do { \
        for (const char* L : gl_libs) register_function_(L, #name, nullptr, ptrs); \
    } while(0)
    #define REG_GL_FP(name, nf) do { \
        for (const char* L : gl_libs) register_function_(L, #name, nullptr, 0, 0, nf); \
    } while(0)
    #define REG_GL_EX(name, ptrs, nstack, nfloat, fl) do { \
        for (const char* L : gl_libs) \
            register_function_(L, #name, nullptr, ptrs, nstack, nfloat, fl); \
    } while(0)
#endif
    REG_GL(glClear);
    REG_GL_FP(glClearColor, 4);
    REG_GL(glBegin);
    REG_GL(glEnd);
    REG_GL_FP(glVertex3f, 3);
    REG_GL_FP(glVertex2f, 2);
    REG_GL_PTR(glVertex3fv, 0x01);      // arg 0: const GLfloat *v
    REG_GL_FP(glColor3f, 3);
    REG_GL_FP(glColor4f, 4);
    REG_GL(glColor3ub);
    REG_GL(glFlush);
    REG_GL(glFinish);
    REG_GL(glGetError);
    REG_GL(glEnable);
    REG_GL(glDisable);
    REG_GL(glIsEnabled);
    REG_GL(glViewport);
    REG_GL(glMatrixMode);
    REG_GL(glLoadIdentity);
    REG_GL(glOrtho);
    REG_GL(glPushMatrix);
    REG_GL(glPopMatrix);
    REG_GL_FP(glRotatef, 4);
    REG_GL_FP(glTranslatef, 3);
    REG_GL_FP(glScalef, 3);
    REG_GL_PTR(glGenTextures, 0x02);    // arg 1: GLuint *textures
    REG_GL(glBindTexture);
    REG_GL(glTexParameteri);
    // glTexImage2D: 9th arg (data) on guest stack — translate bit 8.
    REG_GL_EX(glTexImage2D, (1u << 8), 1, 0, 0);
    REG_GL_EX(glTexSubImage2D, (1u << 8), 1, 0, 0);
    REG_GL(glEnableClientState);
    REG_GL(glDisableClientState);
    REG_GL_PTR(glVertexPointer, 0x08);  // arg 3: const void *pointer
    REG_GL_PTR(glColorPointer, 0x08);   // arg 3: const void *pointer
    REG_GL_PTR(glTexCoordPointer, 0x08);// arg 3: const void *pointer
    REG_GL(glDrawArrays);
    REG_GL_PTR(glDrawElements, 0x10);   // arg 4: const void *indices
    REG_GL_EX(glGetString, 0, 0, 0, THUNK_RET_STRING);
    REG_GL_PTR(glGetIntegerv, 0x02);    // arg 1: GLint *params
    REG_GL(glGenLists);
    REG_GL(glCallList);
    REG_GL(glNewList);
    REG_GL(glEndList);
    REG_GL(glDeleteLists);
    REG_GL(glDepthFunc);
    REG_GL(glDepthMask);
    REG_GL(glColorMask);
    REG_GL(glStencilFunc);
    REG_GL(glStencilOp);
    REG_GL(glStencilMask);
    REG_GL(glStencilFuncSeparate);
    REG_GL(glStencilOpSeparate);
    REG_GL(glStencilMaskSeparate);
    REG_GL(glBlendFunc);
    REG_GL(glBlendFuncSeparate);
    REG_GL(glBlendEquation);
    REG_GL(glBlendEquationSeparate);
    REG_GL(glHint);
    REG_GL(glPixelStorei);
    REG_GL_PTR(glReadPixels, 0x80);     // arg 7: void *pixels
    REG_GL(glDrawBuffer);
    REG_GL(glClearDepth);
    REG_GL(glClearStencil);
    REG_GL_FP(glPointSize, 1);
    REG_GL_FP(glLineWidth, 1);
    REG_GL(glFrontFace);
    REG_GL(glCullFace);
    REG_GL(glShadeModel);
    REG_GL_PTR(glLightfv, 0x04);        // arg 2: const GLfloat *params
    REG_GL_PTR(glMaterialfv, 0x04);     // arg 2: const GLfloat *params
    REG_GL(glNormal3f);
    REG_GL(glTexCoord2f);
    REG_GL(glActiveTexture);
    REG_GL(glClientActiveTexture);
    REG_GL(glMultiTexCoord2f);
    // Shaders.
    REG_GL_PTR(glCreateShader, 0x00);     // returns GLuint
    REG_GL_EX(glShaderSource, 0x0C, 0, 0, THUNK_SHADER_SOURCE);
    REG_GL(glCompileShader);
    REG_GL(glDeleteShader);
    REG_GL_PTR(glCreateProgram, 0x00);    // returns GLuint
    REG_GL(glAttachShader);
    REG_GL(glDetachShader);
    REG_GL(glLinkProgram);
    REG_GL(glUseProgram);
    REG_GL(glDeleteProgram);
    REG_GL_PTR(glGetShaderiv, 0x04);      // arg 2: GLint *params
    REG_GL_PTR(glGetProgramiv, 0x04);     // arg 2: GLint *params
    REG_GL_PTR(glGetShaderInfoLog, 0x08); // arg 3: GLchar *infoLog
    REG_GL_PTR(glGetProgramInfoLog, 0x08);// arg 3: GLchar *infoLog
    REG_GL_PTR(glGetAttribLocation, 0x02);// arg 1: const GLchar *name
    REG_GL_PTR(glGetUniformLocation, 0x02);// arg 1: const GLchar *name
    REG_GL_PTR(glBindAttribLocation, 0x04); // arg 2: const GLchar *name
    REG_GL(glUniform1i);
    // Mixed: location in x0, floats in v0.. (AAPCS64)
    REG_GL_EX(glUniform1f, 0, 1, 1, THUNK_MIXED_FP);
    REG_GL_EX(glUniform2f, 0, 1, 2, THUNK_MIXED_FP);
    REG_GL_EX(glUniform3f, 0, 1, 3, THUNK_MIXED_FP);
    REG_GL_EX(glUniform4f, 0, 1, 4, THUNK_MIXED_FP);
    REG_GL_PTR(glUniform1fv, 0x04);       // arg 2: const GLfloat *value
    REG_GL_PTR(glUniform2fv, 0x04);
    REG_GL_PTR(glUniform3fv, 0x04);
    REG_GL_PTR(glUniform4fv, 0x04);
    REG_GL_PTR(glUniformMatrix3fv, 0x20);
    REG_GL_PTR(glUniformMatrix4fv, 0x20); // arg 3: const GLfloat *value
    REG_GL(glEnableVertexAttribArray);
    REG_GL(glDisableVertexAttribArray);
    REG_GL_PTR(glVertexAttribPointer, 0x80); // arg 5: const void *pointer
    REG_GL_PTR(glGetFloatv, 0x02);
    REG_GL_PTR(glGetBooleanv, 0x02);
#if defined(BIFROST_THUNK_HAVE_GL)
    {
        void* p = dlsym(RTLD_DEFAULT, "glGetProcAddress");
        if (!p) p = dlsym(RTLD_DEFAULT, "glXGetProcAddress");
        if (!p) p = dlsym(RTLD_DEFAULT, "glXGetProcAddressARB");
        if (!p) p = reinterpret_cast<void*>(1);
        for (const char* L : gl_libs) {
            register_function_(L, "glGetProcAddress", p, 0x01, 0, 0,
                               THUNK_GET_PROC);
            register_function_(L, "glXGetProcAddress", p, 0x01, 0, 0,
                               THUNK_GET_PROC);
            register_function_(L, "glXGetProcAddressARB", p, 0x01, 0, 0,
                               THUNK_GET_PROC);
        }
    }
#else
    for (const char* L : gl_libs) {
        register_function_(L, "glGetProcAddress",
                           reinterpret_cast<void*>(1), 0x01, 0, 0,
                           THUNK_GET_PROC);
        register_function_(L, "glXGetProcAddress",
                           reinterpret_cast<void*>(1), 0x01, 0, 0,
                           THUNK_GET_PROC);
    }
#endif
    // VBOs.
    REG_GL_PTR(glGenBuffers, 0x02);       // arg 1: GLuint *buffers
    REG_GL_PTR(glDeleteBuffers, 0x02);    // arg 1: const GLuint *buffers
    REG_GL(glBindBuffer);
    REG_GL_PTR(glBufferData, 0x08);       // arg 3: const void *data
    REG_GL_PTR(glBufferSubData, 0x08);    // arg 3: const void *data
    // FBOs.
    REG_GL_PTR(glGenFramebuffers, 0x02);
    REG_GL_PTR(glDeleteFramebuffers, 0x02);
    REG_GL(glBindFramebuffer);
    REG_GL(glFramebufferTexture2D);
    REG_GL_PTR(glGenRenderbuffers, 0x02);
    REG_GL_PTR(glDeleteRenderbuffers, 0x02);
    REG_GL(glBindRenderbuffer);
    REG_GL(glRenderbufferStorage);
    REG_GL(glFramebufferRenderbuffer);
    REG_GL(glCheckFramebufferStatus);
    // Textures.
    REG_GL(glGenerateMipmap);
    REG_GL(glActiveTexture);
    REG_GL(glTexImage2D);
    REG_GL(glTexSubImage2D);
    REG_GL(glTexParameteri);
    REG_GL_EX(glTexParameterf, 0, 2, 1, THUNK_MIXED_FP);
    REG_GL_EX(glCompressedTexImage2D, (1u << 8), 1, 0, 0);
    // Drawing.
    REG_GL(glDrawArrays);
    REG_GL_PTR(glDrawElements, 0x10);     // arg 4: const void *indices
    // Blending.
    REG_GL(glBlendFunc);
    REG_GL(glBlendFuncSeparate);
    REG_GL(glBlendEquation);
    REG_GL(glBlendEquationSeparate);
    REG_GL(glColorMask);
    // Depth/Stencil.
    REG_GL(glDepthFunc);
    REG_GL(glDepthMask);
    REG_GL(glStencilFunc);
    REG_GL(glStencilOp);
    REG_GL(glStencilMask);
    // Misc.
    REG_GL_FP(glClearDepthf, 1);
    REG_GL(glPixelStorei);
    REG_GL(glFinish);
    REG_GL(glFlush);
    REG_GL(glEnable);
    REG_GL(glDisable);
    REG_GL(glIsEnabled);
    REG_GL(glViewport);
    REG_GL(glScissor);
    REG_GL_FP(glClearColor, 4);
    REG_GL(glClear);
    REG_GL(glGetError);
    REG_GL_EX(glGetString, 0, 0, 0, THUNK_RET_STRING);
    REG_GL_PTR(glGetIntegerv, 0x02);
    REG_GL(glHint);
    REG_GL(glFrontFace);
    REG_GL(glCullFace);
    REG_GL_FP(glLineWidth, 1);
    REG_GL(glPolygonOffset);
    REG_GL(glSampleCoverage);
#undef REG_GL
#undef REG_GL_PTR
#undef REG_GL_FP
#undef REG_GL_EX
    // ── libGLESv2.so / libGLESv2.so.2 ─────────────────────────────
    // GLESv2 shares most entry points with OpenGL 2.0+ (no fixed-function).
    const char* gles_libs[] = {"libGLESv2.so", "libGLESv2.so.2"};
#if defined(BIFROST_THUNK_HAVE_GL)
    #define REG_GLES(name) do { \
        void* p = dlsym(RTLD_DEFAULT, #name); \
        for (const char* L : gles_libs) register_function_(L, #name, p); \
    } while(0)
    #define REG_GLES_PTR(name, ptrs) do { \
        void* p = dlsym(RTLD_DEFAULT, #name); \
        for (const char* L : gles_libs) register_function_(L, #name, p, ptrs); \
    } while(0)
    #define REG_GLES_FP(name, nf) do { \
        void* p = dlsym(RTLD_DEFAULT, #name); \
        for (const char* L : gles_libs) register_function_(L, #name, p, 0, 0, nf); \
    } while(0)
    #define REG_GLES_EX(name, ptrs, nstack, nfloat, fl) do { \
        void* p = dlsym(RTLD_DEFAULT, #name); \
        for (const char* L : gles_libs) \
            register_function_(L, #name, p, ptrs, nstack, nfloat, fl); \
    } while(0)
#else
    #define REG_GLES(name) do { \
        for (const char* L : gles_libs) register_function_(L, #name, nullptr); \
    } while(0)
    #define REG_GLES_PTR(name, ptrs) do { \
        for (const char* L : gles_libs) register_function_(L, #name, nullptr, ptrs); \
    } while(0)
    #define REG_GLES_FP(name, nf) do { \
        for (const char* L : gles_libs) register_function_(L, #name, nullptr, 0, 0, nf); \
    } while(0)
    #define REG_GLES_EX(name, ptrs, nstack, nfloat, fl) do { \
        for (const char* L : gles_libs) \
            register_function_(L, #name, nullptr, ptrs, nstack, nfloat, fl); \
    } while(0)
#endif
    // Core rendering.
    REG_GLES(glClear);
    REG_GLES_FP(glClearColor, 4);
    REG_GLES_FP(glClearDepthf, 1);
    REG_GLES(glClearStencil);
    REG_GLES(glFlush);
    REG_GLES(glFinish);
    REG_GLES(glGetError);
    REG_GLES(glEnable);
    REG_GLES(glDisable);
    REG_GLES(glIsEnabled);
    REG_GLES(glViewport);
    REG_GLES(glScissor);
    REG_GLES(glHint);
    REG_GLES(glFrontFace);
    REG_GLES(glCullFace);
    REG_GLES_FP(glLineWidth, 1);
    REG_GLES(glPolygonOffset);
    REG_GLES(glPixelStorei);
    // Shaders.
    REG_GLES(glCreateShader);
    REG_GLES_EX(glShaderSource, 0x0C, 0, 0, THUNK_SHADER_SOURCE);
    REG_GLES(glCompileShader);
    REG_GLES(glDeleteShader);
    REG_GLES(glCreateProgram);
    REG_GLES(glAttachShader);
    REG_GLES(glDetachShader);
    REG_GLES(glLinkProgram);
    REG_GLES(glUseProgram);
    REG_GLES(glDeleteProgram);
    REG_GLES_PTR(glGetShaderiv, 0x04);
    REG_GLES_PTR(glGetProgramiv, 0x04);
    REG_GLES_PTR(glGetShaderInfoLog, 0x08);
    REG_GLES_PTR(glGetProgramInfoLog, 0x08);
    REG_GLES_PTR(glGetAttribLocation, 0x02);
    REG_GLES_PTR(glGetUniformLocation, 0x02);
    REG_GLES_PTR(glBindAttribLocation, 0x04);
    REG_GLES(glUniform1i);
    REG_GLES_EX(glUniform1f, 0, 1, 1, THUNK_MIXED_FP);
    REG_GLES_EX(glUniform2f, 0, 1, 2, THUNK_MIXED_FP);
    REG_GLES_EX(glUniform3f, 0, 1, 3, THUNK_MIXED_FP);
    REG_GLES_EX(glUniform4f, 0, 1, 4, THUNK_MIXED_FP);
    REG_GLES_PTR(glUniform1fv, 0x04);
    REG_GLES_PTR(glUniform2fv, 0x04);
    REG_GLES_PTR(glUniform3fv, 0x04);
    REG_GLES_PTR(glUniform4fv, 0x04);
    REG_GLES_PTR(glUniformMatrix3fv, 0x20);
    REG_GLES_PTR(glUniformMatrix4fv, 0x20);
    REG_GLES(glEnableVertexAttribArray);
    REG_GLES(glDisableVertexAttribArray);
    REG_GLES_PTR(glVertexAttribPointer, 0x80);
    REG_GLES_PTR(glGetFloatv, 0x02);
    REG_GLES_PTR(glGetBooleanv, 0x02);
    // VBOs.
    REG_GLES_PTR(glGenBuffers, 0x02);
    REG_GLES_PTR(glDeleteBuffers, 0x02);
    REG_GLES(glBindBuffer);
    REG_GLES_PTR(glBufferData, 0x08);
    REG_GLES_PTR(glBufferSubData, 0x08);
    // FBOs.
    REG_GLES_PTR(glGenFramebuffers, 0x02);
    REG_GLES_PTR(glDeleteFramebuffers, 0x02);
    REG_GLES(glBindFramebuffer);
    REG_GLES(glFramebufferTexture2D);
    REG_GLES_PTR(glGenRenderbuffers, 0x02);
    REG_GLES_PTR(glDeleteRenderbuffers, 0x02);
    REG_GLES(glBindRenderbuffer);
    REG_GLES(glRenderbufferStorage);
    REG_GLES(glFramebufferRenderbuffer);
    REG_GLES(glCheckFramebufferStatus);
    // Textures.
    REG_GLES_PTR(glGenTextures, 0x02);
    REG_GLES_PTR(glDeleteTextures, 0x02);
    REG_GLES(glBindTexture);
    REG_GLES(glActiveTexture);
    REG_GLES(glGenerateMipmap);
    REG_GLES(glTexParameteri);
    REG_GLES_EX(glTexParameterf, 0, 2, 1, THUNK_MIXED_FP);
    REG_GLES_EX(glTexImage2D, (1u << 8), 1, 0, 0);
    REG_GLES_EX(glTexSubImage2D, (1u << 8), 1, 0, 0);
    // Drawing.
    REG_GLES(glDrawArrays);
    REG_GLES_PTR(glDrawElements, 0x10);
    // Blending.
    REG_GLES(glBlendFunc);
    REG_GLES(glBlendFuncSeparate);
    REG_GLES(glBlendEquation);
    REG_GLES(glBlendEquationSeparate);
    REG_GLES(glColorMask);
    // Depth/Stencil.
    REG_GLES(glDepthFunc);
    REG_GLES(glDepthMask);
    REG_GLES_FP(glDepthRangef, 2);
    REG_GLES(glStencilFunc);
    REG_GLES(glStencilOp);
    REG_GLES(glStencilMask);
    REG_GLES(glStencilFuncSeparate);
    REG_GLES(glStencilOpSeparate);
    REG_GLES(glStencilMaskSeparate);
    // Queries.
    REG_GLES_EX(glGetString, 0, 0, 0, THUNK_RET_STRING);
    REG_GLES_PTR(glGetIntegerv, 0x02);
    REG_GLES(glSampleCoverage);
#undef REG_GLES
#undef REG_GLES_PTR
#undef REG_GLES_FP
#undef REG_GLES_EX
    // ── libEGL.so / libEGL.so.1 ───────────────────────────────────
    const char* egl_libs[] = {"libEGL.so", "libEGL.so.1"};
#if defined(BIFROST_THUNK_HAVE_EGL)
    #define REG_EGL(name) do { \
        void* p = dlsym(RTLD_DEFAULT, #name); \
        for (const char* L : egl_libs) register_function_(L, #name, p); \
    } while(0)
#else
    #define REG_EGL(name) do { \
        for (const char* L : egl_libs) register_function_(L, #name, nullptr); \
    } while(0)
#endif
    REG_EGL(eglGetDisplay);
    REG_EGL(eglInitialize);
    REG_EGL(eglChooseConfig);
    REG_EGL(eglCreateContext);
    REG_EGL(eglDestroyContext);
    REG_EGL(eglMakeCurrent);
    REG_EGL(eglSwapBuffers);
    REG_EGL(eglDestroySurface);
    REG_EGL(eglCreateWindowSurface);
    REG_EGL(eglCreatePbufferSurface);
    REG_EGL(eglQuerySurface);
    REG_EGL(eglGetConfigAttrib);
    REG_EGL(eglGetError);
    REG_EGL(eglTerminate);
    REG_EGL(eglBindAPI);
    REG_EGL(eglReleaseThread);
    REG_EGL(eglWaitGL);
    REG_EGL(eglWaitNative);
    REG_EGL(eglSwapInterval);
    REG_EGL(eglQueryString);
    // Resolve extension/GL entry points to our trampolines.
#if defined(BIFROST_THUNK_HAVE_EGL)
    {
        void* p = dlsym(RTLD_DEFAULT, "eglGetProcAddress");
        if (!p) p = reinterpret_cast<void*>(1);
        for (const char* L : egl_libs)
            register_function_(L, "eglGetProcAddress", p, 0x01, 0, 0,
                               THUNK_GET_PROC);
    }
#else
    for (const char* L : egl_libs)
        register_function_(L, "eglGetProcAddress",
                           reinterpret_cast<void*>(1), 0x01, 0, 0,
                           THUNK_GET_PROC);
#endif
#undef REG_EGL
    // ── libSDL2.so / libSDL2-2.0.so.0 ─────────────────────────────
    const char* sdl_libs[] = {"libSDL2.so", "libSDL2-2.0.so.0"};
#if defined(BIFROST_THUNK_HAVE_SDL2)
    #define REG_SDL(name) do { \
        void* p = dlsym(RTLD_DEFAULT, #name); \
        for (const char* L : sdl_libs) register_function_(L, #name, p); \
    } while(0)
    #define REG_SDL_PTR(name, ptrs) do { \
        void* p = dlsym(RTLD_DEFAULT, #name); \
        for (const char* L : sdl_libs) register_function_(L, #name, p, ptrs); \
    } while(0)
    #define REG_SDL_EX(name, ptrs, nstack, nfloat, fl) do { \
        void* p = dlsym(RTLD_DEFAULT, #name); \
        for (const char* L : sdl_libs) \
            register_function_(L, #name, p, ptrs, nstack, nfloat, fl); \
    } while(0)
#else
    #define REG_SDL(name) do { \
        for (const char* L : sdl_libs) register_function_(L, #name, nullptr); \
    } while(0)
    #define REG_SDL_PTR(name, ptrs) do { \
        for (const char* L : sdl_libs) register_function_(L, #name, nullptr, ptrs); \
    } while(0)
    #define REG_SDL_EX(name, ptrs, nstack, nfloat, fl) do { \
        for (const char* L : sdl_libs) \
            register_function_(L, #name, nullptr, ptrs, nstack, nfloat, fl); \
    } while(0)
#endif
    REG_SDL(SDL_Init);
    REG_SDL(SDL_Quit);
    // title string is arg 0
    REG_SDL_PTR(SDL_CreateWindow, 0x01);
    REG_SDL(SDL_CreateWindowAndRenderer);
    REG_SDL(SDL_DestroyWindow);
    REG_SDL(SDL_GL_CreateContext);
    REG_SDL(SDL_GL_DeleteContext);
    REG_SDL(SDL_GL_MakeCurrent);
    REG_SDL(SDL_GL_SwapWindow);
    REG_SDL(SDL_GL_SetAttribute);
    REG_SDL_PTR(SDL_GL_GetAttribute, 0x02);
#if defined(BIFROST_THUNK_HAVE_SDL2)
    {
        void* p = dlsym(RTLD_DEFAULT, "SDL_GL_GetProcAddress");
        if (!p) p = reinterpret_cast<void*>(1);
        for (const char* L : sdl_libs)
            register_function_(L, "SDL_GL_GetProcAddress", p, 0x01, 0, 0,
                               THUNK_GET_PROC);
    }
#else
    for (const char* L : sdl_libs)
        register_function_(L, "SDL_GL_GetProcAddress",
                           reinterpret_cast<void*>(1), 0x01, 0, 0,
                           THUNK_GET_PROC);
#endif
    REG_SDL_PTR(SDL_PollEvent, 0x01);
    REG_SDL_PTR(SDL_WaitEvent, 0x01);
    REG_SDL_PTR(SDL_PushEvent, 0x01);
    REG_SDL(SDL_GetWindowSurface);
    REG_SDL(SDL_UpdateWindowSurface);
    REG_SDL(SDL_UpdateWindowSurfaceRects);
    REG_SDL_PTR(SDL_SetWindowTitle, 0x02);
    REG_SDL_EX(SDL_GetWindowTitle, 0, 0, 0, THUNK_RET_STRING);
    REG_SDL(SDL_SetWindowSize);
    REG_SDL_PTR(SDL_GetWindowSize, 0x06);
    REG_SDL(SDL_SetWindowPosition);
    REG_SDL(SDL_ShowWindow);
    REG_SDL(SDL_HideWindow);
    REG_SDL(SDL_RaiseWindow);
    REG_SDL(SDL_SetWindowFullscreen);
    REG_SDL(SDL_GetWindowFlags);
    REG_SDL(SDL_GetTicks);
    REG_SDL(SDL_GetPerformanceCounter);
    REG_SDL(SDL_GetPerformanceFrequency);
    REG_SDL(SDL_Delay);
    REG_SDL_EX(SDL_GetError, 0, 0, 0, THUNK_RET_STRING);
    REG_SDL(SDL_ClearError);
    REG_SDL_PTR(SDL_SetHint, 0x03);
    REG_SDL_EX(SDL_GetHint, 0x01, 0, 0, THUNK_RET_STRING);
    REG_SDL(SDL_CreateRenderer);
    REG_SDL(SDL_DestroyRenderer);
    REG_SDL(SDL_SetRenderDrawColor);
    REG_SDL(SDL_RenderClear);
    REG_SDL(SDL_RenderPresent);
    REG_SDL(SDL_RenderCopy);
    REG_SDL(SDL_CreateTexture);
    REG_SDL(SDL_CreateTextureFromSurface);
    REG_SDL(SDL_DestroyTexture);
    REG_SDL(SDL_UpdateTexture);
    REG_SDL(SDL_RenderSetViewport);
    REG_SDL(SDL_RenderSetLogicalSize);
    REG_SDL(SDL_SetRenderDrawBlendMode);
    REG_SDL(SDL_MapRGB);
    REG_SDL(SDL_MapRGBA);
    REG_SDL(SDL_GetRGB);
    REG_SDL(SDL_GetRGBA);
    REG_SDL(SDL_CreateRGBSurface);
    REG_SDL(SDL_CreateRGBSurfaceWithFormat);
    REG_SDL(SDL_FreeSurface);
    REG_SDL(SDL_SetColorKey);
    REG_SDL(SDL_SetSurfaceAlphaMod);
    REG_SDL(SDL_LoadBMP_RW);
    REG_SDL(SDL_SaveBMP_RW);
    REG_SDL(SDL_RWFromFile);
    REG_SDL(SDL_RWFromMem);
    REG_SDL(SDL_RWclose);
    REG_SDL(SDL_RWread);
    REG_SDL(SDL_RWwrite);
    REG_SDL(SDL_RWseek);
    REG_SDL(SDL_RWtell);
    REG_SDL(SDL_GetKeyboardState);
    REG_SDL(SDL_GetModState);
    REG_SDL(SDL_SetModState);
    REG_SDL(SDL_GetMouseState);
    REG_SDL(SDL_GetGlobalMouseState);
    REG_SDL(SDL_WarpMouseInWindow);
    REG_SDL(SDL_ShowCursor);
    REG_SDL(SDL_CreateCursor);
    REG_SDL(SDL_FreeCursor);
    REG_SDL(SDL_SetCursor);
    REG_SDL(SDL_GetNumVideoDisplays);
    REG_SDL(SDL_GetDisplayBounds);
    REG_SDL(SDL_GetCurrentVideoMode);
    REG_SDL(SDL_GetWindowDisplayIndex);
    REG_SDL(SDL_Vulkan_LoadLibrary);
    REG_SDL(SDL_Vulkan_GetVkGetInstanceProcAddr);
    REG_SDL(SDL_Vulkan_CreateSurface);
#undef REG_SDL
#undef REG_SDL_PTR
#undef REG_SDL_EX
}
// ── FrostGraphics::thunk() — out-of-line definition ───────────────────
// Lives here (not in graphics.cpp) because it needs the full
// GraphicThunk type to construct via `new`. Returns the lazily-created
// GraphicThunk instance owned by FrostGraphics.
GraphicThunk* FrostGraphics::thunk() {
    if (!thunk_) {
        thunk_ = std::unique_ptr<GraphicThunk>(new GraphicThunk());
    }
    return thunk_.get();
}
// v1.5.0.alpha: audio_thunk() and display_thunk() — same lazy pattern.
// They live here for the same reason thunk() does: the FrostGraphics
// header only forward-declares AudioThunk / DisplayThunk, so the
// unique_ptr ctor needs the full type, which is only visible in this
// .cpp (which includes frost/audio_thunk.hpp and frost/display_thunk.hpp).
AudioThunk* FrostGraphics::audio_thunk() {
    if (!audio_thunk_) {
        audio_thunk_ = std::unique_ptr<AudioThunk>(new AudioThunk());
    }
    return audio_thunk_.get();
}
DisplayThunk* FrostGraphics::display_thunk() {
    if (!display_thunk_) {
        display_thunk_ = std::unique_ptr<DisplayThunk>(new DisplayThunk());
    }
    return display_thunk_.get();
}
} // namespace arm64emu
