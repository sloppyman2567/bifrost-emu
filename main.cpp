// main.cpp - Bifrost-EMU: fast ARM64 Linux user-mode emulator
//
// "A bridge between worlds" — runs static AArch64 Linux binaries on x86_64.
//
// Version: 1.0.0-beta.1
//
// Usage:
//   bifrost-emu [options] <elf-file> [args...]
//
// Options:
//   -d, --debug     debug mode (trace every instruction to stderr)
//   -v, --verbose   verbose (print execution stats on exit)
//   -V, --version   show version and exit
//   -h, --help      show help
//
// By default, bifrost-emu is silent: only the emulated program's stdout/stderr
// appears. No stats, no exit codes, no noise — just the output.

#include "arm64_emu.hpp"
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include <termios.h>
#include <unistd.h>
#include <signal.h>

using namespace arm64emu;

// ── Easter egg: ASCII art shown when no arguments are given ─────────────

static void print_banner() {
    fprintf(stderr,
        "\n"
        "  ╭─────────────────────────────────────────────────┮\n"
        "  │                                                 │\n"
        "  │   ██████╗ ██╗██████╗ ███████╗██████╗            │\n"
        "  │   ██╔══██╗██║██╔══██╗██╔════╝██╔══██╗           │\n"
        "  │   ██████╔╝██║██║  ██║█████╗  ██████╔╝           │\n"
        "  │   ██╔══██╗██║██║  ██║██╔══╝  ██╔══██╗           │\n"
        "  │   ██████╔╝██║██████╔╝███████╗██║  ██║           │\n"
        "  │   ╚═════╝ ╚═╝╚═════╝ ╚══════╝╚═╝  ╚═╝           │\n"
        "  │                                                 │\n"
        "  │   bifrost-emu  v%s                          │\n"
        "  │   A bridge between worlds                       │\n"
        "  │   x86_64 ◄─────────────────► ARM64              │\n"
        "  │                                                 │\n"
        "  ╰─────────────────────────────────────────────────╯\n"
        "\n"
        "  Usage: bifrost-emu [options] <elf-file> [args...]\n"
        "\n"
        "  Options:\n"
        "    -d, --debug     trace every instruction to stderr\n"
        "    -v, --verbose   print execution stats on exit\n"
        "    -V, --version   show version and exit\n"
        "    -h, --help      show this message\n"
        "\n"
        "  Runs static AArch64 Linux ELF binaries on x86_64.\n"
        "  Default mode is silent — only program output is shown.\n"
        "\n", VERSION);
}

// ── Terminal raw mode for interactive apps ──────────────────────────────

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

// ── Main ────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    bool debug   = false;
    bool verbose = false;
    int  arg_i   = 1;

    while (arg_i < argc) {
        std::string a = argv[arg_i];
        if (a == "-h" || a == "--help") { print_banner(); return 0; }
        if (a == "-d" || a == "--debug")   { debug   = true; arg_i++; continue; }
        if (a == "-v" || a == "--verbose") { verbose = true; arg_i++; continue; }
        // Hidden easter egg: --bifrost or --rainbow
        if (a == "--bifrost" || a == "--rainbow") {
            fprintf(stderr,
                "\n"
                "       🌈 x86_64                    ARM64 🌈\n"
                "         ║                            ║\n"
                "         ║   ╭──────────────────╮     ║\n"
                "         ╠═══│   B I F R O S T   │═════╣\n"
                "         ║   ╰──────────────────╯     ║\n"
                "         ║                            ║\n"
                "         ╚════════════════════════════╝\n"
                "\n"
                "  \"The bridge trembles, but holds.\"\n"
                "   — an ancient emulator proverb\n\n"
                "  v%s\n\n", VERSION);
            arg_i++; continue;
        }
        if (a == "-V" || a == "--version") {
            printf("bifrost-emu %s\n", VERSION);
            return 0;
        }
        if (a.size() >= 1 && a[0] == '-' && a.size() > 1) {
            fprintf(stderr, "bifrost-emu: unknown option: %s\n", a.c_str());
            return 2;
        }
        break;
    }

    // No file given → show the banner (easter egg)
    if (arg_i >= argc) { print_banner(); return 0; }

    std::string elf_path = argv[arg_i];
    std::vector<std::string> guest_argv;
    guest_argv.push_back(elf_path);
    for (int i = arg_i + 1; i < argc; i++) guest_argv.push_back(argv[i]);

    set_raw_terminal();

    Emulator emu;
    emu.set_verbose(verbose);
    emu.set_trace(debug);       // -d enables instruction tracing
    emu.set_brk_verbose(debug); // -d also shows BRK warnings

    try {
        emu.load_elf_file(elf_path, guest_argv);
        int code = emu.run();
        restore_terminal();
        return code;
    } catch (const std::exception& e) {
        restore_terminal();
        fprintf(stderr, "bifrost-emu: error: %s\n", e.what());
        return 1;
    }
}
