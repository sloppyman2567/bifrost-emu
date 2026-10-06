// frontend/elf_loader.cpp — ELF64 LE AArch64 static loader.
//
// Implements ElfLoader::load(). Reads an ELF64 little-endian AArch64
// file from memory, maps its PT_LOAD segments into the guest address
// space, processes RELA relocations, and parses PT_NOTE segments to
// detect the GNU AArch64 LSE atomics feature (used to disambiguate
// the LDUR/LSE encoding group at decode time).
#include "core/emulator.h"  // ElfLoader declaration
#include "core/memory.h"
#include "bifrost/types.hpp"
#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>
namespace arm64emu {
ElfLoader::Loaded ElfLoader::load(Memory& mem, const std::vector<uint8_t>& data) {
    return load(mem, data, Options{});
}
ElfLoader::Loaded ElfLoader::load(Memory& mem, const std::vector<uint8_t>& data,
                                  const Options& options) {
    if (data.size() < 64) throw EmuError("file too small to be ELF");
    if (data[0] != 0x7f || data[1] != 'E' || data[2] != 'L' || data[3] != 'F')
        throw EmuError("not an ELF file");
    if (data[4] != 2) throw EmuError("not ELF64");
    if (data[5] != 1) throw EmuError("not little-endian");
    uint16_t e_type, e_machine;
    uint32_t e_version, e_flags;
    uint64_t e_entry, e_phoff, e_shoff;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
    const uint8_t* p = data.data();
    memcpy(&e_type, p + 16, 2);
    memcpy(&e_machine, p + 18, 2);
    memcpy(&e_version, p + 20, 4);
    memcpy(&e_entry, p + 24, 8);
    memcpy(&e_phoff, p + 32, 8);
    memcpy(&e_shoff, p + 40, 8);
    memcpy(&e_flags, p + 48, 4);
    memcpy(&e_ehsize, p + 52, 2);
    memcpy(&e_phentsize, p + 54, 2);
    memcpy(&e_phnum, p + 56, 2);
    memcpy(&e_shentsize, p + 58, 2);
    memcpy(&e_shnum, p + 60, 2);
    memcpy(&e_shstrndx, p + 62, 2);
    if (e_machine != 183) throw EmuError("not AArch64 ELF");
    if (e_type != 2 && e_type != 3) throw EmuError("ELF: expected ET_EXEC or ET_DYN");
    if (e_phoff == 0 || e_phnum == 0) throw EmuError("no program headers");
    if (e_phoff >= data.size() || e_phentsize < 56)
        throw EmuError("ELF: bad program-header table layout");
    if (e_phnum > (data.size() - e_phoff) / e_phentsize)
        throw EmuError("ELF: program-header table exceeds file size");
    struct Phdr {
        uint32_t p_type;
        uint32_t p_flags;
        uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
    };
    std::vector<Phdr> phdrs;
    for (int i = 0; i < e_phnum; i++) {
        const uint8_t* pp = data.data() + e_phoff + i * e_phentsize;
        Phdr h;
        memcpy(&h.p_type,  pp + 0,  4);
        memcpy(&h.p_flags, pp + 4,  4);
        memcpy(&h.p_offset,pp + 8,  8);
        memcpy(&h.p_vaddr, pp + 16, 8);
        memcpy(&h.p_paddr, pp + 24, 8);
        memcpy(&h.p_filesz,pp + 32, 8);
        memcpy(&h.p_memsz, pp + 40, 8);
        memcpy(&h.p_align, pp + 48, 8);
        phdrs.push_back(h);
    }
    // Validate the complete load plan before mapping or writing any segment.
    // ELF alignment can exceed the runtime's page size (e.g. Android 16 KiB
    // alignment on a 4 KiB system); the load bias must preserve it.
    uint64_t alignment = Memory::PAGE_SIZE;
    uint64_t image_end = 0;
    bool has_load = false;
    for (const auto& h : phdrs) {
        if (h.p_type != 1) continue;
        has_load = true;
        if (h.p_filesz > h.p_memsz || h.p_offset > data.size() ||
            h.p_filesz > data.size() - h.p_offset)
            throw EmuError("ELF: invalid PT_LOAD file range");
        if (h.p_vaddr > UINT64_MAX - h.p_memsz ||
            h.p_vaddr + h.p_memsz > UINT64_MAX - Memory::PAGE_MASK)
            throw EmuError("ELF: PT_LOAD address overflow");
        if (h.p_align > 1) {
            if ((h.p_align & (h.p_align - 1)) ||
                (h.p_vaddr & (h.p_align - 1)) != (h.p_offset & (h.p_align - 1)))
                throw EmuError("ELF: invalid PT_LOAD alignment");
            alignment = std::max(alignment, h.p_align);
        }
        image_end = std::max(image_end, h.p_vaddr + h.p_memsz);
    }
    if (!has_load) throw EmuError("ELF: no loadable segments");
    if (!options.apply_relocations) {
        bool entry_valid = false;
        for (const auto& h : phdrs)
            if (h.p_type == 1 && (h.p_flags & 1) && h.p_memsz >= 4 &&
                e_entry >= h.p_vaddr && e_entry - h.p_vaddr <= h.p_memsz - 4)
                entry_valid = true;
        if (!entry_valid || (e_entry & 3))
            throw EmuError("ELF: entry point is not aligned executable code");
    }
    Loaded info{};
    info.entry = e_entry;
    info.phent = e_phentsize;
    info.phnum = e_phnum;
    info.phdr_addr = 0;
    info.start_addr = 0;
    info.end_addr  = 0;
    info.has_lse   = false;
    info.base_addr = 0;  // load bias (0 for ET_EXEC, non-zero for ET_DYN/PIE)
    // ET_DYN images are position-relative (start at 0). The Linux kernel
    // loads PIE at a random base address (ELF_ET_DYN_BASE + page-aligned
    // jitter); we mirror that with Memory::pie_base() — 0x400000 plus a
    // per-process offset, always well above the zero page we map for
    // NULL-deref safety and below the mmap heap floor. Without a bias,
    // PIE binaries overlap the zero page — the first PT_LOAD (vaddr=0)
    // overwrites it, and NULL dereferences crash instead of returning 0.
    // This affected musl dynamic binaries (which are PIE by default),
    // causing host SIGSEGV during dynamic linker initialization.
    if (e_type == 3) {  // ET_DYN (PIE or shared library)
        uint64_t base = mem.pie_base();
        if (options.reserve_image) {
            const uint64_t span = (image_end + Memory::PAGE_MASK) & ~Memory::PAGE_MASK;
            if (alignment > Memory::MAX_MMAP_LENGTH ||
                span > Memory::MAX_MMAP_LENGTH - alignment)
                throw EmuError("ELF: interpreter reservation too large");
            base = mem.mmap_alloc(span + alignment, 0, false, 0);
            if (!base) throw EmuError("ELF: cannot reserve interpreter image");
            const uint64_t aligned_base = (base + alignment - 1) & ~(alignment - 1);
            // Return alignment padding to the allocator, retaining the entire
            // image span so later guest mmap calls cannot overlap the linker.
            if (aligned_base > base) mem.untrack_allocation(base, aligned_base - base);
            const uint64_t tail = base + span + alignment - (aligned_base + span);
            if (tail) mem.untrack_allocation(aligned_base + span, tail);
            base = aligned_base;
        } else {
            if (base > UINT64_MAX - (alignment - 1))
                throw EmuError("ELF: load-bias alignment overflow");
            base = (base + alignment - 1) & ~(alignment - 1);
        }
        if (image_end > UINT64_MAX - Memory::PAGE_MASK - base ||
            e_entry > UINT64_MAX - base)
            throw EmuError("ELF: biased image address overflow");
        info.base_addr = base;
        info.entry += base;
    } else if (options.reserve_image) {
        throw EmuError("ELF: interpreter must be ET_DYN");
    }
    // Detect PT_INTERP (dynamic linker path).
    for (auto& h : phdrs) {
        if (h.p_type == 3) {  // PT_INTERP
            if (h.p_filesz < 2 || h.p_filesz > 4096 || h.p_offset > data.size() ||
                h.p_filesz > data.size() - h.p_offset ||
                data[h.p_offset + h.p_filesz - 1] != 0)
                throw EmuError("ELF: invalid PT_INTERP path");
            {
                info.interp.assign(reinterpret_cast<const char*>(data.data() + h.p_offset), h.p_filesz);
                // Strip trailing NUL.
                if (!info.interp.empty() && info.interp.back() == '\0')
                    info.interp.pop_back();
                if (info.interp.find('\0') != std::string::npos)
                    throw EmuError("ELF: embedded NUL in PT_INTERP path");
            }
            break;
        }
    }
    for (auto& h : phdrs) {
        if (h.p_type == 6) {  // PT_PHDR
            info.phdr_addr = h.p_vaddr + info.base_addr;
        }
    }
    bool have_start = false;
    struct SegmentProtection { uint64_t addr, size; uint8_t prot; };
    std::vector<SegmentProtection> segment_protections;
    for (auto& h : phdrs) {
        if (h.p_type != 1) continue;  // PT_LOAD only
        uint64_t vaddr = h.p_vaddr + info.base_addr;
        if (h.p_memsz && !mem.map_range(vaddr, h.p_memsz))
            throw EmuError("ELF: cannot map PT_LOAD");
        if (h.p_filesz > 0) {
            if (h.p_offset + h.p_filesz > data.size())
                throw EmuError("PT_LOAD file range out of bounds");
            mem.write(vaddr, data.data() + h.p_offset, h.p_filesz);
        }
        // p_memsz). On real Linux, the kernel gives zero pages for
        // the BSS. Our map_range may leave stale data from a previous
        // binary (e.g., after execve). Without this, musl's global
        // variables (malloc locks, FILE structs) have garbage values,
        // causing assertion failures (BRK #1000 = musl's a_crash()).
        if (h.p_memsz > h.p_filesz) {
            uint64_t bss_start = vaddr + h.p_filesz;
            uint64_t bss_size = h.p_memsz - h.p_filesz;
            const uint8_t zeros[65536] = {};
            while (bss_size) {
                const uint64_t count = std::min<uint64_t>(bss_size, sizeof(zeros));
                mem.write(bss_start, zeros, count);
                bss_start += count;
                bss_size -= count;
            }
        }
        if (h.p_memsz) {
            uint64_t end = vaddr + h.p_memsz;
            if (end < vaddr || end > UINT64_MAX - Memory::PAGE_MASK)
                throw EmuError("PT_LOAD range overflow");
            const uint64_t prot = ((h.p_flags & 4) ? Memory::GUEST_PROT_READ : 0) |
                                  ((h.p_flags & 2) ? Memory::GUEST_PROT_WRITE : 0) |
                                  ((h.p_flags & 1) ? Memory::GUEST_PROT_EXEC : 0);
            const uint64_t page_start = vaddr & ~Memory::PAGE_MASK;
            const uint64_t page_end = (end + Memory::PAGE_MASK) & ~Memory::PAGE_MASK;
            segment_protections.push_back({page_start, page_end - page_start,
                                           static_cast<uint8_t>(prot)});
        }
        uint64_t end = vaddr + h.p_memsz;
        if (end > info.end_addr) info.end_addr = end;
        const uint64_t page_start = vaddr & ~Memory::PAGE_MASK;
        if (!have_start || page_start < info.start_addr) info.start_addr = page_start;
        have_start = true;
    }
    // Apply PT_LOAD permissions only after all file and BSS bytes have been
    // installed. Adjacent segments can share a page; later headers define
    // the final protection for that shared page as on Linux.
    for (const auto& seg : segment_protections)
        if (!mem.mprotect_guest(seg.addr, seg.size, seg.prot))
            throw EmuError("unable to set PT_LOAD permissions");
    // If phdr_addr still 0 (no PT_PHDR), try to derive from first PT_LOAD
    if (info.phdr_addr == 0 && !phdrs.empty()) {
        for (auto& h : phdrs) {
            if (h.p_type == 1) {
                if (e_phoff >= h.p_offset && e_phoff - h.p_offset <= h.p_filesz &&
                    static_cast<uint64_t>(e_phnum) * e_phentsize <=
                        h.p_filesz - (e_phoff - h.p_offset)) {
                    info.phdr_addr = h.p_vaddr + info.base_addr + e_phoff - h.p_offset;
                    break;
                }
            }
        }
    }
    // Parse PT_NOTE segments to detect the GNU property AArch64 LSE feature.
    for (auto& h : phdrs) {
        if (h.p_type != 4) continue;  // PT_NOTE
        if (h.p_offset + h.p_filesz > data.size()) continue;
        const uint8_t* note_base = data.data() + h.p_offset;
        uint64_t note_size = h.p_filesz;
        uint64_t off = 0;
        while (off + 12 <= note_size) {
            uint32_t n_namesz, n_descsz, n_type;
            memcpy(&n_namesz, note_base + off + 0, 4);
            memcpy(&n_descsz, note_base + off + 4, 4);
            memcpy(&n_type,   note_base + off + 8, 4);
            uint32_t name_pad = (4 - (n_namesz & 3)) & 3;
            uint32_t desc_pad = (4 - (n_descsz & 3)) & 3;
            if (off + 12 + n_namesz + name_pad + n_descsz + desc_pad > note_size) break;
            const uint8_t* name = note_base + off + 12;
            const uint8_t* desc = name + n_namesz + name_pad;
            // NT_GNU_PROPERTY_TYPE_0 (5) with vendor "GNU\0"
            if (n_type == 5 && n_namesz >= 4 &&
                name[0]=='G' && name[1]=='N' && name[2]=='U' && name[3]==0) {
                uint32_t d_off = 0;
                while (d_off + 8 <= n_descsz) {
                    uint32_t pr_type, pr_datasz;
                    memcpy(&pr_type,   desc + d_off + 0, 4);
                    memcpy(&pr_datasz, desc + d_off + 4, 4);
                    if (d_off + 8 + pr_datasz > n_descsz) break;
                    // GNU_PROPERTY_AARCH64_FEATURE_1_AND = 0xC0000000
                    if (pr_type == 0xC0000000 && pr_datasz >= 4) {
                        uint32_t features;
                        memcpy(&features, desc + d_off + 8, 4);
                        // GNU_PROPERTY_AARCH64_FEATURE_1_LSE = 0x8
                        if (features & 0x8) info.has_lse = true;
                    }
                    uint32_t pr_pad = (4 - (pr_datasz & 3)) & 3;
                    d_off += 8 + pr_datasz + pr_pad;
                }
            }
            off += 12 + n_namesz + name_pad + n_descsz + desc_pad;
        }
        if (info.has_lse) break;
    }
    // Process RELA relocations (.rela.plt and .rela.dyn if present).
    if (options.apply_relocations && e_shoff > 0 && e_shnum > 0 && e_shentsize >= 40) {
        uint64_t shstr_off = e_shoff + static_cast<uint64_t>(e_shstrndx) * e_shentsize;
        if (shstr_off + e_shentsize <= data.size()) {
            for (uint16_t i = 0; i < e_shnum; i++) {
                uint64_t sh_off = e_shoff + static_cast<uint64_t>(i) * e_shentsize;
                if (sh_off + e_shentsize > data.size()) break;
                uint32_t sh_name, sh_type;
                uint64_t sh_offset, sh_size;
                memcpy(&sh_name,  data.data() + sh_off + 0,  4);
                memcpy(&sh_type,  data.data() + sh_off + 4,  4);
                memcpy(&sh_offset,data.data() + sh_off + 24, 8);
                memcpy(&sh_size,  data.data() + sh_off + 32, 8);
                if (sh_type != 4) continue;  // SHT_RELA
                if (sh_offset + sh_size > data.size()) continue;
                uint64_t nrela = sh_size / 24;
                for (uint64_t j = 0; j < nrela; j++) {
                    uint64_t r_offset, r_info, r_addend;
                    memcpy(&r_offset, data.data() + sh_offset + j*24 + 0,  8);
                    memcpy(&r_info,   data.data() + sh_offset + j*24 + 8,  8);
                    memcpy(&r_addend, data.data() + sh_offset + j*24 + 16, 8);
                    uint32_t rtype = r_info & 0xFFFFFFFF;
                    // Per the AArch64 ELF ABI (ARM IHI 0056B), the dynamic
                    // relocation codes are:
                    //   257  R_AARCH64_ABS64      : *(addr) = S + A
                    //   1025 R_AARCH64_GLOB_DAT   : *(addr) = S + A
                    //   1026 R_AARCH64_JUMP_SLOT   : *(addr) = S + A  (PLT)
                    //   1027 R_AARCH64_RELATIVE   : *(addr) = Delta + A
                    //   1032 R_AARCH64_IRELATIVE  : *(addr) = Indirect(Delta + A)
                    // For static binaries (no PT_INTERP), S is always 0
                    // (no symbol resolution), so we just write A. The
                    // Delta (= load bias) is info.base_addr (0 for ET_EXEC,
                    // PIE_BASE for ET_DYN/PIE).
                    // relative to the load base. We MUST add info.base_addr
                    // to get the actual guest VA. Without this, relocations
                    // were written to low memory (near 0) instead of the
                    // actual PIE load range, corrupting the zero page and
                    // breaking PIE static binaries.
                    uint64_t target = r_offset + info.base_addr;
                    if (rtype == 1027) {
                        // R_AARCH64_RELATIVE: *(addr) = Delta + A
                        mem.store<uint64_t>(target, info.base_addr + r_addend);
                    } else if (rtype == 1026 || rtype == 1025 ||
                               rtype == 257) {
                        // For static binaries without symbol resolution,
                        // S=0, so *(addr) = A. For PIE, the addend already
                        // includes the relative offset; the dynamic linker
                        // (if present) will handle symbol resolution.
                        mem.store<uint64_t>(target, r_addend);
                    } else if (rtype == 1032) {
                        // R_AARCH64_IRELATIVE: *(addr) = call_resolver(Delta + A)
                        // call the resolver here (no CPU available in
                        // elf_loader). Store info.base_addr + r_addend so at
                        // least the resolver ADDRESS is correct for PIE
                        // (was just r_addend, which was wrong for PIE). The
                        // dynamic linker (dynamic_linker.cpp) handles
                        // IRELATIVE correctly for dynamically-linked binaries
                        // by calling the resolver via the ifunc_resolver_
                        // callback. Static-PIE binaries with ifuncs are rare
                        // (most ifunc users are glibc functions like memcpy
                        // which aren't in static binaries); if encountered,
                        // the guest will call the resolver body as the
                        // function (visible failure, not silent corruption).
                        mem.store<uint64_t>(target, info.base_addr + r_addend);
                    }
                }
            }
        }
    }
    return info;
}
} // namespace arm64emu
