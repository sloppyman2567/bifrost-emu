// frontend/dynamic_linker.cpp — Dynamic linking support for bifrost-emu.
//
// See dynamic_linker.h for the design overview.
//
// The relocations applied here match the AArch64 ELF ABI (ARM IHI 0056B):
//   R_AARCH64_ABS64      (257)  : *(addr) = S + A
//   R_AARCH64_GLOB_DAT   (1025) : *(addr) = S + A
//   R_AARCH64_JUMP_SLOT  (1026) : *(addr) = S + A   (lazy: leave PLT stub)
//   R_AARCH64_RELATIVE   (1027) : *(addr) = Delta + A   (Delta = base)
//   R_AARCH64_TLS_TPREL  (1030) : not applied (TLS unsupported)
//   R_AARCH64_TLSDESC    (1031) : not applied (TLS unsupported)
//   R_AARCH64_IRELATIVE  (1032) : *(addr) = Indirect(Delta + A)
//                                  — calls the ifunc resolver at Delta + A
//                                    and stores its return value.
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

// BUGFIX (Turn 60, C5): symbol versioning tags.
constexpr int DT_VERSYM_    = 0x6FFFFFF0;
constexpr int DT_VERDEF_    = 0x6FFFFFFC;
constexpr int DT_VERDEFNUM_ = 0x6FFFFFFD;
constexpr int DT_VERNEED_   = 0x6FFFFFFE;
constexpr int DT_VERNEEDNUM_= 0x6FFFFFFF;

// AArch64 relocation types (ELF64 codes), per ARM IHI 0056B.
constexpr uint32_t R_AARCH64_ABS64_         = 257;
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
                         const std::string& main_path) {
    objects_.clear();
    symbols_.clear();
    versioned_symbols_.clear();  // Turn 60, C5
    error_.clear();

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
    parse_versions_(objects_.back());  // Turn 60, C5

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
                    // BUGFIX (Turn 59, L6): dedup by DT_SONAME when present,
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

                    // BUGFIX (Turn 59, C6): pass the parent object's
                    // DT_RUNPATH so find_library can search it for
                    // transitive deps. (DT_RUNPATH only applies to the
                    // immediate object's DT_NEEDED per the gABI; we
                    // approximate by passing the parent's runpath.)
                    uint64_t lib_base = load_shared_library(soname,
                        objects_[idx].runpath, objects_[idx].rpath);
                    if (lib_base == 0) {
                        // Library not found — not necessarily fatal
                        // (some programs dlopen at runtime). Log and
                        // continue.
                        fprintf(stderr, "[%s] dynamic linker: could not "
                                "find %s (continuing)\n",
                                CODENAME, soname.c_str());
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
    // dereferences the (zero) GOT slots. The shim must be registered
    // AFTER all libraries are loaded (so the shim's symbols can be
    // overridden by real ld-linux symbols if the guest actually loaded
    // one) but BEFORE relocations are applied (so GLOB_DAT/JUMP_SLOT
    // relocations against these symbols resolve to the shim's
    // addresses).
    //
    // We only register the shim if no real ld-linux was loaded. If
    // the guest's PT_INTERP was found and loaded as a regular shared
    // library, its symbols are already in the table and the shim
    // would just shadow them (the shim's `symbols_[name] = ...` only
    // sets if not already present — see the `add_*_sym` lambdas
    // above, which we'll make conditional on first-define-wins).
    bool has_real_ld = false;
    for (const auto& o : objects_) {
        if (o.name.find("ld-linux") != std::string::npos ||
            o.name.find("ld-musl") != std::string::npos ||
            o.name.find("ld.so") != std::string::npos) {
            has_real_ld = true;
            break;
        }
    }
    if (!has_real_ld) {
        register_ld_linux_shim_();
    }

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
            // BUGFIX (Turn 39): d_val for these tags is a vaddr RELATIVE
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
            Elf64_Dyn dyn;
            for (uint64_t p = obj.dyn_addr; ; p += sizeof(dyn)) {
                mem_.read(p, &dyn, sizeof(dyn));
                if (dyn.d_tag == DT_NULL_) break;
                if (dyn.d_tag == DT_RELA_)      rela_addr = obj.base_addr + dyn.d_val;
                else if (dyn.d_tag == DT_RELASZ_)    rela_size = dyn.d_val;
                else if (dyn.d_tag == DT_JMPREL_)    jmprel_addr = obj.base_addr + dyn.d_val;
                else if (dyn.d_tag == DT_PLTRELSZ_)  jmprel_size = dyn.d_val;
            }
            if (getenv("BIFROST_DYNLINK_TRACE")) {
                fprintf(stderr, "[dynlink] obj '%s' base=0x%llx: "
                        "RELA=0x%llx/%llu JMPREL=0x%llx/%llu\n",
                        obj.name.c_str(),
                        static_cast<unsigned long long>(obj.base_addr),
                        static_cast<unsigned long long>(rela_addr),
                        static_cast<unsigned long long>(rela_size),
                        static_cast<unsigned long long>(jmprel_addr),
                        static_cast<unsigned long long>(jmprel_size));
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
                        mem_.store<uint64_t>(target, obj.base_addr + A);
                    } else if (type == R_AARCH64_ABS64_ ||
                               type == R_AARCH64_GLOB_DAT_) {
                        if (sym == 0) {
                            mem_.store<uint64_t>(target, obj.base_addr + A);
                        } else {
                            // Read symbol name from obj's symtab.
                            Elf64_Sym s;
                            mem_.read(obj.symtab_addr + sym * sizeof(s),
                                      &s, sizeof(s));
                            std::string name = read_guest_cstr(
                                mem_, obj.strtab_addr + s.st_name);
                            // BUGFIX (Turn 60, C5): use resolve_reloc_symbol
                            // which consults versioned_symbols_ when the
                            // object has .gnu.version_r. This prevents
                            // wrong-version symbol selection.
                            uint64_t S = resolve_reloc_symbol(obj, sym);
                            // BUGFIX (Turn 59, C3): undefined-weak symbols
                            // must resolve to 0, NOT obj.base_addr. The old
                            // fallback `S = obj.base_addr + s.st_value` ran
                            // for SHN_UNDEF symbols where st_value==0, so
                            // S became obj.base_addr — the GOT slot pointed
                            // to the start of the binary instead of 0.
                            // Real ld.so: unresolved weak UNDEF → S = 0.
                            if (S == 0 && s.st_shndx != SHN_UNDEF_) {
                                S = obj.base_addr + s.st_value;
                            }
                            mem_.store<uint64_t>(target, S + A);
                        }
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
                            uint64_t sym_addr = resolve_symbol(name);
                            if (sym_addr != 0) {
                                for (const auto& o : objects_) {
                                    if (sym_addr >= o.base_addr &&
                                        sym_addr < o.base_addr + 0x10000000) {
                                        mod_id = o.tls_mod_id;
                                        break;
                                    }
                                }
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
                                for (const auto& o : objects_) {
                                    if (sym_addr >= o.base_addr &&
                                        sym_addr < o.base_addr + 0x10000000) {
                                        mod_tp_off = o.tls_tp_offset;
                                        break;
                                    }
                                }
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
                                for (const auto& o : objects_) {
                                    if (sym_addr >= o.base_addr &&
                                        sym_addr < o.base_addr + 0x10000000) {
                                        mod_tp_off = o.tls_tp_offset;
                                        break;
                                    }
                                }
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
                            // BUGFIX (Turn 60, C5): versioned resolution.
                            uint64_t S = resolve_reloc_symbol(obj, sym);
                            // BUGFIX (Turn 59, C3): undefined-weak → 0, not base_addr.
                            if (S == 0 && s.st_shndx != SHN_UNDEF_) {
                                S = obj.base_addr + s.st_value;
                            }
                            mem_.store<uint64_t>(target, S + A);
                        }
                    }
                }
            }
            // PLT relocations (DT_JMPREL) — eager binding.
            // BUGFIX (Turn 39): the old code only processed DT_RELA and
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
                            // BUGFIX (Turn 60, C5): versioned resolution.
                            uint64_t S = resolve_reloc_symbol(obj, sym);
                            // BUGFIX (Turn 59, C3): undefined-weak → 0, not base_addr.
                            if (S == 0 && s.st_shndx != SHN_UNDEF_) {
                                S = obj.base_addr + s.st_value;
                            }
                            mem_.store<uint64_t>(target, S + A);
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

    // BUGFIX (Turn 59, C1): invoke DT_INIT and DT_INIT_ARRAY for each
    // loaded object (libs first, main last). Runs C++ static constructors,
    // glibc __libc_start_main hooks, etc. Without this, every C++ game
    // runs with uninitialized globals (vtables, std::mutex, std::string).
    // Requires init_runner_ to be set by the Emulator; no-ops if not.
    run_init_arrays_();

    return true;
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
    for (const auto& obj : objects_) {
        // DT_INIT (legacy _init() function) — call before .init_array.
        if (obj.init_addr != 0) {
            try {
                if (getenv("BIFROST_DYNLINK_TRACE")) {
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
                    if (getenv("BIFROST_DYNLINK_TRACE")) {
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

    // BUGFIX (Turn 59, H1/H2): validate e_phoff and e_phentsize before
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
            // BUGFIX (Turn 39): the field at p+8 is p_offset, NOT p_vaddr.
            // The ELF64 program header layout is:
            //   offset 0:  p_type   (4 bytes)
            //   offset 4:  p_flags  (4 bytes)
            //   offset 8:  p_offset (8 bytes)  ← file offset
            //   offset 16: p_vaddr  (8 bytes)  ← virtual address
            //   offset 24: p_paddr  (8 bytes)
            //   offset 32: p_filesz (8 bytes)
            //   offset 40: p_memsz  (8 bytes)
            //   offset 48: p_align  (8 bytes)
            // The old code read p_offset into dyn_vaddr, which happened
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
        // BUGFIX (Turn 59, H1): bounds-check this loop too (was missing).
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
    // BUGFIX (Turn 59): capture DT_INIT/DT_FINI/DT_INIT_ARRAY/DT_FINI_ARRAY
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
            // BUGFIX (Turn 60, C5): capture symbol versioning section addrs.
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

    // BUGFIX (Turn 59, H5): derive symtab_count from DT_HASH when present.
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
    // BUGFIX (Turn 59, C6): these were declared as constants but never
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

// ── apply_relocations (deprecated — now inline in link()) ──────────────
bool DynamicLinker::apply_relocations(const std::vector<uint8_t>& data,
                                      LoadedObject& obj) {
    (void)data; (void)obj;
    return true;  // handled in link()
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

    // Allocate 2 pages: data + code.
    constexpr uint64_t SHIM_SIZE = 8192;
    shim_base_ = mem_.mmap_alloc(SHIM_SIZE);
    if (shim_base_ == 0) {
        error_ = "register_ld_linux_shim_: mmap_alloc failed";
        return false;
    }

    // ── Data page (shim_base_ .. shim_base_+4096) ─────────────────
    // Zero the entire data page (mmap_alloc already does this, but be
    // explicit in case the page was reused from a previous allocation).
    std::vector<uint8_t> zero(4096, 0);
    mem_.write(shim_base_, zero.data(), 4096);

    // Layout (offsets within the data page):
    //   0x000: _rtld_global_ro (256 bytes — glibc reads up to offset 568
    //          for dl_signal_error, dl_catch_error, etc.; we zero it all).
    //   0x100: _rtld_global (256 bytes — glibc reads _dl_ns, _dl_nns).
    //   0x200: _dl_argv (8 bytes — pointer to argv; libc sets this).
    //   0x208: __libc_enable_secure (4 bytes — 0 = not secure).
    //   0x20C: __pointer_chk_guard (8 bytes — random XOR canary).
    //   0x214: _dl_start_args (16 bytes — start time, etc.).
    //   0x224: padding to 0x300.
    //   0x300: function pointer table (8 bytes each, 16 entries):
    //          [0] _dl_find_dso_for_object  → code page + 0
    //          [1] _dl_allocate_tls         → code page + 8
    //          [2] _dl_allocate_tls_init    → code page + 16
    //          [3] _dl_deallocate_tls       → code page + 24
    //          [4] _dl_signal_error         → code page + 32 (abort)
    //          [5] _dl_signal_exception     → code page + 40 (abort)
    //          [6] _dl_catch_exception      → code page + 48
    //          [7] _dl_catch_error          → code page + 56
    //          [8] _dl_audit_symbind_alt    → code page + 64
    //          [9] _dl_audit_preinit        → code page + 72
    //          [10] _dl_rtld_di_serinfo     → code page + 80
    //          [11] _dl_call_fini           → code page + 88
    //          [12] __tls_get_addr          → code page + 96
    //          [13] __tunable_get_val       → code page + 104
    //          [14] __nptl_change_stack_perm→ code page + 112
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

    // ── Code page (shim_base_+4096 .. shim_base_+8192) ────────────
    // Each stub is 2 instructions (8 bytes). Stubs that "abort" actually
    // call abort() — we resolve abort via the global symbol table at
    // registration time (it's a libc symbol that's already indexed).
    // If abort isn't found yet (e.g., shim registered before libc
    // loaded), the abort stubs just BRK #1000 (visible crash) instead.
    uint64_t code_base = shim_base_ + 4096;

    // ARM64 instruction encodings:
    //   mov x0, #0   → 0xD2800000
    //   ret          → 0xD65F03C0
    //   brk #1000    → 0xD4207D00  (musl's a_crash)
    //   bl <imm26>   → 0x94000000 | (imm26 & 0x03FFFFFF)
    //                 where imm26 = (target - (pc+4)) >> 2
    auto emit_mov_x0_0 = [](std::vector<uint8_t>& v) {
        v.push_back(0x00); v.push_back(0x00); v.push_back(0x80); v.push_back(0xD2);
    };
    auto emit_ret = [](std::vector<uint8_t>& v) {
        v.push_back(0xC0); v.push_back(0x03); v.push_back(0x5F); v.push_back(0xD6);
    };
    auto emit_brk_1000 = [](std::vector<uint8_t>& v) {
        v.push_back(0x00); v.push_back(0x7D); v.push_back(0x20); v.push_back(0xD4);
    };
    // Emit `bl target` where target is an absolute address. We use a
    // placeholder and patch the offset later when we know the abort
    // address (if any). For simplicity, we emit BRK for abort stubs
    // — they're never called in normal operation, only on genuine
    // dynamic-linker errors, which should be visible.
    auto emit_stub_return0 = [&](std::vector<uint8_t>& v) {
        emit_mov_x0_0(v);
        emit_ret(v);
    };
    auto emit_stub_void = [&](std::vector<uint8_t>& v) {
        emit_ret(v);
        // Pad to 8 bytes (one more instruction — NOP).
        v.push_back(0x1F); v.push_back(0x20); v.push_back(0x03); v.push_back(0xD5);
    };
    auto emit_stub_abort = [&](std::vector<uint8_t>& v) {
        // For error-signaling stubs, we BRK #1000 to make any
        // dynamic-linker error immediately visible. In production,
        // these should never be called (libc only calls them on
        // actual dlopen/dlsym errors, which we don't support).
        emit_brk_1000(v);
        // Pad with NOP in case the BRK is skipped (it won't be).
        v.push_back(0x1F); v.push_back(0x20); v.push_back(0x03); v.push_back(0xD5);
    };

    std::vector<uint8_t> code;
    code.reserve(128);
    // [0] _dl_find_dso_for_object (returns void*)
    emit_stub_return0(code);     // offset 0
    // [1] _dl_allocate_tls (returns void*)
    emit_stub_return0(code);     // offset 8
    // [2] _dl_allocate_tls_init (returns void*)
    emit_stub_return0(code);     // offset 16
    // [3] _dl_deallocate_tls (void)
    emit_stub_void(code);        // offset 24
    // [4] _dl_signal_error (noreturn)
    emit_stub_abort(code);       // offset 32
    // [5] _dl_signal_exception (noreturn)
    emit_stub_abort(code);       // offset 40
    // [6] _dl_catch_exception (returns int)
    emit_stub_return0(code);     // offset 48
    // [7] _dl_catch_error (returns int)
    emit_stub_return0(code);     // offset 56
    // [8] _dl_audit_symbind_alt (void)
    emit_stub_void(code);        // offset 64
    // [9] _dl_audit_preinit (void)
    emit_stub_void(code);        // offset 72
    // [10] _dl_rtld_di_serinfo (returns int)
    emit_stub_return0(code);     // offset 80
    // [11] _dl_call_fini (void)
    emit_stub_void(code);        // offset 88
    // [12] __tls_get_addr (returns void*)
    emit_stub_return0(code);     // offset 96
    // [13] __tunable_get_val (returns int)
    emit_stub_return0(code);     // offset 104
    // [14] __nptl_change_stack_perm (void)
    emit_stub_void(code);        // offset 112
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
    for (int i = 0; i < 15; i++) {
        mem_.store<uint64_t>(shim_base_ + FPTR_TABLE_OFF + i * 8,
                             code_base + i * 8);
    }

    // ── Register symbols in the global symbol table ──────────────
    // Data symbols point into the data page; function symbols point
    // into the code page.
    auto add_data_sym = [&](const char* name, uint64_t off) {
        symbols_[name] = SymEntry{shim_base_ + off, STB_GLOBAL_};
    };
    auto add_func_sym = [&](const char* name, uint64_t off) {
        symbols_[name] = SymEntry{code_base + off, STB_GLOBAL_};
    };

    // Data symbols (from ld-linux that libc references).
    add_data_sym("_rtld_global_ro", RTLD_GLOBAL_RO_OFF);
    add_data_sym("_rtld_global",    RTLD_GLOBAL_OFF);
    add_data_sym("_dl_argv",        DL_ARGV_OFF);
    add_data_sym("__libc_enable_secure", LIBC_ENABLE_SECURE_OFF);
    add_data_sym("__pointer_chk_guard",  POINTER_CHK_GUARD_OFF);

    // Function symbols (from ld-linux that libc references).
    add_func_sym("_dl_find_dso_for_object", 0);
    add_func_sym("_dl_allocate_tls",        8);
    add_func_sym("_dl_allocate_tls_init",   16);
    add_func_sym("_dl_deallocate_tls",      24);
    add_func_sym("_dl_signal_error",        32);
    add_func_sym("_dl_signal_exception",    40);
    add_func_sym("_dl_catch_exception",     48);
    add_func_sym("_dl_catch_error",         56);
    add_func_sym("_dl_audit_symbind_alt",   64);
    add_func_sym("_dl_audit_preinit",       72);
    add_func_sym("_dl_rtld_di_serinfo",     80);
    add_func_sym("_dl_call_fini",           88);
    add_func_sym("__tls_get_addr",          96);
    add_func_sym("__tunable_get_val",       104);
    add_func_sym("__nptl_change_stack_perm", 112);

    // Also register a synthetic LoadedObject so the shim shows up in
    // /proc/self/maps and the allocations_ tracker (for fork safety).
    LoadedObject shim_obj;
    shim_obj.name = "<ld-linux-shim>";
    shim_obj.base_addr = shim_base_;
    shim_obj.is_main = false;
    shim_obj.dyn_addr = 0;  // no PT_DYNAMIC
    objects_.push_back(std::move(shim_obj));

    if (getenv("BIFROST_DYNLINK_TRACE")) {
        fprintf(stderr, "[dynlink] registered ld-linux shim: "
                "data @0x%llx, code @0x%llx (15 stubs)\n",
                static_cast<unsigned long long>(shim_base_),
                static_cast<unsigned long long>(code_base));
    }
    return true;
}


// ── find_library ───────────────────────────────────────────────────────
// BUGFIX (Turn 59, C6): accept parent_runpath and parent_rpath (from the
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
// BUGFIX (Turn 59, C6): accept parent_runpath and parent_rpath so
// find_library can search the parent object's DT_RUNPATH/DT_RPATH for
// this library. DT_RUNPATH only applies to the immediate object's
// DT_NEEDED per the gABI; DT_RPATH is global (deprecated but still used).
uint64_t DynamicLinker::load_shared_library(const std::string& soname,
                                             const std::string& parent_runpath,
                                             const std::string& parent_rpath) {
    std::string path;
    auto data = find_library(soname, path, parent_runpath, parent_rpath);
    if (data.empty()) {
        // Library not found on disk. If a thunk resolver is registered
        // and this is a known graphic library, register a synthetic
        // LoadedObject whose symbols resolve via the thunk (Turn 37).
        // This lets dynamically-linked guest programs that use GL/EGL/
        // SDL2 work without the host having the AArch64 versions of
        // those libraries installed.
        if (thunk_resolver_ && is_thunk_supported_lib_(soname)) {
            return register_thunk_library_(soname);
        }
        return 0;
    }

    // Allocate a fresh base address via the Memory's mmap_alloc().
    // BUGFIX (Turn 39): the old code used a separate next_lib_base_
    // counter starting at 0x5000000000 — the SAME address as
    // mmap_alloc()'s starting region. This meant the thunk's trampoline
    // page (allocated via mmap_alloc in GraphicThunk::init) could
    // collide with the first library loaded here, causing the library
    // to overwrite the trampolines → "decode error at pc=0x5000000020
    // inst=0x00000040" when the guest tried to call a thunked function.
    // The fix: use mem_.mmap_alloc() for library bases, so the
    // allocator tracks ALL high-memory allocations and prevents
    // collisions. next_lib_base_ is now unused (kept in the header for
    // ABI compat but never read).
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
    parse_versions_(objects_.back());  // Turn 60, C5
    return base;
}

// ── register_thunk_library_ (Turn 37) ──────────────────────────────────
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

    if (getenv("BIFROST_DYNLINK_TRACE")) {
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
        || starts_with(soname, "libGLESv2.so")) {
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
// Lay out each PT_TLS block in a contiguous region. The TPIDR_EL0
// register points to the END of the block (TP = base + total_size);
// each module's TP-offset is negative (its block is below TP).
//
// Layout (mirrors glibc/musl static TLS):
//   [base .. base+libc_memsz)              — module 1 (libc)
//   [base+libc_memsz .. base+total)         — module 2 (main binary)
//   ...
// TP-offset for module i = (its_start_offset) - total_size
//   (e.g., libc at offset 0 → tp_off = -total_size)
//   (main  at offset libc_memsz → tp_off = libc_memsz - total_size)
//
// We copy initialized TLS data from each object's PT_TLS filesz into
// the block; the rest (.bss) is zero-filled (mmap gives us zero pages).
void DynamicLinker::allocate_static_tls() {
    if (static_tls_base_ != 0) return;  // already allocated

    // Compute total size with alignment.
    uint64_t total = 0;
    uint64_t max_align = 16;  // minimum alignment (TP must be 16-aligned)
    for (auto& obj : objects_) {
        if (!obj.tls.present || obj.tls.memsz == 0) continue;
        if (obj.tls.align > max_align) max_align = obj.tls.align;
        // Align current offset up to obj.tls.align.
        total = (total + obj.tls.align - 1) & ~(obj.tls.align - 1);
        obj.tls_mod_id = next_tls_mod_id_++;
        obj.tls_tp_offset = static_cast<int64_t>(total);  // tentative; finalized below
        total += obj.tls.memsz;
    }
    if (total == 0) return;

    // Round up total to max_align.
    total = (total + max_align - 1) & ~(max_align - 1);
    static_tls_size_ = total;

    // Allocate guest memory for the block.
    // Use mmap_alloc to get a fresh region (typically near other allocations).
    static_tls_base_ = mem_.mmap_alloc(total + 16);  // +16 for TCB
    if (static_tls_base_ == 0) {
        error_ = "failed to allocate static TLS block";
        return;
    }

    // TP = base + total (points to the byte AFTER the block).
    // TP-offsets become negative: tp_off = obj_start_offset - total.
    uint64_t cursor = 0;
    for (auto& obj : objects_) {
        if (!obj.tls.present || obj.tls.memsz == 0) continue;
        // Align cursor.
        cursor = (cursor + obj.tls.align - 1) & ~(obj.tls.align - 1);
        // Finalize TP-offset (negative).
        obj.tls_tp_offset = static_cast<int64_t>(cursor) - static_cast<int64_t>(total);

        // Copy initialized data from obj's PT_TLS filesz.
        // obj.tls.vaddr is a file vaddr (relative to base); add base_addr
        // to get the guest VA where the initialized data lives.
        uint64_t src = obj.base_addr + obj.tls.vaddr;
        uint64_t dst = static_tls_base_ + cursor;
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
        cursor += obj.tls.memsz;
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
// BUGFIX (Turn 60, C5): if the object has .gnu.version, look up the
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

// ── parse_versions_ (Turn 60, C5) ──────────────────────────────────────
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

    for (size_t i = 0; i < count; i++) {
        Elf64_Sym s;
        try {
            mem_.read(obj.symtab_addr + i * sizeof(s), &s, sizeof(s));
        } catch (...) { break; }
        if (s.st_name == 0 && s.st_value == 0 && s.st_shndx == 0) break;
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
    // BUGFIX (Turn 59, H5): use obj.symtab_count (from DT_HASH nchain when
    // available, 8192 cap fallback). Previously hardcoded 8192, dropping
    // symbols past the cap in large libs (Qt, webkit).
    // BUGFIX (Turn 59, H6): "first strong wins" — a strong symbol never
    // overrides an existing strong symbol; a weak symbol is overridden by
    // a strong one. Previously "last strong wins" which let load order
    // silently swap library implementations.
    // BUGFIX (Turn 59, C4): STT_GNU_IFUNC (type 10) symbols store the
    // RESOLVER address in st_value, not the function address. For ifuncs,
    // we call the resolver (via ifunc_resolver_) and store the resolved
    // address. Without this, glibc's memcpy/memset/strcmp (which are
    // ifuncs) would jump to the resolver body as if it were the function.
    constexpr size_t MAX_SYMS = 8192;  // safety cap when symtab_count is bogus
    size_t count = obj.symtab_count;
    if (count == 0 || count > MAX_SYMS * 4) count = MAX_SYMS;  // sanity
    constexpr uint8_t STT_GNU_IFUNC_ = 10;
    auto ST_TYPE_ = [](uint8_t info) { return info & 0xF; };
    for (size_t i = 0; i < count; i++) {
        Elf64_Sym s;
        try {
            mem_.read(obj.symtab_addr + i * sizeof(s), &s, sizeof(s));
        } catch (...) {
            break;
        }
        // BUGFIX (Turn 59, M9): break (not continue) on the end-of-table
        // sentinel. The old `continue` caused the loop to scan all 8192
        // entries even when the table was short, wasting ~1.6 GiB of
        // redundant reads across a heavy game load. (Some ELF objects
        // have a real STN_UNDEF entry in the middle, but those are rare
        // and the break only triggers on the all-zero sentinel which is
        // the conventional end marker.)
        if (s.st_name == 0 && s.st_value == 0 && s.st_shndx == 0) {
            break;
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
        if (getenv("BIFROST_DYNLINK_TRACE")) {
            fprintf(stderr, "[dynlink] resolve_symbol: '%s' NOT FOUND\n",
                    name.c_str());
        }
        return 0;
    }
    if (getenv("BIFROST_DYNLINK_TRACE")) {
        fprintf(stderr, "[dynlink] resolve_symbol: '%s' -> 0x%llx\n",
                name.c_str(),
                static_cast<unsigned long long>(it->second.addr));
    }
    return it->second.addr;
}

} // namespace arm64emu
