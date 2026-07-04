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
                    bool found = false;
                    for (const auto& o : objects_) {
                        if (o.name == soname) { found = true; break; }
                    }
                    if (found) continue;

                    uint64_t lib_base = load_shared_library(soname);
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
                            uint64_t S = resolve_symbol(name);
                            if (S == 0) S = obj.base_addr + s.st_value;
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
                            uint64_t S = resolve_symbol(name);
                            if (S == 0) S = obj.base_addr + s.st_value;
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
                            uint64_t S = resolve_symbol(name);
                            if (S == 0) S = obj.base_addr + s.st_value;
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

    return true;
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
    uint64_t symtab_vaddr = 0, strtab_vaddr = 0;
    for (uint64_t off = dyn_off;
         off + sizeof(Elf64_Dyn) <= data.size() && off < dyn_off + dyn_filesz;
         off += sizeof(Elf64_Dyn)) {
        Elf64_Dyn dyn;
        memcpy(&dyn, data.data() + off, sizeof(dyn));
        if (dyn.d_tag == DT_NULL_) break;
        switch (dyn.d_tag) {
            case DT_SYMTAB_:  symtab_vaddr = dyn.d_val; break;
            case DT_STRTAB_:  strtab_vaddr = dyn.d_val; break;
            case DT_JMPREL_:  obj.jmprel_addr = base + dyn.d_val; break;
            case DT_PLTRELSZ_: obj.jmprel_size = dyn.d_val; break;
            default: break;
        }
    }
    obj.symtab_addr = base + symtab_vaddr;
    obj.strtab_addr = base + strtab_vaddr;

    // Count symbols: the .dynsym section has no explicit size in the
    // dynamic section; we infer it from DT_HASH (nchain) if present,
    // or from the gap between symtab and strtab. The simplest reliable
    // heuristic: read until we hit an unmapped region or 4096 symbols.
    // Most libraries have < 4096 exported symbols.
    obj.symtab_count = 4096;  // upper bound; resolve_symbol stops at first

    return true;
}

// ── apply_relocations (deprecated — now inline in link()) ──────────────
bool DynamicLinker::apply_relocations(const std::vector<uint8_t>& data,
                                      LoadedObject& obj) {
    (void)data; (void)obj;
    return true;  // handled in link()
}

// ── find_library ───────────────────────────────────────────────────────
std::vector<uint8_t> DynamicLinker::find_library(const std::string& soname,
                                                 std::string& found_path) {
    // Search order (matches Linux ld.so behavior for AArch64 multiarch):
    //   1. /usr/aarch64-linux-gnu/lib/         (Debian/Ubuntu multiarch)
    //   2. /usr/lib/aarch64-linux-gnu/         (newer Debian multiarch)
    //   3. /lib/aarch64-linux-gnu/             (Debian multiarch)
    //   4. /usr/lib/                            (host libs, fallback)
    //   5. LD_LIBRARY_PATH entries
    std::vector<std::string> dirs = {
        "/usr/aarch64-linux-gnu/lib",
        "/usr/lib/aarch64-linux-gnu",
        "/lib/aarch64-linux-gnu",
        "/usr/lib",
        "/lib",
    };
    const char* llp = getenv("LD_LIBRARY_PATH");
    if (llp) {
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
uint64_t DynamicLinker::load_shared_library(const std::string& soname) {
    std::string path;
    auto data = find_library(soname, path);
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
            symbols_[sym] = addr;
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
// Returns true if `soname` matches the naming pattern of a graphic
// library that the thunk resolver might handle. We accept any libGL*,
// libEGL*, libSDL2*, or libGLESv2* soname (with or without version
// suffix). The thunk itself does the final accept/reject — this is
// just a fast filter to avoid calling the resolver for libc/libm/etc.
bool DynamicLinker::is_thunk_supported_lib_(const std::string& soname) {
    auto starts_with = [](const std::string& s, const char* p) {
        return s.rfind(p, 0) == 0;
    };
    return starts_with(soname, "libGL.so")
        || starts_with(soname, "libEGL.so")
        || starts_with(soname, "libSDL2")
        || starts_with(soname, "libGLESv2.so");
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
    return resolve_symbol(name);
}

// ── index_symbols ──────────────────────────────────────────────────────
void DynamicLinker::index_symbols(const LoadedObject& obj) {
    if (obj.symtab_addr == 0 || obj.strtab_addr == 0) return;
    // Iterate the .dynsym. We don't know the exact count, so we read
    // up to a reasonable limit (4096). Each symbol is 24 bytes.
    // Stop when st_name is 0 and st_value is 0 (typical end-of-table
    // sentinel).
    constexpr size_t MAX_SYMS = 8192;
    for (size_t i = 0; i < MAX_SYMS; i++) {
        Elf64_Sym s;
        try {
            mem_.read(obj.symtab_addr + i * sizeof(s), &s, sizeof(s));
        } catch (...) {
            break;
        }
        if (s.st_name == 0 && s.st_value == 0 && s.st_shndx == 0) {
            // End of table (or empty entry). Continue scanning — there
            // may be more symbols after a STN_UNDEF entry.
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
        // First definition wins (matches ld.so behavior for non-weak
        // symbols; weak symbols are overridden by strong ones — but we
        // keep it simple and just take the first).
        if (symbols_.count(name) == 0) {
            symbols_[name] = addr;
        } else if (bind == STB_GLOBAL_) {
            // Strong symbol overrides weak.
            symbols_[name] = addr;
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
                static_cast<unsigned long long>(it->second));
    }
    return it->second;
}

} // namespace arm64emu
