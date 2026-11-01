// GuestTimers.hpp — POSIX per-process timers (timer_create and friends, syscalls 107-111).
//
// Each guest timer is one host timerfd on the guest's clock, so arming, absolute times, intervals and expiry counts
// are the kernel's. One lazily started service thread waits on them and queues each expiry as a guest signal.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <time.h>

namespace arm64emu {
class Emulator;

class GuestTimers {
public:
    static constexpr int NOTIFY_SIGNAL = 0;     // SIGEV_SIGNAL
    static constexpr int NOTIFY_NONE = 1;       // SIGEV_NONE
    static constexpr int NOTIFY_THREAD_ID = 4;  // SIGEV_THREAD_ID (the libcs build SIGEV_THREAD on it)
    struct Event {
        int notify = NOTIFY_SIGNAL;
        int signo = 0;
        int tid = 0;
        uint64_t value = 0;
        bool value_is_id = false;  // a NULL sigevent: the signal's si_value is the timer's id
    };
    explicit GuestTimers(Emulator& emu) : emu_(emu) {}
    ~GuestTimers() { stop(); }
    GuestTimers(const GuestTimers&) = delete;
    GuestTimers& operator=(const GuestTimers&) = delete;

    // Each returns 0 (or the overrun count) on success, else a negative errno.
    int create(int clock, const Event& event, int& id);
    int set(int id, int flags, const itimerspec& value, itimerspec& old);
    int get(int id, itimerspec& value);
    int overrun(int id);
    int remove(int id);
    void clear();  // execve destroys every timer
    void stop();   // ends the service thread; timers stop firing

    // fork_guest holds this across the host fork. A child inherits no timers (timer_create(2)) and no service thread.
    std::mutex& mutex_for_fork() { return mu_; }
    void after_fork_child();

private:
    struct Timer {
        int fd = -1;
        Event event;
        int last_overrun = 0;
    };
    void start_locked();
    void run(int epoll_fd, int wake_fd);
    void close_timer(const Timer& timer);

    Emulator& emu_;
    std::mutex mu_;
    std::map<int, Timer> timers_;
    int epoll_fd_ = -1, wake_fd_ = -1;
    std::unique_ptr<std::thread> thread_;
};
}  // namespace arm64emu
