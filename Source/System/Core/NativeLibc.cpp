// NativeLibc.cpp — see NativeLibc.hpp.
#include "System/Core/NativeLibc.hpp"
#include "System/Core/CPU.hpp"
#include "System/Core/DebugFlags.hpp"
#include "System/Memory/Memory.hpp"
#include <cstring>
#include <cstdio>
#include <elf.h>
#include <fstream>
#include <mutex>
#include <unistd.h>

namespace arm64emu {

NativeLibcKind NativeLibcRoutines::kind_of(const std::string& name) {
    if (name == "memcpy") return NativeLibcKind::Memcpy;
    if (name == "memmove") return NativeLibcKind::Memmove;
    if (name == "memset") return NativeLibcKind::Memset;
    return NativeLibcKind::None;
}

void NativeLibcRoutines::add(uint64_t pc, NativeLibcKind kind) {
    if (kind == NativeLibcKind::None || pc == 0) return;
    std::unique_lock<std::shared_mutex> lock(mu_);
    const auto [it, inserted] = by_pc_.try_emplace(pc, kind);
    // Bionic's memcpy and memmove are one function under two names; its overlap handling is the memmove one.
    if (!inserted && kind == NativeLibcKind::Memmove) it->second = kind;
    count_.store(by_pc_.size(), std::memory_order_relaxed);
}

NativeLibcKind NativeLibcRoutines::find(uint64_t pc) const {
    if (empty()) return NativeLibcKind::None;
    std::shared_lock<std::shared_mutex> lock(mu_);
    const auto it = by_pc_.find(pc);
    return it == by_pc_.end() ? NativeLibcKind::None : it->second;
}

void NativeLibcRoutines::clear() {
    std::unique_lock<std::shared_mutex> lock(mu_);
    by_pc_.clear();
    count_.store(0, std::memory_order_relaxed);
}

bool NativeLibcRoutines::run(Memory& mem, CPU& cpu, NativeLibcKind kind) {
    const uint64_t dst = cpu.regs[0], arg1 = cpu.regs[1], n = cpu.regs[2];
    if (n < kNativeLibcMinBytes) return false;
    // Every byte of every range must be accessible with the right permission, or the guest routine runs and faults
    // (or is routed through a userfaultfd page) exactly as it always did. Nothing is written until all checks pass.
    uint8_t* d = mem.guest_to_host_ptr(dst, Memory::GUEST_PROT_WRITE, static_cast<size_t>(n));
    if (!d) return false;
    switch (kind) {
    case NativeLibcKind::Memset:
        std::memset(d, static_cast<int>(arg1 & 0xFF), static_cast<size_t>(n));
        break;
    case NativeLibcKind::Memcpy:
    case NativeLibcKind::Memmove: {
        const uint8_t* s = mem.guest_to_host_ptr(arg1, Memory::GUEST_PROT_READ, static_cast<size_t>(n));
        if (!s) return false;
        if (kind == NativeLibcKind::Memcpy) {
            // Overlapping memcpy is undefined; what the guest routine does with it (a forward copy in some
            // libcs, a backward one in others) is not ours to guess, so the guest routine keeps it.
            if (s < d + n && d < s + n) return false;
            std::memcpy(d, s, static_cast<size_t>(n));
        } else {
            std::memmove(d, s, static_cast<size_t>(n));
        }
        break;
    }
    case NativeLibcKind::None:
        return false;
    }
    cpu.regs[0] = dst;       // all three return their destination
    cpu.pc = cpu.regs[30];   // as the routine's `ret` would
    calls_.fetch_add(1, std::memory_order_relaxed);
    bytes_.fetch_add(n, std::memory_order_relaxed);
    return true;
}

namespace {

struct Symbol {
    std::string name;
    uint64_t value;
    uint8_t type;  // STT_FUNC or STT_GNU_IFUNC
};

// Every offset below comes from the file, so each read is bounds-checked against the buffer.
bool in_range(const std::vector<uint8_t>& elf, uint64_t off, uint64_t len) {
    return off <= elf.size() && len <= elf.size() - off;
}

bool read_header(const std::vector<uint8_t>& elf, Elf64_Ehdr& eh) {
    if (!in_range(elf, 0, sizeof(Elf64_Ehdr))) return false;
    std::memcpy(&eh, elf.data(), sizeof(eh));
    return std::memcmp(eh.e_ident, ELFMAG, SELFMAG) == 0 && eh.e_ident[EI_CLASS] == ELFCLASS64 &&
           eh.e_ident[EI_DATA] == ELFDATA2LSB && eh.e_machine == EM_AARCH64;
}

// The defined function symbols of .symtab and .dynsym, ifuncs included (their value is a resolver).
std::vector<Symbol> function_symbols(const std::vector<uint8_t>& elf, const Elf64_Ehdr& eh) {
    std::vector<Symbol> out;
    if (eh.e_shentsize != sizeof(Elf64_Shdr) || eh.e_shnum == 0 || eh.e_shnum > 4096 ||
        !in_range(elf, eh.e_shoff, uint64_t(eh.e_shnum) * sizeof(Elf64_Shdr)))
        return out;
    const auto section = [&](unsigned i) {
        Elf64_Shdr sh;
        std::memcpy(&sh, elf.data() + eh.e_shoff + uint64_t(i) * sizeof(sh), sizeof(sh));
        return sh;
    };
    for (unsigned i = 0; i < eh.e_shnum; i++) {
        const Elf64_Shdr symtab = section(i);
        if ((symtab.sh_type != SHT_SYMTAB && symtab.sh_type != SHT_DYNSYM) || symtab.sh_entsize != sizeof(Elf64_Sym) ||
            symtab.sh_link >= eh.e_shnum || !in_range(elf, symtab.sh_offset, symtab.sh_size))
            continue;
        const Elf64_Shdr strtab = section(symtab.sh_link);
        if (strtab.sh_type != SHT_STRTAB || !in_range(elf, strtab.sh_offset, strtab.sh_size)) continue;
        const uint64_t count = symtab.sh_size / sizeof(Elf64_Sym);
        for (uint64_t k = 0; k < count; k++) {
            Elf64_Sym sym;
            std::memcpy(&sym, elf.data() + symtab.sh_offset + k * sizeof(sym), sizeof(sym));
            const uint8_t type = ELF64_ST_TYPE(sym.st_info);
            if ((type != STT_FUNC && type != STT_GNU_IFUNC) || sym.st_shndx == SHN_UNDEF || sym.st_value == 0 ||
                sym.st_name >= strtab.sh_size)
                continue;
            const char* name = reinterpret_cast<const char*>(elf.data() + strtab.sh_offset + sym.st_name);
            const size_t max_len = static_cast<size_t>(strtab.sh_size - sym.st_name);
            const size_t len = strnlen(name, max_len);
            if (len == max_len) continue;  // not terminated inside the table
            out.push_back({std::string(name, len), sym.st_value, type});
        }
    }
    return out;
}

// Bionic's ifunc targets, named only in its .symtab: __memcpy_aarch64, __memmove_aarch64_simd, __memset_aarch64_nt, ...
// The checked variants (__memcpy_chk) take another argument and are a different function.
NativeLibcKind implementation_kind(const std::string& name) {
    if (name.find("_chk") != std::string::npos) return NativeLibcKind::None;
    if (name.rfind("__memcpy_aarch64", 0) == 0) return NativeLibcKind::Memcpy;
    if (name.rfind("__memmove_aarch64", 0) == 0) return NativeLibcKind::Memmove;
    if (name.rfind("__memset_aarch64", 0) == 0) return NativeLibcKind::Memset;
    return NativeLibcKind::None;
}

}  // namespace

void register_symtab_routines(NativeLibcRoutines& table, const std::vector<uint8_t>& elf, uint64_t load_base) {
    Elf64_Ehdr eh;
    if (!read_header(elf, eh)) return;
    for (const Symbol& sym : function_symbols(elf, eh)) {
        if (sym.type != STT_FUNC) continue;  // an ifunc's value is the resolver, not the routine
        const NativeLibcKind kind = NativeLibcRoutines::kind_of(sym.name);
        if (kind != NativeLibcKind::None) table.add(load_base + sym.value, kind);
    }
}

void register_mapped_routines(NativeLibcRoutines& table, Memory& mem, const std::vector<uint8_t>& elf,
                              uint64_t map_addr, uint64_t map_size, uint64_t map_offset) {
    Elf64_Ehdr eh;
    if (!read_header(elf, eh) || eh.e_phentsize != sizeof(Elf64_Phdr) || eh.e_phnum == 0 || eh.e_phnum > 256 ||
        !in_range(elf, eh.e_phoff, uint64_t(eh.e_phnum) * sizeof(Elf64_Phdr)))
        return;
    // The program header this mapping starts in, and from it the library's load base.
    constexpr uint64_t kPage = 4096;
    Elf64_Phdr seg{};
    bool found = false;
    for (unsigned i = 0; i < eh.e_phnum && !found; i++) {
        Elf64_Phdr ph;
        std::memcpy(&ph, elf.data() + eh.e_phoff + uint64_t(i) * sizeof(ph), sizeof(ph));
        if (ph.p_type != PT_LOAD) continue;
        const uint64_t lo = ph.p_offset & ~(kPage - 1), hi = (ph.p_offset + ph.p_filesz + kPage - 1) & ~(kPage - 1);
        if (map_offset >= lo && map_offset < hi) { seg = ph; found = true; }
    }
    if (!found) return;
    const uint64_t vaddr_at_map = (seg.p_vaddr & ~(kPage - 1)) + (map_offset - (seg.p_offset & ~(kPage - 1)));
    if (map_addr < vaddr_at_map) return;
    const uint64_t base = map_addr - vaddr_at_map;
    for (const Symbol& sym : function_symbols(elf, eh)) {
        if (sym.type != STT_FUNC) continue;
        NativeLibcKind kind = NativeLibcRoutines::kind_of(sym.name);
        if (kind == NativeLibcKind::None) kind = implementation_kind(sym.name);
        if (kind == NativeLibcKind::None) continue;
        // Inside this segment and this mapping, and the live bytes are the file's.
        if (sym.value < seg.p_vaddr || sym.value > seg.p_vaddr + seg.p_filesz || seg.p_vaddr + seg.p_filesz - sym.value < 32)
            continue;
        const uint64_t guest = base + sym.value;
        if (guest < map_addr || guest - map_addr > map_size || map_size - (guest - map_addr) < 32) continue;
        const uint64_t file_off = seg.p_offset + (sym.value - seg.p_vaddr);
        if (!in_range(elf, file_off, 32)) continue;
        uint8_t live[32];
        try {
            mem.read(guest, live, sizeof(live));
        } catch (...) {
            continue;
        }
        if (std::memcmp(live, elf.data() + file_off, sizeof(live)) != 0) continue;
        table.add(guest, kind);
    }
}

void NativeLibcRoutines::on_exec_mapping(Memory& mem, uint64_t addr, uint64_t size, int fd, uint64_t offset) {
    if (size == 0 || dbg().no_native_libc) return;
    char link[64], target[4096];
    std::snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    const ssize_t n = ::readlink(link, target, sizeof(target) - 1);
    if (n <= 0) return;
    const std::string path(target, static_cast<size_t>(n));
    const std::string name = path.substr(path.find_last_of('/') + 1);
    // A libc: Bionic's and glibc's libc.so[.6], musl's libc and its loader (musl's libc is the loader).
    if (name != "libc.so" && name.rfind("libc.so.", 0) != 0 && name.rfind("libc.musl-", 0) != 0 &&
        name.rfind("ld-musl-", 0) != 0)
        return;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return;
    const std::streamoff length = file.tellg();
    if (length <= 0 || length > (64 << 20)) return;
    std::vector<uint8_t> elf(static_cast<size_t>(length));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(elf.data()), length)) return;
    register_mapped_routines(*this, mem, elf, addr, size, offset);
}

}  // namespace arm64emu
