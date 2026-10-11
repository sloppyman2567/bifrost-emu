// dlopen/dlclose/dlsym/dladdr/dlerror/dl_iterate_phdr support.
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
// ── load_library (dlopen support) ─────────────────────────────────────
// Load a shared library at runtime by path. Reuses the same loading
// logic as load_shared_library but takes a full path instead of a
// soname. Returns the base address (handle) on success, 0 on failure.
//
// If the library is already loaded (by path or soname), returns the
// existing handle and bumps the refcount (matching glibc's _dl_open
// fast path). This prevents loading the same .so twice and ensures
// dlopen("libm.so.6") returns the same handle as dlopen("/lib/libm.so.6").
uint64_t DynamicLinker::load_library(CPU& cpu, const std::string& path,
                                     const std::string& parent_runpath,
                                     const std::string& parent_rpath) {
    std::lock_guard<std::recursive_mutex> lk(loader_mu_);
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
        auto data = find_library(path, found_path, parent_runpath, parent_rpath);
        constexpr uint16_t EM_AARCH64 = 183;
        bool aarch64_elf = data.size() >= 20
            && data[0] == 0x7f && data[1] == 'E' && data[2] == 'L' && data[3] == 'F'
            && data[18] == (EM_AARCH64 & 0xFF) && data[19] == (EM_AARCH64 >> 8);
        if (aarch64_elf) {
            resolved = found_path;
            std::vector<uint8_t> file_data = std::move(data);
            return load_library_from_data(cpu, path, file_data);
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
        auto data = find_library(basename, found_path, parent_runpath, parent_rpath);
        constexpr uint16_t EM_AARCH64 = 183;
        bool aarch64_elf = data.size() >= 20
            && data[0] == 0x7f && data[1] == 'E' && data[2] == 'L' && data[3] == 'F'
            && data[18] == (EM_AARCH64 & 0xFF) && data[19] == (EM_AARCH64 >> 8);
        if (aarch64_elf) {
            return load_library_from_data(cpu, path, data);
        }
        // Same thunk fallback as the bare-soname path: an absolute
        // host-arch path (e.g. /usr/lib/x86_64/libGL.so.1) should
        // thunk when the soname names a supported api.
        if (thunk_resolver_ && is_thunk_supported_lib_(basename)) {
            return register_thunk_library_(basename);
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
        auto alt = find_library(basename, found_path, parent_runpath, parent_rpath);
        bool alt_ok = alt.size() >= 20
            && alt[0] == 0x7f && alt[1] == 'E' && alt[2] == 'L' && alt[3] == 'F'
            && alt[18] == (EM_AARCH64 & 0xFF) && alt[19] == (EM_AARCH64 >> 8);
        if (alt_ok) {
            return load_library_from_data(cpu, path, alt);
        }
        // Same thunk fallback: host-arch file at an absolute path
        // thunks when the basename names a supported api.
        if (thunk_resolver_ && is_thunk_supported_lib_(basename)) {
            return register_thunk_library_(basename);
        }
    }
    return load_library_from_data(cpu, path, data);
}
// Internal helper: load a library from an in-memory ELF image.
// Used by load_library (dlopen by path) and by soname-based lookup.
// Handles ELF validation, segment mapping, relocation, symbol indexing,
// init array execution, and refcount tracking.
uint64_t DynamicLinker::load_library_from_data(CPU& cpu,
                                                const std::string& path,
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
    // Static-ELF processes never ran link() so the ld-linux shim
    // (dlerror buffer, TLSDESC resolver, dlopen hooks) doesn't exist yet.
    // Register it lazily before relocations need it. No-op on the dynamic
    // path (already registered, early-returns).
    ensure_shim();
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
    parse_eh_frame(data, base, obj);
    // A static executable can dlopen before any native linker setup has
    // allocated a TLS block. Establish an empty variant-I anchor now so
    // the first late TLS module gets the same DTV/surplus machinery.
    if (obj.tls.present && obj.tls.memsz > 0) {
        if (is_musl_) {
            set_last_error("late TLS allocation is unsupported for musl layout");
            error_ = "late TLS allocation is unsupported for musl layout";
            mem_.untrack_allocation(base, max_end);
            return 0;
        }
        if (static_tls_base_ == 0) allocate_static_tls();
        if (static_tls_base_ == 0) {
            set_last_error(error_.empty() ? "could not initialize TLS layout" : error_);
            mem_.untrack_allocation(base, max_end);
            return 0;
        }
        if (cpu.tpidr_el0 == 0) {
            cpu.tpidr_el0 = thread_pointer();
            cpu.tpidrro_el0 = cpu.tpidr_el0;
        }
        if (cpu.tpidr_el0 != 0)
            register_thread_tls_block(cpu.tpidr_el0);

        obj.tls_mod_id = next_tls_mod_id_++;
        if (obj.tls_mod_id == 0) {
            set_last_error("TLS module ID space exhausted");
            error_ = "TLS module ID space exhausted";
            mem_.untrack_allocation(base, max_end);
            return 0;
        }
        obj.tls_dynamic = true;
        uint64_t alignment = obj.tls.align ? obj.tls.align : 1;
        // The template is aligned, but glibc supplies an aligned TP rather
        // than our template base for pthreads. Both placements agree only
        // when the frozen base-to-TP distance preserves this alignment.
        bool static_slot = alignment <= tls_static_alignment_ &&
                           lib_tls_size_ % alignment == 0 &&
                           tls_surplus_base_ <= UINT64_MAX - tls_surplus_used_;
        uint64_t slot = 0;
        uint64_t need = UINT64_MAX;
        if (static_slot) {
            const uint64_t cursor = tls_surplus_base_ + tls_surplus_used_;
            if (cursor > UINT64_MAX - (alignment - 1)) {
                static_slot = false;
            } else {
                slot = (cursor + alignment - 1) & ~(alignment - 1);
                if (slot < tls_surplus_base_ ||
                    obj.tls.memsz > UINT64_MAX - (slot - tls_surplus_base_)) {
                    static_slot = false;
                } else {
                    need = (slot - tls_surplus_base_) + obj.tls.memsz;
                    static_slot = need <= TLS_DLOPEN_SURPLUS;
                }
            }
        }
        if (static_slot) {
            obj.tls_static_slot = true;
            obj.tls_block_offset = slot;
            obj.tls_template_addr = static_tls_base_ + slot;
            obj.tls_tp_offset = static_cast<int64_t>(slot) -
                                static_cast<int64_t>(lib_tls_size_);
            try {
                std::vector<uint8_t> image(static_cast<size_t>(obj.tls.memsz), 0);
                if (obj.tls.filesz > 0)
                    mem_.read(base + obj.tls.vaddr, image.data(),
                              static_cast<size_t>(obj.tls.filesz));
                mem_.write(obj.tls_template_addr, image.data(), image.size());
            } catch (...) {
                set_last_error("could not initialize static TLS template for " + path);
                error_ = "could not initialize static TLS template for " + path;
                mem_.untrack_allocation(base, max_end);
                return 0;
            }
            tls_surplus_used_ = need;
        } else {
            // Keep the mapped PT_TLS image as the immutable template. Each
            // thread receives an aligned, zero-filled copy on first access.
            obj.tls_static_slot = false;
            obj.tls_block_offset = 0;
            obj.tls_tp_offset = 0;
            obj.tls_template_addr = base + obj.tls.vaddr;
        }
        ++tls_generation_;
        if (tls_generation_ == 0) tls_generation_ = 1;
    }
    const size_t objects_before_load = objects_.size();
    objects_.push_back(std::move(obj));
    size_t parent_idx = objects_.size() - 1;
    std::vector<SymbolUndo> symbol_undo;
    index_symbols(cpu, objects_[parent_idx], &symbol_undo);
    parse_versions_(cpu, objects_[parent_idx], &symbol_undo);
    // Load DT_NEEDED dependencies of the dlopen'd library BEFORE applying
    // its relocations. The startup path does this; the dlopen path skipped
    // it, so JUMP_SLOT/GLOB_DAT slots referencing symbols in deps (e.g.
    // FcPatternCreate from libfontconfig.so.1) stayed at their raw PLT
    // vaddr in GOT.plt — a blr to that jumps into unmapped guest memory.
    // Deps are appended AFTER this library, so keep using parent_idx.
    if (objects_[parent_idx].dyn_addr != 0) {
        try {
            Elf64_Dyn dyn;
            for (uint64_t p = objects_[parent_idx].dyn_addr; ; p += sizeof(dyn)) {
                mem_.read(p, &dyn, sizeof(dyn));
                if (dyn.d_tag == DT_NULL_) break;
                if (dyn.d_tag == DT_NEEDED_) {
                    uint64_t str_addr = objects_[parent_idx].strtab_addr + dyn.d_val;
                    std::string soname = read_guest_cstr(mem_, str_addr);
                    if (soname.empty()) continue;
                    bool found = false;
                    for (const auto& o : objects_) {
                        if (o.name == soname) { found = true; break; }
                        if (!o.soname.empty() && o.soname == soname) { found = true; break; }
                    }
                    if (found) continue;
                    uint64_t dep_base = load_library(cpu, soname,
                                                     objects_[parent_idx].runpath,
                                                     objects_[parent_idx].rpath);
                    if (dep_base == 0) {
                        fprintf(stderr, "[%s] dlopen: could not load dependency "
                                "%s (continuing)\n", CODENAME, soname.c_str());
                    }
                }
            }
        } catch (...) {
            // Reading the dynamic section failed — skip dep loading.
        }
    }
    // Apply relocations: RELA, JMPREL, RELR
    auto& nobj = objects_[parent_idx];
    auto rollback_failed_tls_load = [&]() {
        const LoadedObject failed = objects_[parent_idx];
        std::vector<LoadedObject> newly_loaded_dependencies;
        newly_loaded_dependencies.reserve(objects_.size() - objects_before_load - 1);
        for (size_t i = objects_before_load; i < objects_.size(); ++i) {
            if (i != parent_idx) newly_loaded_dependencies.push_back(objects_[i]);
        }

        // A late TLS allocation may have reached some existing threads before
        // another thread failed. Remove this module's DTV entries and only its
        // per-thread allocations; other modules' TLS remains intact.
        if (failed.tls_mod_id != 0) {
            for (uint64_t tcb : tls_thread_blocks_) {
                if (is_musl_) break;
                try {
                    const uint64_t dtv = mem_.load<uint64_t>(tcb);
                    if (dtv < 2 * sizeof(uint64_t) ||
                        failed.tls_mod_id > (UINT64_MAX - dtv) / 16) continue;
                    const uint64_t capacity = mem_.load<uint64_t>(dtv - 16);
                    if (failed.tls_mod_id > capacity) continue;
                    const uint64_t entry = dtv + failed.tls_mod_id * 16;
                    const uint64_t tls_addr = mem_.load<uint64_t>(entry);
                    if (tls_addr != 0 && failed.tls_static_slot &&
                        failed.tls.memsz != 0 &&
                        mem_.is_mapped(tls_addr, failed.tls.memsz)) {
                        std::vector<uint8_t> zeros(
                            static_cast<size_t>(failed.tls.memsz), 0);
                        mem_.write(tls_addr, zeros.data(), zeros.size());
                    }
                    mem_.store<uint64_t>(entry, 0);
                    mem_.store<uint64_t>(entry + 8, 0);
                } catch (...) {}

                auto allocations = tls_thread_allocations_.find(tcb);
                if (allocations == tls_thread_allocations_.end()) continue;
                auto& owned = allocations->second;
                for (auto it = owned.begin(); it != owned.end();) {
                    if (it->module_id != failed.tls_mod_id) {
                        ++it;
                        continue;
                    }
                    mem_.untrack_allocation(it->addr, it->size);
                    it = owned.erase(it);
                }
                if (owned.empty()) tls_thread_allocations_.erase(allocations);
            }
            if (failed.tls_static_slot && failed.tls_template_addr != 0 &&
                failed.tls.memsz != 0 &&
                failed.tls.memsz <= static_cast<uint64_t>(SIZE_MAX)) {
                try {
                    std::vector<uint8_t> zeros(
                        static_cast<size_t>(failed.tls.memsz), 0);
                    mem_.write(failed.tls_template_addr, zeros.data(), zeros.size());
                } catch (...) {}
            }
        }

        const uint64_t map_end = failed.base_addr <= UINT64_MAX - failed.map_size
            ? failed.base_addr + failed.map_size : UINT64_MAX;
        for (auto it = tlsdesc_indices_.begin(); it != tlsdesc_indices_.end();) {
            if (it->first >= failed.base_addr && it->first < map_end)
                it = tlsdesc_indices_.erase(it);
            else
                ++it;
        }

        // Undo this object's symbol exports, restoring any weak definition it
        // replaced. Leave a later dependency's replacement alone.
        for (auto it = symbol_undo.rbegin(); it != symbol_undo.rend(); ++it) {
            auto& table = it->versioned ? versioned_symbols_ : symbols_;
            auto entry = table.find(it->key);
            if (entry == table.end() ||
                entry->second.addr != it->installed.addr ||
                entry->second.bind != it->installed.bind) continue;
            if (it->had_previous)
                entry->second = it->previous;
            else
                table.erase(entry);
        }

        objects_.erase(objects_.begin() + static_cast<std::ptrdiff_t>(parent_idx));
        mem_.untrack_allocation(failed.base_addr, failed.map_size);

        // Dependencies are still mapped and usable. Reindex them after
        // removing the failed parent's symbols so a strong dependency export
        // that was previously shadowed by the parent becomes visible.
        for (const auto& dependency : newly_loaded_dependencies) {
            index_symbols(cpu, dependency);
            parse_versions_(cpu, dependency);
        }
    };
    std::string tls_relocation_failure;
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
                for (uint64_t off = 0; off+24 <= rs &&
                     tls_relocation_failure.empty(); off += 24) {
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
                            resolved = ifunc_resolver_(cpu, resolver_addr);
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
                        TlsIndex index{};
                        if (!resolve_tlsdesc_index_(nobj, sym, A, index)) {
                            set_last_error("could not resolve initial-exec TLS in " + path);
                            error_ = "could not resolve initial-exec TLS in " + path;
                            tls_relocation_failure = error_;
                            continue;
                        }
                        const LoadedObject* defining = nullptr;
                        for (const auto& candidate : objects_) {
                            if (candidate.tls_mod_id == index.module_id) {
                                defining = &candidate;
                                break;
                            }
                        }
                        if (!defining || (defining->tls_dynamic &&
                                          !defining->tls_static_slot)) {
                            set_last_error("late TLS module requires a static TLS slot: " +
                                           path);
                            error_ = "late TLS module requires a static TLS slot: " + path;
                            tls_relocation_failure = error_;
                            continue;
                        }
                        if (index.offset > static_cast<uint64_t>(INT64_MAX) ||
                            defining->tls_tp_offset >
                                INT64_MAX - static_cast<int64_t>(index.offset)) {
                            set_last_error("initial-exec TLS offset overflow in " + path);
                            error_ = "initial-exec TLS offset overflow in " + path;
                            tls_relocation_failure = error_;
                            continue;
                        }
                        const int64_t tp_off = defining->tls_tp_offset +
                                               static_cast<int64_t>(index.offset);
                        mem_.store<uint64_t>(target, static_cast<uint64_t>(tp_off));
                    } else if (type == R_AARCH64_TLS_DTPMOD_) {
                        TlsIndex index{};
                        if (!resolve_tlsdesc_index_(nobj, sym, 0, index)) {
                            set_last_error("could not resolve TLS module in " + path);
                            error_ = "could not resolve TLS module in " + path;
                            tls_relocation_failure = error_;
                            continue;
                        }
                        mem_.store<uint64_t>(target, index.module_id + A);
                    } else if (type == R_AARCH64_TLS_DTPREL_) {
                        TlsIndex index{};
                        if (!resolve_tlsdesc_index_(nobj, sym, A, index)) {
                            set_last_error("could not resolve TLS offset in " + path);
                            error_ = "could not resolve TLS offset in " + path;
                            tls_relocation_failure = error_;
                            continue;
                        }
                        mem_.store<uint64_t>(target, index.offset);
                    } else if (type == R_AARCH64_TLSDESC_) {
                        if (!install_tlsdesc_(nobj, sym, A, target))
                            tls_relocation_failure = error_;
                    }
                }
            }
            if (ja && js && tls_relocation_failure.empty()) {
                for (uint64_t off = 0; off+24 <= js &&
                     tls_relocation_failure.empty(); off += 24) {
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
                        if (addr) mem_.store<uint64_t>(target, addr + A);
                    } else if (type == R_AARCH64_TLSDESC_) {
                        // Mirror the startup JMPREL path: Qt places TLSDESC
                        // at the end of .rela.plt.
                        if (!install_tlsdesc_(nobj, sym, A, target))
                            tls_relocation_failure = error_;
                    } else if (type == R_AARCH64_IRELATIVE_) {
                        // ifunc in PLT
                        uint64_t resolver_addr = nobj.base_addr + A;
                        uint64_t resolved = ifunc_resolver_ ? ifunc_resolver_(cpu, resolver_addr) : 0;
                        if (resolved == 0) resolved = resolver_addr;
                        mem_.store<uint64_t>(target, resolved);
                    }
                }
            }
            if (rra && rrs && tls_relocation_failure.empty()) {
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
    if (!tls_relocation_failure.empty()) {
        rollback_failed_tls_load();
        return 0;
    }
    if (nobj.tls.present && nobj.tls.memsz > 0 &&
        nobj.tls_dynamic && nobj.tls_static_slot) {
        // The late RELA/JMPREL/RELR paths relocate the mapped .tdata image.
        // Refresh the surplus template before any thread copies it; the
        // pre-relocation copy may contain stale pointer initializers.
        try {
            if (nobj.tls.filesz != 0) {
                std::vector<uint8_t> image(static_cast<size_t>(nobj.tls.filesz));
                mem_.read(nobj.base_addr + nobj.tls.vaddr,
                          image.data(), image.size());
                mem_.write(nobj.tls_template_addr, image.data(), image.size());
            }
        } catch (...) {
            set_last_error("could not refresh relocated TLS template for " + path);
            error_ = "could not refresh relocated TLS template for " + path;
            rollback_failed_tls_load();
            return 0;
        }
        // Materialize it for every thread that already has a TCB so a
        // constructor or the dlopen caller sees initialized TLS immediately.
        for (uint64_t tcb : tls_thread_blocks_) {
            CPU tls_cpu{};
            tls_cpu.tpidr_el0 = tcb;
            if (resolve_tls_address(mem_, tls_cpu, nobj.tls_mod_id, 0) == 0) {
                set_last_error("could not initialize late TLS for " + path);
                error_ = "could not initialize late TLS for " + path;
                rollback_failed_tls_load();
                return 0;
            }
        }
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
                guest_call_args_(cpu, nobj.init_addr, 0, 0, 0);
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
                    guest_call_args_(cpu, fn, 0, 0, 0);
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
int DynamicLinker::close_library(CPU& cpu, uint64_t handle) {
    std::lock_guard<std::recursive_mutex> lk(loader_mu_);
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
                            guest_call_args_(cpu, fn, 0, 0, 0);
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
                        guest_call_args_(cpu, obj.fini_addr, 0, 0, 0);
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
    std::lock_guard<std::recursive_mutex> lk(loader_mu_);
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
    std::lock_guard<std::recursive_mutex> lk(loader_mu_);
    for (const auto& obj : objects_) {
        if (obj.map_size == 0) continue;  // thunk libs / unknown: no range
        if (addr >= obj.base_addr && addr < obj.base_addr + obj.map_size) {
            return &obj;
        }
    }
    // Fallback for main objects without a known size (old records):
    // check a reasonable range for text/data segments.
    for (const auto& obj : objects_) {
        if (obj.is_main && obj.map_size == 0 && addr < 0x10000000) {
            return &obj;
        }
    }
    return nullptr;
}
// ── object_relative_offset ─────────────────────────────────────────────
// Returns addr's file offset relative to the containing object's base if
// that object's name contains `name_fragment`, else ~0.
uint64_t DynamicLinker::object_relative_offset(uint64_t addr, const std::string& name_fragment) const {
    std::lock_guard<std::recursive_mutex> lk(loader_mu_);
    for (const auto& obj : objects_) {
        if (obj.map_size == 0) continue;
        if (addr >= obj.base_addr && addr < obj.base_addr + obj.map_size) {
            if (obj.name.find(name_fragment) != std::string::npos) {
                return addr - obj.base_addr;
            }
            return ~uint64_t(0);
        }
    }
    return ~uint64_t(0);
}
// ── dladdr ─────────────────────────────────────────────────────────────
// Fill in Dl_info for a given address. Returns 1 if the address falls
// within a loaded object, 0 otherwise. Finds the nearest symbol by
// scanning the containing object's .dynsym for the symbol with the
// largest st_value that is <= addr.
int DynamicLinker::dladdr(uint64_t addr, DlInfo& info) {
    std::lock_guard<std::recursive_mutex> lk(loader_mu_);
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
    std::lock_guard<std::recursive_mutex> lk(loader_mu_);
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
    std::lock_guard<std::recursive_mutex> lk(loader_mu_);
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
int DynamicLinker::iterate_phdr(CPU& cpu, uint64_t callback_ptr, uint64_t data_ptr) {
    if (callback_ptr == 0 || guest_call_args_ == nullptr) return 0;
    std::lock_guard<std::recursive_mutex> lk(loader_mu_);
    // Allocate a scratch buffer for one dl_phdr_info struct (64 bytes)
    // plus the name string (256 bytes). We reuse the dlerror buffer area
    // since dlerror and dl_iterate_phdr don't run concurrently.
    uint64_t info_buf = dlerror_buf_ptr_ != 0 ? dlerror_buf_ptr_ : 0;
    if (info_buf == 0) return 0;  // shim not set up
    // Snapshot first: the guest callback below can dlopen/dlclose, which
    // mutates objects_ (vector reallocation). Holding a reference across
    // the call is use-after-free (the dlopen_mt crash: free() inside
    // iterate_phdr). A stale snapshot entry is harmless by comparison.
    struct ObjSnap {
        uint64_t base;
        std::string name;
        uint64_t tls_mod_id;
    };
    std::vector<ObjSnap> snap;
    snap.reserve(objects_.size());
    for (const auto& obj : objects_) {
        if (obj.base_addr == 0 && !obj.is_main) continue;
        // Skip the synthetic ld-linux shim — it's not a real object.
        if (obj.name == "<ld-linux-shim>") continue;
        snap.push_back({obj.base_addr,
                        obj.name.empty() ? "<main>" : obj.name,
                        obj.tls_mod_id});
    }
    int total = 0;
    for (const auto& s : snap) {
        // Write the dl_phdr_info struct to guest memory.
        uint64_t name_ptr = info_buf + 64;  // name goes after the struct
        // Write the name string.
        const std::string& name = s.name;
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
            mem_.store<uint64_t>(info_buf + 0,  s.base);             // dlpi_addr
            mem_.store<uint64_t>(info_buf + 8,  name_ptr);             // dlpi_name
            mem_.store<uint64_t>(info_buf + 16, 0);                    // dlpi_phdr
            mem_.store<uint16_t>(info_buf + 24, 0);                    // dlpi_phnum
            mem_.store<uint64_t>(info_buf + 32, snap.size());          // dlpi_adds
            mem_.store<uint64_t>(info_buf + 40, 0);                    // dlpi_subs
            mem_.store<uint64_t>(info_buf + 48, s.tls_mod_id);         // dlpi_tls_modid
            mem_.store<uint64_t>(info_buf + 56, 0);                    // dlpi_tls_data
        } catch (...) { continue; }
        // Call the guest callback: x0 = info, x1 = sizeof(dl_phdr_info), x2 = data
        // sizeof(struct dl_phdr_info) on AArch64 LP64 = 64 bytes.
        uint64_t rc = guest_call_args_(cpu, callback_ptr, info_buf, 64, data_ptr);
        if (dynlink_trace_enabled()) {
            fprintf(stderr, "[dl_iterate_phdr] callback for '%s' returned %lld\n",
                    s.name.c_str(), static_cast<long long>(rc));
        }
        total += static_cast<int>(rc);
        if (rc != 0) break;  // callback returns non-zero to stop iteration
    }
    return total;
}
}  // namespace arm64emu
