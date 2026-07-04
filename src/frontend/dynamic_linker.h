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
//   - R_AARCH64_COPY is not implemented (rare on AArch64).
//   - Symbol versioning (.gnu.version) is not parsed.
//   - dlopen() of a TLS-using library after the static TLS block is
//     sized is not supported (would require dynamic TLS allocation).
//   - TLSDESC uses a simple inline resolver (no PLT call).
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
#include <string>
#include <unordered_map>
#include <vector>

namespace arm64emu {

class Memory;

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
    uint64_t    base_addr = 0;    // load base (0 for main binary if not PIE)
    uint64_t    entry     = 0;    // entry point (absolute)
    uint64_t    dyn_addr  = 0;    // PT_DYNAMIC vaddr (absolute)
    uint64_t    symtab_addr = 0;  // DT_SYMTAB (absolute)
    uint64_t    strtab_addr = 0;  // DT_STRTAB (absolute)
    uint64_t    symtab_count = 0; // number of symbols in .dynsym
    uint64_t    jmprel_addr = 0;  // DT_JMPREL (absolute)
    uint64_t    jmprel_size = 0;  // DT_PLTRELSZ
    bool        is_main = false;  // main binary vs shared lib

    // TLS info.
    TlsSegment tls;
    uint64_t   tls_mod_id   = 0;  // 1-based module ID (0 = no TLS)
    int64_t    tls_tp_offset = 0; // offset from TPIDR_EL0 to this block
                                  // (negative: block is below TP)
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
    bool link(const std::vector<uint8_t>& main_data,
              uint64_t main_base,
              const std::string& main_path);

    // Look up a symbol by name across all loaded objects. Returns the
    // absolute address, or 0 if not found.
    uint64_t resolve_symbol(const std::string& name) const;

    // resolve_plt_entry was a stub for a future "lazy PLT binding"
    // feature that was never implemented (the linker uses eager
    // binding). Removed in Turn 37 as dead code — the dynamic linker
    // resolves all JUMP_SLOT relocations during link(), not on first
    // call. If you need lazy binding in the future, re-add this with
    // a real implementation that tracks the GOT-slot → symbol mapping.

    // ── TLS ────────────────────────────────────────────────────────
    // Total size of the static TLS block across all loaded objects
    // (sum of aligned memsz). The TPIDR_EL0 register should point to
    // the byte AFTER this block (i.e., TP = tls_base + total_size).
    uint64_t static_tls_size() const { return static_tls_size_; }
    uint64_t static_tls_base() const { return static_tls_base_; }
    void set_static_tls_base(uint64_t b) { static_tls_base_ = b; }

    // Get the module ID for an object by name (0 if not found).
    uint64_t tls_mod_id(const std::string& name) const;

    // Get the TP-offset for a module ID (negative: below TP).
    int64_t tls_tp_offset(uint64_t mod_id) const;

    const std::vector<LoadedObject>& objects() const { return objects_; }
    const std::string& error() const { return error_; }

    // ── ifunc resolver callback ────────────────────────────────────
    // BUGFIX: the old IRELATIVE handler just stored `base + A` (the
    // resolver ADDRESS) instead of calling the resolver to get the
    // actual function pointer. This silently corrupted any program
    // using ifuncs (e.g., glibc memcpy variants selected at load time
    // based on CPU features). Now the Emulator registers a callback
    // that runs the resolver function in a scratch CPU and returns X0.
    // The callback returns 0 on failure (which leaves the GOT slot 0,
    // and the guest will crash on the first call — visible, not silent).
    void set_ifunc_resolver(std::function<uint64_t(uint64_t)> cb) {
        ifunc_resolver_ = std::move(cb);
    }

    // ── Graphic API thunk resolver (Turn 37) ───────────────────────
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
    std::vector<LoadedObject> objects_;
    // Global symbol table: name → absolute address.
    std::unordered_map<std::string, uint64_t> symbols_;
    std::string error_;
    // Optional ifunc resolver callback (set by Emulator before link()).
    std::function<uint64_t(uint64_t)> ifunc_resolver_;
    // Optional thunk resolver callback (set by Emulator before link()).
    // When set, graphic library DT_NEEDED entries that can't be loaded
    // from disk fall back to this resolver instead of failing.
    ThunkResolver thunk_resolver_;

    // TLS state.
    uint64_t static_tls_size_ = 0;  // total bytes (aligned)
    uint64_t static_tls_base_ = 0;  // guest VA where the block is mapped
    uint64_t next_tls_mod_id_ = 1;  // 1-based; 0 reserved

    // Next base address for load_shared_library(). Was a function-local
    // static in Turn 15; promoted to a member in Turn 37 so multiple
    // DynamicLinker instances don't share the same allocator (latent
    // bug if the Emulator ever creates two linkers, e.g., for fork()
    // with separate Memory). 0 = not yet initialized; first call sets
    // it to 0x5000000000.
    uint64_t next_lib_base_ = 0;

    // Parse the dynamic section of `data` starting at `dyn_off` (file
    // offset). Fills in the LoadedObject's symtab/strtab/jmprel/etc.
    // `base` is the load bias to convert vaddrs to absolute addresses.
    bool parse_dynamic(const std::vector<uint8_t>& data,
                       uint64_t base,
                       LoadedObject& obj);

    // Parse PT_TLS from program headers and record it in obj.tls.
    void parse_tls(const std::vector<uint8_t>& data, LoadedObject& obj);

    // Apply all relocations from `data`'s SHT_RELA sections to obj.
    bool apply_relocations(const std::vector<uint8_t>& data,
                           LoadedObject& obj);

    // Find a shared library by soname. Checks standard multiarch paths
    // and returns the file bytes (empty if not found).
    std::vector<uint8_t> find_library(const std::string& soname,
                                      std::string& found_path);

    // Load a shared library's PT_LOAD segments into guest memory at a
    // fresh base address. Records the object in `objects_` and its
    // symbols in `symbols_`. Returns the base address, or 0 on failure.
    uint64_t load_shared_library(const std::string& soname);

    // Register a synthetic LoadedObject for a graphic library that
    // couldn't be loaded from disk but is supported by the thunk
    // resolver. Populates `symbols_` with thunk-resolved addresses.
    // Returns a synthetic (non-zero) base address, or 0 if the thunk
    // resolver declined to handle this library.
    uint64_t register_thunk_library_(const std::string& soname);

    // Map PT_LOAD segments from `data` at `base`. Returns the highest
    // mapped address + 1 (i.e., the new end_addr). Sets `entry` to
    // the absolute entry point.
    uint64_t map_segments(const std::vector<uint8_t>& data,
                          uint64_t base, uint64_t& entry);

    // Build the global symbol table from obj's .dynsym. Only exported
    // (SHN_UNDEF == 0, st_shndx != SHN_UNDEF) symbols are added.
    void index_symbols(const LoadedObject& obj);

    // Allocate the static TLS block and assign TP-offsets to each
    // object with a PT_TLS segment. Must be called after all libraries
    // are loaded but before relocations are applied.
    void allocate_static_tls();

    // ── Per-relocation helpers ─────────────────────────────────────
    // Resolve a symbol referenced by a relocation. Returns the
    // absolute address (or 0 if undefined).
    uint64_t resolve_reloc_symbol(const LoadedObject& obj, uint32_t sym_idx);
};

} // namespace arm64emu
