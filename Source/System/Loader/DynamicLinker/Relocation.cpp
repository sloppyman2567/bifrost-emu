// Relocation passes: deferred COPY, TLS mirrors, RELR and symbol resolution for relocs.
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
    if (obj.tls_template_addr == 0) return;
    uint64_t tdata_start = obj.base_addr + obj.tls.vaddr;
    uint64_t tdata_end = tdata_start + obj.tls.filesz;
    if (target >= tdata_start && target < tdata_end) {
        uint64_t tls_dst = obj.tls_template_addr + (target - tdata_start);
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
}  // namespace arm64emu
