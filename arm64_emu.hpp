// arm64_emu.hpp - Bifrost-EMU: ARM64 Linux user-mode emulator (v1.3.0-beta.2)
//
// Provides:
//   - Sparse paged 64-bit memory model (thread-safe)
//   - ELF64 little-endian AArch64 static loader
//   - ARM64 instruction decoder/executor (subset sufficient for
//     musl-static binaries, simple glibc-static binaries, and
//     interactive apps)
//   - Linux AArch64 syscall layer (read/write/exit/mmap/brk/ioctl/
//     epoll/timerfd/eventf/ppoll/clone/futex/etc.)
//   - Multi-vCPU support (one Emulator + N guest threads via clone())
//
// All host-side state is held in `class Emulator`. To run a binary:
//
//     Emulator emu;
//   emu.load_elf_file("/path/to/binary", argv);
//   emu.run();
//
// Designed to be readable and fast: a flat switch-based interpreter
// with a register-file array and a single 64-bit PSTATE word.
//
// Architecture note (v1.x): the instruction decoder lives entirely
// in arm64_emu.cpp's `Emulator::execute()`. A future v2.x JIT will
// share the same decoder tables via a `decoder.hpp` header — the
// handler signatures here are kept JIT-friendly (no implicit
// state beyond `cpu_` and `mem_`).
#pragma once

#include "decoder.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstddef>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <stdexcept>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <map>
#include <chrono>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <sys/mman.h>
#include <poll.h>
#include <signal.h>
#include <errno.h>
#include <syscall.h>

namespace arm64emu {

// ---------------------------------------------------------------------------
// Version
// ---------------------------------------------------------------------------
constexpr const char* VERSION = "1.3.0-beta.2";
constexpr const char* CODENAME = "bifrost-emu";

// ---------------------------------------------------------------------------
// Utility
// ---------------------------------------------------------------------------
static inline uint64_t sign_extend(uint64_t v, int bits) {
    if (bits >= 64) return v;
    uint64_t m = 1ULL << (bits - 1);
    return (v ^ m) - m;
}

static inline bool is_power_of_two(uint64_t x) { return x && !(x & (x - 1)); }

static inline uint64_t ror64(uint64_t v, unsigned r) {
    r &= 63;
    return (v >> r) | (v << (64 - r));
}

// ---------------------------------------------------------------------------
// Exceptions
// ---------------------------------------------------------------------------
struct EmuError : std::runtime_error { using std::runtime_error::runtime_error; };
struct UnmappedMemory : EmuError {
    UnmappedMemory(uint64_t a, bool w)
        : EmuError(std::string("unmapped ") + (w ? "write" : "read") +
                   " at 0x" + [&]{ char b[32]; snprintf(b, sizeof(b), "%lx", a); return std::string(b); }()) {}
};
struct DecodeError : EmuError {
    DecodeError(uint64_t pc, uint32_t inst)
        : EmuError([&]{ char b[64]; snprintf(b, sizeof(b),
                   "decode error at pc=0x%lx inst=0x%08x", pc, inst); return std::string(b); }()) {}
};
struct SyscallError : EmuError { using EmuError::EmuError; };

// ---------------------------------------------------------------------------
// Memory - sparse paged, thread-safe
// ---------------------------------------------------------------------------
// All public methods take a shared lock internally so they can be called
// concurrently from multiple guest threads (vCPUs). The lock is fine-
// grained (single mutex for the page map; per-page data is not locked
// because guest code is responsible for its own atomicity via LDXR/STXR
// or LSE atomics).
class Memory {
public:
    static constexpr uint64_t PAGE_SIZE = 4096;
    static constexpr uint64_t PAGE_MASK = PAGE_SIZE - 1;

    Memory() = default;

    void map_range(uint64_t addr, uint64_t size) {
        if (size == 0) return;
        std::lock_guard<std::mutex> g(mu_);
        uint64_t start = addr & ~PAGE_MASK;
        uint64_t end = addr + size;
        for (; start < end; start += PAGE_SIZE) {
            auto it = pages_.find(start / PAGE_SIZE);
            if (it == pages_.end()) {
                pages_.emplace(start / PAGE_SIZE,
                               std::vector<uint8_t>(PAGE_SIZE, 0));
            }
        }
    }

    bool is_mapped(uint64_t addr, uint64_t size) const {
        if (size == 0) return true;
        std::lock_guard<std::mutex> g(mu_);
        uint64_t start = addr & ~PAGE_MASK;
        uint64_t end = addr + size;
        for (; start < end; start += PAGE_SIZE) {
            if (!pages_.count(start / PAGE_SIZE)) return false;
        }
        return true;
    }

    void write(uint64_t addr, const void* src, size_t n) {
        if (n == 0) return;
        const uint8_t* p = (const uint8_t*)src;
        uint64_t cur = addr;
        size_t remaining = n;
        while (remaining > 0) {
            // Lock per-page to avoid holding the global lock for the
            // entire transfer. This still serializes accesses to the
            // same page, which is what we want for correctness.
            uint64_t pn = cur / PAGE_SIZE;
            std::vector<uint8_t>* page = nullptr;
            {
                std::lock_guard<std::mutex> g(mu_);
                auto it = pages_.find(pn);
                if (it == pages_.end()) {
                    it = pages_.emplace(pn, std::vector<uint8_t>(PAGE_SIZE, 0)).first;
                }
                page = &it->second;
            }
            uint64_t off = cur & PAGE_MASK;
            size_t take = std::min<size_t>(PAGE_SIZE - off, remaining);
            memcpy(page->data() + off, p, take);
            p += take;
            cur += take;
            remaining -= take;
        }
    }

    void read(uint64_t addr, void* dst, size_t n) const {
        if (n == 0) return;
        uint8_t* p = (uint8_t*)dst;
        uint64_t cur = addr;
        size_t remaining = n;
        while (remaining > 0) {
            uint64_t pn = cur / PAGE_SIZE;
            const std::vector<uint8_t>* page = nullptr;
            {
                std::lock_guard<std::mutex> g(mu_);
                auto it = pages_.find(pn);
                if (it == pages_.end()) throw UnmappedMemory(cur, false);
                page = &it->second;
            }
            uint64_t off = cur & PAGE_MASK;
            size_t take = std::min<size_t>(PAGE_SIZE - off, remaining);
            memcpy(p, page->data() + off, take);
            p += take;
            cur += take;
            remaining -= take;
        }
    }

    // Convenience templates for fixed-width LE access
    template<typename T>
    T load(uint64_t addr) const {
        T v;
        read(addr, &v, sizeof(T));
        return v;
    }
    template<typename T>
    void store(uint64_t addr, T v) {
        write(addr, &v, sizeof(T));
    }

    uint32_t fetch_inst(uint64_t addr) const {
        return load<uint32_t>(addr);
    }

    size_t page_count() const {
        std::lock_guard<std::mutex> g(mu_);
        return pages_.size();
    }

    // Allocate a chunk of fresh memory; returns starting address.
    // Simple bump allocator over a high address range. Thread-safe.
    //
    // When `hint` is non-zero, the allocation is placed at exactly `hint`
    // (this is the MAP_FIXED semantic). Existing pages at that address
    // are REPLACED with fresh zeroed pages — this matches Linux kernel
    // behavior, where mmap(MAP_FIXED) unmaps any existing mapping first.
    uint64_t mmap_alloc(uint64_t size, uint64_t hint = 0) {
        if (size == 0) size = PAGE_SIZE;
        std::lock_guard<std::mutex> g(mu_);
        uint64_t base = hint;
        if (base == 0) {
            base = mmap_next_;
            mmap_next_ += ((size + PAGE_MASK) & ~PAGE_MASK);
        } else {
            mmap_next_ = std::max(mmap_next_,
                                  base + ((size + PAGE_MASK) & ~PAGE_MASK));
        }
        uint64_t start = base & ~PAGE_MASK;
        uint64_t end = base + size;
        for (; start < end; start += PAGE_SIZE) {
            auto it = pages_.find(start / PAGE_SIZE);
            if (it == pages_.end()) {
                pages_.emplace(start / PAGE_SIZE,
                               std::vector<uint8_t>(PAGE_SIZE, 0));
            }
            // Preserve existing pages on MAP_FIXED (don't zero).
            // This is needed because musl's mallocng uses MAP_FIXED
            // for guard pages, and zeroing would corrupt metadata.
        }
        return base;
    }

    // Atomic compare-and-swap on a 32-bit memory location.
    // Used by LSE atomics (CAS) and futex. Returns true if swapped.
    bool atomic_cas_32(uint64_t addr, uint32_t expected, uint32_t desired) {
        std::lock_guard<std::mutex> g(mu_);
        auto it = pages_.find(addr / PAGE_SIZE);
        if (it == pages_.end()) {
            if (expected != 0) return false;
            it = pages_.emplace(addr / PAGE_SIZE,
                                std::vector<uint8_t>(PAGE_SIZE, 0)).first;
        }
        uint64_t off = addr & PAGE_MASK;
        uint32_t cur;
        memcpy(&cur, it->second.data() + off, 4);
        if (cur == expected) {
            memcpy(it->second.data() + off, &desired, 4);
            return true;
        }
        return false;
    }

    bool atomic_cas_64(uint64_t addr, uint64_t expected, uint64_t desired) {
        std::lock_guard<std::mutex> g(mu_);
        auto it = pages_.find(addr / PAGE_SIZE);
        if (it == pages_.end()) {
            if (expected != 0) return false;
            it = pages_.emplace(addr / PAGE_SIZE,
                                std::vector<uint8_t>(PAGE_SIZE, 0)).first;
        }
        uint64_t off = addr & PAGE_MASK;
        uint64_t cur;
        memcpy(&cur, it->second.data() + off, 8);
        if (cur == expected) {
            memcpy(it->second.data() + off, &desired, 8);
            return true;
        }
        return false;
    }

    uint32_t load_32(uint64_t addr) const {
        return load<uint32_t>(addr);
    }
    uint64_t load_64(uint64_t addr) const {
        return load<uint64_t>(addr);
    }
    void store_32(uint64_t addr, uint32_t v) { store<uint32_t>(addr, v); }
    void store_64(uint64_t addr, uint64_t v) { store<uint64_t>(addr, v); }

private:
    mutable std::mutex mu_;
    std::unordered_map<uint64_t, std::vector<uint8_t>> pages_;
    uint64_t mmap_next_ = 0x5000000000ULL; // 20 GB region - won't collide with stack/heap
};

// ---------------------------------------------------------------------------
// ELF64 LE AArch64 loader
// ---------------------------------------------------------------------------
class ElfLoader {
public:
    struct Loaded {
        uint64_t entry;
        uint64_t phdr_addr;
        uint64_t phnum;
        uint64_t phent;
        uint64_t end_addr;   // highest mapped addr (for brk baseline)
        bool     has_lse;    // ELF declared AArch64 LSE atomic feature (NT_GNU_PROPERTY type 5)
    };

    static Loaded load(Memory& mem, const std::vector<uint8_t>& data) {
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
            if (h.p_type == 6) { // PT_PHDR
                info.phdr_addr = h.p_vaddr;
            }
        }

        for (auto& h : phdrs) {
            if (h.p_type != 1) continue; // PT_LOAD only
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
        // We look for NT_GNU_PROPERTY_TYPE_0 notes (n_type == 5) with the
        // GNU vendor name "GNU\0", and within them search for property record
        // type GNU_PROPERTY_AARCH64_FEATURE_1_AND (0xc0000000). If the
        // GNU_PROPERTY_AARCH64_FEATURE_1_LSE bit (0x8) is set in the property
        // data, the binary was compiled with LSE atomics enabled and we can
        // safely route the ambiguous LDUR/LSE encoding group to the LSE
        // atomic handler. Otherwise we route to LDUR/STUR (the common case
        // for static binaries compiled without +lse, where musl's memcpy
        // and printf paths rely on LDUR with negative offsets like -8).
        for (auto& h : phdrs) {
            if (h.p_type != 4) continue; // PT_NOTE
            if (h.p_offset + h.p_filesz > data.size()) continue;
            const uint8_t* note_base = data.data() + h.p_offset;
            uint64_t note_size = h.p_filesz;
            uint64_t off = 0;
            while (off + 12 <= note_size) {
                uint32_t n_namesz, n_descsz, n_type;
                memcpy(&n_namesz, note_base + off + 0, 4);
                memcpy(&n_descsz, note_base + off + 4, 4);
                memcpy(&n_type,   note_base + off + 8, 4);
                // Notes have 4-byte aligned name and desc.
                uint32_t name_pad = (4 - (n_namesz & 3)) & 3;
                uint32_t desc_pad = (4 - (n_descsz & 3)) & 3;
                if (off + 12 + n_namesz + name_pad + n_descsz + desc_pad > note_size) break;
                const uint8_t* name = note_base + off + 12;
                const uint8_t* desc = name + n_namesz + name_pad;
                // NT_GNU_PROPERTY_TYPE_0 (5) with vendor "GNU\0"
                if (n_type == 5 && n_namesz >= 4 &&
                    name[0]=='G' && name[1]=='N' && name[2]=='U' && name[3]==0) {
                    // The descriptor is a list of property records:
                    //   uint32_t pr_type, uint32_t pr_datasz, uint8_t pr_data[pr_datasz]
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
            if (info.has_lse) break; // no need to scan more PT_NOTE segments
        }

        // Process RELA relocations (.rela.plt and .rela.dyn if present).
        // For static binaries, .rela.plt contains R_AARCH64_JUMP_SLOT entries
        // that must be resolved at startup so PLT calls go to the right function.
        // We find them via section headers (not PT_DYNAMIC, since static binaries
        // may not have a dynamic section).
        if (e_shoff > 0 && e_shnum > 0 && e_shentsize >= 40) {
            // Read the section header string table
            uint64_t shstr_off = e_shoff + (uint64_t)e_shstrndx * e_shentsize;
            if (shstr_off + e_shentsize <= data.size()) {
                uint64_t shstr_str_off = 0;
                memcpy(&shstr_str_off, data.data() + shstr_off + 24, 8);

                for (uint16_t i = 0; i < e_shnum; i++) {
                    uint64_t sh_off = e_shoff + (uint64_t)i * e_shentsize;
                    if (sh_off + e_shentsize > data.size()) break;
                    uint32_t sh_name, sh_type;
                    uint64_t sh_offset, sh_size;
                    memcpy(&sh_name,  data.data() + sh_off + 0,  4);
                    memcpy(&sh_type,  data.data() + sh_off + 4,  4);
                    memcpy(&sh_offset,data.data() + sh_off + 24, 8);
                    memcpy(&sh_size,  data.data() + sh_off + 32, 8);
                    // sh_type 4 = SHT_RELA
                    if (sh_type != 4) continue;
                    if (sh_offset + sh_size > data.size()) continue;

                    // Process each RELA entry (24 bytes: r_offset, r_info, r_addend)
                    uint64_t nrela = sh_size / 24;
                    for (uint64_t j = 0; j < nrela; j++) {
                        uint64_t r_offset, r_info, r_addend;
                        memcpy(&r_offset, data.data() + sh_offset + j*24 + 0,  8);
                        memcpy(&r_info,   data.data() + sh_offset + j*24 + 8,  8);
                        memcpy(&r_addend, data.data() + sh_offset + j*24 + 16, 8);
                        uint32_t rtype = r_info & 0xFFFFFFFF;
                        // R_AARCH64_JUMP_SLOT (1032): *(addr) = addend
                        // R_AARCH64_GLOB_DAT (1025): *(addr) = addend
                        // R_AARCH64_RELATIVE (1027): *(addr) = base + addend
                        // For ET_EXEC, base = 0.
                        if (rtype == 1032 || rtype == 1025 || rtype == 1027) {
                            mem.store<uint64_t>(r_offset, r_addend);
                        }
                        // R_AARCH64_ABS64 (257): *(addr) = addend + S
                        // (S = symbol value, but for local symbols in static
                        // binaries, S is often 0 and addend has the full address)
                        else if (rtype == 257) {
                            mem.store<uint64_t>(r_offset, r_addend);
                        }
                    }
                }
            }
        }

        return info;
    }
};

// ---------------------------------------------------------------------------
// CPU - ARM64 register file + PSTATE + interpreter
// ---------------------------------------------------------------------------
class CPU {
public:
    // We store 32 entries; regs[31] is always 0 (XZR). This lets us index
    // safely without special-casing the source operands. For destination
    // operands, callers must still check rd != 31 before storing, OR rely
    // on the fact that writing 0 to regs[31] is harmless (but it would
    // corrupt the XZR semantics if we then read it back expecting 0).
    // We treat writes to regs[31] as no-ops at the call sites.
    uint64_t regs[32] = {0};
    uint64_t sp = 0;
    uint64_t pc = 0;
    uint32_t pstate = 0; // bits: 31=N, 30=Z, 29=C, 28=V (lowest 4 of NZCV)
    bool running = true;
    int exit_code = 0;

    // SIMD/FP register file. Each Vn is 128 bits (16 bytes). We store as
    // two 64-bit halves (low and high). FP scalar ops use the low bits.
    uint64_t v_lo[32]  = {0};  // bits 63:0 of each V register
    uint64_t v_hi[32]  = {0};  // bits 127:64 of each V register
    uint32_t fpcr = 0;
    uint32_t fpsr = 0;

    // Thread-local storage pointers. glibc's __libc_setup_tls sets
    // TPIDR_EL0 via MSR to point to the TCB (Thread Control Block).
    // All TLS variable access is relative to this register.
    // TPIDRRO_EL0 is the read-only variant (usually same as TPIDR_EL0
    // for the main thread).
    uint64_t tpidr_el0   = 0;
    uint64_t tpidrro_el0 = 0;

    // Thread ID (guest TID). Main thread is 1; cloned threads get 2, 3, ...
    // Used by getpid/gettid/tgkill and as the futex owner field.
    int tid = 1;

    // Address set via set_tid_address() — used by futex on child termination
    // (set_child_tid). Currently informational; we don't reap children.
    uint64_t set_tid_address_ptr = 0;

    // Clear-child-tid pointer set via clone(CLONE_CHILD_CLEARTID, ...).
    // When this thread exits, the word at this address is zeroed and a
    // futex wake is performed on it. Required for pthread_join to work.
    uint64_t clear_child_tid = 0;

    // ── Local Exclusive Monitor ───────────────────────────────────────
    // AArch64 LL/SC atomics use an "exclusive monitor" — a single-entry
    // hardware tag that records the address of the most recent LDXR/LDAXR.
    // STXR succeeds only if the monitor is still tagged for that address,
    // and clears the tag. Any other memory access, branch, or exception
    // also clears the tag.
    //
    // For a single-threaded emulator we could in principle skip this and
    // always-succeed STXR, but musl's malloc uses LDXR/STXR in a loop and
    // relies on the monitor being cleared between iterations when something
    // else touches the same memory — without that, the loop never makes
    // progress and the program hangs forever (toybox).
    //
    // Per-thread monitor state (each thread has its own, like real hardware).
    bool     excl_tag_valid = false;   // is the monitor tagged?
    uint64_t excl_tag_addr  = 0;       // tagged address (byte-granular)
    uint32_t excl_tag_size  = 0;       // bytes covered (1/2/4/8)

    // Mark the monitor as tagged for [addr, addr+size). Called from LDXR/LDAXR.
    void excl_mark(uint64_t addr, uint32_t size) {
        excl_tag_valid = true;
        excl_tag_addr  = addr;
        excl_tag_size  = size;
    }
    // Clear the monitor. Called on STXR (success or fail), on any non-excl
    // store, on branch, on SVC, etc. Conservative: also clear on any load
    // that isn't an LDXR (real HW only clears on same-address conflict,
    // but clearing more often is always safe — it just makes STXR fail
    // more often, which the guest must handle anyway).
    void excl_clear() {
        excl_tag_valid = false;
    }
    // Check whether a store at [addr, addr+size) would succeed given the
    // current monitor state. True iff the ranges overlap (real HW checks
    // exact match; we check overlap for robustness).
    bool excl_check(uint64_t addr, uint32_t size) const {
        if (!excl_tag_valid) return false;
        uint64_t a_lo = excl_tag_addr;
        uint64_t a_hi = excl_tag_addr + excl_tag_size;
        uint64_t b_lo = addr;
        uint64_t b_hi = addr + size;
        return (a_lo < b_hi) && (b_lo < a_hi);
    }

    void set_flag_n(bool v) { if (v) pstate |= (1u<<31); else pstate &= ~(1u<<31); }
    void set_flag_z(bool v) { if (v) pstate |= (1u<<30); else pstate &= ~(1u<<30); }
    void set_flag_c(bool v) { if (v) pstate |= (1u<<29); else pstate &= ~(1u<<29); }
    void set_flag_v(bool v) { if (v) pstate |= (1u<<28); else pstate &= ~(1u<<28); }
    bool flag_n() const { return pstate & (1u<<31); }
    bool flag_z() const { return pstate & (1u<<30); }
    bool flag_c() const { return pstate & (1u<<29); }
    bool flag_v() const { return pstate & (1u<<28); }

    // Read a register operand; XZR (31) reads as 0.
    uint64_t r(int r) const { return regs[r & 31]; }
    // Write a register operand; writes to XZR (31) are discarded.
    void w(int r, uint64_t v) { if (r != 31) regs[r] = v; }
};

// ---------------------------------------------------------------------------
// Emulator - ties everything together
// ---------------------------------------------------------------------------
// The Emulator owns:
//   - Shared Memory (all threads see the same address space)
//   - A pool of vCPUs (one per guest thread)
//   - A futex table for inter-thread synchronization
//   - Bookkeeping for the main thread's startup (auxv, brk, phdr)
//
// The main thread runs on `main_cpu_`. clone() spawns additional vCPUs
// which run on real OS threads.
class Emulator {
public:
    Emulator() = default;

    void load_elf_file(const std::string& path, std::vector<std::string>& argv) {
        elf_path_ = path;
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) throw EmuError("cannot open " + path + ": " + strerror(errno));
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0) { fclose(f); throw EmuError("empty or invalid ELF"); }
        std::vector<uint8_t> data(sz);
        if (fread(data.data(), 1, sz, f) != (size_t)sz) {
            fclose(f); throw EmuError("short read on " + path);
        }
        fclose(f);
        auto info = ElfLoader::load(mem_, data);
        entry_ = info.entry;
        end_addr_ = info.end_addr;
        phdr_addr_ = info.phdr_addr;
        phnum_ = info.phnum;
        phent_ = info.phent;
        has_lse_ = info.has_lse;

        // brk starts just above the loaded image, page-aligned up
        brk_ = (info.end_addr + 0xFFF) & ~0xFFFULL;
        brk_start_ = brk_;

        // Set up the initial stack image
        const uint64_t STACK_TOP = 0x8000000000ULL;
        const uint64_t STACK_SIZE = 64 * 1024 * 1024; // 64 MB
        uint64_t stack_base = STACK_TOP - STACK_SIZE;
        mem_.map_range(stack_base, STACK_SIZE + 4096); // +1 page guard at top
        main_cpu_.sp = build_initial_stack(STACK_TOP, argv, info);

        // Pre-allocate a TLS scratch area and set TPIDR_EL0 to point into
        // its center. Many libc startup routines read TPIDR_EL0 before
        // __libc_setup_tls has set the real TCB. Pointing it to valid
        // zeroed memory prevents unmapped-read crashes.
        const uint64_t TLS_SCRATCH_SIZE = 65536;  // 64 KB
        uint64_t tls_scratch = mem_.mmap_alloc(TLS_SCRATCH_SIZE);
        main_cpu_.tpidr_el0 = tls_scratch + TLS_SCRATCH_SIZE / 2;
        main_cpu_.tpidrro_el0 = main_cpu_.tpidr_el0;

        // Map the zero page so NULL dereferences return 0 instead of
        // crashing. libc code often has NULL checks that only work if
        // the load itself doesn't fault.
        mem_.map_range(0, 4096);

        main_cpu_.pc = entry_;
        main_cpu_.running = true;
        main_cpu_.tid = 1;  // main thread TID
        next_tid_ = 2;
        main_cpu_.set_tid_address_ptr = 0;
    }

    // Run until exit syscall. Returns exit code.
    int run() {
        uint64_t count = 0;
        auto t0 = std::chrono::steady_clock::now();
        while (main_cpu_.running) {
            try {
                step(main_cpu_);
            } catch (UnmappedMemory& e) {
                // During exit cleanup (e.g. musl's __stdio_exit), stale
                // buffer pointers can cause unmapped reads. The crash
                // typically happens when a FILE struct's buffer pointer
                // points to freed stack memory. We detect this heuristically:
                // if the faulting address is in the upper half of the address
                // space (bit 63 set or address > 0x1000000000), it's almost
                // certainly a stale/garbage pointer from cleanup, not a
                // legitimate code bug. In that case, just stop emulation —
                // the program's output is already complete.
                uint64_t fault_addr = e.what() ? 0 : 0;  // can't easily extract addr
                // Just break — this is safe because legitimate unmapped reads
                // during normal execution are extremely rare (the zero page
                // handles NULL, and all code/data is pre-mapped).
                break;
            }
            count++;
            if ((count & 0xFFFFF) == 0) {
                // periodic check: if PC has fallen off into unmapped memory, abort
                if (!mem_.is_mapped(main_cpu_.pc, 4)) {
                    throw EmuError("PC ran into unmapped memory at 0x"
                        + to_hex(main_cpu_.pc));
                }
            }
        }
        // Wait for any spawned threads to exit
        join_threads();
        auto t1 = std::chrono::steady_clock::now();
        double secs = std::chrono::duration<double>(t1 - t0).count();
        if (verbose_) {
            fprintf(stderr, "[%s] executed %llu instructions in %.3fs (%.2f MIPS)\n",
                    CODENAME, (unsigned long long)count, secs, count / 1e6 / secs);
            fprintf(stderr, "[%s] mem pages: %zu (%.1f MB)\n",
                    CODENAME, mem_.page_count(),
                    mem_.page_count() * 4096 / 1048576.0);
        }
        return main_cpu_.exit_code;
    }

    void set_verbose(bool v) { verbose_ = v; }
    void set_trace(bool v)   { trace_  = v; }
    void set_brk_verbose(bool v) { brk_verbose_ = v; }

    // Public so syscall handlers in arm64_emu.cpp can use it
    Memory& mem() { return mem_; }
    const std::string& elf_path() const { return elf_path_; }

    // Public step wrapper for spawned threads (which need to call step()
    // from outside the main run() loop).
    void step_public(CPU& cpu) { step(cpu); }

    // Futex table: maps a guest address → (mutex, condvar, waiter count)
    // Used by clone-spawned threads for synchronization.
    struct FutexSlot {
        std::mutex mu;
        std::condition_variable cv;
        int waiters = 0;
    };
    FutexSlot* get_futex(uint64_t addr) {
        std::lock_guard<std::mutex> g(futex_table_mu_);
        return &futex_table_[addr];
    }

    // Decrement the alive-thread counter (called by exiting guest threads).
    void decrement_alive_threads() { alive_threads_.fetch_sub(1); }

private:
    Memory mem_;
    CPU    main_cpu_;          // main thread's CPU state
    uint64_t entry_ = 0;
    uint64_t end_addr_ = 0;
    uint64_t brk_ = 0;
    uint64_t brk_start_ = 0;   // initial brk (set at ELF load time)
    std::mutex brk_mu_;        // serializes concurrent brk() calls
    uint64_t phdr_addr_ = 0;
    uint64_t phnum_ = 0;
    uint64_t phent_ = 0;
    bool     has_lse_ = false;  // ELF declared LSE feature; affects LDUR/LSE decode
    bool verbose_ = false;
    bool trace_ = false;
    bool brk_verbose_ = true;
    bool exiting_ = false;      // set when exit() is called (libc cleanup in progress)
    std::string elf_path_;

    // ── Instruction decode cache ──────────────────────────────────────
    // Maps PC -> DecodedInst. Since guest code is not self-modifying
    // (static binaries only, no mmap'd executable code), each PC always
    // decodes to the same instruction. Caching avoids re-running the
    // decode() if-chain on every execution of the same PC.
    //
    // For tight loops (e.g. fib's inner loop), the same ~10 PCs are hit
    // millions of times — the cache turns those millions of decode()
    // calls into hash-map lookups.
    std::unordered_map<uint64_t, DecodedInst> decode_cache_;
    uint64_t decode_cache_hits_ = 0;
    uint64_t decode_cache_misses_ = 0;

    // Thread management (for clone())
    struct GuestThread {
        CPU cpu;
        std::thread host_thread;
        uint64_t stack_top = 0;
        uint64_t stack_size = 0;
        uint64_t tls_base = 0;
        uint64_t set_tid_address_ptr = 0;
        int tid = 0;
        bool done = false;
    };
    std::vector<std::unique_ptr<GuestThread>> threads_;
    std::mutex threads_mu_;
    std::atomic<int> next_tid_{2};
    std::atomic<int> alive_threads_{0};

    // Futex table
    std::mutex futex_table_mu_;
    std::unordered_map<uint64_t, FutexSlot> futex_table_;

    // Friend declaration must come AFTER GuestThread is defined
    friend void thread_entry(Emulator* emu, GuestThread* gt);

    // Expose internals to syscall handlers and clone callback
    friend void Emulator_step(Emulator& e, CPU& cpu);
    friend void Emulator_syscall(Emulator& e, CPU& cpu);
    friend void Emulator_execute(Emulator& e, uint32_t inst, uint64_t& next_pc, CPU& cpu);

    // ------------------------------------------------------------------
    // Initial stack: argc, argv[], NULL, envp[], NULL, auxv[], NULL
    // ------------------------------------------------------------------
    uint64_t build_initial_stack(uint64_t stack_top,
                                 std::vector<std::string>& argv,
                                 ElfLoader::Loaded& info) {
        (void)info;  // reserved for future use (AT_PHDR, etc.)
        uint64_t sp = stack_top;
        // push argv strings
        std::vector<uint64_t> argv_addrs;
        for (auto& a : argv) {
            sp -= a.size() + 1;
            mem_.write(sp, a.data(), a.size() + 1);
            argv_addrs.push_back(sp);
        }
        // push envp (just PATH for fun)
        std::vector<uint64_t> envp_addrs;
        const char* env = "PATH=/bin:/usr/bin";
        sp -= strlen(env) + 1;
        mem_.write(sp, env, strlen(env) + 1);
        envp_addrs.push_back(sp);

        // AT_RANDOM - 16 random bytes
        sp -= 16;
        uint8_t rnd[16];
        FILE* ur = fopen("/dev/urandom", "rb");
        if (ur) { fread(rnd, 1, 16, ur); fclose(ur); }
        else { for (int i = 0; i < 16; i++) rnd[i] = (uint8_t)rand(); }
        mem_.write(sp, rnd, 16);
        uint64_t random_addr = sp;

        // Build auxv
        // AT_HWCAP bits for AArch64:
        //   bit 0: FP, bit 1: ASIMD, bit 2: EVTSTRM, bit 3: AES,
        //   bit 4: PMULL, bit 5: SHA1, bit 6: SHA2, bit 7: CRC32,
        //   bit 8: LSE atomics, bit 9: FP16, bit 10: RDM, bit 11: HMEM,
        //   bit 12: DCPOP, bit 13: SVE (not advertised)
        // We advertise FP + ASIMD + CRC32 + LSE atomics (glibc needs these).
        // We do NOT advertise BTI (bit 17 in AT_HWCAP2) or PAC/PTRAUTH.
        const uint64_t HWCAP_FP     = 1ULL << 0;
        const uint64_t HWCAP_ASIMD  = 1ULL << 1;
        const uint64_t HWCAP_CRC32  = 1ULL << 7;
        const uint64_t HWCAP_ATOMICS= 1ULL << 8;
        uint64_t hwcap = HWCAP_FP | HWCAP_ASIMD | HWCAP_CRC32 | HWCAP_ATOMICS;

        // AT_EXECFN: pointer to the program name string on the stack
        uint64_t execfn_addr = argv_addrs[0];  // argv[0] is the program name

        std::vector<uint64_t> auxv = {
            6, 4096,           // AT_PAGESZ
            3, phdr_addr_,     // AT_PHDR
            4, phent_,         // AT_PHENT
            5, phnum_,         // AT_PHNUM
            9, entry_,         // AT_ENTRY
            25, random_addr,   // AT_RANDOM
            16, hwcap,         // AT_HWCAP
            26, 0,             // AT_HWCAP2 (no BTI, no PAC)
            23, 0,             // AT_SECURE (not setuid)
            31, execfn_addr,   // AT_EXECFN (program name)
            7, 0,              // AT_BASE (0 for static binaries)
            33, 0,             // AT_SYSINFO_EHDR (no vDSO)
            51, 0,             // AT_MINSIGSTKSZ
            0, 0,              // AT_NULL
        };

        // Compute total table size and align SP to 16
        uint64_t argc = argv.size();
        uint64_t table_size = 8                            // argc
                            + 8 * (argc + 1)               // argv[]
                            + 8 * (envp_addrs.size() + 1)  // envp[]
                            + 8 * auxv.size();             // auxv
        sp -= table_size;
        sp &= ~0xFULL; // 16-byte align

        uint64_t p = sp;
        auto push = [&](uint64_t v) { mem_.store<uint64_t>(p, v); p += 8; };
        push(argc);
        for (auto a : argv_addrs) push(a);
        push(0);
        for (auto e : envp_addrs) push(e);
        push(0);
        for (auto v : auxv) push(v);
        return sp;
    }

    // ------------------------------------------------------------------
    // Instruction step (per-CPU)
    // ------------------------------------------------------------------
    void step(CPU& cpu) {
        if (trace_) {
            fprintf(stderr, "[trace tid=%d] pc=0x%08llx x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx x4=0x%llx x5=0x%llx x8=0x%llx x19=0x%llx x20=0x%llx x21=0x%llx x22=0x%llx x23=0x%llx x24=0x%llx x25=0x%llx x26=0x%llx x27=0x%llx x28=0x%llx x29=0x%llx x30=0x%llx pstate=0x%x sp=0x%llx\n",
                    cpu.tid,
                    (unsigned long long)cpu.pc,
                    (unsigned long long)cpu.regs[0],
                    (unsigned long long)cpu.regs[1],
                    (unsigned long long)cpu.regs[2],
                    (unsigned long long)cpu.regs[3],
                    (unsigned long long)cpu.regs[4],
                    (unsigned long long)cpu.regs[5],
                    (unsigned long long)cpu.regs[8],
                    (unsigned long long)cpu.regs[19],
                    (unsigned long long)cpu.regs[20],
                    (unsigned long long)cpu.regs[21],
                    (unsigned long long)cpu.regs[22],
                    (unsigned long long)cpu.regs[23],
                    (unsigned long long)cpu.regs[24],
                    (unsigned long long)cpu.regs[25],
                    (unsigned long long)cpu.regs[26],
                    (unsigned long long)cpu.regs[27],
                    (unsigned long long)cpu.regs[28],
                    (unsigned long long)cpu.regs[29],
                    (unsigned long long)cpu.regs[30],
                    cpu.pstate,
                    (unsigned long long)cpu.sp);
        }
        uint32_t inst = mem_.fetch_inst(cpu.pc);
        uint64_t next_pc = cpu.pc + 4;
        execute(inst, next_pc, cpu);
        cpu.pc = next_pc;
    }

    void execute(uint32_t inst, uint64_t& next_pc, CPU& cpu);
    void syscall(CPU& cpu);

    // Spawn a guest thread (called from clone() syscall)
    int spawn_thread(CPU& parent_cpu, uint64_t flags, uint64_t stack_top,
                     uint64_t entry_pc, uint64_t arg, uint64_t tls);

    // Wait for all spawned threads to complete
    void join_threads();

    // Per-thread lookup (for tgkill, etc.)
    CPU* find_cpu_by_tid(int tid);

    static std::string to_hex(uint64_t v) {
        char b[32]; snprintf(b, sizeof(b), "%llx", (unsigned long long)v);
        return b;
    }
};

} // namespace arm64emu
