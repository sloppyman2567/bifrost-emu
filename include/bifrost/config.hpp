// bifrost/config.hpp — bifrost-emu configuration system (1.5.2-alpha).
//
// bifrost-emu historically configured itself through CLI flags and a
// growing pile of BIFROST_* env vars (BIFROST_NO_THREAD_JIT,
// BIFROST_JIT_VERIFY, BIFROST_ENABLE_FWD, BIFROST_THUNK_GRAPHICS,
// BIFROST_SYSCALL_TRACE, …). 1.5.2-alpha unifies all of these into a
// single Config struct that can be loaded from a TOML-ish file, set
// programmatically by C API consumers, or filled in from CLI flags by
// main.cpp.
//
// Resolution precedence (highest to lowest):
//   1. CLI flag (e.g. --no-jit, --fb-dump PATH)
//   2. Env var (e.g. BIFROST_NO_JIT=1)
//   3. Config file ([jit] enabled = false)
//   4. Built-in defaults (see Config::defaults())
//
// The config file is a tiny TOML subset:
//   - [section] headers
//   - key = value lines (value is string, int, bool, or float)
//   - # line comments and trailing # comments
//   - bare keys ([A-Za-z0-9_]+)
// We deliberately don't support arrays, inline tables, multi-line
// strings, or quoted keys — that's enough for our use case and the
// parser is <200 lines.
//
// Example .bifrost.toml:
//
//   # ~/.bifrost.toml — global user defaults
//   [jit]
//   enabled = true
//   threshold = 0           # 0 = use JIT from start
//   verify = false
//   fwd = false
//   thread_jit = true
//
//   [fb]
//   width = 1280
//   height = 720
//   bpp = 32
//   dump = ""               # PPM path on exit (empty = no dump)
//
//   [audio]
//   sample_rate = 44100
//   channels = 2
//   sample_size = 2         # bytes per sample (16-bit signed)
//   dump = ""               # WAV path on exit
//
//   [thunk]
//   graphics = false        # BIFROST_THUNK_GRAPHICS
//   audio = false           # new in 1.5.2-alpha
//   display = false         # new in 1.5.2-alpha
//   trace = false           # BIFROST_THUNK_TRACE
//
//   [paths]
//   rootfs = ""             # BIFROST_ROOT
//   cwd = "/"               # guest-side cwd
//
//   [signal]
//   forward_host = true     # forward SIGINT/SIGTERM/SIGCHLD to guest
//   trace_syscalls = false  # BIFROST_SYSCALL_TRACE
//
//   [perf]
//   no_chain = false        # BIFROST_NO_CHAIN
//   no_selfloop = false     # BIFROST_NO_SELFLOOP
//   no_wex = false          # BIFROST_NO_WEX (security vs perf)
//   no_fma3 = false         # BIFROST_NO_FMA3
#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace arm64emu {
// ── Config ─────────────────────────────────────────────────────────────
// All knobs that affect emulator behavior. Each field has a sensible
// default; only override what you need.
struct Config {
    // ── [jit] ────────────────────────────────────────────────────────
    bool     jit_enabled       = true;
    uint64_t jit_threshold     = 0;        // 0 = JIT from start
    bool     jit_verify        = false;    // BIFROST_JIT_VERIFY
    bool     jit_fwd           = false;    // BIFROST_ENABLE_FWD
    bool     jit_thread_jit    = true;     // BIFROST_NO_THREAD_JIT (inverted)
    bool     jit_no_chain      = false;    // BIFROST_NO_CHAIN
    bool     jit_no_selfloop   = false;    // BIFROST_NO_SELFLOOP
    bool     jit_no_wex        = false;    // BIFROST_NO_WEX
    bool     jit_no_fma3       = false;    // BIFROST_NO_FMA3
    // ── [fb] ─────────────────────────────────────────────────────────
    uint32_t fb_width          = 1280;
    uint32_t fb_height         = 720;
    uint8_t  fb_bpp            = 32;
    std::string fb_dump_path;             // PPM path on exit (empty = none)
    // ── [audio] ──────────────────────────────────────────────────────
    uint32_t audio_sample_rate = 44100;
    uint8_t  audio_channels    = 2;
    uint8_t  audio_sample_size = 2;
    std::string audio_dump_path;          // WAV path on exit
    // ── [thunk] ──────────────────────────────────────────────────────
    bool     thunk_graphics    = false;
    bool     thunk_audio       = false;
    bool     thunk_display     = false;
    bool     thunk_trace       = false;
    // ── [paths] ──────────────────────────────────────────────────────
    std::string rootfs_path;              // BIFROST_ROOT (empty = no sandbox)
    std::string guest_cwd     = "/";
    // ── [signal] ─────────────────────────────────────────────────────
    bool     forward_host_signals = true;
    bool     trace_syscalls    = false;
    // ── [perf] ───────────────────────────────────────────────────────
    // Per-CPU instruction counter is always on; this controls the
    // periodic stats printout interval (in instructions). 0 = silent.
    uint64_t perf_stats_interval = 0;
    // ── [log] ────────────────────────────────────────────────────────
    bool     log_verbose       = false;
    bool     log_trace         = false;    // instruction trace (-d)
    bool     log_brk_verbose   = true;     // BRK warnings (always on)
    // ── Methods ──────────────────────────────────────────────────────
    // Build a Config from the built-in defaults.
    static Config defaults();
    // Load a TOML-ish config file. Unknown sections / keys are silently
    // ignored (so old configs work after new keys are added). Returns
    // true on success, false on parse error (with `err` filled in).
    //
    // `path` is "-" for stdin, or a filesystem path. Missing files are
    // NOT an error — they just leave the Config unchanged.
    bool load_from_file(const std::string& path, std::string& err);
    // Load a config from a string (same format as the file). Used by
    // tests and by the C API's bifrost_config_load_string().
    bool load_from_string(const std::string& text, std::string& err);
    // Apply env vars on top of the current config. BIFROST_FOO=1 sets
    // the corresponding bool to true (or, for inverted knobs like
    // BIFROST_NO_THREAD_JIT, sets jit_thread_jit to false).
    void apply_env();
    // Dump the config to `out` in the same TOML-ish format the parser
    // accepts. Useful for `bifrost-emu --print-config` and for tests.
    void dump(std::string& out) const;
    // Validate the config: clamp out-of-range values, fix inconsistent
    // combinations (e.g. fb_width = 0 → default). Returns the number of
    // fixes applied (0 = clean config).
    int validate();
};
// ── Path helpers ───────────────────────────────────────────────────────
// Search these locations (in order) for a config file. Returns the first
// match, or "" if none found.
//   1. $BIFROST_CONFIG (if set and non-empty)
//   2. ./bifrost.toml
//   3. $XDG_CONFIG_HOME/bifrost/config.toml (or ~/.config/bifrost/config.toml)
//   4. ~/.bifrost.toml
//   5. /etc/bifrost.toml
std::string find_config_file();
} // namespace arm64emu
