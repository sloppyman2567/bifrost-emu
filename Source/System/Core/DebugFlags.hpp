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

#include <climits>
#include <cstdlib>
#include <unistd.h>
#include <algorithm>
#include <string>
#include <cstdint>
#include <utility>
#include <vector>

namespace bifrost {

struct DebugFlags {
    int media_backend = 0; // BIFROST_MEDIA_BACKEND=auto/cpu/gpu; gpu forbids silent CPU fallback
    bool media_trace = false; // BIFROST_MEDIA_TRACE — decoded frame transport
    // ── X protocol wire / fd-lifecycle traces ─────────────────────────
    size_t jit_cache_initial_mb = 64; // BIFROST_JIT_CACHE_INITIAL_MB: growth-regression diagnostic
    size_t jit_cache_mb = 512; // BIFROST_JIT_CACHE_MB: code-cache budget before the JIT is replaced (1..1024 MiB)
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
    bool no_shared_futex = false;  // BIFROST_NO_SHARED_FUTEX — futexes on shared file mappings use the in-process table
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
    bool tier2_graph = false; // BIFROST_TIER2_GRAPH — report the natural loop around each hot head
    bool tier2_trace_verbose = false; // BIFROST_TIER2_TRACE_VERBOSE — discovery and compiler details
    unsigned jit_superblock_limit = 8; // BIFROST_JIT_SUPERBLOCK_LIMIT=2..8
    bool no_chain = false; // BIFROST_NO_CHAIN — disable block/region edges
    bool jit_verify = false;  // BIFROST_JIT_VERIFY: presence enables differential replay
    // Differential-replay options (they only act with BIFROST_JIT_VERIFY):
    uint64_t jit_verify_every = 1;  // BIFROST_JIT_VERIFY_EVERY=N: re-verify each block on its first N dispatches
    bool jit_verify_mem = false;  // BIFROST_JIT_VERIFY_MEM: compare each tracked store's bytes
    bool jit_verify_memfull = false;  // BIFROST_JIT_VERIFY_MEMFULL: compare every byte of the range below
    uint64_t jit_memfull_start = 0x10000000ULL;  // BIFROST_MEMFULL_START: first guest VA of that range
    uint64_t jit_memfull_end = 0x14000000ULL;  // BIFROST_MEMFULL_END: end (exclusive) of that range
    bool verify_untracked = false;  // BIFROST_VERIFY_UNTRACKED: also verify blocks with unresolved stores
    bool verify_trace = false;  // BIFROST_VERIFY_TRACE: print each verified block's entry state
    // ── JIT toggles and diagnostics (presence of the variable turns each on) ──
    bool no_selfloop = false;  // BIFROST_NO_SELFLOOP: keep self-loop blocks as ordinary blocks (no pinned loop body)
    bool no_vec_cache = false;  // BIFROST_NO_VEC_CACHE: no cached vector registers across instructions (A/B control)
    bool no_fp_cache = false;  // BIFROST_NO_FP_CACHE: no cached scalar FP registers across instructions (A/B control)
    bool step_trace = false;  // BIFROST_STEP_TRACE: trace the interpreter steps the JIT bridge takes
    bool no_shared_jit = false;  // BIFROST_NO_SHARED_JIT: one JIT per thread instead of one shared instance
    bool block_prof = false;  // BIFROST_BLOCK_PROF: print the block-end reason counters at exit
    bool pc_hist = false;  // BIFROST_PC_HIST: histogram of the guest pcs the JIT bridge runs
    bool interp_bl_call = false;  // BIFROST_INTERP_BL_CALL: run BL through the interpreter bridge
    bool hot_interp = false;  // BIFROST_HOT_INTERP: demote hot blocks that mostly call the interpreter
    bool jit_trace = false;  // BIFROST_JIT_TRACE: trace each block dispatch
    bool no_callregion = false;  // BIFROST_NO_CALLREGION: Tier-2 regions do not run through calls
    bool no_opt = false;  // BIFROST_NO_OPT: skip the IR optimizer (Tier-1 and Tier-2)
    bool no_fma3 = false;  // BIFROST_NO_FMA3: decomposed FMA codegen even on FMA3 CPUs
    bool no_avx2 = false;  // BIFROST_NO_AVX2: no AVX2 codegen even on AVX2 CPUs
    bool trace_simdst16 = false;  // BIFROST_TRACE_SIMDST16: trace 16-byte vector stores recorded for replay
    bool mem_trace = false;  // BIFROST_MEM_TRACE: trace each memory access the JIT emits
    bool jit_slow_stores = false;  // BIFROST_JIT_SLOW_STORES: route every store through the slow path (BIFROST_WATCH sees JIT stores)
    bool regalloc_check = false;  // BIFROST_REGALLOC_CHECK: vreg bounds checks in release builds
    bool no_bl_call = false;  // BIFROST_NO_BL_CALL: BL ends the block instead of calling the target
    bool no_blr_call = false;  // BIFROST_NO_BLR_CALL: BLR ends the block instead of calling the target
    bool enable_leaf_inline = false;  // BIFROST_ENABLE_LEAF_INLINE: inline leaf callees into the calling block
    bool debug_inline = false;  // BIFROST_DEBUG_INLINE: log leaf-inlining decisions
    bool malloc_interp = false;  // BIFROST_MALLOC_INTERP: run the allocator entry points through the interpreter
    bool class_prof = false;  // BIFROST_CLASS_PROF: instruction-class counts for blocks that fall back to the interpreter
    bool jit_dump = false;  // BIFROST_JIT_DUMP: dump the IR of every compiled block
    bool no_pin = false;  // BIFROST_NO_PIN: no pinned host registers in loops
    bool no_rdce = false;  // BIFROST_NO_RDCE: Tier-2 planner: no redundant-store elimination
    bool no_licm = false;  // BIFROST_NO_LICM: Tier-2 planner: no loop-invariant hoisting
    bool no_destsrc = false;  // BIFROST_NO_DESTSRC: Tier-2 planner: no destination-as-source reuse
    bool no_keepres = false;  // BIFROST_NO_KEEPRES: Tier-2: no resident registers across the loop back-edge
    bool no_exitchain = false;  // BIFROST_NO_EXITCHAIN: Tier-2: region exits do not chain
    bool t2_regiondump = false;  // BIFROST_T2_REGIONDUMP: hexdump each published Tier-2 region
    bool no_chain_skip = false;  // BIFROST_NO_CHAIN_SKIP: chain edges do not skip the frame teardown and prologue
    bool no_direct_call = false;  // BIFROST_NO_DIRECT_CALL: BL sites call through the helper, not a patched direct call
    bool no_fwd = false;  // BIFROST_NO_FWD: skip load/store forwarding in the optimizer
    bool no_dse = false;  // BIFROST_NO_DSE: skip dead-store elimination in the optimizer
    bool no_wex = false;  // BIFROST_NO_WEX: allocate the code buffer writable and executable (no W^X toggling)
    bool dbg_pc_set = false;  // BIFROST_DBG_PC=addr: print the entry and exit state of the block at addr
    uint64_t dbg_pc = 0;
    uint64_t dump_pc = 0;  // BIFROST_DUMP_PC=addr: BIFROST_JIT_DUMP only dumps the block at addr (0 = all)
    bool tier2_hits_fixed = false;  // BIFROST_TIER2_HITS=N: Tier-2 candidates need N entries and skip the rate gate
    uint32_t tier2_hits = 1000;
    int fp_native_gate = -1;  // BIFROST_FP_NATIVE_GATE=mask: FP op classes the translator handles natively (-1 = all)
    bool no_ssa = false; // BIFROST_NO_SSA — compare with the legacy scalar IR pipeline
    bool ir_validate = false;  // BIFROST_IR_VALIDATE — per-op IR param contract checks after translate
    using Ranges = std::vector<std::pair<uint64_t, uint64_t>>;  // [lo, hi) guest pcs
    Ranges fwd_ranges;   // BIFROST_FWD_RANGES=lo-hi,...  — FWD only for blocks starting here
    Ranges block_trace;  // BIFROST_BLOCK_TRACE=lo-hi,... — log every entry of blocks starting here
    bool fault_trace = false;  // BIFROST_FAULT_TRACE — one line per fault signal + its sigreturn
    bool process_trace = false; // BIFROST_PROCESS_TRACE — shared-VM process clones and group exits
    bool ptrace_trace = false; // BIFROST_PTRACE_TRACE — guest tracing requests and results
    bool jit_software_guard = false; // BIFROST_JIT_SOFTWARE_GUARD — compare with checked memory accesses
    // ── graphics-thunk traces ────────────────────────────────────────
    bool thunk_trace = false;  // BIFROST_THUNK_TRACE — GL/GLFW/SDL thunk dispatch
    bool audio_trace = false;  // BIFROST_AUDIO_TRACE — audio thunk calls with arguments and results
    bool audio_dump = false;   // BIFROST_AUDIO_DUMP=1 — keep the guest's PCM for the WAV that --audio-dump writes at exit
    // BIFROST_AUDIO_DUMP=/path — write the guest's PCM to the file as it arrives. The WAV above is written only when the
    // process returns from main, and a managed app's emulator is ended by a signal.
    std::string audio_dump_raw;
    bool vk_no_placed = false; // BIFROST_VK_NO_PLACED — keep vkMapMemory bounces (A/B control for placed maps)
    bool vk_no_astc = false;   // BIFROST_VK_NO_ASTC — do not offer ASTC to Vulkan guests (A/B control for the BC7 transcoder)
    unsigned astc_threads = 0; // BIFROST_ASTC_THREADS — transcoder threads per upload (1..64; 0 = half the host's cores, at most 8)
    unsigned astc_cache_mb = 256; // BIFROST_ASTC_CACHE_MB — finished BC7 transcodes kept for re-uploads (0 = off, at most 4096)
    bool astc_stats = false;   // BIFROST_ASTC_STATS — log transcoded and cached volume every 64 uploads
    // BIFROST_TEXT_SCRIPT — key and text events pushed into the Android window's SDL queue once
    // text input starts: "down:H,text:h;up:H;edit:ni;text:..." (steps by ';' 100 ms apart, items
    // in a step pushed together; down:/up: take an SDL scancode name). For input tests.
    std::string text_script;
    std::vector<uint64_t> input_watch; // BIFROST_INPUT_WATCH — comma-separated guest u32 addresses
    bool input_trace = false;  // BIFROST_INPUT_TRACE — bounded SDL mouse/input-mode log
    // BIFROST_ANDROID_LOGICAL_PIXELS — A/B control: open the Android window
    // at its desktop size (compositor-upscaled, 160 dpi), not native density.
    bool android_logical_pixels = false;
    bool window_stats_title = false;
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
    bool trace_unimplemented = false;  // BIFROST_TRACE_UNIMPLEMENTED — each syscall ending in ENOSYS, each ioctl ending in ENOTTY, once
    bool binder_trace = false; // BIFROST_BINDER_TRACE — transaction failures and registry calls
    bool trace_madvise = false;  // BIFROST_TRACE_MADVISE — madvise lines
    // BIFROST_FORK_STUCK_SECONDS — a fork child that has not finished an execve after this many seconds
    // reports where its guest thread (library+offset) and host thread are blocked; 0 = off.
    unsigned fork_stuck_seconds = 0;
    bool watch_trap = false;  // BIFROST_WATCH_TRAP — SIGTRAP (host) on the
                              // first write hitting the BIFROST_WATCH range
    // ── capture sinks (paths) ────────────────────────────────────────
    std::string xseqlog;  // BIFROST_XSEQLOG — X wire sequence capture file
    std::string xcap;     // BIFROST_XCAP    — X capture output root
    // ── watch / store-log ──────────────────────────────────────────
    std::string watch;  // BIFROST_WATCH — addr[:size] hex; log every
                        // Memory::write overlapping it with the current
                        // interp pc (heap wild-write hunt; --no-jit).
    std::string fallback_profile; // BIFROST_FALLBACK_PROFILE=path: complete per-thread inventory ("%p" = host PID)
    std::string regalloc_profile; // BIFROST_REGALLOC_PROFILE=path: emitted IR ranges and allocation costs
    size_t regalloc_profile_mb = 256; // BIFROST_REGALLOC_PROFILE_MB: per-process size cap of that file (1..65536 MiB)
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
    // Output files stay where the user launched from: a rootfs guest's
    // startup moves the host cwd into BIFROST_ROOT (enter_root_cwd).
    static std::string host_path(const char* path) {
        char cwd[PATH_MAX];
        if (path[0] == '/' || path[0] == '\0' || !::getcwd(cwd, sizeof(cwd))) return path;
        return std::string(cwd) + "/" + path;
    }
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
        if (const char* backend = std::getenv("BIFROST_MEDIA_BACKEND")) {
            if (std::string(backend) == "cpu") f.media_backend = 1;
            else if (std::string(backend) == "gpu") f.media_backend = 2;
        }
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
        if (const char* path = std::getenv("BIFROST_FALLBACK_PROFILE")) f.fallback_profile = host_path(path);
        if (const char* path = std::getenv("BIFROST_REGALLOC_PROFILE")) f.regalloc_profile = host_path(path);
        if (const char* cap = std::getenv("BIFROST_REGALLOC_PROFILE_MB")) {
            char* end = nullptr;
            unsigned long mb = std::strtoul(cap, &end, 10);
            if (end != cap && *end == '\0' && mb >= 1 && mb <= 65536) f.regalloc_profile_mb = static_cast<size_t>(mb);
        }
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
        f.no_shared_futex = env("BIFROST_NO_SHARED_FUTEX");
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
        f.ptrace_trace = all || env("BIFROST_PTRACE_TRACE");
        f.jit_software_guard = env("BIFROST_JIT_SOFTWARE_GUARD") || env("BIFROST_GUARD_ALLOCS") || env("BIFROST_JIT_SLOW_STORES");
        f.no_chain = env("BIFROST_NO_CHAIN");
        f.jit_verify = std::getenv("BIFROST_JIT_VERIFY") != nullptr;
        if (const char* every = std::getenv("BIFROST_JIT_VERIFY_EVERY")) f.jit_verify_every = std::strtoull(every, nullptr, 0);
        f.jit_verify_mem = env("BIFROST_JIT_VERIFY_MEM");
        f.jit_verify_memfull = env("BIFROST_JIT_VERIFY_MEMFULL");
        if (const char* start = std::getenv("BIFROST_MEMFULL_START")) f.jit_memfull_start = std::strtoull(start, nullptr, 0);
        if (const char* end = std::getenv("BIFROST_MEMFULL_END")) f.jit_memfull_end = std::strtoull(end, nullptr, 0);
        f.verify_untracked = env("BIFROST_VERIFY_UNTRACKED");
        f.verify_trace = env("BIFROST_VERIFY_TRACE");
        f.no_selfloop = env("BIFROST_NO_SELFLOOP");
        f.no_vec_cache = env("BIFROST_NO_VEC_CACHE");
        f.no_fp_cache = env("BIFROST_NO_FP_CACHE");
        f.step_trace = env("BIFROST_STEP_TRACE");
        f.no_shared_jit = env("BIFROST_NO_SHARED_JIT");
        f.block_prof = env("BIFROST_BLOCK_PROF");
        f.pc_hist = env("BIFROST_PC_HIST");
        f.interp_bl_call = env("BIFROST_INTERP_BL_CALL");
        f.hot_interp = env("BIFROST_HOT_INTERP");
        f.jit_trace = env("BIFROST_JIT_TRACE");
        f.no_callregion = env("BIFROST_NO_CALLREGION");
        f.no_opt = env("BIFROST_NO_OPT");
        f.no_fma3 = env("BIFROST_NO_FMA3");
        f.no_avx2 = env("BIFROST_NO_AVX2");
        f.trace_simdst16 = env("BIFROST_TRACE_SIMDST16");
        f.mem_trace = env("BIFROST_MEM_TRACE");
        f.jit_slow_stores = env("BIFROST_JIT_SLOW_STORES");
        f.regalloc_check = env("BIFROST_REGALLOC_CHECK");
        f.no_bl_call = env("BIFROST_NO_BL_CALL");
        f.no_blr_call = env("BIFROST_NO_BLR_CALL");
        f.enable_leaf_inline = env("BIFROST_ENABLE_LEAF_INLINE");
        f.debug_inline = env("BIFROST_DEBUG_INLINE");
        f.malloc_interp = env("BIFROST_MALLOC_INTERP");
        f.class_prof = env("BIFROST_CLASS_PROF");
        f.jit_dump = env("BIFROST_JIT_DUMP");
        f.no_pin = env("BIFROST_NO_PIN");
        f.no_rdce = env("BIFROST_NO_RDCE");
        f.no_licm = env("BIFROST_NO_LICM");
        f.no_destsrc = env("BIFROST_NO_DESTSRC");
        f.no_keepres = env("BIFROST_NO_KEEPRES");
        f.no_exitchain = env("BIFROST_NO_EXITCHAIN");
        f.t2_regiondump = env("BIFROST_T2_REGIONDUMP");
        f.no_chain_skip = env("BIFROST_NO_CHAIN_SKIP");
        f.no_direct_call = env("BIFROST_NO_DIRECT_CALL");
        f.no_fwd = env("BIFROST_NO_FWD");
        f.no_dse = env("BIFROST_NO_DSE");
        f.no_wex = env("BIFROST_NO_WEX");
        if (const char* pc = std::getenv("BIFROST_DBG_PC")) { f.dbg_pc_set = true; f.dbg_pc = std::strtoull(pc, nullptr, 0); }
        if (const char* pc = std::getenv("BIFROST_DUMP_PC")) f.dump_pc = std::strtoull(pc, nullptr, 0);
        if (const char* hits = std::getenv("BIFROST_TIER2_HITS")) {
            f.tier2_hits_fixed = true;
            f.tier2_hits = static_cast<uint32_t>(std::strtoul(hits, nullptr, 10));
        }
        if (const char* gate = std::getenv("BIFROST_FP_NATIVE_GATE")) f.fp_native_gate = static_cast<int>(std::strtol(gate, nullptr, 0));
        f.thunk_trace = all || env("BIFROST_THUNK_TRACE");
        f.audio_trace = all || env("BIFROST_AUDIO_TRACE");
        if (const char* dump = std::getenv("BIFROST_AUDIO_DUMP")) {
            if (dump[0] == '1') f.audio_dump = true;
            else if (dump[0] == '/') f.audio_dump_raw = dump;
        }
        f.vk_no_placed = env("BIFROST_VK_NO_PLACED");
        f.vk_no_astc = env("BIFROST_VK_NO_ASTC");
        f.astc_stats = env("BIFROST_ASTC_STATS");
        if (const char* mb = std::getenv("BIFROST_ASTC_CACHE_MB")) {
            char* end = nullptr;
            unsigned long n = std::strtoul(mb, &end, 10);
            if (end != mb && *end == '\0' && n <= 4096) f.astc_cache_mb = static_cast<unsigned>(n);
        }
        if (const char* threads = std::getenv("BIFROST_ASTC_THREADS")) {
            char* end = nullptr;
            unsigned long n = std::strtoul(threads, &end, 10);
            if (end != threads && *end == '\0' && n >= 1 && n <= 64) f.astc_threads = static_cast<unsigned>(n);
        }
        if (const char* script = std::getenv("BIFROST_TEXT_SCRIPT")) f.text_script = script;
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
        f.tier2_graph = env("BIFROST_TIER2_GRAPH");
        f.tier2_trace = env("BIFROST_TIER2_TRACE") || f.tier2_trace_verbose;
        if (const char* value = std::getenv("BIFROST_JIT_SUPERBLOCK_LIMIT"))
            f.jit_superblock_limit = std::clamp(std::strtoul(value, nullptr, 10), 2UL, 8UL);
        // Window stats are opt-in (BIFROST_WINDOW_STATS=1). BIFROST_WINDOW_STATS_MODE picks the layout:
        // 1 or minimal (title statistics), 2 or normal (title and overlay, the default), 3 or overlay
        // (overlay only). The per-display switches below override what the mode implies.
        const bool stats = toggle("BIFROST_WINDOW_STATS", false);
        int stats_mode = 2;
        if (const char* value = std::getenv("BIFROST_WINDOW_STATS_MODE")) {
            const std::string mode = value;
            stats_mode = mode == "1" || mode == "minimal" ? 1 : mode == "3" || mode == "overlay" ? 3 : 2;
        }
        f.window_stats_title = toggle("BIFROST_WINDOW_STATS_TITLE", stats && stats_mode != 3);
        f.window_stats_window = toggle("BIFROST_WINDOW_STATS_WINDOW", false);
        f.window_stats_overlay = toggle("BIFROST_WINDOW_STATS_OVERLAY", stats && stats_mode != 1);
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
        f.trace_unimplemented = all || env("BIFROST_TRACE_UNIMPLEMENTED");
        f.binder_trace = all || env("BIFROST_BINDER_TRACE");
        f.media_trace = env("BIFROST_MEDIA_TRACE");
        f.trace_madvise = all || env("BIFROST_TRACE_MADVISE");
        if (const char* value = std::getenv("BIFROST_FORK_STUCK_SECONDS"))
            f.fork_stuck_seconds = static_cast<unsigned>(std::min(3600UL, std::strtoul(value, nullptr, 10)));
        f.watch_trap = all || env("BIFROST_WATCH_TRAP");
        if (const char* p = std::getenv("BIFROST_XSEQLOG")) f.xseqlog = host_path(p);
        if (const char* p = std::getenv("BIFROST_XCAP"))    f.xcap    = host_path(p);
        if (const char* p = std::getenv("BIFROST_WATCH"))   f.watch   = p;
        if (const char* p = std::getenv("BIFROST_WRITE_TRACE")) f.write_trace = host_path(p);
        if (const char* p = std::getenv("BIFROST_WRITE_TRACE_RANGE")) f.write_trace_range = p;
        return f;
    }
};

}  // namespace bifrost

// Convenience accessor at global scope so it is reachable from the
// arm64emu:: syscall/interp namespaces without qualification.
inline const bifrost::DebugFlags& dbg() { return bifrost::DebugFlags::get(); }
