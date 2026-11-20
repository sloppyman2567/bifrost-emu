// TracyProfile.hpp — an optional Tracy timeline of the emulator: BIFROST_PROF=tracy[:TARGET], in a build made with
// `make USE_TRACY=1` (Tools/Fetch/fetch-tracy.sh fetches the client sources).
//
// Without USE_TRACY every macro below expands to nothing and the functions do nothing, so a normal build pays nothing. With
// it, a zone costs one flag test until a process is profiled. Each guest thread is a host thread, so Tracy shows one row per
// guest thread. Tracy starts in a process only when BIFROST_PROF asks for it: `tracy` profiles this process, `tracy:NAME`
// the process whose name or command line contains NAME (an Android app's package name, for one). A forked child never uses
// the profiler its parent started: Tracy does not support that.
#pragma once
#include <cstdint>
#include <string>

#ifdef BIFROST_TRACY
#include "tracy/Tracy.hpp"
#include <atomic>
#include <cstring>

namespace arm64emu {
extern std::atomic<bool> g_tracy_active;
inline bool tracy_active() { return g_tracy_active.load(std::memory_order_relaxed); }
// The name a guest syscall is shown under, and the colour of its kind (futex, poll, sleep, io, ioctl, thunk, other).
// The text is the calling thread's, valid until its next call.
const char* tracy_syscall_name(uint64_t number);
uint32_t tracy_syscall_color(uint64_t number);
}  // namespace arm64emu

// A zone named by a string literal, from here to the end of the scope.
#define BIFROST_ZONE(name) ZoneNamedN(bifrost_zone_, name, ::arm64emu::tracy_active())
// A zone named by a C string known only at run time (copied by Tracy).
#define BIFROST_ZONE_DYNAMIC(text)                                                                                     \
    ZoneNamedN(bifrost_dynamic_zone_, "dynamic", ::arm64emu::tracy_active());                                          \
    if (::arm64emu::tracy_active()) {                                                                                  \
        const char* bifrost_text_ = (text);                                                                            \
        bifrost_dynamic_zone_.Name(bifrost_text_, std::strlen(bifrost_text_));                                         \
    }
// A guest syscall, named and coloured by kind. The graphics thunk (0x1000) has its own zone per call, by symbol.
#define BIFROST_ZONE_SYSCALL(number)                                                                                   \
    ZoneNamedN(bifrost_syscall_zone_, "syscall", ::arm64emu::tracy_active() && (number) != 0x1000);                    \
    if (::arm64emu::tracy_active() && (number) != 0x1000) {                                                            \
        const char* bifrost_name_ = ::arm64emu::tracy_syscall_name(number);                                            \
        bifrost_syscall_zone_.Name(bifrost_name_, std::strlen(bifrost_name_));                                         \
        bifrost_syscall_zone_.Color(::arm64emu::tracy_syscall_color(number));                                          \
    }
// A point in time, shown as a message on the timeline.
#define BIFROST_INSTANT(text)                                                                                          \
    do {                                                                                                               \
        if (::arm64emu::tracy_active()) TracyMessage(text, std::strlen(text));                                         \
    } while (0)
// The end of a frame (a buffer swap).
#define BIFROST_FRAME()                                                                                                \
    do {                                                                                                               \
        if (::arm64emu::tracy_active()) FrameMark;                                                                     \
    } while (0)
#else
#define BIFROST_ZONE(name) do {} while (0)
#define BIFROST_ZONE_DYNAMIC(text) do {} while (0)
#define BIFROST_ZONE_SYSCALL(number) do {} while (0)
#define BIFROST_INSTANT(text) do {} while (0)
#define BIFROST_FRAME() do {} while (0)
#endif

namespace arm64emu {

// Offers a name or command line of this process (its program and arguments, or the name it later gives itself). Starts Tracy
// the first time it matches what BIFROST_PROF asked for. Guest threads are named after their guest names (set_task_comm names
// the host thread), and Tracy reads a thread's name from the operating system.
void tracy_consider_process(const std::string& text);
// In the child of a host fork: the profiler of a parent is not the child's.
void tracy_fork_child();
// At a clean exit: lets Tracy finish sending.
void tracy_shutdown();

}  // namespace arm64emu
