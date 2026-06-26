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

    // Resolve a single PLT entry lazily: look up the symbol named in
    // the JUMP_SLOT relocation at `plt_entry_idx` and write its address
    // into the GOT. Returns the resolved function address (so the
    // guest can jump to it directly).
    uint64_t resolve_plt_entry(uint64_t got_slot_addr);

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

private:
    Memory& mem_;
    std::vector<LoadedObject> objects_;
    // Global symbol table: name → absolute address.
    std::unordered_map<std::string, uint64_t> symbols_;
    std::string error_;

    // TLS state.
    uint64_t static_tls_size_ = 0;  // total bytes (aligned)
    uint64_t static_tls_base_ = 0;  // guest VA where the block is mapped
    uint64_t next_tls_mod_id_ = 1;  // 1-based; 0 reserved

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
