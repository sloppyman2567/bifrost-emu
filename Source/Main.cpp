#include "FrostJIT/Backend/CpuFeatures.hpp"
#include "System/Android/Binder/Broker.hpp"
#include "System/Android/Media/HostDecoder.hpp"
// Source/Main.cpp — bifrost-emu: fast ARM64 Linux user-mode emulator
//
// "A bridge between worlds" — runs static AArch64 Linux binaries on x86_64.
//
// Usage:
//   bifrost-emu [options] <elf-file> [args...]
//   bifrost-emu <package.apk|.apkm|.apks|.xapk> [args...]   (install and run via Asgard)
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
//   --rootfs PATH   use PATH as the guest root: libraries come from it and
//                   guest paths are confined to it (equivalent to
//                   BIFROST_ROOT=PATH). Without it, a rootfs found next to the
//                   executable or under ~/.local/share/bifrost/rootfs
//                   supplies libraries only.
//   --setup-rootfs [OPTIONS]  build and install the guest rootfs, then exit; takes
//                   install-rootfs.sh's options (--system, --prefix DIR, --full,
//                   --no-fetch, --uninstall). Needs the checkout's Scripts/.
//   --rootfs-profile NAME  use the installed rootfs profile NAME (rootfs.d/NAME) for
//                   libraries only; BIFROST_ROOT / --rootfs still wins. A profile that
//                   is not installed is an error.
//   --rootfs-default NAME  save NAME as the profile to use ("default" for the normal rootfs), then exit
//   --rootfs-list   list the installed rootfs profiles (* marks the one in use), then exit
//   --config PATH   load config from PATH (overrides the auto-search)
//   --print-config  print the resolved config and exit
//   -V, --version   show version and exit
//   -h, --help      show this help
//   --help-all      also show the options Asgard uses internally
//
// Graphical programs log their first presentation by default.
#include "Bifrost/Emulator.hpp"
#include "Bifrost/Config.hpp"
#include "Interpreter/Decoder/Decoder.hpp"
#include "System/Graphics/Display/WindowStats.hpp"
#include "System/Android/Package/PackageLauncher.hpp"
#include "System/Android/Surface/Surface.hpp"
#include "System/Android/Window/WindowBackend.hpp"
#include "System/Syscalls/Files/SocketPolicy.hpp"
#include "System/Loader/RootfsDiscovery.hpp"
#include "System/Loader/RootfsSetup.hpp"
#include <string>
#include <filesystem>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <termios.h>
#include <sys/resource.h>
#include <sys/prctl.h>
#include <signal.h>
#include <climits>
#include <unistd.h>
using arm64emu::Emulator;
using arm64emu::Memory;
using arm64emu::VERSION;
// ── Start page (no arguments), --help and --help-all ────────────────────
static void print_art() {
    printf("\n"
           "  ____ _____ ______ _____   ____   _____ _______ \n"
           " |  _ \\_   _|  ____|  __ \\ / __ \\ / ____|__   __|\n"
           " | |_) || | | |__  | |__) | |  | | (___    | |   \n"
           " |  _ < | | |  __| |  _  /| |  | |\\___ \\   | |   \n"
           " | |_) || |_| |    | | \\ \\| |__| |____) |  | |   \n"
           " |____/_____|_|    |_|  \\_\\\\____/|_____/   |_|   \n"
           "\n"
           "  bifrost-emu  v%s\n"
           "  Run ARM64 Linux programs and Android apps on x86-64\n"
           "\n", VERSION);
}
// What Android needs: Asgard's script, and the toolchain that provisioning adds beside it (a fresh
// checkout has only the script). Uses the search a real launch does, so this page cannot disagree with it.
static std::string find_set_up_asgard(const std::string& exe_dir) {
    for (const auto& path : arm64emu::asgard_candidates(exe_dir, getenv("BIFROST_ASGARD"), getenv("XDG_DATA_HOME"),
                                                        getenv("HOME"))) {
        const std::string suffix = "/Scripts/Android/asgard.py";
        if (path.size() <= suffix.size() || access(path.c_str(), R_OK) != 0) continue;
        const std::string root = path.substr(0, path.size() - suffix.size());
        if (access((root + "/Tools/android-tools/aapt2").c_str(), X_OK) == 0) return path;
    }
    return {};
}
static void print_status_line(const char* what, bool ready, const std::string& fix) {
    if (ready) printf("    %-22sinstalled\n", what);
    else printf("    %-22s%-16s%s\n", what, "not installed", fix.c_str());
}
static void print_start_page() {
    const std::string exe_dir = arm64emu::self_exe_dir();
    const std::string asgard = find_set_up_asgard(exe_dir);
    bool games = false;
    for (const auto& profile : arm64emu::list_rootfs_profiles())
        if (profile.name == "games" && !profile.path.empty()) games = true;
    const bool from_checkout = !asgard.empty() && asgard.rfind(exe_dir + "/Scripts/", 0) == 0;
    const std::string setup_asgard = access((exe_dir + "/asgard-setup").c_str(), X_OK) == 0 ? "asgard-setup"
        : access((exe_dir + "/Scripts/Setup/install-asgard.py").c_str(), R_OK) == 0 ? "Scripts/Setup/install-asgard.py"
        : "see the README";
    print_art();
    printf("  Run something\n"
           "    %-38sa Linux program\n"
           "    %-38sinstall and run an Android app\n", "bifrost-emu ./program [args]", "bifrost-emu app.apk");
    if (!asgard.empty())
        printf("    %-38sopen the app library\n", from_checkout ? "Scripts/Android/asgard.py launcher" : "asgard launcher");
    printf("\n  Status\n");
    const std::string in_use = arm64emu::requested_rootfs_profile();
    if (!in_use.empty() && !arm64emu::library_root().empty()) printf("    %-22sinstalled, using %s\n", "Linux libraries", in_use.c_str());
    else print_status_line("Linux libraries", !arm64emu::library_root().empty(), "bifrost-emu --setup-rootfs");
    print_status_line("Linux game libraries", games, "bifrost-emu --setup-rootfs --games");
    print_status_line("Android (Asgard)", !asgard.empty(), setup_asgard);
    printf("\n  Common options\n"
           "    --setup-rootfs          install or update the libraries Linux programs need\n"
           "    --setup-rootfs --games  add the libraries Linux games need\n"
           "    --rootfs-list           list the installed Linux libraries\n"
           "    --rootfs-profile NAME   use other Linux libraries for this run, e.g. games\n"
           "    --rootfs-default NAME   always use them\n"
           "    --guest-ld              use the program's own ld.so (experimental)\n"
           "    --window-stats          show FPS and load in the window\n"
           "    --no-jit                interpreter only, for debugging\n"
           "\n  bifrost-emu --help shows every option.\n\n");
}
// --rootfs-default NAME: remember which rootfs Linux programs use, without editing a config file by hand.
static int set_default_rootfs(const std::string& name) {
    const bool reset = name == "default";
    if (!reset && !arm64emu::valid_rootfs_profile_name(name)) {
        fprintf(stderr, "bifrost-emu: invalid rootfs profile name '%s'\n", name.c_str());
        return 2;
    }
    if (!reset) {
        bool installed = false;
        for (const auto& profile : arm64emu::list_rootfs_profiles())
            if (profile.name == name && !profile.path.empty()) installed = true;
        if (!installed) {
            fprintf(stderr, "bifrost-emu: rootfs profile '%s' is not installed; nothing changed. See --rootfs-list.\n", name.c_str());
            if (name == "games") fprintf(stderr, "Install it with: bifrost-emu --setup-rootfs --games\n");
            return 2;
        }
    }
    std::string err;
    const bool shadows_system = arm64emu::find_config_file() == "/etc/bifrost.toml";
    const std::string path = arm64emu::save_default_rootfs_profile(reset ? "" : name, err);
    if (path.empty()) { fprintf(stderr, "bifrost-emu: %s\n", err.c_str()); return 1; }
    printf("Linux programs now use the %s rootfs (saved in %s).\n", name.c_str(), path.c_str());
    if (shadows_system) printf("Note: this file is now read instead of /etc/bifrost.toml, so its settings no longer apply.\n");
    // Only the first config file found is read, and the environment beats it.
    const std::string active = arm64emu::find_config_file();
    std::error_code ec;
    if (!active.empty() && !std::filesystem::equivalent(active, path, ec))
        printf("Note: %s is read instead of that file, so this has no effect while it exists.\n", active.c_str());
    if (const char* env = getenv("BIFROST_ROOTFS_PROFILE"); env && *env)
        printf("Note: BIFROST_ROOTFS_PROFILE=%s overrides this in the current environment.\n", env);
    if (const char* env = getenv("BIFROST_ROOT"); env && *env)
        printf("Note: BIFROST_ROOT=%s overrides this in the current environment.\n", env);
    return 0;
}
static void print_help(bool all) {
    print_art();
    printf("  Usage: bifrost-emu [options] <program> [args...]\n"
           "         bifrost-emu <app.apk|.apkm|.apks|.xapk> [-d]   install and run an Android app\n"
           "         Options go before the program: anything after it goes to the program.\n"
           "\n  Libraries\n"
           "    --setup-rootfs [OPTS]   build and install the rootfs Linux programs use, then exit\n"
           "                            (--games, --system, --prefix DIR, --full, --uninstall)\n"
           "                            --games adds the libraries Linux games need. Android apps\n"
           "                            use neither: Asgard brings its own\n"
           "    --rootfs-profile NAME   use the installed rootfs profile NAME for libraries\n"
           "    --rootfs-default NAME   always use that profile, saved in your config, then exit\n"
           "                            (\"default\" goes back to the normal rootfs)\n"
           "    --rootfs-list           list installed rootfs profiles and exit\n"
           "    --rootfs PATH           guest root for libraries and paths (BIFROST_ROOT)\n"
           "    --guest-ld              run dynamic programs under their own ld.so (experimental)\n"
           "\n  Execution\n"
           "    --no-jit                use the interpreter only (the JIT is on by default)\n"
           "    --jit                   use the JIT (the default)\n"
           "    --jit-threshold N       interpret for N instructions, then switch to the JIT\n"
           "    --jit-disk-cache[=verify]  experimental: reuse JIT translations across launches\n"
           "    --no-jit-disk-cache     do not use that cache\n"
           "\n  Display\n"
           "    --window-stats          show presentation stats (off by default)\n"
           "    --window-stats-mode M   1|minimal title, 2|normal title and overlay, 3|overlay\n"
           "    --[no-]window-stats-title|window|overlay  choose the stats displays\n"
           "    --no-window-stats       turn every stats display off\n"
           "    --render-log, --no-render-log  log the first frame presented (on by default)\n"
           "    --raw-tty               force raw terminal input (per character, no echo)\n"
           "\n  Android\n"
           "    --android PATH          run a NativeActivity .so (ANativeActivity_onCreate)\n"
           "\n  Debugging\n"
           "    -d, --debug             trace every instruction to stderr\n"
           "    -v, --verbose           print execution stats on exit\n"
           "    -q, --quiet             suppress BRK warnings (even with -d)\n"
           "    --fb-dump PATH          dump /dev/fb0 to PATH on exit (PPM)\n"
           "    --audio-dump PATH       dump /dev/dsp to PATH on exit (WAV)\n"
           "\n  Configuration\n"
           "    --config PATH           load config from PATH\n"
           "    --print-config          print the resolved config and exit\n"
           "    -V, --version           show the version and exit\n"
           "    -h, --help              show this message\n"
           "    --help-all              also show the options Asgard uses internally\n\n");
    if (!all) return;
    printf("  Internal (Asgard starts these for you)\n"
           "    --android-runtime       bootstrap a Bionic ELF with guest linker64 (--rootfs required)\n"
           "    --android-services      provide the API-35 Binder service-manager subset\n"
           "    --android-window-plugin PATH  load a host window backend before --android or the ELF\n"
           "    --android-app-id N      user-0 app UID (10000..19999) or isolated UID (99000..99999)\n"
           "    --android-system-uid N  run as an Android system UID\n"
           "    --android-zygote        start as uid 0; each fork child drops once to an app or isolated UID\n"
           "    --android-binder-broker SOCKET  join a shared guest Binder domain\n"
           "    --asgard-broker SOCKET  run Asgard's shared guest Binder broker\n"
           "    --asgard-service-parent PID  end with this parent process\n"
           "    --asgard-idle SECONDS  with --asgard-broker: end after this long with no app attached (0: never)\n"
           "    --media-capabilities    probe native CPU/GPU decoders and print JSON\n"
           "    --window-stats-ui ...   the stats window's own process\n\n");
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
    at_quick_exit(restore_terminal);  // guest SIGKILL (Emulator::signal_exit)
}
// ── Quick ELF sanity check (friendlier than letting the loader throw) ────
static bool looks_like_aarch64_elf(const std::string& path) {
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
// Host paths used after loading stay relative to the launch directory:
// a rootfs guest's startup moves the host cwd into BIFROST_ROOT.
static std::string absolute_host_path(const std::string& path) {
    char cwd[PATH_MAX];
    if (path.empty() || path[0] == '/' || !getcwd(cwd, sizeof(cwd))) return path;
    return std::string(cwd) + "/" + path;
}
// ── Main ─────────────────────────────────────────────────────────────────
static bool install_android_window_plugin(const std::string& path) {
    if (path.empty()) return true;
    std::string error;
    auto backend = frost::load_android_window_backend(path, error);
    if (backend && frost::AndroidSurfaceManager::instance().install_window_backend(std::move(backend)))
        return true;
    fprintf(stderr, "bifrost-emu: %s\n", error.empty() ?
            "window plugin cannot initialize or replace an active backend" : error.c_str());
    return false;
}

static void raise_open_file_limit() {
    // Desktop sessions commonly inherit a soft limit of 1024 even though the
    // hard limit permits many more files. Guest opens and broker transfers
    // consume host descriptors too. Match `ulimit -Sn $(ulimit -Hn)` in this
    // process, without changing the user's session or requiring privileges.
    struct rlimit limit{};
    if (::getrlimit(RLIMIT_NOFILE, &limit) != 0) {
        fprintf(stderr, "bifrost-emu: cannot read host open-file limit: %s\n", strerror(errno));
        return;
    }
    if (limit.rlim_cur >= limit.rlim_max) return;
    limit.rlim_cur = limit.rlim_max;
    if (::setrlimit(RLIMIT_NOFILE, &limit) != 0)
        fprintf(stderr, "bifrost-emu: cannot raise host open-file limit: %s\n", strerror(errno));
}

static bool bind_asgard_parent(const char* argument) {
    char* end = nullptr;
    long parent = std::strtol(argument, &end, 10);
    return end && !*end && parent > 1 && parent <= INT_MAX &&
        !::prctl(PR_SET_PDEATHSIG, SIGKILL) && ::getppid() == parent;
}

// The JIT's minimum host CPU. Checked on the real CPU: BIFROST_NO_FMA3 is a test switch.
static bool jit_host_supported() {
    const arm64emu::CpuFeatures cpu = arm64emu::detect_cpu_features();
    if (cpu.has_sse41() && cpu.has_fma3()) return true;
    fprintf(stderr, "bifrost-emu: the JIT needs a CPU with SSE4.1 and FMA3; this one lacks %s%s%s. "
            "--no-jit runs on any x86-64 CPU, much more slowly.\n", cpu.has_sse41() ? "" : "SSE4.1",
            !cpu.has_sse41() && !cpu.has_fma3() ? " and " : "", cpu.has_fma3() ? "" : "FMA3");
    return false;
}

int main(int argc, char** argv) {
    // First registration runs last: after the emulator's destructors and
    // every later handler (terminal restore, profile dumps).
    std::atexit(Emulator::exit_by_guest_signal);
    std::at_quick_exit(Emulator::exit_by_guest_signal);
    raise_open_file_limit();
    if (argc == 2 && std::string(argv[1]) == "--media-capabilities") {
        puts(arm64emu::yggdrasil::HostDecoder::capabilities_json().c_str());
        return 0;
    }
    if (argc >= 3 && std::string(argv[1]) == "--asgard-broker") {
        // --asgard-broker SOCKET [--asgard-service-parent PID] [--asgard-idle SECONDS]
        unsigned idle = 0;
        for (int i = 3; i < argc; i += 2) {
            const std::string option = argv[i];
            if (i + 1 >= argc) { fprintf(stderr, "bifrost-emu: %s needs a value\n", argv[i]); return 2; }
            if (option == "--asgard-service-parent") { if (!bind_asgard_parent(argv[i + 1])) return 2; }
            else if (option == "--asgard-idle") {
                char* end = nullptr;
                const long seconds = std::strtol(argv[i + 1], &end, 10);
                if (!end || *end || seconds < 0 || seconds > 7 * 24 * 3600) { fprintf(stderr, "bifrost-emu: invalid --asgard-idle\n"); return 2; }
                idle = static_cast<unsigned>(seconds);
            } else { fprintf(stderr, "bifrost-emu: unknown broker option %s\n", argv[i]); return 2; }
        }
        return arm64emu::yggdrasil::asgard_broker_main(argv[2], idle);
    }
    if (argc == 3 && std::string(argv[1]) == "--window-stats-ui")
        return arm64emu::window_stats::ui_main(std::atoi(argv[2]));
    if (argc >= 2 && std::string(argv[1]) == "--rootfs-default") {
        if (argc != 3) { fprintf(stderr, "bifrost-emu: --rootfs-default requires one NAME (see --rootfs-list)\n"); return 2; }
        return set_default_rootfs(argv[2]);
    }
    // Not a guest run: hand the remaining arguments to the rootfs installer.
    if (argc >= 2 && std::string(argv[1]) == "--setup-rootfs")
        return arm64emu::run_rootfs_setup(std::vector<std::string>(argv + 2, argv + argc));
    bool debug   = false;
    bool verbose = false;
    bool quiet   = false;
    bool raw_tty = false;
    // JIT is ON by default. Use --no-jit to force the interpreter.
    // The interpreter is the fallback for programs that hit a JIT bug
    // or for debugging.
    bool use_jit = true;
    bool android_runtime = false;
    bool guest_ld = false;
    bool android_services = false;
    uint32_t android_app_id = 10000;
    bool android_app_id_set = false;
    bool android_zygote = false;
    std::string android_window_plugin;
    std::string android_binder_broker;
    std::string fb_dump_path;
    std::string audio_dump_path;
    uint64_t jit_threshold = 0;  // 0 = use JIT from start
    int  arg_i   = 1;
    // ── Config ─────────────────────────────────────────
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
        // Guest argv and option values are not host options. In particular,
        // a guest's --print-config must not suppress plugin initialization.
        if (a == "--" || a == "--android" || a.empty() || a[0] != '-') break;
        if (a == "--config") {
            if (i + 1 < argc) { config_path = argv[i + 1]; i++; }
        } else if (a == "--print-config") {
            print_config_only = true;
        } else if (a == "--rootfs" || a == "--rootfs-profile" || a == "--jit-threshold" ||
                   a == "--window-stats-mode" || a == "--fb-dump" || a == "--audio-dump" ||
                   a == "--android-window-plugin" || a == "--android-binder-broker" ||
                   a == "--android-app-id" || a == "--android-system-uid" || a == "--asgard-service-parent") {
            if (i + 1 < argc) i++;
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
    // A [paths] rootfs from the file reaches the VFS and linker through
    // BIFROST_ROOT, like --rootfs below; an environment value wins.
    if (!cfg.rootfs_path.empty()) setenv("BIFROST_ROOT", cfg.rootfs_path.c_str(), 0);
    if (!cfg.rootfs_profile.empty()) setenv("BIFROST_ROOTFS_PROFILE", cfg.rootfs_profile.c_str(), 0);
    {
        int back = frost::android_keycode_for_name(cfg.android_back_key);
        if (back < 0) {
            fprintf(stderr, "bifrost-emu: unknown [android] back_key '%s' "
                    "(a key name such as escape, backspace, f1, or none)\n",
                    cfg.android_back_key.c_str());
            return 2;
        }
        frost::set_android_back_key(back);
        int enter = frost::android_enter_key_for_name(cfg.android_enter_key);
        if (enter < 0) {
            fprintf(stderr, "bifrost-emu: unknown [android] enter_key '%s' "
                    "(enter, dpad_center or button_a)\n", cfg.android_enter_key.c_str());
            return 2;
        }
        frost::set_android_enter_key(enter);
        int controller = frost::pad_cursor_mode_for_name(cfg.android_controller);
        if (controller < 0) {
            fprintf(stderr, "bifrost-emu: unknown [android] controller '%s' "
                    "(auto, gamepad or cursor)\n", cfg.android_controller.c_str());
            return 2;
        }
        frost::set_pad_cursor_mode(controller);
    }
    arm64emu::network_offline().store(cfg.android_offline);
    while (arg_i < argc) {
        std::string a = argv[arg_i];
        if (a == "-h" || a == "--help")     { print_help(false); return 0; }
        if (a == "--help-all")              { print_help(true);  return 0; }
        if (a == "-V" || a == "--version")  { printf("bifrost-emu %s\n", VERSION); return 0; }
        if (a == "-d" || a == "--debug")    { debug   = true;  arg_i++; continue; }
        if (a == "-v" || a == "--verbose")  { verbose = true;  arg_i++; continue; }
        if (a == "-q" || a == "--quiet")    { quiet   = true;  arg_i++; continue; }
        if (a == "--window-stats" || a == "--window_stats") { setenv("BIFROST_WINDOW_STATS", "1", 1); arg_i++; continue; }
        if (a == "--no-window-stats") {
            for (auto key : {"BIFROST_WINDOW_STATS", "BIFROST_WINDOW_STATS_TITLE", "BIFROST_WINDOW_STATS_WINDOW", "BIFROST_WINDOW_STATS_OVERLAY"}) setenv(key,"0",1);
            arg_i++; continue;
        }
        if (a == "--window-stats-mode" || a.rfind("--window-stats-mode=", 0) == 0) {
            std::string value;
            if (a.size() > 19) value = a.substr(20);
            else if (arg_i + 1 < argc) value = argv[++arg_i];
            const char* mode = value == "1" || value == "minimal" ? "1" : value == "2" || value == "normal" ? "2"
                             : value == "3" || value == "overlay" ? "3" : nullptr;
            if (!mode) {
                fprintf(stderr, "bifrost-emu: --window-stats-mode takes 1 (minimal), 2 (normal) or 3 (overlay)\n");
                return 2;
            }
            setenv("BIFROST_WINDOW_STATS_MODE", mode, 1);
            setenv("BIFROST_WINDOW_STATS", "1", 1);  // choosing a layout turns the stats on
            arg_i++; continue;
        }
        bool stats_option = false;
        for (auto mode : {"title", "window", "overlay"}) {
            std::string flag = std::string("--window-stats-") + mode;
            std::string key = std::string("BIFROST_WINDOW_STATS_") + (std::string(mode)=="title" ? "TITLE" : std::string(mode)=="window" ? "WINDOW" : "OVERLAY");
            if (a == flag || a == "--no-" + flag.substr(2)) {
                setenv(key.c_str(),a == flag ? "1" : "0",1); stats_option = true;
            }
        }
        if (stats_option) { arg_i++; continue; }
        if (a == "--no-render-log" || a == "--render-log") { setenv("BIFROST_RENDER_LOG",a == "--render-log" ? "1" : "0",1); arg_i++; continue; }
        if (a == "--raw-tty")               { raw_tty = true;  arg_i++; continue; }
        if (a == "--jit")                   { use_jit = true;  arg_i++; continue; }
        if (a == "--jit-disk-cache" || a == "--jit-disk-cache=verify" || a == "--no-jit-disk-cache") {
            setenv("BIFROST_JIT_DISK_CACHE", a == "--no-jit-disk-cache" ? "0" : a == "--jit-disk-cache" ? "1" : "verify", 1);
            arg_i++; continue;
        }
        if (a == "--no-jit")                { use_jit = false; arg_i++; continue; }
        if (a == "--android-runtime")       { android_runtime = true; arg_i++; continue; }
        if (a == "--guest-ld")              { guest_ld = true; arg_i++; continue; }
        if (a == "--android-services")      { android_services = true; arg_i++; continue; }
        if (a == "--android-zygote")        { android_zygote = true; arg_i++; continue; }
        if (a == "--asgard-service-parent") {
            if (arg_i + 1 >= argc) return 2;
            if (!bind_asgard_parent(argv[arg_i + 1])) return 2;
            arg_i += 2; continue;
        }
        if (a == "--android-system-uid") {
            if (arg_i + 1 >= argc) return 2;
            const std::string value = argv[arg_i + 1];
            if (value != "1000" && value != "1013" && value != "1040" && value != "1046" && value != "1047") {
                fprintf(stderr, "Asgard: unsupported system-service UID\n"); return 2;
            }
            android_app_id = std::stoul(value); android_app_id_set = true;
            arg_i += 2; continue;
        }
        if (a == "--android-binder-broker") {
            if (arg_i + 1 >= argc) { fprintf(stderr, "Asgard: --android-binder-broker requires a socket path\n"); return 2; }
            android_binder_broker = absolute_host_path(argv[arg_i + 1]); arg_i += 2; continue;
        }
        if (a == "--android-app-id") {
            if (arg_i + 1 >= argc) { fprintf(stderr, "bifrost-emu: --android-app-id requires N\n"); return 2; }
            std::string value = argv[arg_i + 1];
            if (value.size() != 5 || value.find_first_not_of("0123456789") != std::string::npos
                    || !((value >= "10000" && value <= "19999")
                         || (value >= "99000" && value <= "99999"))) {
                fprintf(stderr, "bifrost-emu: Android UID must be in 10000..19999 or 99000..99999\n"); return 2;
            }
            android_app_id = static_cast<uint32_t>(std::stoul(value));
            android_app_id_set = true;
            arg_i += 2;
            continue;
        }
        if (a == "--android-window-plugin") {
            if (arg_i + 1 >= argc) {
                fprintf(stderr, "bifrost-emu: --android-window-plugin requires a PATH argument\n");
                return 2;
            }
            if (!android_window_plugin.empty()) {
                fprintf(stderr, "bifrost-emu: only one --android-window-plugin is allowed\n");
                return 2;
            }
            android_window_plugin = argv[arg_i + 1];
            if (android_window_plugin.empty()) {
                fprintf(stderr, "bifrost-emu: window plugin path cannot be empty\n");
                return 2;
            }
            arg_i += 2;
            continue;
        }
        if (a == "--config")                { arg_i += 2; continue; }  // already handled
        if (a == "--print-config")          { arg_i++; continue; }
        // Selects an installed rootfs profile (rootfs.d/NAME) for libraries; unlike
        // --rootfs it does not confine the guest's paths.
        if (a == "--rootfs-profile") {
            if (arg_i + 1 >= argc || !arm64emu::valid_rootfs_profile_name(argv[arg_i + 1])) {
                fprintf(stderr, "bifrost-emu: --rootfs-profile requires a NAME (letters, digits, '.', '_', '-')\n");
                return 2;
            }
            setenv("BIFROST_ROOTFS_PROFILE", argv[arg_i + 1], 1);
            cfg.rootfs_profile = argv[arg_i + 1];
            arg_i += 2;
            continue;
        }
        if (a == "--rootfs-list") {
            for (const auto& profile : arm64emu::list_rootfs_profiles())
                printf("%c %-12s %s\n", profile.active ? '*' : ' ', profile.name.c_str(),
                       profile.path.empty() ? "(not installed)" : profile.path.c_str());
            return 0;
        }
        // Equivalent to `BIFROST_ROOT=PATH bifrost-emu ...` but more
        // ergonomic and works even when the env var isn't inherited.
        if (a == "--rootfs") {
            if (arg_i + 1 >= argc) {
                fprintf(stderr, "bifrost-emu: --rootfs requires a PATH argument\n");
                return 2;
            }
            setenv("BIFROST_ROOT", argv[arg_i + 1], 1);
            cfg.rootfs_path = argv[arg_i + 1];
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
        if (a == "--android") {
            if (arg_i + 1 >= argc) {
                fprintf(stderr, "bifrost-emu: --android requires a PATH argument\n");
                return 2;
            }
            // Use the common resolved-config exit without creating an
            // emulator or loading any plugin for this early-launch branch.
            if (print_config_only) break;
            // --android libfoo.so runs a NativeActivity .so as the main
            // image. We steal the rest of argv as guest argv[0..] so the
            // rest of main can reuse the same argv vector.
            std::string android_so = argv[arg_i + 1];
            std::vector<std::string> guest_argv;
            guest_argv.push_back(android_so);
            for (int i = arg_i + 2; i < argc; i++) guest_argv.push_back(argv[i]);
            if (raw_tty) set_raw_terminal();
            Emulator emu(android_binder_broker.empty() ? Memory::WINDOW_SIZE_MAX : Memory::BINDER_REALM_WINDOW_SIZE);
            if (!android_binder_broker.empty()) {
                emu.vfs().set_binder_broker(android_binder_broker);
                emu.vfs().set_self_pid(::getpid());
                emu.main_cpu().tid = ::getpid();
                emu.main_cpu().is_fork_process = true;
            }
            if (android_services || !android_binder_broker.empty()) emu.enable_android_services();
            // Mirror the normal path's CLI-over-config precedence
            // (applied below the loop there; here the branch exits early).
            emu.set_verbose(verbose || cfg.log_verbose);
            emu.set_trace(debug || cfg.log_trace);
            emu.set_brk_verbose(quiet ? false : cfg.log_brk_verbose);
            // use_jit only ever DISABLES against the config value (--jit
            // cannot re-enable a config-disabled JIT), matching the
            // normal path's `if (!use_jit) cfg.jit_enabled = false`.
            if (use_jit && cfg.jit_enabled) {
                if (!jit_host_supported()) { restore_terminal(); return 1; }
                emu.enable_jit();
            }
            emu.set_jit_threshold(jit_threshold ? jit_threshold
                                                : cfg.jit_threshold);
            if (cfg.forward_host_signals) emu.install_host_signal_handlers();
            try {
                if (!install_android_window_plugin(android_window_plugin)) {
                    restore_terminal();
                    return 2;
                }
                emu.load_android_activity(android_so, guest_argv);
                int code = emu.run_android();
                restore_terminal();
                return code;
            } catch (const std::exception& e) {
                restore_terminal();
                fprintf(stderr, "bifrost-emu: %s\n", e.what());
                return 1;
            }
        }
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
    cfg.fb_dump_path = absolute_host_path(cfg.fb_dump_path);
    cfg.audio_dump_path = absolute_host_path(cfg.audio_dump_path);
    cfg.validate();
    // The persistent JIT cache reads its settings from the environment (child
    // processes inherit them); a config file supplies what is not set there.
    if (!cfg.jit_disk_cache.empty() && !getenv("BIFROST_JIT_DISK_CACHE"))
        setenv("BIFROST_JIT_DISK_CACHE", cfg.jit_disk_cache.c_str(), 1);
    if (cfg.jit_disk_cache_mb && !getenv("BIFROST_JIT_DISK_CACHE_MB"))
        setenv("BIFROST_JIT_DISK_CACHE_MB", std::to_string(cfg.jit_disk_cache_mb).c_str(), 1);
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
    if (arg_i >= argc) { print_start_page(); return 0; }
    // A named profile that is not installed is an error, not a silent fall back to another rootfs
    // (an explicit --rootfs / BIFROST_ROOT takes precedence and is not checked here).
    if (!arm64emu::requested_rootfs_profile().empty() && arm64emu::library_root().empty()) {
        fprintf(stderr, "bifrost-emu: %s\n", arm64emu::rootfs_search_hint().c_str());
        return 2;
    }
    std::string elf_path = argv[arg_i];
    std::vector<std::string> guest_argv;
    guest_argv.push_back(elf_path);
    for (int i = arg_i + 1; i < argc; i++) guest_argv.push_back(argv[i]);
    // An Android package (APK/APKM/APKS/XAPK) is installed and run by Asgard.
    if (arm64emu::has_android_package_extension(elf_path))
        return arm64emu::exec_android_package(
            elf_path, std::vector<std::string>(guest_argv.begin() + 1, guest_argv.end()),
            android_window_plugin);
    // Friendly error if the file is missing or not an AArch64 ELF.
    // Pre-checks run against the host-side (BIFROST_ROOT-remapped) path;
    // load_elf_file still receives the guest path so /proc/self/exe stays
    // consistent with the guest filesystem view.
    const std::string host_elf = host_path_for(elf_path, cfg.rootfs_path);
    if (access(host_elf.c_str(), R_OK) != 0) {
        fprintf(stderr, "bifrost-emu: cannot open '%s': %s\n",
                host_elf.c_str(), strerror(errno));
        return 127;  // 127 = "command not found" convention
    }
    if (!looks_like_aarch64_elf(host_elf)) {
        fprintf(stderr,
            "bifrost-emu: '%s' is not a 64-bit little-endian AArch64 ELF.\n"
            "bifrost-emu runs AArch64 Linux binaries, static or dynamic.\n",
            host_elf.c_str());
        return 126;  // 126 = "found but not executable" convention
    }
    // Only switch the host TTY into raw mode if the user explicitly asked
    // for it. Default: leave the TTY alone so the host line discipline can
    // buffer input for guest line-oriented stdio (fgets, gets, scanf, …).
    if (raw_tty) set_raw_terminal();
    if (android_app_id_set && !android_runtime) {
        fprintf(stderr, "bifrost-emu: --android-app-id requires --android-runtime\n"); return 2;
    }
    if (android_zygote && (!android_runtime || android_app_id_set)) {
        fprintf(stderr, "bifrost-emu: --android-zygote requires --android-runtime and no fixed app UID\n"); return 2;
    }
    Emulator emu(android_binder_broker.empty() ? Memory::WINDOW_SIZE_MAX : Memory::BINDER_REALM_WINDOW_SIZE);
    emu.set_android_runtime(android_runtime);
    emu.set_guest_linker(guest_ld);
    if (android_runtime) emu.vfs().set_process_identity(android_app_id, android_app_id);
    if (android_zygote) emu.vfs().set_zygote_identity();
    if (!android_binder_broker.empty()) {
        emu.vfs().set_binder_broker(android_binder_broker);
        emu.vfs().set_self_pid(::getpid());
        emu.main_cpu().tid = ::getpid();
        emu.main_cpu().is_fork_process = true;
    }
    if (android_services || !android_binder_broker.empty()) emu.enable_android_services();
    emu.set_verbose(cfg.log_verbose);
    emu.set_trace(cfg.log_trace);
    emu.set_brk_verbose(cfg.log_brk_verbose);
    if (cfg.jit_enabled) {
        if (!jit_host_supported()) { restore_terminal(); return 1; }
        emu.enable_jit();
    }
    emu.set_jit_threshold(cfg.jit_threshold);
    if (cfg.forward_host_signals) {
        emu.install_host_signal_handlers();  // forward host signals to guest
    }
    try {
        if (!install_android_window_plugin(android_window_plugin)) {
            restore_terminal();
            return 2;
        }
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
