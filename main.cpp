// main.cpp — bifrost-emu: fast ARM64 Linux user-mode emulator
//
// "A bridge between worlds" — runs static AArch64 Linux binaries on x86_64.
//
// Version: 1.5.0.alpha
//
// Usage:
//   bifrost-emu [options] <elf-file> [args...]
//
// Options:
//   -d, --debug     trace every instruction to stderr
//   -v, --verbose   print execution stats on exit
//   -q, --quiet     suppress BRK warnings (even with -d)
//   --fb-dump PATH  dump the /dev/fb0 framebuffer to PATH on exit (PPM)
//   --audio-dump PATH  dump /dev/dsp PCM to PATH on exit (WAV)
//   --raw-tty       force raw TTY mode (per-character input, no echo)
//                   default is to leave the host TTY alone so guest
//                   line-buffered stdio (fgets, gets, readline) works
//   --no-jit        disable frostJIT and use the interpreter; this is the
//                   escape hatch for programs that hit a JIT bug or for
//                   debugging the interpreter directly. JIT is on by
//                   default — the full test suite, toybox, and musl/glibc
//                   libc all pass under it (5-6x speedup on compute workloads).
//   --jit           enable frostJIT (now the default; kept for backwards-
//                   compatibility with existing scripts)
//   --jit-threshold N  interpret for the first N instructions, then JIT.
//                       0 = JIT from start (default). Useful for short
//                       programs where JIT compilation overhead dominates.
//   --rootfs PATH   set rootfs for dynamic linking (equivalent to
//                   BIFROST_ROOT=PATH)
//   --config PATH   load config from PATH (overrides the auto-search)
//   --print-config  print the resolved config and exit
//   -V, --version   show version and exit
//   -h, --help      show this help
//
// By default bifrost-emu is silent: only the emulated program's
// stdout/stderr appears. No stats, no exit codes, no noise — just the
// output.
#include "bifrost/emulator.hpp"
#include "bifrost/config.hpp"
#include "decoder.hpp"
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <termios.h>
#include <unistd.h>
using arm64emu::Emulator;
using arm64emu::VERSION;
// ── Banner (shown on no-args, --help) ────────────────────────────────────
static void print_banner() {
    fprintf(stderr,
        "\n"
        "  ____ _____ ______ _____   ____   _____ _______ \n"
        " |  _ \\_   _|  ____|  __ \\ / __ \\ / ____|__   __|\n"
        " | |_) || | | |__  | |__) | |  | | (___    | |   \n"
        " |  _ < | | |  __| |  _  /| |  | |\\___ \\   | |   \n"
        " | |_) || |_| |    | | \\ \\| |__| |____) |  | |   \n"
        " |____/_____|_|    |_|  \\_\\\\____/|_____/   |_|   \n"
        "\n"
        "  bifrost-emu  v%s\n"
        "  A bridge between worlds\n"
        "  x86_64 <─────────────────> ARM64\n"
        "\n"
        "  Usage: bifrost-emu [options] <elf-file> [args...]\n"
        "\n"
        "  Options:\n"
        "    -d, --debug        trace every instruction to stderr\n"
        "    -v, --verbose      print execution stats on exit\n"
        "    -q, --quiet        suppress BRK warnings (even with -d)\n"
        "    --fb-dump PATH     dump /dev/fb0 to PATH on exit (PPM)\n"
        "    --audio-dump PATH  dump /dev/dsp to PATH on exit (WAV)\n"
        "    --raw-tty          force raw TTY mode (per-char input, no echo)\n"
        "    --no-jit           use interpreter only (JIT is on by default)\n"
        "    --jit              enable frostJIT (default; for compatibility)\n"
        "    --jit-threshold N  interpret for N insns then switch to JIT\n"
        "    --rootfs PATH      set rootfs for dynamic linking (BIFROST_ROOT)\n"
        "    --config PATH      load config from PATH\n"
        "    --print-config     print resolved config and exit\n"
        "    -V, --version      show version and exit\n"
        "    -h, --help         show this message\n"
        "\n"
        "  Runs static AArch64 Linux ELF binaries on x86_64.\n"
        "  Default mode is silent — only program output is shown.\n"
        "\n",
        VERSION);
}
// ── Hidden easter egg (--bifrost / --rainbow) ────────────────────────────
static void print_rainbow() {
    fprintf(stderr,
        "\n"
        "       _ x86_64                    ARM64 _\n"
        "      | |                           | |\n"
        "      | |   ____ _____ ______ _____ | |\n"
        "      | |  |  _ \\_   _|  ____|  __ \\| |\n"
        "      | |__| |_) || | | |__  | |__) |_|\n"
        "      |____|  _ <| | |  __| |  _  /<_>\n"
        "      | |  | |_) || |_| |    | | \\ \\| |\n"
        "      |_|  |____/_____|_|    |_|  \\_\\_|\n"
        "\n"
        "  \"The bridge trembles, but holds.\"\n"
        "   — an ancient emulator proverb\n\n"
        "  v%s\n\n",
        VERSION);
}
// ── Terminal raw mode for interactive apps ───────────────────────────────
//
// Default: leave the host TTY alone. The host kernel's line discipline
// already does the right thing for the vast majority of guest programs
// (fgets, gets, scanf, getline, …): it buffers a line, delivers the
// whole line on Enter, and echoes characters so the user can see what
// they typed. Raw mode is opt-in via --raw-tty for guests that
// genuinely need per-character input (e.g. a guest terminal emulator
// or curses-style UI that does its own line editing).
static struct termios orig_termios;
static bool term_set = false;
static void restore_terminal() {
    if (term_set) {
        tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
        term_set = false;
    }
}
static void set_raw_terminal() {
    if (!isatty(STDIN_FILENO)) return;
    if (tcgetattr(STDIN_FILENO, &orig_termios) != 0) return;
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    term_set = true;
    atexit(restore_terminal);
}
// ── Quick ELF sanity check (friendlier than letting the loader throw) ────
static bool looks_like_static_aarch64_elf(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    unsigned char hdr[20];
    size_t n = fread(hdr, 1, sizeof(hdr), f);
    fclose(f);
    if (n < 20) return false;
    // ELF magic + 64-bit + little-endian + AArch64
    return hdr[0] == 0x7f && hdr[1] == 'E' && hdr[2] == 'L' && hdr[3] == 'F'
        && hdr[4] == 2        // ELFCLASS64
        && hdr[5] == 1        // ELFDATA2LSB
        && hdr[18] == 0xB7 && hdr[19] == 0x00;  // e_machine = EM_AARCH64
}
// Map a guest path to its host path under the BIFROST_ROOT sandbox.
// Mirrors yggdrasil/host.cpp's map_guest_path (config rootfs_path ==
// BIFROST_ROOT). Used for host-side pre-checks (access/ELF sniffing)
// so guest-style absolute paths like /usr/local/bin/app.elf resolve to
// "$BIFROST_ROOT/usr/local/bin/app.elf".
static std::string host_path_for(const std::string& guest_path,
                                 const std::string& rootfs) {
    if (rootfs.empty()) return guest_path;
    if (guest_path.empty() || guest_path[0] != '/') return guest_path;
    if (guest_path.rfind("/proc", 0) == 0 || guest_path.rfind("/dev", 0) == 0)
        return guest_path;
    std::string root(rootfs);
    while (root.size() > 1 && root.back() == '/') root.pop_back();
    return root + guest_path;
}
// ── Main ─────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    bool debug   = false;
    bool verbose = false;
    bool quiet   = false;
    bool raw_tty = false;
    // JIT is ON by default. Use --no-jit to force the interpreter.
    // The interpreter is the fallback for programs that hit a JIT bug
    // or for debugging.
    bool use_jit = true;
    std::string fb_dump_path;
    std::string audio_dump_path;
    uint64_t jit_threshold = 0;  // 0 = use JIT from start
    int  arg_i   = 1;
    // ── Config (v1.5.0.alpha) ─────────────────────────────────────────
    // Resolution order: CLI > env var > config file > defaults.
    // We load the config file first, then env vars, then CLI flags
    // override on top.
    arm64emu::Config cfg = arm64emu::Config::defaults();
    std::string config_path = arm64emu::find_config_file();
    bool print_config_only = false;
    // First pass: scan for --config PATH so we can load it before parsing
    // the rest of the flags. Other flags are parsed in the main loop
    // below, AFTER the config file is loaded, so they override config.
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--config") {
            if (i + 1 < argc) { config_path = argv[i + 1]; i++; }
        } else if (a == "--print-config") {
            print_config_only = true;
        }
    }
    if (!config_path.empty()) {
        std::string err;
        if (!cfg.load_from_file(config_path, err)) {
            fprintf(stderr, "bifrost-emu: config error in '%s': %s\n",
                    config_path.c_str(), err.c_str());
            return 2;
        }
    }
    // Apply env vars (they override the file).
    cfg.apply_env();
    while (arg_i < argc) {
        std::string a = argv[arg_i];
        if (a == "-h" || a == "--help")     { print_banner();   return 0; }
        if (a == "-V" || a == "--version")  { printf("bifrost-emu %s\n", VERSION); return 0; }
        if (a == "-d" || a == "--debug")    { debug   = true;  arg_i++; continue; }
        if (a == "-v" || a == "--verbose")  { verbose = true;  arg_i++; continue; }
        if (a == "-q" || a == "--quiet")    { quiet   = true;  arg_i++; continue; }
        if (a == "--raw-tty")               { raw_tty = true;  arg_i++; continue; }
        if (a == "--jit")                   { use_jit = true;  arg_i++; continue; }
        if (a == "--no-jit")                { use_jit = false; arg_i++; continue; }
        if (a == "--config")                { arg_i += 2; continue; }  // already handled
        if (a == "--print-config")          { arg_i++; continue; }
        // Equivalent to `BIFROST_ROOT=PATH bifrost-emu ...` but more
        // ergonomic and works even when the env var isn't inherited.
        if (a == "--rootfs") {
            if (arg_i + 1 >= argc) {
                fprintf(stderr, "bifrost-emu: --rootfs requires a PATH argument\n");
                return 2;
            }
            setenv("BIFROST_ROOT", argv[arg_i + 1], 1);
            arg_i += 2;
            continue;
        }
        // --jit-threshold N: use the interpreter for the first N
        // instructions, then switch to JIT. Useful for short programs
        // where JIT compilation overhead exceeds the runtime. Typical
        // values: 1000-100000. 0 = use JIT from start (default).
        if (a == "--jit-threshold") {
            if (arg_i + 1 >= argc) {
                fprintf(stderr, "bifrost-emu: --jit-threshold requires a NUMBER argument\n");
                return 2;
            }
            char* endp = nullptr;
            unsigned long long val = strtoull(argv[arg_i + 1], &endp, 0);
            if (*endp != '\0' || endp == argv[arg_i + 1]) {
                fprintf(stderr, "bifrost-emu: --jit-threshold: invalid number '%s'\n",
                        argv[arg_i + 1]);
                return 2;
            }
            jit_threshold = val;
            arg_i += 2;
            continue;
        }
        // `--` is the standard POSIX end-of-options separator. Treat
        // the NEXT argument as the ELF file, even if it starts with `-`.
        // This lets users run programs whose path looks like a flag
        // (e.g. `bifrost-emu -- ./-myprogram`) and matches the
        // convention used by qemu-user.
        if (a == "--") {
            arg_i++;
            break;
        }
        if (a == "--fb-dump") {
            if (arg_i + 1 >= argc) {
                fprintf(stderr, "bifrost-emu: --fb-dump requires a PATH argument\n");
                return 2;
            }
            fb_dump_path = argv[arg_i + 1];
            arg_i += 2;
            continue;
        }
        if (a == "--audio-dump") {
            if (arg_i + 1 >= argc) {
                fprintf(stderr, "bifrost-emu: --audio-dump requires a PATH argument\n");
                return 2;
            }
            audio_dump_path = argv[arg_i + 1];
            arg_i += 2;
            continue;
        }
        if (a == "--bifrost" || a == "--rainbow") { print_rainbow(); arg_i++; continue; }
        if (a.size() >= 1 && a[0] == '-' && a.size() > 1) {
            fprintf(stderr, "bifrost-emu: unknown option: %s (try --help)\n", a.c_str());
            return 2;
        }
        break;
    }
    // CLI overrides config file values (highest precedence).
    if (debug)               cfg.log_trace = true;
    if (verbose)             cfg.log_verbose = true;
    if (quiet)               cfg.log_brk_verbose = false;  // -q suppresses BRK warnings
    if (!use_jit)            cfg.jit_enabled = false;
    if (jit_threshold)       cfg.jit_threshold = jit_threshold;
    if (!fb_dump_path.empty())     cfg.fb_dump_path = fb_dump_path;
    if (!audio_dump_path.empty())  cfg.audio_dump_path = audio_dump_path;
    cfg.validate();
    // --print-config: dump the resolved config and exit. Useful for
    // debugging "why isn't my config taking effect?"
    if (print_config_only) {
        std::string dump;
        cfg.dump(dump);
        fprintf(stderr, "# Resolved config (config_path=%s)\n", config_path.c_str());
        fputs(dump.c_str(), stderr);
        return 0;
    }
    // No file given → show the Banner
    if (arg_i >= argc) { print_banner(); return 0; }
    std::string elf_path = argv[arg_i];
    std::vector<std::string> guest_argv;
    guest_argv.push_back(elf_path);
    for (int i = arg_i + 1; i < argc; i++) guest_argv.push_back(argv[i]);
    // Friendly error if the file is missing or not a static AArch64 ELF.
    // Pre-checks run against the host-side (BIFROST_ROOT-remapped) path;
    // load_elf_file still receives the guest path so /proc/self/exe stays
    // consistent with the guest filesystem view.
    const std::string host_elf = host_path_for(elf_path, cfg.rootfs_path);
    if (access(host_elf.c_str(), R_OK) != 0) {
        fprintf(stderr, "bifrost-emu: cannot open '%s': %s\n",
                host_elf.c_str(), strerror(errno));
        return 127;  // 127 = "command not found" convention
    }
    if (!looks_like_static_aarch64_elf(host_elf)) {
        fprintf(stderr,
            "bifrost-emu: '%s' is not a 64-bit little-endian AArch64 ELF.\n"
            "bifrost-emu only runs static AArch64 Linux binaries.\n",
            host_elf.c_str());
        return 126;  // 126 = "found but not executable" convention
    }
    // Only switch the host TTY into raw mode if the user explicitly asked
    // for it. Default: leave the TTY alone so the host line discipline can
    // buffer input for guest line-oriented stdio (fgets, gets, scanf, …).
    if (raw_tty) set_raw_terminal();
    Emulator emu;
    emu.set_verbose(cfg.log_verbose);
    emu.set_trace(cfg.log_trace);
    emu.set_brk_verbose(cfg.log_brk_verbose);
    if (cfg.jit_enabled) emu.enable_jit();
    emu.set_jit_threshold(cfg.jit_threshold);
    if (cfg.forward_host_signals) {
        emu.install_host_signal_handlers();  // forward host signals to guest
    }
    try {
        emu.load_elf_file(elf_path, guest_argv);
        int code = emu.run();
        restore_terminal();
        // Optional framebuffer dump on exit. Useful for headless
        // debugging of programs that draw to /dev/fb0.
        if (!cfg.fb_dump_path.empty() && emu.graphics().ready()) {
            // Sync the guest's framebuffer writes back to the host's
            // fb_data_ before dumping. The emulator's mmap handler
            // allocates separate pages for the guest and does NOT
            // propagate writes back to the host memfd, so we have to
            // copy the guest's pages here.
            uint64_t gaddr = emu.graphics().guest_fb_addr();
            if (gaddr != 0) {
                std::vector<uint8_t> buf(emu.graphics().size());
                try {
                    emu.mem().read(gaddr, buf.data(), buf.size());
                    emu.graphics().sync_from(buf.data());
                } catch (const std::exception&) {
                    // guest address no longer mapped — skip sync
                }
            }
            if (emu.graphics().dump_to_ppm(cfg.fb_dump_path)) {
                if (cfg.log_verbose) {
                    fprintf(stderr, "[emu] framebuffer dumped to '%s'\n",
                            cfg.fb_dump_path.c_str());
                }
            } else {
                fprintf(stderr, "[emu] framebuffer dump failed\n");
            }
        }
        // Optional audio dump on exit — writes accumulated PCM to a WAV file.
        if (!cfg.audio_dump_path.empty() && emu.audio().ready()) {
            if (emu.audio().dump_to_wav(cfg.audio_dump_path)) {
                if (cfg.log_verbose) {
                    fprintf(stderr, "[emu] audio dumped to '%s'\n",
                            cfg.audio_dump_path.c_str());
                }
            } else {
                fprintf(stderr, "[emu] audio dump failed (no data?)\n");
            }
        }
        return code;
    } catch (const std::exception& e) {
        restore_terminal();
        fprintf(stderr, "bifrost-emu: %s\n", e.what());
        return 1;
    }
}
