// frontend/dynamic_linker.h — Dynamic linking support for bifrost-emu.
//
// Implements DT_NEEDED processing, GOT/PLT relocation, and symbol
// resolution for dynamically-linked AArch64 ELF binaries. The dynamic
// linker is invoked by Emulator::load_elf_file() after the main
// binary's PT_LOAD segments have been mapped.
//
// This is a substantial improvement over the previous "load PT_INTERP
// only" approach. We now:
//   1. Parse the main binary's PT_DYNAMIC segment.
//   2. For each DT_NEEDED entry, locate and load the shared library
//      (libc.so, libm.so, etc.) from common host multiarch paths.
//   3. Build a global symbol table from all loaded objects' .dynsym
//      sections, so relocations can resolve symbols across objects.
//   4. Apply R_AARCH64_RELATIVE, R_AARCH64_GLOB_DAT, R_AARCH64_ABS64,
//      R_AARCH64_JUMP_SLOT, and R_AARCH64_IRELATIVE relocations.
//   5. Set up the GOT[1] (link map) and GOT[2] (resolver) entries so
//      lazy PLT binding works (the resolver is a guest-side stub that
//      calls back into the emulator via a syscall).
//
// Limitations (documented for the user):
//   - TLS relocations (TLS_DTPMOD, TLS_TPREL, TLSDESC) are not applied;
//     TLS-using programs will see zero-initialized TLS variables. This
//     is sufficient for most musl/glibc programs that don't use __thread
//     with non-trivial initialization.
//   - R_AARCH64_COPY is not implemented (it's rare on AArch64; the ABI
//     prefers GLOB_DAT through GOT for shared data).
//   - Symbol versioning (GNU symbol versioning / .gnu.version) is not
//     parsed; we always pick the default version of each symbol.
//   - Lazy PLT binding is supported via a guest-side resolver stub
//     that traps to the emulator (syscall number 0x10001, our private
//     "resolve_plt" call). Eager binding is the default.
//
// References:
//   - ARM IHI 0056B (AArch64 ELF ABI): https://github.com/ARM-software/abi-aa
//   - ELF gABI: https://refspecs.linuxbase.org/elf/gabi4+/ch5.dynamic.html
//   - Linux kernel: fs/binfmt_elf.c, load_elf_binary()
#pragma once

#include "bifrost/types.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace arm64emu {

class Memory;

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

    const std::vector<LoadedObject>& objects() const { return objects_; }
    const std::string& error() const { return error_; }

private:
    Memory& mem_;
    std::vector<LoadedObject> objects_;
    // Global symbol table: name → (object index, symbol address).
    std::unordered_map<std::string, uint64_t> symbols_;
    std::string error_;

    // Parse the dynamic section of `data` starting at `dyn_off` (file
    // offset). Fills in the LoadedObject's symtab/strtab/jmprel/etc.
    // `base` is the load bias to convert vaddrs to absolute addresses.
    bool parse_dynamic(const std::vector<uint8_t>& data,
                       uint64_t base,
                       LoadedObject& obj);

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
};

} // namespace arm64emu
