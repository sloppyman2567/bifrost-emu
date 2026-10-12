// DebugFlags.hpp — centralized trace-mode gate for the emulator.
//
// All bifrost_* diagnostic switches are parsed ONCE at first use and cached,
// so the hot syscall/interp paths never call getenv() repeatedly.
//
// Setting BIFROST_TRACE=1 OR-enables the whole diagnostic trace suite;
// the fine-grained BIFROST_* switches only add (they cannot disable a
// flag under blanket mode). Path-typed switches (capture sinks) are
// cached as strings.
#pragma once

#include <cstdlib>
#include <algorithm>
#include <string>
#include <cstdint>
#include <utility>
#include <vector>

namespace bifrost {

struct DebugFlags {
    // ── X protocol wire / fd-lifecycle traces ─────────────────────────
    size_t jit_cache_initial_mb = 64; // BIFROST_JIT_CACHE_INITIAL_MB: growth-regression diagnostic
    size_t jit_cache_mb = 1024; // BIFROST_JIT_CACHE_MB: bounded cache-budget diagnostic (1..1024 MiB)
    int jit_block_limit = 32; // BIFROST_JIT_BLOCK_LIMIT: bounded instruction-cap comparison (8..128)
    bool xtrace   = false;  // BIFROST_XTRACE     — X wire + fd lifecycle
    bool xfull    = false;  // BIFROST_XFULL      — full X request payload dumps
    bool xbt      = false;  // BIFROST_XBT        — X opcode backtraces
    bool xprobe   = false;  // BIFROST_XPROBE     — X request probes
    bool xwrcheck = false;  // BIFROST_XWRCHECK   — X write-corruption checks
    bool xrecv    = false;  // BIFROST_XRECV_TRACE— X receive traces
    bool xdelay   = false;  // BIFROST_XDELAY     — X delayed-delivery sim
    // ── poll / futex / thread diagnostics ────────────────────────────
    bool ppoll      = false;  // BIFROST_PPOLL_TRACE — ppoll entry/revents
    bool ppoll_peek = false;  // BIFROST_PPOLL_PEEK  — msgs peek before/after
    bool futex_trace= false;  // BIFROST_FUTEX_TRACE — futex WAIT/WAKE lines
    bool futex_bt   = false;  // BIFROST_FUTEX_BT    — futex-wait backtraces
    bool allbt      = false;  // BIFROST_ALLBT      — dump all thread stacks
    bool allbt2     = false;  // BIFROST_ALLBT2      — allbt frame walk
    bool btraw      = false;  // BIFROST_BTRAW       — raw (unsymbolized) frames
    bool exec_trace = false;  // BIFROST_EXEC_TRACE  — thread exec tracing
    // ── loader / interpreter traces ──────────────────────────────────
    bool ld2  = false;  // BIFROST_LD2_DBG  — second-run dynamic-linker
    // ── JIT codegen diagnostics ──────────────────────────────────────
    bool regalloc_stats = false;  // BIFROST_REGALLOC_STATS — per-block spill/reload density
    bool tier2 = false; // BIFROST_TIER2=1 — opt in to experimental region promotion
    bool tier2_trace = false; // BIFROST_TIER2_TRACE — compiled candidates and timing decisions
    bool tier2_trace_verbose = false; // BIFROST_TIER2_TRACE_VERBOSE — discovery and compiler details
    unsigned jit_superblock_limit = 8; // BIFROST_JIT_SUPERBLOCK_LIMIT=2..8
    bool no_chain = false; // BIFROST_NO_CHAIN — disable block/region edges
    bool jit_verify = false;  // BIFROST_JIT_VERIFY: presence enables differential replay
    bool no_ssa = false; // BIFROST_NO_SSA — compare with the legacy scalar IR pipeline
    bool ir_validate = false;  // BIFROST_IR_VALIDATE — per-op IR param contract checks after translate
    using Ranges = std::vector<std::pair<uint64_t, uint64_t>>;  // [lo, hi) guest pcs
    Ranges fwd_ranges;   // BIFROST_FWD_RANGES=lo-hi,...  — FWD only for blocks starting here
    Ranges block_trace;  // BIFROST_BLOCK_TRACE=lo-hi,... — log every entry of blocks starting here
    bool fault_trace = false;  // BIFROST_FAULT_TRACE — one line per fault signal + its sigreturn
    bool process_trace = false; // BIFROST_PROCESS_TRACE — shared-VM process clones and group exits
    bool jit_software_guard = false; // BIFROST_JIT_SOFTWARE_GUARD — compare with checked memory accesses
    // ── graphics-thunk traces ────────────────────────────────────────
    bool thunk_trace = false;  // BIFROST_THUNK_TRACE — GL/GLFW/SDL thunk dispatch
    std::vector<uint64_t> input_watch; // BIFROST_INPUT_WATCH — comma-separated guest u32 addresses
    bool input_trace = false;  // BIFROST_INPUT_TRACE — bounded SDL mouse/input-mode log
    // BIFROST_ANDROID_LOGICAL_PIXELS — A/B control: open the Android window
    // at its desktop size (compositor-upscaled, 160 dpi), not native density.
    bool android_logical_pixels = false;
    bool window_stats_title = true;
    bool window_stats_window = false;
    bool window_stats_overlay = false;
    bool render_log = true;
    bool frame_trace = false;  // BIFROST_FRAME_TRACE — per-present frame counter
    bool cpu_profile = false;  // BIFROST_PROF — sampling on every guest thread
    // BIFROST_HANG_WATCHDOG — abort same-PC/frozen tight loops and demote
    // long-running blocks to the interpreter. A codegen-bug diagnostic:
    // real Linux never stops a spinning program, so it is off by default.
    bool hang_watchdog = false;
    bool jit_dispatch_stats = false; // BIFROST_JIT_DISPATCH_STATS — TLS cache telemetry
    bool no_code_page_filter = false; // BIFROST_NO_CODE_PAGE_FILTER — invalidation A/B control
    bool jit_perf_map = false; // BIFROST_PERF_MAP — /tmp/perf-<pid>.map guest block names
    // ── crash / decode diagnostics ───────────────────────────────────
    bool crash_dump = false;  // BIFROST_CRASH_DUMP  — detailed crash report
    bool dbg_guard  = false;  // BIFROST_DBG_GUARD   — decode-error backtraces
    bool trace_crash = false;  // BIFROST_TRACE_CRASH — BRK #1000 regs dump
    bool dynlink_trace = false;  // BIFROST_DYNLINK_TRACE — loader/dlsym lines
    bool nan_trace = false;  // BIFROST_NAN_TRACE — nan writes in fp helpers
    bool simd_trace = false;  // BIFROST_SIMD_TRACE — unhandled simd log
    bool simd_collect = false;  // BIFROST_SIMD_COLLECT — enumerate-and-nop mode
    bool trace_mmap = false;  // BIFROST_TRACE_MMAP — mmap/munmap/mremap lines
    bool binder_trace = false; // BIFROST_BINDER_TRACE — transaction failures and registry calls
    bool trace_madvise = false;  // BIFROST_TRACE_MADVISE — madvise lines
    bool watch_trap = false;  // BIFROST_WATCH_TRAP — SIGTRAP (host) on the
                              // first write hitting the BIFROST_WATCH range
    // ── capture sinks (paths) ────────────────────────────────────────
    std::string xseqlog;  // BIFROST_XSEQLOG — X wire sequence capture file
    std::string xcap;     // BIFROST_XCAP    — X capture output root
    // ── watch / store-log ──────────────────────────────────────────
    std::string watch;  // BIFROST_WATCH — addr[:size] hex; log every
                        // Memory::write overlapping it with the current
                        // interp pc (heap wild-write hunt; --no-jit).
    std::string fallback_profile; // BIFROST_FALLBACK_PROFILE=path: complete per-thread inventory
    std::string write_trace;  // BIFROST_WRITE_TRACE=path — complete guest
                              // store log (addr/size/tid/guest pc/value)
    std::string write_trace_range;  // BIFROST_WRITE_TRACE_RANGE=lo:hi hex
                                    // filter; empty = log everything

    // True when a store-time guest pc is needed (watch or store log active).
    bool need_store_pc() const { return !watch.empty() || !write_trace.empty(); }

    // Return the process-wide flags, parsed lazily on first use.
    static const DebugFlags& get() {
        static const DebugFlags f = parse();
        return f;
    }

    static bool env(const char* name) { return std::getenv(name) != nullptr; }

    static bool in_ranges(const Ranges& ranges, uint64_t pc) {
        for (const auto& r : ranges)
            if (pc >= r.first && pc < r.second) return true;
        return false;
    }

 private:
    // "lo-hi[,lo-hi...]" (any strtoull base, e.g. 0x70000000-0x71000000).
    // Parsing stops at the first malformed or empty range.
    static Ranges parse_ranges(const char* p) {
        Ranges ranges;
        while (p && *p) {
            char* end = nullptr;
            uint64_t lo = std::strtoull(p, &end, 0);
            if (end == p || *end != '-') break;
            const char* q = end + 1;
            uint64_t hi = std::strtoull(q, &end, 0);
            if (end == q || (*end && *end != ',') || hi <= lo) break;
            ranges.push_back({lo, hi});
            p = *end ? end + 1 : end;
        }
        return ranges;
    }

    static DebugFlags parse() {
        DebugFlags f;
        if (const char* limit = std::getenv("BIFROST_JIT_BLOCK_LIMIT")) {
            char* end = nullptr;
            unsigned long n = std::strtoul(limit, &end, 10);
            if (end != limit && *end == '\0' && n >= 8 && n <= 128)
                f.jit_block_limit = static_cast<int>(n);
        }
        if (const char* budget = std::getenv("BIFROST_JIT_CACHE_MB")) {
            char* end = nullptr;
            unsigned long mb = std::strtoul(budget, &end, 10);
            if (end != budget && *end == '\0' && mb >= 1 && mb <= 1024)
                f.jit_cache_mb = static_cast<size_t>(mb);
        }
        if (const char* initial = std::getenv("BIFROST_JIT_CACHE_INITIAL_MB")) {
            char* end = nullptr;
            unsigned long mb = std::strtoul(initial, &end, 10);
            if (end != initial && *end == '\0' && mb >= 1 && mb <= 1024)
                f.jit_cache_initial_mb = static_cast<size_t>(mb);
        }
        if (const char* path = std::getenv("BIFROST_FALLBACK_PROFILE")) f.fallback_profile = path;
        const bool all = env("BIFROST_TRACE");
        f.xtrace    = all || env("BIFROST_XTRACE");
        f.xfull     = all || env("BIFROST_XFULL");
        f.xbt       = all || env("BIFROST_XBT");
        f.xprobe    = all || env("BIFROST_XPROBE");
        f.xwrcheck  = all || env("BIFROST_XWRCHECK");
        f.xrecv     = all || env("BIFROST_XRECV_TRACE");
        f.xdelay    = all || env("BIFROST_XDELAY");
        f.ppoll       = all || env("BIFROST_PPOLL_TRACE");
        f.ppoll_peek  = all || env("BIFROST_PPOLL_PEEK");
        f.futex_trace = all || env("BIFROST_FUTEX_TRACE");
        f.futex_bt    = all || env("BIFROST_FUTEX_BT");
        f.allbt     = all || env("BIFROST_ALLBT");
        f.allbt2    = all || env("BIFROST_ALLBT2");
        f.btraw     = all || env("BIFROST_BTRAW");
        f.exec_trace= all || env("BIFROST_EXEC_TRACE");
        f.ld2       = all || env("BIFROST_LD2_DBG");
        f.regalloc_stats = all || env("BIFROST_REGALLOC_STATS");
        f.no_ssa = env("BIFROST_NO_SSA");
        f.ir_validate = all || env("BIFROST_IR_VALIDATE");
        f.fwd_ranges = parse_ranges(std::getenv("BIFROST_FWD_RANGES"));
        f.block_trace = parse_ranges(std::getenv("BIFROST_BLOCK_TRACE"));
        f.fault_trace = all || env("BIFROST_FAULT_TRACE");
        f.process_trace = all || env("BIFROST_PROCESS_TRACE");
        f.jit_software_guard = env("BIFROST_JIT_SOFTWARE_GUARD") || env("BIFROST_GUARD_ALLOCS") || env("BIFROST_JIT_SLOW_STORES");
        f.no_chain = env("BIFROST_NO_CHAIN");
        f.jit_verify = std::getenv("BIFROST_JIT_VERIFY") != nullptr;
        f.thunk_trace = all || env("BIFROST_THUNK_TRACE");
        f.input_trace = all || env("BIFROST_INPUT_TRACE");
        f.android_logical_pixels = env("BIFROST_ANDROID_LOGICAL_PIXELS");
        if (const char* p = std::getenv("BIFROST_INPUT_WATCH")) {
            while (*p && f.input_watch.size() < 32) {
                char* end = nullptr;
                uint64_t address = std::strtoull(p, &end, 0);
                if (end == p || (*end && *end != ',')) break;
                f.input_watch.push_back(address);
                p = *end ? end + 1 : end;
            }
        }
        auto toggle = [](const char* key, bool fallback) {
            const char* v = std::getenv(key);
            return v ? std::string(v) != "0" && std::string(v) != "false" : fallback;
        };
        f.tier2 = toggle("BIFROST_TIER2", false);
        f.tier2_trace_verbose = env("BIFROST_TIER2_TRACE_VERBOSE");
        f.tier2_trace = env("BIFROST_TIER2_TRACE") || f.tier2_trace_verbose;
        if (const char* value = std::getenv("BIFROST_JIT_SUPERBLOCK_LIMIT"))
            f.jit_superblock_limit = std::clamp(std::strtoul(value, nullptr, 10), 2UL, 8UL);
        f.window_stats_title = toggle("BIFROST_WINDOW_STATS_TITLE", toggle("BIFROST_WINDOW_STATS", true));
        f.window_stats_window = toggle("BIFROST_WINDOW_STATS_WINDOW", false);
        f.window_stats_overlay = toggle("BIFROST_WINDOW_STATS_OVERLAY", false);
        f.render_log = toggle("BIFROST_RENDER_LOG", true);
        f.frame_trace = all || env("BIFROST_FRAME_TRACE");
        f.cpu_profile = env("BIFROST_PROF");
        f.hang_watchdog = env("BIFROST_HANG_WATCHDOG");
        f.no_code_page_filter = env("BIFROST_NO_CODE_PAGE_FILTER");
        f.jit_perf_map = env("BIFROST_PERF_MAP");
        f.jit_dispatch_stats = env("BIFROST_JIT_DISPATCH_STATS");
        f.crash_dump = all || env("BIFROST_CRASH_DUMP");
        f.dbg_guard  = all || env("BIFROST_DBG_GUARD");
        f.trace_crash = all || env("BIFROST_TRACE_CRASH");
        f.dynlink_trace = all || env("BIFROST_DYNLINK_TRACE");
        f.nan_trace = all || env("BIFROST_NAN_TRACE");
        f.simd_trace = all || env("BIFROST_SIMD_TRACE");
        f.simd_collect = all || env("BIFROST_SIMD_COLLECT");
        f.trace_mmap = all || env("BIFROST_TRACE_MMAP");
        f.binder_trace = all || env("BIFROST_BINDER_TRACE");
        f.trace_madvise = all || env("BIFROST_TRACE_MADVISE");
        f.watch_trap = all || env("BIFROST_WATCH_TRAP");
        if (const char* p = std::getenv("BIFROST_XSEQLOG")) f.xseqlog = p;
        if (const char* p = std::getenv("BIFROST_XCAP"))    f.xcap    = p;
        if (const char* p = std::getenv("BIFROST_WATCH"))   f.watch   = p;
        if (const char* p = std::getenv("BIFROST_WRITE_TRACE")) f.write_trace = p;
        if (const char* p = std::getenv("BIFROST_WRITE_TRACE_RANGE")) f.write_trace_range = p;
        return f;
    }
};

}  // namespace bifrost

// Convenience accessor at global scope so it is reachable from the
// arm64emu:: syscall/interp namespaces without qualification.
inline const bifrost::DebugFlags& dbg() { return bifrost::DebugFlags::get(); }
