// Global symbol index and (versioned) symbol lookup.
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
void DynamicLinker::index_symbols(CPU& cpu, const LoadedObject& obj,
                                  std::vector<SymbolUndo>* undo) {
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
                uint64_t resolved = ifunc_resolver_(cpu, addr);
                if (resolved != 0) addr = resolved;
                // else: fall back to resolver address (visible failure)
            }
            // else: no resolver registered; store resolver address.
            // The guest will crash on first call (visible, not silent).
        }
        // First-strong-wins symbol resolution (H6).
        auto it = symbols_.find(name);
        if (it == symbols_.end()) {
            const SymEntry installed{addr, bind};
            if (undo)
                undo->push_back({false, name, false, {}, installed});
            symbols_[name] = installed;
        } else {
            // Existing entry. Override only if existing is WEAK and new
            // is STRONG. Never override an existing STRONG.
            if (it->second.bind == STB_WEAK_ && bind == STB_GLOBAL_) {
                const SymEntry previous = it->second;
                const SymEntry installed{addr, bind};
                if (undo)
                    undo->push_back({false, name, true, previous, installed});
                it->second = installed;
            }
            // else: keep existing (first strong wins, or weak kept as-is).
        }
    }
}
// ── resolve_symbol ─────────────────────────────────────────────────────
uint64_t DynamicLinker::resolve_symbol(const std::string& name) const {
    std::lock_guard<std::recursive_mutex> lk(loader_mu_);
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
}  // namespace arm64emu
