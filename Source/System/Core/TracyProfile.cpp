// TracyProfile.cpp — see TracyProfile.hpp.
#include "System/Core/TracyProfile.hpp"
#include "System/Core/DebugFlags.hpp"
#include "System/Core/WaitProfile.hpp"
#include "System/Syscalls/SyscallNames.hpp"
#include "Bifrost/Version.hpp"  // CODENAME
#ifdef BIFROST_TRACY
#include "common/TracyVersion.hpp"
#endif
#include <cstdio>
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

const char* tracy_syscall_name(uint64_t number) {
    if (number == 0x1000) return "thunk";
    if (const char* name = aarch64_syscall_name(number)) return name;
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
