#include "System/Loader/ThunkStub.hpp"
#include <cstring>
#include <unordered_set>
namespace arm64emu {
namespace {
constexpr uint16_t EM_AARCH64_ = 183;
constexpr uint64_t PAGE = 0x1000;
constexpr size_t PHNUM = 4;
constexpr size_t STUB_BYTES = 20;  // movz + 3 movk + br
// Dynamic entries: HASH, STRTAB, SYMTAB, STRSZ, SYMENT, SONAME, NULL.
constexpr size_t DYN_ENTRIES = 7;

uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

uint32_t sysv_hash(const std::string& name) {
    uint32_t h = 0;
    for (unsigned char c : name) {
        h = (h << 4) + c;
        const uint32_t g = h & 0xf0000000u;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

template <typename T>
void put(std::vector<uint8_t>& out, uint64_t off, T value) {
    memcpy(out.data() + off, &value, sizeof(T));
}

// Branch to `target` through x16 (IP0, free for veneers); a0..a7 and the
// stack arguments stay untouched, and the trampoline's RET returns to the
// original caller.
void write_branch(std::vector<uint8_t>& out, uint64_t off, uint64_t target) {
    for (unsigned hw = 0; hw < 4; hw++) {
        const uint32_t imm = static_cast<uint32_t>((target >> (16 * hw)) & 0xFFFF);
        const uint32_t base = hw == 0 ? 0xD2800000u : 0xF2800000u;  // MOVZ / MOVK
        put<uint32_t>(out, off + 4 * hw, base | (hw << 21) | (imm << 5) | 16);
    }
    put<uint32_t>(out, off + 16, 0xD61F0200u);  // BR x16
}
}  // namespace

std::vector<uint8_t> build_thunk_stub_elf(
    const std::string& soname,
    const std::vector<std::pair<std::string, uint64_t>>& symbols) {
    std::vector<std::pair<std::string, uint64_t>> syms;
    std::unordered_set<std::string> seen;
    for (const auto& s : symbols)
        if (!s.first.empty() && seen.insert(s.first).second) syms.push_back(s);
    const size_t nsyms = syms.size();
    // Read-only part: headers, hash, dynsym, dynstr, code.
    std::string dynstr(1, '\0');
    const uint32_t soname_off = static_cast<uint32_t>(dynstr.size());
    dynstr += soname;
    dynstr += '\0';
    std::vector<uint32_t> name_off(nsyms);
    for (size_t i = 0; i < nsyms; i++) {
        name_off[i] = static_cast<uint32_t>(dynstr.size());
        dynstr += syms[i].first;
        dynstr += '\0';
    }
    const uint32_t nbucket = static_cast<uint32_t>(nsyms / 4 + 1);
    const uint32_t nchain = static_cast<uint32_t>(nsyms + 1);
    const uint64_t hash_off = align_up(64 + PHNUM * 56, 8);
    const uint64_t sym_off = align_up(hash_off + 4ull * (2 + nbucket + nchain), 8);
    const uint64_t str_off = sym_off + 24ull * nchain;
    const uint64_t code_off = align_up(str_off + dynstr.size(), 16);
    const uint64_t ro_end = code_off + STUB_BYTES * nsyms;
    // Writable part: the dynamic section (ld.so adjusts it in place).
    const uint64_t dyn_off = align_up(ro_end, PAGE);
    const uint64_t dyn_size = 16ull * DYN_ENTRIES;
    std::vector<uint8_t> out(dyn_off + dyn_size, 0);

    memcpy(out.data(), "\x7f" "ELF\x02\x01\x01", 7);
    put<uint16_t>(out, 16, 3);  // ET_DYN
    put<uint16_t>(out, 18, EM_AARCH64_);
    put<uint32_t>(out, 20, 1);
    put<uint64_t>(out, 32, 64);  // e_phoff
    put<uint16_t>(out, 52, 64);  // e_ehsize
    put<uint16_t>(out, 54, 56);  // e_phentsize
    put<uint16_t>(out, 56, PHNUM);
    put<uint16_t>(out, 58, 64);  // e_shentsize
    auto phdr = [&](size_t i, uint32_t type, uint32_t flags, uint64_t off,
                    uint64_t size, uint64_t align) {
        const uint64_t p = 64 + i * 56;
        put<uint32_t>(out, p, type);
        put<uint32_t>(out, p + 4, flags);
        put<uint64_t>(out, p + 8, off);
        put<uint64_t>(out, p + 16, off);  // vaddr == offset
        put<uint64_t>(out, p + 24, off);
        put<uint64_t>(out, p + 32, size);
        put<uint64_t>(out, p + 40, size);
        put<uint64_t>(out, p + 48, align);
    };
    phdr(0, 1, 5, 0, ro_end, PAGE);           // PT_LOAD R+X
    phdr(1, 1, 6, dyn_off, dyn_size, PAGE);   // PT_LOAD R+W
    phdr(2, 2, 6, dyn_off, dyn_size, 8);      // PT_DYNAMIC
    phdr(3, 0x6474e551, 6, 0, 0, 16);         // PT_GNU_STACK, non-executable

    // SysV hash: buckets then chains, symbol 0 is the null symbol.
    std::vector<uint32_t> bucket(nbucket, 0), chain(nchain, 0);
    for (size_t i = 0; i < nsyms; i++) {
        const uint32_t idx = static_cast<uint32_t>(i + 1);
        const uint32_t b = sysv_hash(syms[i].first) % nbucket;
        chain[idx] = bucket[b];
        bucket[b] = idx;
    }
    put<uint32_t>(out, hash_off, nbucket);
    put<uint32_t>(out, hash_off + 4, nchain);
    for (uint32_t i = 0; i < nbucket; i++) put<uint32_t>(out, hash_off + 8 + 4ull * i, bucket[i]);
    for (uint32_t i = 0; i < nchain; i++)
        put<uint32_t>(out, hash_off + 8 + 4ull * nbucket + 4ull * i, chain[i]);

    for (size_t i = 0; i < nsyms; i++) {
        const uint64_t s = sym_off + 24ull * (i + 1);
        const uint64_t fn = code_off + STUB_BYTES * i;
        put<uint32_t>(out, s, name_off[i]);
        out[s + 4] = 0x12;                // STB_GLOBAL, STT_FUNC
        put<uint16_t>(out, s + 6, 1);     // defined (not SHN_UNDEF/SHN_ABS)
        put<uint64_t>(out, s + 8, fn);
        put<uint64_t>(out, s + 16, STUB_BYTES);
        write_branch(out, fn, syms[i].second);
    }
    memcpy(out.data() + str_off, dynstr.data(), dynstr.size());

    uint64_t d = dyn_off;
    auto dyn = [&](uint64_t tag, uint64_t val) {
        put<uint64_t>(out, d, tag);
        put<uint64_t>(out, d + 8, val);
        d += 16;
    };
    dyn(4, hash_off);        // DT_HASH
    dyn(5, str_off);         // DT_STRTAB
    dyn(6, sym_off);         // DT_SYMTAB
    dyn(10, dynstr.size());  // DT_STRSZ
    dyn(11, 24);             // DT_SYMENT
    dyn(14, soname_off);     // DT_SONAME
    dyn(0, 0);               // DT_NULL
    return out;
}
}  // namespace arm64emu
