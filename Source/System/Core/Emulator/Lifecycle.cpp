// Emulator construction/teardown, the main run loop and single-stepping.
#include "System/Core/Emulator.hpp"
#include "System/Core/DebugFlags.hpp"
#include "Bifrost/Version.hpp"
#include "System/Core/CrashReport.hpp"
#include "System/Memory/Memory.hpp"
#include "System/Loader/DynamicLinker.hpp"
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
// ── Static active-emu pointer (defined in Source/System/Core/Signals.cpp) ────────
// Emulator* Emulator::g_active_emu_ = nullptr;  // defined in Signals.cpp
Emulator::Emulator() = default;
Emulator::~Emulator() {
    // Clear the active-emu pointer so host signal handlers don't
    // dereference freed memory. Without this, a signal arriving after
    // the destructor runs would crash with UAF.
    g_active_emu_ = nullptr;
    // Defensive: if run() never reached its join_threads()/stop path
    // (e.g. an exception unwound out of run), stop + join any still-runny
    // SDL threads so their joinable std::threads aren't destroyed here.
    // Idempotent — run() already cleared sdl_threads_ in the normal path.
    stop_sdl_threads();
    // Same for guest vCPU threads: ask every still-running worker to stop,
    // wake futex sleepers so they observe running==false, then join. A
    // joinable std::thread destroyed without join() calls std::terminate.
    {
        std::lock_guard<std::mutex> g(threads_mu_);
        for (auto& gt : threads_) gt->cpu.running = false;
    }
    vfs_.stop_binder();
    wake_all_futexes();
    join_threads();
    // Stop + join the periodic reporter (it captures `this`).
    if (stats_reporter_thread_.joinable()) {
        stats_reporter_stop_.store(true, std::memory_order_relaxed);
        stats_reporter_thread_.join();
    }
    // Stop audio callback workers BEFORE the memory/CPU teardown.
    if (auto* at = graphics_.audio_thunk()) at->shutdown();
}
// resolve pc to module name + offset for crash logs. copies the
// name out so the pointer stays valid. never throws.
static void resolve_crash_module(DynamicLinker* dl, uint64_t pc,
                                 std::string& out_name, uint64_t& out_off) {
    out_name.clear();
    out_off = 0;
    if (!dl) return;
    try {
        const LoadedObject* obj = dl->find_object_by_addr(pc);
        if (!obj) return;
        out_name = obj->name;
        out_off = pc - obj->base_addr;
    } catch (...) {}
}
// ── Run loop ──────────────────────────────────────────────────────────
int Emulator::run() {
    uint64_t count = 0;
    auto t0 = std::chrono::steady_clock::now();
    run_start_time_ = t0;
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
    // ── Periodic reporter thread (BIFROST_STATS_PERIOD=seconds) ────────
    // MUST be a background thread, not an in-loop check: games park their
    // frame loops inside jit_call_helper's callee-dispatch loop, so run()'s
    // outer loop stops iterating right when mid-run profiling matters. The
    // thread sleeps on wall time and calls dump_periodic_stats (which also
    // chains the SIGPROF snapshot and syscall histogram). Inert unless the
    // env var is set. Joined before run() returns.
    const double stats_period_ = []() {
        const char* s = getenv("BIFROST_STATS_PERIOD");
        return s ? atof(s) : 0.0;
    }();
    if (stats_period_ > 0.0) {
        stats_reporter_stop_.store(false, std::memory_order_relaxed);
        stats_reporter_thread_ = std::thread([this, stats_period_]() {
            auto last = std::chrono::steady_clock::now();
            while (!stats_reporter_stop_.load(std::memory_order_relaxed)) {
                // Sleep in small slices so run() can join quickly on exit.
                std::this_thread::sleep_for(
                    std::chrono::duration<double>(std::min(stats_period_, 0.25)));
                auto now = std::chrono::steady_clock::now();
                double dt = std::chrono::duration<double>(now - last).count();
                if (dt >= stats_period_ && main_cpu_.running) {
                    dump_periodic_stats(dt);
                    last = now;
                }
            }
        });
    }
    main_host_thread_ = ::pthread_self();
    main_host_thread_set_.store(true, std::memory_order_release);
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
                // JIT dispatch — defined in Source/FrostJIT/Runtime/EmulatorBridge.cpp so the
                // FrostJIT definition is available. Falls back to
                // step() for any instruction it can't handle.
                jit_step(main_cpu_);
            } else {
                step(main_cpu_);
                interp_count_++;
            }
        } catch (UnmappedMemory& e) {
            // If the guest has installed a SIGSEGV handler, deliver the
            // signal with the fault address and si_code (MAPERR for an
            // unmapped page, ACCERR for permissions) and continue.
            int si_code = e.segv_code();
            if (deliver_signal(*this, main_cpu_, signals_, e.signo(),
                               si_code, e.addr)) {
                count++;
                continue;
            }
            {
                std::string mod;
                uint64_t off = 0;
                resolve_crash_module(dyn_linker_.get(), main_cpu_.pc, mod, off);
                report_crash(main_cpu_, mem_, "unmapped memory", e.addr, nullptr,
                             mod.empty() ? nullptr : mod.c_str(), off);
            }
            break;
        } catch (DecodeError& e) {
            if (dbg().crash_dump) fprintf(stderr, "[decode] %s\n", e.what());
            // pointer calls and genuinely illegal instructions.
            //
            // On real ARM64 Linux:
            //   - Jumping to NULL (pc=0) or into the zero page (0x0-0xFFF)
            //     delivers SIGSEGV with si_code=SEGV_MAPERR (address not
            //     mapped for execution). The zero page is normally not
            //     mapped on Linux (vm.mmap_min_addr >= 4096).
            //   - Encountering an illegal/undefined instruction delivers
            //     SIGILL with si_code=ILL_ILLOPC.
            //
            // The emulator maps the zero page (mem_.map_range(0, 4096))
            // so NULL dereferences return 0 instead of faulting. This
            // means instruction fetch at pc=0 reads 0x00000000, which
            // is UDF #0 — the decoder throws DecodeError.
            //
            // Previously, DecodeError propagated to main() and killed
            // the emulator with "bifrost-emu: decode error at pc=0x0".
            // signal handler can recover) or terminate with the correct
            // exit code (128+11=139), not an emulator crash.
            //
            // pc=0 means a NULL function pointer was called (e.g., an
            // unresolved weak symbol or ifunc). pc in [1, 4095] means
            // a jump into the zero page. Both are SIGSEGV.
            uint64_t fault_pc = main_cpu_.pc;
            if (fault_pc < 4096) {
                // NULL pointer execution / jump into zero page → SIGSEGV
                if (deliver_signal(*this, main_cpu_, signals_,
                                   BIFROST_SIGSEGV, SEGV_MAPERR_EMU,
                                   fault_pc)) {
                    count++;
                    continue;
                }
                // No handler — default disposition terminates with SIGSEGV
                // (exit code 128+11=139). deliver_signal already set
                // cpu.running=false and cpu.exit_code=139.
                // throw site is quiet by default, so this is the crash line.
                {
                    std::string mod;
                    uint64_t off = 0;
                    resolve_crash_module(dyn_linker_.get(), fault_pc, mod, off);
                    report_crash(main_cpu_, mem_, "sigsegv null pc", fault_pc, nullptr,
                                 mod.empty() ? nullptr : mod.c_str(), off);
                }
                break;
            }
            // Genuinely illegal instruction at a valid PC → SIGILL.
            // This lets JITs and dynamic code generators that use SIGILL
            // for trap-on-overflow patterns to work.
            if (deliver_signal(*this, main_cpu_, signals_,
                               BIFROST_SIGILL, ILL_ILLOPC_EMU, fault_pc)) {
                count++;
                continue;
            }
            // No SIGILL handler — terminate with SIGILL (exit 128+4=132).
            // throw site is quiet by default, so this is the crash line.
            {
                std::string mod;
                uint64_t off = 0;
                resolve_crash_module(dyn_linker_.get(), fault_pc, mod, off);
                report_crash(main_cpu_, mem_, "sigill illegal inst", fault_pc, nullptr,
                             mod.empty() ? nullptr : mod.c_str(), off);
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
            // Also drain per-CPU pending signals (queued by cross-thread
            // tgkill/tkill/kill). This is the fix for the cross-thread
            // CPU-mutation race.
            drain_pending_signals(main_cpu_);
            // rusage/times. Add the ~4K instructions since last drain to
            // the total. (Not exact — we add 4096 each time, but close
            // enough for CPU time estimation.)
            guest_instructions_total_.fetch_add(4096, std::memory_order_relaxed);
        }
        // so guest_instr_to_seconds() has a real conversion factor.
        if ((count & 0xFFFFF) == 0 && count > 0) {
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - t0).count();
            if (elapsed > 0.001) {  // avoid div-by-zero
                double mips = static_cast<double>(count) / elapsed / 1e6;
                mips_estimate_.store(mips, std::memory_order_relaxed);
            }
        }
        if ((count & 0xFFFFF) == 0) {
            // Reap guest threads that have exited, so per-thread state
            // (decode cache + optional JIT) doesn't accumulate.
            reap_finished_threads();
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
    // Group exit (exit_group / fatal signal on any thread): stop the rest
    // now. Otherwise main merely exited and the process lives until the
    // last thread does (join_threads also honors a later group exit).
    if (group_exit_requested()) kill_other_threads(main_cpu_);
    join_threads();
    main_host_thread_set_.store(false, std::memory_order_release);
    // Also stop + join SDL worker threads: exit_group only clears the
    // calling CPU's running flag, so SDL threads may still be running
    // (one may be blocked in a host SDL call). Without this, ~Emulator
    // destroys joinable std::threads → std::terminate.
    stop_sdl_threads();
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
    // Stop + join the periodic reporter before returning (it captures
    // `this`; the Emulator must outlive it by construction, not by luck).
    if (stats_reporter_thread_.joinable()) {
        stats_reporter_stop_.store(true, std::memory_order_relaxed);
        stats_reporter_thread_.join();
    }
    if (group_exit_requested()) return group_exit_code_.load(std::memory_order_acquire);
    return main_cpu_.exit_code;
}
// ── Per-instruction step (decode cache + trace) ───────────────────────
void Emulator::step(CPU& cpu) {
    // BIFROST_PROF sampling-flag toggle (see EmulatorBridge.cpp): the
    // SIGPROF handler buckets time spent in the interpreter vs JIT.
    extern thread_local bool prof_in_interp;
    extern bool bifrost_prof_active();
    const bool prof_active = bifrost_prof_active();
    if (prof_active) prof_in_interp = true;
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
    uint32_t inst = mem_.fetch_inst(cpu.pc);
    uint64_t next_pc = cpu.pc + 4;
    execute(inst, next_pc, cpu);
    cpu.pc = next_pc;
    if (prof_active) prof_in_interp = false;
}
}  // namespace arm64emu
