// WaitProfile.cpp — see WaitProfile.hpp.
//
// State is never destroyed: guest threads can end during process exit, after other statics are gone. A thread's record is
// created on its first counted call and kept for the report, which is cumulative like BIFROST_PROF's samples.
#include "System/Core/WaitProfile.hpp"
#include "System/Core/DebugFlags.hpp"
#include "Bifrost/Version.hpp"  // CODENAME
#include <algorithm>
#include <atomic>
#include <ctime>
#include <mutex>
#include <pthread.h>

namespace arm64emu {
namespace {

constexpr size_t KINDS = static_cast<size_t>(WaitKind::Count);
constexpr size_t MAX_THREADS = 4096;
constexpr size_t SHOWN = 12;  // the busiest threads; a game can have a hundred

struct Record {
    std::atomic<int> tid{0};
    std::atomic<uint64_t> born_ns{0};
    std::atomic<uint64_t> died_ns{0};  // 0 while the thread runs
    std::atomic<uint64_t> cpu_at_exit_ns{0};
    clockid_t cpu_clock = 0;           // set before the record is published
    uint64_t cpu_base_ns = 0;          // the thread's CPU time when its record was made: lifetime starts there too
    std::atomic<uint64_t> ns[KINDS] = {};
    std::atomic<uint64_t> calls[KINDS] = {};
};

struct State {
    std::mutex mutex;
    std::vector<Record*> records;
    uint64_t dropped = 0;  // threads past MAX_THREADS: counted by nothing
};

State& state() {
    static State* s = new State;
    return *s;
}

uint64_t now_ns() {
    timespec t{};
    ::clock_gettime(CLOCK_MONOTONIC, &t);
    return static_cast<uint64_t>(t.tv_sec) * 1000000000ull + static_cast<uint64_t>(t.tv_nsec);
}

uint64_t thread_cpu_ns(clockid_t clock) {
    timespec t{};
    if (::clock_gettime(clock, &t) != 0) return 0;
    return static_cast<uint64_t>(t.tv_sec) * 1000000000ull + static_cast<uint64_t>(t.tv_nsec);
}

// The calling thread's state. Its destructor runs when the host thread (a guest thread) ends.
struct Local {
    Record* record = nullptr;
    bool refused = false;  // the table was full: do not try again
    unsigned depth = 0;
    ~Local() {
        if (!record) return;
        record->cpu_at_exit_ns.store(thread_cpu_ns(record->cpu_clock), std::memory_order_relaxed);
        record->died_ns.store(now_ns(), std::memory_order_release);
    }
};

Local& local() {
    thread_local Local l;
    return l;
}

Record* record_for(Local& l, int guest_tid) {
    if (l.record || l.refused) return l.record;
    // Only try-locked, here and in the report: `Emulator::fork_guest` does not hold this lock, so a child forked while another
    // thread held it would wait forever. A busy lock costs one uncounted call here, and one skipped report there.
    State& s = state();
    std::unique_lock<std::mutex> lock(s.mutex, std::try_to_lock);
    if (!lock) return nullptr;
    if (s.records.size() >= MAX_THREADS) {
        ++s.dropped;
        l.refused = true;
        return nullptr;
    }
    auto* r = new Record;
    r->tid.store(guest_tid, std::memory_order_relaxed);
    r->born_ns.store(now_ns(), std::memory_order_relaxed);
    if (::pthread_getcpuclockid(::pthread_self(), &r->cpu_clock) != 0) r->cpu_clock = CLOCK_THREAD_CPUTIME_ID;
    r->cpu_base_ns = thread_cpu_ns(r->cpu_clock);
    s.records.push_back(r);
    l.record = r;
    return r;
}

double percent(uint64_t part, uint64_t whole) { return whole ? 100.0 * static_cast<double>(part) / static_cast<double>(whole) : 0.0; }
double seconds(uint64_t ns) { return static_cast<double>(ns) / 1e9; }

}  // namespace

WaitKind wait_kind_for_syscall(uint64_t number) {
    switch (number) {
        case 98: return WaitKind::Futex;
        // ppoll, pselect6, epoll_pwait, epoll_pwait2, wait4, waitid, rt_sigtimedwait
        case 73: case 72: case 22: case 441: case 260: case 95: case 137: return WaitKind::Poll;
        case 101: case 115: return WaitKind::Sleep;  // nanosleep, clock_nanosleep
        // read, write, readv, writev, pread64, pwrite64, sendto, recvfrom, sendmsg, recvmsg, accept, accept4, connect
        case 63: case 64: case 65: case 66: case 67: case 68: case 206: case 207: case 211: case 212:
        case 202: case 242: case 203: return WaitKind::Io;
        case 29: return WaitKind::Ioctl;  // Binder transactions are ioctls
        case 0x1000: return WaitKind::Thunk;  // GraphicThunk::SYSCALL_NUMBER
        default: return WaitKind::Other;
    }
}

WaitTimer::WaitTimer(int guest_tid, uint64_t syscall_number) {
    if (!dbg().cpu_profile) return;
    Local& l = local();
    armed_ = true;
    if (l.depth++ != 0) return;  // inside another counted call: that one is charged
    Record* r = record_for(l, guest_tid);
    if (!r) return;
    record_ = r;
    kind_ = wait_kind_for_syscall(syscall_number);
    start_ns_ = now_ns();
}

WaitTimer::~WaitTimer() {
    if (!armed_) return;
    --local().depth;
    if (!record_) return;
    auto* r = static_cast<Record*>(record_);
    const size_t k = static_cast<size_t>(kind_);
    r->ns[k].fetch_add(now_ns() - start_ns_, std::memory_order_relaxed);
    r->calls[k].fetch_add(1, std::memory_order_relaxed);
}

std::vector<WaitThreadReport> wait_prof_collect() {
    State& s = state();
    std::vector<Record*> records;
    {
        std::unique_lock<std::mutex> lock(s.mutex, std::try_to_lock);
        if (!lock) return {};
        records = s.records;
    }
    const uint64_t now = now_ns();
    std::vector<WaitThreadReport> out;
    out.reserve(records.size());
    for (Record* r : records) {
        WaitThreadReport t;
        t.guest_tid = r->tid.load(std::memory_order_relaxed);
        const uint64_t died = r->died_ns.load(std::memory_order_acquire);
        t.exited = died != 0;
        t.lifetime_ns = (t.exited ? died : now) - r->born_ns.load(std::memory_order_relaxed);
        const uint64_t cpu = t.exited ? r->cpu_at_exit_ns.load(std::memory_order_relaxed) : thread_cpu_ns(r->cpu_clock);
        t.cpu_ns = cpu > r->cpu_base_ns ? cpu - r->cpu_base_ns : 0;
        for (size_t k = 0; k < KINDS; ++k) {
            t.kind_ns[k] = r->ns[k].load(std::memory_order_relaxed);
            t.kind_calls[k] = r->calls[k].load(std::memory_order_relaxed);
        }
        out.push_back(t);
    }
    std::sort(out.begin(), out.end(), [](const WaitThreadReport& a, const WaitThreadReport& b) { return a.cpu_ns > b.cpu_ns; });
    return out;
}

void wait_prof_print(std::FILE* out, const std::function<std::string(int)>& name_of) {
    const std::vector<WaitThreadReport> threads = wait_prof_collect();
    if (threads.empty()) return;
    constexpr size_t F = static_cast<size_t>(WaitKind::Futex), P = static_cast<size_t>(WaitKind::Poll),
                     S = static_cast<size_t>(WaitKind::Sleep), I = static_cast<size_t>(WaitKind::Io),
                     C = static_cast<size_t>(WaitKind::Ioctl), T = static_cast<size_t>(WaitKind::Thunk),
                     O = static_cast<size_t>(WaitKind::Other);
    uint64_t total[KINDS] = {}, cpu = 0, life = 0;
    for (const auto& t : threads) {
        cpu += t.cpu_ns;
        life += t.lifetime_ns;
        for (size_t k = 0; k < KINDS; ++k) total[k] += t.kind_ns[k];
    }
    std::fprintf(out, "[%s] waiting, %zu thread(s), %.1f thread-seconds alive, cpu %.1f s: blocked in futex %.1f s, poll %.1f s, "
                 "sleep %.1f s; inside io %.1f s, ioctl %.1f s, gl/egl thunks %.1f s, other syscalls %.1f s\n",
                 CODENAME, threads.size(), seconds(life), seconds(cpu), seconds(total[F]), seconds(total[P]),
                 seconds(total[S]), seconds(total[I]), seconds(total[C]), seconds(total[T]), seconds(total[O]));
    for (size_t i = 0; i < threads.size() && i < SHOWN; ++i) {
        const WaitThreadReport& t = threads[i];
        const std::string name = name_of ? name_of(t.guest_tid) : std::string();
        std::fprintf(out, "[%s]   tid %d%s%s%s: alive %.1f s, cpu %.1f%% | blocked: futex %.1f%% poll %.1f%% sleep %.1f%% | "
                     "inside: io %.1f%% ioctl %.1f%% thunk %.1f%% other %.1f%%\n",
                     CODENAME, t.guest_tid, name.empty() ? "" : " \"", name.c_str(), name.empty() ? "" : "\"",
                     seconds(t.lifetime_ns), percent(t.cpu_ns, t.lifetime_ns), percent(t.kind_ns[F], t.lifetime_ns),
                     percent(t.kind_ns[P], t.lifetime_ns), percent(t.kind_ns[S], t.lifetime_ns),
                     percent(t.kind_ns[I], t.lifetime_ns), percent(t.kind_ns[C], t.lifetime_ns),
                     percent(t.kind_ns[T], t.lifetime_ns), percent(t.kind_ns[O], t.lifetime_ns));
    }
    if (threads.size() > SHOWN) {
        uint64_t rest_cpu = 0, rest_futex = 0;
        for (size_t i = SHOWN; i < threads.size(); ++i) {
            rest_cpu += threads[i].cpu_ns;
            rest_futex += threads[i].kind_ns[F];
        }
        std::fprintf(out, "[%s]   and %zu more threads: cpu %.1f s, blocked in futex %.1f s\n", CODENAME,
                     threads.size() - SHOWN, seconds(rest_cpu), seconds(rest_futex));
    }
    State& s = state();
    std::unique_lock<std::mutex> lock(s.mutex, std::try_to_lock);
    if (lock && s.dropped) std::fprintf(out, "[%s]   %llu thread(s) not counted: the table of %zu is full\n", CODENAME,
                                static_cast<unsigned long long>(s.dropped), MAX_THREADS);
}

}  // namespace arm64emu
