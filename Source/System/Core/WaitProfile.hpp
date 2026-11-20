// WaitProfile.hpp — BIFROST_PROF: where each guest thread's wall time went when it was not running translated code.
//
// BIFROST_PROF's sampler counts host CPU time, so a thread that waits shows nothing. This adds, per guest thread, the time
// spent inside guest syscalls by kind, next to the thread's lifetime and CPU time. The futex, poll and sleep kinds are
// time spent blocked; io, ioctl, thunk (the GL/EGL/SDL thunks, where a swap waits for the display) and other are time
// inside the call, work and waiting together. Only the outermost call on a thread is counted.
//
// A thread is counted from its first counted call. Everything is inert unless BIFROST_PROF is set: a timer that is not armed
// costs one flag test.
#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace arm64emu {

enum class WaitKind : uint8_t { Futex, Poll, Sleep, Io, Ioctl, Thunk, Other, Count };

// What a guest syscall number (AArch64) is counted as. GraphicThunk::SYSCALL_NUMBER (0x1000) is Thunk.
WaitKind wait_kind_for_syscall(uint64_t number);

// Charges the time from construction to destruction to the calling thread, under the kind of `syscall_number`.
class WaitTimer {
public:
    WaitTimer(int guest_tid, uint64_t syscall_number);
    ~WaitTimer();
    WaitTimer(const WaitTimer&) = delete;
    WaitTimer& operator=(const WaitTimer&) = delete;
private:
    bool armed_ = false;      // counted in this thread's nesting depth
    void* record_ = nullptr;  // the thread's record when this is the outermost timer, else null
    uint64_t start_ns_ = 0;
    WaitKind kind_ = WaitKind::Other;
};

// One thread's totals since it started, for the report and for tests.
struct WaitThreadReport {
    int guest_tid = 0;
    bool exited = false;
    uint64_t lifetime_ns = 0;
    uint64_t cpu_ns = 0;
    uint64_t kind_ns[static_cast<size_t>(WaitKind::Count)] = {};
    uint64_t kind_calls[static_cast<size_t>(WaitKind::Count)] = {};
};

// Every thread that has made a counted call, most CPU time first.
std::vector<WaitThreadReport> wait_prof_collect();

// A summary line and a line for the busiest threads. `name_of` gives a guest thread's name (may be empty).
void wait_prof_print(std::FILE* out, const std::function<std::string(int)>& name_of);

}  // namespace arm64emu
