// TracyProfile.cpp — see TracyProfile.hpp.
#include "System/Core/TracyProfile.hpp"
#include "System/Core/DebugFlags.hpp"
#include "System/Core/WaitProfile.hpp"
#include "Bifrost/Version.hpp"  // CODENAME
#ifdef BIFROST_TRACY
#include "common/TracyVersion.hpp"
#endif
#include <algorithm>
#include <cstdio>
#include <iterator>
#include <mutex>

namespace arm64emu {

#ifdef BIFROST_TRACY
std::atomic<bool> g_tracy_active{false};

namespace {
std::mutex g_start_mutex;  // serialises starting; never held while a profiled thread runs
bool g_started = false;    // Tracy was started in this process image (a fork child inherits it, and must not use it)

// `text` names `target`: it contains it, or it is the tail of it (a name cut to the 15 characters of a thread name keeps the end
// of a long package name).
bool names_target(const std::string& text, const std::string& target) {
    if (text.empty() || target.empty()) return false;
    if (text.find(target) != std::string::npos) return true;
    return text.size() >= 8 && text.size() < target.size() &&
           target.compare(target.size() - text.size(), text.size(), text) == 0;
}
}  // namespace

// The names of the syscalls a game is likely to make, with their AArch64 numbers (generated from the kernel's
// <asm-generic/unistd.h>, sorted by number). Not `syscall_name_for_crash`: that table is shorter and has wrong entries.
struct SyscallName { uint16_t number; const char* name; };
constexpr SyscallName kSyscallNames[] = {
    {17, "getcwd"},
    {19, "eventfd2"},
    {20, "epoll_create1"},
    {21, "epoll_ctl"},
    {22, "epoll_pwait"},
    {23, "dup"},
    {24, "dup3"},
    {25, "fcntl"},
    {29, "ioctl"},
    {32, "flock"},
    {34, "mkdirat"},
    {35, "unlinkat"},
    {38, "renameat"},
    {45, "truncate"},
    {46, "ftruncate"},
    {47, "fallocate"},
    {48, "faccessat"},
    {56, "openat"},
    {57, "close"},
    {59, "pipe2"},
    {61, "getdents64"},
    {62, "lseek"},
    {63, "read"},
    {64, "write"},
    {65, "readv"},
    {66, "writev"},
    {67, "pread64"},
    {68, "pwrite64"},
    {72, "pselect6"},
    {73, "ppoll"},
    {78, "readlinkat"},
    {79, "fstatat"},
    {80, "fstat"},
    {82, "fsync"},
    {83, "fdatasync"},
    {93, "exit"},
    {94, "exit_group"},
    {95, "waitid"},
    {96, "set_tid_address"},
    {98, "futex"},
    {99, "set_robust_list"},
    {101, "nanosleep"},
    {113, "clock_gettime"},
    {115, "clock_nanosleep"},
    {122, "sched_setaffinity"},
    {123, "sched_getaffinity"},
    {124, "sched_yield"},
    {129, "kill"},
    {130, "tkill"},
    {131, "tgkill"},
    {132, "sigaltstack"},
    {134, "rt_sigaction"},
    {135, "rt_sigprocmask"},
    {137, "rt_sigtimedwait"},
    {139, "rt_sigreturn"},
    {140, "setpriority"},
    {141, "getpriority"},
    {165, "getrusage"},
    {166, "umask"},
    {167, "prctl"},
    {169, "gettimeofday"},
    {172, "getpid"},
    {173, "getppid"},
    {174, "getuid"},
    {178, "gettid"},
    {179, "sysinfo"},
    {198, "socket"},
    {199, "socketpair"},
    {200, "bind"},
    {201, "listen"},
    {202, "accept"},
    {203, "connect"},
    {204, "getsockname"},
    {206, "sendto"},
    {207, "recvfrom"},
    {208, "setsockopt"},
    {209, "getsockopt"},
    {210, "shutdown"},
    {211, "sendmsg"},
    {212, "recvmsg"},
    {214, "brk"},
    {215, "munmap"},
    {216, "mremap"},
    {220, "clone"},
    {221, "execve"},
    {222, "mmap"},
    {226, "mprotect"},
    {227, "msync"},
    {228, "mlock"},
    {229, "munlock"},
    {232, "mincore"},
    {233, "madvise"},
    {242, "accept4"},
    {260, "wait4"},
    {261, "prlimit64"},
    {278, "getrandom"},
    {279, "memfd_create"},
    {282, "userfaultfd"},
    {283, "membarrier"},
    {285, "copy_file_range"},
    {291, "statx"},
    {434, "pidfd_open"},
    {435, "clone3"},
    {439, "faccessat2"},
    {441, "epoll_pwait2"},
};

const char* tracy_syscall_name(uint64_t number) {
    if (number == 0x1000) return "thunk";
    const auto* end = std::end(kSyscallNames);
    const auto* at = std::lower_bound(std::begin(kSyscallNames), end, number,
                                      [](const SyscallName& n, uint64_t value) { return n.number < value; });
    if (at != end && at->number == number) return at->name;
    thread_local char text[32];
    std::snprintf(text, sizeof(text), "syscall %llu", static_cast<unsigned long long>(number));
    return text;
}

uint32_t tracy_syscall_color(uint64_t number) {
    switch (wait_kind_for_syscall(number)) {
        case WaitKind::Futex: return 0xC0392B;
        case WaitKind::Poll: return 0xE67E22;
        case WaitKind::Sleep: return 0x7F8C8D;
        case WaitKind::Io: return 0x2980B9;
        case WaitKind::Ioctl: return 0x8E44AD;
        case WaitKind::Thunk: return 0x27AE60;
        default: return 0x95A5A6;
    }
}

void tracy_consider_process(const std::string& text) {
    const auto& flags = dbg();
    if (!flags.tracy) return;
    if (!flags.tracy_target.empty() && !names_target(text, flags.tracy_target)) return;
    std::lock_guard<std::mutex> lock(g_start_mutex);
    if (g_started) return;
    g_started = true;
    tracy::StartupProfiler();
    g_tracy_active.store(true, std::memory_order_release);
    std::fprintf(stderr, "[%s] Tracy profiling started (%s); connect a Tracy %d.%d viewer\n", CODENAME,
                 flags.tracy_target.empty() ? "this process" : flags.tracy_target.c_str(), tracy::Version::Major,
                 tracy::Version::Minor);
}

void tracy_fork_child() {
    // The profiler's threads exist only in the parent: zones in the child would queue for nobody.
    g_tracy_active.store(false, std::memory_order_release);
}

void tracy_shutdown() {
    if (!g_tracy_active.exchange(false, std::memory_order_acq_rel)) return;
    std::fprintf(stderr, "[%s] Stopping Tracy profiling\n", CODENAME);
    tracy::ShutdownProfiler();
}
#else
void tracy_consider_process(const std::string&) {
    static std::once_flag said;
    if (!dbg().tracy) return;
    std::call_once(said, [] {
        std::fprintf(stderr, "[%s] BIFROST_PROF=tracy needs a build made with `make USE_TRACY=1` (see Tools/Fetch/fetch-tracy.sh)\n",
                     CODENAME);
    });
}
void tracy_fork_child() {}
void tracy_shutdown() {}
#endif

}  // namespace arm64emu
