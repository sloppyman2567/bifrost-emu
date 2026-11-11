// Exec.cpp — execve: replace the guest's image with a new program.
//
// What it follows from Linux execve(2) / binfmt_elf / binfmt_script:
//   - the new image gets exactly the environment the caller passed (NULL is empty);
//   - a "#!" file runs its interpreter with [interpreter, optional-arg, script, argv[1..]],
//     up to four levels deep;
//   - ENOENT for a missing file, EACCES for a directory or a file that is not executable,
//     ENOEXEC for something that is neither a script nor an ELF this emulator can run;
//   - descriptors marked FD_CLOEXEC close, /proc/self/exe names the new program, AT_RANDOM is
//     fresh entropy.
// Emulator-specific: a foreign (non-AArch64) ELF such as a host /bin/ls can be stood in for
// by the running program when that is a multi-call binary (BusyBox, toybox) with an applet of
// that name; see program_is_multicall().
#include "System/Core/Emulator.hpp"
#include "System/Memory/Memory.hpp"
#include "System/Core/CPU.hpp"
#include "System/Core/Signals.hpp"
#include "System/Core/DebugFlags.hpp"
#include "System/Syscalls/Syscalls.hpp"
#include "System/VFS/VFS.hpp"
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include "System/Loader/DynamicLinker.hpp"
#include <errno.h>
#include <fcntl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>
#include <climits>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
namespace arm64emu {
namespace {
// Linux limits: one argument or environment string is at most 32 pages; the whole block is
// bounded by a quarter of the stack limit (2 MiB for the usual 8 MiB stack).
constexpr size_t MAX_ARG_STRLEN = 131072;
constexpr size_t MAX_ARG_BYTES = 2 * 1024 * 1024;
constexpr int MAX_SHEBANG_DEPTH = 4;

// A NUL-terminated string from guest memory, up to MAX_ARG_STRLEN (read_path stops at 4096,
// which silently truncated long arguments). Returns false past the limit or on a bad pointer.
bool read_exec_string(Memory& mem, uint64_t address, std::string& out) {
    out.clear();
    try {
        for (size_t i = 0; i < MAX_ARG_STRLEN; ++i) {
            const char c = static_cast<char>(mem.load<uint8_t>(address + i));
            if (c == '\0') return true;
            out.push_back(c);
        }
    } catch (...) {
    }
    return false;
}

// A NULL-terminated array of string pointers (argv or envp). A NULL array is empty.
bool read_exec_vector(Memory& mem, uint64_t array, std::vector<std::string>& out, size_t& bytes) {
    out.clear();
    for (; array; array += 8) {
        uint64_t pointer = 0;
        try { pointer = mem.load<uint64_t>(array); } catch (...) { return false; }
        if (pointer == 0) return true;
        std::string value;
        if (!read_exec_string(mem, pointer, value)) return false;
        bytes += value.size() + 1;
        if (bytes > MAX_ARG_BYTES) return false;
        out.push_back(std::move(value));
    }
    return true;
}

// What execve found at one path.
struct ExecCandidate {
    std::vector<uint8_t> data;  // the whole file for an AArch64 ELF, its first bytes otherwise
    std::string host_file;      // the canonical host path of the file that was opened
    bool elf = false;           // any ELF
    bool aarch64 = false;       // an AArch64 ELF
};

// Opens `path` as the guest sees it and applies execve's checks. Returns 0 or a positive errno.
// The open goes through the VFS, so /proc/self/exe is the running program (the raw host path
// would be the emulator's own binary), BIFROST_ROOT confines it and the library overlay applies.
int read_exec_candidate(Emulator& emu, const std::string& path, ExecCandidate& out, bool check_permission) {
    out = ExecCandidate{};
    int open_error = 0;
    std::unique_ptr<yggdrasil::Node> node = emu.vfs().open(path, O_RDONLY | O_CLOEXEC, 0, &open_error);
    if (!node) return open_error < 0 ? -open_error : (open_error ? open_error : ENOENT);
    const int fd = node->host_fd();
    if (fd < 0) return EACCES;  // a synthetic file is not a program
    {
        char link[48], target[PATH_MAX];
        snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
        const ssize_t length = ::readlink(link, target, sizeof(target) - 1);
        if (length > 0) out.host_file.assign(target, static_cast<size_t>(length));
    }
    struct stat st;
    if (::fstat(fd, &st) != 0) return errno;
    if (S_ISDIR(st.st_mode)) return EACCES;
    if (!S_ISREG(st.st_mode)) return EACCES;
    if (check_permission) {
        // The kernel's own check on this exact file: execute permission for the caller,
        // including a noexec mount.
        char proc[48];
        snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
        if (::faccessat(AT_FDCWD, proc, X_OK, AT_EACCESS) != 0) return EACCES;
    }
    uint8_t head[256];
    const ssize_t got = ::pread(fd, head, sizeof(head), 0);
    if (got < 0) return errno;
    out.elf = got >= 20 && head[0] == 0x7f && head[1] == 'E' && head[2] == 'L' && head[3] == 'F';
    if (out.elf) {
        uint16_t machine;
        memcpy(&machine, head + 18, sizeof(machine));
        out.aarch64 = machine == 183;  // EM_AARCH64
    }
    if (!out.aarch64) {
        out.data.assign(head, head + got);
        return 0;
    }
    out.data.resize(static_cast<size_t>(st.st_size));
    size_t done = 0;
    while (done < out.data.size()) {
        const ssize_t n = ::pread(fd, out.data.data() + done, out.data.size() - done, static_cast<off_t>(done));
        if (n <= 0) return n < 0 ? errno : EIO;
        done += static_cast<size_t>(n);
    }
    return 0;
}

enum class Shebang { None, Valid, Invalid };
// binfmt_script: "#!" interpreter [optional single argument] on the first line (at most 255 bytes).
Shebang parse_shebang(const std::vector<uint8_t>& data, std::string& interpreter, std::string& argument) {
    if (data.size() < 2 || data[0] != '#' || data[1] != '!') return Shebang::None;
    size_t end = 2;
    while (end < data.size() && end < 255 && data[end] != '\n') ++end;
    const std::string line(data.begin() + 2, data.begin() + static_cast<std::ptrdiff_t>(end));
    auto blank = [](char c) { return c == ' ' || c == '\t'; };
    size_t i = 0;
    while (i < line.size() && blank(line[i])) ++i;
    const size_t start = i;
    while (i < line.size() && !blank(line[i])) ++i;
    interpreter = line.substr(start, i - start);
    while (i < line.size() && blank(line[i])) ++i;
    size_t last = line.size();
    while (last > i && blank(line[last - 1])) --last;
    argument = line.substr(i, last - i);
    return interpreter.empty() ? Shebang::Invalid : Shebang::Valid;
}

std::string base_name(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Whether the running program is a multi-call binary (BusyBox, toybox), the only kind that can
// stand in for a foreign ELF of an applet's name. Looks for the markers those programs carry.
bool program_is_multicall(Emulator& emu) {
    std::string host = emu.vfs().elf_backing_path();
    if (host.empty()) host = emu.elf_path();
    std::ifstream in(host, std::ios::binary);
    if (!in) return false;
    std::string bytes(8u << 20, '\0');
    in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<size_t>(in.gcount()));
    return bytes.find("BusyBox v") != std::string::npos || bytes.find("multi-call binary") != std::string::npos ||
           bytes.find("toybox") != std::string::npos;
}

std::vector<uint8_t> read_whole_file(const std::string& host_path) {
    std::vector<uint8_t> data;
    if (FILE* f = fopen(host_path.c_str(), "rb")) {
        fseek(f, 0, SEEK_END);
        const long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size > 0) {
            data.resize(static_cast<size_t>(size));
            if (fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
        }
        fclose(f);
    }
    return data;
}
}  // namespace

int64_t syscall_execve(Emulator& emu, CPU& cpu) {
    const uint64_t a0 = cpu.regs[0], a1 = cpu.regs[1], a2 = cpu.regs[2];
    auto& mem_ = emu.mem_;
    auto& signals_ = emu.signals_;
    // execve(path, argv, envp) — called in the child after fork() to run a program. The child has a
    // copy-on-write copy of the parent's memory, so replacing it leaves the parent unaffected.
    std::string path;
    if (!read_exec_string(mem_, a0, path)) { ret_err(EFAULT); return 0; }
    if (path.empty()) { ret_err(ENOENT); return 0; }
    std::vector<std::string> new_argv, new_env;
    size_t arg_bytes = 0;
    if (!read_exec_vector(mem_, a1, new_argv, arg_bytes) || !read_exec_vector(mem_, a2, new_env, arg_bytes)) {
        ret_err(arg_bytes > MAX_ARG_BYTES ? E2BIG : EFAULT);
        return 0;
    }
    // Follow #! interpreters (binfmt_script), then classify what is left.
    std::string exec_path = path;
    ExecCandidate candidate;
    int error = 0;
    for (int level = 0;; ++level) {
        error = read_exec_candidate(emu, exec_path, candidate, !emu.android_runtime_);
        if (error || candidate.aarch64) break;
        std::string interpreter, interpreter_arg;
        const Shebang kind = parse_shebang(candidate.data, interpreter, interpreter_arg);
        if (kind == Shebang::None) break;
        if (kind == Shebang::Invalid) { error = ENOEXEC; break; }
        if (level >= MAX_SHEBANG_DEPTH) { error = ELOOP; break; }
        std::vector<std::string> shifted{interpreter};
        if (!interpreter_arg.empty()) shifted.push_back(interpreter_arg);
        shifted.push_back(exec_path);  // the script, as the kernel hands it over
        if (new_argv.size() > 1) shifted.insert(shifted.end(), new_argv.begin() + 1, new_argv.end());
        new_argv = std::move(shifted);
        exec_path = interpreter;
    }
    if (error) { ret_err(error); return 0; }
    std::vector<uint8_t> elf_data;
    bool redirected = false;
    if (candidate.aarch64) {
        elf_data = std::move(candidate.data);
    } else {
        // Not an AArch64 ELF. Text without "#!" is ENOEXEC (a shell runs it itself). A foreign ELF,
        // typically a host /bin/ls found by a PATH search, is stood in for by the running program
        // when that is a multi-call binary: `toybox sh -c 'ls /'` and `busybox sh` rely on it.
        // Anything else is ENOEXEC; a missing file never gets here (it is ENOENT above, so a PATH
        // search moves on to the next directory instead of re-running this program).
        const std::string applet = base_name(exec_path);
        if (!candidate.elf || emu.android_runtime_ || applet == base_name(emu.elf_path()) ||
            !program_is_multicall(emu)) {
            ret_err(ENOEXEC);
            return 0;
        }
        elf_data = read_whole_file(emu.vfs().elf_backing_path().empty() ? emu.elf_path()
                                                                        : emu.vfs().elf_backing_path());
        if (elf_data.empty()) { ret_err(ENOEXEC); return 0; }
        if (new_argv.empty()) new_argv.push_back(applet); else new_argv[0] = applet;
        redirected = true;
        if (dbg().exec_trace)
            fprintf(stderr, "[exec] multi-call redirect: '%s' -> '%s %s'\n", exec_path.c_str(),
                    emu.elf_path().c_str(), applet.c_str());
    }
    if (new_argv.empty()) new_argv.push_back(path);
    // fork(), the child is executing INSIDE the JIT code buffer
    // (the fork/execve syscall was JIT'd, and jit_interp_step()
    // was called from JIT code). flush_cache() resets
    // code_buf_used_ to 0, so the next block translation would
    // write at offset 0, OVERWRITING the currently-executing
    // JIT code → host SIGTRAP (exit code 133). This broke
    // `toybox sh /tmp/script.sh` (fork+exec from script file)
    // while `sh -c 'cmd'` worked (different code path).
    //
    // Fix: set jit_disabled_ = true atomically. This prevents
    // new block translations (run_block falls back to interpreter)
    // without touching the code buffer. The stale JIT code can
    // finish executing safely (it just returns to run_block,
    // which calls emu.step() because jit_disabled_ is true).
    // Once the image is replaced, restart_jit_for_exec() abandons
    // this JIT and builds a fresh one for the new program.
    if (emu.jit()) {
        emu.jit()->jit_disabled_.store(true, std::memory_order_relaxed);
    }
    // Linux kills all other threads on execve; the caller survives
    // as the new main. Join them BEFORE tearing down memory so
    // their exit paths (robust list, clear_child_tid) still read
    // valid guest state. Fresh cursors: the old heap is gone, so
    // new mmaps start from a fresh-process layout.
    emu.kill_other_threads(cpu);
    // Device guards describe the old address space. Broker Binder
    // nodes reject RPC from a fork child (owner PID), so clearing its
    // local alias cannot unmap the parent's receive arena.
    emu.fds().invalidate_device_mappings(0, mem_.window_size());
    mem_.reset_allocator_cursors();
    // simulate execve's memory image replacement. On real Linux,
    // execve() removes ALL old mappings (heap, mmap, etc.) and
    // only the new binary's PT_LOAD segments + stack remain.
    // Without this, the new binary's musl finds stale data from
    // the old binary (malloc locks, thread structures) and hits
    // an assertion failure (BRK #1000 = musl's a_crash()).
    //
    // We discard the pages in the mmap_alloc region. This clears
    // stale heap data, malloc locks, and thread structures: anonymous
    // pages read back as zero, so the new binary's musl will see zeroed
    // memory and initialize fresh. We don't touch the stack (at
    // Memory::STACK_TOP) or the binary's own PT_LOAD segments (below
    // 0x40000000). Pages are dropped, never written: a fork child shares
    // them copy-on-write with its parent, and writing zeros through
    // them copied the whole image (and a host buffer as large as each
    // mapping) just before the exec threw it away.
    {
        auto allocs = mem_.allocations_snapshot();
        for (auto& [addr, size] : allocs) {
            // Heap allocations live below AND above the main-stack
            // region (mmap_alloc skips over the stack); none overlap
            // the stack itself, so no upper bound here — discarding
            // must cover stale heap data on both sides.
            if (addr >= Memory::MMAP_BASE_MIN) {
                mem_.madvise_dontneed(addr, size);
                mem_.untrack_allocation(addr, size);
            }
        }
    }
    // Reload the ELF into the existing Memory. The ElfLoader will
    // map new PT_LOAD segments. Old mappings remain but are
    // overwritten by the new binary's segments.
    ElfLoader::Options options;
    options.apply_relocations = !emu.android_runtime_;
    auto info = ElfLoader::load(mem_, elf_data, options);
    uint64_t exec_entry = info.entry;
    emu.timers().clear();  // execve destroys the POSIX timers
    if (!emu.android_runtime_) {
        // The point of no return: descriptors marked FD_CLOEXEC close, and /proc/self/exe and
        // /proc/self/maps name the new program (a multi-call redirect keeps the running one).
        emu.fds().close_on_exec();
        if (!redirected) {
            // The file actually opened (its descriptor's real path), so /proc/self/exe and
            // symlinked or sandboxed programs name the right file.
            if (!candidate.host_file.empty()) emu.record_executable_host_file(candidate.host_file, exec_path);
            else emu.record_executable_path(exec_path);
            emu.name_elf_segments(info, emu.elf_path_);
        }
    }
    if (emu.android_runtime_) {
        if ((!info.interp.empty() && info.interp != "/system/bin/linker64" &&
             info.interp != "/apex/com.android.runtime/bin/linker64") || !info.phdr_addr)
            throw EmuError("Android exec: " + path + " requests interpreter '" + info.interp +
                           "', not a Bionic linker64, or has no mapped program headers");
        // No PT_INTERP: the kernel runs the image itself with AT_BASE 0 (a static Bionic executable,
        // or linker64 as the program, which then loads argv[1]).
        ElfLoader::Loaded loaded{};
        loaded.entry = info.entry;
        loaded.base_addr = 0;
        if (!info.interp.empty()) {
            auto interpreter = emu.read_android_elf(info.interp);
            ElfLoader::Options interpreter_options;
            interpreter_options.apply_relocations = false;
            interpreter_options.reserve_image = true;
            loaded = ElfLoader::load(mem_, interpreter, interpreter_options);
            if (!loaded.interp.empty())
                throw EmuError("Android linker64 must not itself require PT_INTERP");
            emu.name_elf_segments(loaded, info.interp);
        }
        emu.name_elf_segments(info, path);
        emu.elf_path_ = path;
        emu.vfs_.set_elf_path(path);
        emu.prog_entry_ = info.entry;
        emu.entry_ = exec_entry = loaded.entry;
        emu.interp_base_ = loaded.base_addr;
        emu.end_addr_ = info.end_addr;
        emu.phdr_addr_ = info.phdr_addr;
        emu.phnum_ = info.phnum;
        emu.phent_ = info.phent;
        emu.has_lse_ = info.has_lse || loaded.has_lse;
        emu.dyn_linker_.reset();
    }
    // execve resets process state: brk to the new image end,
    // caught signal handlers to default, pending signals and
    // altstack cleared. (sigmask preserved per POSIX.)
    {
        uint64_t new_brk = (info.end_addr + 0xFFFULL) & ~0xFFFULL;
        std::lock_guard<std::mutex> g(emu.brk_mu_);
        emu.brk_ = new_brk;
        emu.brk_start_ = new_brk;
    }
    signals_.reset_exec(cpu);
    cpu.sigpending.store(0);
    {
        CPU::PendingSig tmp;
        while (cpu.pop_pending(tmp)) {}
    }
    cpu.altstack = CPU::AltStack{};
    // Set up a new initial stack.
    const uint64_t STACK_TOP = mem_.stack_top();
    // The stack is already mapped from the parent; just reset SP.
    // Use the Emulator's guest_env_ (which was either
    // set via set_guest_env() or built by build_default_guest_env()
    // at startup). This propagates TZ, LANG, LC_*, etc. from the
    // host so locale-aware programs work correctly in the new
    // process image too.
    //
    // BUGFIX: the old code only pushed "PATH=..." here, dropping
    // TZ and all locale vars in execve'd processes. This meant
    // `toybox sh -c 'uptime'` showed UTC time even when the
    // parent shell had TZ set correctly.
    uint64_t sp;
    if (emu.android_runtime_) {
        emu.vdso_base_ = emu.vdso_size_ = 0;
        emu.load_vdso();
        sp = emu.build_initial_stack(STACK_TOP, new_argv, info);
    } else {
        // Exactly the environment the caller passed to execve (NULL means empty), as on Linux.
        const auto& envs = new_env;
        std::vector<uint64_t> argv_addrs, envp_addrs;
        sp = emu.place_argument_strings(STACK_TOP, new_argv, envs,
                                                 argv_addrs, envp_addrs);
        // AT_RANDOM.
        sp -= 16;
        // AT_RANDOM seeds the new program's stack canary and pointer guard: real entropy, not zeros.
        uint8_t rnd[16] = {0};
        if (::getrandom(rnd, sizeof(rnd), 0) != static_cast<ssize_t>(sizeof(rnd))) {
            if (FILE* urandom = fopen("/dev/urandom", "rb")) {
                if (fread(rnd, 1, sizeof(rnd), urandom) != sizeof(rnd)) memset(rnd, 0x5a, sizeof(rnd));
                fclose(urandom);
            }
        }
        mem_.write(sp, rnd, 16);
        uint64_t random_addr = sp;
        // Build auxv.
        std::vector<uint64_t> auxv = {
            6, 4096,           // AT_PAGESZ
            3, info.phdr_addr, // AT_PHDR
            4, info.phent,     // AT_PHENT
            5, info.phnum,     // AT_PHNUM
            9, info.entry,     // AT_ENTRY
            25, random_addr,   // AT_RANDOM
            16, 0x3ff,         // AT_HWCAP (FP+ASIMD+ATOMICS)
            7, 0,              // AT_BASE
            0, 0,              // AT_NULL
        };
        // Compute total table size and align SP to 16.
        uint64_t argc = new_argv.size();
        uint64_t table_size = 8 + 8 * (argc + 1) + 8 * (envp_addrs.size() + 1) + 8 * auxv.size();
        sp -= table_size;
        sp &= ~0xFULL;
        uint64_t p = sp;
        auto push = [&](uint64_t v) { mem_.store<uint64_t>(p, v); p += 8; };
        push(argc);
        for (auto a : argv_addrs) push(a);
        push(0);
        for (auto e : envp_addrs) push(e);
        push(0);
        for (auto v : auxv) push(v);
    }
    // Set CPU state for the new program.
    cpu.pc = exec_entry;
    cpu.sp = sp;
    cpu.set_nzcv(0);
    cpu.running = true;
    cpu.exit_code = 0;
    memset(cpu.regs, 0, sizeof(cpu.regs));
    emu.debugger().exec(cpu);
    memset(cpu.v_lo, 0, sizeof(cpu.v_lo));
    memset(cpu.v_hi, 0, sizeof(cpu.v_hi));
    cpu.tid = emu.vfs().process_ids().pid;  // exec keeps the PID; the leader TID equals it
    // begin_new_exec: comm becomes the basename of the exec'd path.
    emu.set_guest_comm(path.substr(path.find_last_of('/') + 1));
    // the new binary loads at the same addresses as the old one
    // (e.g., 0x400000). The decode cache matches on PC, so stale
    // entries from the old binary would match new PCs but return
    // wrong decoded instructions — causing the child to execute
    // garbage and exit with code 133 (SIGTRAP from a BRK in the
    // wrongly-decoded instruction stream). Setting all tags to
    // zero (the "empty" sentinel) forces a fresh decode.
    cpu.decode_cache.clear();
    if (dbg().exec_trace) {
        fprintf(stderr, "[exec] post-execve: pc=0x%llx sp=0x%llx "
                "tid=%d regs zeroed, decode cache invalidated\n",
                static_cast<unsigned long long>(cpu.pc),
                static_cast<unsigned long long>(cpu.sp),
                cpu.tid);
    }
    if (emu.android_runtime_) {
        // Bionic's interpreter establishes the new process TLS itself.
        cpu.tpidr_el0 = cpu.tpidrro_el0 = 0;
    } else {
        const uint64_t TLS_SCRATCH_SIZE = 65536;
        uint64_t tls_scratch = mem_.mmap_alloc(TLS_SCRATCH_SIZE);
        cpu.tpidr_el0 = tls_scratch + TLS_SCRATCH_SIZE / 2;
        cpu.tpidrro_el0 = cpu.tpidr_el0;
        mem_.map_range(0, 4096);
    }
    // The new image gets its own JIT (see restart_jit_for_exec); Android's exec branch keeps the
    // interpreter, as before.
    if (!emu.android_runtime_) emu.restart_jit_for_exec();
    emu.fork_child_exec_done_.store(true, std::memory_order_release);
    // Return 0 to indicate execve succeeded (the syscall doesn't
    // actually return on success — we just set PC to the entry
    // point and continue).
    return 0;
}
}  // namespace arm64emu
