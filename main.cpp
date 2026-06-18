// main.cpp — bifrost-emu: fast ARM64 Linux user-mode emulator
//
// "A bridge between worlds" — runs static AArch64 Linux binaries on x86_64.
//
// Version: 1.3.0-alpha.3
//
// Usage:
//   bifrost-emu [options] <elf-file> [args...]
//
// Options:
//   -d, --debug     trace every instruction to stderr
//   -v, --verbose   print execution stats on exit
//   -q, --quiet     suppress BRK warnings (even with -d)
//   -V, --version   show version and exit
//   -h, --help      show this help
//
// By default bifrost-emu is silent: only the emulated program's
// stdout/stderr appears. No stats, no exit codes, no noise — just the
// output.

#include "arm64_emu.hpp"
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <termios.h>
#include <unistd.h>

using namespace arm64emu;

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
        "    -d, --debug     trace every instruction to stderr\n"
        "    -v, --verbose   print execution stats on exit\n"
        "    -q, --quiet     suppress BRK warnings (even with -d)\n"
        "    -V, --version   show version and exit\n"
        "    -h, --help      show this message\n"
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

// ── Main ─────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    bool debug   = false;
    bool verbose = false;
    bool quiet   = false;
    int  arg_i   = 1;

    while (arg_i < argc) {
        std::string a = argv[arg_i];
        if (a == "-h" || a == "--help")     { print_banner();   return 0; }
        if (a == "-V" || a == "--version")  { printf("bifrost-emu %s\n", VERSION); return 0; }
        if (a == "-d" || a == "--debug")    { debug   = true;  arg_i++; continue; }
        if (a == "-v" || a == "--verbose")  { verbose = true;  arg_i++; continue; }
        if (a == "-q" || a == "--quiet")    { quiet   = true;  arg_i++; continue; }
        if (a == "--bifrost" || a == "--rainbow") { print_rainbow(); arg_i++; continue; }
        if (a.size() >= 1 && a[0] == '-' && a.size() > 1) {
            fprintf(stderr, "bifrost-emu: unknown option: %s (try --help)\n", a.c_str());
            return 2;
        }
        break;
    }

    // No file given → show the banner
    if (arg_i >= argc) { print_banner(); return 0; }

    std::string elf_path = argv[arg_i];
    std::vector<std::string> guest_argv;
    guest_argv.push_back(elf_path);
    for (int i = arg_i + 1; i < argc; i++) guest_argv.push_back(argv[i]);

    // Friendly error if the file is missing or not a static AArch64 ELF
    if (access(elf_path.c_str(), R_OK) != 0) {
        fprintf(stderr, "bifrost-emu: cannot open '%s': %s\n",
                elf_path.c_str(), strerror(errno));
        return 127;  // 127 = "command not found" convention
    }
    if (!looks_like_static_aarch64_elf(elf_path)) {
        fprintf(stderr,
            "bifrost-emu: '%s' is not a 64-bit little-endian AArch64 ELF.\n"
            "bifrost-emu only runs static AArch64 Linux binaries.\n",
            elf_path.c_str());
        return 126;  // 126 = "found but not executable" convention
    }

    set_raw_terminal();

    Emulator emu;
    emu.set_verbose(verbose);
    emu.set_trace(debug);
    emu.set_brk_verbose(debug && !quiet);  // -d shows BRKs unless -q

    try {
        emu.load_elf_file(elf_path, guest_argv);
        int code = emu.run();
        restore_terminal();
        return code;
    } catch (const std::exception& e) {
        restore_terminal();
        fprintf(stderr, "bifrost-emu: %s\n", e.what());
        return 1;
    }
}
