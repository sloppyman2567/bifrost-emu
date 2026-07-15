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
//   NOP                      →  0xD503201F
namespace trampoline_enc {
    constexpr uint32_t MOVZ_Xd_IMM16(int Xd, uint16_t imm16) {
        return 0xD2800000u | (static_cast<uint32_t>(imm16) << 5)
                            | (static_cast<uint32_t>(Xd) & 0x1Fu);
    }
    constexpr uint32_t SVC_0   = 0xD4000001u;
    constexpr uint32_t NOP     = 0xD503201Fu;
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
    uint8_t     pointer_args = 0;  // bitmask: which args (0-7) are pointers
};
struct GraphicThunkImpl {
    bool   enabled = false;
    Memory* mem    = nullptr;
    bool   initialized = false;
    // The trampoline page: a single 64 KiB region of guest memory.
    uint64_t trampoline_base = 0;
    static constexpr uint64_t TRAMPOLINE_PAGE_SIZE =
        GraphicThunk::TRAMPOLINE_SIZE * GraphicThunk::MAX_SYMBOLS;  // 64 KiB
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
    // Register the known GL/EGL/SDL2 entry points.
    register_known_symbols_();
    impl_->initialized = true;
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
                                       uint8_t pointer_args) {
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
    lt->entries.push_back({sym, host_fn, addr, sym_id, pointer_args});
    impl_->id_to_idx_.push_back({
        static_cast<uint32_t>(std::distance(impl_->libs_.data(), lt)),
        static_cast<uint32_t>(lt->entries.size() - 1)
    });
    if (getenv("BIFROST_THUNK_TRACE")) {
        fprintf(stderr, "[thunk] registered %s:%s -> 0x%llx (id=%u, ptrs=0x%x)\n",
                lib.c_str(), sym.c_str(),
                static_cast<unsigned long long>(addr), sym_id, pointer_args);
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
    buf[3] = trampoline_enc::NOP;
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
    // v1.5.0.alpha: strip the ID_BASE_GRAPHICS prefix to get
    // the local index. If the symbol_id is outside our range, return
    // -ENOENT so the dispatcher can try other thunks.
    if ((symbol_id & GraphicThunk::ID_MASK) != GraphicThunk::ID_BASE_GRAPHICS) {
        return -ENOENT;  // belongs to a different thunk
    }
    uint32_t local_id = symbol_id - GraphicThunk::ID_BASE_GRAPHICS;
    if (local_id >= impl_->id_to_idx_.size()) {
        if (getenv("BIFROST_THUNK_TRACE")) {
            fprintf(stderr, "[thunk] dispatch: unknown symbol_id=%u\n", symbol_id);
        }
        return -ENOENT;
    }
    // Look up the entry (lock-free — id_to_idx_ is immutable after init).
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
    // Read the first 8 args from the CPU's general-purpose registers.
    // Per AArch64 AAPCS, the first 8 integer/pointer args are in x0..x7.
    // FP args would be in v0..v7, but most GL/EGL/SDL2 entry points take
    // integer/pointer args only (GLbitfield, GLuint, GLsizei, GLclampf,
    // pointer, etc.). For FP args, the dispatch would need to read from
    // cpu.v_lo[] — left as a future enhancement.
    uint64_t args[8];
    for (int i = 0; i < 8; i++) {
        args[i] = cpu.regs[i];
    }
    // v1.5.0.alpha: translate pointer args from guest to host.
    // For each arg marked as a pointer in entry.pointer_args, translate
    // the guest address to a host pointer using Memory::guest_to_host_ptr.
    // This is critical — without it, passing a guest pointer (e.g. a
    // vertex array address) to the host GL function would crash because
    // the host can't read guest memory at that address.
    //
    // Only args in the direct window (< 4 GiB) can be translated. Args
    // in the high mmap region (≥ 4 GiB) are passed as-is — the host
    // function will likely crash, but that's a known limitation (the
    // thunk would need to copy the data to a low buffer first).
    if (entry.pointer_args && impl_->mem) {
        for (int i = 0; i < 8; i++) {
            if (entry.pointer_args & (1u << i)) {
                uint8_t* host_ptr = impl_->mem->guest_to_host_ptr(args[i]);
                if (host_ptr) {
                    args[i] = reinterpret_cast<uint64_t>(host_ptr);
                }
                // If translation failed (address ≥ 4 GiB), pass the
                // original value. The host function may handle it
                // gracefully (e.g. NULL check) or crash — either way
                // it's a visible failure, not silent corruption.
            }
        }
    }
    if (getenv("BIFROST_THUNK_TRACE")) {
        fprintf(stderr, "[thunk] dispatch: %s (host_fn=%p) "
                "a0=0x%llx a1=0x%llx a2=0x%llx a3=0x%llx ptrs=0x%x\n",
                entry.name.c_str(), entry.host_fn,
                static_cast<unsigned long long>(args[0]),
                static_cast<unsigned long long>(args[1]),
                static_cast<unsigned long long>(args[2]),
                static_cast<unsigned long long>(args[3]),
                entry.pointer_args);
    }
    // Call the host function. We use a union of function pointer types
    // to handle the common calling conventions. The host's calling
    // convention (System V AMD64) is: first 6 integer/pointer args in
    // rdi, rsi, rdx, rcx, r8, r9; first 8 FP args in xmm0..xmm7.
    //
    // We cast to a generic 8-arg function pointer. This works for most
    // GL/EGL/SDL2 entry points because they take 0-8 integer/pointer
    // args. FP-arg functions would need a separate dispatch path.
    using GenericFn = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
    auto fn = reinterpret_cast<GenericFn>(entry.host_fn);
    uint64_t ret = fn(args[0], args[1], args[2], args[3],
                       args[4], args[5], args[6], args[7]);
    cpu.regs[0] = ret;
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
#else
    #define REG_GL(name) do { \
        for (const char* L : gl_libs) register_function_(L, #name, nullptr); \
    } while(0)
    #define REG_GL_PTR(name, ptrs) do { \
        for (const char* L : gl_libs) register_function_(L, #name, nullptr, ptrs); \
    } while(0)
#endif
    REG_GL(glClear);
    REG_GL(glClearColor);
    REG_GL(glBegin);
    REG_GL(glEnd);
    REG_GL(glVertex3f);
    REG_GL(glVertex2f);
    REG_GL_PTR(glVertex3fv, 0x01);      // arg 0: const GLfloat *v
    REG_GL(glColor3f);
    REG_GL(glColor4f);
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
    REG_GL(glRotatef);
    REG_GL(glTranslatef);
    REG_GL(glScalef);
    REG_GL_PTR(glGenTextures, 0x02);    // arg 1: GLuint *textures
    REG_GL(glBindTexture);
    REG_GL(glTexParameteri);
    // glTexImage2D: arg 8 (data) is pointer — beyond our 8-arg limit.
    // Pass 0 for pointer_args; the host fn will get garbage for data.
    REG_GL(glTexImage2D);
    REG_GL(glTexSubImage2D);
    REG_GL(glEnableClientState);
    REG_GL(glDisableClientState);
    REG_GL_PTR(glVertexPointer, 0x08);  // arg 3: const void *pointer
    REG_GL_PTR(glColorPointer, 0x08);   // arg 3: const void *pointer
    REG_GL_PTR(glTexCoordPointer, 0x08);// arg 3: const void *pointer
    REG_GL(glDrawArrays);
    REG_GL_PTR(glDrawElements, 0x10);   // arg 4: const void *indices
    // glGetString returns const GLubyte* — the return value is a host
    // pointer, not a guest pointer. This is a known issue: the guest
    // will get a host pointer it can't dereference. For now, pass 0
    // (no pointer args); the guest should use a wrapper that copies
    // the string.
    REG_GL(glGetString);
    REG_GL_PTR(glGetIntegerv, 0x01);    // arg 1: GLint *params
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
    REG_GL(glBlendFunc);
    REG_GL(glHint);
    REG_GL(glPixelStorei);
    REG_GL_PTR(glReadPixels, 0x80);     // arg 7: void *pixels
    REG_GL(glDrawBuffer);
    REG_GL(glClearDepth);
    REG_GL(glClearStencil);
    REG_GL(glPointSize);
    REG_GL(glLineWidth);
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
    REG_GL_PTR(glShaderSource, 0x08);     // arg 3: const GLchar* const*string
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
    REG_GL(glUniform1i);
    REG_GL(glUniform1f);
    REG_GL(glUniform2f);
    REG_GL(glUniform3f);
    REG_GL(glUniform4f);
    REG_GL_PTR(glUniform1fv, 0x04);       // arg 2: const GLfloat *value
    REG_GL_PTR(glUniformMatrix4fv, 0x20); // arg 3: const GLfloat *value
    REG_GL(glEnableVertexAttribArray);
    REG_GL(glDisableVertexAttribArray);
    REG_GL_PTR(glVertexAttribPointer, 0x80); // arg 5: const void *pointer
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
    REG_GL(glTexParameterf);
    REG_GL_PTR(glCompressedTexImage2D, 0x80); // arg 8: const void *data
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
    REG_GL(glClearDepthf);
    REG_GL(glPixelStorei);
    REG_GL(glFinish);
    REG_GL(glFlush);
    REG_GL(glEnable);
    REG_GL(glDisable);
    REG_GL(glIsEnabled);
    REG_GL(glViewport);
    REG_GL(glScissor);
    REG_GL(glClearColor);
    REG_GL(glClear);
    REG_GL(glGetError);
    REG_GL_PTR(glGetString, 0x00);
    REG_GL_PTR(glGetIntegerv, 0x02);
    REG_GL(glHint);
    REG_GL(glFrontFace);
    REG_GL(glCullFace);
    REG_GL(glLineWidth);
    REG_GL(glPolygonOffset);
    REG_GL(glSampleCoverage);
#undef REG_GL
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
#else
    #define REG_GLES(name) do { \
        for (const char* L : gles_libs) register_function_(L, #name, nullptr); \
    } while(0)
    #define REG_GLES_PTR(name, ptrs) do { \
        for (const char* L : gles_libs) register_function_(L, #name, nullptr, ptrs); \
    } while(0)
#endif
    // Core rendering.
    REG_GLES(glClear);
    REG_GLES(glClearColor);
    REG_GLES(glClearDepthf);
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
    REG_GLES(glLineWidth);
    REG_GLES(glPolygonOffset);
    REG_GLES(glPixelStorei);
    // Shaders.
    REG_GLES(glCreateShader);
    REG_GLES_PTR(glShaderSource, 0x08);
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
    REG_GLES(glUniform1i);
    REG_GLES(glUniform1f);
    REG_GLES(glUniform2f);
    REG_GLES(glUniform3f);
    REG_GLES(glUniform4f);
    REG_GLES_PTR(glUniform1fv, 0x04);
    REG_GLES_PTR(glUniformMatrix4fv, 0x20);
    REG_GLES(glEnableVertexAttribArray);
    REG_GLES(glDisableVertexAttribArray);
    REG_GLES_PTR(glVertexAttribPointer, 0x80);
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
    REG_GLES(glTexParameterf);
    REG_GLES(glTexImage2D);
    REG_GLES(glTexSubImage2D);
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
    REG_GLES(glDepthRangef);
    REG_GLES(glStencilFunc);
    REG_GLES(glStencilOp);
    REG_GLES(glStencilMask);
    // Queries.
    REG_GLES_PTR(glGetString, 0x00);
    REG_GLES_PTR(glGetIntegerv, 0x02);
    REG_GLES(glSampleCoverage);
#undef REG_GLES
#undef REG_GLES_PTR
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
    REG_EGL(eglGetDisplay);
    REG_EGL(eglBindAPI);
    REG_EGL(eglReleaseThread);
    REG_EGL(eglWaitGL);
    REG_EGL(eglWaitNative);
#undef REG_EGL
    // ── libSDL2.so / libSDL2-2.0.so.0 ─────────────────────────────
    const char* sdl_libs[] = {"libSDL2.so", "libSDL2-2.0.so.0"};
#if defined(BIFROST_THUNK_HAVE_SDL2)
    #define REG_SDL(name) do { \
        void* p = dlsym(RTLD_DEFAULT, #name); \
        for (const char* L : sdl_libs) register_function_(L, #name, p); \
    } while(0)
#else
    #define REG_SDL(name) do { \
        for (const char* L : sdl_libs) register_function_(L, #name, nullptr); \
    } while(0)
#endif
    REG_SDL(SDL_Init);
    REG_SDL(SDL_Quit);
    REG_SDL(SDL_CreateWindow);
    REG_SDL(SDL_CreateWindowAndRenderer);
    REG_SDL(SDL_DestroyWindow);
    REG_SDL(SDL_GL_CreateContext);
    REG_SDL(SDL_GL_DeleteContext);
    REG_SDL(SDL_GL_MakeCurrent);
    REG_SDL(SDL_GL_SwapWindow);
    REG_SDL(SDL_GL_SetAttribute);
    REG_SDL(SDL_GL_GetAttribute);
    REG_SDL(SDL_PollEvent);
    REG_SDL(SDL_WaitEvent);
    REG_SDL(SDL_PushEvent);
    REG_SDL(SDL_GetWindowSurface);
    REG_SDL(SDL_UpdateWindowSurface);
    REG_SDL(SDL_UpdateWindowSurfaceRects);
    REG_SDL(SDL_SetWindowTitle);
    REG_SDL(SDL_GetWindowTitle);
    REG_SDL(SDL_SetWindowSize);
    REG_SDL(SDL_GetWindowSize);
    REG_SDL(SDL_SetWindowPosition);
    REG_SDL(SDL_ShowWindow);
    REG_SDL(SDL_HideWindow);
    REG_SDL(SDL_RaiseWindow);
    REG_SDL(SDL_SetWindowFullscreen);
    REG_SDL(SDL_GetWindowFlags);
    REG_SDL(SDL_GetTicks);
    REG_SDL(SDL_Delay);
    REG_SDL(SDL_GetError);
    REG_SDL(SDL_ClearError);
    REG_SDL(SDL_SetHint);
    REG_SDL(SDL_GetHint);
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
