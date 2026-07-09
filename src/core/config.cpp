// bifrost/config.cpp — Config system implementation (v1.5.0.alpha).
//
// Tiny TOML-subset parser (~200 LOC) + env-var bridge + validators.
// See include/bifrost/config.hpp for the format spec.
#include "bifrost/config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_map>

namespace arm64emu {

// ── Helpers ────────────────────────────────────────────────────────────

static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}

static std::string strip_comment(const std::string& s) {
    // Strip a trailing # comment, honoring that # inside a quoted string
    // is not a comment. (We don't currently allow # in unquoted values
    // without being a comment — same as TOML.)
    bool in_quote = false;
    char quote = 0;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (in_quote) {
            if (c == quote) in_quote = false;
            else if (c == '\\' && i + 1 < s.size()) i++;  // skip escaped char
        } else {
            if (c == '"' || c == '\'') { in_quote = true; quote = c; }
            else if (c == '#') return s.substr(0, i);
        }
    }
    return s;
}

static bool parse_bool(const std::string& v, bool& out) {
    std::string s = trim(v);
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (s == "true" || s == "yes" || s == "on" || s == "1") { out = true; return true; }
    if (s == "false" || s == "no" || s == "off" || s == "0") { out = false; return true; }
    return false;
}

static bool parse_int(const std::string& v, int64_t& out) {
    std::string s = trim(v);
    if (s.empty()) return false;
    try {
        size_t pos = 0;
        int base = 10;
        if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            base = 16;
            pos = 2;
        }
        out = std::stoll(s.substr(pos), nullptr, base);
        return true;
    } catch (...) { return false; }
}

// (parse_float removed — no float config keys currently exist. If one is
// added later, reintroduce a std::stod-based parser here.)

// Strip surrounding quotes from a string value. Returns true if quotes
// were present and stripped; false (and leaves `v` unchanged) otherwise.
static bool unquote(std::string& v) {
    std::string s = trim(v);
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                          (s.front() == '\'' && s.back() == '\''))) {
        // Process common escape sequences for double-quoted strings.
        std::string inner = s.substr(1, s.size() - 2);
        if (s.front() == '"') {
            std::string out;
            out.reserve(inner.size());
            for (size_t i = 0; i < inner.size(); i++) {
                char c = inner[i];
                if (c == '\\' && i + 1 < inner.size()) {
                    char n = inner[++i];
                    switch (n) {
                        case 'n': out += '\n'; break;
                        case 't': out += '\t'; break;
                        case 'r': out += '\r'; break;
                        case '"': out += '"'; break;
                        case '\\': out += '\\'; break;
                        default:  out += '\\'; out += n; break;
                    }
                } else {
                    out += c;
                }
            }
            v = out;
        } else {
            // Single-quoted: literal, no escapes (TOML convention).
            v = inner;
        }
        return true;
    }
    return false;
}

// ── Config::defaults ───────────────────────────────────────────────────
Config Config::defaults() {
    Config c;
    // All fields already have their default values from the struct
    // definition. This method exists for explicitness and to give a
    // single point of truth if defaults change.
    return c;
}

// ── Config::load_from_string ───────────────────────────────────────────
// Parses a TOML-subset string and applies it to this Config. Unknown
// sections / keys are silently ignored (forward-compat). Malformed lines
// produce an error message in `err` and abort parsing.
bool Config::load_from_string(const std::string& text, std::string& err) {
    std::istringstream ss(text);
    std::string line;
    int lineno = 0;
    std::string section;

    auto set_field = [&](const std::string& key, const std::string& raw_val) -> bool {
        // [jit]
        if (section == "jit") {
            if (key == "enabled")     { bool v; if (!parse_bool(raw_val, v)) return false; jit_enabled = v; return true; }
            if (key == "threshold")   { int64_t v; if (!parse_int(raw_val, v)) return false; jit_threshold = static_cast<uint64_t>(v); return true; }
            if (key == "verify")      { bool v; if (!parse_bool(raw_val, v)) return false; jit_verify = v; return true; }
            if (key == "fwd")         { bool v; if (!parse_bool(raw_val, v)) return false; jit_fwd = v; return true; }
            if (key == "thread_jit")  { bool v; if (!parse_bool(raw_val, v)) return false; jit_thread_jit = v; return true; }
            if (key == "no_chain")    { bool v; if (!parse_bool(raw_val, v)) return false; jit_no_chain = v; return true; }
            if (key == "no_selfloop") { bool v; if (!parse_bool(raw_val, v)) return false; jit_no_selfloop = v; return true; }
            if (key == "no_wex")      { bool v; if (!parse_bool(raw_val, v)) return false; jit_no_wex = v; return true; }
            if (key == "no_fma3")     { bool v; if (!parse_bool(raw_val, v)) return false; jit_no_fma3 = v; return true; }
        }
        // [fb]
        if (section == "fb") {
            if (key == "width")   { int64_t v; if (!parse_int(raw_val, v)) return false; fb_width = static_cast<uint32_t>(v); return true; }
            if (key == "height")  { int64_t v; if (!parse_int(raw_val, v)) return false; fb_height = static_cast<uint32_t>(v); return true; }
            if (key == "bpp")     { int64_t v; if (!parse_int(raw_val, v)) return false; fb_bpp = static_cast<uint8_t>(v); return true; }
            if (key == "dump")    { std::string v = raw_val; unquote(v); fb_dump_path = v; return true; }
        }
        // [audio]
        if (section == "audio") {
            if (key == "sample_rate") { int64_t v; if (!parse_int(raw_val, v)) return false; audio_sample_rate = static_cast<uint32_t>(v); return true; }
            if (key == "channels")    { int64_t v; if (!parse_int(raw_val, v)) return false; audio_channels = static_cast<uint8_t>(v); return true; }
            if (key == "sample_size") { int64_t v; if (!parse_int(raw_val, v)) return false; audio_sample_size = static_cast<uint8_t>(v); return true; }
            if (key == "dump")        { std::string v = raw_val; unquote(v); audio_dump_path = v; return true; }
        }
        // [thunk]
        if (section == "thunk") {
            if (key == "graphics") { bool v; if (!parse_bool(raw_val, v)) return false; thunk_graphics = v; return true; }
            if (key == "audio")    { bool v; if (!parse_bool(raw_val, v)) return false; thunk_audio = v; return true; }
            if (key == "display")  { bool v; if (!parse_bool(raw_val, v)) return false; thunk_display = v; return true; }
            if (key == "trace")    { bool v; if (!parse_bool(raw_val, v)) return false; thunk_trace = v; return true; }
        }
        // [paths]
        if (section == "paths") {
            if (key == "rootfs")   { std::string v = raw_val; unquote(v); rootfs_path = v; return true; }
            if (key == "cwd")      { std::string v = raw_val; unquote(v); guest_cwd = v; return true; }
        }
        // [signal]
        if (section == "signal") {
            if (key == "forward_host")  { bool v; if (!parse_bool(raw_val, v)) return false; forward_host_signals = v; return true; }
            if (key == "trace_syscalls"){ bool v; if (!parse_bool(raw_val, v)) return false; trace_syscalls = v; return true; }
        }
        // [perf]
        if (section == "perf") {
            if (key == "stats_interval") { int64_t v; if (!parse_int(raw_val, v)) return false; perf_stats_interval = static_cast<uint64_t>(v); return true; }
        }
        // [log]
        if (section == "log") {
            if (key == "verbose")      { bool v; if (!parse_bool(raw_val, v)) return false; log_verbose = v; return true; }
            if (key == "trace")        { bool v; if (!parse_bool(raw_val, v)) return false; log_trace = v; return true; }
            if (key == "brk_verbose")  { bool v; if (!parse_bool(raw_val, v)) return false; log_brk_verbose = v; return true; }
        }
        // Unknown section / key — silently ignored for forward compat.
        return true;
    };

    while (std::getline(ss, line)) {
        lineno++;
        std::string l = strip_comment(line);
        l = trim(l);
        if (l.empty()) continue;

        // Section header.
        if (l.front() == '[') {
            if (l.back() != ']') {
                err = "line " + std::to_string(lineno) + ": missing ']' in section header";
                return false;
            }
            section = trim(l.substr(1, l.size() - 2));
            continue;
        }

        // key = value
        size_t eq = l.find('=');
        if (eq == std::string::npos) {
            err = "line " + std::to_string(lineno) + ": missing '=' in key=value";
            return false;
        }
        std::string key = trim(l.substr(0, eq));
        std::string val = trim(l.substr(eq + 1));
        if (key.empty()) {
            err = "line " + std::to_string(lineno) + ": empty key";
            return false;
        }
        if (!set_field(key, val)) {
            err = "line " + std::to_string(lineno) + ": invalid value for '" + key + "' in [" + section + "]";
            return false;
        }
    }
    return true;
}

// ── Config::load_from_file ─────────────────────────────────────────────
bool Config::load_from_file(const std::string& path, std::string& err) {
    if (path.empty()) return true;
    if (path == "-") {
        std::stringstream ss;
        ss << std::cin.rdbuf();
        return load_from_string(ss.str(), err);
    }
    std::ifstream f(path);
    if (!f) {
        // Missing file is not an error — it just means no config.
        return true;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return load_from_string(ss.str(), err);
}

// ── Config::apply_env ──────────────────────────────────────────────────
// Apply BIFROST_* env vars on top of the config. Env vars always win
// over the config file (precedence rule). Inverted knobs (e.g.
// BIFROST_NO_THREAD_JIT) flip the corresponding positive field.
void Config::apply_env() {
    auto env_bool = [](const char* name, bool& out) {
        if (const char* v = getenv(name)) {
            bool b;
            if (parse_bool(v, b)) out = b;
        }
    };
    auto env_str = [](const char* name, std::string& out) {
        if (const char* v = getenv(name)) out = v;
    };

    // [jit]
    // BIFROST_NO_JIT is an INVERTED knob: BIFROST_NO_JIT=1 means
    // jit_enabled=false, BIFROST_NO_JIT=0 means jit_enabled=true. We must
    // NOT pass it through env_bool() directly because that would set
    // jit_enabled to the parsed bool (inverted meaning). Handle it
    // explicitly here.
    if (const char* v = getenv("BIFROST_NO_JIT")) {
        bool b;
        if (parse_bool(v, b)) jit_enabled = !b;
    }
    env_bool("BIFROST_JIT_VERIFY",     jit_verify);
    env_bool("BIFROST_ENABLE_FWD",     jit_fwd);
    // BIFROST_NO_THREAD_JIT inverts jit_thread_jit
    if (const char* v = getenv("BIFROST_NO_THREAD_JIT")) {
        bool b;
        if (parse_bool(v, b)) jit_thread_jit = !b;
    }
    env_bool("BIFROST_NO_CHAIN",       jit_no_chain);
    env_bool("BIFROST_NO_SELFLOOP",    jit_no_selfloop);
    env_bool("BIFROST_NO_WEX",         jit_no_wex);
    env_bool("BIFROST_NO_FMA3",        jit_no_fma3);
    if (const char* v = getenv("BIFROST_JIT_THRESHOLD")) {
        int64_t n;
        if (parse_int(v, n) && n >= 0) jit_threshold = static_cast<uint64_t>(n);
    }

    // [thunk]
    env_bool("BIFROST_THUNK_GRAPHICS", thunk_graphics);
    env_bool("BIFROST_THUNK_AUDIO",    thunk_audio);
    env_bool("BIFROST_THUNK_DISPLAY",  thunk_display);
    env_bool("BIFROST_THUNK_TRACE",    thunk_trace);

    // [paths]
    env_str ("BIFROST_ROOT",           rootfs_path);

    // [signal]
    env_bool("BIFROST_SYSCALL_TRACE",  trace_syscalls);

    // [log]
    env_bool("BIFROST_VERBOSE",        log_verbose);
    env_bool("BIFROST_TRACE",          log_trace);
}

// ── Config::dump ───────────────────────────────────────────────────────
void Config::dump(std::string& out) const {
    std::ostringstream ss;
    ss << "# bifrost-emu config (v1.5.0.alpha)\n\n";

    ss << "[jit]\n";
    ss << "enabled     = " << (jit_enabled     ? "true" : "false") << "\n";
    ss << "threshold   = " << jit_threshold    << "\n";
    ss << "verify      = " << (jit_verify      ? "true" : "false") << "\n";
    ss << "fwd         = " << (jit_fwd         ? "true" : "false") << "\n";
    ss << "thread_jit  = " << (jit_thread_jit  ? "true" : "false") << "\n";
    ss << "no_chain    = " << (jit_no_chain    ? "true" : "false") << "\n";
    ss << "no_selfloop = " << (jit_no_selfloop ? "true" : "false") << "\n";
    ss << "no_wex      = " << (jit_no_wex      ? "true" : "false") << "\n";
    ss << "no_fma3     = " << (jit_no_fma3     ? "true" : "false") << "\n";
    ss << "\n";

    ss << "[fb]\n";
    ss << "width  = " << fb_width  << "\n";
    ss << "height = " << fb_height << "\n";
    ss << "bpp    = " << static_cast<int>(fb_bpp) << "\n";
    ss << "dump   = \"" << fb_dump_path << "\"\n";
    ss << "\n";

    ss << "[audio]\n";
    ss << "sample_rate = " << audio_sample_rate << "\n";
    ss << "channels    = " << static_cast<int>(audio_channels) << "\n";
    ss << "sample_size = " << static_cast<int>(audio_sample_size) << "\n";
    ss << "dump        = \"" << audio_dump_path << "\"\n";
    ss << "\n";

    ss << "[thunk]\n";
    ss << "graphics = " << (thunk_graphics ? "true" : "false") << "\n";
    ss << "audio    = " << (thunk_audio    ? "true" : "false") << "\n";
    ss << "display  = " << (thunk_display  ? "true" : "false") << "\n";
    ss << "trace    = " << (thunk_trace    ? "true" : "false") << "\n";
    ss << "\n";

    ss << "[paths]\n";
    ss << "rootfs = \"" << rootfs_path << "\"\n";
    ss << "cwd    = \"" << guest_cwd   << "\"\n";
    ss << "\n";

    ss << "[signal]\n";
    ss << "forward_host    = " << (forward_host_signals ? "true" : "false") << "\n";
    ss << "trace_syscalls  = " << (trace_syscalls       ? "true" : "false") << "\n";
    ss << "\n";

    ss << "[perf]\n";
    ss << "stats_interval = " << perf_stats_interval << "\n";
    ss << "\n";

    ss << "[log]\n";
    ss << "verbose     = " << (log_verbose     ? "true" : "false") << "\n";
    ss << "trace       = " << (log_trace       ? "true" : "false") << "\n";
    ss << "brk_verbose = " << (log_brk_verbose ? "true" : "false") << "\n";
    ss << "\n";

    out = ss.str();
}

// ── Config::validate ───────────────────────────────────────────────────
// Clamp out-of-range values and fix inconsistent combinations. Returns
// the number of fixes applied (0 = clean config).
int Config::validate() {
    int fixes = 0;

    // FB sanity: at least 1x1, at most 8K x 8K (avoids OOM on bogus
    // configs). bpp must be 16, 24, or 32.
    if (fb_width == 0)        { fb_width = 1280; fixes++; }
    if (fb_height == 0)       { fb_height = 720;  fixes++; }
    if (fb_width  > 8192)     { fb_width  = 8192; fixes++; }
    if (fb_height > 8192)     { fb_height = 8192; fixes++; }
    if (fb_bpp != 16 && fb_bpp != 24 && fb_bpp != 32) { fb_bpp = 32; fixes++; }

    // Audio sanity.
    if (audio_sample_rate == 0)              { audio_sample_rate = 44100; fixes++; }
    if (audio_sample_rate > 384000)          { audio_sample_rate = 384000; fixes++; }
    if (audio_channels == 0 || audio_channels > 8) { audio_channels = 2; fixes++; }
    if (audio_sample_size != 1 && audio_sample_size != 2 && audio_sample_size != 3 && audio_sample_size != 4) {
        audio_sample_size = 2; fixes++;
    }

    // JIT threshold is uint64; huge values effectively mean "never JIT"
    // which is fine. Negative thresholds (which can't happen because
    // the field is unsigned) would also be fine.

    // guest_cwd must be absolute.
    if (!guest_cwd.empty() && guest_cwd[0] != '/') {
        guest_cwd = "/" + guest_cwd;
        fixes++;
    }

    return fixes;
}

// ── find_config_file ───────────────────────────────────────────────────
std::string find_config_file() {
    // 1. $BIFROST_CONFIG
    if (const char* p = getenv("BIFROST_CONFIG")) {
        if (*p) {
            std::ifstream f(p);
            if (f) return p;
        }
    }
    // 2. ./bifrost.toml
    {
        std::ifstream f("bifrost.toml");
        if (f) return "bifrost.toml";
    }
    // 3. $XDG_CONFIG_HOME/bifrost/config.toml or ~/.config/bifrost/config.toml
    {
        std::string path;
        if (const char* xdg = getenv("XDG_CONFIG_HOME")) {
            if (*xdg) path = std::string(xdg) + "/bifrost/config.toml";
        }
        if (path.empty()) {
            if (const char* home = getenv("HOME")) {
                if (*home) path = std::string(home) + "/.config/bifrost/config.toml";
            }
        }
        if (!path.empty()) {
            std::ifstream f(path);
            if (f) return path;
        }
    }
    // 4. ~/.bifrost.toml
    {
        if (const char* home = getenv("HOME")) {
            if (*home) {
                std::string path = std::string(home) + "/.bifrost.toml";
                std::ifstream f(path);
                if (f) return path;
            }
        }
    }
    // 5. /etc/bifrost.toml
    {
        std::ifstream f("/etc/bifrost.toml");
        if (f) return "/etc/bifrost.toml";
    }
    return "";
}

} // namespace arm64emu
