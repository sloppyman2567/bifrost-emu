// EmulatorBridge.cpp — Glue between Emulator and FrostJIT.
//
// Emulator's constructor/destructor live in System/Core/Emulator/Lifecycle.cpp.
// This file holds the JIT-specific Emulator methods that need to see
// FrostJIT's full definition (held via unique_ptr in Emulator).
#include "System/Core/Emulator.hpp"
#include "System/Core/DebugFlags.hpp"
#include "FrostJIT/Runtime/FallbackProfile.hpp"
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include "FrostJIT/Runtime/SampleProfile.hpp"
#include "FrostJIT/Runtime/Generations.hpp"
#include "System/Syscalls/Syscalls.hpp"
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <mutex>
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
// Samples that land inside the JIT code buffer go to the thread's ring in SampleProfile.cpp, which resolves them
// to a thread and a part of a translated block when the report is printed.
namespace {
struct ProfGuard {
    bool* f_;
    bool active_, saved_;
    explicit ProfGuard(bool* f) : f_(f), active_(prof_enabled.load(std::memory_order_relaxed)), saved_(*f) {
        if (active_) *f_ = true;
    }
    ~ProfGuard() { if (active_) *f_ = saved_; }
};
}
// Internal diagnostic query, also used by the deterministic thread-registration
// regression. It reads exactly the range used by the signal sampler.
bool bifrost_prof_contains_ip(uint64_t rip) {
    return tls_prof_codebuf && rip >= reinterpret_cast<uint64_t>(tls_prof_codebuf) &&
        rip - reinterpret_cast<uint64_t>(tls_prof_codebuf) < tls_prof_codebuf_size;
}
namespace {
static void prof_signal_handler(int, siginfo_t*, void* ctx) {
    auto* uc = static_cast<ucontext_t*>(ctx);
    uint64_t rip = static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RIP]);
    if (bifrost_prof_contains_ip(rip)) {
        prof_jit.fetch_add(1, std::memory_order_relaxed);
        prof_record_sample(rip);
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
static void prof_install() {
    struct sigaction sa = {};
    sa.sa_sigaction = prof_signal_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, nullptr);
    prof_enabled.store(true, std::memory_order_release);
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
    if (!dbg().cpu_profile) return;
    // Registration is per host thread/JIT owner, even though the process
    // timer is installed once. The reserved range remains stable on growth.
    tls_prof_codebuf = jit->code_buf();
    tls_prof_codebuf_size = jit->code_buf_size();
    prof_claim_thread();
    static std::once_flag installed;
    std::call_once(installed, prof_install);
}
bool bifrost_prof_active() {
    return prof_enabled.load(std::memory_order_relaxed);
}
JitSet::~JitSet() {
    // The reaper must not touch an instance this destructor is about to free.
    for (auto& jit : owned_) jit_generations::forget(jit.get());
}
FrostJIT* JitSet::add(std::unique_ptr<FrostJIT> fresh) {
    std::lock_guard<std::mutex> g(mu_);
    FrostJIT* jit = fresh.get();
    owned_.push_back(std::move(fresh));
    live_.push_back(jit);
    current_.store(jit, std::memory_order_release);
    return jit;
}
std::vector<FrostJIT*> JitSet::all() const {
    std::lock_guard<std::mutex> g(mu_);
    std::vector<FrostJIT*> out;
    for (const auto& jit : owned_) out.push_back(jit.get());
    return out;
}
bool jit_released(const FrostJIT* jit) { return jit->released(); }
size_t JitSet::live_snapshot(FrostJIT** out) const {
    std::lock_guard<std::mutex> g(mu_);
    live_.erase(std::remove_if(live_.begin(), live_.end(), [](FrostJIT* jit) { return jit->released(); }),
                live_.end());
    size_t n = 0;
    for (FrostJIT* jit : live_) if (n < kMaxLive) out[n++] = jit;
    return n;
}
void JitSet::abandon() {
    std::lock_guard<std::mutex> g(mu_);
    current_.store(nullptr, std::memory_order_release);
    for (auto& jit : owned_) (void)jit.release();
    owned_.clear();
    live_.clear();
}
void Emulator::configure_jit(FrostJIT& jit) {
    jit.set_direct_window(mem_.direct_window(), &mem_);
    jit.set_vdso_range(vdso_base_, vdso_size_);
    jit.set_renewable(jit_shared_mode_);
}
void Emulator::enable_jit() {
    if (jit_) return;
    // Shared-JIT mode (default): spawned threads share the main's FrostJIT,
    // saving 64 MiB of code-cache per thread. This requires the code buffer to be RWX
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
    jit_shared_mode_ = shared_jit_mode;
    jit_.add(std::make_unique<FrostJIT>());
    jit_enabled_ = static_cast<bool>(jit_);
    jit_wanted_ = jit_enabled_;
    if (jit_enabled_) {
        configure_jit(*jit_.get());
        if (verbose_) {
            const auto& cf = jit_->cpu_features();
            fprintf(stderr, "[%s] frostJIT enabled (x86 codegen, %s mode, "
                    "features: %s)\n",
                    CODENAME, shared_jit_mode ? "shared" : "per-thread",
                    cpu_features_string(cf));
        }
    }
}
bool Emulator::renew_jit(FrostJIT* full) {
    if (jit_generations::unreleased() >= jit_generations::kMaxUnreleased) return false;
    std::lock_guard<std::mutex> g(jit_renew_mu_);
    if (jit_.get() != full) return true;  // another thread already replaced it
    if (jit_generations::unreleased() >= jit_generations::kMaxUnreleased) return false;
    auto fresh = std::make_unique<FrostJIT>();
    configure_jit(*fresh);
    // Join multithreaded mode before any thread can run it (the ticker is already running).
    if (full->mt_active()) fresh->enter_multithreaded();
    const size_t used = full->code_buf_used(), blocks = full->cache_entries();
    jit_.add(std::move(fresh));
    full->retire();
    if (verbose_) {
        size_t retired = 0, released = 0;
        jit_generations::counts(retired, released);
        fprintf(stderr, "[%s] frostJIT: code cache full (%zu KiB, %zu blocks): replaced by a fresh one "
                "(%zu retired, %zu released)\n", CODENAME, used >> 10, blocks, retired, released);
    }
    return true;
}
void Emulator::restart_jit_for_exec() {
    if (!jit_ && !jit_wanted_) return;
    // Abandon the old JIT instead of destroying it: this thread is still inside its code buffer
    // (the execve syscall was called from JIT code and returns through it), and destroying it would
    // unmap that buffer. It is released with the process.
    jit_.abandon();
    jit_enabled_ = false;
    enable_jit();
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
    // Earlier generations of the main JIT: their work counts toward the totals, not the cache size.
    const auto all_jits = jit_.all();
    for (FrostJIT* old : all_jits) {
        if (old == jit_.get()) continue;
        blocks_translated += old->blocks_translated;
        blocks_executed   += old->blocks_executed;
        instructions      += old->instructions_executed;
        cache_hits        += old->cache_hits;
        cache_misses      += old->cache_misses;
        fallbacks         += old->interpreter_fallbacks;
        chains            += old->block_chains_patched;
    }
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
    if (all_jits.size() > 1) {
        size_t retired = 0, released = 0;
        jit_generations::counts(retired, released);
        fprintf(stderr, "[%s] frostJIT: %zu code cache generations, %zu retired, %zu released\n",
                CODENAME, all_jits.size(), retired, released);
    }
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
    jit_->dump_compile_stats();
    jit_->dump_code_cache_stats();
    jit_->dump_memory_stats();
    dump_memory_breakdown();
}
// Periodic (or exit-time) SIGPROF bucket snapshot. Shared by print_jit_stats
// and the BIFROST_STATS_PERIOD reporter so the jit/dispatch/translate/interp
// split can be observed DURING a run phase (e.g. worldgen) without requiring a
// clean guest exit. Inert unless BIFROST_PROF=1.
void Emulator::dump_prof_snapshot() {
    dump_interpreted_profile();
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
    // Where the JIT samples fell: thread, and entry/body/exit/cold of the block. BIFROST_PC_HIST=1 also lists the hottest
    // guest blocks (map a pc to a function with aarch64 objdump). Gated separately so the default run stays short.
    static const bool pc_hist = (getenv("BIFROST_PC_HIST") != nullptr);
    prof_print(stderr, prof_collect(pc_hist ? 20 : 0), pc_hist);
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
    if (!jit_ || !jit_enabled_) {
        // Interpreter mode (--no-jit): no JIT counters, but throughput and
        // the syscall histogram are still useful. interp_count_ counts
        // step() calls on the main CPU (benign racy read from the reporter
        // thread — stats-grade accuracy only).
        static uint64_t last_ic_ = 0;
        uint64_t ic = interp_count_;
        double mips = dt > 0 ? static_cast<double>(ic - last_ic_) / 1e6 / dt : 0.0;
        last_ic_ = ic;
        fprintf(stderr, "[%s] guest(interp): %.1f MIPS real (%llu instructions total)\n",
                CODENAME, mips, static_cast<unsigned long long>(ic));
        dump_syscall_histogram(dt);
        return;
    }
    // Totals over every JIT whose counters are still reachable: all generations of the main JIT
    // (renew_jit replaces a full code cache with a fresh instance whose counters start at zero)
    // and the per-thread JITs of BIFROST_NO_SHARED_JIT.
    uint64_t instr = 0, blocks = 0, blocks_tr = 0, chains = 0;
    uint64_t natural = 0, entry = 0, ci_cap = 0, bl_cap = 0, max_sz = 0, decfail = 0, ionly = 0;
    uint64_t hot_heads = 0, t2_regions = 0, t2_rejections = 0, t2_retries = 0, t2_retired = 0;
    uint64_t t2_ns = 0, t2_bytes = 0;
    auto add = [&](const FrostJIT& j) {
        constexpr auto r = std::memory_order_relaxed;
        instr     += j.instructions_executed.load(r);
        blocks    += j.blocks_executed.load(r);
        blocks_tr += j.blocks_translated.load(r);
        chains    += j.block_chains_patched.load(r);
        natural   += j.block_profile.natural_branch.load(r);
        entry     += j.block_profile.entry_point.load(r);
        ci_cap    += j.block_profile.call_interp_cap.load(r);
        bl_cap    += j.block_profile.bl_call_cap.load(r);
        max_sz    += j.block_profile.max_size.load(r);
        decfail   += j.block_profile.decode_fail.load(r);
        ionly     += j.block_profile.interp_only.load(r);
        hot_heads += j.tier2_hot_heads.load(r);
        t2_regions += j.tier2_regions.load(r);
        t2_rejections += j.tier2_profit_rejections.load(r);
        t2_retries += j.tier2_profile_retries.load(r);
        t2_retired += j.tier2_phase_retirements.load(r);
        t2_ns += j.tier2_compile_ns.load(r);
        t2_bytes += j.tier2_code_bytes.load(r);
    };
    for (FrostJIT* j : jit_.all()) add(*j);
    {
        std::lock_guard<std::mutex> g(threads_mu_);
        for (auto& gt : threads_)
            if (gt->jit) add(*gt->jit);
    }
    // deltas since the last dump. A total can still fall when counters leave the sum (exec starts
    // a fresh JIT, a per-thread JIT exits with its thread); the delta is then the new total rather
    // than an unsigned wrap.
    auto delta = [](uint64_t now, uint64_t before) { return now >= before ? now - before : now; };
    static uint64_t last_instr_, last_blocks_, last_blocks_tr_, last_chains_;
    static uint64_t last_natural_, last_entry_, last_ci_cap_, last_bl_cap_,
                    last_max_sz_, last_decfail_, last_ionly_;
    static uint64_t last_hot_heads_, last_t2_regions_, last_t2_rejections_, last_t2_retries_;
    uint64_t d_instr     = delta(instr, last_instr_);
    uint64_t d_blocks    = delta(blocks, last_blocks_);
    uint64_t d_blocks_tr = delta(blocks_tr, last_blocks_tr_);
    uint64_t d_chains    = delta(chains, last_chains_);
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
            static_cast<unsigned long long>(delta(natural, last_natural_)),
            static_cast<unsigned long long>(delta(entry, last_entry_)),
            static_cast<unsigned long long>(delta(ci_cap, last_ci_cap_)),
            static_cast<unsigned long long>(delta(bl_cap, last_bl_cap_)),
            static_cast<unsigned long long>(delta(max_sz, last_max_sz_)),
            static_cast<unsigned long long>(delta(decfail, last_decfail_)),
            static_cast<unsigned long long>(delta(ionly, last_ionly_)));
    if (t2_retired || t2_bytes)
        fprintf(stderr, "[%s] tier2 lifetime: retired=%llu compile_ms=%.3f code_bytes=%llu\n",
            CODENAME, (unsigned long long)t2_retired, t2_ns / 1e6,
            (unsigned long long)t2_bytes);
    // Tier-2 hot-head count (opt-in): blocks that crossed the
    // BIFROST_TIER2_HITS threshold since the last dump. Inert (0) by default.
    fprintf(stderr,
            "[%s] tier2: hot_heads=%llu regions=%llu rejected=%llu retries=%llu\n", CODENAME,
            static_cast<unsigned long long>(delta(hot_heads, last_hot_heads_)),
            static_cast<unsigned long long>(delta(t2_regions, last_t2_regions_)),
            static_cast<unsigned long long>(delta(t2_rejections, last_t2_rejections_)),
            static_cast<unsigned long long>(delta(t2_retries, last_t2_retries_)));
    last_instr_     = instr;     last_blocks_     = blocks;
    last_blocks_tr_ = blocks_tr; last_chains_     = chains;
    last_natural_   = natural;   last_entry_      = entry;
    last_ci_cap_    = ci_cap;    last_bl_cap_     = bl_cap;
    last_max_sz_    = max_sz;    last_decfail_    = decfail;
    last_ionly_     = ionly;
    last_t2_rejections_ = t2_rejections; last_t2_retries_ = t2_retries;
    last_hot_heads_ = hot_heads;
    last_t2_regions_ = t2_regions;
    // SIGPROF bucket snapshot too: games/loops that exit via exit_group
    // (or are killed by a timeout) never reach print_jit_stats's exit-time
    // dump, so the jit/dispatch/translate/interp split was unobservable
    // without a clean exit. Inert unless BIFROST_PROF=1.
    dump_prof_snapshot();
    jit_->dump_compile_stats();
    jit_->dump_code_cache_stats();
    jit_->dump_memory_stats();
    dump_memory_breakdown();
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
    // BIFROST_INTERP_BL_CALL=1: run every BL_CALL/BLR_CALL callee through
    // the interpreter (bisection gate — confirms corruption originates in a
    // callee block, which jit_call_helper dispatches WITHOUT the run_block
    // verify path, so MEMFULL never sees those blocks).
    static const bool interp_bl_call_ = (getenv("BIFROST_INTERP_BL_CALL") != nullptr);
    // Starts on the instance whose code called this helper; once that instance is retired the
    // loop moves to the current one (below), so a long callee does not interpret forever.
    auto* jit = FrostJIT::running(*emu);
    FrostJIT::FaultScope helper_scope(jit);
    // ── Block-dispatch watchdog (mirrors run_block) ────────────────
    // Thread-local count of dispatches through this helper, checked
    // against GLOBAL_BLOCK_LIMIT (1e12 ≈ 14h of pure dispatch). The old
    // per-invocation `steps > 10000000` cap broke LEGITIMATE long-running
    // BL_CALLs: window_loop's helper dispatches >10M blocks in ~40s of
    // gameplay, so the cap cut it off mid-game and returned a garbage pc
    // (0x41fd7c) into main's block. main's block then overwrote cpu.pc
    // with its static fall-through (0x400f38), main's tail block popped
    // the frame and ret'd through a stack-resident LR (pc=0x3efffbb8) →
    // DecodeError. A legit callee must never be cut off; like run_block's
    // watchdog this is a pure codegen-bug safety valve, opt-in through
    // BIFROST_HANG_WATCHDOG.
    static const bool hang_watchdog = dbg().hang_watchdog;
    thread_local uint64_t tls_call_blocks_ = 0;
    static const bool dbg_call_trace_ = dbg().dbg_guard;
    // Keep nested tiny calls from turning batched counters into contended
    // per-call atomic writes. The outermost return still publishes tails,
    // including callback/nonlocal-return exits; regular loops flush at 64K.
    static thread_local unsigned call_depth = 0;
    struct CallStatsGuard {
        FrostJIT* jit;
        unsigned& depth;
        CallStatsGuard(FrostJIT* j, unsigned& d) : jit(j), depth(d) { ++depth; }
        ~CallStatsGuard() { if (--depth == 0 && jit) jit->flush_stat_tls(); }
    } stats_guard(jit, call_depth);
    static const bool verify_mode_ = (getenv("BIFROST_JIT_VERIFY") != nullptr);
    uint64_t dbg2_count = 0;
    while (cpu->running && !cpu->jit_dispatch_requested && cpu->pc != return_pc &&
           (!cpu->jit_stop_pc || cpu->pc != cpu->jit_stop_pc)) {
        if (__builtin_expect(jit && jit->retired(), 0)) {
            FrostJIT* current = emu->jit();
            if (current && current != jit) { jit = current; FrostJIT::retarget_scope(jit); }
        }
        bool debug_step = emu->debugger_poll(*cpu);
        if (!cpu->running || cpu->jit_dispatch_requested) break;
        if (debug_step) { emu->step(*cpu); continue; }
        // The callee ended at an SVC it left unrun (see emit_call_native_svc).
        if (__builtin_expect(cpu->jit_svc_pc != 0, 0) && jit && jit->run_pending_svc(*cpu, *emu)) continue;
        if (__builtin_expect(hang_watchdog && ++tls_call_blocks_ > FrostJIT::GLOBAL_BLOCK_LIMIT, 0)) {
            if (jit) jit->jit_disabled_.store(true, std::memory_order_relaxed);
            fprintf(stderr, "[JIT] jit_call_helper watchdog: %llu blocks dispatched by a thread — disabling JIT (likely codegen bug)\n",
                    static_cast<unsigned long long>(tls_call_blocks_));
            break;
        }
        if (jit && !interp_bl_call_) {
            // Verify mode: route BL_CALL/BLR_CALL targets through run_block
            // so the register + MEMFULL verify machinery covers callee
            // blocks. lookup_call_target dispatches fn directly, bypassing
            // run_block's verify path — so a corrupting SIMD_ST16 callee
            // (the inflate copy loop) was invisible to BIFROST_JIT_VERIFY
            // (MFTRACE never fired; the block is entered via `bl`, not the
            // dispatcher). Under verify this is slower but catches the bug.
            if (verify_mode_) {
                cpu->pc = jit->run_block(*cpu, *emu);
                continue;
            }
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
                jit_grant_lease(*cpu);
                // A cloned CPU may still carry another thread's table.
                jit->bind_ibl(*cpu);
                uint64_t pcbefore = cpu->pc;
                // Bounded-staleness stats (2026-08-21): count BL_CALL
                // callee blocks into the shared TLS (see run_block) and
                // batch-flush every 64K — a game's frame loop parked in
                // this helper used to be invisible to the stats reporter.
                FrostJIT::tls_stat_instr_ += ic;
                if (__builtin_expect((++FrostJIT::tls_stat_exec_ & 0xFFFF) != 0, 1)) {
                    cpu->pc = fn(cpu, emu);
                } else {
                    jit->flush_stat_tls();
                    cpu->pc = fn(cpu, emu);
                }
                if (dbg_call_trace_) {
                    fprintf(stderr, "[DBG3] %llu: 0x%llx -> 0x%llx (fn=0x%llx sp=0x%llx x30=0x%llx)\n",
                            (unsigned long long)++dbg2_count, (unsigned long long)pcbefore,
                            (unsigned long long)cpu->pc, (unsigned long long)fn,
                            (unsigned long long)cpu->sp, (unsigned long long)cpu->regs[30]);
                    // TEMP-NEVERBALL-DEBUG: regs at loop-block entries.
                    uint64_t pb = pcbefore, pa = cpu->pc;
                    if ((pa >= 0x10570000 && pa < 0x10580000) ||
                        (pb >= 0x10570000 && pb < 0x10580000)) {
                        fprintf(stderr, "[DBG3-REGS 0x%llx->0x%llx] x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx x4=0x%llx x5=0x%llx x6=0x%llx x7=0x%llx x15=0x%llx x19=0x%llx x21=0x%llx\n",
                                (unsigned long long)pb, (unsigned long long)pa,
                                (unsigned long long)cpu->regs[0], (unsigned long long)cpu->regs[1],
                                (unsigned long long)cpu->regs[2], (unsigned long long)cpu->regs[3],
                                (unsigned long long)cpu->regs[4], (unsigned long long)cpu->regs[5],
                                (unsigned long long)cpu->regs[6], (unsigned long long)cpu->regs[7],
                                (unsigned long long)cpu->regs[15],
                                (unsigned long long)cpu->regs[19],
                                (unsigned long long)cpu->regs[21]);
                    }
                }
                continue;
            }
        }
        if (dbg().dbg_guard) {
            fprintf(stderr, "[STEP] pc=0x%llx sp=0x%llx x30=0x%llx\n",
                    (unsigned long long)cpu->pc, (unsigned long long)cpu->sp,
                    (unsigned long long)cpu->regs[30]);
        }
        if (jit) {
            FrostJIT::tls_stat_instr_++;
            if ((++FrostJIT::tls_stat_exec_ & 0xFFFF) == 0) jit->flush_stat_tls();
        }
        // This helper is itself called through generated frames: guest
        // memory exceptions must be handled before crossing that boundary.
        jit_interp_step(emu, cpu);
    }
    return cpu->pc;
}

} // namespace arm64emu
