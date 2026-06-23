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

namespace arm64emu {

ElfLoader::Loaded ElfLoader::load(Memory& mem, const std::vector<uint8_t>& data) {
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

    Loaded info{};
    info.entry = e_entry;
    info.phent = e_phentsize;
    info.phnum = e_phnum;
    info.phdr_addr = 0;
    info.end_addr  = 0;
    info.has_lse   = false;

    for (auto& h : phdrs) {
        if (h.p_type == 6) {  // PT_PHDR
            info.phdr_addr = h.p_vaddr;
        }
    }

    for (auto& h : phdrs) {
        if (h.p_type != 1) continue;  // PT_LOAD only
        uint64_t vaddr = h.p_vaddr;
        mem.map_range(vaddr, h.p_memsz);
        if (h.p_filesz > 0) {
            if (h.p_offset + h.p_filesz > data.size())
                throw EmuError("PT_LOAD file range out of bounds");
            mem.write(vaddr, data.data() + h.p_offset, h.p_filesz);
        }
        uint64_t end = vaddr + h.p_memsz;
        if (end > info.end_addr) info.end_addr = end;
    }

    // If phdr_addr still 0 (no PT_PHDR), try to derive from first PT_LOAD
    if (info.phdr_addr == 0 && !phdrs.empty()) {
        for (auto& h : phdrs) {
            if (h.p_type == 1) {
                info.phdr_addr = h.p_vaddr + e_phoff;
                break;
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
    if (e_shoff > 0 && e_shnum > 0 && e_shentsize >= 40) {
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
                    // R_AARCH64_JUMP_SLOT (1032), R_AARCH64_GLOB_DAT (1025),
                    // R_AARCH64_RELATIVE (1027): *(addr) = addend
                    if (rtype == 1032 || rtype == 1025 || rtype == 1027) {
                        mem.store<uint64_t>(r_offset, r_addend);
                    }
                    // R_AARCH64_ABS64 (257): *(addr) = addend + S
                    else if (rtype == 257) {
                        mem.store<uint64_t>(r_offset, r_addend);
                    }
                }
            }
        }
    }

    return info;
}

} // namespace arm64emu
