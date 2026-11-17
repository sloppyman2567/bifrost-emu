// NativeLibc.cpp — see NativeLibc.hpp.
#include "System/Core/NativeLibc.hpp"
#include "System/Core/CPU.hpp"
#include "System/Memory/Memory.hpp"
#include <cstring>
#include <elf.h>
#include <mutex>

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
    by_pc_[pc] = kind;
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

void register_symtab_routines(NativeLibcRoutines& table, const std::vector<uint8_t>& elf, uint64_t load_base) {
    // Every offset below comes from the file, so each read is bounds-checked against the buffer.
    const auto in_range = [&](uint64_t off, uint64_t len) { return off <= elf.size() && len <= elf.size() - off; };
    if (!in_range(0, sizeof(Elf64_Ehdr))) return;
    Elf64_Ehdr eh;
    std::memcpy(&eh, elf.data(), sizeof(eh));
    if (std::memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_ident[EI_CLASS] != ELFCLASS64 ||
        eh.e_ident[EI_DATA] != ELFDATA2LSB || eh.e_machine != EM_AARCH64 || eh.e_shentsize != sizeof(Elf64_Shdr) ||
        eh.e_shnum == 0 || eh.e_shnum > 4096 || !in_range(eh.e_shoff, uint64_t(eh.e_shnum) * sizeof(Elf64_Shdr)))
        return;
    const auto section = [&](unsigned i) {
        Elf64_Shdr sh;
        std::memcpy(&sh, elf.data() + eh.e_shoff + uint64_t(i) * sizeof(sh), sizeof(sh));
        return sh;
    };
    for (unsigned i = 0; i < eh.e_shnum; i++) {
        const Elf64_Shdr symtab = section(i);
        if (symtab.sh_type != SHT_SYMTAB || symtab.sh_entsize != sizeof(Elf64_Sym) || symtab.sh_link >= eh.e_shnum ||
            !in_range(symtab.sh_offset, symtab.sh_size))
            continue;
        const Elf64_Shdr strtab = section(symtab.sh_link);
        if (strtab.sh_type != SHT_STRTAB || !in_range(strtab.sh_offset, strtab.sh_size)) continue;
        const uint64_t count = symtab.sh_size / sizeof(Elf64_Sym);
        for (uint64_t k = 0; k < count; k++) {
            Elf64_Sym sym;
            std::memcpy(&sym, elf.data() + symtab.sh_offset + k * sizeof(sym), sizeof(sym));
            if (ELF64_ST_TYPE(sym.st_info) != STT_FUNC || sym.st_shndx == SHN_UNDEF || sym.st_value == 0 ||
                sym.st_name >= strtab.sh_size)
                continue;
            const char* name = reinterpret_cast<const char*>(elf.data() + strtab.sh_offset + sym.st_name);
            const size_t max_len = static_cast<size_t>(strtab.sh_size - sym.st_name);
            const size_t len = strnlen(name, max_len);
            if (len == max_len) continue;  // not terminated inside the table
            const NativeLibcKind kind = NativeLibcRoutines::kind_of(std::string(name, len));
            if (kind != NativeLibcKind::None) table.add(load_base + sym.st_value, kind);
        }
    }
}

}  // namespace arm64emu
