// jit_glue.cpp — Glue between Emulator and FrostJIT.
//
// Emulator's constructor/destructor live in src/core/emulator.cpp.
// This file holds the JIT-specific Emulator methods that need to see
// FrostJIT's full definition (held via unique_ptr in Emulator).
#include "core/emulator.h"
#include "jit/frostjit.hpp"
#include "syscalls/syscalls.h"
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <sys/time.h>
#include <ucontext.h>
namespace arm64emu {
// ── BIFROST_PROF=1 sampling profiler ─────────────────────────────────
// SIGPROF/ITIMER_PROF sampler that buckets the interrupted host RIP:
//   jit        — inside a translated block's x86 code (executing guest code)
//   dispatch   — inside run_block (lookup, chaining, watchdog)
//   translate  — inside translate_block (decoding + IR + x86 codegen)
//   interp     — inside Emulator::step (interpreter)
//   other      — syscall handlers, memcpy, host libs, anything else
// Thread-local flags are toggled by RAII guards in run_block /
// translate_block / step; the handler only does atomic increments.
namespace {
std::atomic<uint64_t> prof_jit{0};
std::atomic<uint64_t> prof_dispatch{0};
std::atomic<uint64_t> prof_translate{0};
std::atomic<uint64_t> prof_interp{0};
std::atomic<uint64_t> prof_other{0};
std::atomic<bool>     prof_enabled{false};
thread_local const uint8_t* tls_prof_codebuf = nullptr;
thread_local size_t         tls_prof_codebuf_size = 0;
}
// Thread-local "currently inside" flags, toggled by RAII guards in
// run_block / translate_block / Emulator::step (declared extern in
// those TUs). The handler reads them; the guards set them.
thread_local bool prof_in_run_block = false;
thread_local bool prof_in_translate = false;
thread_local bool prof_in_interp = false;
// Per-thread ring of sampled RIPs that landed inside the JIT code buffer.
// The handler is async-signal-safe (plain TLS array writes only); the
// buffers are resolved to guest PCs at exit by prof_pc_histogram(). Fixed
// size, power-of-two, wraps (dropping the oldest) — at 100 Hz a 4096-ring
// holds 41s of samples.
constexpr size_t PROF_RIP_MAX = 4096;
thread_local uint64_t tls_prof_rips[PROF_RIP_MAX];
thread_local uint32_t tls_prof_rip_head = 0;
namespace {
struct ProfGuard {
    bool* f_;
    explicit ProfGuard(bool* f) : f_(f) { if (prof_enabled.load(std::memory_order_relaxed)) *f_ = true; }
    ~ProfGuard() { if (prof_enabled.load(std::memory_order_relaxed)) *f_ = false; }
};
static void prof_signal_handler(int, siginfo_t*, void* ctx) {
    auto* uc = static_cast<ucontext_t*>(ctx);
    uint64_t rip = static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RIP]);
    if (tls_prof_codebuf && rip >= reinterpret_cast<uint64_t>(tls_prof_codebuf) &&
        rip < reinterpret_cast<uint64_t>(tls_prof_codebuf) + tls_prof_codebuf_size) {
        prof_jit.fetch_add(1, std::memory_order_relaxed);
        tls_prof_rips[tls_prof_rip_head & (PROF_RIP_MAX - 1)] = rip;
        tls_prof_rip_head++;
    } else if (prof_in_translate) {
        prof_translate.fetch_add(1, std::memory_order_relaxed);
    } else if (prof_in_interp) {
        prof_interp.fetch_add(1, std::memory_order_relaxed);
    } else if (prof_in_run_block) {
        prof_dispatch.fetch_add(1, std::memory_order_relaxed);
    } else {
        prof_other.fetch_add(1, std::memory_order_relaxed);
    }
}
static void prof_install(FrostJIT* jit) {
    if (prof_enabled.exchange(true)) return;
    tls_prof_codebuf = jit->code_buf();
    tls_prof_codebuf_size = jit->code_buf_size();
    struct sigaction sa = {};
    sa.sa_sigaction = prof_signal_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, nullptr);
    struct itimerval it = {};
    it.it_interval.tv_usec = 10000;  // 100 Hz
    it.it_value.tv_usec = 10000;
    setitimer(ITIMER_PROF, &it, nullptr);
}
}
// Called lazily from run_block's first dispatch (after any host-signal
// forwarding handlers are installed) so our SIGPROF handler isn't
// overwritten by install_host_signal_handlers.
void bifrost_prof_init(FrostJIT* jit) {
    static bool inited = false;
    if (inited || !getenv("BIFROST_PROF")) return;
    inited = true;
    prof_install(jit);
}
bool bifrost_prof_active() {
    return prof_enabled.load(std::memory_order_relaxed);
}
void Emulator::enable_jit() {
    if (jit_) return;
    // Shared-JIT mode (default): spawned threads share the main's FrostJIT,
    // saving 64 MiB per thread. This requires the code buffer to be RWX
    // (not W^X) so translation (writes) and execution (reads/exec) can
    // happen concurrently. W^X would toggle mprotect on the WHOLE buffer,
    // crashing any thread executing JIT code.
    // We force-disable W^X by setting BIFROST_NO_WEX before the FrostJIT
    // constructor runs (it checks the env var at construction).
    // Opt out via BIFROST_NO_SHARED_JIT=1 for per-thread JIT (lock-free,
    // W^X-protected, but 64 MiB per thread).
    // Security tradeoff: acceptable — the emulator is a single-process
    // user-mode emulator; the only code in the JIT buffer is generated
    // from the trusted guest binary.
    static bool shared_jit_checked = false;
    static bool shared_jit_mode = false;
    if (!shared_jit_checked) {
        shared_jit_mode = (getenv("BIFROST_NO_SHARED_JIT") == nullptr);
        shared_jit_checked = true;
    }
    if (shared_jit_mode) {
        setenv("BIFROST_NO_WEX", "1", 1);
    }
    jit_ = std::make_unique<FrostJIT>();
    jit_enabled_ = (jit_ != nullptr);
    if (jit_enabled_) {
        jit_->set_direct_window(mem_.direct_window());
        jit_->set_vdso_range(vdso_base_, vdso_size_);
        if (verbose_) {
            const auto& cf = jit_->cpu_features();
            fprintf(stderr, "[%s] frostJIT enabled (x86 codegen, %s mode, "
                    "features: %s)\n",
                    CODENAME, shared_jit_mode ? "shared" : "per-thread",
                    cpu_features_string(cf));
        }
    }
}
void Emulator::print_jit_stats() {
    if (!jit_ || !jit_enabled_) return;
    // Aggregate main JIT + all per-thread JITs for a complete picture.
    uint64_t blocks_translated = jit_->blocks_translated;
    uint64_t blocks_executed   = jit_->blocks_executed;
    uint64_t instructions      = jit_->instructions_executed;
    uint64_t cache_hits        = jit_->cache_hits;
    uint64_t cache_misses      = jit_->cache_misses;
    uint64_t fallbacks         = jit_->interpreter_fallbacks;
    uint64_t chains            = jit_->block_chains_patched;
    size_t    code_used        = jit_->code_buf_used();
    size_t    code_size        = jit_->code_buf_size();
    size_t    cache_entries    = jit_->cache_entries();
    size_t    num_jits         = 1;  // main JIT
    {
        std::lock_guard<std::mutex> g(threads_mu_);
        for (auto& gt : threads_) {
            if (gt->jit) {
                blocks_translated += gt->jit->blocks_translated;
                blocks_executed   += gt->jit->blocks_executed;
                instructions      += gt->jit->instructions_executed;
                cache_hits        += gt->jit->cache_hits;
                cache_misses      += gt->jit->cache_misses;
                fallbacks         += gt->jit->interpreter_fallbacks;
                chains            += gt->jit->block_chains_patched;
                code_used         += gt->jit->code_buf_used();
                cache_entries     += gt->jit->cache_entries();
                num_jits++;
            }
        }
    }
    fprintf(stderr, "[%s] frostJIT (%zu thread%s): %llu blocks translated, "
            "%llu executed (%llu instructions, %llu cache hits, %llu misses, "
            "%llu fallbacks, %llu chains)\n",
            CODENAME, num_jits, num_jits == 1 ? "" : "s",
            static_cast<unsigned long long>(blocks_translated),
            static_cast<unsigned long long>(blocks_executed),
            static_cast<unsigned long long>(instructions),
            static_cast<unsigned long long>(cache_hits),
            static_cast<unsigned long long>(cache_misses),
            static_cast<unsigned long long>(fallbacks),
            static_cast<unsigned long long>(chains));
    fprintf(stderr, "[%s] frostJIT: code cache %zu/%zu bytes, %zu blocks\n",
            CODENAME, code_used, code_size * num_jits, cache_entries);
    if (blocks_executed > 0) {
        double avg = static_cast<double>(instructions) /
                     static_cast<double>(blocks_executed);
        fprintf(stderr, "[%s] frostJIT: avg %.1f instructions/block\n",
                CODENAME, avg);
    }
    if (getenv("BIFROST_BLOCK_PROF")) {
        auto& bp = jit_->block_profile;
        fprintf(stderr, "[%s] frostJIT block-end reasons: "
                "natural_branch=%llu entry_point=%llu call_interp_cap=%llu "
                "bl_call_cap=%llu max_size=%llu decode_fail=%llu interp_only=%llu\n",
                CODENAME,
                static_cast<unsigned long long>(bp.natural_branch.load()),
                static_cast<unsigned long long>(bp.entry_point.load()),
                static_cast<unsigned long long>(bp.call_interp_cap.load()),
                static_cast<unsigned long long>(bp.bl_call_cap.load()),
                static_cast<unsigned long long>(bp.max_size.load()),
                static_cast<unsigned long long>(bp.decode_fail.load()),
                static_cast<unsigned long long>(bp.interp_only.load()));
    }
    dump_prof_snapshot();
}
// Periodic (or exit-time) SIGPROF bucket snapshot. Shared by print_jit_stats
// and the BIFROST_STATS_PERIOD reporter so the jit/dispatch/translate/interp
// split can be observed DURING a run phase (e.g. worldgen) without requiring a
// clean guest exit. Inert unless BIFROST_PROF=1.
void Emulator::dump_prof_snapshot() {
    if (!prof_enabled.load(std::memory_order_relaxed)) return;
    uint64_t jit = prof_jit.load(std::memory_order_relaxed);
    uint64_t disp = prof_dispatch.load(std::memory_order_relaxed);
    uint64_t trans = prof_translate.load(std::memory_order_relaxed);
    uint64_t interp = prof_interp.load(std::memory_order_relaxed);
    uint64_t other = prof_other.load(std::memory_order_relaxed);
    uint64_t total = jit + disp + trans + interp + other;
    if (total > 0) {
        fprintf(stderr, "[%s] SIGPROF samples: jit=%llu (%.1f%%) dispatch=%llu "
                "(%.1f%%) translate=%llu (%.1f%%) interp=%llu (%.1f%%) other=%llu (%.1f%%)\n",
                CODENAME,
                static_cast<unsigned long long>(jit), 100.0 * jit / total,
                static_cast<unsigned long long>(disp), 100.0 * disp / total,
                static_cast<unsigned long long>(trans), 100.0 * trans / total,
                static_cast<unsigned long long>(interp), 100.0 * interp / total,
                static_cast<unsigned long long>(other), 100.0 * other / total);
    }
    // BIFROST_PC_HIST=1: resolve the sampled jit-bucket RIPs to guest
    // PCs and print the hottest ones (map to functions with aarch64
    // objdump). Gated separately so the default run stays quiet.
    static const bool pc_hist_ = (getenv("BIFROST_PC_HIST") != nullptr);
    if (pc_hist_ && tls_prof_rip_head > 0) {
        uint32_t n = std::min<uint32_t>(tls_prof_rip_head, (uint32_t)PROF_RIP_MAX);
        jit_->dump_pc_histogram(tls_prof_rips, n);
    }
}
// Periodic real-guest-throughput + block-structure reporter. The old
// BIFROST_STATS_PERIOD "rolling MIPS" used the run-loop dispatch counter
// (block dispatches, NOT guest instructions) so it under-reported real
// throughput ~10x. This aggregates jit_->instructions_executed across all
// JITs (the actual guest-instruction count, incremented per block body)
// and prints the block-end-reason histogram deltas so the codegen-quality
// levers (avg instr/block, CALL_INTERP/BL_CALL caps, max_size) stay visible
// DURING a run phase. Inert unless BIFROST_STATS_PERIOD is set.
void Emulator::dump_periodic_stats(double dt) {
    if (!jit_ || !jit_enabled_) return;
    uint64_t instr  = jit_->instructions_executed.load(std::memory_order_relaxed);
    uint64_t blocks = jit_->blocks_executed.load(std::memory_order_relaxed);
    uint64_t blocks_tr  = jit_->blocks_translated.load(std::memory_order_relaxed);
    uint64_t chains = jit_->block_chains_patched.load(std::memory_order_relaxed);
    // BlockProfile is only tracked on the main JIT in practice, but sum the
    // per-thread JITs if present so the numbers are comparable to print_jit_stats.
    uint64_t natural = jit_->block_profile.natural_branch.load(std::memory_order_relaxed);
    uint64_t entry   = jit_->block_profile.entry_point.load(std::memory_order_relaxed);
    uint64_t ci_cap  = jit_->block_profile.call_interp_cap.load(std::memory_order_relaxed);
    uint64_t bl_cap  = jit_->block_profile.bl_call_cap.load(std::memory_order_relaxed);
    uint64_t max_sz  = jit_->block_profile.max_size.load(std::memory_order_relaxed);
    uint64_t decfail = jit_->block_profile.decode_fail.load(std::memory_order_relaxed);
    uint64_t ionly   = jit_->block_profile.interp_only.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> g(threads_mu_);
        for (auto& gt : threads_) {
            if (gt->jit) {
                instr     += gt->jit->instructions_executed.load(std::memory_order_relaxed);
                blocks    += gt->jit->blocks_executed.load(std::memory_order_relaxed);
                blocks_tr += gt->jit->blocks_translated.load(std::memory_order_relaxed);
                chains    += gt->jit->block_chains_patched.load(std::memory_order_relaxed);
                natural   += gt->jit->block_profile.natural_branch.load(std::memory_order_relaxed);
                entry     += gt->jit->block_profile.entry_point.load(std::memory_order_relaxed);
                ci_cap    += gt->jit->block_profile.call_interp_cap.load(std::memory_order_relaxed);
                bl_cap    += gt->jit->block_profile.bl_call_cap.load(std::memory_order_relaxed);
                max_sz    += gt->jit->block_profile.max_size.load(std::memory_order_relaxed);
                decfail   += gt->jit->block_profile.decode_fail.load(std::memory_order_relaxed);
                ionly     += gt->jit->block_profile.interp_only.load(std::memory_order_relaxed);
            }
        }
    }
    // deltas since the last dump
    static uint64_t last_instr_, last_blocks_, last_blocks_tr_, last_chains_;
    static uint64_t last_natural_, last_entry_, last_ci_cap_, last_bl_cap_,
                    last_max_sz_, last_decfail_, last_ionly_;
    uint64_t d_instr     = instr - last_instr_;
    uint64_t d_blocks    = blocks - last_blocks_;
    uint64_t d_blocks_tr = blocks_tr - last_blocks_tr_;
    uint64_t d_chains    = chains - last_chains_;
    double mips = dt > 0 ? d_instr / 1e6 / dt : 0.0;
    fprintf(stderr,
            "[%s] guest: %.1f MIPS real (%.1f M blocks/s, avg %.1f instr/block, "
            "%llu new blocks translated, %llu chains patched)\n",
            CODENAME, mips,
            dt > 0 ? d_blocks / 1e6 / dt : 0.0,
            d_blocks > 0 ? static_cast<double>(d_instr) / d_blocks : 0.0,
            static_cast<unsigned long long>(d_blocks_tr),
            static_cast<unsigned long long>(d_chains));
    // Block-end reasons: why translated blocks stop early (structural
    // branch vs JIT caps). Deltas so a phase-local spike is visible.
    fprintf(stderr,
            "[%s] block-end: natural=%llu entry=%llu call_interp=%llu "
            "bl_call=%llu max_size=%llu decode_fail=%llu interp_only=%llu\n",
            CODENAME,
            static_cast<unsigned long long>(natural - last_natural_),
            static_cast<unsigned long long>(entry - last_entry_),
            static_cast<unsigned long long>(ci_cap - last_ci_cap_),
            static_cast<unsigned long long>(bl_cap - last_bl_cap_),
            static_cast<unsigned long long>(max_sz - last_max_sz_),
            static_cast<unsigned long long>(decfail - last_decfail_),
            static_cast<unsigned long long>(ionly - last_ionly_));
    last_instr_     = instr;     last_blocks_     = blocks;
    last_blocks_tr_ = blocks_tr; last_chains_     = chains;
    last_natural_   = natural;   last_entry_      = entry;
    last_ci_cap_    = ci_cap;    last_bl_cap_     = bl_cap;
    last_max_sz_    = max_sz;    last_decfail_    = decfail;
    last_ionly_     = ionly;
    // SIGPROF bucket snapshot too: games/loops that exit via exit_group
    // (or are killed by a timeout) never reach print_jit_stats's exit-time
    // dump, so the jit/dispatch/translate/interp split was unobservable
    // without a clean exit. Inert unless BIFROST_PROF=1.
    dump_prof_snapshot();
    // Syscall histogram (attributes the SIGPROF "other" bucket). Always
    // counted; printed whenever this periodic reporter runs.
    dump_syscall_histogram(dt);
}
void Emulator::jit_step(CPU& cpu) {
    jit_->run_block(cpu, *this);
}
// Runs the callee until it returns (PC = LR). This means dispatching
// multiple blocks in a loop — the callee's first block only executes
// part of the function. We must keep dispatching until RET sets PC=LR.
extern "C" uint64_t jit_call_helper(CPU* cpu, Emulator* emu, uint64_t target_pc) {
    uint64_t return_pc = cpu->regs[30];  // LR set by BL_CALL's STORE_REG
    cpu->pc = target_pc;
    auto* jit = emu->jit();
    int steps = 0;
    while (cpu->running && cpu->pc != return_pc) {
        if (jit) {
            // Fast path mirrors run_block (last-block + inline caches) but
            // ALSO populates them — jit_call_helper is the only entry point
            // for BL_CALL/BLR_CALL targets and previously never wrote the
            // caches, so every worldgen noise call (grad3 ×8 per noise3,
            // ~40K compute calls per fresh chunk column) fell to the
            // mutex-protected unordered_map. The caches are thread-local
            // and written exactly like run_block's slow path.
            int ic = 0;
            auto fn = jit->lookup_call_target(*emu, cpu->pc, ic);
            if (fn) {
                cpu->pc = fn(cpu, emu);
                if (++steps > 10000000) break;
                continue;
            }
        }
        emu->step(*cpu);
        if (++steps > 10000000) break;
    }
    return cpu->pc;
}
} // namespace arm64emu
