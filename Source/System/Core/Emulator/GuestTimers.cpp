// GuestTimers.cpp — see GuestTimers.hpp.
#include "System/Core/Emulator/GuestTimers.hpp"
#include "System/Core/Emulator.hpp"
#include <cerrno>
#include <climits>
#include <new>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <system_error>
#include <unistd.h>

namespace arm64emu {
namespace {
// Linux bounds a process's timers by RLIMIT_SIGPENDING (EAGAIN); each one here also costs a host descriptor.
constexpr size_t MAX_TIMERS = 4096;
constexpr uint32_t WAKE_KEY = UINT32_MAX;
}

int GuestTimers::create(int clock, const Event& event, int& id) {
    // CPU-time clocks (the fixed ids and the negative per-process and per-thread ones) have no timerfd. Refuse them
    // rather than time them on another clock.
    if (clock == CLOCK_PROCESS_CPUTIME_ID || clock == CLOCK_THREAD_CPUTIME_ID || clock < 0) return -ENOTSUP;
    if (event.notify != NOTIFY_SIGNAL && event.notify != NOTIFY_NONE && event.notify != NOTIFY_THREAD_ID) return -EINVAL;
    if (event.notify != NOTIFY_NONE && (event.signo < 1 || event.signo > 64)) return -EINVAL;
    if (event.notify == NOTIFY_THREAD_ID && !emu_.find_cpu_by_tid(event.tid)) return -EINVAL;
    std::lock_guard<std::mutex> lock(mu_);
    if (timers_.size() >= MAX_TIMERS) return -EAGAIN;
    const int fd = ::timerfd_create(clock, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0) return -errno;  // EINVAL for a clock with no timers, EPERM for an alarm clock without its capability
    int next = 0;
    while (timers_.count(next)) ++next;
    Timer timer;
    timer.fd = fd;
    timer.event = event;
    if (event.value_is_id) timer.event.value = static_cast<uint64_t>(next);
    if (event.notify != NOTIFY_NONE) {
        try { start_locked(); }
        catch (const std::system_error&) { ::close(fd); return -EAGAIN; }
        epoll_event ready{};
        ready.events = EPOLLIN;
        ready.data.u32 = static_cast<uint32_t>(next);
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ready) < 0) {
            const int error = errno;
            ::close(fd);
            return -error;
        }
    }
    timers_[next] = timer;
    id = next;
    return 0;
}

int GuestTimers::set(int id, int flags, const itimerspec& value, itimerspec& old) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto found = timers_.find(id);
    if (found == timers_.end()) return -EINVAL;
    // TIMER_ABSTIME and TFD_TIMER_ABSTIME are the same bit; timer_settime ignores the others, timerfd_settime does not.
    if (::timerfd_settime(found->second.fd, flags & TFD_TIMER_ABSTIME, &value, &old) < 0) return -errno;
    return 0;
}

int GuestTimers::get(int id, itimerspec& value) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto found = timers_.find(id);
    if (found == timers_.end()) return -EINVAL;
    return ::timerfd_gettime(found->second.fd, &value) < 0 ? -errno : 0;
}

int GuestTimers::overrun(int id) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto found = timers_.find(id);
    return found == timers_.end() ? -EINVAL : found->second.last_overrun;
}

void GuestTimers::close_timer(const Timer& timer) {
    // Closing the only descriptor also leaves the epoll set.
    ::close(timer.fd);
}

int GuestTimers::remove(int id) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto found = timers_.find(id);
    if (found == timers_.end()) return -EINVAL;
    close_timer(found->second);
    timers_.erase(found);
    return 0;
}

void GuestTimers::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& entry : timers_) close_timer(entry.second);
    timers_.clear();
}

void GuestTimers::start_locked() {
    if (thread_) return;
    const int epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
    const int wake_fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    epoll_event wake{};
    wake.events = EPOLLIN;
    wake.data.u32 = WAKE_KEY;
    if (epoll_fd < 0 || wake_fd < 0 || ::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, wake_fd, &wake) < 0) {
        if (epoll_fd >= 0) ::close(epoll_fd);
        if (wake_fd >= 0) ::close(wake_fd);
        throw std::system_error(errno, std::generic_category());
    }
    // A signal sent to the process is for the guest's threads, so this thread never takes one.
    sigset_t all, previous;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &previous);
    try {
        thread_ = std::make_unique<std::thread>([this, epoll_fd, wake_fd] { run(epoll_fd, wake_fd); });
    } catch (...) {
        pthread_sigmask(SIG_SETMASK, &previous, nullptr);
        ::close(epoll_fd);
        ::close(wake_fd);
        throw;
    }
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    epoll_fd_ = epoll_fd;
    wake_fd_ = wake_fd;
}

void GuestTimers::run(int epoll_fd, int wake_fd) {
    (void)wake_fd;
    epoll_event ready[32];
    for (;;) {
        const int count = ::epoll_wait(epoll_fd, ready, 32, -1);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return;
        for (int i = 0; i < count; ++i) {
            if (ready[i].data.u32 == WAKE_KEY) return;
            const int id = static_cast<int>(ready[i].data.u32);
            Event event;
            int overrun = 0;
            {
                std::lock_guard<std::mutex> lock(mu_);
                const auto found = timers_.find(id);
                if (found == timers_.end()) continue;
                // Nothing to read: the timer was re-armed, or deleted and its id reused, after epoll saw it.
                uint64_t expiries = 0;
                if (::read(found->second.fd, &expiries, sizeof(expiries)) != sizeof(expiries) || !expiries) continue;
                event = found->second.event;
                overrun = expiries - 1 > INT_MAX ? INT_MAX : static_cast<int>(expiries - 1);
            }
            // Not under mu_: delivery takes the thread list's lock, and fork_guest takes both all-or-none.
            const int total = emu_.send_timer_signal(event, id, overrun);
            std::lock_guard<std::mutex> lock(mu_);
            const auto found = timers_.find(id);
            if (found != timers_.end()) found->second.last_overrun = total;
        }
    }
}

void GuestTimers::stop() {
    std::unique_ptr<std::thread> thread;
    int epoll_fd, wake_fd;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& entry : timers_) close_timer(entry.second);
        timers_.clear();
        thread = std::move(thread_);
        epoll_fd = epoll_fd_;
        wake_fd = wake_fd_;
        epoll_fd_ = wake_fd_ = -1;
    }
    if (!thread) return;
    const uint64_t one = 1;
    (void)!::write(wake_fd, &one, sizeof(one));
    thread->join();
    ::close(epoll_fd);
    ::close(wake_fd);
}

void GuestTimers::after_fork_child() {
    // The forking thread is the only one here, and any lock holder is gone. The thread handle is the parent's: abandon it.
    new (&mu_) std::mutex();
    (void)thread_.release();
    for (const auto& entry : timers_) ::close(entry.second.fd);
    timers_.clear();
    if (epoll_fd_ >= 0) ::close(epoll_fd_);
    if (wake_fd_ >= 0) ::close(wake_fd_);
    epoll_fd_ = wake_fd_ = -1;
}

// A timer's expiry, as Linux posix_timer_event: thread-directed to its thread, otherwise process-directed to a thread
// that does not block it. While the last signal of a timer is still queued, a new expiry only raises its overrun.
int Emulator::send_timer_signal(const GuestTimers::Event& event, int id, int overrun) {
    if (event.notify == GuestTimers::NOTIFY_NONE) return overrun;
    const int signo = event.signo;
    std::shared_ptr<CPU> target;
    if (event.notify == GuestTimers::NOTIFY_THREAD_ID) {
        target = find_cpu_by_tid(event.tid);
        if (!target) return overrun;  // the thread has exited
    } else {
        std::lock_guard<std::mutex> g(threads_mu_);
        if (main_cpu_.running && !SignalTable::is_blocked(main_cpu_, signo))
            target = std::shared_ptr<CPU>(&main_cpu_, [](CPU*) {});
        for (auto& gt : threads_) {
            if (target) break;
            if (gt && gt->cpu.running && !SignalTable::is_blocked(gt->cpu, signo))
                target = std::shared_ptr<CPU>(gt, &gt->cpu);
        }
    }
    if (!target) {
        // Every thread blocks it: the process-wide set is one bit per signal, so its timer details are not kept.
        shared_sigpending_.fetch_or(1ULL << (signo - 1), std::memory_order_acq_rel);
        return overrun;
    }
    const int merged = target->merge_timer_overrun(id, overrun);
    if (merged >= 0) return merged;
    CPU::PendingSig signal{signo, SI_TIMER_EMU, event.value};
    signal.timer_id = id;
    signal.timer_overrun = overrun;
    if (!target->push_pending(signal)) target->sigpending.fetch_or(1ULL << (signo - 1));
    if (!SignalTable::is_blocked(*target, signo)) kick_cpu_wait(*target);
    return overrun;
}
}  // namespace arm64emu
