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
#include <stdexcept>
#include <thread>
#include <vector>

namespace arm64emu {

// ── Static active-emu pointer (defined in src/core/signal.cpp) ────────
// Emulator* Emulator::g_active_emu_ = nullptr;  // defined in signal.cpp

Emulator::Emulator() = default;
Emulator::~Emulator() = default;

// ── ELF loading ───────────────────────────────────────────────────────
void Emulator::load_elf_file(const std::string& path, std::vector<std::string>& argv) {
    elf_path_ = path;
    vfs_.set_elf_path(path);
    vfs_.set_argv(argv);
    vfs_.set_graphics(&graphics_);
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

    // Push envp (just PATH)
    std::vector<uint64_t> envp_addrs;
    const char* env = "PATH=/bin:/usr/bin";
    sp -= strlen(env) + 1;
    mem_.write(sp, env, strlen(env) + 1);
    envp_addrs.push_back(sp);

    // AT_RANDOM — 16 random bytes
    sp -= 16;
    uint8_t rnd[16];
    FILE* ur = fopen("/dev/urandom", "rb");
    if (ur) { fread(rnd, 1, 16, ur); fclose(ur); }
    else { for (int i = 0; i < 16; i++) rnd[i] = static_cast<uint8_t>(rand()); }
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
    constexpr uint64_t HANG_LIMIT = 50'000'000;  // ~50M instructions
    uint64_t last_pc = static_cast<uint64_t>(-1);
    uint64_t same_pc_count = 0;

    while (main_cpu_.running) {
        try {
            if (jit_enabled_ && jit_) {
                // JIT dispatch — defined in src/jit/jit_glue.cpp so the
                // FrostJIT definition is available. Falls back to
                // step_public() for any instruction it can't handle.
                jit_step(main_cpu_);
            } else {
                step(main_cpu_);
            }
        } catch (UnmappedMemory& e) {
            // If the guest has installed a SIGSEGV handler, deliver the
            // signal and continue. Otherwise, stop emulation.
            if (deliver_signal(*this, main_cpu_, signals_, BIFROST_SIGSEGV)) {
                count++;
                continue;
            }
            (void)e;
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
