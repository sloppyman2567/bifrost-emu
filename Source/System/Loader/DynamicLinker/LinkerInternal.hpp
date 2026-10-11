// LinkerInternal.hpp — ELF constants, record layouts and helpers shared by
// the dynamic-linker sources. Include only from Source/System/Loader/DynamicLinker/.
#pragma once
#include "System/Loader/DynamicLinker.hpp"
#include "System/Memory/Memory.hpp"
#include <cstdint>
#include <string>
namespace arm64emu {
bool dynlink_trace_enabled();  // BIFROST_DYNLINK_TRACE (Link.cpp)
bool ifunc_trace_enabled();    // BIFROST_IFUNC_TRACE (Link.cpp)
// ── ELF dynamic tag constants ──────────────────────────────────────────
// From elf.h (we hardcode to avoid pulling in the host's elf.h, which
// may not have all AArch64-specific tags).
namespace {
constexpr int DT_NULL_      = 0;
constexpr int DT_NEEDED_    = 1;
constexpr int DT_PLTRELSZ_  = 2;
constexpr int DT_PLTGOT_    = 3;
constexpr int DT_HASH_      = 4;
constexpr int DT_GNU_HASH_  = 0x6ffffef5;
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
inline uint32_t ELF64_R_SYM_(uint64_t info)  { return info >> 32; }
inline uint32_t ELF64_R_TYPE_(uint64_t info) { return info & 0xFFFFFFFF; }
// Symbol binding/type extractors.
inline uint8_t ST_BIND_(uint8_t info)  { return info >> 4; }
constexpr uint8_t STB_GLOBAL_= 1;
constexpr uint8_t STB_WEAK_  = 2;
constexpr uint16_t SHN_UNDEF_ = 0;
// ── Helpers ────────────────────────────────────────────────────────────
// Read a string from guest memory at `addr` (NUL-terminated).
inline std::string read_guest_cstr(Memory& mem, uint64_t addr) {
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
}  // namespace arm64emu
