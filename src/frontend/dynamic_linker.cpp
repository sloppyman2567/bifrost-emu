// frontend/dynamic_linker.cpp — Dynamic linking support for bifrost-emu.
//
// See dynamic_linker.h for the design overview.
//
// The relocations applied here match the AArch64 ELF ABI (ARM IHI 0056B):
//   R_AARCH64_ABS64      (257)  : *(addr) = S + A
//   R_AARCH64_GLOB_DAT   (1025) : *(addr) = S + A
//   R_AARCH64_JUMP_SLOT  (1026) : *(addr) = S + A   (eager; PLT stub left
//                                  in place but resolved up-front)
//   R_AARCH64_RELATIVE   (1027) : *(addr) = Delta + A   (Delta = base)
//   R_AARCH64_TLS_DTPMOD (1028) : *(addr) = module id (static TLS)
//   R_AARCH64_TLS_DTPREL (1029) : *(addr) = TP-offset within module
//   R_AARCH64_TLS_TPREL  (1030) : *(addr) = TP-offset (Initial-Exec)
//   R_AARCH64_TLSDESC    (1031) : inline static descriptor: desc[0]=0
//                                  (no resolver), desc[1]=TP-offset
//   R_AARCH64_IRELATIVE  (1032) : *(addr) = Indirect(Delta + A)
//                                  — calls the ifunc resolver at Delta + A
//                                    and stores its return value.
//   R_AARCH64_COPY       (1024) : deferred second pass — copies the
//                                  defining object's symbol bytes into
//                                  the main binary's GOT slot after all
//                                  other relocations are applied.
//
// References:
//   - https://github.com/ARM-software/abi-aa/blob/main/aaelf64/aaelf64.rst
//   - Linux kernel: arch/arm64/kernel/module-plts.c, arch/arm64/kernel/module.c
#include "frontend/dynamic_linker.h"
#include "core/memory.h"
#include "bifrost/types.hpp"
#include "bifrost/version.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
namespace arm64emu {
// ── Trace flag cache ───────────────────────────────────────────────────
// getenv() is not cheap (it scans environ linearly). On the dynamic
// linker hot path (resolve_symbol is called for every PLT relocation),
// calling getenv per-lookup adds measurable overhead. Cache the result
// in a function-local static — initialized once on first call.
bool dynlink_trace_enabled() {
    static bool enabled = (getenv("BIFROST_DYNLINK_TRACE") != nullptr);
    return enabled;
}
bool ifunc_trace_enabled() {
    static bool enabled = (getenv("BIFROST_IFUNC_TRACE") != nullptr);
    return enabled;
}
// ── ELF dynamic tag constants ──────────────────────────────────────────
// From elf.h (we hardcode to avoid pulling in the host's elf.h, which
// may not have all AArch64-specific tags).
namespace {
constexpr int DT_NULL_      = 0;
constexpr int DT_NEEDED_    = 1;
constexpr int DT_PLTRELSZ_  = 2;
constexpr int DT_PLTGOT_    = 3;
constexpr int DT_HASH_      = 4;
constexpr int DT_STRTAB_    = 5;
constexpr int DT_SYMTAB_    = 6;
constexpr int DT_RELA_      = 7;
constexpr int DT_RELASZ_    = 8;
constexpr int DT_RELAENT_   = 9;
constexpr int DT_STRSZ_     = 10;
constexpr int DT_SYMENT_    = 11;
constexpr int DT_INIT_      = 12;
constexpr int DT_FINI_      = 13;
constexpr int DT_SONAME_    = 14;
constexpr int DT_RPATH_     = 15;
constexpr int DT_SYMBOLIC_  = 16;
constexpr int DT_REL_       = 17;
constexpr int DT_RELSZ_     = 18;
constexpr int DT_RELENT_    = 19;
constexpr int DT_PLTREL_    = 20;
constexpr int DT_DEBUG_     = 21;
constexpr int DT_TEXTREL_   = 22;
constexpr int DT_JMPREL_    = 23;
constexpr int DT_BIND_NOW_  = 24;
constexpr int DT_INIT_ARRAY_    = 25;
constexpr int DT_FINI_ARRAY_    = 26;
constexpr int DT_INIT_ARRAYSZ_  = 27;
constexpr int DT_FINI_ARRAYSZ_  = 28;
constexpr int DT_RUNPATH_   = 29;
constexpr int DT_FLAGS_     = 30;
// DT_RELR / DT_RELRSZ / DT_RELRENT — compact relative relocations.
// Added in glibc 2.36+ and produced by default with binutils 2.38+ when
// linking against glibc 2.36+ (so glibc 2.40 ships .relr.dyn in libc.so.6).
// Without DT_RELR support, the relative relocations that fix up libc's
// internal pointers (.init_array, .data.rel.ro, .got) never fire, so
// init_array entries point to vaddr 0 → "decode error at pc=0x0
// inst=0x00000000" on the first constructor call.
//
// Encoding (per glibc's elf_machine_relr in dl-machine.h):
//   Each entry is uint64_t. Bit 0 is the flag.
//   - bit 0 == 0 (address entry): the entry value IS the relocation vaddr.
//     Apply R_AARCH64_RELATIVE there, then advance reloc_addr to vaddr + 8.
//   - bit 0 == 1 (bitmap entry): bits 1..63 (63 bits) are a bitmap.
//     Bit i (i=1..63) → reloc_addr + (i-1)*8. After processing,
//     advance reloc_addr by 63*8 (the bitmap covers 63 slots, and the
//     address entry's slot was already advanced past by +8).
//   The addend is the existing value at *target (the file vaddr).
//   R_AARCH64_RELATIVE: *(addr) = base + addend.
constexpr int DT_RELR_      = 36;
constexpr int DT_RELRSZ_    = 35;
constexpr int DT_RELRENT_   = 37;
constexpr int DT_VERSYM_    = 0x6FFFFFF0;
constexpr int DT_VERDEF_    = 0x6FFFFFFC;
constexpr int DT_VERDEFNUM_ = 0x6FFFFFFD;
constexpr int DT_VERNEED_   = 0x6FFFFFFE;
constexpr int DT_VERNEEDNUM_= 0x6FFFFFFF;
// AArch64 relocation types (ELF64 codes), per ARM IHI 0056B.
constexpr uint32_t R_AARCH64_ABS64_         = 257;
constexpr uint32_t R_AARCH64_COPY_          = 1024;  // R_AARCH64_COPY
constexpr uint32_t R_AARCH64_GLOB_DAT_      = 1025;
constexpr uint32_t R_AARCH64_JUMP_SLOT_     = 1026;
constexpr uint32_t R_AARCH64_RELATIVE_      = 1027;
constexpr uint32_t R_AARCH64_TLS_DTPMOD_    = 1028;  // TLS module ID
constexpr uint32_t R_AARCH64_TLS_DTPREL_    = 1029;  // TLS offset within module
constexpr uint32_t R_AARCH64_TLS_TPREL_     = 1030;  // TLS TP-relative offset
constexpr uint32_t R_AARCH64_TLSDESC_       = 1031;  // TLS descriptor
constexpr uint32_t R_AARCH64_IRELATIVE_     = 1032;
// ELF64 section header types.
constexpr uint32_t SHT_RELA_ = 4;
// ELF64 symbol table entry (24 bytes).
struct Elf64_Sym {
    uint32_t st_name;   // offset into strtab
    uint8_t  st_info;   // type + binding
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
};
// ELF64 dynamic section entry (16 bytes).
struct Elf64_Dyn {
    int64_t  d_tag;
    uint64_t d_val;     // also d_ptr
};
// ELF64 RELA relocation entry (24 bytes).
struct Elf64_Rela {
    uint64_t r_offset;
    uint64_t r_info;    // sym << 32 | type
    int64_t  r_addend;
};
uint32_t ELF64_R_SYM_(uint64_t info)  { return info >> 32; }
uint32_t ELF64_R_TYPE_(uint64_t info) { return info & 0xFFFFFFFF; }
// Symbol binding/type extractors.
uint8_t ST_BIND_(uint8_t info)  { return info >> 4; }
constexpr uint8_t STB_GLOBAL_= 1;
constexpr uint8_t STB_WEAK_  = 2;
constexpr uint16_t SHN_UNDEF_ = 0;
// ── Helpers ────────────────────────────────────────────────────────────
// Read a string from guest memory at `addr` (NUL-terminated).
std::string read_guest_cstr(Memory& mem, uint64_t addr) {
    std::string out;
    if (addr == 0) return out;
    out.reserve(64);  // most symbol names are < 64 chars
    try {
        char buf[256];
        uint64_t p = addr;
        while (true) {
            mem.read(p, buf, sizeof(buf));
            for (size_t i = 0; i < sizeof(buf); i++) {
                if (buf[i] == 0) return out;
                out.push_back(buf[i]);
            }
            p += sizeof(buf);
            if (out.size() > 4096) break;  // sanity limit
        }
    } catch (...) {}
    return out;
}
} // namespace
// ── DynamicLinker::link ────────────────────────────────────────────────
bool DynamicLinker::link(const std::vector<uint8_t>& main_data,
                         uint64_t main_base,
                         const std::string& main_path,
                         const std::string& interp_path) {
    objects_.clear();
    symbols_.clear();
    versioned_symbols_.clear();
    error_.clear();
    // Detect musl vs glibc from the interpreter path.
    // musl: /lib/ld-musl-aarch64.so.1
    // glibc: /lib/ld-linux-aarch64.so.1
    // This determines the TLS layout: glibc uses variant-I (main TLS at
    // positive TP offsets), musl uses variant-II (all TLS at negative TP).
    is_musl_ = (interp_path.find("musl") != std::string::npos);
    if (dynlink_trace_enabled()) {
        fprintf(stderr, "[dynlink] interp='%s' → %s TLS layout\n",
                interp_path.c_str(), is_musl_ ? "musl (variant-II)" : "glibc (variant-I)");
    }
    // Index the main binary.
    LoadedObject main_obj;
    main_obj.name = main_path;
    main_obj.base_addr = main_base;
    main_obj.is_main = true;
    if (!parse_dynamic(main_data, main_base, main_obj)) {
        return false;
    }
    parse_tls(main_data, main_obj);
    objects_.push_back(std::move(main_obj));
    index_symbols(objects_.back());
    parse_versions_(objects_.back());
    // Recursively load DT_NEEDED libraries. We use a worklist to handle
    // transitive dependencies (libc → ld-musl, libm → libc, etc.).
    std::vector<size_t> worklist = {0};
    size_t max_libs = 32;  // sanity limit to prevent infinite loops
    while (!worklist.empty() && objects_.size() < max_libs) {
        size_t idx = worklist.back();
        worklist.pop_back();
        if (idx >= objects_.size()) continue;
        // Re-scan the dynamic section of objects_[idx] for DT_NEEDED.
        // We have to re-read from memory because the dynamic section
        // was relocated in place.
        if (objects_[idx].dyn_addr == 0) continue;
        try {
            Elf64_Dyn dyn;
            for (uint64_t p = objects_[idx].dyn_addr; ; p += sizeof(dyn)) {
                mem_.read(p, &dyn, sizeof(dyn));
                if (dyn.d_tag == DT_NULL_) break;
                if (dyn.d_tag == DT_NEEDED_) {
                    // d_val is a string-table offset into the strtab of
                    // the *object that owns this dynamic section*.
                    uint64_t str_addr = objects_[idx].strtab_addr + dyn.d_val;
                    std::string soname = read_guest_cstr(mem_, str_addr);
                    if (soname.empty()) continue;
                    // Skip if already loaded.
                    // falling back to the DT_NEEDED string. Real ld.so uses
                    // DT_SONAME for dedup so a DT_NEEDED "libfoo.so.1" and
                    // a loaded library whose DT_SONAME is "libfoo.so.1.0.0"
                    // are recognized as the same library.
                    bool found = false;
                    for (const auto& o : objects_) {
                        if (o.name == soname) { found = true; break; }
                        if (!o.soname.empty() && o.soname == soname) { found = true; break; }
                    }
                    if (found) continue;
                    // DT_RUNPATH so find_library can search it for
                    // transitive deps. (DT_RUNPATH only applies to the
                    // immediate object's DT_NEEDED per the gABI; we
                    // approximate by passing the parent's runpath.)
                    uint64_t lib_base = load_shared_library(soname,
                        objects_[idx].runpath, objects_[idx].rpath);
                    if (lib_base == 0) {
                        // Library not found — not necessarily fatal
                        // (some programs dlopen at runtime). Log and
                        // continue. If BIFROST_ROOT is not set, suggest
                        // setting it up.
                        fprintf(stderr, "[%s] dynamic linker: could not "
                                "find %s (continuing)\n",
                                CODENAME, soname.c_str());
                        // First-miss hint: if BIFROST_ROOT is unset or
                        // the rootfs doesn't have libs, suggest setup.
                        static bool hinted = false;
                        if (!hinted) {
                            const char* root = getenv("BIFROST_ROOT");
                            if (!root || root[0] == '\0') {
                                hinted = true;
                                fprintf(stderr, "[%s] hint: set BIFROST_ROOT "
                                        "or use --rootfs for dynamic linking "
                                        "(run ./scripts/setup-rootfs.sh)\n",
                                        CODENAME);
                            }
                        }
                        continue;
                    }
                    worklist.push_back(objects_.size() - 1);
                }
            }
        } catch (...) {
            // Reading the dynamic section failed — skip this object.
            continue;
        }
    }
    // Now apply relocations for all loaded objects. We do this after
    // all libraries are loaded so symbol resolution can find symbols
    // in any object.
    //
    // First, allocate the static TLS block (must be done before TLS
    // relocations, which reference tls_tp_offset / tls_mod_id).
    allocate_static_tls();
    // Register the synthetic ld-linux shim. This provides definitions
    // for symbols that glibc's libc.so references from ld-linux
    // (_rtld_global_ro, _dl_argv, _dl_find_dso_for_object, etc.).
    // Without these, libc crashes during __libc_start_main when it
    // dereferences the (zero) GOT slots.
    //
    // ld-linux was loaded. Reason: production glibc builds strip ld-linux's
    // .symtab, leaving only a 40-entry .dynsym that does NOT export
    // _rtld_global, _rtld_global_ro, _dl_argv, __libc_enable_secure,
    // _dl_find_dso_for_object, etc. These symbols are referenced by
    // libc.so.6's relocations (GLOB_DAT/JUMP_SLOT) and MUST resolve to
    // valid addresses, or libc dereferences zero GOT slots and crashes.
    //
    // The shim's symbol registration uses "first-define-wins" so any
    // real ld-linux .dynsym symbol (rare but possible in debug builds)
    // takes precedence over the shim's stub.
    register_ld_linux_shim_();
    // Note: we re-apply using the original file bytes for each object,
    // since the in-memory dynamic section may have been relocated.
    // For the main binary we have `main_data`; for libs we kept their
    // bytes in a side table. To keep this simple, we read relocations
    // from the in-memory image (which is fine because RELA relocations
    // don't get overwritten in place — only the *targets* get patched).
    for (auto& obj : objects_) {
        if (obj.dyn_addr == 0) continue;
        try {
            // Find DT_RELA / DT_RELASZ / DT_JMPREL / DT_PLTRELSZ.
            // to the object's load base. For the main binary (non-PIE,
            // base=0) this is already absolute. For shared libraries
            // (PIE, base!=0) we MUST add obj.base_addr to get the
            // absolute address. The old code used d_val directly, which
            // worked for the main binary but read from wrong addresses
            // for shared libs (e.g., libc's DT_JMPREL at 0x2a880 was
            // read from low memory instead of 0x500002a880). This caused
            // libc's PLT GOT entries to never be filled → PLT stubs
            // jumped to PLT0 → jumped to GOT[2] (resolver) = 0 → crash.
            uint64_t rela_addr = 0, rela_size = 0;
            uint64_t jmprel_addr = 0, jmprel_size = 0;
            uint64_t relr_addr = 0, relr_size = 0;
            Elf64_Dyn dyn;
            for (uint64_t p = obj.dyn_addr; ; p += sizeof(dyn)) {
                mem_.read(p, &dyn, sizeof(dyn));
                if (dyn.d_tag == DT_NULL_) break;
                if (dyn.d_tag == DT_RELA_)      rela_addr = obj.base_addr + dyn.d_val;
                else if (dyn.d_tag == DT_RELASZ_)    rela_size = dyn.d_val;
                else if (dyn.d_tag == DT_JMPREL_)    jmprel_addr = obj.base_addr + dyn.d_val;
                else if (dyn.d_tag == DT_PLTRELSZ_)  jmprel_size = dyn.d_val;
                else if (dyn.d_tag == DT_RELR_)      relr_addr = obj.base_addr + dyn.d_val;
                else if (dyn.d_tag == DT_RELRSZ_)    relr_size = dyn.d_val;
            }
            if (dynlink_trace_enabled()) {
                fprintf(stderr, "[dynlink] obj '%s' base=0x%llx: "
                        "RELA=0x%llx/%llu JMPREL=0x%llx/%llu RELR=0x%llx/%llu\n",
                        obj.name.c_str(),
                        static_cast<unsigned long long>(obj.base_addr),
                        static_cast<unsigned long long>(rela_addr),
                        static_cast<unsigned long long>(rela_size),
                        static_cast<unsigned long long>(jmprel_addr),
                        static_cast<unsigned long long>(jmprel_size),
                        static_cast<unsigned long long>(relr_addr),
                        static_cast<unsigned long long>(relr_size));
            }
            // ── DT_RELR (compact relative relocations) ───────────────
            // Apply BEFORE DT_RELA: DT_RELR only encodes R_AARCH64_RELATIVE
            // (no symbol resolution), so order doesn't strictly matter,
            // but applying first makes the trace easier to read and avoids
            // any chance of a later GLOB_DAT/COPY reading a pre-RELR value.
            // Each successful RELR writes `*(addr) = base + 0` (addend is
            // implicit 0 in the RELR format).
            if (relr_addr && relr_size) {
                apply_relr_relocations_(obj, relr_addr, relr_size);
            }
            // DT_RELA entries are absolute addresses already (relocated
            // by R_AARCH64_RELATIVE during the main binary's load).
            // For non-PIE main binaries, d_val is a vaddr; for PIE/libs,
            // it's base + vaddr. We assume the dynamic linker (us) is
            // called with vaddrs already adjusted to absolute.
            if (rela_addr && rela_size) {
                for (uint64_t off = 0; off + sizeof(Elf64_Rela) <= rela_size;
                     off += sizeof(Elf64_Rela)) {
                    Elf64_Rela r;
                    mem_.read(rela_addr + off, &r, sizeof(r));
                    uint32_t type = ELF64_R_TYPE_(r.r_info);
                    uint32_t sym  = ELF64_R_SYM_(r.r_info);
                    uint64_t target = obj.base_addr + r.r_offset;
                    int64_t A = r.r_addend;
                    if (type == R_AARCH64_RELATIVE_) {
                        uint64_t value = obj.base_addr + A;
                        mem_.store<uint64_t>(target, value);
                        // the TLS block copy. See apply_tls_mirror_().
                        apply_tls_mirror_(obj, target, value);
                    } else if (type == R_AARCH64_COPY_) {
                        // R_AARCH64_COPY: defer until after all other
                        // relocations are applied. The COPY must read
                        // the ORIGINAL symbol's value AFTER the original
                        // object's RELATIVE relocations have been applied
                        // (otherwise we copy pre-relocation values).
                        // We collect COPY relocations here and apply them
                        // in a second pass after all objects are relocated.
                        if (sym != 0) {
                            Elf64_Sym s;
                            mem_.read(obj.symtab_addr + sym * sizeof(s),
                                      &s, sizeof(s));
                            std::string name = read_guest_cstr(
                                mem_, obj.strtab_addr + s.st_name);
                            pending_copies_.push_back({
                                target, name, s.st_size, &obj});
                        }
                    } else if (type == R_AARCH64_ABS64_ ||
                               type == R_AARCH64_GLOB_DAT_) {
                        uint64_t value;
                        if (sym == 0) {
                            value = obj.base_addr + A;
                            mem_.store<uint64_t>(target, value);
                        } else {
                            // Read symbol name from obj's symtab.
                            Elf64_Sym s;
                            mem_.read(obj.symtab_addr + sym * sizeof(s),
                                      &s, sizeof(s));
                            std::string name = read_guest_cstr(
                                mem_, obj.strtab_addr + s.st_name);
                            // which consults versioned_symbols_ when the
                            // object has .gnu.version_r. This prevents
                            // wrong-version symbol selection.
                            uint64_t S = resolve_reloc_symbol(obj, sym);
                            // must resolve to 0, NOT obj.base_addr. The old
                            // fallback `S = obj.base_addr + s.st_value` ran
                            // for SHN_UNDEF symbols where st_value==0, so
                            // S became obj.base_addr — the GOT slot pointed
                            // to the start of the binary instead of 0.
                            // Real ld.so: unresolved weak UNDEF → S = 0.
                            if (S == 0 && s.st_shndx != SHN_UNDEF_) {
                                S = obj.base_addr + s.st_value;
                            }
                            value = S + A;
                            mem_.store<uint64_t>(target, value);
                            // Debug trace for _rtld_global_ro resolution.
                            if (dynlink_trace_enabled() &&
                                (name == "_rtld_global_ro" ||
                                 name == "_rtld_global")) {
                                fprintf(stderr, "[dynlink] GLOB_DAT '%s' "
                                        "resolved to 0x%llx (target=0x%llx)\n",
                                        name.c_str(),
                                        static_cast<unsigned long long>(value),
                                        static_cast<unsigned long long>(target));
                            }
                        }
                        apply_tls_mirror_(obj, target, value);
                    } else if (type == R_AARCH64_IRELATIVE_) {
                        // ifunc: call the resolver at base + A to get the
                        // real function pointer. The resolver is a small
                        // guest function that returns a pointer to one of
                        // several implementations (selected by CPU
                        // features, e.g., glibc's memcpy variants).
                        // BUGFIX: the old code just stored `base + A` (the
                        // resolver ADDRESS) instead of CALLING the resolver.
                        // This silently corrupted any program using ifuncs.
                        // Now we invoke the resolver via the Emulator's
                        // callback (which runs it in a scratch CPU and
                        // returns X0). If no callback is registered (e.g.,
                        // when DynamicLinker is used standalone), fall back
                        // to the old behavior with a warning.
                        uint64_t resolver_addr = obj.base_addr + A;
                        uint64_t resolved = 0;
                        if (ifunc_resolver_) {
                            resolved = ifunc_resolver_(resolver_addr);
                        }
                        if (resolved == 0) {
                            // Fallback: store the resolver address. The
                            // guest will call it as if it were the real
                            // function — wrong, but visible (it'll crash
                            // or return garbage) instead of silently
                            // using the wrong implementation.
                            if (!ifunc_resolver_) {
                                fprintf(stderr, "[%s] IRELATIVE at 0x%llx: "
                                        "no ifunc resolver registered; storing "
                                        "resolver address as fallback\n",
                                        "bifrost-emu",
                                        static_cast<unsigned long long>(target));
                            }
                            resolved = resolver_addr;
                        }
                        mem_.store<uint64_t>(target, resolved);
                    } else if (type == R_AARCH64_TLS_DTPMOD_) {
                        // TLS_DTPMOD: store the module ID of the symbol's
                        // defining object. If sym==0, it's the current
                        // object's module ID (used for LD access).
                        uint64_t mod_id = obj.tls_mod_id;
                        if (sym != 0) {
                            Elf64_Sym s;
                            mem_.read(obj.symtab_addr + sym * sizeof(s),
                                      &s, sizeof(s));
                            std::string name = read_guest_cstr(
                                mem_, obj.strtab_addr + s.st_name);
                            // Look up which object defines this symbol.
                            // Use the object's real [base, base+map_size)
                            // extent (find_object_by_addr), NOT a fixed
                            // 256 MiB window — with a contiguous bump
                            // allocator every symbol in a later library
                            // would be misattributed to the first object.
                            uint64_t sym_addr = resolve_symbol(name);
                            if (sym_addr != 0) {
                                const LoadedObject* o = find_object_by_addr(sym_addr);
                                if (o) mod_id = o->tls_mod_id;
                            }
                        }
                        mem_.store<uint64_t>(target, mod_id + A);
                    } else if (type == R_AARCH64_TLS_DTPREL_) {
                        // TLS_DTPREL: offset of the symbol within its
                        // module's TLS block. For sym==0, addend is the
                        // offset (used for LD access to current module).
                        uint64_t off = static_cast<uint64_t>(A);
                        if (sym != 0) {
                            Elf64_Sym s;
                            mem_.read(obj.symtab_addr + sym * sizeof(s),
                                      &s, sizeof(s));
                            // st_value is the offset within the defining
                            // module's PT_TLS segment.
                            off = s.st_value + A;
                        }
                        mem_.store<uint64_t>(target, off);
                    } else if (type == R_AARCH64_TLS_TPREL_) {
                        // TLS_TPREL: TP-relative offset for Initial-Exec
                        // access. Value = symbol's offset within its
                        // module's TLS block + module's TP-offset.
                        int64_t tp_off = A;
                        if (sym != 0) {
                            Elf64_Sym s;
                            mem_.read(obj.symtab_addr + sym * sizeof(s),
                                      &s, sizeof(s));
                            // Find which object defines this symbol.
                            std::string name = read_guest_cstr(
                                mem_, obj.strtab_addr + s.st_name);
                            uint64_t sym_addr = resolve_symbol(name);
                            int64_t mod_tp_off = obj.tls_tp_offset;
                            if (sym_addr != 0) {
                                const LoadedObject* o = find_object_by_addr(sym_addr);
                                if (o) mod_tp_off = o->tls_tp_offset;
                            }
                            tp_off = mod_tp_off + static_cast<int64_t>(s.st_value) + A;
                        } else {
                            tp_off = obj.tls_tp_offset + A;
                        }
                        mem_.store<uint64_t>(target, static_cast<uint64_t>(tp_off));
                    } else if (type == R_AARCH64_TLSDESC_) {
                        // TLSDESC: a 16-byte descriptor. The first 8
                        // bytes are the resolver function pointer; the
                        // second 8 bytes are the argument (TP-offset).
                        // For static TLS, we use a "lazy resolver" that
                        // just returns the pre-computed offset — no PLT
                        // call needed. We store:
                        //   desc[0] = 0  (resolver = NULL → caller treats
                        //                 desc[1] as the TP-offset directly)
                        //   desc[1] = TP-offset
                        // This is the "static TLSDESC" trick used by
                        // musl/glibc when the offset is known at load time.
                        int64_t tp_off = A;
                        if (sym != 0) {
                            Elf64_Sym s;
                            mem_.read(obj.symtab_addr + sym * sizeof(s),
                                      &s, sizeof(s));
                            std::string name = read_guest_cstr(
                                mem_, obj.strtab_addr + s.st_name);
                            uint64_t sym_addr = resolve_symbol(name);
                            int64_t mod_tp_off = obj.tls_tp_offset;
                            if (sym_addr != 0) {
                                const LoadedObject* o = find_object_by_addr(sym_addr);
                                if (o) mod_tp_off = o->tls_tp_offset;
                            }
                            tp_off = mod_tp_off + static_cast<int64_t>(s.st_value) + A;
                        } else {
                            tp_off = obj.tls_tp_offset + A;
                        }
                        // desc[0] = 0 (no resolver; inline)
                        mem_.store<uint64_t>(target, 0);
                        // desc[1] = TP-offset
                        mem_.store<uint64_t>(target + 8,
                            static_cast<uint64_t>(tp_off));
                    } else if (type == R_AARCH64_JUMP_SLOT_) {
                        // Eager binding: resolve now.
                        if (sym == 0) {
                            mem_.store<uint64_t>(target, obj.base_addr + A);
                        } else {
                            Elf64_Sym s;
                            mem_.read(obj.symtab_addr + sym * sizeof(s),
                                      &s, sizeof(s));
                            std::string name = read_guest_cstr(
                                mem_, obj.strtab_addr + s.st_name);
                            uint64_t S = resolve_reloc_symbol(obj, sym);
                            if (S == 0 && s.st_shndx != SHN_UNDEF_) {
                                S = obj.base_addr + s.st_value;
                            }
                            mem_.store<uint64_t>(target, S + A);
                        }
                    }
                }
            }
            // PLT relocations (DT_JMPREL) — eager binding.
            // ignored DT_JMPREL entirely. JUMP_SLOT relocations (the PLT
            // entries that point to libc functions like printf, malloc,
            // __libc_start_main) live in DT_JMPREL, NOT in DT_RELA. The
            // old code's JUMP_SLOT case (inside the DT_RELA loop) never
            // fired because JUMP_SLOTs aren't in DT_RELA. This meant
            // dynamically-linked glibc binaries crashed at _start because
            // the GOT entries for __libc_start_main etc. were never
            // filled (they stayed 0, so `br x17` jumped to 0 → decode
            // error at pc=0x0). Musl binaries happened to work because
            // musl's ld.so does its own lazy PLT binding at runtime.
            // The fix: process DT_JMPREL separately, eagerly binding
            // each JUMP_SLOT relocation.
            if (jmprel_addr && jmprel_size) {
                for (uint64_t off = 0; off + sizeof(Elf64_Rela) <= jmprel_size;
                     off += sizeof(Elf64_Rela)) {
                    Elf64_Rela r;
                    mem_.read(jmprel_addr + off, &r, sizeof(r));
                    uint32_t type = ELF64_R_TYPE_(r.r_info);
                    uint32_t sym  = ELF64_R_SYM_(r.r_info);
                    uint64_t target = obj.base_addr + r.r_offset;
                    int64_t A = r.r_addend;
                    if (type == R_AARCH64_JUMP_SLOT_) {
                        if (sym == 0) {
                            mem_.store<uint64_t>(target, obj.base_addr + A);
                        } else {
                            Elf64_Sym s;
                            mem_.read(obj.symtab_addr + sym * sizeof(s),
                                      &s, sizeof(s));
                            std::string name = read_guest_cstr(
                                mem_, obj.strtab_addr + s.st_name);
                            uint64_t S = resolve_reloc_symbol(obj, sym);
                            if (S == 0 && s.st_shndx != SHN_UNDEF_) {
                                S = obj.base_addr + s.st_value;
                            }
                            mem_.store<uint64_t>(target, S + A);
                            if (dynlink_trace_enabled() &&
                                (name == "_dl_allocate_tls" ||
                                 name == "_dl_allocate_tls_init")) {
                                fprintf(stderr, "[dynlink] JUMP_SLOT %s "
                                        "resolved to 0x%llx (GOT@0x%llx)\n",
                                        name.c_str(),
                                        (unsigned long long)(S + A),
                                        (unsigned long long)target);
                            }
                        }
                    }
                    // Other relocation types in DT_JMPREL (rare) fall
                    // through unprocessed — they'd need the same handling
                    // as in the DT_RELA loop above. JUMP_SLOT is the only
                    // type that should appear in DT_JMPREL per the ELF ABI.
                }
            }
        } catch (...) {
            // Relocation failed for this object — continue.
        }
    }
    // all objects' RELATIVE/GLOB_DAT/JUMP_SLOT/IRELATIVE relocations are
    // done. The COPY reads the original symbol's post-relocation value.
    // If applied during the first pass, it reads pre-relocation values
    // (e.g., libc's stdout variable before RELATIVE sets it to the
    // relocated _IO_2_1_stdout_ address).
    apply_pending_copies_();
    // In glibc 2.34+, the dynamic linker (ld-linux) calls this function
    // during early initialization. It calls __ctype_init() which sets up
    // the thread-local character type tables (ctype_b, ctype_tolower,
    // ctype_toupper). Without this, printf can't classify format
    // characters, causing float formatting to produce wrong output
    // (e.g., %.2f of 3.14 gives "3.1" instead of "3.14").
    //
    // Since we use our own native dynamic linker (not the guest's
    // ld-linux), __libc_early_init is never called. We call it here,
    // before DT_INIT_ARRAY, matching the order ld-linux uses.
    //
    // the resolved _rtld_global_ro to set dl_tls_static_size,
    // dl_tls_static_align, and dl_pagesize. Without these, glibc's
    // _dl_allocate_tls_storage allocates 0 bytes and pthread_create
    // hits `assert(size != 0)`. We patch ld-linux's REAL _rtld_global_ro
    // (not our shim) because __libc_early_init reads many other fields
    // from the real struct at specific offsets.
    patch_rtld_global_ro_();
    // _rtld_global. Must happen before __libc_early_init and before the
    // program runs pthread_create. See init_nptl_stack_lists_() for the
    // full rationale. (No-op for musl, which has no _rtld_global.)
    init_nptl_stack_lists_();
    if (init_runner_) {
        uint64_t early_init = resolve_symbol("__libc_early_init");
        if (early_init != 0) {
            if (dynlink_trace_enabled()) {
                fprintf(stderr, "[dynlink] calling __libc_early_init @ 0x%llx\n",
                        static_cast<unsigned long long>(early_init));
            }
            init_runner_(early_init);
            if (dynlink_trace_enabled()) {
                fprintf(stderr, "[dynlink] __libc_early_init returned\n");
            }
        }
    }
    // Re-patch _rtld_global_ro AFTER __libc_early_init.
    // __libc_early_init zeroes dl_pagesize. Re-apply.
    patch_rtld_global_ro_();
    // loaded object (libs first, main last). Runs C++ static constructors,
    // glibc __libc_start_main hooks, etc. Without this, every C++ game
    // runs with uninitialized globals (vtables, std::mutex, std::string).
    // Requires init_runner_ to be set by the Emulator; no-ops if not.
    run_init_arrays_();
    if (dynlink_trace_enabled()) {
        fprintf(stderr, "[dynlink] all DT_INIT_ARRAY done, link() complete\n");
    }
    // Final re-patch AFTER DT_INIT_ARRAY.
    patch_rtld_global_ro_();
    return true;
}
// ── patch_rtld_global_ro_ ──────────────────────────────────────────────
// Patch the resolved _rtld_global_ro to set dl_pagesize,
// dl_tls_static_size, and dl_tls_static_align. These fields are
// normally set by ld-linux during startup, but since we use our own
// dynamic linker, they remain 0. Without them:
//   - __getpagesize asserts GLRO(dl_pagesize) != 0
//   - _dl_allocate_tls_storage allocates 0 bytes → assert(size != 0)
//
// We resolve _rtld_global_ro via the global symbol table (which points
// to ld-linux's data section after relocations). Then we write the
// correct values at known offsets (determined from glibc 2.36 disassembly).
//
// We ONLY patch these specific fields — the rest of the struct retains
// whatever values ld-linux's .data section has (mostly zeros, which
// glibc handles gracefully via NULL checks).
// ── detect_tls_field_offsets_ ──────────────────────────────────────────
// Dynamically detect the offsets of dl_tls_static_size and
// dl_tls_static_align within struct rtld_global_ro by disassembling
// __libc_early_init in libc.so.6.
//
// Background: the offsets of these fields CHANGED between glibc versions:
//   glibc 2.36 (Arm GNU 13.2):  size @ +0x1D0, align @ +0x1D8
//   glibc 2.40 (Arm GNU 14.2):  size @ +0x1D8, align @ +0x1E0
//   glibc 2.42 (Arm GNU 15.2):  size @ +0x98,  align @ +0xA0
//
// Hardcoding offsets breaks when the toolchain is upgraded. Instead, we
// disassemble __libc_early_init to find the actual LDP offset it uses to
// load these two consecutive fields.
//
// The function pattern varies by glibc version:
//   glibc 2.36/2.40: adrp x1,...; ldr x1,[x1,#...]; ldp x?,x?,[x1,#off]
//     → base register is x1, offset range 0x100-0x300
//   glibc 2.42:      adrp x0,...; ldr x0,[x0,#...]; ldr x2,[x0,#0x18];
//                     ldp x1,x0,[x0,#off]
//     → base register is x0, offset can be as low as 0x98
//
// We scan the first ~48 instructions of __libc_early_init for an LDP
// that:
//   1. Uses a base register that was recently loaded from an adrp+ldr
//      pair (we track which register holds the _rtld_global_ro pointer).
//   2. Has an offset in the range 0x18..0x400 (covers all known glibc
//      versions, excludes prologue LDP x29,x30 at offset 0).
//
// Returns true on success, filling out_size_off and out_align_off.
// Returns false if detection fails (caller falls back to known offsets).
bool DynamicLinker::detect_dlopen_hook_offset_(uint32_t& out_hook_off) {
    out_hook_off = 0;
    // Prefer dlopen@@GLIBC_2.34; fall back to __libc_dlopen_mode.
    const char* fn_name = "dlopen";
    uint64_t fn = resolve_symbol("dlopen");
    if (fn == 0) {
        fn_name = "__libc_dlopen_mode";
        fn = resolve_symbol("__libc_dlopen_mode");
    }
    if (fn == 0) return false;
    uint8_t code[96];
    try {
        mem_.read(fn, code, sizeof(code));
    } catch (...) {
        return false;
    }
    // Same ADRP+LDR chain used by detect_tls_field_offsets_: once a
    // register holds &_rtld_global_ro, the next LDR from that base with
    // an offset in the known hook window is the dlfcn_hook pointer.
    uint32_t rtld_ro_reg = 0xFFFFFFFF;
    bool rtld_ro_reg_valid = false;
    for (size_t i = 0; i + 4 <= sizeof(code); i += 4) {
        uint32_t insn;
        memcpy(&insn, code + i, 4);
        if ((insn & 0x9F000000) == 0x90000000) {
            rtld_ro_reg = insn & 0x1F;
            rtld_ro_reg_valid = false;
            continue;
        }
        if ((insn & 0xFFC00000) == 0xF9400000) {
            uint32_t rn = (insn >> 5) & 0x1F;
            uint32_t rt = insn & 0x1F;
            uint32_t imm12 = (insn >> 10) & 0xFFF;
            uint32_t off = imm12 * 8;  // 64-bit LDR
            // Hook field first: glibc often reuses the same register
            // (ldr x4, [x4, #376]), which looks like the ADRP+LDR GOT
            // chain — so check the hook window whenever the base is
            // already known to hold &_rtld_global_ro.
            if (rtld_ro_reg_valid && rn == rtld_ro_reg &&
                off >= 0x160 && off <= 0x190) {
                out_hook_off = off;
                if (dynlink_trace_enabled()) {
                    fprintf(stderr, "[dynlink] detected dlopen hook offset "
                            "+0x%x via %s @0x%llx+%zu\n",
                            off, fn_name,
                            static_cast<unsigned long long>(fn), i);
                }
                return true;
            }
            if (rtld_ro_reg != 0xFFFFFFFF && rn == rtld_ro_reg && rt == rn) {
                rtld_ro_reg = rt;
                rtld_ro_reg_valid = true;
                continue;
            }
        }
    }
    return false;
}
bool DynamicLinker::detect_tls_field_offsets_(uint32_t& out_size_off,
                                               uint32_t& out_align_off) {
    out_size_off = 0;
    out_align_off = 0;
    uint64_t early_init = resolve_symbol("__libc_early_init");
    if (early_init == 0) return false;
    // Read up to 192 bytes (48 instructions) of __libc_early_init.
    // glibc 2.42's version is longer (includes __getrlimit + rlimit
    // adjustment before the TLS LDP), so we scan more than the old 32.
    uint8_t code[192];
    try {
        mem_.read(early_init, code, sizeof(code));
    } catch (...) {
        return false;
    }
    // Track which register holds the _rtld_global_ro pointer.
    // The pattern is: adrp xN, <page>; ldr xN, [xN, #<offset>]
    // After the ldr, xN holds &_rtld_global_ro.
    // We accept any register as the base (not just x1) because glibc
    // 2.42 uses x0 instead of x1.
    uint32_t rtld_ro_reg = 0xFFFFFFFF;  // invalid sentinel
    bool rtld_ro_reg_valid = false;
    for (size_t i = 0; i + 4 <= sizeof(code); i += 4) {
        uint32_t insn;
        memcpy(&insn, code + i, 4);
        // Detect ADRP: 1 immlo 10000 immhi Rd
        // mask 0x9F000000, value 0x90000000
        if ((insn & 0x9F000000) == 0x90000000) {
            uint32_t rd = insn & 0x1F;
            // Next instruction might be LDR xRd, [xRd, #imm]
            // We'll check on the next iteration.
            // For now, just remember this register had an ADRP.
            // (We don't track the ADRP target page — we just note
            //  that xRd is a candidate for the rtld_global_ro pointer.)
            rtld_ro_reg = rd;
            rtld_ro_reg_valid = false;  // not yet — need the LDR
            continue;
        }
        // Detect LDR (64-bit GPR, unsigned offset): 11 111 0 01 01 imm12 Rn Rt
        // mask 0xFFC00000, value 0xF9400000
        if ((insn & 0xFFC00000) == 0xF9400000) {
            uint32_t rn = (insn >> 5) & 0x1F;
            uint32_t rt = insn & 0x1F;
            // Only follow the chain when rt == rn (i.e., ldr xN, [xN, #imm]).
            // This is the pattern: adrp xN, <page>; ldr xN, [xN, #imm] →
            // xN now holds the value at that address (the rtld_global_ro
            // pointer). If rt != rn (e.g., ldr x2, [x0, #24] to load
            // dl_pagesize), x2 does NOT hold the rtld_global_ro pointer.
            if (rtld_ro_reg != 0xFFFFFFFF && rn == rtld_ro_reg && rt == rn) {
                rtld_ro_reg = rt;
                rtld_ro_reg_valid = true;
            }
            continue;
        }
        // Detect LDP (64-bit GPR, unsigned offset):
        //   10 1010 0101 0 imm7 Rt2 Rn Rt
        //   mask 0xFFC00000, value 0xA9400000
        if ((insn & 0xFFC00000) != 0xA9400000) continue;
        uint32_t rn = (insn >> 5) & 0x1F;
        // The base must be the register holding _rtld_global_ro.
        if (!rtld_ro_reg_valid || rn != rtld_ro_reg) continue;
        // imm7 is at bits 21-15 (7 bits, unsigned for this variant).
        uint32_t imm7 = (insn >> 15) & 0x7F;
        uint32_t offset = imm7 * 8;  // scaled by 8 for 64-bit
        // Sanity: offset must be in a reasonable range (0x18..0x400)
        // for rtld_global_ro's fields. This excludes LDP x29,x30
        // in the prologue (offset 0).
        // 0x18 = dl_pagesize offset (stable across versions).
        // The TLS fields are at a higher offset, but we accept any
        // non-zero offset here because the LDP we want is the ONLY
        // LDP from the rtld_global_ro register (besides the pagesize
        // LDR which uses a different instruction).
        if (offset < 0x18 || offset > 0x400) continue;
        // Heuristic: the LDP we want loads (dl_tls_static_size,
        // dl_tls_static_align). In glibc 2.42, the LDP at offset 0x98
        // loads (size, align). In glibc 2.40, the LDP at 0x1D8 loads
        // (size, align). We accept any LDP from the rtld_global_ro
        // register as the TLS field pair.
        out_size_off = offset;
        out_align_off = offset + 8;
        if (dynlink_trace_enabled()) {
            fprintf(stderr, "[dynlink] detected TLS field offsets via "
                    "__libc_early_init @0x%llx+%zu: size@+0x%x, align@+0x%x "
                    "(imm7=%u, base=x%u)\n",
                    static_cast<unsigned long long>(early_init), i,
                    out_size_off, out_align_off, imm7, rtld_ro_reg);
        }
        return true;
    }
    return false;
}
void DynamicLinker::patch_rtld_global_ro_() {
    if (pending_tls_static_size_ == 0) return;  // no TLS → nothing to patch
    uint64_t rtld_ro = resolve_symbol("_rtld_global_ro");
    if (rtld_ro == 0) {
        // _rtld_global_ro not found — might be a musl binary (no _rtld_global_ro).
        return;
    }
    // ── Dynamic offset detection ────────────────────────────────────
    // The offsets of dl_tls_static_size and dl_tls_static_align within
    // struct rtld_global_ro vary by glibc version:
    //   glibc 2.36 (Arm GNU 13.2): size @ +0x1D0, align @ +0x1D8
    //   glibc 2.40 (Arm GNU 14.2): size @ +0x1D8, align @ +0x1E0
    // Hardcoding breaks when the toolchain is upgraded. We disassemble
    // __libc_early_init to find the actual LDP offset it uses to load
    // these two consecutive fields. If detection fails, we fall back to
    // spraying ALL known offsets (safe because the fields are consecutive
    // size_t values and spraying writes the same value to adjacent slots).
    uint32_t size_off = 0, align_off = 0;
    bool detected = detect_tls_field_offsets_(size_off, align_off);
    // Known offset pairs (glibc version → (size_off, align_off)).
    // Used as fallback if dynamic detection fails.
    struct KnownOffset { uint32_t size; uint32_t align; const char* ver; };
    constexpr KnownOffset known_offsets[] = {
        {0x98,  0xA0,  "glibc 2.42 (Arm GNU 15.2)"},  // check newest first
        {0x1D8, 0x1E0, "glibc 2.40 (Arm GNU 14.2)"},
        {0x1D0, 0x1D8, "glibc 2.36 (Arm GNU 13.2)"},
    };
    // Pagesize is at offset 0x18 in all known glibc versions.
    constexpr uint32_t PAGESIZE_OFF = 0x18;
    try {
        // ── Patch dl_pagesize (offset 0x18, stable across versions) ──
        uint64_t pagesize = mem_.load<uint64_t>(rtld_ro + PAGESIZE_OFF);
        if (pagesize == 0) {
            mem_.store<uint64_t>(rtld_ro + PAGESIZE_OFF, 4096);
        }
        // ── Patch dl_tls_static_size and dl_tls_static_align ─────────
        // Strategy: if dynamic detection succeeded, patch exactly those
        // two offsets. Otherwise, "spray" — write the size and align
        // values to ALL known offset pairs. Spraying is safe because:
        //   1. The fields are consecutive size_t values in the struct.
        //   2. Writing a valid size to an adjacent field that happens
        //      to be dl_tls_static_used or dl_tls_static_surplus is
        //      harmless (glibc adds them to the size, and a slightly
        //      larger size just means a bit more TLS surplus).
        //   3. We only write if the current value is 0, so we never
        //      clobber a field that ld-linux already initialized.
        if (detected) {
            // Precise patching — write only the detected offsets.
            uint64_t cur_size = mem_.load<uint64_t>(rtld_ro + size_off);
            if (cur_size == 0) {
                mem_.store<uint64_t>(rtld_ro + size_off,
                                     pending_tls_static_size_);
            }
            uint64_t cur_align = mem_.load<uint64_t>(rtld_ro + align_off);
            if (cur_align == 0) {
                mem_.store<uint64_t>(rtld_ro + align_off, 64);
            }
        } else {
            // Fallback: spray all known offset pairs.
            if (dynlink_trace_enabled()) {
                fprintf(stderr, "[dynlink] TLS offset detection failed; "
                        "spraying all known offsets\n");
            }
            for (const auto& ko : known_offsets) {
                uint64_t cur_size = mem_.load<uint64_t>(rtld_ro + ko.size);
                if (cur_size == 0) {
                    mem_.store<uint64_t>(rtld_ro + ko.size,
                                         pending_tls_static_size_);
                }
                uint64_t cur_align = mem_.load<uint64_t>(rtld_ro + ko.align);
                if (cur_align == 0) {
                    mem_.store<uint64_t>(rtld_ro + ko.align, 64);
                }
            }
        }
        if (dynlink_trace_enabled()) {
            fprintf(stderr, "[dynlink] patched _rtld_global_ro @0x%llx: "
                    "dl_pagesize=4096, dl_tls_static_size=%llu, "
                    "dl_tls_static_align=64 (%s)\n",
                    static_cast<unsigned long long>(rtld_ro),
                    static_cast<unsigned long long>(pending_tls_static_size_),
                    detected ? "dynamic detection"
                             : "spray fallback");
        }
    } catch (...) {
        // Reading/writing _rtld_global_ro failed — the struct might
        // not be mapped at the expected address. This is non-fatal;
        // glibc will hit the assertion later (visible failure).
    }
    // dlopen / dlfcn hook pointer inside _rtld_global_ro.
    // glibc's dlopen@@GLIBC_2.34 and __libc_dlopen_mode do:
    //   ldr xN, [rtld_global_ro, #hook_off]  → hook struct*
    //   ldr xM, [xN] / [xN, #72]             → _dl_open
    // The hook field offset moved across glibc versions:
    //   glibc ≤2.40: +368 (0x170)
    //   glibc 2.43+:  +376 (0x178)
    // Hardcoding 368 made dlopen fall into the no-hook path on 2.43,
    // which calls the real ld-linux _dl_open and ends at pc=0.
    if (dlopen_hook_ptr_ != 0) {
        try {
            uint32_t hook_off = 0;
            bool hook_detected = detect_dlopen_hook_offset_(hook_off);
            // Known offsets newest-first; spray only empty slots so we
            // never clobber a non-NULL field that belongs to another
            // member of rtld_global_ro.
            constexpr uint32_t known_hook_offs[] = {376, 368};
            if (hook_detected) {
                mem_.store<uint64_t>(rtld_ro + hook_off, dlopen_hook_ptr_);
                if (dynlink_trace_enabled()) {
                    fprintf(stderr, "[dynlink] patched _rtld_global_ro + %u "
                            "= 0x%llx (dlopen hook, detected)\n",
                            hook_off,
                            static_cast<unsigned long long>(dlopen_hook_ptr_));
                }
            } else {
                for (uint32_t off : known_hook_offs) {
                    uint64_t cur = 0;
                    try { cur = mem_.load<uint64_t>(rtld_ro + off); }
                    catch (...) { continue; }
                    if (cur != 0) continue;
                    mem_.store<uint64_t>(rtld_ro + off, dlopen_hook_ptr_);
                    if (dynlink_trace_enabled()) {
                        fprintf(stderr, "[dynlink] patched _rtld_global_ro + %u "
                                "= 0x%llx (dlopen hook, spray)\n",
                                off,
                                static_cast<unsigned long long>(dlopen_hook_ptr_));
                    }
                }
            }
        } catch (...) {}
    }
}
// ── init_nptl_stack_lists_ ────────────────────────────────────────────
// Initialize the NPTL stack-cache list heads in _rtld_global so that
// glibc's pthread_create -> allocate_stack does not spin forever.
//
// Background: glibc (2.34+, NPTL merged into libc) keeps three circular
// doubly-linked lists in `struct rtld_global` to manage thread stacks:
//
//   _dl_stack_used   — threads currently using a stack
//   _dl_stack_user   — threads which need a stack
//   _dl_stack_cache  — cache of free, reusable stacks
//
// Each is a `list_t` (= `struct list_head { list_t *next, *prev; }`,
// 16 bytes). An EMPTY list must have head->next == head->prev == &head
// (the INIT_LIST_HEAD macro). `allocate_stack` walks `_dl_stack_cache`
// via `list_for_each(entry, &GL(dl_stack_cache))`, which expands to
// `for (entry = head->next; entry != head; entry = entry->next)`. If
// the list is properly empty (head->next == head), the body is skipped
// and a fresh stack is allocated.
//
// The bug: ld-linux's `__pthread_initialize_minimal_internal` calls
// INIT_LIST_HEAD on all three heads during startup, but we use our own
// dynamic linker (not the guest ld-linux), so those heads stay zeroed
// (they live in ld-linux's .bss, mapped read-write). With head->next ==
// NULL, the `list_for_each` loop dereferences NULL -> bifrost-emu
// returns 0 for unmapped reads (instead of faulting) -> entry stays 0
// forever -> infinite CPU spin (no syscall, no futex). This matches the
// observed hang: test_dyn_pthread_min prints "start", then spins in
// libc.so.6 + 0x8102c (inside pthread_create) without making any
// syscall.
//
// The fix: resolve `_rtld_global` (the read-write rtld global, NOT
// `_rtld_global_ro`) and write self-referential pointers into the three
// list heads.
//
// ── DYNAMIC OFFSET DETECTION ───────────────────────
// The three list heads live at version-dependent offsets within
// `struct rtld_global`. Rather than hardcode offsets for one glibc
// version, we discover them at runtime from the libthread_db
// descriptors that glibc exports in libc.so.6 .rodata:
//
//   _thread_db_rtld_global__dl_stack_used  (12-byte descriptor)
//   _thread_db_rtld_global__dl_stack_user  (12-byte descriptor)
//
// Each descriptor is `{ uint32_t struct_offset; uint32_t struct_size;
// uint32_t field_offset_lo; uint32_t field_offset_hi }` — the field
// offset within `struct rtld_global` is at byte +8 (a 32-bit LE value).
// (struct_offset/struct_size are libthread_db bookkeeping; we only
// need the field offset.)
//
// The three list heads are CONTIGUOUS 16-byte `list_t`s:
//   stack_used  @ desc_used.offset
//   stack_user  @ desc_used.offset + 16   (= stack_used + sizeof(list_t))
//   stack_cache @ desc_used.offset + 32
// We cross-check: desc_user.offset MUST equal desc_used.offset + 16;
// if it doesn't, the layout assumption is wrong for this glibc and we
// fall back to NOT initializing (safer than writing at wrong offsets —
// glibc will spin, a visible failure, rather than corrupt memory).
//
// Fallback: if the descriptors aren't found (stripped libc, or a libc
// without NPTL), we fall back to the glibc 2.36 / Arm GNU 13.2 offsets
// (0x1158 / 0x1168 / 0x1178). These are correct for the toolchain we
// ship tests against; the dynamic path covers other versions.
//
// This is glibc-specific (musl has no _rtld_global); if the symbol is
// not found (musl, or static binary), this is a no-op. We also guard
// against double-init: if a head already points to itself (or to a
// non-zero value — meaning ld-linux init already ran), we leave it
// alone so we never corrupt a populated list.
void DynamicLinker::init_nptl_stack_lists_() {
    uint64_t rtld = resolve_symbol("_rtld_global");
    if (rtld == 0) {
        // musl or static binary — no _rtld_global. Nothing to do.
        return;
    }
    // ── Discover the stack_used / stack_user offsets dynamically ──
    // Default to the glibc 2.36 / Arm GNU 13.2 offsets (our shipped
    // toolchain). Try to override with the libthread_db descriptor
    // values for portability across glibc versions.
    uint64_t off_used  = 0x1158;
    uint64_t off_user  = 0x1168;
    uint64_t off_cache = 0x1178;
    bool dynamic = false;
    uint64_t desc_used = resolve_symbol(
        "_thread_db_rtld_global__dl_stack_used");
    uint64_t desc_user = resolve_symbol(
        "_thread_db_rtld_global__dl_stack_user");
    if (desc_used != 0 && desc_user != 0) {
        try {
            // The field offset is a 32-bit LE value at descriptor +8.
            uint32_t u = mem_.load<uint32_t>(desc_used + 8);
            uint32_t v = mem_.load<uint32_t>(desc_user + 8);
            // Cross-check the contiguity invariant:
            //   stack_user == stack_used + 16 (one list_t).
            // If it holds, the layout matches our assumption; adopt the
            // dynamic offsets. If not, keep the fallback (don't risk
            // writing at inconsistent offsets).
            if (v == u + 16 && u != 0) {
                off_used  = u;
                off_user  = v;
                off_cache = static_cast<uint64_t>(u) + 32;
                dynamic = true;
            }
        } catch (...) {
            // Descriptor not readable — keep the fallback offsets.
        }
    }
    struct ListHeadOff { const char* name; uint64_t off; };
    const ListHeadOff heads[] = {
        { "_dl_stack_used",  off_used  },
        { "_dl_stack_user",  off_user  },
        { "_dl_stack_cache", off_cache },
    };
    bool patched = false;
    try {
        for (const auto& h : heads) {
            uint64_t head_addr = rtld + h.off;
            uint64_t next = mem_.load<uint64_t>(head_addr);
            // Only initialize if the head is still zero (uninitialized).
            // If next is non-zero, either ld-linux already initialized it
            // (self-pointer) or a previous pthread_create populated the
            // list — in either case, leave it alone.
            if (next == 0) {
                // INIT_LIST_HEAD: head->next = head->prev = &head.
                mem_.store<uint64_t>(head_addr,     head_addr);  // next
                mem_.store<uint64_t>(head_addr + 8, head_addr);  // prev
                patched = true;
            }
        }
    } catch (...) {
        // _rtld_global not mapped at the expected range — non-fatal.
        // glibc will spin later (visible failure).
    }
    if (patched && dynlink_trace_enabled()) {
        fprintf(stderr, "[dynlink] initialized NPTL stack list heads in "
                "_rtld_global @0x%llx (%s offsets: "
                "stack_used@+0x%llx, stack_user@+0x%llx, "
                "stack_cache@+0x%llx)\n",
                static_cast<unsigned long long>(rtld),
                dynamic ? "dynamic" : "fallback",
                static_cast<unsigned long long>(off_used),
                static_cast<unsigned long long>(off_user),
                static_cast<unsigned long long>(off_cache));
    }
}
// ── apply_pending_copies_ ──────────────────────────────────────────────
// Apply all deferred R_AARCH64_COPY relocations. For each COPY:
//   1. Find the original symbol definition in a shared library (skip
//      the main binary's own copy).
//   2. Copy st_size bytes from the original to the target (main binary's
//      .bss/.data).
//   3. Update the global symbol table so future resolutions of this
//      symbol return `target` (the copy in the main binary). This
//      ensures libc's own GLOB_DAT relocations for `stdout` resolve
//      to the main binary's copy, so libc and the main binary share
//      the same FILE* pointer.
//
// This MUST be called after ALL objects' RELATIVE/ABS64/GLOB_DAT/
// JUMP_SLOT/IRELATIVE relocations are applied, so the original symbol's
// value reflects post-relocation state (e.g., libc's `stdout` variable
// contains a relocated pointer to `_IO_2_1_stdout_`).
void DynamicLinker::apply_pending_copies_() {
    for (const auto& cp : pending_copies_) {
        uint64_t src_addr = 0;
        uint64_t copy_size = cp.size;
        // Find original definition in a shared lib (skip main binary
        // and skip the object that contains the COPY reloc).
        for (const auto& o : objects_) {
            if (&o == cp.copy_obj) continue;
            if (o.is_main) continue;
            if (o.symtab_addr == 0) continue;
            for (size_t i = 1; i < o.symtab_count && i < 8192*4; i++) {
                Elf64_Sym os;
                try {
                    mem_.read(o.symtab_addr + i*sizeof(os), &os, sizeof(os));
                } catch (...) { break; }
                if (os.st_shndx == SHN_UNDEF_) continue;
                uint8_t obind = ST_BIND_(os.st_info);
                if (obind != STB_GLOBAL_ && obind != STB_WEAK_) continue;
                std::string oname = read_guest_cstr(
                    mem_, o.strtab_addr + os.st_name);
                if (oname == cp.name) {
                    src_addr = o.base_addr + os.st_value;
                    if (copy_size == 0) copy_size = os.st_size;
                    break;
                }
            }
            if (src_addr) break;
        }
        if (src_addr && copy_size > 0 && copy_size <= 65536) {
            std::vector<uint8_t> buf(copy_size);
            mem_.read(src_addr, buf.data(), copy_size);
            mem_.write(cp.target, buf.data(), copy_size);
            // Update symbol table: future resolutions return the copy.
            symbols_[cp.name] = SymEntry{cp.target, STB_GLOBAL_};
            if (dynlink_trace_enabled()) {
                fprintf(stderr,
                    "[dynlink] COPY %s: %llu bytes from 0x%llx to 0x%llx\n",
                    cp.name.c_str(),
                    (unsigned long long)copy_size,
                    (unsigned long long)src_addr,
                    (unsigned long long)cp.target);
            }
        } else if (dynlink_trace_enabled()) {
            fprintf(stderr,
                "[dynlink] COPY %s: src=0x%llx size=%llu (SKIPPED)\n",
                cp.name.c_str(),
                (unsigned long long)src_addr,
                (unsigned long long)copy_size);
        }
    }
    pending_copies_.clear();
}
// ── apply_tls_mirror_ ──────────────────────────────────────────────────
// If `target` falls within obj's PT_TLS (.tdata) segment, also store
// `value` at the corresponding offset in the static TLS block. See
// the header comment for why this is necessary.
void DynamicLinker::apply_tls_mirror_(const LoadedObject& obj,
                                       uint64_t target, uint64_t value) {
    if (!obj.tls.present || obj.tls.filesz == 0) return;
    if (static_tls_base_ == 0) return;
    uint64_t tdata_start = obj.base_addr + obj.tls.vaddr;
    uint64_t tdata_end = tdata_start + obj.tls.filesz;
    if (target >= tdata_start && target < tdata_end) {
        uint64_t tls_dst = static_tls_base_ +
            obj.tls_block_offset +
            (target - tdata_start);
        mem_.store<uint64_t>(tls_dst, value);
    }
}
// ── apply_relr_relocations_ ──────────────────────────────────
// Apply DT_RELR — compact relative relocations. Each bit in the bitmap
// represents one 8-byte relocation slot. Two encoding forms:
//
//   1. "Bitmap" form (bit 63 set): the low 63 bits are a bitmap where
//      bit i means "apply R_AARCH64_RELATIVE at addr + i*8". After
//      processing, advance addr by 63*8 = 504 bytes.
//
//   2. "Address+bitmap" form (bit 63 clear): the current word is the
//      new relocation address (raw vaddr, NOT shifted by base). The
//      NEXT word in the stream is a 63-bit bitmap (bit 63 is reserved
//      and ignored) where bit i means "apply at addr + i*8". After
//      processing, advance addr = (new addr) + 63*8.
//
// The first word of the section is always an address+bitmap form
// (bit 63 is clear because vaddrs in shared libs are < 2^63). After
// that, the encoder chooses whichever form is more compact: bitmaps
// for dense runs of relocations, address+bitmap for sparse regions
// (skipping over zero runs without emitting padding words).
//
// Each RELR relocation is equivalent to:
//     R_AARCH64_RELATIVE  *(addr) = base + 0
//
// This must be called BEFORE apply_tls_mirror_ would otherwise fire
// for RELR targets — RELR has no symbol resolution, so there's no
// overlap with GLOB_DAT/COPY. But RELR can target .tdata, so we
// mirror to the TLS block copy here too.
void DynamicLinker::apply_relr_relocations_(const LoadedObject& obj,
                                             uint64_t relr_addr,
                                             uint64_t relr_size) {
    if (relr_size == 0 || relr_addr == 0) return;
    // Read the entire RELR section up front. It's typically small
    // (libc.so.6 glibc 2.40: 272 bytes = 34 words).
    std::vector<uint8_t> buf(relr_size);
    try {
        mem_.read(relr_addr, buf.data(), relr_size);
    } catch (...) {
        return;  // unreadable — skip
    }
    // ── RELR encoding (per glibc's elf_machine_relr in dl-machine.h) ──
    // Each entry is uint64_t. The LOW bit (bit 0) is the flag:
    //
    //   bit 0 == 0 (address entry):
    //     The entry value IS the relocation address (vaddr). Apply
    //     R_AARCH64_RELATIVE there, then advance the relocation pointer
    //     to addr + 8 (i.e., the next 8-byte slot).
    //
    //   bit 0 == 1 (bitmap entry):
    //     Bits 1..63 (63 bits) are a bitmap. Bit i (for i=1..63) means
    //     "apply R_AARCH64_RELATIVE at reloc_addr + (i-1)*8". After
    //     processing, advance reloc_addr by 62*8 (= 63 slots minus 1,
    //     because the address entry's slot was already advanced past).
    //
    // NOTE: this is DIFFERENT from what some blog posts describe (they
    // claim bit 63 is the flag). The actual glibc/binutils implementation
    // uses bit 0. Without the correct flag bit, the decoder treats valid
    // bitmap entries as addresses (or vice versa), producing wrong
    // relocation targets and crashing on the first init_array call.
    //
    // Each R_AARCH64_RELATIVE: *(addr) += base. The addend is the
    // existing value at *addr (the file vaddr), so we read it, add base,
    // and write back. (RELR doesn't have an explicit r_addend field.)
    const size_t n_words = relr_size / sizeof(uint64_t);
    size_t applied = 0;
    uint64_t reloc_addr = 0;  // current relocation address (raw vaddr)
    auto apply_one = [&](uint64_t vaddr) {
        uint64_t target = obj.base_addr + vaddr;
        uint64_t addend = 0;
        try {
            mem_.read(target, &addend, sizeof(addend));
        } catch (...) {
            addend = 0;
        }
        uint64_t value = obj.base_addr + addend;
        mem_.store<uint64_t>(target, value);
        apply_tls_mirror_(obj, target, value);
        applied++;
    };
    for (size_t i = 0; i < n_words; i++) {
        uint64_t word;
        memcpy(&word, buf.data() + i * sizeof(uint64_t), sizeof(uint64_t));
        if ((word & 1) == 0) {
            // Address entry: word IS the vaddr (bit 0 is 0, so word = vaddr).
            reloc_addr = word;
            apply_one(reloc_addr);
            reloc_addr += 8;  // advance past this slot
        } else {
            // Bitmap entry: bits 1..63 (63 bits).
            // Bit i (i=1..63) → reloc_addr + (i-1)*8.
            for (int i = 1; i <= 63; i++) {
                if (word & (1ULL << i)) {
                    apply_one(reloc_addr + (i - 1) * 8);
                }
            }
            // Advance by 63*8: the bitmap covers 63 slots (bits 1..63),
            // and the address entry's slot was already advanced past (+8).
            // So the next bitmap starts 63 slots after the current one.
            // (NOT 62*8 — that would skip a slot and misalign all
            // subsequent bitmaps, corrupting stdout->_lock and other
            // critical pointers.)
            reloc_addr += 63 * 8;
        }
    }
    if (dynlink_trace_enabled()) {
        fprintf(stderr, "[dynlink] RELR obj '%s': %zu relocations applied\n",
                obj.name.c_str(), applied);
    }
}
// ── run_init_arrays_ ───────────────────────────────────────────────────
// Invoke DT_INIT (legacy _init()) and each entry in DT_INIT_ARRAY for
// every loaded object, in dependency order (libs first, main last).
// The init_runner_ callback runs a guest function at `addr` and returns
// when it RETs. If no callback is registered, this is a no-op (the guest
// will run with uninitialized statics — visible as crashes/vtables-full-
// of-zero, not silent corruption).
void DynamicLinker::run_init_arrays_() {
    if (!init_runner_) return;
    // Dependencies must initialize before dependents: libc/libm before the
    // main binary. objects_[0] is the main executable and libraries are
    // appended after it, so iterate in reverse load order. (The DT_NEEDED
    // worklist appends a dependency after its dependent, so reversing puts
    // shared libs first and main last.)
    for (size_t oi = objects_.size(); oi-- > 0;) {
        const auto& obj = objects_[oi];
        // DT_INIT (legacy _init() function) — call before .init_array.
        if (obj.init_addr != 0) {
            try {
                if (dynlink_trace_enabled()) {
                    fprintf(stderr, "[dynlink] DT_INIT for '%s' @ 0x%llx\n",
                            obj.name.c_str(),
                            static_cast<unsigned long long>(obj.init_addr));
                }
                init_runner_(obj.init_addr);
            } catch (...) {}
        }
        // DT_INIT_ARRAY — array of function pointers, count = size/8.
        if (obj.init_array_addr != 0 && obj.init_array_size >= 8) {
            size_t count = obj.init_array_size / 8;
            for (size_t i = 0; i < count; i++) {
                uint64_t fn = 0;
                try {
                    fn = mem_.load<uint64_t>(obj.init_array_addr + i * 8);
                } catch (...) { break; }
                if (fn == 0) continue;  // skip NULL entries
                try {
                    if (dynlink_trace_enabled()) {
                        fprintf(stderr, "[dynlink] DT_INIT_ARRAY[%zu] for '%s' @ 0x%llx\n",
                                i, obj.name.c_str(),
                                static_cast<unsigned long long>(fn));
                    }
                    init_runner_(fn);
                } catch (...) {
                    // If one constructor throws, continue with the rest.
                    // (Real ld.so aborts, but for an emulator it's better
                    // to be lenient and let the user see the failure.)
                }
            }
        }
    }
}
// ── parse_dynamic ──────────────────────────────────────────────────────
bool DynamicLinker::parse_dynamic(const std::vector<uint8_t>& data,
                                  uint64_t base, LoadedObject& obj) {
    // Read ELF header.
    if (data.size() < 64) {
        error_ = "parse_dynamic: ELF file too small (" +
                 std::to_string(data.size()) + " < 64)";
        return false;
    }
    uint64_t e_phoff, e_shoff;
    uint16_t e_phentsize, e_phnum, e_shentsize, e_shnum;
    memcpy(&e_phoff,     data.data() + 32, 8);
    memcpy(&e_shoff,     data.data() + 40, 8);
    memcpy(&e_phentsize, data.data() + 54, 2);
    memcpy(&e_phnum,     data.data() + 56, 2);
    memcpy(&e_shentsize, data.data() + 58, 2);
    memcpy(&e_shnum,     data.data() + 60, 2);
    // either phdr loop. A malformed ELF with bogus e_phoff could OOB-read
    // `data`. Also require e_phentsize >= 56 (we read up to p+48 for
    // p_align, and the second loop reads p+32 for p_filesz).
    if (e_phoff == 0 || e_phoff >= data.size() || e_phentsize < 56) {
        error_ = "parse_dynamic: invalid program header table in " + obj.name;
        return false;
    }
    // Find PT_DYNAMIC in program headers.
    uint64_t dyn_vaddr = 0, dyn_filesz = 0;
    for (int i = 0; i < e_phnum; i++) {
        if (e_phoff + (i + 1) * e_phentsize > data.size()) break;
        const uint8_t* p = data.data() + e_phoff + i * e_phentsize;
        uint32_t p_type;
        memcpy(&p_type, p + 0, 4);
        if (p_type == 2) {  // PT_DYNAMIC
            // The ELF64 program header layout is:
            //   offset 0:  p_type   (4 bytes)
            //   offset 4:  p_flags  (4 bytes)
            //   offset 8:  p_offset (8 bytes)  ← file offset
            //   offset 16: p_vaddr  (8 bytes)  ← virtual address
            //   offset 24: p_paddr  (8 bytes)
            //   offset 32: p_filesz (8 bytes)
            //   offset 40: p_memsz  (8 bytes)
            //   offset 48: p_align  (8 bytes)
            // to work for some musl PIE binaries where p_offset happened
            // to fall inside a LOAD segment's p_vaddr range, but broke
            // for glibc executables where the DYNAMIC segment's p_offset
            // (0xfdd8) didn't match any LOAD segment's p_vaddr range
            // (LOAD2 vaddr = 0x41fdd8). The fix reads p_vaddr (p+16)
            // into dyn_vaddr, which is the correct field.
            memcpy(&dyn_vaddr,  p + 16, 8);  // p_vaddr (was p+8 = p_offset)
            memcpy(&dyn_filesz, p + 32, 8);  // p_filesz
            break;
        }
    }
    if (dyn_vaddr == 0) {
        // No PT_DYNAMIC — this is a static binary. Nothing to do.
        return true;
    }
    obj.dyn_addr = base + dyn_vaddr;
    // Parse the dynamic section from the file bytes (since the in-memory
    // copy may not yet be relocated).
    // Find the file offset corresponding to dyn_vaddr.
    uint64_t dyn_off = 0;
    bool found = false;
    for (int i = 0; i < e_phnum; i++) {
        if (e_phoff + (i + 1) * e_phentsize > data.size()) break;
        const uint8_t* p = data.data() + e_phoff + i * e_phentsize;
        uint32_t p_type;
        uint64_t p_offset, p_vaddr, p_filesz;
        memcpy(&p_type,   p + 0,  4);
        memcpy(&p_offset, p + 8,  8);
        memcpy(&p_vaddr,  p + 16, 8);
        memcpy(&p_filesz, p + 32, 8);
        if (p_type == 1 && dyn_vaddr >= p_vaddr &&
            dyn_vaddr < p_vaddr + p_filesz) {
            dyn_off = p_offset + (dyn_vaddr - p_vaddr);
            found = true;
            break;
        }
    }
    if (!found) {
        error_ = "parse_dynamic: no PT_DYNAMIC segment found in " +
                 obj.name;
        return false;
    }
    // Iterate Elf64_Dyn entries.
    // /DT_SONAME/DT_RPATH/DT_RUNPATH (previously declared as constants
    // but never read). Also capture DT_HASH for symbol-count derivation (H5).
    uint64_t symtab_vaddr = 0, strtab_vaddr = 0;
    uint64_t hash_vaddr = 0;
    uint64_t soname_off = 0, rpath_off = 0, runpath_off = 0;
    for (uint64_t off = dyn_off;
         off + sizeof(Elf64_Dyn) <= data.size() && off < dyn_off + dyn_filesz;
         off += sizeof(Elf64_Dyn)) {
        Elf64_Dyn dyn;
        memcpy(&dyn, data.data() + off, sizeof(dyn));
        if (dyn.d_tag == DT_NULL_) break;
        switch (dyn.d_tag) {
            case DT_SYMTAB_:        symtab_vaddr = dyn.d_val; break;
            case DT_STRTAB_:        strtab_vaddr = dyn.d_val; break;
            case DT_JMPREL_:        obj.jmprel_addr = base + dyn.d_val; break;
            case DT_PLTRELSZ_:      obj.jmprel_size = dyn.d_val; break;
            // produces these by default. Without processing them, libc's
            // internal pointers (init_array, .data.rel.ro, .got) never
            // get fixed up → "decode error at pc=0x0" on first init call.
            case DT_RELR_:          obj.relr_addr = base + dyn.d_val; break;
            case DT_RELRSZ_:        obj.relr_size = dyn.d_val; break;
            case DT_HASH_:          hash_vaddr = dyn.d_val; break;
            case DT_INIT_:          obj.init_addr = base + dyn.d_val; break;
            case DT_FINI_:          obj.fini_addr = base + dyn.d_val; break;
            case DT_INIT_ARRAY_:    obj.init_array_addr = base + dyn.d_val; break;
            case DT_INIT_ARRAYSZ_:  obj.init_array_size = dyn.d_val; break;
            case DT_FINI_ARRAY_:    obj.fini_array_addr = base + dyn.d_val; break;
            case DT_FINI_ARRAYSZ_:  obj.fini_array_size = dyn.d_val; break;
            case DT_SONAME_:        soname_off = dyn.d_val; break;
            case DT_RPATH_:         rpath_off = dyn.d_val; break;
            case DT_RUNPATH_:       runpath_off = dyn.d_val; break;
            case DT_VERSYM_:        obj.versym_addr = base + dyn.d_val; break;
            case DT_VERDEF_:        obj.verdef_addr = base + dyn.d_val; break;
            case DT_VERDEFNUM_:     obj.verdef_num = dyn.d_val; break;
            case DT_VERNEED_:       obj.verneed_addr = base + dyn.d_val; break;
            case DT_VERNEEDNUM_:    obj.verneed_num = dyn.d_val; break;
            default: break;
        }
    }
    obj.symtab_addr = base + symtab_vaddr;
    obj.strtab_addr = base + strtab_vaddr;
    // DT_HASH's first two uint32_t are nbucket and nchain; nchain is the
    // number of symbols in .dynsym (exact count — no more 8192 cap).
    // Without this, libraries with > 8192 symbols (Qt, webkit) silently
    // drop symbols past the cap.
    if (hash_vaddr != 0) {
        try {
            uint32_t nchain = 0;
            mem_.read(base + hash_vaddr + 4, &nchain, 4);
            obj.symtab_count = nchain;
        } catch (...) {
            obj.symtab_count = 0;  // fall back to old heuristic
        }
    }
    if (obj.symtab_count == 0) {
        // Fall back to old heuristic. Without DT_HASH we can't know the
        // exact count; DT_GNU_HASH would work but is more complex to parse.
        // 8192 is the old cap; index_symbols also uses it as a safety bound.
        obj.symtab_count = 8192;
    }
    // Read DT_SONAME (for dedup), DT_RPATH, DT_RUNPATH.
    // consulted. Now captured for use in find_library and dedup.
    if (soname_off != 0 && strtab_vaddr != 0) {
        // Read the soname string from the file bytes (the strtab may not
        // be relocated yet). Find the file offset of strtab_vaddr first.
        // Simple approach: read from guest memory after we've loaded
        // the segments. But parse_dynamic runs before relocations, so
        // the strtab IS already mapped (PT_LOAD covers it). Read from
        // guest memory.
        try {
            obj.soname = read_guest_cstr(mem_, obj.strtab_addr + soname_off);
        } catch (...) {}
    }
    // Helper to expand $ORIGIN in rpath/runpath.
    auto expand_origin = [&](const std::string& path) -> std::string {
        // $ORIGIN expands to the directory containing the ELF file.
        // For the main binary, that's the dir of main_path; for libs,
        // the dir of the lib's path (stored in obj.name).
        std::string dir;
        std::string path_str = obj.is_main ? obj.name : obj.name;
        size_t slash = path_str.rfind('/');
        if (slash != std::string::npos) dir = path_str.substr(0, slash);
        std::string result = path;
        const std::string token = "$ORIGIN";
        for (size_t pos = 0; (pos = result.find(token, pos)) != std::string::npos; ) {
            result.replace(pos, token.size(), dir);
            pos += dir.size();
        }
        // Also handle ${ORIGIN} form.
        const std::string token2 = "${ORIGIN}";
        for (size_t pos = 0; (pos = result.find(token2, pos)) != std::string::npos; ) {
            result.replace(pos, token2.size(), dir);
            pos += dir.size();
        }
        return result;
    };
    if (rpath_off != 0) {
        try {
            obj.rpath = expand_origin(read_guest_cstr(mem_, obj.strtab_addr + rpath_off));
        } catch (...) {}
    }
    if (runpath_off != 0) {
        try {
            obj.runpath = expand_origin(read_guest_cstr(mem_, obj.strtab_addr + runpath_off));
        } catch (...) {}
    }
    return true;
}
// ── register_ld_linux_shim_ ────────────────────────────────────────────
// Allocate a small data + code page in guest memory and populate it
// with synthetic versions of the symbols glibc's libc.so expects from
// ld-linux (the dynamic linker). Without these, libc's GOT slots for
// _rtld_global_ro, _rtld_global, _dl_argv, etc. stay at 0 (because
// no ld-linux was loaded), and libc crashes when it dereferences them
// during __libc_start_main.
//
// The shim provides:
//   - A writable 4 KiB data page with:
//       - _rtld_global_ro struct (zeroed — glibc reads feature flags
//         from here; all-zero means "no special features", which is
//         safe).
//       - _rtld_global struct (zeroed — glibc reads _dl_ns[0]._ns_nloaded
//         = 0 meaning "no libraries loaded via dlopen", which is fine
//         because we don't support dlopen yet).
//       - Storage for _dl_argv (initially NULL — libc sets it during
//         __libc_start_main).
//       - Storage for __libc_enable_secure (0 = not setuid).
//       - Storage for __pointer_chk_guard (random value — see below).
//       - Storage for _dl_start_args._dl_start_time (0).
//   - An executable 4 KiB code page with tiny ARM64 stub functions:
//       - _dl_find_dso_for_object: returns 0 (not found).
//       - _dl_allocate_tls: returns 0 (no dynamic TLS).
//       - _dl_allocate_tls_init: returns 0.
//       - _dl_deallocate_tls: no-op (RET).
//       - _dl_signal_error: calls abort() (which we resolve via
//         the global symbol table).
//       - _dl_signal_exception: same.
//       - _dl_catch_exception: returns 0 (no exception).
//       - _dl_catch_error: returns 0.
//       - _dl_audit_symbind_alt: no-op.
//       - _dl_audit_preinit: no-op.
//       - _dl_rtld_di_serinfo: returns 0.
//       - _dl_call_fini: no-op.
//       - __tls_get_addr: returns 0 (static TLS only; this is the
//         fallback for the rare case where the compiler emits a
//         __tls_get_addr call for a static-TLS variable).
//       - __tunable_get_val: returns 0 (no tunables).
//       - __nptl_change_stack_perm: no-op.
//
// Each stub is a sequence of ARM64 instructions:
//   mov x0, #0   ; 0xd2800000
//   ret          ; 0xd65f03c0
// (or just `ret` for void functions). They're laid out consecutively
// at the start of the code page, 8 bytes each (2 instructions).
//
// The data page is at shim_base_; the code page is at shim_base_+4096.
// Symbols are registered in the global symbol table so relocations
// resolve to the correct addresses.
bool DynamicLinker::register_ld_linux_shim_() {
    if (shim_base_ != 0) return true;  // already registered
    // Allocate 4 pages: 3 data pages + 1 code page.
    // The extra data pages are needed because glibc's _rtld_global_ro
    // struct is large (~4-8 KiB) and fields like dl_tls_static_size
    // are at high offsets (0x800-0x2000+). With only 1 data page,
    // reads past 0xFFF would hit the code page and get garbage.
    constexpr uint64_t SHIM_SIZE = 16384;       // 4 pages
    constexpr uint64_t DATA_SIZE = 12288;       // 3 data pages
    constexpr uint64_t CODE_PAGE_OFF = 12288;   // code at page 4
    shim_base_ = mem_.mmap_alloc(SHIM_SIZE);
    if (shim_base_ == 0) {
        error_ = "register_ld_linux_shim_: mmap_alloc failed";
        return false;
    }
    // ── Data area (shim_base_ .. shim_base_+DATA_SIZE) ────────────
    // Zero the entire data area (mmap_alloc already does this, but be
    // explicit in case the pages were reused from a previous allocation).
    std::vector<uint8_t> zero(DATA_SIZE, 0);
    mem_.write(shim_base_, zero.data(), DATA_SIZE);
    // Layout (offsets within the data page):
    //   0x000: _rtld_global_ro (glibc reads many fields at offsets up
    //          to ~0x1000+ from this struct; the function-pointer fields
    //          like _dl_signal_error are at low offsets and stay zero so
    //          glibc falls back to internal defaults).
    //   0x100: _rtld_global (256 bytes — glibc reads _dl_ns, _dl_nns).
    //   0x200: _dl_argv (8 bytes — pointer to argv; libc sets this).
    //   0x208: __libc_enable_secure (4 bytes — 0 = not secure).
    //   0x20C: __pointer_chk_guard (8 bytes — random XOR canary).
    //   0x214: _dl_start_args (16 bytes — start time, etc.).
    //   0x224: padding to 0x300.
    //   0x300: function pointer table (8 bytes each, 16 entries):
    //          [0] _dl_find_dso_for_object  → code page + 0
    //          [1] _dl_allocate_tls         → code page + 8
    //          [2] _dl_allocate_tls_init    → code page + 24
    //          [3] _dl_deallocate_tls       → code page + 32
    //          [4] _dl_signal_error         → code page + 40 (abort)
    //          [5] _dl_signal_exception     → code page + 48 (abort)
    //          [6] _dl_catch_exception      → code page + 56
    //          [7] _dl_catch_error          → code page + 64
    //          [8] _dl_audit_symbind_alt    → code page + 72
    //          [9] _dl_audit_preinit        → code page + 80
    //          [10] _dl_rtld_di_serinfo     → code page + 88
    //          [11] _dl_call_fini           → code page + 96
    //          [12] __tls_get_addr          → code page + 104
    //          [13] __tunable_get_val       → code page + 112
    //          [14] __nptl_change_stack_perm→ code page + 120
    //          [15] (reserved)
    //   _rtld_global_ro._dl_signal_error etc. point into this table.
    constexpr uint64_t RTLD_GLOBAL_RO_OFF = 0x000;
    constexpr uint64_t RTLD_GLOBAL_OFF    = 0x100;
    constexpr uint64_t DL_ARGV_OFF        = 0x200;
    constexpr uint64_t LIBC_ENABLE_SECURE_OFF = 0x208;
    constexpr uint64_t POINTER_CHK_GUARD_OFF = 0x20C;
    constexpr uint64_t FPTR_TABLE_OFF     = 0x300;
    // Generate a random __pointer_chk_guard value. glibc uses this as
    // a stack-protector canary; we use /dev/urandom for entropy.
    uint64_t chk_guard = 0;
    FILE* ur = fopen("/dev/urandom", "rb");
    if (ur) {
        if (fread(&chk_guard, sizeof(chk_guard), 1, ur) != 1) chk_guard = 0;
        fclose(ur);
    }
    if (chk_guard == 0) chk_guard = 0xDEADBEEFCAFEBABEULL;
    mem_.store<uint64_t>(shim_base_ + POINTER_CHK_GUARD_OFF, chk_guard);
    // ── Populate _rtld_global_ro TLS fields ──────────────────────────
    // glibc's pthread_create → allocate_stack → _dl_allocate_tls_storage
    // reads GLRO(dl_tls_static_size) and GLRO(dl_tls_static_align) to
    // determine how much memory to allocate for a new thread's TLS block.
    // Without these fields set, _dl_allocate_tls_storage allocates 0
    // bytes and allocate_stack hits `assert(size != 0)`.
    //
    // The exact offset of dl_tls_static_size within struct
    // rtld_global_ro varies by glibc version (it's after the large
    // _dl_ns[DL_NNS] array, typically at offset 0x800-0x1800). Rather
    // than hardcode a version-specific offset, we populate a RANGE of
    // candidate offsets with the correct values. The fields are:
    //   dl_tls_static_nelem  (size_t) — number of TLS elements
    //   dl_tls_static_size   (size_t) — total static TLS size
    //   dl_tls_static_used   (size_t) — used portion
    //   dl_tls_static_surplus(size_t) — surplus for alignment
    //   dl_tls_static_align  (size_t) — alignment
    //   dl_tls_max_dtv_idx   (size_t) — max DTV index (= # TLS modules)
    //
    // We set all size_t slots in the upper data area (offsets 0x400
    // through 0x2FF8 — past the function-pointer table, up to the code
    // page boundary) to the correct TLS size value. For alignment
    // fields, we use 64 (TLS_TCB_ALIGN on AArch64).
    // This is a "spray" approach — it's not surgical, but it's robust
    // across glibc versions. The function pointers at lower offsets
    // (0x000-0x3FF) are NOT overwritten — they remain zero so glibc
    // falls back to internal defaults.
    if (static_tls_size_ > 0) {
        // dl_tls_static_size includes the TLS data + TCB + struct pthread +
        // surplus. glibc's allocate_stack allocates this many bytes per
        // thread at the top of the stack. The TCB (tcbhead_t) is at the
        // END of this region, and struct pthread extends below it.
        //
        // On glibc AArch64, sizeof(struct pthread) ≈ 2304 bytes. We must
        // include this in dl_tls_static_size so glibc allocates enough
        // space for struct pthread + TLS data + TCB.
        //
        // (lib + tcb + main) + 2KB + 16KB surplus. This was too small —
        // glibc's struct pthread (~2.3KB) overflowed into the TLS data
        // area, causing the "got = expected/4" TLS corruption pattern
        // with 8+ threads. Now we add sizeof(struct pthread) explicitly.
        constexpr uint64_t TLS_SURPLUS = 16384;
        constexpr uint64_t STRUCT_PTHREAD_SIZE = 2304;  // glibc AArch64
        uint64_t tls_static_size = static_tls_size_ + STRUCT_PTHREAD_SIZE +
                                   2048 + TLS_SURPLUS;
        // Round up to alignment (64 bytes).
        tls_static_size = (tls_static_size + 63) & ~63ULL;
        // Store for later use (post-relocation patching of _rtld_global_ro).
        pending_tls_static_size_ = tls_static_size;
    }
    // ── Code page (shim_base_+CODE_PAGE_OFF .. shim_base_+SHIM_SIZE) ─
    // Each stub is 2 instructions (8 bytes), EXCEPT _dl_allocate_tls
    // which is 4 instructions (16 bytes) because it calls back into
    // the emulator via a bifrost-specific syscall to allocate a real
    // per-thread TLS block (see syscall 0x1001 in misc.cpp).
    //
    // Stubs that "abort" actually call abort() — we resolve abort via
    // the global symbol table at registration time (it's a libc symbol
    // that's already indexed). If abort isn't found yet (e.g., shim
    // registered before libc loaded), the abort stubs just BRK #1000
    // (visible crash) instead.
    uint64_t code_base = shim_base_ + CODE_PAGE_OFF;
    // ARM64 instruction encodings (little-endian byte order):
    //   mov x0, #0        → 0xD2800000  (MOVZ x0, #0)
    //   ret               → 0xD65F03C0
    //   brk #1000         → 0xD4207D00  (musl's a_crash)
    //   movz x8, #0x1001  → 0xD2820028  (bifrost TLS-alloc syscall nr)
    //   svc #0            → 0xD4000001
    //   nop               → 0xD503201F
    auto emit_mov_x0_0 = [](std::vector<uint8_t>& v) {
        v.push_back(0x00); v.push_back(0x00); v.push_back(0x80); v.push_back(0xD2);
    };
    auto emit_ret = [](std::vector<uint8_t>& v) {
        v.push_back(0xC0); v.push_back(0x03); v.push_back(0x5F); v.push_back(0xD6);
    };
    auto emit_brk_1000 = [](std::vector<uint8_t>& v) {
        v.push_back(0x00); v.push_back(0x7D); v.push_back(0x20); v.push_back(0xD4);
    };
    auto emit_nop = [](std::vector<uint8_t>& v) {
        v.push_back(0x1F); v.push_back(0x20); v.push_back(0x03); v.push_back(0xD5);
    };
    auto emit_stub_return0 = [&](std::vector<uint8_t>& v) {
        emit_mov_x0_0(v);
        emit_ret(v);
    };
    auto emit_stub_void = [&](std::vector<uint8_t>& v) {
        emit_ret(v);
        emit_nop(v);  // pad to 8 bytes
    };
    auto emit_stub_abort = [&](std::vector<uint8_t>& v) {
        emit_brk_1000(v);
        emit_nop(v);
    };
    // _dl_allocate_tls syscall stub (16 bytes = 4 instructions):
    //   movz x8, #0x1001   ; bifrost TLS-alloc syscall number
    //   svc  #0            ; call the emulator
    //   ret                ; return (x0 = TCB pointer from emulator)
    //   nop                ; pad to 16 bytes (next stub is 16-aligned)
    //
    // This replaces the old "return 0" stub. Returning 0 caused glibc's
    // allocatestack.c to hit `assert (size != 0)` because _dl_allocate_tls
    // returned NULL, and glibc treated that as a zero-size allocation.
    // The real fix is to actually allocate a per-thread TLS block + TCB,
    // copy the static TLS template, and return the TCB pointer. The
    // syscall handler (case 0x1001 in misc.cpp) does this.
    auto emit_tls_alloc_stub = [&](std::vector<uint8_t>& v) {
        // movz x8, #0x1001  →  0xD2820028
        // Encoding: D2800000 | (hw<<21) | (imm16<<5) | Rd
        //   hw=0 (no shift), imm16=0x1001, Rd=8 (x8)
        //   = 0xD2800000 | 0 | (0x1001<<5) | 8 = 0xD2820028
        // Little-endian bytes: 28 00 82 D2
        //
        // typo made syscall 0x1001 never fire. Now it fires on every
        // _dl_allocate_tls AND _dl_allocate_tls_init call (both stubs use
        // this emitter). The syscall handler copies lib TLS to
        // [tcb-lib_size, tcb) and ZEROS [tcb, tcb+main_memsz) to clear
        // stale .tbss data on stack-cache reuse. This is safe because
        // _dl_allocate_tls/init run BEFORE create_thread sets TCB fields
        // (tcb, self, stack_guard, etc.) — glibc will overwrite the
        // zeroed TCB-area bytes with correct values afterwards.
        v.push_back(0x28); v.push_back(0x00); v.push_back(0x82); v.push_back(0xD2);
        // svc #0           →  0xD4000001
        v.push_back(0x01); v.push_back(0x00); v.push_back(0x00); v.push_back(0xD4);
        // ret              →  0xD65F03C0
        v.push_back(0xC0); v.push_back(0x03); v.push_back(0x5F); v.push_back(0xD6);
        // nop (pad to 16 bytes)
        emit_nop(v);
    };
    std::vector<uint8_t> code;
    code.reserve(256);
    // Stub layout — _dl_allocate_tls and _dl_allocate_tls_init are 16 bytes;
    // all others are 8.
    // Offsets are tracked via named constants so the FPTR table and
    // symbol registrations stay in sync.
    constexpr uint32_t OFF_DSO     = 0;    // _dl_find_dso_for_object
    constexpr uint32_t OFF_TLS     = 8;    // _dl_allocate_tls (16 bytes)
    constexpr uint32_t OFF_TLSINIT = 24;   // _dl_allocate_tls_init (16 bytes)
    constexpr uint32_t OFF_TLSFREE = 40;   // _dl_deallocate_tls
    constexpr uint32_t OFF_SIGERR  = 48;   // _dl_signal_error
    constexpr uint32_t OFF_SIGEXC  = 56;   // _dl_signal_exception
    constexpr uint32_t OFF_CEXC    = 64;   // _dl_catch_exception
    constexpr uint32_t OFF_CERR    = 72;   // _dl_catch_error
    constexpr uint32_t OFF_SBA     = 80;   // _dl_audit_symbind_alt
    constexpr uint32_t OFF_PREINIT = 88;   // _dl_audit_preinit
    constexpr uint32_t OFF_SERINFO = 96;   // _dl_rtld_di_serinfo
    constexpr uint32_t OFF_FINI    = 104;  // _dl_call_fini
    constexpr uint32_t OFF_TLSADDR = 112;  // __tls_get_addr
    constexpr uint32_t OFF_TUNABLE = 120;  // __tunable_get_val
    constexpr uint32_t OFF_STACKPERM = 128; // __nptl_change_stack_perm
    constexpr uint32_t OFF_DLOPEN   = 136; // _dl_open (16 bytes — calls syscall 0x1002)
    constexpr uint32_t OFF_DLSYM    = 152; // _dl_sym (16 bytes — calls syscall 0x1003)
    constexpr uint32_t OFF_DLCLOSE  = 168; // _dl_close (8 bytes — return 0 stub)
    constexpr uint32_t OFF_DLITER   = 176; // dl_iterate_phdr (16 bytes — calls syscall 0x1007)
    constexpr uint32_t OFF_DLADDR   = 192; // dladdr (16 bytes — calls syscall 0x1005)
    // [0] _dl_find_dso_for_object (returns void*)
    //     Returns 0 (not found). glibc's dladdr and _dl_open use this
    //     to find the containing DSO for a given address. Returning 0
    //     makes glibc skip link_map validation, which is safer than
    //     returning a base address that glibc would misinterpret as a
    //     struct link_map*.
    //     The real implementation (find_object_by_addr) is available
    //     via syscall 0x1006 for internal use, but the glibc-visible
    //     _dl_find_dso_for_object stub returns 0 for compatibility.
    emit_stub_return0(code);     // offset 0
    // [1] _dl_allocate_tls (16 bytes — calls syscall 0x1001)
    emit_tls_alloc_stub(code);   // offset 8 (16 bytes)
    // [2] _dl_allocate_tls_init (16 bytes — also calls syscall 0x1001)
    //     _dl_allocate_tls is NOT called). Without this, stale .tbss data
    //     from the previous thread would persist. Both stubs use the same
    //     emit_tls_alloc_stub, so syscall 0x1001 fires for both new and
    //     reused stacks.
    emit_tls_alloc_stub(code);   // offset 24 (16 bytes)
    // [3] _dl_deallocate_tls (void)
    emit_stub_void(code);        // offset 32
    // [4] _dl_signal_error (noreturn)
    emit_stub_abort(code);       // offset 40
    // [5] _dl_signal_exception (noreturn)
    emit_stub_abort(code);       // offset 48
    // [6] _dl_catch_exception (returns int)
    emit_stub_return0(code);     // offset 56
    // [7] _dl_catch_error (returns int)
    emit_stub_return0(code);     // offset 64
    // [8] _dl_audit_symbind_alt (void)
    emit_stub_void(code);        // offset 72
    // [9] _dl_audit_preinit (void)
    emit_stub_void(code);        // offset 80
    // [10] _dl_rtld_di_serinfo (returns int)
    emit_stub_return0(code);     // offset 88
    // [11] _dl_call_fini (void)
    emit_stub_void(code);        // offset 96
    // [12] __tls_get_addr (returns void*)
    emit_stub_return0(code);     // offset 104
    // [13] __tunable_get_val (returns int)
    emit_stub_return0(code);     // offset 112
    // [14] __nptl_change_stack_perm (void)
    emit_stub_void(code);        // offset 120
    // [15] _dl_open (16 bytes — calls syscall 0x1002 for dlopen support)
    //     struct at _rtld_global_ro + 368, offset +72. The stub calls
    //     syscall 0x1002 which loads the library via DynamicLinker.
    {
        // movz x8, #0x1002  →  0xD2820048
        code.push_back(0x48); code.push_back(0x00); code.push_back(0x82); code.push_back(0xD2);
        // svc #0           →  0xD4000001
        code.push_back(0x01); code.push_back(0x00); code.push_back(0x00); code.push_back(0xD4);
        // ret              →  0xD65F03C0
        code.push_back(0xC0); code.push_back(0x03); code.push_back(0x5F); code.push_back(0xD6);
        // nop (pad to 16 bytes)
        emit_nop(code);
    }
    // [16] _dl_sym (16 bytes — calls syscall 0x1003 for dlsym support)
    //     dlsym@@GLIBC_2.34 reads *(hook+16) for _dl_sym.
    {
        // movz x8, #0x1003  →  0xD2820068
        code.push_back(0x68); code.push_back(0x00); code.push_back(0x82); code.push_back(0xD2);
        // svc #0           →  0xD4000001
        code.push_back(0x01); code.push_back(0x00); code.push_back(0x00); code.push_back(0xD4);
        // ret              →  0xD65F03C0
        code.push_back(0xC0); code.push_back(0x03); code.push_back(0x5F); code.push_back(0xD6);
        // nop (pad to 16 bytes)
        emit_nop(code);
    }
    // [17] _dl_close (8 bytes — stub: return 0)
    //     dlclose reads *(hook+8). We return 0 (success) without
    //     actually unloading the library. This is safe because:
    //     1. glibc's _dl_open wrapper calls _dl_close internally if it
    //        detects the handle isn't a valid link_map struct (which it
    //        isn't — we return base_addr as the handle, not a link_map*).
    //        Returning 0 makes glibc think cleanup succeeded.
    //     2. The user's dlclose() also goes through this stub. Returning
    //        0 means "success" — the library stays mapped (glibc does
    //        the same for nodelete libraries). This is safe for an
    //        emulator where processes are short-lived.
    //     The close_library() method exists for future use (e.g. if we
    //     implement real link_map support and can distinguish internal
    //     vs. user dlclose calls).
    emit_stub_return0(code);  // offset 168 (OFF_DLCLOSE)
    // [18] dl_iterate_phdr (16 bytes — calls syscall 0x1007)
    //     glibc's dl_iterate_phdr walks the link_map list, which we don't
    //     have. We override it with a stub that calls syscall 0x1007,
    //     which invokes our iterate_phdr() implementation. The stub
    //     passes x0 (callback) and x1 (data) through to the syscall.
    {
        // movz x8, #0x1007  →  0xD28200E8
        code.push_back(0xE8); code.push_back(0x00); code.push_back(0x82); code.push_back(0xD2);
        // svc #0           →  0xD4000001
        code.push_back(0x01); code.push_back(0x00); code.push_back(0x00); code.push_back(0xD4);
        // ret              →  0xD65F03C0
        code.push_back(0xC0); code.push_back(0x03); code.push_back(0x5F); code.push_back(0xD6);
        // nop (pad to 16 bytes)
        emit_nop(code);
    }
    // [19] dladdr (16 bytes — calls syscall 0x1005)
    //     Used by the dlfcn_hook (hook+40). glibc's dladdr@@GLIBC_2.34
    //     checks the hook and calls hook->dladdr. We point hook+40 to
    //     this stub, which calls our dladdr implementation via syscall
    //     0x1005. The syscall fills in the Dl_info struct and returns
    //     1 (found) or 0 (not found).
    {
        // movz x8, #0x1005  →  0xD28200A8
        code.push_back(0xA8); code.push_back(0x00); code.push_back(0x82); code.push_back(0xD2);
        // svc #0           →  0xD4000001
        code.push_back(0x01); code.push_back(0x00); code.push_back(0x00); code.push_back(0xD4);
        // ret              →  0xD65F03C0
        code.push_back(0xC0); code.push_back(0x03); code.push_back(0x5F); code.push_back(0xD6);
        // nop (pad to 16 bytes)
        emit_nop(code);
    }
    // Pad to page size.
    code.resize(4096, 0x1F);  // NOP-fill the rest (0xD503201F LE)
    // Write the code page.
    mem_.write(code_base, code.data(), 4096);
    // ── Populate the function pointer table in the data page ──────
    // _rtld_global_ro has fields at specific offsets that glibc reads
    // to find _dl_signal_error, _dl_catch_error, etc. Rather than
    // replicate the exact struct layout (which varies by glibc
    // version), we point the FPTR table at the code stubs and let
    // _rtld_global_ro's fields be zero (glibc will fall back to
    // internal defaults or skip the call if the field is 0). This is
    // the same approach glibc itself uses when loaded by a non-glibc
    // dynamic linker (e.g., for static-PIE binaries).
    //
    // The FPTR table is mostly for our own bookkeeping — if a future
    // glibc version reads a function pointer from _rtld_global_ro
    // at a specific offset, we can wire it up here.
    uint32_t fptr_offsets[15] = {
        OFF_DSO, OFF_TLS, OFF_TLSINIT, OFF_TLSFREE,
        OFF_SIGERR, OFF_SIGEXC, OFF_CEXC, OFF_CERR,
        OFF_SBA, OFF_PREINIT, OFF_SERINFO, OFF_FINI,
        OFF_TLSADDR, OFF_TUNABLE, OFF_STACKPERM,
    };
    for (int i = 0; i < 15; i++) {
        mem_.store<uint64_t>(shim_base_ + FPTR_TABLE_OFF + i * 8,
                             code_base + fptr_offsets[i]);
    }
    // ── dlopen hook struct ─────────────────────────────────────────
    // glibc's __libc_dlopen_mode reads _dl_open from a hook struct:
    //   1. ldr x2, [rtld_global_ro + 368]  → hook struct pointer
    //   2. ldr x2, [x2 + 72]               → _dl_open function pointer
    //   3. blr x2                          → call _dl_open
    // We allocate a 128-byte hook struct in the shim data area (at
    // offset 0x800, past the FPTR table) and set hook+72 = _dl_open stub.
    // After patch_rtld_global_ro_ resolves _rtld_global_ro's address,
    // we write hook_ptr to rtld_global_ro + 368.
    // ── dlopen hook struct ──────────────────────────────────────────
    // glibc reads _dl_open_hook from _rtld_global_ro + 368.
    // The hook struct has function pointers at various offsets:
    //   +0:  _dl_open  (dlopen@@GLIBC_2.34)
    //   +8:  _dl_close (dlclose@@GLIBC_2.34)
    //   +16: _dl_sym   (dlsym@@GLIBC_2.34)
    //   +72: _dl_open  (__libc_dlopen_mode)
    // The dlfcn_hook struct also has slots for dlvsym(+24), dlerror(+32),
    // dladdr(+40), dladdr1(+48), dlinfo(+56), dlmopen(+64). If these are
    // left as NULL, glibc's dladdr@@GLIBC_2.34 checks hook->dladdr (non-
    // NULL because the hook struct itself is non-NULL) and calls 0x0,
    // crashing. We fill all unused slots with the return-0 stub (OFF_DSO,
    // which is the _dl_find_dso_for_object stub that just returns 0).
    constexpr uint64_t DLOPEN_HOOK_OFF = 0x800;
    constexpr uint64_t DLOPEN_HOOK_SIZE = 128;
    {
        std::vector<uint8_t> zeros(DLOPEN_HOOK_SIZE, 0);
        mem_.write(shim_base_ + DLOPEN_HOOK_OFF, zeros.data(), DLOPEN_HOOK_SIZE);
    }
    mem_.store<uint64_t>(shim_base_ + DLOPEN_HOOK_OFF + 0,  code_base + OFF_DLOPEN);
    mem_.store<uint64_t>(shim_base_ + DLOPEN_HOOK_OFF + 8,  code_base + OFF_DLCLOSE);
    mem_.store<uint64_t>(shim_base_ + DLOPEN_HOOK_OFF + 16, code_base + OFF_DLSYM);
    // Fill unused dlfcn_hook slots with the return-0 stub to prevent
    // crashes when glibc calls dladdr/dlerror/dlinfo/dlmopen/dlvsym
    // through the hook.
    mem_.store<uint64_t>(shim_base_ + DLOPEN_HOOK_OFF + 24, code_base + OFF_DSO); // dlvsym
    mem_.store<uint64_t>(shim_base_ + DLOPEN_HOOK_OFF + 32, code_base + OFF_DSO); // dlerror
    mem_.store<uint64_t>(shim_base_ + DLOPEN_HOOK_OFF + 40, code_base + OFF_DLADDR); // dladdr (real)
    mem_.store<uint64_t>(shim_base_ + DLOPEN_HOOK_OFF + 48, code_base + OFF_DSO); // dladdr1
    mem_.store<uint64_t>(shim_base_ + DLOPEN_HOOK_OFF + 56, code_base + OFF_DSO); // dlinfo
    mem_.store<uint64_t>(shim_base_ + DLOPEN_HOOK_OFF + 64, code_base + OFF_DSO); // dlmopen
    mem_.store<uint64_t>(shim_base_ + DLOPEN_HOOK_OFF + 72, code_base + OFF_DLOPEN);
    dlopen_hook_ptr_ = shim_base_ + DLOPEN_HOOK_OFF;
    // ── dlerror string buffer ──────────────────────────────────────
    // A 256-byte guest buffer for dlerror strings. get_last_error()
    // copies the host-side error string here and returns the guest
    // pointer. Placed at offset 0x900 (past the dlopen hook at 0x800).
    constexpr uint64_t DLERROR_BUF_OFF = 0x900;
    constexpr uint64_t DLERROR_BUF_SZ = 256;
    {
        std::vector<uint8_t> zeros(DLERROR_BUF_SZ, 0);
        mem_.write(shim_base_ + DLERROR_BUF_OFF, zeros.data(), DLERROR_BUF_SZ);
    }
    dlerror_buf_ptr_ = shim_base_ + DLERROR_BUF_OFF;
    // ── Register symbols in the global symbol table ──────────────
    // Data symbols point into the data page; function symbols point
    // into the code page.
    // assigned `symbols_[name] = ...`, which would override a real
    // ld-linux symbol if one was already indexed from .dynsym. Now we
    // only insert if no prior definition exists, matching index_symbols'
    // "first strong wins" semantics.
    auto add_data_sym = [&](const char* name, uint64_t off) {
        if (symbols_.count(name) == 0) {
            symbols_[name] = SymEntry{shim_base_ + off, STB_GLOBAL_};
        }
    };
    auto add_func_sym = [&](const char* name, uint64_t off) {
        if (symbols_.count(name) == 0) {
            symbols_[name] = SymEntry{code_base + off, STB_GLOBAL_};
        }
    };
    // Data symbols (from ld-linux that libc references).
    add_data_sym("_rtld_global_ro", RTLD_GLOBAL_RO_OFF);
    add_data_sym("_rtld_global",    RTLD_GLOBAL_OFF);
    add_data_sym("_dl_argv",        DL_ARGV_OFF);
    add_data_sym("__libc_enable_secure", LIBC_ENABLE_SECURE_OFF);
    add_data_sym("__pointer_chk_guard",  POINTER_CHK_GUARD_OFF);
    // Function symbols (from ld-linux that libc references).
    // Offsets match the fptr_offsets[] table above.
    add_func_sym("_dl_find_dso_for_object", OFF_DSO);
    add_func_sym("_dl_allocate_tls",        OFF_TLS);
    add_func_sym("_dl_allocate_tls_init",   OFF_TLSINIT);
    add_func_sym("_dl_deallocate_tls",      OFF_TLSFREE);
    add_func_sym("_dl_signal_error",        OFF_SIGERR);
    add_func_sym("_dl_signal_exception",    OFF_SIGEXC);
    add_func_sym("_dl_catch_exception",     OFF_CEXC);
    add_func_sym("_dl_catch_error",         OFF_CERR);
    add_func_sym("_dl_audit_symbind_alt",   OFF_SBA);
    add_func_sym("_dl_audit_preinit",       OFF_PREINIT);
    add_func_sym("_dl_rtld_di_serinfo",     OFF_SERINFO);
    add_func_sym("_dl_call_fini",           OFF_FINI);
    add_func_sym("__tls_get_addr",          OFF_TLSADDR);
    add_func_sym("__tunable_get_val",       OFF_TUNABLE);
    add_func_sym("__nptl_change_stack_perm", OFF_STACKPERM);
    // Override glibc's dl_iterate_phdr with our stub that calls syscall
    // 0x1007. glibc's implementation walks the link_map list, which we
    // don't have. We use FORCE override (not add_func_sym which is
    // first-define-wins) because libc.so.6 already defines
    // dl_iterate_phdr@@GLIBC_2.17.
    symbols_["dl_iterate_phdr"] = SymEntry{code_base + OFF_DLITER, STB_GLOBAL_};
    versioned_symbols_["dl_iterate_phdr@GLIBC_2.17"] = SymEntry{code_base + OFF_DLITER, STB_GLOBAL_};
    // Override glibc's dladdr with our stub that calls syscall 0x1005.
    // glibc's dladdr@@GLIBC_2.34 checks the dlfcn_hook (hook+40, which
    // we already point at OFF_DLADDR) AND may be called directly. We
    // use FORCE override (same pattern as dl_iterate_phdr above) so the
    // symbol resolves to our stub regardless of which call path is used.
    // The stub calls our dladdr() implementation via syscall 0x1005,
    // which fills in the Dl_info struct and returns 1 (found) or 0.
    //
    // History: this override was previously disabled because an earlier
    // attempt crashed glibc startup (the versioned-symbol override
    // conflicted with glibc's internal _dl_addr path). The dlfcn_hook
    // wiring (hook+40 → OFF_DLADDR) was added later and made the
    // user-facing dladdr() work, but the symbol itself stayed
    // unresolved-to-glibc. Re-enabling the symbol override now routes
    // ALL dladdr calls (including _dl_addr's internal use) through our
    // stub, which returns 0 gracefully for unknown addresses — so
    // glibc's internal callers see "not found" and skip link_map
    // validation, which is safe.
    symbols_["dladdr"] = SymEntry{code_base + OFF_DLADDR, STB_GLOBAL_};
    versioned_symbols_["dladdr@GLIBC_2.34"] = SymEntry{code_base + OFF_DLADDR, STB_GLOBAL_};
    versioned_symbols_["dladdr@GLIBC_2.0"] = SymEntry{code_base + OFF_DLADDR, STB_GLOBAL_};
    // _dl_allocate_tls_init with our shim's stubs, even if the real
    // ld-linux already defined them in .dynsym. The real ld-linux's
    // _dl_allocate_tls -> allocate_dtv calls calloc via a function
    // pointer (_rtld_global._dl_calloc / the slot at ld-linux+0x3fb18)
    // that is only populated during ld-linux's own _dl_start startup,
    // which we bypass (we use our own dynamic linker). With that slot
    // NULL, allocate_dtv does `blr x2` with x2=0 -> decode error at
    // pc=0x0. Our shim's _dl_allocate_tls instead traps into the
    // emulator via syscall 0x1001 (see misc.cpp case 0x1001), which
    // allocates the per-thread TLS block + TCB, copies the static TLS
    // template, copies the TCB canary fields, and returns the TCB
    // pointer — all without touching ld-linux's uninitialized internal
    // state. _dl_allocate_tls_init returns its argument unchanged
    // (the TLS template copy is already done by the syscall handler).
    //
    // We override BOTH the unversioned table (symbols_) AND the
    // versioned table (versioned_symbols_, keyed "name@GLIBC_PRIVATE")
    // because libc's JUMP_SLOT relocations against these symbols are
    // versioned (@GLIBC_PRIVATE), and resolve_versioned_symbol()
    // consults versioned_symbols_ first. Without the versioned
    // override, libc's PLT call would still resolve to the real
    // ld-linux's _dl_allocate_tls. (Other real ld-linux symbols like
    // _rtld_global remain first-define-wins via add_data_sym above.)
    symbols_["_dl_allocate_tls"]      = SymEntry{code_base + OFF_TLS,     STB_GLOBAL_};
    symbols_["_dl_allocate_tls_init"] = SymEntry{code_base + OFF_TLSINIT, STB_GLOBAL_};
    versioned_symbols_["_dl_allocate_tls@GLIBC_PRIVATE"]      = SymEntry{code_base + OFF_TLS,     STB_GLOBAL_};
    versioned_symbols_["_dl_allocate_tls_init@GLIBC_PRIVATE"] = SymEntry{code_base + OFF_TLSINIT, STB_GLOBAL_};
    // Also register a synthetic LoadedObject so the shim shows up in
    // /proc/self/maps and the allocations_ tracker (for fork safety).
    LoadedObject shim_obj;
    shim_obj.name = "<ld-linux-shim>";
    shim_obj.base_addr = shim_base_;
    shim_obj.is_main = false;
    shim_obj.dyn_addr = 0;  // no PT_DYNAMIC
    objects_.push_back(std::move(shim_obj));
    if (dynlink_trace_enabled()) {
        fprintf(stderr, "[dynlink] registered ld-linux shim: "
                "data @0x%llx, code @0x%llx (15 stubs)\n",
                static_cast<unsigned long long>(shim_base_),
                static_cast<unsigned long long>(code_base));
    }
    return true;
}
// ── find_library ───────────────────────────────────────────────────────
// parent object's DT_RUNPATH/DT_RPATH) and search them BEFORE the
// standard multiarch paths. This lets games that bundle their own libs
// (via DT_RUNPATH=$ORIGIN/lib) actually find them.
std::vector<uint8_t> DynamicLinker::find_library(const std::string& soname,
                                                 std::string& found_path,
                                                 const std::string& parent_runpath,
                                                 const std::string& parent_rpath) {
    // Search order (matches Linux ld.so behavior for AArch64 multiarch):
    //   0. Parent object's DT_RPATH (deprecated, global) — searched FIRST
    //      per the gABI (only if no DT_RUNPATH was seen in any object).
    //      We approximate by searching it before RUNPATH.
    //   0.5. Parent object's DT_RUNPATH (per-object, $ORIGIN-expanded).
    //   1. BIFROST_ROOT sandbox (if set) — $BIFROST_ROOT/lib and
    //      $BIFROST_ROOT/usr/lib. This lets the user provide a self-
    //      contained rootfs without polluting the host's multiarch dirs.
    //   2. LD_LIBRARY_PATH entries (user override).
    //   3. /usr/aarch64-linux-gnu/lib/         (Debian/Ubuntu multiarch)
    //   4. /usr/lib/aarch64-linux-gnu/         (newer Debian multiarch)
    //   5. /lib/aarch64-linux-gnu/             (Debian multiarch)
    //   6. /usr/lib/                            (host libs, fallback)
    //   7. /lib
    //   8. Bundled toolchain libs (auto-detected at startup, see below)
    //
    // The bundled toolchain paths (./tools/aarch64-linux-gnu-cross/...
    // and ./tools/aarch64-linux-musl-cross/...) are checked LAST so the
    // user can override with BIFROST_ROOT or LD_LIBRARY_PATH. They're
    // included so dynamically-linked test binaries work out-of-the-box
    // after fetching the toolchains, without requiring the user to set
    // up a rootfs or install aarch64 multiarch packages on the host.
    std::vector<std::string> dirs;
    // 0. Parent's DT_RPATH (semicolon-separated).
    if (!parent_rpath.empty()) {
        std::string s = parent_rpath;
        size_t pos = 0;
        while (pos < s.size()) {
            size_t colon = s.find(':', pos);
            if (colon == std::string::npos) {
                dirs.push_back(s.substr(pos));
                break;
            }
            dirs.push_back(s.substr(pos, colon - pos));
            pos = colon + 1;
        }
    }
    // 0.5. Parent's DT_RUNPATH (semicolon-separated).
    if (!parent_runpath.empty()) {
        std::string s = parent_runpath;
        size_t pos = 0;
        while (pos < s.size()) {
            size_t colon = s.find(':', pos);
            if (colon == std::string::npos) {
                dirs.push_back(s.substr(pos));
                break;
            }
            dirs.push_back(s.substr(pos, colon - pos));
            pos = colon + 1;
        }
    }
    // 1. BIFROST_ROOT sandbox.
    if (const char* root = getenv("BIFROST_ROOT")) {
        std::string r(root);
        // Strip trailing slash(es) for clean concatenation.
        while (r.size() > 1 && r.back() == '/') r.pop_back();
        if (!r.empty()) {
            dirs.push_back(r + "/lib");
            dirs.push_back(r + "/lib64");
            dirs.push_back(r + "/usr/lib");
            dirs.push_back(r + "/usr/lib64");
        }
    }
    // 2. LD_LIBRARY_PATH.
    if (const char* llp = getenv("LD_LIBRARY_PATH")) {
        std::string s = llp;
        size_t pos = 0;
        while (pos < s.size()) {
            size_t colon = s.find(':', pos);
            if (colon == std::string::npos) {
                dirs.push_back(s.substr(pos));
                break;
            }
            dirs.push_back(s.substr(pos, colon - pos));
            pos = colon + 1;
        }
    }
    // 3-7. Standard host multiarch paths.
    dirs.push_back("/usr/aarch64-linux-gnu/lib");
    dirs.push_back("/usr/lib/aarch64-linux-gnu");
    dirs.push_back("/lib/aarch64-linux-gnu");
    dirs.push_back("/usr/lib");
    dirs.push_back("/lib");
    // 3.5. Android-compatible library paths.
    // Android games and Android-ported apps look for shared libraries
    // in /system/lib64 and /vendor/lib64. When BIFROST_ROOT is set,
    // these resolve to $BIFROST_ROOT/system/lib64 etc. (created by
    // setup-rootfs.sh as symlinks to ../lib64). Adding them here lets
    // Android-style DT_NEEDED entries (e.g., "libGLESv2.so") resolve
    // from the rootfs.
    if (const char* root = getenv("BIFROST_ROOT")) {
        std::string r(root);
        while (r.size() > 1 && r.back() == '/') r.pop_back();
        if (!r.empty()) {
            dirs.push_back(r + "/system/lib");
            dirs.push_back(r + "/system/lib64");
            dirs.push_back(r + "/vendor/lib");
            dirs.push_back(r + "/vendor/lib64");
        }
    }
    // 8. Bundled toolchain libs (auto-detected relative to the
    //    executable's directory, so it works regardless of CWD).
    //    We use /proc/self/exe to find the executable's path, then
    //    look for tools/aarch64-{linux-gnu,linux-musl}-cross/...
    //    relative to that.
    {
        char exe_path[4096];
        ssize_t n = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
        if (n > 0) {
            exe_path[n] = '\0';
            std::string exe(exe_path);
            // Walk up to the project root (the directory containing
            // the 'tools' subdir). The executable is typically at
            // <project>/bifrost-emu, so the project root is its parent.
            size_t slash = exe.rfind('/');
            if (slash != std::string::npos) {
                std::string project_root = exe.substr(0, slash);
                dirs.push_back(project_root +
                    "/tools/aarch64-linux-gnu-cross/aarch64-none-linux-gnu/libc/lib64");
                dirs.push_back(project_root +
                    "/tools/aarch64-linux-gnu-cross/aarch64-none-linux-gnu/libc/lib");
                dirs.push_back(project_root +
                    "/tools/aarch64-linux-gnu-cross/aarch64-none-linux-gnu/libc/usr/lib64");
                dirs.push_back(project_root +
                    "/tools/aarch64-linux-musl-cross/aarch64-linux-musl/lib");
            }
        }
    }
    for (const auto& dir : dirs) {
        std::string path = dir + "/" + soname;
        struct stat st;
        if (::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            std::ifstream f(path, std::ios::binary | std::ios::ate);
            if (!f) continue;
            std::streamsize sz = f.tellg();
            f.seekg(0);
            std::vector<uint8_t> data(sz);
            if (!f.read(reinterpret_cast<char*>(data.data()), sz)) continue;
            found_path = path;
            return data;
        }
    }
    return {};
}
// ── map_segments ───────────────────────────────────────────────────────
uint64_t DynamicLinker::map_segments(const std::vector<uint8_t>& data,
                                     uint64_t base, uint64_t& entry) {
    if (data.size() < 64) return 0;
    uint64_t e_entry, e_phoff;
    uint16_t e_phentsize, e_phnum;
    memcpy(&e_entry,     data.data() + 24, 8);
    memcpy(&e_phoff,     data.data() + 32, 8);
    memcpy(&e_phentsize, data.data() + 54, 2);
    memcpy(&e_phnum,     data.data() + 56, 2);
    uint64_t end_addr = base;
    entry = base + e_entry;
    for (int i = 0; i < e_phnum; i++) {
        if (e_phoff + (i + 1) * e_phentsize > data.size()) break;
        const uint8_t* p = data.data() + e_phoff + i * e_phentsize;
        uint32_t p_type;
        uint64_t p_offset, p_vaddr, p_filesz, p_memsz;
        memcpy(&p_type,   p + 0,  4);
        memcpy(&p_offset, p + 8,  8);
        memcpy(&p_vaddr,  p + 16, 8);
        memcpy(&p_filesz, p + 32, 8);
        memcpy(&p_memsz,  p + 40, 8);
        if (p_type != 1) continue;  // PT_LOAD
        uint64_t addr = base + p_vaddr;
        mem_.map_range(addr, p_memsz);
        if (p_filesz > 0 && p_offset + p_filesz <= data.size()) {
            mem_.write(addr, data.data() + p_offset, p_filesz);
        }
        uint64_t end = addr + p_memsz;
        if (end > end_addr) end_addr = end;
    }
    return end_addr;
}
// ── load_shared_library ────────────────────────────────────────────────
// find_library can search the parent object's DT_RUNPATH/DT_RPATH for
// this library. DT_RUNPATH only applies to the immediate object's
// DT_NEEDED per the gABI; DT_RPATH is global (deprecated but still used).
uint64_t DynamicLinker::load_shared_library(const std::string& soname,
                                             const std::string& parent_runpath,
                                             const std::string& parent_rpath) {
    std::string path;
    auto data = find_library(soname, path, parent_runpath, parent_rpath);
    // Host search paths often surface x86_64 libGL/libSDL2. Those must
    // NOT be mapped as guest code — prefer the GraphicThunk synthetic
    // object whenever the file is missing or not AArch64.
    constexpr uint16_t EM_AARCH64 = 183;
    bool aarch64_elf = data.size() >= 20
        && data[0] == 0x7f && data[1] == 'E' && data[2] == 'L' && data[3] == 'F'
        && data[18] == (EM_AARCH64 & 0xFF) && data[19] == (EM_AARCH64 >> 8);
    if (!aarch64_elf) {
        if (thunk_resolver_ && is_thunk_supported_lib_(soname)) {
            return register_thunk_library_(soname);
        }
        return 0;
    }
    // Allocate a fresh base address via the Memory's mmap_alloc().
    // (v1.5.2: mmap_alloc's base is MMAP_BASE_MIN inside the 4 GiB
    // direct window — the SAME region where the thunk trampolines and
    // string caches are allocated. The allocator tracks ALL allocations
    // so library bases can't collide with the trampolines; that was the
    // bug this comment documents: the old code used a separate counter
    // starting at 0x5000000000, which could collide with the thunk's
    // trampoline page → "decode error at pc=0x5000000020". The fix is
    // using mem_.mmap_alloc() for library bases.)
    uint64_t max_end = 0;
    if (data.size() >= 56) {
        uint64_t e_phoff;
        uint16_t e_phentsize, e_phnum;
        memcpy(&e_phoff,     data.data() + 32, 8);
        memcpy(&e_phentsize, data.data() + 54, 2);
        memcpy(&e_phnum,     data.data() + 56, 2);
        for (int i = 0; i < e_phnum; i++) {
            const uint8_t* p = data.data() + e_phoff + i * e_phentsize;
            uint32_t p_type;
            uint64_t p_vaddr, p_memsz;
            memcpy(&p_type,  p + 0,  4);
            memcpy(&p_vaddr, p + 16, 8);
            memcpy(&p_memsz, p + 40, 8);
            if (p_type == 1) {
                uint64_t end = p_vaddr + p_memsz;
                if (end > max_end) max_end = end;
            }
        }
    }
    // Allocate via mmap_alloc (page-aligned, collision-free with other
    // high-memory allocations like the thunk's trampoline page).
    // We round up to 1 MiB alignment to match the old behavior (libraries
    // are typically 1 MiB aligned in real ld.so).
    max_end = (max_end + 0xFFFFF) & ~0xFFFFFULL;  // 1 MiB align
    uint64_t base = mem_.mmap_alloc(max_end);
    if (base == 0) return 0;
    LoadedObject obj;
    obj.name = soname;
    obj.base_addr = base;
    obj.is_main = false;
    obj.map_size = max_end;  // for find_object_by_addr (dladdr)
    uint64_t entry;
    uint64_t end = map_segments(data, base, entry);
    obj.entry = entry;
    (void)end;
    if (!parse_dynamic(data, base, obj)) {
        return 0;
    }
    parse_tls(data, obj);
    objects_.push_back(std::move(obj));
    index_symbols(objects_.back());
    parse_versions_(objects_.back());
    return base;
}
// ── register_thunk_library_ ──────────────────────────────────
// Synthesize a LoadedObject for a graphic library that's supported by
// the thunk resolver but couldn't be loaded from disk. The object has
// no PT_LOAD segments, no PT_DYNAMIC, no PT_TLS — its only purpose is
// to provide symbol addresses via the global `symbols_` map.
//
// The thunk resolver is called once with `soname` and returns the full
// list of (symbol_name, guest_trampoline_addr) pairs it supports for
// that library. We insert each into the global symbol table.
uint64_t DynamicLinker::register_thunk_library_(const std::string& soname) {
    if (!thunk_resolver_) return 0;
    // Use a synthetic base address in a high region that won't collide
    // with real libraries. We don't actually map anything at this
    // address — it's just a sentinel for the LoadedObject record.
    // The "real" addresses live in the thunk's trampoline page.
    constexpr uint64_t THUNK_LIB_BASE = 0x6000000000ULL;
    LoadedObject obj;
    obj.name = soname;
    obj.base_addr = THUNK_LIB_BASE;  // synthetic; never dereferenced
    obj.is_main = false;
    obj.dyn_addr = 0;     // no PT_DYNAMIC
    obj.symtab_addr = 0;  // no .dynsym
    obj.strtab_addr = 0;  // no .dynstr
    objects_.push_back(std::move(obj));
    // Ask the thunk for all symbols it supports for this library.
    // The thunk is the single source of truth for its symbol inventory;
    // we don't need a hardcoded list of GL/EGL/SDL2 entry points here.
    ThunkSymbolList syms = thunk_resolver_(soname);
    size_t added = 0;
    for (const auto& [sym, addr] : syms) {
        if (addr == 0) continue;
        // First definition wins (matches the existing index_symbols
        // behavior). Don't override a strong symbol from a real lib.
        if (symbols_.count(sym) == 0) {
            symbols_[sym] = SymEntry{addr, STB_GLOBAL_};
            added++;
        }
    }
    if (dynlink_trace_enabled()) {
        fprintf(stderr, "[dynlink] registered thunk library '%s': "
                "%zu/%zu symbols\n",
                soname.c_str(), added, syms.size());
    }
    return THUNK_LIB_BASE;
}
// ── is_thunk_supported_lib_ ────────────────────────────────────────────
// Returns true if `soname` matches the naming pattern of a library that
// the thunk resolver might handle. We accept:
//   - Graphics (GraphicThunk): libGL*, libEGL*, libSDL2*, libGLESv2*
//   - Audio (AudioThunk, v1.5.0.alpha): libasound*, libpulse*,
//     libopenal*
//   - Display (DisplayThunk, v1.5.0.alpha): libvulkan*, libwayland-*,
//     libX11*, libgbm*
// (libSDL2 is in both graphics and audio — both thunks will try to
// resolve its symbols, and the dispatcher tries graphics first.)
//
// The thunk itself does the final accept/reject — this is just a fast
// filter to avoid calling the resolver for libc/libm/etc.
bool DynamicLinker::is_thunk_supported_lib_(const std::string& soname) {
    auto starts_with = [](const std::string& s, const char* p) {
        return s.rfind(p, 0) == 0;
    };
    // Graphics (v1.4.5-alpha).
    if (starts_with(soname, "libGL.so")
        || starts_with(soname, "libEGL.so")
        || starts_with(soname, "libSDL2")
        || starts_with(soname, "libGLESv2.so")
        || starts_with(soname, "libglfw.so")) {
        return true;
    }
    // Audio (v1.5.0.alpha).
    if (starts_with(soname, "libasound")
        || starts_with(soname, "libpulse")
        || starts_with(soname, "libopenal")) {
        return true;
    }
    // Display (v1.5.0.alpha).
    if (starts_with(soname, "libvulkan")
        || starts_with(soname, "libwayland-")
        || starts_with(soname, "libX11")
        || starts_with(soname, "libgbm")) {
        return true;
    }
    return false;
}
// ── parse_tls ──────────────────────────────────────────────────────────
void DynamicLinker::parse_tls(const std::vector<uint8_t>& data,
                              LoadedObject& obj) {
    if (data.size() < 64) return;
    uint64_t e_phoff;
    uint16_t e_phentsize, e_phnum;
    memcpy(&e_phoff,     data.data() + 32, 8);
    memcpy(&e_phentsize, data.data() + 54, 2);
    memcpy(&e_phnum,     data.data() + 56, 2);
    for (int i = 0; i < e_phnum; i++) {
        if (e_phoff + (i + 1) * e_phentsize > data.size()) break;
        const uint8_t* p = data.data() + e_phoff + i * e_phentsize;
        uint32_t p_type;
        memcpy(&p_type, p + 0, 4);
        if (p_type != 7) continue;  // PT_TLS = 7
        obj.tls.vaddr  = obj.base_addr + 0;  // relative to base; we'll add base when copying
        memcpy(&obj.tls.vaddr,  p + 16, 8);  // p_vaddr (relative, NOT adjusted by base)
        memcpy(&obj.tls.filesz, p + 32, 8);
        memcpy(&obj.tls.memsz,  p + 40, 8);
        memcpy(&obj.tls.align,  p + 48, 8);
        obj.tls.present = true;
        return;
    }
}
// ── allocate_static_tls ────────────────────────────────────────────────
// Lay out each PT_TLS block using AArch64 glibc's variant-I TLS layout:
//   - Main exe TLS: at POSITIVE TP offsets (TP + tcb_size .. TP + tcb_size + main_memsz)
//   - Shared lib TLS: at NEGATIVE TP offsets (TP - lib_size .. TP)
//
// This matches what glibc expects on AArch64:
//   - Local-exec TLS access (mrs tpidr_el0; add x0, x0, #:tprel_hi:sym;
//     add x0, x0, #:tprel_lo12:sym) uses a POSITIVE offset baked at link
//     time = st_value + TLS_TCB_SIZE. So the main exe's TLS block MUST
//     be at TP + TLS_TCB_SIZE (positive offset from TP).
//   - The TCB header (tcbhead_t) occupies [TP, TP + TLS_TCB_SIZE). glibc's
//     create_thread fills in tcb/dtv/self/stack_guard/pointer_guard here.
//   - Shared lib TLS (accessed via __tls_get_addr / initial-exec for libs)
//     is at negative TP offsets, below the TCB.
//
// static_tls_base_ block layout (the "template" we copy per-thread):
//   [base .. base + lib_size)                          — lib TLS (negative TP)
//   [base + lib_size .. base + lib_size + tcb_size)    — TCB header (TP+0..TP+tcb_size)
//   [base + lib_size + tcb_size .. base + total)       — main exe TLS (positive TP)
// TP = base + lib_size  (points to the TCB header start)
//
// Per-thread (TP = tcb):
//   [tcb - lib_size .. tcb)                — lib TLS
//   [tcb .. tcb + tcb_size)                — TCB header
//   [tcb + tcb_size .. tcb + tcb_size + main_memsz) — main exe TLS
//
// offsets, TP = base + total). This broke local-exec TLS access for the
// main exe: the binary's hardcoded positive TPREL offset (e.g. +0x20)
// landed in the TCB header area instead of the main exe's TLS block.
// With small main TLS (8 bytes) and lucky alignment, this sometimes
// worked by accident. With larger main TLS (64+ bytes, e.g. __thread
// long tls_array[8]), the TLS data overlapped glibc's TCB fields and
// got clobbered, causing the "got = expected/4" TLS corruption pattern
// across 8+ threads.
void DynamicLinker::allocate_static_tls() {
    if (static_tls_base_ != 0) return;  // already allocated
    // First pass: compute lib_size and main TLS info.
    uint64_t lib_size = 0;
    uint64_t main_memsz = 0;
    uint64_t main_align = 1;
    for (const auto& obj : objects_) {
        if (!obj.tls.present || obj.tls.memsz == 0) continue;
        if (obj.is_main) {
            main_memsz = obj.tls.memsz;
            main_align = obj.tls.align ? obj.tls.align : 16;
        } else {
            uint64_t a = obj.tls.align ? obj.tls.align : 16;
            lib_size = (lib_size + a - 1) & ~(a - 1);
            lib_size += obj.tls.memsz;
        }
    }
    if (lib_size == 0 && main_memsz == 0) return;
    // Round lib_size up to 16 (minimum TLS alignment).
    lib_size = (lib_size + 15) & ~15ULL;
    if (is_musl_) {
        // ── Variant-II (musl): ALL TLS at negative TP offsets ──────
        // TP = base + total (points PAST the block).
        // All modules' tp_offset = cursor - total (negative).
        // This is the original layout that worked for musl.
        uint64_t total = 0;
        uint64_t max_align = 16;
        for (auto& obj : objects_) {
            if (!obj.tls.present || obj.tls.memsz == 0) continue;
            if (obj.tls.align > max_align) max_align = obj.tls.align;
            total = (total + obj.tls.align - 1) & ~(obj.tls.align - 1);
            obj.tls_mod_id = next_tls_mod_id_++;
            total += obj.tls.memsz;
        }
        total = (total + max_align - 1) & ~(max_align - 1);
        static_tls_size_ = total;
        lib_tls_size_ = total;  // variant-II: all TLS is "negative TP"
        tcb_size_ = 0;          // no TCB header for musl variant-II
        static_tls_base_ = mem_.mmap_alloc(total + 16);
        if (static_tls_base_ == 0) {
            error_ = "failed to allocate static TLS block";
            return;
        }
        uint64_t cursor = 0;
        for (auto& obj : objects_) {
            if (!obj.tls.present || obj.tls.memsz == 0) continue;
            cursor = (cursor + obj.tls.align - 1) & ~(obj.tls.align - 1);
            obj.tls_block_offset = cursor;
            obj.tls_tp_offset = static_cast<int64_t>(cursor) -
                                static_cast<int64_t>(total);
            uint64_t src = obj.base_addr + obj.tls.vaddr;
            uint64_t dst = static_tls_base_ + cursor;
            if (obj.tls.filesz > 0) {
                try {
                    std::vector<uint8_t> buf(obj.tls.filesz);
                    mem_.read(src, buf.data(), buf.size());
                    mem_.write(dst, buf.data(), buf.size());
                } catch (...) {}
            }
            cursor += obj.tls.memsz;
        }
        return;
    }
    // ── Variant-I (glibc AArch64) ──────────────────────────────────
    // Main exe TLS at POSITIVE TP offsets, lib TLS at NEGATIVE TP offsets.
    // TCB header (tcbhead_t) at [TP, TP + tcb_size).
    //
    // See the long comment above for the full rationale.
    constexpr uint64_t TLS_TCB_SIZE_BASE = 0x10;  // sizeof(tcbhead_t) = tcb + dtv
    uint64_t tcb_size = (TLS_TCB_SIZE_BASE + main_align - 1) & ~(main_align - 1);
    lib_tls_size_ = lib_size;
    tcb_size_ = tcb_size;
    // Total static TLS block = lib + TCB + main.
    uint64_t total = lib_size + tcb_size + main_memsz;
    // Round up to max alignment (16 minimum).
    uint64_t max_align = 16;
    if (main_align > max_align) max_align = main_align;
    total = (total + max_align - 1) & ~(max_align - 1);
    static_tls_size_ = total;
    // Allocate guest memory for the template block.
    static_tls_base_ = mem_.mmap_alloc(total + 16);  // +16 slack
    if (static_tls_base_ == 0) {
        error_ = "failed to allocate static TLS block";
        return;
    }
    // TP = static_tls_base_ + lib_size  (points to the TCB header start).
    // Main exe TLS is at TP + tcb_size (positive offset).
    // Lib TLS is at TP - lib_size (negative offset).
    // Second pass: assign module IDs, tp_offsets, block_offsets, and copy
    // .tdata templates.
    uint64_t lib_cursor = 0;   // offset within [base, base+lib_size)
    for (auto& obj : objects_) {
        if (!obj.tls.present || obj.tls.memsz == 0) continue;
        obj.tls_mod_id = next_tls_mod_id_++;
        if (obj.is_main) {
            // Main exe TLS: at POSITIVE TP offset = tcb_size.
            // In the template block, it's at [base + lib_size + tcb_size, ...).
            obj.tls_block_offset = lib_size + tcb_size;
            obj.tls_tp_offset = static_cast<int64_t>(tcb_size);  // positive
        } else {
            // Lib TLS: at NEGATIVE TP offset.
            // Align lib_cursor.
            uint64_t a = obj.tls.align ? obj.tls.align : 16;
            lib_cursor = (lib_cursor + a - 1) & ~(a - 1);
            // In the template block, lib TLS is at [base + lib_cursor, ...).
            // But we store libs in REVERSE order so that the first lib loaded
            // (libc, objects_[1]) is closest to TP (smallest negative offset).
            // Actually, for simplicity, store libs in load order at increasing
            // negative offsets. The tp_offset = lib_cursor - lib_size (negative).
            obj.tls_block_offset = lib_cursor;
            obj.tls_tp_offset = static_cast<int64_t>(lib_cursor) -
                                static_cast<int64_t>(lib_size);  // negative
            lib_cursor += obj.tls.memsz;
        }
        // Copy initialized data (.tdata) from obj's PT_TLS filesz.
        uint64_t src = obj.base_addr + obj.tls.vaddr;
        uint64_t dst = static_tls_base_ + obj.tls_block_offset;
        if (obj.tls.filesz > 0) {
            try {
                std::vector<uint8_t> buf(obj.tls.filesz);
                mem_.read(src, buf.data(), buf.size());
                mem_.write(dst, buf.data(), buf.size());
            } catch (...) {
                // Reading the source failed — leave zeroed (mmap gave us zeros).
            }
        }
        // .bss (memsz - filesz) is already zero from mmap.
    }
}
// ── TLS accessors ──────────────────────────────────────────────────────
uint64_t DynamicLinker::tls_mod_id(const std::string& name) const {
    for (const auto& o : objects_) {
        if (o.name == name) return o.tls_mod_id;
    }
    return 0;
}
int64_t DynamicLinker::tls_tp_offset(uint64_t mod_id) const {
    for (const auto& o : objects_) {
        if (o.tls_mod_id == mod_id) return o.tls_tp_offset;
    }
    return 0;
}
// ── resolve_reloc_symbol ───────────────────────────────────────────────
// version index for this symbol and try the versioned symbol table first.
// This lets relocations that request a specific version (e.g. memcpy@GLIBC_2.17)
// resolve to the correct implementation rather than whichever unversioned
// symbol happened to be indexed first.
uint64_t DynamicLinker::resolve_reloc_symbol(const LoadedObject& obj,
                                             uint32_t sym_idx) {
    if (sym_idx == 0) return 0;
    Elf64_Sym s;
    try {
        mem_.read(obj.symtab_addr + sym_idx * sizeof(s), &s, sizeof(s));
    } catch (...) {
        return 0;
    }
    std::string name = read_guest_cstr(mem_, obj.strtab_addr + s.st_name);
    if (name.empty()) return 0;
    // Look up the version requirement for this symbol.
    // .gnu.version (DT_VERSYM) is an array of uint16_t, one per .dynsym
    // entry. The value is an index into the verneed/verdef tables.
    // For an UNDEF symbol being resolved against a dependency, the version
    // comes from .gnu.version_r (DT_VERNEED). For simplicity, we look up
    // the version name from the object's own .gnu.version + the defining
    // object's verdef — but that requires knowing which object defines
    // the symbol. Instead, we take a simpler approach: try every
    // versioned entry for this name (by scanning versioned_symbols_ for
    // keys starting with "name@"). This is O(n) but n is small per name.
    //
    // Actually, the cleanest approach: look up the version index from
    // THIS object's .gnu.version[sym_idx], then find the corresponding
    // version name from THIS object's .gnu.version_r (DT_VERNEED).
    // Then resolve_versioned_symbol(name, version_name).
    if (obj.versym_addr != 0 && obj.verneed_addr != 0) {
        try {
            uint16_t vidx = 0;
            mem_.read(obj.versym_addr + sym_idx * 2, &vidx, 2);
            uint16_t real_idx = vidx & 0x7FFF;
            if (real_idx >= 2) {
                // Find the version name from verneed. Walk the Verneed
                // chain and find the Vernaux with matching vna_other
                // (which is the version index used in .gnu.version).
                // Elf64_Verneed (16 bytes):
                //   +0: uint16_t vn_version
                //   +2: uint16_t vn_cnt (number of Vernaux)
                //   +4: uint32_t vn_file (strtab offset of dep soname)
                //   +8: uint32_t vn_aux (offset to first Vernaux)
                //   +12: uint32_t vn_next (offset to next Verneed)
                // Elf64_Vernaux (16 bytes):
                //   +0: uint32_t vna_hash
                //   +4: uint16_t vna_flags
                //   +6: uint16_t vna_other (version index, matches .gnu.version)
                //   +8: uint32_t vna_name (strtab offset of version name)
                //   +12: uint32_t vna_next
                std::string version_name;
                uint64_t p = obj.verneed_addr;
                for (uint64_t i = 0; i < obj.verneed_num; i++) {
                    uint16_t vn_cnt;
                    uint32_t vn_aux, vn_next;
                    mem_.read(p + 2, &vn_cnt, 2);
                    mem_.read(p + 8, &vn_aux, 4);
                    mem_.read(p + 12, &vn_next, 4);
                    uint64_t ap = p + vn_aux;
                    for (uint16_t j = 0; j < vn_cnt; j++) {
                        uint16_t vna_other;
                        uint32_t vna_name, vna_next;
                        mem_.read(ap + 6, &vna_other, 2);
                        mem_.read(ap + 8, &vna_name, 4);
                        mem_.read(ap + 12, &vna_next, 4);
                        if (vna_other == real_idx) {
                            version_name = read_guest_cstr(mem_, obj.strtab_addr + vna_name);
                            break;
                        }
                        if (vna_next == 0) break;
                        ap += vna_next;
                    }
                    if (!version_name.empty()) break;
                    if (vn_next == 0) break;
                    p += vn_next;
                }
                if (!version_name.empty()) {
                    uint64_t addr = resolve_versioned_symbol(name, version_name);
                    if (addr != 0) return addr;
                    // else fall back to unversioned.
                }
            }
        } catch (...) {
            // corrupt version info — fall back to unversioned
        }
    }
    return resolve_symbol(name);
}
// ── parse_versions_ ──────────────────────────────────────
// Parse the GNU symbol versioning sections and populate
// versioned_symbols_ with "name@version" keys. This lets relocations
// that request a specific version (via .gnu.version_r) resolve to the
// correct symbol implementation.
//
// ELF versioning structures (from elf.h):
//   .gnu.version (DT_VERSYM): array of uint16_t, one per .dynsym entry.
//     Values: 0=local, 1=global (base), 2+=index into verdef/verneed.
//     Bit 15 (0x8000) set = "hidden" version (preferred for resolution).
//   .gnu.version_d (DT_VERDEF): array of Elf64_Verdef structures, each
//     describing a version THIS object exports. Each Verdef has a chain
//     of Verdaux entries (name + hash).
//   .gnu.version_r (DT_VERNEED): array of Elf64_Verneed structures,
//     each describing a version THIS object needs from a dependency.
//     Each Verneed has a chain of Vernaux entries (version name + hash).
//
// We build a mapping: symbol_index → version_name (from verdef for
// exported symbols). Then index_symbols can store both "name" (default)
// and "name@version" (versioned) in versioned_symbols_.
//
// For simplicity, we ONLY parse verdef (exported versions). verneed
// (needed versions) is consulted at relocation time to look up the
// correct versioned symbol in dependencies.
void DynamicLinker::parse_versions_(const LoadedObject& obj) {
    if (obj.versym_addr == 0 || obj.verdef_addr == 0) return;
    if (obj.symtab_addr == 0 || obj.strtab_addr == 0) return;
    // Build verdef index → version name map.
    // Elf64_Verdef layout (20 bytes):
    //   +0:  uint16_t vd_version (always 1)
    //   +2:  uint16_t vd_flags
    //   +4:  uint16_t vd_ndx (version index, matches .gnu.version entries)
    //   +6:  uint16_t vd_cnt (number of Verdaux entries)
    //   +8:  uint32_t vd_hash
    //   +12: uint32_t vd_aux (offset to first Verdaux, from this Verdef)
    //   +16: uint32_t vd_next (offset to next Verdef, from this Verdef)
    // Elf64_Verdaux layout (16 bytes):
    //   +0:  uint32_t vda_name (offset into strtab of version string)
    //   +4:  uint32_t vda_next (offset to next Verdaux, from this Verdaux)
    std::unordered_map<uint16_t, std::string> verdef_names;
    try {
        uint64_t p = obj.verdef_addr;
        for (uint64_t i = 0; i < obj.verdef_num; i++) {
            uint16_t vd_ndx, vd_cnt;
            uint32_t vd_aux, vd_next;
            mem_.read(p + 4, &vd_ndx, 2);
            mem_.read(p + 6, &vd_cnt, 2);
            mem_.read(p + 12, &vd_aux, 4);
            mem_.read(p + 16, &vd_next, 4);
            // First Verdaux is the version name.
            if (vd_cnt > 0 && vd_aux != 0) {
                uint32_t vda_name;
                mem_.read(p + vd_aux, &vda_name, 4);
                std::string vname = read_guest_cstr(mem_, obj.strtab_addr + vda_name);
                if (!vname.empty()) {
                    verdef_names[vd_ndx] = vname;
                }
            }
            if (vd_next == 0) break;
            p += vd_next;
        }
    } catch (...) {
        return;  // corrupt verdef — skip versioning for this object
    }
    if (verdef_names.empty()) return;
    // Now iterate .dynsym and for each symbol with a version index > 1,
    // store "name@version" in versioned_symbols_.
    constexpr size_t MAX_SYMS = 8192;
    size_t count = obj.symtab_count;
    if (count == 0 || count > MAX_SYMS * 4) count = MAX_SYMS;
    constexpr uint8_t STT_GNU_IFUNC_ = 10;
    auto ST_TYPE_ = [](uint8_t info) { return info & 0xF; };
    for (size_t i = 1; i < count; i++) {  // skip STN_UNDEF (symbol 0)
        Elf64_Sym s;
        try {
            mem_.read(obj.symtab_addr + i * sizeof(s), &s, sizeof(s));
        } catch (...) { break; }
        if (s.st_name == 0 && s.st_value == 0 && s.st_shndx == 0) continue;
        if (s.st_shndx == SHN_UNDEF_) continue;  // only defined symbols
        uint8_t bind = ST_BIND_(s.st_info);
        if (bind != STB_GLOBAL_ && bind != STB_WEAK_) continue;
        // Read the version index for this symbol from .gnu.version.
        uint16_t vidx = 0;
        try {
            mem_.read(obj.versym_addr + i * 2, &vidx, 2);
        } catch (...) { continue; }
        // Bit 15 = hidden flag. Mask it off to get the real index.
        uint16_t real_idx = vidx & 0x7FFF;
        if (real_idx < 2) continue;  // 0=local, 1=global (unversioned)
        auto vit = verdef_names.find(real_idx);
        if (vit == verdef_names.end()) continue;
        std::string name = read_guest_cstr(mem_, obj.strtab_addr + s.st_name);
        if (name.empty()) continue;
        uint64_t addr = obj.base_addr + s.st_value;
        // STT_GNU_IFUNC: call resolver (same as index_symbols).
        if (ST_TYPE_(s.st_info) == STT_GNU_IFUNC_ && ifunc_resolver_) {
            uint64_t resolved = ifunc_resolver_(addr);
            if (resolved != 0) addr = resolved;
        }
        std::string key = name + "@" + vit->second;
        // First-strong-wins (same as index_symbols).
        auto it = versioned_symbols_.find(key);
        if (it == versioned_symbols_.end()) {
            versioned_symbols_[key] = SymEntry{addr, bind};
        } else {
            if (it->second.bind == STB_WEAK_ && bind == STB_GLOBAL_) {
                it->second = SymEntry{addr, bind};
            }
        }
    }
}
// ── resolve_versioned_symbol ────────────────────────────────────────────
uint64_t DynamicLinker::resolve_versioned_symbol(const std::string& name,
                                                   const std::string& version) const {
    if (!version.empty()) {
        std::string key = name + "@" + version;
        auto it = versioned_symbols_.find(key);
        if (it != versioned_symbols_.end()) {
            return it->second.addr;
        }
    }
    // Fall back to unversioned.
    return resolve_symbol(name);
}
// ── index_symbols ──────────────────────────────────────────────────────
void DynamicLinker::index_symbols(const LoadedObject& obj) {
    if (obj.symtab_addr == 0 || obj.strtab_addr == 0) return;
    // available, 8192 cap fallback). Previously hardcoded 8192, dropping
    // symbols past the cap in large libs (Qt, webkit).
    // overrides an existing strong symbol; a weak symbol is overridden by
    // a strong one. Previously "last strong wins" which let load order
    // silently swap library implementations.
    // RESOLVER address in st_value, not the function address. For ifuncs,
    // we call the resolver (via ifunc_resolver_) and store the resolved
    // address. Without this, glibc's memcpy/memset/strcmp (which are
    // ifuncs) would jump to the resolver body as if it were the function.
    constexpr size_t MAX_SYMS = 8192;  // safety cap when symtab_count is bogus
    size_t count = obj.symtab_count;
    if (count == 0 || count > MAX_SYMS * 4) count = MAX_SYMS;  // sanity
    constexpr uint8_t STT_GNU_IFUNC_ = 10;
    auto ST_TYPE_ = [](uint8_t info) { return info & 0xF; };
    // and indexing ZERO symbols. This broke every dynamically-linked binary:
    // libc.so.6's 2973 defined symbols were never indexed, so every
    // relocation against strlen/printf/puts/free/abort/__libc_start_main
    // returned NOT FOUND, the GOT slots stayed at 0, and the program
    // crashed with "decode error at pc=0x0 inst=0x00000000" on the first
    // call. The fix: start at i=1 (skip STN_UNDEF) and use `continue`
    // for any subsequent all-zero entry (defensive — should not happen
    // in well-formed ELFs but cheap to check). Performance is fine because
    // symtab_count from DT_HASH is the exact symbol count (no over-scan).
    for (size_t i = 1; i < count; i++) {
        Elf64_Sym s;
        try {
            mem_.read(obj.symtab_addr + i * sizeof(s), &s, sizeof(s));
        } catch (...) {
            break;
        }
        // Defensive: skip any all-zero entry (should only be symbol 0,
        // already skipped above, but be safe against malformed ELFs).
        if (s.st_name == 0 && s.st_value == 0 && s.st_shndx == 0) {
            continue;
        }
        // Only index defined symbols (st_shndx != SHN_UNDEF).
        if (s.st_shndx == SHN_UNDEF_) continue;
        // Only index global/weak symbols (skip local).
        uint8_t bind = ST_BIND_(s.st_info);
        if (bind != STB_GLOBAL_ && bind != STB_WEAK_) continue;
        std::string name = read_guest_cstr(mem_, obj.strtab_addr + s.st_name);
        if (name.empty()) continue;
        uint64_t addr = obj.base_addr + s.st_value;
        // STT_GNU_IFUNC: st_value is the resolver, not the function.
        // Call the resolver to get the real address. If no resolver is
        // registered (DynamicLinker used standalone), fall back to the
        // resolver address — the guest will call the resolver body as
        // the function (visible failure, not silent corruption).
        if (ST_TYPE_(s.st_info) == STT_GNU_IFUNC_) {
            if (ifunc_resolver_) {
                uint64_t resolved = ifunc_resolver_(addr);
                if (resolved != 0) addr = resolved;
                // else: fall back to resolver address (visible failure)
            }
            // else: no resolver registered; store resolver address.
            // The guest will crash on first call (visible, not silent).
        }
        // First-strong-wins symbol resolution (H6).
        auto it = symbols_.find(name);
        if (it == symbols_.end()) {
            symbols_[name] = SymEntry{addr, bind};
        } else {
            // Existing entry. Override only if existing is WEAK and new
            // is STRONG. Never override an existing STRONG.
            if (it->second.bind == STB_WEAK_ && bind == STB_GLOBAL_) {
                it->second = SymEntry{addr, bind};
            }
            // else: keep existing (first strong wins, or weak kept as-is).
        }
    }
}
// ── resolve_symbol ─────────────────────────────────────────────────────
uint64_t DynamicLinker::resolve_symbol(const std::string& name) const {
    auto it = symbols_.find(name);
    if (it == symbols_.end()) {
        if (dynlink_trace_enabled()) {
            fprintf(stderr, "[dynlink] resolve_symbol: '%s' NOT FOUND\n",
                    name.c_str());
        }
        return 0;
    }
    if (dynlink_trace_enabled()) {
        fprintf(stderr, "[dynlink] resolve_symbol: '%s' -> 0x%llx\n",
                name.c_str(),
                static_cast<unsigned long long>(it->second.addr));
    }
    return it->second.addr;
}
// ── load_library (dlopen support) ─────────────────────────────────────
// Load a shared library at runtime by path. Reuses the same loading
// logic as load_shared_library but takes a full path instead of a
// soname. Returns the base address (handle) on success, 0 on failure.
//
// If the library is already loaded (by path or soname), returns the
// existing handle and bumps the refcount (matching glibc's _dl_open
// fast path). This prevents loading the same .so twice and ensures
// dlopen("libm.so.6") returns the same handle as dlopen("/lib/libm.so.6").
uint64_t DynamicLinker::load_library(const std::string& path) {
    // ── Dedup: check if already loaded by path ────────────────────
    // Extract the basename (soname) from the path for dedup. glibc's
    // _dl_open does the same: dlopen("/lib/libm.so.6") and
    // dlopen("libm.so.6") return the same handle if the library is
    // already loaded.
    std::string basename = path;
    size_t slash = basename.find_last_of('/');
    if (slash != std::string::npos) basename = basename.substr(slash + 1);
    for (auto& obj : objects_) {
        if (obj.name == path || obj.soname == basename ||
            obj.name == basename) {
            obj.refcount++;
            if (dynlink_trace_enabled()) {
                fprintf(stderr, "[dlopen] dedup '%s' → handle=0x%llx refcount=%u\n",
                        path.c_str(),
                        static_cast<unsigned long long>(obj.base_addr),
                        obj.refcount);
            }
            return obj.base_addr;
        }
    }
    // Resolve path via BIFROST_ROOT sandbox.
    std::string resolved = path;
    if (const char* root = getenv("BIFROST_ROOT")) {
        std::string rp = std::string(root) + path;
        std::ifstream test(rp, std::ios::binary);
        if (test) resolved = rp;
    }
    // If the path is a bare soname (no /), try standard search paths.
    if (path.find('/') == std::string::npos) {
        std::string found_path;
        auto data = find_library(path, found_path);
        constexpr uint16_t EM_AARCH64 = 183;
        bool aarch64_elf = data.size() >= 20
            && data[0] == 0x7f && data[1] == 'E' && data[2] == 'L' && data[3] == 'F'
            && data[18] == (EM_AARCH64 & 0xFF) && data[19] == (EM_AARCH64 >> 8);
        if (aarch64_elf) {
            resolved = found_path;
            std::vector<uint8_t> file_data = std::move(data);
            return load_library_from_data(path, file_data);
        }
        // Missing or host-arch library: thunk graphic/audio/display APIs.
        if (thunk_resolver_ && is_thunk_supported_lib_(path)) {
            return register_thunk_library_(path);
        }
        if (data.empty()) {
            set_last_error("cannot find '" + path + "'");
            error_ = "load_library: cannot find '" + path + "'";
            return 0;
        }
        set_last_error("'" + path + "' is not an AArch64 ELF");
        error_ = "load_library: wrong ELF machine for '" + path + "'";
        return 0;
    }
    std::ifstream f(resolved, std::ios::binary | std::ios::ate);
    if (!f) {
        // Absolute path missing under BIFROST_ROOT / host: fall back to
        // soname search (e.g. /lib/libm.so.6 → aarch64 libm on the
        // toolchain search path).
        std::string found_path;
        auto data = find_library(basename, found_path);
        constexpr uint16_t EM_AARCH64 = 183;
        bool aarch64_elf = data.size() >= 20
            && data[0] == 0x7f && data[1] == 'E' && data[2] == 'L' && data[3] == 'F'
            && data[18] == (EM_AARCH64 & 0xFF) && data[19] == (EM_AARCH64 >> 8);
        if (aarch64_elf) {
            return load_library_from_data(path, data);
        }
        set_last_error("cannot open '" + resolved + "'");
        error_ = "load_library: cannot open '" + resolved + "'";
        return 0;
    }
    std::streamsize size = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(size);
    if (!f.read(reinterpret_cast<char*>(data.data()), size)) {
        set_last_error("read error");
        error_ = "load_library: read error";
        return 0;
    }
    constexpr uint16_t EM_AARCH64 = 183;
    bool aarch64_elf = data.size() >= 20
        && data[0] == 0x7f && data[1] == 'E' && data[2] == 'L' && data[3] == 'F'
        && data[18] == (EM_AARCH64 & 0xFF) && data[19] == (EM_AARCH64 >> 8);
    if (!aarch64_elf) {
        // Host absolute path resolved to a non-guest ELF (common when
        // BIFROST_ROOT lacks the lib and /lib/libm.so.6 is x86_64).
        std::string found_path;
        auto alt = find_library(basename, found_path);
        bool alt_ok = alt.size() >= 20
            && alt[0] == 0x7f && alt[1] == 'E' && alt[2] == 'L' && alt[3] == 'F'
            && alt[18] == (EM_AARCH64 & 0xFF) && alt[19] == (EM_AARCH64 >> 8);
        if (alt_ok) {
            return load_library_from_data(path, alt);
        }
    }
    return load_library_from_data(path, data);
}
// Internal helper: load a library from an in-memory ELF image.
// Used by load_library (dlopen by path) and by soname-based lookup.
// Handles ELF validation, segment mapping, relocation, symbol indexing,
// init array execution, and refcount tracking.
uint64_t DynamicLinker::load_library_from_data(const std::string& path,
                                                std::vector<uint8_t>& data) {
    if (data.size() < 64 || data[0] != 0x7f || data[1] != 'E' ||
        data[2] != 'L' || data[3] != 'F') {
        set_last_error("not an ELF file");
        error_ = "load_library: not an ELF file";
        return 0;
    }
    // Absolute-path dlopen used to map host x86_64 libs (e.g. host
    // /lib/libm.so.6) when the guest rootfs was missing the soname.
    // Reject non-AArch64 images before mmap so we never execute host
    // machine code as guest.
    constexpr uint16_t EM_AARCH64 = 183;
    uint16_t e_machine = 0;
    memcpy(&e_machine, data.data() + 18, 2);
    if (e_machine != EM_AARCH64) {
        set_last_error("'" + path + "' is not an AArch64 ELF");
        error_ = "load_library: wrong ELF machine for '" + path + "'";
        return 0;
    }
    uint64_t max_end = 0;
    if (data.size() >= 56) {
        uint64_t e_phoff; uint16_t e_phentsize, e_phnum;
        memcpy(&e_phoff, data.data()+32, 8);
        memcpy(&e_phentsize, data.data()+54, 2);
        memcpy(&e_phnum, data.data()+56, 2);
        for (int i = 0; i < e_phnum; i++) {
            const uint8_t* p = data.data()+e_phoff+i*e_phentsize;
            uint32_t pt; uint64_t pv, pm;
            memcpy(&pt, p+0, 4); memcpy(&pv, p+16, 8); memcpy(&pm, p+40, 8);
            if (pt == 1) { uint64_t e = pv+pm; if (e > max_end) max_end = e; }
        }
    }
    max_end = (max_end + 0xFFFFF) & ~0xFFFFFULL;
    uint64_t base = mem_.mmap_alloc(max_end);
    if (base == 0) {
        set_last_error("mmap_alloc failed");
        error_ = "load_library: mmap_alloc failed";
        return 0;
    }
    LoadedObject obj;
    obj.name = path;
    obj.base_addr = base;
    obj.is_main = false;
    obj.refcount = 1;
    obj.map_size = max_end;
    uint64_t entry;
    map_segments(data, base, entry);
    obj.entry = entry;
    if (!parse_dynamic(data, base, obj)) {
        set_last_error("parse_dynamic failed");
        error_ = "load_library: parse_dynamic failed";
        return 0;
    }
    parse_tls(data, obj);
    // Assign TLS module ID and tp_offset for dlopened libs.
    if (obj.tls.present && obj.tls.memsz > 0) {
        obj.tls_mod_id = next_tls_mod_id_++;
        if (!is_musl_) {
            // Variant-I (glibc): lib TLS at negative TP offsets
            uint64_t a = obj.tls.align ? obj.tls.align : 16;
            lib_tls_size_ = (lib_tls_size_ + a - 1) & ~(a - 1);
            obj.tls_tp_offset = -static_cast<int64_t>(lib_tls_size_ + obj.tls.memsz);
            lib_tls_size_ += obj.tls.memsz;
            obj.tls_block_offset = static_tls_size_ - lib_tls_size_;
        }
    }
    objects_.push_back(std::move(obj));
    index_symbols(objects_.back());
    parse_versions_(objects_.back());
    // Apply relocations: RELA, JMPREL, RELR
    auto& nobj = objects_.back();
    if (nobj.dyn_addr != 0) {
        try {
            uint64_t ra=0,rs=0,ja=0,js=0,rra=0,rrs=0;
            Elf64_Dyn dyn;
            for (uint64_t p = nobj.dyn_addr; ; p += sizeof(dyn)) {
                mem_.read(p, &dyn, sizeof(dyn));
                if (dyn.d_tag == DT_NULL_) break;
                if (dyn.d_tag == DT_RELA_) ra = nobj.base_addr + dyn.d_val;
                else if (dyn.d_tag == DT_RELASZ_) rs = dyn.d_val;
                else if (dyn.d_tag == DT_JMPREL_) ja = nobj.base_addr + dyn.d_val;
                else if (dyn.d_tag == DT_PLTRELSZ_) js = dyn.d_val;
                else if (dyn.d_tag == DT_RELR_) rra = nobj.base_addr + dyn.d_val;
                else if (dyn.d_tag == DT_RELRSZ_) rrs = dyn.d_val;
            }
            if (ra && rs) {
                for (uint64_t off = 0; off+24 <= rs; off += 24) {
                    Elf64_Rela r; mem_.read(ra+off, &r, sizeof(r));
                    uint32_t type = r.r_info & 0xFFFFFFFF;
                    uint32_t sym = r.r_info >> 32;
                    uint64_t target = nobj.base_addr + r.r_offset;
                    int64_t A = r.r_addend;
                    if (type == R_AARCH64_RELATIVE_) {
                        mem_.store<uint64_t>(target, nobj.base_addr + A);
                    } else if (type == R_AARCH64_IRELATIVE_) {
                        // ifunc: call the resolver at base + A
                        uint64_t resolver_addr = nobj.base_addr + A;
                        uint64_t resolved = 0;
                        if (ifunc_resolver_) {
                            resolved = ifunc_resolver_(resolver_addr);
                        }
                        if (resolved == 0) resolved = resolver_addr;
                        mem_.store<uint64_t>(target, resolved);
                    } else if (type == R_AARCH64_GLOB_DAT_ || type == R_AARCH64_JUMP_SLOT_ || type == R_AARCH64_ABS64_) {
                        uint64_t addr = 0;
                        if (sym != 0) {
                            Elf64_Sym s; mem_.read(nobj.symtab_addr + sym*sizeof(s), &s, sizeof(s));
                            std::string name = read_guest_cstr(mem_, nobj.strtab_addr + s.st_name);
                            if (!name.empty()) addr = resolve_symbol(name);
                        }
                        if (addr) mem_.store<uint64_t>(target, addr + A);
                    } else if (type == R_AARCH64_TLS_TPREL_) {
                        // TLS_TPREL: initial-exec access
                        int64_t tp_off = A;
                        if (sym != 0) {
                            Elf64_Sym s; mem_.read(nobj.symtab_addr + sym*sizeof(s), &s, sizeof(s));
                            std::string name = read_guest_cstr(mem_, nobj.strtab_addr + s.st_name);
                            uint64_t sym_addr = resolve_symbol(name);
                            int64_t mod_tp_off = nobj.tls_tp_offset;
                            if (sym_addr != 0) {
                                const LoadedObject* o = find_object_by_addr(sym_addr);
                                if (o) mod_tp_off = o->tls_tp_offset;
                            }
                            tp_off = mod_tp_off + static_cast<int64_t>(s.st_value) + A;
                        } else {
                            tp_off = nobj.tls_tp_offset + A;
                        }
                        mem_.store<uint64_t>(target, static_cast<uint64_t>(tp_off));
                    } else if (type == R_AARCH64_TLS_DTPMOD_) {
                        // TLS_DTPMOD: module ID
                        uint64_t mod_id = nobj.tls_mod_id;
                        if (sym != 0) {
                            Elf64_Sym s; mem_.read(nobj.symtab_addr + sym*sizeof(s), &s, sizeof(s));
                            std::string name = read_guest_cstr(mem_, nobj.strtab_addr + s.st_name);
                            uint64_t sym_addr = resolve_symbol(name);
                            if (sym_addr != 0) {
                                const LoadedObject* o = find_object_by_addr(sym_addr);
                                if (o) mod_id = o->tls_mod_id;
                            }
                        }
                        mem_.store<uint64_t>(target, mod_id + A);
                    } else if (type == R_AARCH64_TLS_DTPREL_) {
                        // TLS_DTPREL: offset within module
                        uint64_t tls_off = static_cast<uint64_t>(A);
                        if (sym != 0) {
                            Elf64_Sym s; mem_.read(nobj.symtab_addr + sym*sizeof(s), &s, sizeof(s));
                            tls_off = s.st_value + A;
                        }
                        mem_.store<uint64_t>(target, tls_off);
                    } else if (type == R_AARCH64_TLSDESC_) {
                        // TLSDESC: 16-byte descriptor
                        int64_t tp_off = A;
                        if (sym != 0) {
                            Elf64_Sym s; mem_.read(nobj.symtab_addr + sym*sizeof(s), &s, sizeof(s));
                            std::string name = read_guest_cstr(mem_, nobj.strtab_addr + s.st_name);
                            uint64_t sym_addr = resolve_symbol(name);
                            int64_t mod_tp_off = nobj.tls_tp_offset;
                            if (sym_addr != 0) {
                                const LoadedObject* o = find_object_by_addr(sym_addr);
                                if (o) mod_tp_off = o->tls_tp_offset;
                            }
                            tp_off = mod_tp_off + static_cast<int64_t>(s.st_value) + A;
                        } else {
                            tp_off = nobj.tls_tp_offset + A;
                        }
                        mem_.store<uint64_t>(target, 0); // resolver = NULL
                        mem_.store<uint64_t>(target + 8, static_cast<uint64_t>(tp_off));
                    }
                }
            }
            if (ja && js) {
                for (uint64_t off = 0; off+24 <= js; off += 24) {
                    Elf64_Rela r; mem_.read(ja+off, &r, sizeof(r));
                    uint32_t type = r.r_info & 0xFFFFFFFF;
                    uint32_t sym = r.r_info >> 32;
                    uint64_t target = nobj.base_addr + r.r_offset;
                    int64_t A = r.r_addend;
                    if (type == R_AARCH64_JUMP_SLOT_) {
                        uint64_t addr = 0;
                        if (sym != 0) {
                            Elf64_Sym s; mem_.read(nobj.symtab_addr + sym*sizeof(s), &s, sizeof(s));
                            std::string name = read_guest_cstr(mem_, nobj.strtab_addr + s.st_name);
                            if (!name.empty()) addr = resolve_symbol(name);
                        }
                        if (addr) mem_.store<uint64_t>(target, addr);
                    } else if (type == R_AARCH64_IRELATIVE_) {
                        // ifunc in PLT
                        uint64_t resolver_addr = nobj.base_addr + A;
                        uint64_t resolved = ifunc_resolver_ ? ifunc_resolver_(resolver_addr) : 0;
                        if (resolved == 0) resolved = resolver_addr;
                        mem_.store<uint64_t>(target, resolved);
                    }
                }
            }
            if (rra && rrs) {
                // RELR decode: an address entry (bit0=0) seeds the running
                // reloc vaddr; bitmap entries (bit0=1) cover 63 slots
                // starting from the seeded address. Each slot reads its
                // addend from [base+vaddr] and writes base+addend back.
                // This mirrors apply_relr_relocations_() (startup path) —
                // the old code here mixed guest memory addresses with
                // reloc vaddrs (cur = addr - 16), corrupting the RELR
                // section and never relocating the real slots.
                uint64_t reloc_addr = 0;
                auto relr_apply_one = [&](uint64_t vaddr) {
                    uint64_t target = nobj.base_addr + vaddr;
                    uint64_t addend = 0;
                    try { mem_.read(target, &addend, 8); } catch (...) {}
                    try { mem_.store<uint64_t>(target, nobj.base_addr + addend); } catch (...) {}
                };
                uint64_t addr = rra, end = rra + rrs;
                while (addr < end) {
                    uint64_t entry; mem_.read(addr, &entry, 8); addr += 8;
                    if ((entry & 1) == 0) {
                        reloc_addr = entry;
                        relr_apply_one(reloc_addr);
                        reloc_addr += 8;
                    } else {
                        for (int bit = 1; bit <= 63; bit++) {
                            if (entry & (1ULL << bit)) {
                                relr_apply_one(reloc_addr + (bit - 1) * 8);
                            }
                        }
                        reloc_addr += 63 * 8;
                    }
                }
            }
        } catch (...) {}
    }
    // ── Run DT_INIT and DT_INIT_ARRAY for the dlopen'd library ──
    // glibc's _dl_open calls _dl_init after all relocations are applied,
    // before returning from dlopen. This runs C++ static constructors
    // and library init hooks. Without this, dlopen'd libraries have
    // uninitialized globals (e.g., libz's crc tables, libpng's error
    // handlers, OpenSSL's algorithm tables).
    //
    // We use guest_call_args_ (which sets x0/x1/x2 and returns x0)
    // rather than init_runner_ (which doesn't pass arguments). The
    // init functions receive (argc, argv, env) per the AArch64 ABI,
    // but most init functions ignore their arguments. We pass 0/0/0.
    if (guest_call_args_) {
        if (nobj.init_addr != 0) {
            try {
                if (dynlink_trace_enabled()) {
                    fprintf(stderr, "[dlopen] DT_INIT for '%s' @ 0x%llx\n",
                            path.c_str(),
                            static_cast<unsigned long long>(nobj.init_addr));
                }
                guest_call_args_(nobj.init_addr, 0, 0, 0);
            } catch (...) {}
        }
        if (nobj.init_array_addr != 0 && nobj.init_array_size >= 8) {
            size_t count = nobj.init_array_size / 8;
            for (size_t i = 0; i < count; i++) {
                uint64_t fn = 0;
                try {
                    fn = mem_.load<uint64_t>(nobj.init_array_addr + i * 8);
                } catch (...) { break; }
                if (fn == 0) continue;
                try {
                    if (dynlink_trace_enabled()) {
                        fprintf(stderr, "[dlopen] DT_INIT_ARRAY[%zu] for '%s' @ 0x%llx\n",
                                i, path.c_str(),
                                static_cast<unsigned long long>(fn));
                    }
                    guest_call_args_(fn, 0, 0, 0);
                } catch (...) {}
            }
        }
    }
    if (dynlink_trace_enabled()) {
        fprintf(stderr, "[dlopen] loaded '%s' at 0x%llx (refcount=1)\n",
                path.c_str(), static_cast<unsigned long long>(base));
    }
    return base;
}
// ── close_library (dlclose support) ────────────────────────────────────
// Decrement the refcount. When it reaches 0, run DT_FINI_ARRAY (in
// reverse order) and DT_FINI. The memory is NOT unmapped (glibc keeps
// the link_map for safety). Returns 0 on success, -1 on error.
int DynamicLinker::close_library(uint64_t handle) {
    for (auto& obj : objects_) {
        if (obj.base_addr == handle) {
            if (obj.refcount == 0) {
                // Already closed or was loaded during link() (refcount 0).
                // glibc returns -1 with "invalid handle" in this case.
                set_last_error("invalid handle");
                return -1;
            }
            obj.refcount--;
            if (obj.refcount == 0 && guest_call_args_) {
                // Run DT_FINI_ARRAY in reverse order (glibc convention).
                if (obj.fini_array_addr != 0 && obj.fini_array_size >= 8) {
                    size_t count = obj.fini_array_size / 8;
                    for (size_t i = count; i-- > 0; ) {
                        uint64_t fn = 0;
                        try {
                            fn = mem_.load<uint64_t>(obj.fini_array_addr + i * 8);
                        } catch (...) { break; }
                        if (fn == 0) continue;
                        try {
                            if (dynlink_trace_enabled()) {
                                fprintf(stderr, "[dlclose] DT_FINI_ARRAY[%zu] for '%s' @ 0x%llx\n",
                                        i, obj.name.c_str(),
                                        static_cast<unsigned long long>(fn));
                            }
                            guest_call_args_(fn, 0, 0, 0);
                        } catch (...) {}
                    }
                }
                // DT_FINI (legacy _fini() function) — called after fini_array.
                if (obj.fini_addr != 0) {
                    try {
                        if (dynlink_trace_enabled()) {
                            fprintf(stderr, "[dlclose] DT_FINI for '%s' @ 0x%llx\n",
                                    obj.name.c_str(),
                                    static_cast<unsigned long long>(obj.fini_addr));
                        }
                        guest_call_args_(obj.fini_addr, 0, 0, 0);
                    } catch (...) {}
                }
            }
            if (dynlink_trace_enabled()) {
                fprintf(stderr, "[dlclose] '%s' refcount=%u\n",
                        obj.name.c_str(), obj.refcount);
            }
            return 0;
        }
    }
    set_last_error("invalid handle");
    return -1;
}
// ── resolve_symbol_in (dlsym with a handle) ────────────────────────────
// Search for a symbol within a specific library's scope. First checks
// the library's own .dynsym, then falls back to the global symbol
// table (which includes all loaded libraries). This matches glibc's
// _dl_sym behavior: the handle's local scope is searched first, then
// the global scope.
uint64_t DynamicLinker::resolve_symbol_in(uint64_t handle,
                                           const std::string& name) {
    // First, try the global symbol table (which includes all loaded
    // objects' exported symbols). This is fast and handles 99% of
    // dlsym calls.
    uint64_t addr = resolve_symbol(name);
    if (addr != 0) return addr;
    // Not in the global table — search the specified library's .dynsym
    // for symbols that weren't exported (STB_LOCAL or SHN_COMMON).
    for (const auto& obj : objects_) {
        if (obj.base_addr != handle) continue;
        if (obj.symtab_addr == 0 || obj.strtab_addr == 0) break;
        // Walk the .dynsym table.
        for (uint64_t i = 0; i < obj.symtab_count; i++) {
            Elf64_Sym s;
            try {
                mem_.read(obj.symtab_addr + i * sizeof(s), &s, sizeof(s));
            } catch (...) { break; }
            if (s.st_name == 0) continue;
            std::string sym_name = read_guest_cstr(mem_, obj.strtab_addr + s.st_name);
            if (sym_name == name && s.st_value != 0) {
                return obj.base_addr + s.st_value;
            }
        }
        break;
    }
    return 0;
}
// ── find_object_by_addr ────────────────────────────────────────────────
// Find the loaded object whose [base_addr, base_addr + map_size) range
// contains `addr`. Used by dladdr and _dl_find_dso_for_object.
const LoadedObject* DynamicLinker::find_object_by_addr(uint64_t addr) const {
    for (const auto& obj : objects_) {
        if (obj.base_addr == 0) continue;  // skip main binary (base 0)
        if (addr >= obj.base_addr && addr < obj.base_addr + obj.map_size) {
            return &obj;
        }
    }
    // Check the main binary (base 0, map_size unknown — check a reasonable
    // range for the main binary's text/data segments).
    for (const auto& obj : objects_) {
        if (obj.is_main && addr < 0x10000000) {
            return &obj;
        }
    }
    return nullptr;
}
// ── dladdr ─────────────────────────────────────────────────────────────
// Fill in Dl_info for a given address. Returns 1 if the address falls
// within a loaded object, 0 otherwise. Finds the nearest symbol by
// scanning the containing object's .dynsym for the symbol with the
// largest st_value that is <= addr.
int DynamicLinker::dladdr(uint64_t addr, DlInfo& info) {
    info = {0, 0, 0, 0};
    const LoadedObject* obj = find_object_by_addr(addr);
    if (obj == nullptr) return 0;
    info.dli_fbase = obj->base_addr;
    // dli_fname: copy the object's name to a guest-side buffer. For
    // dynamic binaries we reuse the dlerror buffer (set in the shim).
    // For static binaries (no shim, dlerror_buf_ptr_ == 0) we lazily
    // mmap a small page so dli_fname is non-NULL.
    uint64_t fname_buf = dlerror_buf_ptr_;
    if (fname_buf == 0) {
        if (dladdr_fname_buf_ == 0) {
            dladdr_fname_buf_ = mem_.mmap_alloc(4096);
        }
        fname_buf = dladdr_fname_buf_;
    }
    if (fname_buf != 0) {
        std::string name = obj->name;
        if (name.empty() && obj->is_main) name = "<main>";
        size_t n = std::min(name.size(), DLERROR_BUF_SIZE - 1);
        try {
            mem_.write(fname_buf,
                       reinterpret_cast<const uint8_t*>(name.data()), n);
            mem_.store<uint8_t>(fname_buf + n, 0);
            info.dli_fname = fname_buf;
        } catch (...) {
            info.dli_fname = 0;
        }
    }
    // Find the nearest symbol by scanning .dynsym. We want the symbol
    // with the largest st_value that is <= addr. When multiple symbols
    // share the same st_value (aliases like sqrt/sqrtf32x), prefer the
    // one whose address EXACTLY matches addr (canonical name), then the
    // first one encountered (deterministic).
    if (obj->symtab_addr != 0 && obj->strtab_addr != 0) {
        uint64_t best_value = 0;
        uint64_t best_sym_name_off = 0;
        bool best_is_exact = false;
        for (uint64_t i = 0; i < obj->symtab_count; i++) {
            Elf64_Sym s;
            try {
                mem_.read(obj->symtab_addr + i * sizeof(s), &s, sizeof(s));
            } catch (...) { break; }
            if (s.st_name == 0) continue;
            if (s.st_shndx == 0) continue;  // SHN_UNDEF
            uint64_t sym_addr = obj->base_addr + s.st_value;
            if (sym_addr > addr) continue;
            bool is_exact = (sym_addr == addr);
            // Prefer: exact match > larger value > first encountered.
            if (best_value == 0) {
                best_value = sym_addr;
                best_sym_name_off = s.st_name;
                best_is_exact = is_exact;
            } else if (is_exact && !best_is_exact) {
                // Exact match beats a non-exact one regardless of value.
                best_value = sym_addr;
                best_sym_name_off = s.st_name;
                best_is_exact = true;
            } else if (!is_exact && sym_addr > best_value && !best_is_exact) {
                best_value = sym_addr;
                best_sym_name_off = s.st_name;
            } else if (is_exact && best_is_exact && sym_addr == best_value) {
                // Both exact at the same address — keep the first
                // (already set), so the canonical name wins.
            }
        }
        if (best_value != 0) {
            info.dli_saddr = best_value;
            info.dli_sname = obj->strtab_addr + best_sym_name_off;
        }
    }
    return 1;
}
// ── get_last_error / set_last_error (dlerror support) ──────────────────
// The error is stored as a host string. get_last_error() copies it to
// a guest-side buffer (in the shim data area) and returns the guest
// pointer. After get_last_error() returns the string, the error is
// cleared — the next call returns 0 (matching glibc's "dlerror returns
// NULL on second call" semantics).
uint64_t DynamicLinker::get_last_error() {
    if (!error_pending_ || last_error_.empty()) {
        return 0;
    }
    if (dlerror_buf_ptr_ == 0) {
        // Buffer not allocated yet — return 0 (no error).
        error_pending_ = false;
        return 0;
    }
    // Copy the error string to the guest buffer (truncated to fit).
    size_t n = std::min(last_error_.size(), DLERROR_BUF_SIZE - 1);
    try {
        mem_.write(dlerror_buf_ptr_, reinterpret_cast<const uint8_t*>(last_error_.data()), n);
        mem_.store<uint8_t>(dlerror_buf_ptr_ + n, 0);  // null terminator
    } catch (...) {
        error_pending_ = false;
        return 0;
    }
    error_pending_ = false;
    last_error_.clear();
    return dlerror_buf_ptr_;
}
void DynamicLinker::set_last_error(const std::string& msg) {
    last_error_ = msg;
    error_pending_ = true;
}
// ── iterate_phdr (dl_iterate_phdr support) ─────────────────────────────
// Iterate over all loaded objects and call the guest callback for each.
// The callback receives a guest pointer to a dl_phdr_info struct and
// the user's data pointer. Returns the sum of callback return values.
//
// struct dl_phdr_info {
//   ElfW(Addr) dlpi_addr;          // load bias
//   const char *dlpi_name;         // library name (guest pointer)
//   const ElfW(Phdr) *dlpi_phdr;   // program headers (guest pointer)
//   ElfW(Half) dlpi_phnum;         // number of program headers
//   u64 dlpi_adds;                 // total loads
//   u64 dlpi_subs;                 // total unloads
//   size_t dlpi_tls_modid;         // TLS module ID
//   void *dlpi_tls_data;           // TLS data pointer
// };
// On AArch64 (LP64), this struct is 64 bytes.
int DynamicLinker::iterate_phdr(uint64_t callback_ptr, uint64_t data_ptr) {
    if (callback_ptr == 0 || guest_call_args_ == nullptr) return 0;
    // Allocate a scratch buffer for one dl_phdr_info struct (64 bytes)
    // plus the name string (256 bytes). We reuse the dlerror buffer area
    // since dlerror and dl_iterate_phdr don't run concurrently.
    uint64_t info_buf = dlerror_buf_ptr_ != 0 ? dlerror_buf_ptr_ : 0;
    if (info_buf == 0) return 0;  // shim not set up
    int total = 0;
    for (const auto& obj : objects_) {
        if (obj.base_addr == 0 && !obj.is_main) continue;
        // Skip the synthetic ld-linux shim — it's not a real object.
        if (obj.name == "<ld-linux-shim>") continue;
        // Write the dl_phdr_info struct to guest memory.
        uint64_t name_ptr = info_buf + 64;  // name goes after the struct
        // Write the name string.
        std::string name = obj.name;
        if (name.empty()) name = "<main>";
        size_t name_len = std::min(name.size(), static_cast<size_t>(255));
        try {
            mem_.write(name_ptr, reinterpret_cast<const uint8_t*>(name.data()), name_len);
            mem_.store<uint8_t>(name_ptr + name_len, 0);
            // Write the struct fields.
            // struct dl_phdr_info layout (LP64):
            //   +0:  ElfW(Addr) dlpi_addr      (load bias)
            //   +8:  const char *dlpi_name      (guest pointer)
            //   +16: const ElfW(Phdr) *dlpi_phdr (program headers — 0 for now)
            //   +24: ElfW(Half) dlpi_phnum      (number of phdrs — 0 for now)
            //   +28: padding to 8-byte align
            //   +32: u64 dlpi_adds              (total loads)
            //   +40: u64 dlpi_subs              (total unloads)
            //   +48: size_t dlpi_tls_modid      (TLS module ID)
            //   +56: void *dlpi_tls_data        (TLS data pointer)
            mem_.store<uint64_t>(info_buf + 0,  obj.base_addr);       // dlpi_addr
            mem_.store<uint64_t>(info_buf + 8,  name_ptr);             // dlpi_name
            mem_.store<uint64_t>(info_buf + 16, 0);                    // dlpi_phdr
            mem_.store<uint16_t>(info_buf + 24, 0);                    // dlpi_phnum
            mem_.store<uint64_t>(info_buf + 32, objects_.size());      // dlpi_adds
            mem_.store<uint64_t>(info_buf + 40, 0);                    // dlpi_subs
            mem_.store<uint64_t>(info_buf + 48, obj.tls_mod_id);       // dlpi_tls_modid
            mem_.store<uint64_t>(info_buf + 56, 0);                    // dlpi_tls_data
        } catch (...) { continue; }
        // Call the guest callback: x0 = info, x1 = sizeof(dl_phdr_info), x2 = data
        // sizeof(struct dl_phdr_info) on AArch64 LP64 = 64 bytes.
        uint64_t rc = guest_call_args_(callback_ptr, info_buf, 64, data_ptr);
        if (dynlink_trace_enabled()) {
            fprintf(stderr, "[dl_iterate_phdr] callback for '%s' returned %lld\n",
                    obj.name.c_str(), static_cast<long long>(rc));
        }
        total += static_cast<int>(rc);
        if (rc != 0) break;  // callback returns non-zero to stop iteration
    }
    return total;
}
} // namespace arm64emu
