// ELF parsing: PT_DYNAMIC, .eh_frame registration and symbol versioning.
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
    collect_thread_db_symbols_(data, base, obj);
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
    uint64_t hash_vaddr = 0, gnu_hash_vaddr = 0;
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
            case DT_GNU_HASH_:      gnu_hash_vaddr = dyn.d_val; break;
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
    } else if (gnu_hash_vaddr != 0) {
        // DT_GNU_HASH only (glibc ≥2.35 defaults; Qt libs are GNU_HASH-
        // only). DT_HASH nchain is the exact .dynsym count, but without
        // DT_HASH we must derive it from GNU_HASH. Header:
        //   +0  uint32 nbucket
        //   +4  uint32 symoffset
        //   +8  uint32 bloom_size
        //   +12 uint32 bloom_shift
        //   then bloom_size uint64 bloom words,
        //   then nbucket uint32 buckets (each an index into .dynsym),
        //   then a chain word per symbol from symoffset upward.
        // The .dynsym table extends past the last bucket's chain run, so
        // the symbol count = 1 + max chain index reached across all
        // buckets (GNU_HASH guarantees chains are contiguous from
        // symoffset). Cap it at MAX_SYMS*4 for safety like DT_HASH.
        try {
            uint32_t nbucket = 0, symoffset = 0, bloom_size = 0;
            mem_.read(base + gnu_hash_vaddr + 0, &nbucket, 4);
            mem_.read(base + gnu_hash_vaddr + 4, &symoffset, 4);
            mem_.read(base + gnu_hash_vaddr + 8, &bloom_size, 4);
            if (nbucket != 0 && nbucket <= 0x100000 && bloom_size <= 0x100000) {
                // bucket table starts right after the bloom words.
                uint64_t bucket_off = gnu_hash_vaddr + 16 + (uint64_t)bloom_size * 8;
                uint64_t max_sym = symoffset;  // symbols before symoffset exist too
                for (uint32_t b = 0; b < nbucket; b++) {
                    uint32_t first = 0;
                    mem_.read(base + bucket_off + b * 4, &first, 4);
                    if (first == 0 || first < symoffset) continue;
                    // Walk the chain from `first` until the low bit of the
                    // hash word is set (last entry in the chain).
                    uint64_t chain_off = bucket_off + (uint64_t)nbucket * 4 +
                                         (uint64_t)(first - symoffset) * 4;
                    for (;;) {
                        uint32_t h = 0;
                        mem_.read(base + chain_off, &h, 4);
                        uint64_t sym_idx = symoffset + (chain_off -
                            (bucket_off + (uint64_t)nbucket * 4)) / 4;
                        if (sym_idx > max_sym) max_sym = sym_idx;
                        if (h & 1) break;  // LSB set → last in chain
                        chain_off += 4;
                        if (max_sym > 0x1000000) break;  // safety
                    }
                }
                obj.symtab_count = max_sym + 1;
            }
        } catch (...) {
            obj.symtab_count = 0;
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
// ── parse_eh_frame ──────────────────────────────────────────────────────
// Locate PT_GNU_EH_FRAME (p_type 0x6474e550) and record the absolute
// guest address of the .eh_frame_hdr. libgcc's _Unwind_Find_FDE reads
// this from the dl_find_object result struct (see syscall 0x1008 in
// Miscellaneous.cpp) to binary-search FDEs during C++ exception unwinding.
void DynamicLinker::parse_eh_frame(const std::vector<uint8_t>& data,
                                   uint64_t base, LoadedObject& obj) {
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
        uint64_t p_vaddr;
        memcpy(&p_type,  p + 0,  4);
        if (p_type != 0x6474e550u) continue;  // PT_GNU_EH_FRAME
        memcpy(&p_vaddr, p + 16, 8);
        obj.eh_frame_hdr_addr = base + p_vaddr;
        return;
    }
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
void DynamicLinker::parse_versions_(CPU& cpu, const LoadedObject& obj,
                                    std::vector<SymbolUndo>* undo) {
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
            uint64_t resolved = ifunc_resolver_(cpu, addr);
            if (resolved != 0) addr = resolved;
        }
        std::string key = name + "@" + vit->second;
        // First-strong-wins (same as index_symbols).
        auto it = versioned_symbols_.find(key);
        if (it == versioned_symbols_.end()) {
            const SymEntry installed{addr, bind};
            if (undo)
                undo->push_back({true, key, false, {}, installed});
            versioned_symbols_[key] = installed;
        } else {
            if (it->second.bind == STB_WEAK_ && bind == STB_GLOBAL_) {
                const SymEntry previous = it->second;
                const SymEntry installed{addr, bind};
                if (undo)
                    undo->push_back({true, key, true, previous, installed});
                it->second = installed;
            }
        }
    }
}
}  // namespace arm64emu
