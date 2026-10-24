// GuestLibcSymbol.cpp — read a libc symbol's address from the mapped image's file.
#include "System/Loader/GuestLibcSymbol.hpp"
#include "System/Memory/Memory.hpp"
#include "System/VFS/VFS.hpp"
#include <elf.h>
#include <fstream>
#include <vector>

namespace arm64emu {

static bool is_glibc_libc(const std::string& path) {
    const size_t slash = path.rfind('/');
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    return name == "libc.so.6" || name.rfind("libc-", 0) == 0;
}

uint64_t find_guest_libc_symbol(const Memory& mem, const std::string& symbol) {
    for (const auto& map : mem.mappings_snapshot()) {
        if (!map.file || map.offset != 0 || !is_glibc_libc(map.file->path)) continue;
        const std::string host = map.file->host_path ? map.file->path
                                                     : yggdrasil::Yggdrasil::remap_path(map.file->path);
        std::ifstream file(host, std::ios::binary);
        Elf64_Ehdr eh{};
        if (!file.read(reinterpret_cast<char*>(&eh), sizeof eh) || eh.e_machine != EM_AARCH64 ||
            eh.e_shentsize != sizeof(Elf64_Shdr) || eh.e_shnum == 0 || eh.e_shnum > 256)
            continue;
        std::vector<Elf64_Shdr> sections(eh.e_shnum);
        file.seekg(static_cast<std::streamoff>(eh.e_shoff));
        if (!file.read(reinterpret_cast<char*>(sections.data()), sections.size() * sizeof(Elf64_Shdr))) continue;
        for (const auto& dynsym : sections) {
            if (dynsym.sh_type != SHT_DYNSYM || dynsym.sh_link >= sections.size() ||
                dynsym.sh_entsize != sizeof(Elf64_Sym) || dynsym.sh_size > (16u << 20))
                continue;
            const auto& strtab = sections[dynsym.sh_link];
            if (strtab.sh_size > (16u << 20)) continue;
            std::vector<Elf64_Sym> symbols(dynsym.sh_size / sizeof(Elf64_Sym));
            std::vector<char> names(strtab.sh_size + 1, '\0');
            file.seekg(static_cast<std::streamoff>(dynsym.sh_offset));
            if (!file.read(reinterpret_cast<char*>(symbols.data()), symbols.size() * sizeof(Elf64_Sym))) continue;
            file.seekg(static_cast<std::streamoff>(strtab.sh_offset));
            if (!file.read(names.data(), strtab.sh_size)) continue;
            for (const auto& sym : symbols) {
                if (sym.st_shndx == SHN_UNDEF || ELF64_ST_TYPE(sym.st_info) != STT_OBJECT ||
                    sym.st_name >= strtab.sh_size || symbol != &names[sym.st_name])
                    continue;
                // The first PT_LOAD of libc has vaddr 0 and offset 0, so the mapping start is the base.
                return map.start + sym.st_value;
            }
        }
    }
    return 0;
}

}  // namespace arm64emu
