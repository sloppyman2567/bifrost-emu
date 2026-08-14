// frontend/dynamic_linker.h — Dynamic linking support for bifrost-emu.
//
// Implements DT_NEEDED processing, GOT/PLT relocation, symbol
// resolution, and TLS block allocation for dynamically-linked AArch64
// ELF binaries. The dynamic linker is invoked by
// Emulator::load_elf_file() after the main binary's PT_LOAD segments
// have been mapped.
//
// We now:
//   1. Parse the main binary's PT_DYNAMIC segment.
//   2. For each DT_NEEDED entry, locate and load the shared library
//      (libc.so, libm.so, etc.) from common host multiarch paths.
//   3. Build a global symbol table from all loaded objects' .dynsym
//      sections, so relocations can resolve symbols across objects.
//   4. Apply R_AARCH64_RELATIVE, R_AARCH64_GLOB_DAT, R_AARCH64_ABS64,
//      R_AARCH64_JUMP_SLOT, R_AARCH64_IRELATIVE relocations.
//   5. Allocate a static TLS block for each object that has a PT_TLS
//      segment, and apply R_AARCH64_TLS_DTPMOD, R_AARCH64_TLS_DTPREL,
//      R_AARCH64_TLS_TPREL, and R_AARCH64_TLSDESC relocations.
//   6. Set up the GOT[1] (link map) and GOT[2] (resolver) entries so
//      lazy PLT binding works (the resolver is a guest-side stub that
//      calls back into the emulator via a syscall).
//
// TLS model:
//   We use the "static TLS" model — all PT_TLS blocks are allocated
//   up-front at load time, laid out contiguously in the TP-relative
//   region. This matches what musl/glibc do for initially-loaded
//   libraries (i.e., not dlopen'd ones). The TPIDR_EL0 register points
//   to the end of the static TLS block (TP = &static_tls_block[size]);
//   each thread gets its own copy via clone(CLONE_SETTLS).
//
//   Layout (TP-relative offsets):
//     [0 .. libc_tls_size)         — libc's TLS block (TP-offset = -libc_tls_size)
//     [libc_tls_size .. total)     — main binary's TLS block (TP-offset = -total)
//   The TCB (thread control block) lives at TP+0 (16 bytes for musl,
//   8 bytes for glibc). We leave it zeroed; libc's __libc_setup_tls
//   will overwrite it.
//
// Limitations:
//   - R_AARCH64_COPY is implemented via a deferred second pass (the
//     COPY must read the defining symbol's bytes AFTER that object's
//     RELATIVE relocations are applied). See apply_pending_copies_().
//   - Symbol versioning (.gnu.version / .gnu.version_r) IS parsed and
//     used for versioned symbol resolution (versioned_symbols_ map).
//   - dlopen() of a TLS-using library after the static TLS block is
//     sized is not supported (would require dynamic TLS allocation).
//   - TLSDESC uses an inline static resolver (desc[0]=0, desc[1]=
//     TP-offset) — no PLT call to a resolver. This is the "static
//     TLSDESC" trick valid when the TP-offset is known at load time.
//   - dladdr() is overridden (1.5.2-alpha): the dladdr@GLIBC_2.34 and
//     dladdr@GLIBC_2.0 symbols are FORCE-overridden to point at our
//     OFF_DLADDR stub, which calls DynamicLinker::dladdr() via syscall
//     0x1005. This fills in Dl_info (dli_fname, dli_fbase, dli_sname,
//     dli_saddr) and returns 1 (found) or 0. The dlfcn_hook (hook+40)
//     is also wired to the same stub. (Previously disabled because an
//     earlier attempt crashed glibc startup via the _dl_addr path; the
//     later dlfcn_hook wiring made the stub safe to register as a
//     symbol override too.)
//
// References:
//   - ARM IHI 0056B (AArch64 ELF ABI): https://github.com/ARM-software/abi-aa
//   - ELF gABI: https://refspecs.linuxbase.org/elf/gabi4+/ch5.dynamic.html
//   - Android ELF TLS: https://github.com/aosp-mirror/platform_bionic/blob/main/docs/elf-tls.md
//   - MaskRay TLS blog: https://maskray.me/blog/2021-02-14-all-about-thread-local-storage
//   - Linux kernel: fs/binfmt_elf.c, load_elf_binary()
#pragma once
#include "bifrost/types.hpp"
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
namespace arm64emu {
class Memory;
class CPU;
// PT_TLS segment info for a loaded object.
struct TlsSegment {
    uint64_t vaddr    = 0;  // file vaddr (relative to base)
    uint64_t filesz   = 0;  // size of initialized data
    uint64_t memsz    = 0;  // size in memory (incl. .bss)
    uint64_t align    = 1;  // alignment in bytes
    bool     present  = false;
};
// Information about one loaded ELF object (main binary or shared lib).
struct LoadedObject {
    std::string name;             // soname or path
    std::string soname;           // DT_SONAME (for dedup; empty if not present)
    std::string runpath;          // DT_RUNPATH (semicolon-separated, $ORIGIN expanded)
    std::string rpath;            // DT_RPATH (deprecated, but still used by some binaries)
    uint64_t    base_addr = 0;    // load base (0 for main binary if not PIE)
    uint64_t    entry     = 0;    // entry point (absolute)
    uint64_t    dyn_addr  = 0;    // PT_DYNAMIC vaddr (absolute)
    uint64_t    symtab_addr = 0;  // DT_SYMTAB (absolute)
    uint64_t    strtab_addr = 0;  // DT_STRTAB (absolute)
    uint64_t    symtab_count = 0; // number of symbols in .dynsym
    uint64_t    jmprel_addr = 0;  // DT_JMPREL (absolute)
    uint64_t    jmprel_size = 0;  // DT_PLTRELSZ
    // DT_RELR (compact relative relocations). glibc 2.36+ / binutils 2.38+
    // produce these by default. Without processing them, libc's
    // .init_array / .data.rel.ro / .got relative pointers stay at their
    // (unrelocated) file vaddrs, so init_array entries point to low
    // memory and the first DT_INIT_ARRAY call crashes with
    // "decode error at pc=0x... inst=0x00000000".
    uint64_t    relr_addr  = 0;   // DT_RELR (absolute)
    uint64_t    relr_size  = 0;   // DT_RELRSZ (bytes)
    // and sizes. The linker invokes these after relocations are applied
    // (C++ static constructors, glibc hooks, etc.).
    uint64_t    init_addr       = 0;  // DT_INIT (legacy _init() function)
    uint64_t    fini_addr       = 0;  // DT_FINI (legacy _fini() function)
    uint64_t    init_array_addr = 0;  // DT_INIT_ARRAY (array of function ptrs)
    uint64_t    init_array_size = 0;  // DT_INIT_ARRAYSZ (bytes; count = size/8)
    uint64_t    fini_array_addr = 0;  // DT_FINI_ARRAY
    uint64_t    fini_array_size = 0;  // DT_FINI_ARRAYSZ
    // DT_VERSYM  = address of the .gnu.version section (uint16_t per symbol,
    //              index into the version definition/needed tables).
    // DT_VERDEF  = address of the .gnu.version_d section (this object's
    //              version definitions — what versions THIS lib exports).
    // DT_VERNEED = address of the .gnu.version_r section (what versions
    //              this object NEEDS from its dependencies).
    uint64_t    versym_addr  = 0;
    uint64_t    verdef_addr  = 0;
    uint64_t    verdef_num   = 0;
    uint64_t    verneed_addr = 0;
    uint64_t    verneed_num  = 0;
    bool        is_main = false;  // main binary vs shared lib
    // Refcount for dlopen'd libraries. Starts at 0 for libraries loaded
    // during link(). Bumped to 1 on first dlopen, incremented on re-dlopen,
    // decremented on dlclose. When it reaches 0, DT_FINI_ARRAY runs.
    uint32_t    refcount = 0;
    // Total mapped size (highest PT_LOAD vaddr+memsz, page-aligned).
    // Used by find_object_by_addr to check if an address falls within
    // this object's mapping.
    uint64_t    map_size = 0;
    // Absolute guest address of this object's PT_GNU_EH_FRAME
    // (.eh_frame_hdr), or 0 if none. Populated by parse_eh_frame() and
    // returned to the guest via the _dl_find_object shim (syscall
    // 0x1008) so libgcc's _Unwind_Find_FDE can find FDEs for C++
    // exceptions. Without it every throw/catch aborts.
    uint64_t    eh_frame_hdr_addr = 0;
    // TLS info.
    TlsSegment tls;
    uint64_t   tls_mod_id   = 0;  // 1-based module ID (0 = no TLS)
    int64_t    tls_tp_offset = 0; // offset from TPIDR_EL0 to this block
                                  // (negative: block is below TP)
    // static TLS block. Used to translate .tdata relocations to the
    // TLS block copy. Without this, RELATIVE relocations targeting
    // .tdata (e.g., glibc's _nl_global_locale pointer in .tdata)
    // were applied to the original .tdata location but NOT the TLS
    // block copy, so the TLS block had pre-relocation (wrong) values.
    uint64_t   tls_block_offset = 0;
};
class DynamicLinker {
public:
    DynamicLinker(Memory& mem) : mem_(mem) {}
    // Load and relocate a dynamically-linked binary. The main binary's
    // PT_LOAD segments must already be mapped; `main_data` is the raw
    // ELF file bytes (used to parse the dynamic section and relocations
    // before they're relocated). `main_base` is the load bias (0 for
    // non-PIE executables; the actual base for PIEs).
    //
    // Returns true on success. On failure, sets `error_` and returns
    // false (caller should fall back to the PT_INTERP-only path or
    // fail the load).
    bool link(CPU& cpu,
              const std::vector<uint8_t>& main_data,
              uint64_t main_base,
              const std::string& main_path,
              const std::string& interp_path = "");
    // Look up a symbol by name across all loaded objects. Returns the
    // absolute address, or 0 if not found.
    uint64_t resolve_symbol(const std::string& name) const;
    // Guest address of libc's __libc_single_threaded BSS word (0 if libc
    // doesn't export it, e.g. musl). link() writes 1 into it so glibc's
    // single-threaded fast paths (e.g. __cxa_guard_acquire, which throws
    // std::__throw_recursive_init_error on recursive static init) behave
    // like a real single-threaded process. The Emulator flips it to 0
    // when the first guest thread is spawned (matching glibc's
    // pthread_create behavior).
    uint64_t libc_single_threaded_addr() const {
        return libc_single_threaded_addr_;
    }
    // Point glibc's __environ global at the environment array on the
    // initial stack. On a real Linux boot, ld.so sets __environ during
    // _dl_start_user; the native dynlink path never runs ld.so, so
    // __environ stays NULL and getenv()/environ return nothing. Call
    // after the initial stack (envp array) is built in guest memory.
    // No-op for musl (no __environ symbol) and static binaries.
    void set_guest_environ(uint64_t envp_addr);
    // resolve_plt_entry was a stub for a future "lazy PLT binding"
    // feature that was never implemented (the linker uses eager
    // binding). Removed as dead code — the dynamic linker
    // resolves all JUMP_SLOT relocations during link(), not on first
    // call. If you need lazy binding in the future, re-add this with
    // a real implementation that tracks the GOT-slot → symbol mapping.
    // ── TLS ────────────────────────────────────────────────────────
    // Total size of the static TLS block across all loaded objects.
    // With variant-I layout: total = lib_size + tcb_size + main_memsz.
    uint64_t static_tls_size() const { return static_tls_size_; }
    uint64_t static_tls_base() const { return static_tls_base_; }
    // The thread pointer (TPIDR_EL0) for the main thread.
    // Variant-I (glibc): TP = static_tls_base_ + lib_size (points to TCB header).
    //   Main exe TLS is at TP + tcb_size (positive offset).
    //   Lib TLS is at TP - lib_size (negative offset).
    // Variant-II (musl): TP = static_tls_base_ + static_tls_size_ (end of block).
    //   All TLS is at negative TP offsets.
    uint64_t thread_pointer() const {
        if (is_musl_) return static_tls_base_ + static_tls_size_;
        return static_tls_base_ + lib_tls_size_;
    }
    // Size of the lib TLS block (negative TP region).
    uint64_t lib_tls_size() const { return lib_tls_size_; }
    // Size of the TCB header (tcbhead_t), rounded up to main exe alignment.
    uint64_t tcb_size() const { return tcb_size_; }
    // ── ld-linux shim base address ─────────────────────────────────
    // The shim's data page (_rtld_global_ro etc.) is at shim_base_,
    // and the code page (function stubs) is at shim_base_ + 4096.
    // Used by the _dl_allocate_tls syscall handler to populate
    // GLRO(dl_tls_static_size) so glibc's _dl_allocate_tls_storage
    // allocates the correct amount.
    uint64_t shim_base() const { return shim_base_; }
    // Get the module ID for an object by name (0 if not found).
    uint64_t tls_mod_id(const std::string& name) const;
    // Get the TP-offset for a module ID (negative: below TP).
    int64_t tls_tp_offset(uint64_t mod_id) const;
    const std::vector<LoadedObject>& objects() const { return objects_; }
    const std::string& error() const { return error_; }
    // Loader-wide lock accessor (see loader_mu_ below). External code
    // that iterates objects() or reads TLS state directly (e.g. the
    // syscall 0x1001 _dl_allocate_tls handler in misc.cpp) must hold
    // this lock around the whole iteration to avoid racing with a
    // concurrent dlopen() from another guest thread.
    std::recursive_mutex& loader_lock() const { return loader_mu_; }
    // Load a shared library at runtime (dlopen support).
    // path: absolute or relative path to the .so file.
    // Returns: base address (>0) on success, 0 on failure.
    // If the library is already loaded (by path or soname), returns the
    // existing handle and bumps the refcount (matching glibc's
    // _dl_open fast path).
    //
    // `cpu` is the guest thread performing the dlopen. It is passed
    // down to the ifunc/init guest-call callbacks so they can borrow
    // the CALLING thread's CPU (parked inside the syscall handler)
    // instead of racing over main_cpu_. The startup path (link()) passes
    // main_cpu_, which is idle at load time.
    uint64_t load_library(CPU& cpu, const std::string& path);
    // Decrement the refcount of a dlopen'd library. When the refcount
    // reaches 0, the library's DT_FINI_ARRAY is invoked (in reverse
    // order) and the library is marked for unload. The memory is NOT
    // actually unmapped (glibc doesn't either, for safety — the link_map
    // stays in the list but l_direct_opencount=0).
    // Returns 0 on success (dlclose convention), -1 on error.
    // `cpu` is the calling guest thread (see load_library).
    int close_library(CPU& cpu, uint64_t handle);
    // Resolve a symbol within a specific library's scope (dlsym with a
    // handle). Searches the library's own .dynsym first, then its
    // DT_NEEDED dependencies. Returns 0 if not found.
    uint64_t resolve_symbol_in(uint64_t handle, const std::string& name);
    // Find the loaded object that contains `addr` in its [base, base+size)
    // range. Returns nullptr if no object contains the address. Used by
    // dladdr and _dl_find_dso_for_object.
    const LoadedObject* find_object_by_addr(uint64_t addr) const;
    // If `addr` lies within a loaded object whose `name` contains
    // `name_fragment`, returns the file offset relative to that object's
    // base (== start_pc - base_addr). Otherwise returns ~0. Used by the
    // JIT to apply module-relative quirk/quarantine ranges that must
    // survive ASLR (e.g. glibc's allocator region).
    uint64_t object_relative_offset(uint64_t addr, const std::string& name_fragment) const;
    // Fill in Dl_info for a given address (dladdr). Returns 1 on success
    // (address found in a loaded object), 0 on failure.
    //   info->dli_fname  → guest pointer to filename string
    //   info->dli_fbase  → load base of the containing object
    //   info->dli_sname  → guest pointer to nearest symbol name (or 0)
    //   info->dli_saddr  → address of nearest symbol (or 0)
    struct DlInfo {
        uint64_t dli_fname;  // guest pointer to filename
        uint64_t dli_fbase;  // load base
        uint64_t dli_sname;  // guest pointer to symbol name
        uint64_t dli_saddr;  // symbol address
    };
    int dladdr(uint64_t addr, DlInfo& info);
    // Get the last error message (dlerror). Returns a guest pointer to
    // a null-terminated error string, or 0 if no error is pending.
    // After calling this, the error is cleared (next call returns 0).
    uint64_t get_last_error();
    // Set the last error message (used by dlopen/dlsym failure paths).
    void set_last_error(const std::string& msg);
    // Iterate over all loaded objects and call the callback for each
    // (dl_iterate_phdr). The callback receives a guest pointer to a
    // dl_phdr_info struct and the user's data pointer. Returns the
    // sum of callback return values (matching glibc semantics).
    // Iterate over all loaded objects and call the callback for each
    // (dl_iterate_phdr). The callback receives a guest pointer to a
    // dl_phdr_info struct and the user's data pointer. Returns the
    // sum of callback return values (matching glibc semantics).
    // The callback is a guest function pointer — we call it via the
    // init_runner_ mechanism.
    // `cpu` is the calling guest thread (see load_library) — the
    // callback borrows it so a non-main thread's dl_iterate_phdr runs
    // its callback on its own CPU instead of racing on main_cpu_.
    int iterate_phdr(CPU& cpu, uint64_t callback_ptr, uint64_t data_ptr);
    // ── ifunc resolver callback ────────────────────────────────────
    // BUGFIX: the old IRELATIVE handler just stored `base + A` (the
    // resolver ADDRESS) instead of calling the resolver to get the
    // actual function pointer. This silently corrupted any program
    // using ifuncs (e.g., glibc memcpy variants selected at load time
    // based on CPU features). Now the Emulator registers a callback
    // that runs the resolver function in a scratch CPU and returns X0.
    // The callback returns 0 on failure (which leaves the GOT slot 0,
    // and the guest will crash on the first call — visible, not silent).
    void set_ifunc_resolver(std::function<uint64_t(CPU&, uint64_t)> cb) {
        ifunc_resolver_ = std::move(cb);
    }
    // ── Constructor/init callback ────────────────────────
    // After relocations are applied, the linker must invoke DT_INIT and
    // DT_INIT_ARRAY for each loaded object (in dependency order: libs
    // first, main binary last). These run C++ static constructors,
    // glibc __libc_start_main hooks, etc. Without them, every C++ game
    // runs with uninitialized globals (vtables, std::mutex, std::string).
    //
    // The callback runs a guest function at `addr` and returns when the
    // function returns. The Emulator implements this by borrowing the
    // main CPU (saving/restoring architectural state) and stepping until
    // RET to a sentinel LR.
    void set_init_runner(std::function<void(CPU&, uint64_t)> cb) {
        init_runner_ = std::move(cb);
    }
    // ── Argument-passing guest-call callback ──────────────────────────
    // Like init_runner_, but passes up to 3 arguments in x0/x1/x2 and
    // returns x0. Used by dl_iterate_phdr (callback + data), dlopen init
    // arrays (argc/argv/env), and other guest calls that need arguments.
    //
    // The callback runs a guest function at `addr` with:
    //   x0 = arg0, x1 = arg1, x2 = arg2
    // and returns the value of x0 when the function RETs.
    // If the function doesn't return (infinite loop), the callback
    // aborts after a step limit and returns 0.
    void set_guest_call_args(std::function<uint64_t(CPU&, uint64_t, uint64_t, uint64_t, uint64_t)> cb) {
        guest_call_args_ = std::move(cb);
    }
    // ── Graphic API thunk resolver ───────────────────────
    // When `find_library()` returns empty for a graphic library soname
    // (libGL.so*, libEGL.so*, libSDL2.so*, libGLESv2.so*), the dynamic
    // linker consults the thunk resolver to populate the global symbol
    // table for that library. The callback receives the lib_soname and
    // returns a list of (symbol_name, guest_trampoline_addr) pairs.
    //
    // Using a list (instead of one-at-a-time lookups) keeps the thunk
    // as the single source of truth for its symbol inventory — the
    // dynamic linker doesn't need a hardcoded list of GL/EGL/SDL2
    // entry points that could drift out of sync with the thunk's
    // actual registrations.
    //
    // The Emulator wires FrostGraphics::thunk() into this callback
    // after creating both. When BIFROST_THUNK_GRAPHICS=1 is unset, the
    // thunk returns an empty list — the dynamic linker falls through
    // to its existing "library not found" path.
    using ThunkSymbolList = std::vector<std::pair<std::string, uint64_t>>;
    using ThunkResolver = std::function<ThunkSymbolList(const std::string&)>;
    void set_thunk_resolver(ThunkResolver cb) {
        thunk_resolver_ = std::move(cb);
    }
    // Enumerate the libraries the thunk resolver supports. Used by
    // `link()` to decide which DT_NEEDED entries to handle as synthetic
    // thunk-backed libraries instead of trying to load them from disk.
    // Returns true if `soname` is a known graphic library.
    static bool is_thunk_supported_lib_(const std::string& soname);
private:
    Memory& mem_;
    // Loader-wide lock. The loader's state (objects_, symbols_,
    // versioned_symbols_, last_error_, scratch buffers) can be touched
    // concurrently from multiple guest threads — each guest thread runs
    // on its own host thread, and dlopen/dlsym/dlclose/dladdr/
    // dl_iterate_phdr are all re-entrant from any thread. This is the
    // equivalent of glibc's _dl_load_lock. Recursive because guest
    // callbacks (dlopen DT_INIT_ARRAY, dl_iterate_phdr hooks) can
    // re-enter the loader through guest code that calls dl* again on
    // the same thread. The single-threaded startup path (link()) holds
    // it uncontended.
    mutable std::recursive_mutex loader_mu_;
    std::vector<LoadedObject> objects_;
    // Global symbol table: name → (absolute address, binding).
    // track the binding (STB_GLOBAL vs STB_WEAK) so we can implement
    // "first strong wins" instead of "last strong wins" (H6).
    struct SymEntry { uint64_t addr; uint8_t bind; };
    std::unordered_map<std::string, SymEntry> symbols_;
    // "name@version" (e.g. "memcpy@GLIBC_2.17"). When a relocation
    // requests a specific version (via .gnu.version_r), we look up
    // the versioned entry first, then fall back to the unversioned
    // `symbols_` table. This prevents wrong-version symbol selection
    // (e.g. GLIBC_2.17 memcpy vs GLIBC_2.29 memcpy with ERMS support,
    // or stat@GLIBC_2.33 returning a different struct layout than
    // stat@GLIBC_2.17).
    std::unordered_map<std::string, SymEntry> versioned_symbols_;
    std::string error_;
    // Optional ifunc resolver callback (set by Emulator before link()).
    // The first parameter is the CPU to borrow while running the
    // resolver (the calling thread for runtime dl*; main_cpu_ during
    // startup link()). Borrowing the caller's OWN parked CPU (instead
    // of always main_cpu_) fixes a race where a non-main guest thread
    // triggered a resolver/init callback that clobbered main_cpu_ while
    // the main thread was mid-execution.
    std::function<uint64_t(CPU&, uint64_t)> ifunc_resolver_;
    // Optional init runner callback (set by Emulator before link()).
    // Used to invoke DT_INIT_ARRAY entries (C++ static constructors).
    // First param is the CPU to borrow (see ifunc_resolver_).
    std::function<void(CPU&, uint64_t)> init_runner_;
    // Optional argument-passing guest-call callback (set by Emulator
    // before link()). Used by dl_iterate_phdr and dlopen init arrays.
    // First param is the CPU to borrow (see ifunc_resolver_).
    std::function<uint64_t(CPU&, uint64_t, uint64_t, uint64_t, uint64_t)> guest_call_args_;
    // Optional thunk resolver callback (set by Emulator before link()).
    // When set, graphic library DT_NEEDED entries that can't be loaded
    // from disk fall back to this resolver instead of failing.
    ThunkResolver thunk_resolver_;
    // TLS state.
    uint64_t static_tls_size_ = 0;  // total bytes (aligned)
    uint64_t static_tls_base_ = 0;  // guest VA where the block is mapped
    uint64_t lib_tls_size_ = 0;     // lib TLS size (negative TP region)
    uint64_t tcb_size_ = 0;         // TCB header size (tcbhead_t, rounded to align)
    uint64_t next_tls_mod_id_ = 1;  // 1-based; 0 reserved
    bool is_musl_ = false;          // true if linked against musl (variant-II TLS)
    uint64_t dlopen_hook_ptr_ = 0;  // dlopen hook struct addr (shim data area)
    uint64_t libc_single_threaded_addr_ = 0;  // guest VA of __libc_single_threaded
    // dlerror state. Stored as a host string; get_last_error() copies it
    // to a guest buffer and returns the guest pointer. The error is
    // cleared after get_last_error() returns it (matching glibc's
    // "dlerror returns NULL on second call" semantics).
    std::string last_error_;
    bool error_pending_ = false;
    // Guest buffer for dlerror strings. Allocated once in the shim data
    // area (offset 0x900, 256 bytes). get_last_error() writes the error
    // string here and returns the guest pointer.
    uint64_t dlerror_buf_ptr_ = 0;
    constexpr static size_t DLERROR_BUF_SIZE = 256;
    // Lazily-allocated guest buffer for dladdr's dli_fname string. For
    // dynamic binaries this reuses dlerror_buf_ptr_ (set in the shim).
    // For static binaries (no shim), we mmap a small page on first
    // dladdr() call so dli_fname is non-NULL.
    uint64_t dladdr_fname_buf_ = 0;
    // Parse the dynamic section of `data` starting at `dyn_off` (file
    // offset). Fills in the LoadedObject's symtab/strtab/jmprel/etc.
    // `base` is the load bias to convert vaddrs to absolute addresses.
    bool parse_dynamic(const std::vector<uint8_t>& data,
                       uint64_t base,
                       LoadedObject& obj);
    // Internal helper: load a library from an in-memory ELF image.
    // Shared by load_library (dlopen by path) and soname-based lookup.
    uint64_t load_library_from_data(CPU& cpu,
                                    const std::string& path,
                                    std::vector<uint8_t>& data);
    // Parse PT_TLS from program headers and record it in obj.tls.
    void parse_tls(const std::vector<uint8_t>& data, LoadedObject& obj);
    // Parse PT_GNU_EH_FRAME from program headers and record the
    // absolute .eh_frame_hdr address in obj.eh_frame_hdr_addr.
    void parse_eh_frame(const std::vector<uint8_t>& data, uint64_t base,
                        LoadedObject& obj);
    // Find a shared library by soname. Checks standard multiarch paths
    // and returns the file bytes (empty if not found).
    // object's DT_RUNPATH/DT_RPATH (semicolon-separated, $ORIGIN expanded).
    // find_library searches these BEFORE the standard multiarch paths so
    // games bundling their own libs (DT_RUNPATH=$ORIGIN/lib) find them.
    std::vector<uint8_t> find_library(const std::string& soname,
                                      std::string& found_path,
                                      const std::string& parent_runpath = "",
                                      const std::string& parent_rpath = "");
    // Load a shared library's PT_LOAD segments into guest memory at a
    // fresh base address. Records the object in `objects_` and its
    // symbols in `symbols_`. Returns the base address, or 0 on failure.
    uint64_t load_shared_library(CPU& cpu,
                                 const std::string& soname,
                                 const std::string& parent_runpath = "",
                                 const std::string& parent_rpath = "");
    // Register a synthetic LoadedObject for a graphic library that
    // couldn't be loaded from disk but is supported by the thunk
    // resolver. Populates `symbols_` with thunk-resolved addresses.
    // Returns a synthetic (non-zero) base address, or 0 if the thunk
    // resolver declined to handle this library.
    uint64_t register_thunk_library_(const std::string& soname);
    // ── Synthetic ld-linux shim ──────────────────────────
    // glibc's libc.so references many symbols that are normally
    // provided by ld-linux (the dynamic linker): _rtld_global,
    // _rtld_global_ro, _dl_argv, _dl_find_dso_for_object, etc. When
    // we use our own native dynamic linker (instead of running the
    // guest ld-linux), these symbols are undefined and the GOT slots
    // stay at 0, causing crashes when libc dereferences them.
    //
    // The shim allocates a small writable page in guest memory and
    // populates it with:
    //   - A struct rtld_global_ro with safe defaults (all zeros +
    //     a few function pointers that return 0 / do nothing).
    //   - A struct rtld_global with a few stub function pointers.
    //   - Storage for _dl_argv, __libc_enable_secure, etc.
    //
    // The shim also registers synthetic symbol table entries so
    // relocations against these symbols resolve to the shim's
    // addresses. The functions themselves are tiny ARM64 stubs that
    // return 0 (so a libc that calls _dl_find_dso_for_object gets 0
    // = "not found" instead of crashing).
    //
    // This is similar to how Android's Bionic libc provides its own
    // __libc_init instead of depending on ld-linux, except we do it
    // at the dynamic-linker level rather than the libc level.
    bool register_ld_linux_shim_();
    // Map PT_LOAD segments from `data` at `base`. Returns the highest
    // mapped address + 1 (i.e., the new end_addr). Sets `entry` to
    // the absolute entry point.
    uint64_t map_segments(const std::vector<uint8_t>& data,
                          uint64_t base, uint64_t& entry);
    // Build the global symbol table from obj's .dynsym. Only exported
    // (SHN_UNDEF == 0, st_shndx != SHN_UNDEF) symbols are added.
    void index_symbols(CPU& cpu, const LoadedObject& obj);
    // (.gnu.version, .gnu.version_d, .gnu.version_r) and populate
    // versioned_symbols_ with "name@version" keys.
    void parse_versions_(CPU& cpu, const LoadedObject& obj);
    // Resolve a symbol by name AND version. Looks up versioned_symbols_
    // first (key "name@version"), then falls back to unversioned
    // symbols_. Returns 0 if not found.
    uint64_t resolve_versioned_symbol(const std::string& name,
                                       const std::string& version) const;
    // Allocate the static TLS block and assign TP-offsets to each
    // object with a PT_TLS segment. Must be called after all libraries
    // are loaded but before relocations are applied.
    void allocate_static_tls();
    // object (libs first, main last). Runs C++ static constructors.
    // Requires init_runner_ to be set; no-ops if not.
    void run_init_arrays_(CPU& cpu);
    // ── Per-relocation helpers ─────────────────────────────────────
    // Resolve a symbol referenced by a relocation. Returns the
    // absolute address (or 0 if undefined).
    // requirements), look up the version for this symbol and use
    // resolve_versioned_symbol. This prevents wrong-version symbol
    // selection (e.g. GLIBC_2.17 stat vs GLIBC_2.33 stat with different
    // struct layouts).
    uint64_t resolve_reloc_symbol(const LoadedObject& obj, uint32_t sym_idx);
    // Compute the TP offset for an R_AARCH64_TLSDESC relocation.
    // `obj` is the module whose relocation is being applied, `sym_idx`
    // the referenced .dynsym index (0 = local TLS, offset = A), and
    // `A` the addend. For imported symbols (defined in another module)
    // this resolves the symbol to find the *defining* module and uses
    // that module's TLS block offset + the defining symbol's st_value
    // (the importing module's st_value is 0 for undefined symbols).
    // Returns the TP offset relative to TPIDR_EL0.
    int64_t tlsdesc_tp_offset(const LoadedObject& obj, uint32_t sym_idx,
                              int64_t A);
    // ── ld-linux shim state ────────────────────────────────────────
    // Guest VA of the synthetic ld-linux data page (allocated by
    // register_ld_linux_shim_()). 0 if the shim hasn't been registered.
    uint64_t shim_base_ = 0;
    // Offset of the shim's code page within shim_base_ (the ld-linux
    // synthetic shim is SHIM_SIZE bytes: data pages + one code page).
    static constexpr uint64_t SHIM_CODE_PAGE_OFF_ = 12288;
    // Offset into the shim code page of the TLSDESC resolver stub
    // (a function that returns the TP offset stored at [x0+8]).
    // Populated by register_ld_linux_shim_() and used by the
    // R_AARCH64_TLSDESC fixup so desc[0] points at a real function
    // the guest can `blr` into.
    uint32_t tlsdesc_resolver_off_ = 0;
    // Guest address of the _dl_find_object shim stub (calls syscall
    // 0x1008 which fills the glibc 2.42 dl_find_object result struct
    // from host LoadedObject state). Populated by register_ld_linux_shim_()
    // and written into the _rtld_global_ro.dl_find_object funnel slot by
    // patch_rtld_funnel_() so funnelled-glibc trampolines land in the
    // native implementation. 0 if the shim wasn't registered.
    uint64_t dl_find_object_stub_ = 0;
    // ── Pending TLS static size for post-relocation patching ──────
    // After all relocations are applied, we patch the resolved
    // _rtld_global_ro (which points to ld-linux's data section) to
    // set dl_tls_static_size, dl_tls_static_align, and dl_pagesize.
    // This is needed because ld-linux's initialization never runs,
    // so these fields are 0, causing pthread_create to assert.
    // We can't set them in the shim because overriding _rtld_global_ro
    // breaks __libc_early_init (which reads other fields from the
    // real struct at specific offsets).
    uint64_t pending_tls_static_size_ = 0;
    // ── Pending R_AARCH64_COPY relocations ───────────────
    // COPY relocations must be deferred until AFTER all other relocations
    // are applied. The COPY reads the original symbol's value (which is
    // set by the original object's RELATIVE relocations). If we apply
    // COPY during the first pass, we read pre-relocation values.
    struct PendingCopy {
        uint64_t target;              // main binary .bss/.data address
        std::string name;             // symbol name (e.g., "stdout")
        uint64_t size;                // bytes to copy (from symtab st_size)
        const LoadedObject* copy_obj; // the object containing the COPY reloc
    };
    std::vector<PendingCopy> pending_copies_;
    // Apply all pending COPY relocations. Called after all objects'
    // RELATIVE/ABS64/GLOB_DAT/JUMP_SLOT/IRELATIVE relocations are done.
    void apply_pending_copies_();
    // produces these by default; without them, libc's .init_array / .got /
    // .data.rel.ro relative pointers never get fixed up. RELR is a stream
    // of uint64_t words encoding R_AARCH64_RELATIVE relocations in a
    // bitmap format (1 bit per 8-byte slot, with periodic 2-word entries
    // that reset the relocation address for sparse regions). See:
    // https://maskray.me/blog/2021-10-31-relative-relocations-and-relr
    void apply_relr_relocations_(const LoadedObject& obj,
                                 uint64_t relr_addr, uint64_t relr_size);
    // Patch the resolved _rtld_global_ro to set dl_pagesize,
    // dl_tls_static_size, and dl_tls_static_align. Called after all
    // relocations (so _rtld_global_ro is resolved to ld-linux's data)
    // but before __libc_early_init (which reads dl_pagesize).
    void patch_rtld_global_ro_();
    // Funnelled glibc (2.42+): libc's _dl_find_object is a trampoline that
    // jumps through GLRO(dl_find_object) in the REAL _rtld_global_ro
    // (written by ld-linux's _dl_start_final, which the native dynlink
    // never runs). Detect the slot from the trampoline and fill in the
    // real function pointers from ld-linux. No-op for non-funnelled glibc.
    void patch_rtld_funnel_();
    void set_libc_single_threaded_();
    // Dynamically detect the offsets of dl_tls_static_size and
    // dl_tls_static_align within struct rtld_global_ro by disassembling
    // __libc_early_init. Returns true on success, filling out the
    // offsets; false if detection fails (caller falls back to spray).
    bool detect_tls_field_offsets_(uint32_t& out_size_off,
                                    uint32_t& out_align_off);
    // Detect _dl_open_hook / dlfcn_hook offset within _rtld_global_ro by
    // disassembling dlopen@@GLIBC_2.34 (or __libc_dlopen_mode). glibc
    // moved this field from +368 to +376 between 2.40 and 2.43.
    bool detect_dlopen_hook_offset_(uint32_t& out_hook_off);
    // _rtld_global (the read-write rtld global, NOT _rtld_global_ro).
    // glibc's pthread_create -> allocate_stack walks the _dl_stack_cache
    // list (a circular doubly-linked list_t) looking for a reusable
    // thread stack. An empty list must have head->next == head->prev ==
    // &head (INIT_LIST_HEAD). Normally ld-linux's
    // __pthread_initialize_minimal_internal does this during startup,
    // but since we use our own dynamic linker, those list heads stay
    // zeroed (.bss). bifrost-emu returns 0 for unmapped reads instead
    // of faulting, so the loop follows NULL->next forever -> infinite
    // spin (no futex, no syscall — a pure CPU loop). This initializes
    // the three list heads (_dl_stack_used, _dl_stack_user,
    // _dl_stack_cache) so the first pthread_create proceeds to allocate
    // a fresh stack instead of spinning. Offsets determined from glibc
    // 2.36 (Arm GNU 13.2) libc.so.6 disassembly of pthread_create and
    // the _thread_db_rtld_global__dl_stack_* descriptors.
    void init_nptl_stack_lists_();
    // If `target` falls within obj's PT_TLS (.tdata) segment, also
    // store `value` at the corresponding offset in the static TLS
    // block. This is needed because allocate_static_tls() copies
    // .tdata to the TLS block BEFORE relocations are applied, so the
    // TLS block has pre-relocation values. Without this mirror,
    // TLS variables containing pointers (e.g., glibc's
    // _nl_global_locale pointer in .tdata) have wrong values,
    // causing locale/stdio functions to dereference NULL.
    void apply_tls_mirror_(const LoadedObject& obj,
                           uint64_t target, uint64_t value);
};
} // namespace arm64emu
