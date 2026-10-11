// frontend/dynamic_linker.cpp — Dynamic linking support for bifrost-emu.
//
// See DynamicLinker.hpp for the design overview.
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
//   R_AARCH64_TLSDESC    (1031) : 16-byte descriptor. desc[0] = resolver
//                                  stub (returns the TP-offset stored at
//                                  [x0+8], glibc's __tlsdesc_return ABI),
//                                  desc[1] = TP-offset
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
#include "System/Loader/DynamicLinker.hpp"
#include "System/Core/CPU.hpp"
#include "System/Memory/Memory.hpp"
#include "Bifrost/Types.hpp"
#include "Bifrost/Version.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include "System/Loader/DynamicLinker/LinkerInternal.hpp"
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
// ── DynamicLinker::link ────────────────────────────────────────────────
bool DynamicLinker::link(CPU& cpu,
                         const std::vector<uint8_t>& main_data,
                         uint64_t main_base,
                         const std::string& main_path,
                         const std::string& interp_path) {
    std::lock_guard<std::recursive_mutex> lk(loader_mu_);
    objects_.clear();
    symbols_.clear();
    versioned_symbols_.clear();
    error_.clear();
    next_tls_mod_id_ = 1;
    tls_surplus_used_ = 0;
    static_tls_size_ = 0;
    static_tls_base_ = 0;
    static_tls_alloc_base_ = 0;
    static_tls_alloc_size_ = 0;
    lib_tls_size_ = 0;
    tcb_size_ = 0;
    tls_static_alignment_ = 16;
    rseq_offset_ = 0;
    dtv_storage_offset_ = 0;
    dtv_capacity_ = 0;
    tls_generation_ = 1;
    tls_surplus_base_ = 0;
    pending_tls_static_size_ = 0;
    pending_tls_static_align_ = 64;
    tlsdesc_indices_.clear();
    tls_thread_allocations_.clear();
    tls_thread_blocks_.clear();
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
    // Compute the main binary's mapping extent (highest PT_LOAD
    // p_vaddr+p_memsz) so dlfo_map_start/map_end are meaningful.
    if (main_data.size() >= 56) {
        uint64_t e_phoff; uint16_t e_phentsize, e_phnum;
        memcpy(&e_phoff,     main_data.data() + 32, 8);
        memcpy(&e_phentsize, main_data.data() + 54, 2);
        memcpy(&e_phnum,     main_data.data() + 56, 2);
        uint64_t max_end = 0;
        for (int i = 0; i < e_phnum; i++) {
            const uint8_t* p = main_data.data() + e_phoff + i * e_phentsize;
            uint32_t pt; uint64_t pv, pm;
            memcpy(&pt, p + 0, 4); memcpy(&pv, p + 16, 8); memcpy(&pm, p + 40, 8);
            if (pt == 1) { uint64_t e = pv + pm; if (e > max_end) max_end = e; }
        }
        main_obj.map_size = max_end;
    }
    if (!parse_dynamic(main_data, main_base, main_obj)) {
        return false;
    }
    parse_tls(main_data, main_obj);
    parse_eh_frame(main_data, main_base, main_obj);
    objects_.push_back(std::move(main_obj));
    index_symbols(cpu, objects_.back());
    parse_versions_(cpu, objects_.back());
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
                    uint64_t lib_base = load_shared_library(cpu, soname,
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
                                        "(run ./Scripts/Setup/setup-rootfs.sh)\n",
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
                            resolved = ifunc_resolver_(cpu, resolver_addr);
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
                        install_tlsdesc_(obj, sym, A, target);
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
                    } else if (type == R_AARCH64_TLSDESC_) {
                        // Some toolchains (GCC for Qt5Core) place
                        // R_AARCH64_TLSDESC relocations at the END of
                        // .rela.plt (DT_JMPREL), not in .rela.dyn. Apply
                        // the same 16-byte descriptor as the RELA loop.
                        install_tlsdesc_(obj, sym, A, target);
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
    // ld-linux normally computes this from the final static TLS layout.
    // Our native linker bypasses that startup code, so publish the offset
    // for glibc's rseq initialization before libc's startup code runs.
    patch_rseq_offset_();
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
    // Funnelled glibc 2.42+: libc's _dl_find_object trampoline forwards
    // through _rtld_global_ro.dl_find_object, which ld-linux's
    // _dl_start_final normally writes but our native dynlink never runs.
    // Fill the funnel slots so the trampoline doesn't `br 0`.
    patch_rtld_funnel_();

    // _rtld_global. Must happen before __libc_early_init and before the
    // program runs pthread_create. See init_nptl_stack_lists_() for the
    // full rationale. (No-op for musl, which has no _rtld_global.)
    init_nptl_stack_lists_();
    init_main_thread_descriptor_(cpu);
    if (init_runner_) {
        uint64_t early_init = resolve_symbol("__libc_early_init");
        if (early_init != 0) {
            if (dynlink_trace_enabled()) {
                fprintf(stderr, "[dynlink] calling __libc_early_init @ 0x%llx\n",
                        static_cast<unsigned long long>(early_init));
            }
            init_runner_(cpu, early_init);
            if (dynlink_trace_enabled()) {
                fprintf(stderr, "[dynlink] __libc_early_init returned\n");
            }
        }
    }
    // Re-patch _rtld_global_ro AFTER __libc_early_init.
    // __libc_early_init zeroes dl_pagesize. Re-apply.
    patch_rtld_global_ro_();
    // glibc's __libc_start_main_impl sets __libc_single_threaded = 1
    // BEFORE running DT_INIT_ARRAY (static constructors). This makes
    // glibc take the single-threaded fast paths: __cxa_guard_acquire
    // detects recursive static initialization and throws
    // std::__throw_recursive_init_error instead of taking the
    // multithreaded CAS+futex path, which deadlocks when only the main
    // thread exists (FUTEX_WAIT can never be woken). musl has no such
    // symbol — resolve_symbol returns 0 and the write is skipped.
    set_libc_single_threaded_();
    // loaded object (libs first, main last). Runs C++ static constructors,
    // glibc __libc_start_main hooks, etc. Without this, every C++ game
    // runs with uninitialized globals (vtables, std::mutex, std::string).
    // Requires init_runner_ to be set by the Emulator; no-ops if not.
    run_init_arrays_(cpu);
    if (dynlink_trace_enabled()) {
        fprintf(stderr, "[dynlink] all DT_INIT_ARRAY done, link() complete\n");
    }
    // Final re-patch AFTER DT_INIT_ARRAY.
    patch_rtld_global_ro_();
    return true;
}
// ── set_libc_single_threaded_ ─────────────────────────────────────────
// Mirror glibc's __libc_start_main_impl: write 1 into libc's
// __libc_single_threaded BSS word so single-threaded guests use glibc's
// fast paths (recursive static init → std::__throw_recursive_init_error
// instead of an unwakeable FUTEX_WAIT deadlock). No-op for musl.
void DynamicLinker::set_libc_single_threaded_() {
    libc_single_threaded_addr_ = resolve_symbol("__libc_single_threaded");
    if (libc_single_threaded_addr_ == 0) return;
    mem_.store<uint8_t>(libc_single_threaded_addr_, 1);
    if (dynlink_trace_enabled()) {
        fprintf(stderr, "[dynlink] __libc_single_threaded = 1 @ 0x%llx\n",
                static_cast<unsigned long long>(libc_single_threaded_addr_));
    }
}
// ── patch_rseq_offset_ ────────────────────────────────────────────────
// glibc's startup code uses TP + __rseq_offset even when rseq registration
// is disabled; it writes the cpu_id failure marker there. ld-linux normally
// places this 32-byte area after static TLS. The native linker bypasses that
// calculation, so publish the matching offset into our reserved TLS slack.
void DynamicLinker::patch_rseq_offset_() {
    if (is_musl_ || static_tls_base_ == 0 || rseq_offset_ == 0) return;
    const uint64_t addr = resolve_symbol("__rseq_offset");
    if (addr == 0) return;
    try {
        mem_.store<uint64_t>(addr, rseq_offset_);
        if (dynlink_trace_enabled()) {
            fprintf(stderr, "[dynlink] __rseq_offset = %llu @ 0x%llx\n",
                    static_cast<unsigned long long>(rseq_offset_),
                    static_cast<unsigned long long>(addr));
        }
    } catch (...) {
        if (dynlink_trace_enabled()) {
            fprintf(stderr, "[dynlink] could not patch __rseq_offset @ 0x%llx\n",
                    static_cast<unsigned long long>(addr));
        }
    }
}
// ── set_guest_environ ──────────────────────────────────────────────────
// Mirror what ld.so's _dl_start_user does on a real boot: set libc's
// __environ global to point at the envp array on the initial stack.
// Without this, glibc's dynamically-linked startup never initializes
// __environ (its __libc_start_main_impl SHARED path deliberately does
// not set it) so getenv() returns NULL for every variable. No-op for
// musl, which resolves no __environ symbol.
void DynamicLinker::set_guest_environ(uint64_t envp_addr) {
    uint64_t addr = resolve_symbol("__environ");
    if (addr == 0) return;
    mem_.store<uint64_t>(addr, envp_addr);
    if (dynlink_trace_enabled()) {
        fprintf(stderr, "[dynlink] __environ = 0x%llx @ 0x%llx\n",
                static_cast<unsigned long long>(envp_addr),
                static_cast<unsigned long long>(addr));
    }
}
void DynamicLinker::set_libc_stack_end(uint64_t initial_sp) {
    // An executable that references the variable gets a COPY of it, but
    // libc's versioned import may still bind ld.so's definition. Store to
    // every binding so both read the same value.
    std::vector<uint64_t> addrs;
    if (uint64_t addr = resolve_symbol("__libc_stack_end")) addrs.push_back(addr);
    for (const auto& [key, entry] : versioned_symbols_)
        if (key.rfind("__libc_stack_end@", 0) == 0 && entry.addr &&
            std::find(addrs.begin(), addrs.end(), entry.addr) == addrs.end())
            addrs.push_back(entry.addr);
    for (uint64_t addr : addrs) {
        mem_.store<uint64_t>(addr, initial_sp);
        if (dynlink_trace_enabled()) {
            fprintf(stderr, "[dynlink] __libc_stack_end = 0x%llx @ 0x%llx\n",
                    static_cast<unsigned long long>(initial_sp),
                    static_cast<unsigned long long>(addr));
        }
    }
}
// ── run_init_arrays_ ───────────────────────────────────────────────────
// Invoke DT_INIT (legacy _init()) and each entry in DT_INIT_ARRAY for
// every loaded object, in dependency order (libs first, main last).
// The init_runner_ callback runs a guest function at `addr` and returns
// when it RETs. If no callback is registered, this is a no-op (the guest
// will run with uninitialized statics — visible as crashes/vtables-full-
// of-zero, not silent corruption).
void DynamicLinker::run_init_arrays_(CPU& cpu) {
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
                init_runner_(cpu, obj.init_addr);
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
                    init_runner_(cpu, fn);
                } catch (...) {
                    // If one constructor throws, continue with the rest.
                    // (Real ld.so aborts, but for an emulator it's better
                    // to be lenient and let the user see the failure.)
                }
            }
        }
    }
}
}  // namespace arm64emu
