// Guest process startup: default environment, vDSO image and the initial stack.
#include "System/Core/Emulator.hpp"
#include "System/Core/DebugFlags.hpp"
#include "Bifrost/Version.hpp"
#include "System/Core/CrashReport.hpp"
#include "System/Memory/Memory.hpp"
#include "System/Loader/DynamicLinker.hpp"
#include "System/Loader/VdsoBytes.h"      // embedded AArch64 vDSO
#include "System/Graphics/Thunks/GraphicThunk.hpp"        // GraphicThunk full definition (for init/resolve)
#include "System/Android/Surface/Surface.hpp"// 2026-08: Android activity
#include "System/Audio/AudioThunk.hpp"  // AudioThunk
#include "System/Graphics/Thunks/DisplayThunk.hpp"// DisplayThunk
#include "System/VFS/Nodes/HostNode.hpp"  // wl fd publisher wraps duped host fds
#include <fcntl.h>
#include <unistd.h>
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>
namespace arm64emu {
// ── Default guest environment ──────────────────────────────────────────
// Builds a minimal but realistic environment for the guest, propagating
// locale/timezone-related host env vars so programs like `toybox uptime`,
// `date`, `ls` display correct local time and language conventions.
//
// Propagated vars (if set on host):
//   TZ, LANG, LC_ALL, LC_CTYPE, LC_NUMERIC, LC_TIME, LC_COLLATE,
//   LC_MONETARY, LC_MESSAGES, LC_PAPER, LC_NAME, LC_ADDRESS,
//   LC_TELEPHONE, LC_MEASUREMENT, LC_IDENTIFICATION,
//   LESSCHARSET, LESSUTFCHARDEF, COLORTERM, COLORFGBG
//
// NOT propagated (sandbox/security):
//   LD_PRELOAD, LD_LIBRARY_PATH (would break the emulated loader),
//   BIFROST_* (emulator-internal flags must not leak to guest).
std::vector<std::string> Emulator::build_default_guest_env() {
    std::vector<std::string> envs;
    // Core environment every guest needs.
    envs.push_back("PATH=/tmp/aarch64-bin:/bin:/usr/bin:/sbin:/usr/sbin");
    envs.push_back("HOME=/root");
    envs.push_back("SHELL=/bin/sh");
    envs.push_back("TERM=linux");
    envs.push_back("PWD=/");
    envs.push_back("SHLVL=1");
    envs.push_back("_=/bin/sh");
    // Propagate locale + timezone vars from the host so locale-aware
    // programs (date, uptime, ls -l's month names, etc.) work correctly.
    // Without TZ, musl defaults to UTC and `toybox uptime` shows UTC time
    // instead of the user's local time.
    static const char* const propagate[] = {
        "TZ",
        "LANG",
        "LC_ALL",
        "LC_CTYPE",
        "LC_NUMERIC",
        "LC_TIME",
        "LC_COLLATE",
        "LC_MONETARY",
        "LC_MESSAGES",
        "LC_PAPER",
        "LC_NAME",
        "LC_ADDRESS",
        "LC_TELEPHONE",
        "LC_MEASUREMENT",
        "LC_IDENTIFICATION",
        "LESSCHARSET",
        "LESSUTFCHARDEF",
        "COLORTERM",
        "COLORFGBG",
        "PAGER",
        "EDITOR",
        "VISUAL",
        // GUI/graphics-adjacent vars. Propagating DISPLAY lets guest X11/Qt
        // (xcb) clients reach a host X server; the QT_* vars select the Qt
        // platform plugin and its search path (offscreen/xcb/eglfs).
        "DISPLAY",
        "XAUTHORITY",
        // Propagate the host D-Bus session so guest Qt apps can reach the
        // session bus. Without this, Qt's AT-SPI accessibility bridge
        // (activated during QWidget::show) cannot find the bus and blocks
        // forever on its connection-ready semaphore.
        "DBUS_SESSION_BUS_ADDRESS",
        "XDG_RUNTIME_DIR",
        "QT_QPA_PLATFORM",
        "QT_QPA_PLATFORM_PLUGIN_PATH",
        "QT_PLUGIN_PATH",
        "QT_QPA_FONTDIR",
        "QT_DEBUG_PLUGINS",
        "QT_X11_NO_MITSHM",
        "QT_ACCESSIBILITY",
        "QT_LINUX_ACCESSIBILITY_ALWAYS_ON",
        nullptr,
    };
    for (size_t i = 0; propagate[i]; ++i) {
        const char* v = getenv(propagate[i]);
        if (v && v[0] != '\0') {
            envs.push_back(std::string(propagate[i]) + "=" + v);
        }
    }
    // BIFROST_GUEST_ENV="KEY=VAL;KEY2=VAL2": debug passthrough so host env
    // vars the curated list doesn't cover (e.g. MIMALLOC_*) can reach the
    // guest. Split on ';'.
    if (const char* extra = getenv("BIFROST_GUEST_ENV")) {
        std::string s(extra);
        size_t pos = 0;
        while (pos <= s.size()) {
            size_t semi = s.find(';', pos);
            std::string kv = s.substr(pos, semi == std::string::npos
                                                ? std::string::npos
                                                : semi - pos);
            if (!kv.empty()) envs.push_back(kv);
            if (semi == std::string::npos) break;
            pos = semi + 1;
        }
    }
    return envs;
}
// ── vDSO loader ───────────────────────────────────────
// Maps the embedded AArch64 vDSO ELF into guest memory and returns the
// base address (the ELF header location = AT_SYSINFO_EHDR). The vDSO
// provides gettimeofday/clock_gettime/clock_getres/__kernel_rt_sigreturn
// stubs that trap to the emulator's syscall handler via svc #0.
uint64_t Emulator::load_vdso() {
    if (vdso_base_ != 0) return vdso_base_;  // already loaded
    const uint8_t* data = vdso_so_bytes;
    size_t size = vdso_so_len;
    if (size < 64) return 0;
    if (data[0] != 0x7f || data[1] != 'E' || data[2] != 'L' || data[3] != 'F')
        return 0;
    uint64_t e_phoff;
    uint16_t e_phentsize, e_phnum;
    memcpy(&e_phoff, data + 32, 8);
    memcpy(&e_phentsize, data + 54, 2);
    memcpy(&e_phnum, data + 56, 2);
    if (e_phoff == 0 || e_phnum == 0 || e_phentsize < 56) return 0;
    uint64_t max_vaddr_end = 0;
    struct Phdr { uint32_t p_type; uint32_t p_flags; uint64_t p_offset,
        p_vaddr, p_paddr, p_filesz, p_memsz, p_align; };
    std::vector<Phdr> loads;
    for (int i = 0; i < e_phnum; i++) {
        if (e_phoff + i * e_phentsize + 56 > size) break;
        Phdr h;
        memcpy(&h, data + e_phoff + i * e_phentsize, 56);
        if (h.p_type == 1) {  // PT_LOAD
            loads.push_back(h);
            uint64_t end = h.p_vaddr + h.p_memsz;
            if (end > max_vaddr_end) max_vaddr_end = end;
        }
    }
    if (loads.empty() || max_vaddr_end == 0) return 0;
    max_vaddr_end = (max_vaddr_end + 0xFFF) & ~0xFFFULL;
    uint64_t base = mem_.mmap_alloc(max_vaddr_end);
    if (base == 0) return 0;
    size_t hdr_size = std::min<size_t>(e_phoff + e_phnum * e_phentsize, size);
    try {
        mem_.write(base, data, hdr_size);
    } catch (...) { return 0; }
    for (const auto& h : loads) {
        if (h.p_filesz == 0) continue;
        if (h.p_offset + h.p_filesz > size) continue;
        try {
            mem_.write(base + h.p_vaddr, data + h.p_offset,
                       static_cast<size_t>(h.p_filesz));
        } catch (...) { return 0; }
    }
    for (const auto& h : loads) {
        if (h.p_memsz == 0) continue;
        const uint64_t seg_addr = base + h.p_vaddr;
        const uint64_t seg_end = seg_addr + h.p_memsz;
        if (seg_end < seg_addr || seg_end > UINT64_MAX - Memory::PAGE_MASK)
            return 0;
        const uint64_t page_start = seg_addr & ~Memory::PAGE_MASK;
        const uint64_t page_end = (seg_end + Memory::PAGE_MASK) & ~Memory::PAGE_MASK;
        const uint8_t prot = ((h.p_flags & 4) ? Memory::GUEST_PROT_READ : 0) |
                             ((h.p_flags & 2) ? Memory::GUEST_PROT_WRITE : 0) |
                             ((h.p_flags & 1) ? Memory::GUEST_PROT_EXEC : 0);
        if (!mem_.mprotect_guest(page_start, page_end - page_start, prot))
            return 0;
    }
    vdso_base_ = base;
    vdso_size_ = max_vaddr_end;
    if (dbg().dynlink_trace) {
        fprintf(stderr, "[vdso] loaded at 0x%llx (size=%llu, %zu segments)\n",
                static_cast<unsigned long long>(base),
                static_cast<unsigned long long>(max_vaddr_end),
                loads.size());
    }
    return base;
}
// Strings ascend as Linux fs/exec.c copies them: argv[0] lowest, the argv
// block, then the environment, ending at stack_top. Android's app_main
// sizes argv as one contiguous block and Process.setArgV0 rewrites it;
// /proc/self/cmdline reports [arg_start_, arg_end_) live.
uint64_t Emulator::place_argument_strings(uint64_t stack_top,
                                          const std::vector<std::string>& argv,
                                          const std::vector<std::string>& env,
                                          std::vector<uint64_t>& argv_addrs,
                                          std::vector<uint64_t>& envp_addrs) {
    uint64_t size = 0;
    for (const auto& a : argv) size += a.size() + 1;
    for (const auto& e : env) size += e.size() + 1;
    uint64_t cursor = stack_top - size;
    const uint64_t start = cursor;
    auto place = [&](const std::string& value) {
        mem_.write(cursor, value.c_str(), value.size() + 1);
        cursor += value.size() + 1;
        return cursor - value.size() - 1;
    };
    argv_addrs.clear();
    envp_addrs.clear();
    arg_start_ = cursor;
    for (const auto& a : argv) argv_addrs.push_back(place(a));
    arg_end_ = cursor;
    for (const auto& e : env) envp_addrs.push_back(place(e));
    return start;
}
// ── Initial stack: argc, argv[], NULL, envp[], NULL, auxv[], NULL ─────
uint64_t Emulator::build_initial_stack(uint64_t stack_top,
                                       std::vector<std::string>& argv,
                                       ElfLoader::Loaded& info) {
    (void)info;  // reserved for future use (AT_PHDR, etc.)
    // If guest_env_ was not set via set_guest_env(), build a default
    // environment that propagates locale/timezone-related host env vars
    // (TZ, LANG, LC_*, etc.) so programs like `toybox uptime`, `date`,
    // `ls` display correct local time and language conventions.
    if (guest_env_.empty()) {
        guest_env_ = build_default_guest_env();
    }
    std::vector<uint64_t> argv_addrs, envp_addrs;
    uint64_t sp = place_argument_strings(stack_top, argv, guest_env_, argv_addrs, envp_addrs);
    // AT_RANDOM — 16 random bytes used by glibc for stack canary init and
    // pointer guard. MUST be high-quality random; never fall back to rand()
    // (which is unseeded by default → predictable canary → stack-overflow
    // exploits in the guest become trivial).
    sp -= 16;
    uint8_t rnd[16];
    bool got_random = false;
    // Prefer the host kernel's getrandom(2) (no fd needed, never blocks
    // after boot, doesn't fail under seccomp unless explicitly blocked).
#ifdef SYS_getrandom
    long gr = ::syscall(SYS_getrandom, rnd, 16, 0);
    if (gr == 16) {
        got_random = true;
    }
#endif
    if (!got_random) {
        FILE* ur = fopen("/dev/urandom", "rb");
        if (ur) {
            size_t nread = fread(rnd, 1, 16, ur);
            fclose(ur);
            if (nread == 16) got_random = true;
        }
    }
    if (!got_random) {
        // Last-resort fallback: mix a stack address (ASLR'd) with the
        // monotonic clock. Better than rand() but still not great.
        uint64_t mix = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&rnd))
                     ^ static_cast<uint64_t>(std::chrono::steady_clock::now()
                                              .time_since_epoch().count());
        for (int i = 0; i < 16; i += 8) {
            uint64_t v = mix;
            // Simple LCG mixing
            v = v * 6364136223846793005ULL + 1442695040888963407ULL;
            mix ^= v;
            memcpy(rnd + i, &mix, 8);
        }
    }
    mem_.write(sp, rnd, 16);
    uint64_t random_addr = sp;
    // AT_HWCAP bits for AArch64:
    //   bit 0: FP, bit 1: ASIMD, bit 7: CRC32, bit 8: LSE atomics.
    // We advertise FP + ASIMD + CRC32 + LSE atomics (glibc needs these).
    // We do NOT advertise BTI or PAC/PTRAUTH.
    const uint64_t HWCAP_FP      = 1ULL << 0;
    const uint64_t HWCAP_ASIMD   = 1ULL << 1;
    const uint64_t HWCAP_CRC32   = 1ULL << 7;
    const uint64_t HWCAP_ATOMICS = 1ULL << 8;
    uint64_t hwcap = HWCAP_FP | HWCAP_ASIMD | HWCAP_CRC32 | HWCAP_ATOMICS;
    // AT_EXECFN: pointer to the program name string on the stack.
    // argc==0 is permitted by the C API (argv empty) — indexing [0] would
    // be UB, so synthesize an empty string when there is no argv0.
    uint64_t execfn_addr;
    if (!argv_addrs.empty()) {
        execfn_addr = argv_addrs[0];
    } else {
        sp -= 1;
        const char empty[1] = {'\0'};
        mem_.write(sp, empty, 1);
        execfn_addr = sp;
    }
    // AT_PLATFORM: aarch64 string (some libc ifunc resolvers consult it).
    sp -= 8;
    const char platform_str[] = "aarch64";
    mem_.write(sp, platform_str, sizeof(platform_str));
    uint64_t platform_addr = sp;
    std::vector<uint64_t> auxv = {
        6,  Memory::PAGE_SIZE, // AT_PAGESZ: runtime granule, not ELF p_align
        3,  phdr_addr_,      // AT_PHDR
        4,  phent_,          // AT_PHENT
        5,  phnum_,          // AT_PHNUM
        9,  prog_entry_,     // AT_ENTRY (original binary's entry, not interp's)
        25, random_addr,     // AT_RANDOM
        16, hwcap,           // AT_HWCAP
        26, 0,               // AT_HWCAP2 (no BTI, no PAC)
        23, 0,               // AT_SECURE (not setuid)
        31, execfn_addr,     // AT_EXECFN (program name)
        7,  interp_base_,    // AT_BASE (interpreter load address, 0 if static)
        33, vdso_base_,     // AT_SYSINFO_EHDR (vDSO ELF header address)
        // BUGFIX: AT_MINSIGSTKSZ was 0, which glibc uses to size altstacks.
        // A zero value can cause glibc to allocate an undersized altstack
        // and overflow into unmapped memory in signal handlers. The kernel
        // reports ~6 KiB on AArch64; we use the same value.
        51, 6144,            // AT_MINSIGSTKSZ
        // BUGFIX: missing AT_UID/AT_EUID/AT_GID/AT_EGID/AT_PLATFORM/AT_CLKTCK.
        // glibc reads these in __libc_start_main / _dl_aux_init; musl reads
        // AT_UID/AT_EUID to set the getuid/geteuid caches. AT_CLKTCK backs
        // sysconf(_SC_CLK_TCK); without it, sysconf returns -1 and tools
        // like top/ps miscompute CPU%. Android uses the installed-app
        // credentials; the Linux profile retains its legacy auxiliary IDs.
        11, vfs_.process_uid(), // AT_UID
        12, vfs_.process_uid(), // AT_EUID
        13, vfs_.process_gid(), // AT_GID
        14, vfs_.process_gid(), // AT_EGID
        15, platform_addr,   // AT_PLATFORM ("aarch64")
        17, 100,             // AT_CLKTCK (sysconf(_SC_CLK_TCK))
        0,  0,               // AT_NULL
    };
    // Compute total table size and align SP to 16
    uint64_t argc = argv.size();
    uint64_t table_size = 8                            // argc
                        + 8 * (argc + 1)               // argv[]
                        + 8 * (envp_addrs.size() + 1)  // envp[]
                        + 8 * auxv.size();             // auxv
    sp -= table_size;
    sp &= ~0xFULL;  // 16-byte align
    uint64_t p = sp;
    auto push = [&](uint64_t v) { mem_.store<uint64_t>(p, v); p += 8; };
    push(argc);
    for (auto a : argv_addrs) push(a);
    push(0);
    // Record the guest address of the envp array so the native dynlink
    // path can point glibc's __environ at it (ld.so never runs to do it).
    guest_envp_addr_ = p;
    for (auto e : envp_addrs) push(e);
    push(0);
    for (auto v : auxv) push(v);
    vfs_.set_initial_stack(sp);
    return sp;
}
}  // namespace arm64emu
