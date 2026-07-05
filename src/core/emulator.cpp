// core/emulator.cpp — Emulator lifecycle, ELF loading, run loop.
//
// This file implements the high-level "drive the emulator" logic:
//   - Constructor / destructor
//   - load_elf_file() — load an ELF, set up the initial stack + TLS
//   - run() — main interpreter/JIT dispatch loop with hang watchdog,
//     periodic signal draining, and SDL2 refresh hooks
//   - step() — decode-cache-aware single-instruction step (with tracing)
//   - Host signal handlers (SIGINT/SIGTERM/SIGCHLD/SIGWINCH → guest)
//
// The per-instruction execute() body lives in src/interp/interpreter.cpp.
// The syscall() body lives in src/syscalls/syscalls.cpp. Thread spawn/join
// lives in src/core/thread_mgr.cpp. Each is a friend of Emulator.
#include "core/emulator.h"

#include "bifrost/version.hpp"
#include "core/memory.h"
#include "frontend/dynamic_linker.h"
#include "frost/thunk.hpp"  // GraphicThunk full definition (for init/resolve)
#include "jit/frostjit.hpp"

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

// ── Static active-emu pointer (defined in src/core/signal.cpp) ────────
// Emulator* Emulator::g_active_emu_ = nullptr;  // defined in signal.cpp

Emulator::Emulator() = default;
Emulator::~Emulator() = default;

void* Emulator::excl_monitor_shard_pub(uint64_t addr) {
    return &excl_monitor_shards_[excl_shard_idx(addr)];
}

// ── ELF loading ───────────────────────────────────────────────────────
void Emulator::load_elf_file(const std::string& path, std::vector<std::string>& argv) {
    elf_path_ = path;
    vfs_.set_elf_path(path);
    vfs_.set_argv(argv);
    vfs_.set_graphics(&graphics_);
    vfs_.set_audio(&audio_);

    // Wire up /proc/self/maps to query live memory state. The callback
    // captures `this` — the Emulator outlives the VFS, so this is safe.
    // BUGFIX: previously /proc/self/maps returned hardcoded 5-line string
    // that didn't reflect actual guest memory layout. Now we emit real
    // entries: ELF load range, brk (heap), stack, mmap region, and
    // dynamic linker range.
    vfs_.set_maps_provider([this]() {
        std::vector<yggdrasil::Yggdrasil::MapEntry> out;
        // ELF image: from end_addr_ min down to lowest PT_LOAD start.
        // We don't track the lowest PT_LOAD start, so use end_addr_ as
        // the upper bound and 0x400000 (typical PIE/static base) as the
        // lower bound heuristic. Conservative: covers all code/data.
        if (end_addr_ > 0) {
            yggdrasil::Yggdrasil::MapEntry e;
            e.start = 0x400000;
            e.end   = end_addr_;
            std::snprintf(e.perms, sizeof(e.perms), "rwxp");
            out.push_back(e);
        }
        // Heap (brk): from brk_start_ to brk_.
        if (brk_start_ > 0 && brk_ >= brk_start_) {
            yggdrasil::Yggdrasil::MapEntry e;
            e.start = brk_start_;
            e.end   = brk_;
            std::snprintf(e.perms, sizeof(e.perms), "rw-p");
            e.label = "[heap]";
            out.push_back(e);
        }
        // Dynamic linker range.
        if (interp_base_ > 0) {
            yggdrasil::Yggdrasil::MapEntry e;
            e.start = interp_base_;
            e.end   = interp_base_ + 0x10000000;  // 256 MiB upper bound
            std::snprintf(e.perms, sizeof(e.perms), "rwxp");
            e.label = "[interp]";
            out.push_back(e);
        }
        // All mmap_alloc'd regions (excluding the heap which is above).
        for (const auto& kv : mem_.allocations_snapshot()) {
            uint64_t a = kv.first, s = kv.second;
            // Skip the heap region (already emitted above).
            if (a == brk_start_) continue;
            // Skip the TLS scratch area near brk_start_ (it's part of the
            // mmap region but we want to show it as anon).
            yggdrasil::Yggdrasil::MapEntry e;
            e.start = a;
            e.end   = a + s;
            std::snprintf(e.perms, sizeof(e.perms), "rw-p");
            out.push_back(e);
        }
        // Stack: fixed 64 MiB at 0x8000000000 - 64 MiB.
        {
            yggdrasil::Yggdrasil::MapEntry e;
            e.start = 0x8000000000ULL - 64 * 1024 * 1024;
            e.end   = 0x8000000000ULL;
            std::snprintf(e.perms, sizeof(e.perms), "rw-p");
            e.label = "[stack]";
            out.push_back(e);
        }
        return out;
    });

    // Wire up guest cwd tracking. The host cwd is meaningless because
    // BIFROST_ROOT sandboxing decouples guest paths from host paths.
    // The guest starts at "/" by default; chdir/fchdir update this.
    vfs_.set_cwd_provider(
        [this]() -> std::string { return guest_cwd_; },
        [this](const std::string& p) -> bool {
            // Resolve relative paths against the current cwd.
            if (p.empty()) return false;
            if (p[0] == '/') {
                guest_cwd_ = p;
            } else {
                if (guest_cwd_.empty()) guest_cwd_ = "/";
                if (guest_cwd_.back() == '/') guest_cwd_.pop_back();
                guest_cwd_ += "/";
                guest_cwd_ += p;
            }
            // Normalize: collapse "." and ".." segments.
            std::vector<std::string> parts;
            std::stringstream ss(guest_cwd_);
            std::string seg;
            while (std::getline(ss, seg, '/')) {
                if (seg.empty() || seg == ".") continue;
                if (seg == "..") {
                    if (!parts.empty()) parts.pop_back();
                } else {
                    parts.push_back(seg);
                }
            }
            guest_cwd_ = "/";
            for (size_t i = 0; i < parts.size(); i++) {
                guest_cwd_ += parts[i];
                if (i + 1 < parts.size()) guest_cwd_ += "/";
            }
            return true;
        });
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) throw EmuError("cannot open " + path + ": " + strerror(errno));
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); throw EmuError("empty or invalid ELF"); }
    std::vector<uint8_t> data(sz);
    if (fread(data.data(), 1, sz, f) != static_cast<size_t>(sz)) {
        fclose(f); throw EmuError("short read on " + path);
    }
    fclose(f);
    auto info = ElfLoader::load(mem_, data);
    entry_ = info.entry;
    prog_entry_ = info.entry;  // save original entry for AT_ENTRY
    end_addr_ = info.end_addr;
    phdr_addr_ = info.phdr_addr;
    phnum_ = info.phnum;
    phent_ = info.phent;
    has_lse_ = info.has_lse;
    interp_base_ = 0;

    // If the binary has a PT_INTERP (dynamic linker), load it.
    // The dynamic linker's entry point becomes the real entry point;
    // the binary's entry is passed via AT_ENTRY in auxv.
    //
    // Two paths are supported:
    //   (a) If BIFROST_NATIVE_DYNLINK=1, we use our own DynamicLinker
    //       to process DT_NEEDED, apply relocations, and resolve
    //       symbols — no guest-side ld.so needed. Faster and works
    //       even when the host doesn't have the exact aarch64 ld.so.
    //   (b) Otherwise, load the guest-side dynamic linker (ld-musl /
    //       ld-linux) and let it run its own code to do dynamic linking.
    //       This works for simple dynamically-linked musl binaries but
    //       not yet for glibc.
    uint64_t interp_base = 0;
    if (!info.interp.empty()) {
        // First, try the native (in-emulator) dynamic linker. This is
        // faster and more reliable because we don't depend on the
        // guest-side ld.so working correctly under emulation.
        // Default: use the native (in-emulator) dynamic linker. It's faster
        // and more reliable than depending on the guest-side ld.so under
        // emulation. Set BIFROST_NO_NATIVE_DYNLINK=1 to fall back to the
        // guest-side ld.so (for debugging).
        bool native_dynlink = (getenv("BIFROST_NO_NATIVE_DYNLINK") == nullptr);
        if (native_dynlink) {
            dyn_linker_ = std::make_unique<DynamicLinker>(mem_);

            // ── Wire GraphicThunk into the dynamic linker (Turn 37) ──
            // When BIFROST_THUNK_GRAPHICS=1 is set, the thunk forwards
            // guest GL/EGL/SDL2 calls to the host. We init the thunk
            // (allocates the guest trampoline page) and register a
            // resolver callback so the dynamic linker can populate the
            // global symbol table for graphic libraries that couldn't
            // be loaded from disk.
            //
            // We always wire the resolver (even if the thunk is
            // disabled) — when disabled, enumerate_symbols() returns 0
            // and the dynamic linker falls through to its existing
            // "library not found" path. This keeps the code path
            // uniform.
            if (auto* thunk = graphics_.thunk()) {
                if (thunk->enabled()) {
                    thunk->init(mem_);
                }
                // Capture the thunk pointer (not `this`) so the
                // callback doesn't depend on the Emulator's lifetime
                // beyond the thunk's. The thunk is owned by graphics_,
                // which is owned by the Emulator — they have the same
                // lifetime.
                GraphicThunk* thunk_ptr = thunk;
                dyn_linker_->set_thunk_resolver(
                    [thunk_ptr](const std::string& lib)
                        -> DynamicLinker::ThunkSymbolList {
                        DynamicLinker::ThunkSymbolList out;
                        if (!thunk_ptr->enabled()) return out;
                        thunk_ptr->enumerate_symbols(lib,
                            [&](const std::string& sym, uint64_t addr) {
                                out.emplace_back(sym, addr);
                            });
                        return out;
                    });
            }

            // Register the ifunc resolver callback. BUGFIX: the old
            // IRELATIVE handler just stored the resolver ADDRESS instead
            // of CALLING it. Now we run the resolver in a scratch CPU
            // state (borrowing main_cpu_, which hasn't been initialized
            // yet at this point in load_elf_file — its real init happens
            // later). The resolver is a small guest function that
            // returns a function pointer in X0; we run it via step
            // until it RETs to a sentinel address, then capture X0.
            dyn_linker_->set_ifunc_resolver([this](uint64_t resolver_addr) -> uint64_t {
                if (resolver_addr == 0) return 0;
                // Borrow main_cpu_ as scratch. Save its current state so
                // we don't disturb the (still-default) init.
                CPU saved = main_cpu_;
                // Allocate a small scratch stack for the resolver (4 KiB
                // is plenty — resolvers are leaf-ish functions that don't
                // recurse deeply).
                uint64_t scratch_stack = mem_.mmap_alloc(4096);
                uint64_t stack_top = scratch_stack + 4096;
                // Sentinel return address — when PC == this, the resolver
                // has RET'd. Use 0x1000 (in the zero page, unmapped for
                // execution but a recognizable sentinel).
                constexpr uint64_t SENTINEL_LR = 0x1000;
                main_cpu_.pc = resolver_addr;
                main_cpu_.sp = stack_top;
                main_cpu_.regs[30] = SENTINEL_LR;  // LR
                main_cpu_.running = true;
                main_cpu_.pstate = 0;
                // Run the resolver. Cap at 1M instructions to avoid
                // infinite loops in buggy resolvers.
                constexpr uint64_t IRESOLVER_LIMIT = 1'000'000;
                uint64_t steps = 0;
                try {
                    while (main_cpu_.running && main_cpu_.pc != SENTINEL_LR
                           && steps < IRESOLVER_LIMIT) {
                        step(main_cpu_);
                        steps++;
                    }
                } catch (const std::exception& e) {
                    if (getenv("BIFROST_IFUNC_TRACE")) {
                        fprintf(stderr, "[%s] ifunc resolver at 0x%llx threw: %s\n",
                                CODENAME,
                                static_cast<unsigned long long>(resolver_addr),
                                e.what());
                    }
                    main_cpu_ = saved;
                    return 0;
                }
                uint64_t result = main_cpu_.regs[0];
                if (steps >= IRESOLVER_LIMIT) {
                    if (getenv("BIFROST_IFUNC_TRACE")) {
                        fprintf(stderr, "[%s] ifunc resolver at 0x%llx ran >%llu "
                                "instructions; aborting (likely infinite loop)\n",
                                CODENAME,
                                static_cast<unsigned long long>(resolver_addr),
                                static_cast<unsigned long long>(IRESOLVER_LIMIT));
                    }
                    result = 0;
                }
                if (getenv("BIFROST_IFUNC_TRACE")) {
                    fprintf(stderr, "[%s] ifunc resolver at 0x%llx returned 0x%llx "
                            "(steps=%llu)\n",
                            CODENAME,
                            static_cast<unsigned long long>(resolver_addr),
                            static_cast<unsigned long long>(result),
                            static_cast<unsigned long long>(steps));
                }
                // Restore main_cpu_ to its pre-resolver state.
                main_cpu_ = saved;
                return result;
            });
            if (!dyn_linker_->link(data, info.base_addr, path)) {
                fprintf(stderr, "[%s] native dynamic linking failed: %s; "
                        "falling back to guest ld.so\n",
                        CODENAME, dyn_linker_->error().c_str());
                dyn_linker_.reset();
            } else if (dyn_linker_->static_tls_size() > 0) {
                // Set up TPIDR_EL0 to point to the end of the static TLS
                // block (TP = base + total). This is the AArch64 TLS
                // convention: the thread pointer points PAST the static
                // TLS block, and TP-relative offsets are negative.
                uint64_t tp = dyn_linker_->static_tls_base() +
                              dyn_linker_->static_tls_size();
                main_cpu_.tpidr_el0 = tp;
                main_cpu_.tpidrro_el0 = tp;
                if (verbose_) {
                    fprintf(stderr, "[%s] native dynlink: static TLS block "
                            "at 0x%llx (size %llu), TP=0x%llx\n",
                            CODENAME,
                            static_cast<unsigned long long>(dyn_linker_->static_tls_base()),
                            static_cast<unsigned long long>(dyn_linker_->static_tls_size()),
                            static_cast<unsigned long long>(tp));
                }
            }
            // Even with native dynlink, we still load the interpreter
            // (ld.so) because some programs call ld.so's _dl_*
            // functions directly. The interpreter's symbols are
            // available via the native linker's symbol table.
        }

        // Try to open the interpreter. Check common host paths for
        // aarch64 dynamic linkers (musl and glibc multiarch).
        std::vector<std::string> interp_paths = {
            info.interp,                                    // exact path from PT_INTERP
            "/usr/aarch64-linux-gnu" + info.interp,         // Debian/Ubuntu multiarch
            "/usr/lib/aarch64-linux-gnu" + info.interp,     // newer Debian multiarch
        };
        FILE* ifile = nullptr;
        std::string found_path;
        for (auto& ip : interp_paths) {
            ifile = fopen(ip.c_str(), "rb");
            if (ifile) { found_path = ip; break; }
        }
        if (ifile) {
            fseek(ifile, 0, SEEK_END);
            long isz = ftell(ifile);
            fseek(ifile, 0, SEEK_SET);
            if (isz <= 0) { fclose(ifile); throw EmuError("empty or invalid interpreter"); }
            std::vector<uint8_t> idata(isz);
            if (fread(idata.data(), 1, isz, ifile) != static_cast<size_t>(isz)) {
                fclose(ifile);
                throw EmuError("short read on interpreter");
            }
            fclose(ifile);
            // Load the interpreter at a high address to avoid conflicts.
            // Use 0x4000000000 (above the 16GB direct window).
            interp_base = 0x4000000000ULL;
            interp_base_ = interp_base;  // Save for AT_BASE in auxv.
            if (idata.size() >= 64 && idata[0] == 0x7f && idata[1] == 'E') {
                uint64_t i_entry, i_phoff;
                uint16_t i_phnum, i_phentsize;
                memcpy(&i_entry, idata.data() + 24, 8);
                memcpy(&i_phoff, idata.data() + 32, 8);
                memcpy(&i_phentsize, idata.data() + 54, 2);
                memcpy(&i_phnum, idata.data() + 56, 2);
                for (int i = 0; i < i_phnum; i++) {
                    const uint8_t* pp = idata.data() + i_phoff + i * i_phentsize;
                    uint32_t p_type;
                    uint64_t p_offset, p_vaddr, p_filesz, p_memsz;
                    memcpy(&p_type, pp + 0, 4);
                    memcpy(&p_offset, pp + 8, 8);
                    memcpy(&p_vaddr, pp + 16, 8);
                    memcpy(&p_filesz, pp + 32, 8);
                    memcpy(&p_memsz, pp + 40, 8);
                    if (p_type != 1) continue;  // PT_LOAD
                    uint64_t addr = interp_base + p_vaddr;
                    mem_.map_range(addr, p_memsz);
                    if (p_filesz > 0) {
                        mem_.write(addr, idata.data() + p_offset, p_filesz);
                    }
                    uint64_t end = addr + p_memsz;
                    if (end > end_addr_) end_addr_ = end;
                }
                // The entry point is the interpreter's entry (relative to interp_base).
                // Skip this if we used native dynlink — we run the main
                // binary's entry directly.
                if (!native_dynlink || !dyn_linker_) {
                    entry_ = interp_base + i_entry;
                }
            }
        }
    }

    // brk starts just above the loaded image, page-aligned up
    brk_ = (info.end_addr + 0xFFF) & ~0xFFFULL;
    brk_start_ = brk_;

    // Set up the initial stack image
    const uint64_t STACK_TOP = 0x8000000000ULL;
    const uint64_t STACK_SIZE = 64 * 1024 * 1024;  // 64 MiB
    uint64_t stack_base = STACK_TOP - STACK_SIZE;
    mem_.map_range(stack_base, STACK_SIZE + 4096);  // +1 page guard at top
    main_cpu_.sp = build_initial_stack(STACK_TOP, argv, info);

    // Pre-allocate a TLS scratch area and set TPIDR_EL0 to point into
    // its center. Many libc startup routines read TPIDR_EL0 before
    // __libc_setup_tls has set the real TCB. Pointing it to valid
    // zeroed memory prevents unmapped-read crashes.
    const uint64_t TLS_SCRATCH_SIZE = 65536;  // 64 KiB
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

// ── Initial stack: argc, argv[], NULL, envp[], NULL, auxv[], NULL ─────
uint64_t Emulator::build_initial_stack(uint64_t stack_top,
                                       std::vector<std::string>& argv,
                                       ElfLoader::Loaded& info) {
    (void)info;  // reserved for future use (AT_PHDR, etc.)
    uint64_t sp = stack_top;

    // Push argv strings
    std::vector<uint64_t> argv_addrs;
    for (auto& a : argv) {
        sp -= a.size() + 1;
        mem_.write(sp, a.data(), a.size() + 1);
        argv_addrs.push_back(sp);
    }

    // Push envp. toybox sh and other programs need more than just PATH.
    // Provide a minimal but realistic environment.
    std::vector<uint64_t> envp_addrs;
    const char* envs[] = {
        "PATH=/tmp/aarch64-bin:/bin:/usr/bin:/sbin:/usr/sbin",
        "HOME=/root",
        "SHELL=/bin/sh",
        "TERM=linux",
        "PWD=/",
        "SHLVL=1",
        "_=/bin/sh",
    };
    for (const char* e : envs) {
        sp -= strlen(e) + 1;
        mem_.write(sp, e, strlen(e) + 1);
        envp_addrs.push_back(sp);
    }

    // AT_RANDOM — 16 random bytes
    sp -= 16;
    uint8_t rnd[16];
    FILE* ur = fopen("/dev/urandom", "rb");
    if (ur) {
        size_t nread = fread(rnd, 1, 16, ur);
        fclose(ur);
        // If we got fewer than 16 bytes, fill the rest with rand().
        for (size_t i = nread; i < 16; i++)
            rnd[i] = static_cast<uint8_t>(rand());
    } else {
        for (int i = 0; i < 16; i++) rnd[i] = static_cast<uint8_t>(rand());
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

    // AT_EXECFN: pointer to the program name string on the stack
    uint64_t execfn_addr = argv_addrs[0];

    std::vector<uint64_t> auxv = {
        6, 4096,           // AT_PAGESZ
        3, phdr_addr_,     // AT_PHDR
        4, phent_,         // AT_PHENT
        5, phnum_,         // AT_PHNUM
        9, prog_entry_,    // AT_ENTRY (original binary's entry, not interp's)
        25, random_addr,   // AT_RANDOM
        16, hwcap,         // AT_HWCAP
        26, 0,             // AT_HWCAP2 (no BTI, no PAC)
        23, 0,             // AT_SECURE (not setuid)
        31, execfn_addr,   // AT_EXECFN (program name)
        7, interp_base_,   // AT_BASE (interpreter load address, 0 if static)
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
    sp &= ~0xFULL;  // 16-byte align

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

// ── Run loop ──────────────────────────────────────────────────────────
int Emulator::run() {
    uint64_t count = 0;
    auto t0 = std::chrono::steady_clock::now();

    // ── Hang watchdog ───────────────────────────────────────────────
    // Detects infinite loops where the same PC is executed over and
    // over without making progress (a common symptom of mallocng init
    // recursion, softfloat loops, or atomic-CAS loops where STXR
    // always fails).
    // Also detects tight multi-PC loops (e.g., a 4-instruction cycle
    // that never exits) by tracking the set of recently-seen PCs.
    constexpr uint64_t HANG_LIMIT = 50'000'000;  // ~50M instructions
    constexpr uint64_t LOOP_DETECT_WINDOW = 16;  // track last 16 PCs
    constexpr uint64_t LOOP_DETECT_LIMIT = 5'000'000;  // 5M in tight loop
    uint64_t last_pc = static_cast<uint64_t>(-1);
    uint64_t same_pc_count = 0;
    // Tight-loop detection: track PCs in a small ring buffer. If we
    // see the same set of PCs repeat for too long without a syscall,
    // it's a tight loop.
    uint64_t recent_pcs[LOOP_DETECT_WINDOW] = {};
    uint64_t pc_index = 0;
    uint64_t tight_loop_count = 0;
    // BUGFIX: the old watchdog sampled ONLY x2 as the progress
    // indicator. x2 is the third argument register and is routinely
    // unchanged in legitimate tight compute loops (e.g., a hot inner
    // loop using x0/x1 only). The new scheme samples a hash of x0+x1+x2
    //+x3+x19+x20+x21+x22+x23+x24+x25+x26+x27+x28 + sp + pc. Any of
    // those changing means the loop is making progress. This eliminates
    // false positives while still catching true linked-list-cycle /
    // infinite-spin bugs.
    uint64_t last_progress_hash = 0;

    while (main_cpu_.running) {
        try {
            // Optional PC trace for debugging. Gated by env var so
            // it's a no-op in production. Set BIFROST_TRACE_PC=1 to
            // log every instruction's PC + first 16 register values.
            // Use BIFROST_TRACE_PC_MAX=N to cap the trace at N
            // instructions (default 10000).
            static const bool trace_pc_ = (getenv("BIFROST_TRACE_PC") != nullptr);
            static const uint64_t trace_pc_max_ =
                getenv("BIFROST_TRACE_PC_MAX") ?
                    strtoull(getenv("BIFROST_TRACE_PC_MAX"), nullptr, 10) : 10000ULL;
            static uint64_t trace_pc_count_ = 0;
            if (trace_pc_ && trace_pc_count_ < trace_pc_max_) {
                fprintf(stderr, "[pc] 0x%llx sp=0x%llx x0=%llx x1=%llx x2=%llx x8=%llx x19=%llx x30=%llx\n",
                        (unsigned long long)main_cpu_.pc,
                        (unsigned long long)main_cpu_.sp,
                        (unsigned long long)main_cpu_.regs[0],
                        (unsigned long long)main_cpu_.regs[1],
                        (unsigned long long)main_cpu_.regs[2],
                        (unsigned long long)main_cpu_.regs[8],
                        (unsigned long long)main_cpu_.regs[19],
                        (unsigned long long)main_cpu_.regs[30]);
                trace_pc_count_++;
            }
            // JIT warmup threshold: use the interpreter for the first
            // `jit_threshold_` instructions, then switch to JIT. This
            // avoids JIT compilation overhead for short programs.
            bool use_jit_now = jit_enabled_ && jit_ &&
                               (jit_threshold_ == 0 || interp_count_ >= jit_threshold_);
            if (use_jit_now) {
                // JIT dispatch — defined in src/jit/jit_glue.cpp so the
                // FrostJIT definition is available. Falls back to
                // step() for any instruction it can't handle.
                jit_step(main_cpu_);
            } else {
                step(main_cpu_);
                interp_count_++;
            }
        } catch (UnmappedMemory& e) {
            // If the guest has installed a SIGSEGV handler, deliver the
            // signal with the fault address and si_code (MAPERR for read,
            // ACCERR for write) and continue. Otherwise, stop emulation.
            int si_code = e.write ? SEGV_ACCERR_EMU : SEGV_MAPERR_EMU;
            if (deliver_signal(*this, main_cpu_, signals_, BIFROST_SIGSEGV,
                               si_code, e.addr)) {
                count++;
                continue;
            }
            break;
        }
        count++;

        // Watchdog: if PC hasn't changed, increment same_pc_count.
        if (main_cpu_.pc == last_pc) {
            same_pc_count++;
            if (same_pc_count > HANG_LIMIT) {
                fprintf(stderr,
                    "[%s] hang watchdog: PC=0x%llx executed %llu times "
                    "without progress; aborting (likely mallocng init "
                    "recursion or atomic loop)\n",
                    CODENAME,
                    static_cast<unsigned long long>(main_cpu_.pc),
                    static_cast<unsigned long long>(same_pc_count));
                main_cpu_.running = false;
                main_cpu_.exit_code = 70;  // EX_SOFTWARE
                break;
            }
        } else {
            last_pc = main_cpu_.pc;
            same_pc_count = 0;
        }

        // Tight-loop detection: track recent PCs in a ring buffer.
        // If we've been cycling through a small set of PCs for too long
        // without hitting a syscall AND registers aren't changing, it's
        // likely a bug-induced tight loop. We sample a hash of multiple
        // registers as a progress indicator — if any of them changes,
        // the loop is making progress (e.g., a tight compute loop
        // working on x0/x1 only). Only trigger if registers are frozen
        // (true bug). Sample every 256 instructions to reduce overhead.
        if ((count & 0xFF) == 0) {
            recent_pcs[pc_index % LOOP_DETECT_WINDOW] = main_cpu_.pc;
            pc_index++;
            // Check if all recent PCs are the same small set (≤4 unique)
            // AND the progress register hasn't changed.
            if ((pc_index % LOOP_DETECT_WINDOW) == 0) {
                std::set<uint64_t> unique_pcs(recent_pcs, recent_pcs + LOOP_DETECT_WINDOW);
                if (unique_pcs.size() <= 4) {
                    // Compute a progress hash from the most volatile
                    // registers. We pick x0-x3 (argument registers) and
                    // x19-x28 (callee-saved scratch) plus sp and pc —
                    // any of those changing means the loop is making
                    // progress. The old code only checked x2, which is
                    // the third argument register and routinely
                    // unchanged in legitimate tight compute loops.
                    uint64_t current_hash =
                          main_cpu_.regs[0] ^ main_cpu_.regs[1]
                        ^ main_cpu_.regs[2] ^ main_cpu_.regs[3]
                        ^ main_cpu_.regs[19] ^ main_cpu_.regs[20]
                        ^ main_cpu_.regs[21] ^ main_cpu_.regs[22]
                        ^ main_cpu_.regs[23] ^ main_cpu_.regs[24]
                        ^ main_cpu_.regs[25] ^ main_cpu_.regs[26]
                        ^ main_cpu_.regs[27] ^ main_cpu_.regs[28]
                        ^ main_cpu_.sp ^ main_cpu_.pc;
                    if (current_hash == last_progress_hash) {
                        tight_loop_count += LOOP_DETECT_WINDOW * 16;
                        if (tight_loop_count > LOOP_DETECT_LIMIT) {
                            fprintf(stderr,
                                "[%s] tight-loop watchdog: %zu unique PCs, "
                                "registers frozen for %llu instructions; aborting "
                                "(likely linked-list cycle or similar bug)\n",
                                CODENAME, unique_pcs.size(),
                                static_cast<unsigned long long>(tight_loop_count));
                            main_cpu_.running = false;
                            main_cpu_.exit_code = 70;
                            break;
                        }
                    } else {
                        tight_loop_count = 0;
                    }
                    last_progress_hash = current_hash;
                } else {
                    tight_loop_count = 0;
                }
            }
        }

        // Drain the host-signal queue every ~4K instructions.
        if ((count & 0xFFF) == 0) {
            drain_host_signals(main_cpu_);
        }

        if ((count & 0xFFFFF) == 0) {
            if (!mem_.is_mapped(main_cpu_.pc, 4)) {
                throw EmuError("PC ran into unmapped memory at 0x"
                    + to_hex(main_cpu_.pc));
            }
            // SDL2 real-time refresh (~1M instructions ≈ 6 fps at 6 MIPS).
            if (graphics_.ready() && graphics_.guest_fb_addr() != 0) {
                uint64_t gaddr = graphics_.guest_fb_addr();
                std::vector<uint8_t> buf(graphics_.size());
                try {
                    mem_.read(gaddr, buf.data(), buf.size());
                    graphics_.sync_from(buf.data());
                    graphics_.refresh();
                    if (!graphics_.poll_events()) {
                        main_cpu_.running = false;
                        main_cpu_.exit_code = 0;
                        break;
                    }
                } catch (const std::exception&) {
                    // guest fb address no longer mapped — skip
                }
            }
        }
    }

    // Wait for any spawned threads to exit
    join_threads();
    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();
    if (verbose_) {
        fprintf(stderr, "[%s] executed %llu instructions in %.3fs (%.2f MIPS)\n",
                CODENAME, static_cast<unsigned long long>(count), secs, count / 1e6 / secs);
        fprintf(stderr, "[%s] mem pages: %zu (%.1f MB)\n",
                CODENAME, mem_.page_count(),
                mem_.page_count() * 4096 / 1048576.0);
        uint64_t hits = main_cpu_.decode_cache_hits;
        uint64_t misses = main_cpu_.decode_cache_misses;
        {
            std::lock_guard<std::mutex> g(threads_mu_);
            for (auto& gt : threads_) {
                hits   += gt->cpu.decode_cache_hits;
                misses += gt->cpu.decode_cache_misses;
            }
        }
        uint64_t total = hits + misses;
        if (total > 0) {
            fprintf(stderr, "[%s] decode cache: %llu hits, %llu misses (%.1f%% hit rate)\n",
                    CODENAME, static_cast<unsigned long long>(hits),
                    static_cast<unsigned long long>(misses),
                    100.0 * hits / total);
        }
        if (jit_ && jit_enabled_) {
            print_jit_stats();
        }
    }
    return main_cpu_.exit_code;
}

// ── Per-instruction step (decode cache + trace) ───────────────────────
void Emulator::step(CPU& cpu) {
    if (trace_) {
        fprintf(stderr,
            "[trace tid=%d] pc=0x%08llx x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx "
            "x4=0x%llx x5=0x%llx x8=0x%llx x16=0x%llx x17=0x%llx x19=0x%llx "
            "x20=0x%llx x21=0x%llx x22=0x%llx x23=0x%llx x24=0x%llx x25=0x%llx "
            "x26=0x%llx x27=0x%llx x28=0x%llx x29=0x%llx x30=0x%llx pstate=0x%x "
            "sp=0x%llx v0lo=0x%llx v0hi=0x%llx v1lo=0x%llx v1hi=0x%llx v2lo=0x%llx v2hi=0x%llx\n",
            cpu.tid,
            static_cast<unsigned long long>(cpu.pc),
            static_cast<unsigned long long>(cpu.regs[0]), static_cast<unsigned long long>(cpu.regs[1]),
            static_cast<unsigned long long>(cpu.regs[2]), static_cast<unsigned long long>(cpu.regs[3]),
            static_cast<unsigned long long>(cpu.regs[4]), static_cast<unsigned long long>(cpu.regs[5]),
            static_cast<unsigned long long>(cpu.regs[8]), static_cast<unsigned long long>(cpu.regs[16]),
            static_cast<unsigned long long>(cpu.regs[17]), static_cast<unsigned long long>(cpu.regs[19]),
            static_cast<unsigned long long>(cpu.regs[20]), static_cast<unsigned long long>(cpu.regs[21]),
            static_cast<unsigned long long>(cpu.regs[22]), static_cast<unsigned long long>(cpu.regs[23]),
            static_cast<unsigned long long>(cpu.regs[24]), static_cast<unsigned long long>(cpu.regs[25]),
            static_cast<unsigned long long>(cpu.regs[26]), static_cast<unsigned long long>(cpu.regs[27]),
            static_cast<unsigned long long>(cpu.regs[28]), static_cast<unsigned long long>(cpu.regs[29]),
            static_cast<unsigned long long>(cpu.regs[30]), cpu.pstate,
            static_cast<unsigned long long>(cpu.sp),
            static_cast<unsigned long long>(cpu.v_lo[0]), static_cast<unsigned long long>(cpu.v_hi[0]),
            static_cast<unsigned long long>(cpu.v_lo[1]), static_cast<unsigned long long>(cpu.v_hi[1]),
            static_cast<unsigned long long>(cpu.v_lo[2]), static_cast<unsigned long long>(cpu.v_hi[2]));
    }
    uint32_t inst = mem_.fetch_inst(cpu.pc, &cpu.page_cache);
    uint64_t next_pc = cpu.pc + 4;
    execute(inst, next_pc, cpu);
    cpu.pc = next_pc;
}

// Host-to-guest signal forwarding (install_host_signal_handlers,
// queue_host_signal, host_signal_handler, drain_host_signals) is
// implemented in src/core/signal.cpp — see that file for the full
// disposition table.

} // namespace arm64emu
