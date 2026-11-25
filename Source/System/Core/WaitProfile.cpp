// WaitProfile.cpp — see WaitProfile.hpp.
//
// State is never destroyed: guest threads can end during process exit, after other statics are gone. A thread's record is
// created on its first counted call and kept for the report, which is cumulative like BIFROST_PROF's samples.
#include "System/Core/WaitProfile.hpp"
#include "System/Core/DebugFlags.hpp"
#include "Bifrost/Version.hpp"  // CODENAME
#include "System/Syscalls/SyscallNames.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <map>
#include <tuple>
#include <ctime>
#include <string>
#include <mutex>
#include <thread>
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
    std::atomic<uint64_t> translate_ns{0}, translate_wait_ns{0}, translate_wait_max_ns{0};
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

constexpr uint64_t STALL_NS = 1000000000ull;  // a call this long on a busy thread is reported
constexpr unsigned MAX_STALL_LINES = 200;       // per process: a stuck program must not fill the log
constexpr unsigned MAX_LINES_PER_SITE = 5;      // per thread, call and caller: a loop that sleeps a second must not use them all

struct Describers {
    std::function<std::string(int)> thread_name;
    std::function<std::string(uint64_t)> describe_address;
    std::function<std::string(int)> describe_fd;
};
std::mutex g_describers_mutex;  // try-locked only (fork_guest does not hold it): a busy lock only leaves a name out
Describers g_describers;
std::atomic<unsigned> g_stall_lines{0};

// How often each call site stalled. Only the first MAX_LINES_PER_SITE stalls of a site print; the rest are counted here and the
// report lists the sites that went past it, with their count and time, so a repeating freeze is still seen in full. A site is a
// thread's record (never reused, unlike a guest tid), the call and its caller.
struct Site {
    int tid = 0;
    uint64_t count = 0, total_ns = 0, max_ns = 0;
};
using SiteKey = std::tuple<const Record*, uint64_t, uint64_t>;  // thread record, syscall number, caller
constexpr size_t MAX_SITES = 4096;                              // the table is never pruned: a full one counts nothing new
std::mutex g_sites_mutex;                                       // locked briefly like the describers' (fork_guest does not hold it)
std::map<SiteKey, Site> g_sites;
std::atomic<unsigned> g_unsited_lines{0};  // stalls that could not be counted per site (table busy or full): a few lines print

// Stalls that end together meet at these locks: retry for up to 50 ms, but never wait for good, since a fork child's copy of a
// lock may be held by a thread that does not exist there.
bool lock_briefly(std::unique_lock<std::mutex>& lock) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
    while (!lock && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        (void)lock.try_lock();
    }
    return lock.owns_lock();
}

// Calls whose first argument is a descriptor and whose wait is worth naming: the I/O calls and ioctl (a Binder transaction).
bool takes_descriptor(uint64_t number) {
    return wait_kind_for_syscall(number) == WaitKind::Io || wait_kind_for_syscall(number) == WaitKind::Ioctl;
}

void report_stall(const Record& r, uint64_t duration_ns, uint64_t number, uint64_t arg0, uint64_t arg1, uint64_t caller) {
    // Only a thread that normally runs: a pool worker that sleeps until it is needed waits for seconds as a matter of course.
    const uint64_t lifetime = now_ns() - r.born_ns.load(std::memory_order_relaxed);
    const uint64_t cpu_now = thread_cpu_ns(r.cpu_clock);
    const uint64_t cpu = cpu_now > r.cpu_base_ns ? cpu_now - r.cpu_base_ns : 0;
    if (!lifetime || cpu * 20 < lifetime) return;
    uint64_t site_count = 0;
    bool counted = false;
    {
        std::unique_lock<std::mutex> lock(g_sites_mutex, std::try_to_lock);
        if (lock_briefly(lock)) {
            const SiteKey key{&r, number, caller};
            if (g_sites.size() < MAX_SITES || g_sites.count(key)) {
                Site& site = g_sites[key];
                site.tid = r.tid.load(std::memory_order_relaxed);
                site_count = ++site.count;
                site.total_ns += duration_ns;
                site.max_ns = std::max(site.max_ns, duration_ns);
                counted = true;
            }
        }
    }
    // A busy or full table (a fork child can inherit it locked) must not bring the flood back: a few lines, then none.
    if (counted ? site_count > MAX_LINES_PER_SITE : g_unsited_lines.fetch_add(1, std::memory_order_relaxed) >= MAX_LINES_PER_SITE)
        return;
    if (g_stall_lines.fetch_add(1, std::memory_order_relaxed) >= MAX_STALL_LINES) return;
    std::string name, where, fd;
    {
        std::unique_lock<std::mutex> lock(g_describers_mutex, std::try_to_lock);
        if (lock_briefly(lock)) {
            if (g_describers.thread_name) name = g_describers.thread_name(r.tid.load(std::memory_order_relaxed));
            if (g_describers.describe_address && caller) where = g_describers.describe_address(caller);
            if (g_describers.describe_fd && takes_descriptor(number)) {
                const int guest_fd = static_cast<int>(arg0);
                fd = " fd=" + std::to_string(guest_fd) + " " + g_describers.describe_fd(guest_fd);
            }
        }
    }
    // The time of day in the form of Android's log lines (MM-DD HH:MM:SS.mmm), so a stall lines up with the app's own log.
    timespec wall{};
    ::clock_gettime(CLOCK_REALTIME, &wall);
    tm local_time{};
    ::localtime_r(&wall.tv_sec, &local_time);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%m-%d %H:%M:%S", &local_time);
    const char* call = aarch64_syscall_name(number);
    std::fprintf(stderr, "[%s] [stall] %s.%03ld tid %d%s%s%s: %s took %.2f s a0=0x%llx a1=0x%llx%s caller=%s%s\n", CODENAME, stamp,
                 wall.tv_nsec / 1000000, r.tid.load(std::memory_order_relaxed), name.empty() ? "" : " \"", name.c_str(),
                 name.empty() ? "" : "\"", number == 0x1000 ? "graphics thunk" : (call ? call : "syscall"),
                 static_cast<double>(duration_ns) / 1e9, static_cast<unsigned long long>(arg0),
                 static_cast<unsigned long long>(arg1), fd.c_str(), where.empty() ? "?" : where.c_str(),
                 site_count == MAX_LINES_PER_SITE ? " (last line for this call site; repeats are counted in the report)" : "");
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

WaitTimer::WaitTimer(int guest_tid, uint64_t syscall_number, uint64_t arg0, uint64_t arg1, uint64_t caller) {
    if (!dbg().cpu_profile) return;
    Local& l = local();
    armed_ = true;
    if (l.depth++ != 0) return;  // inside another counted call: that one is charged
    Record* r = record_for(l, guest_tid);
    if (!r) return;
    record_ = r;
    kind_ = wait_kind_for_syscall(syscall_number);
    number_ = syscall_number;
    arg0_ = arg0;
    arg1_ = arg1;
    caller_ = caller;
    start_ns_ = now_ns();
}

WaitTimer::~WaitTimer() {
    if (!armed_) return;
    --local().depth;
    if (!record_) return;
    auto* r = static_cast<Record*>(record_);
    const size_t k = static_cast<size_t>(kind_);
    const uint64_t duration = now_ns() - start_ns_;
    r->ns[k].fetch_add(duration, std::memory_order_relaxed);
    r->calls[k].fetch_add(1, std::memory_order_relaxed);
    if (duration >= STALL_NS) report_stall(*r, duration, number_, arg0_, arg1_, caller_);
}

void wait_prof_set_describers(std::function<std::string(int)> thread_name, std::function<std::string(uint64_t)> describe_address,
                              std::function<std::string(int)> describe_fd) {
    std::lock_guard<std::mutex> lock(g_describers_mutex);
    g_describers = {std::move(thread_name), std::move(describe_address), std::move(describe_fd)};
}

void wait_prof_note_translation(uint64_t translate_ns, uint64_t wait_ns) {
    // A thread is counted from its first syscall, like the rest of its line; translations before it (the dynamic linker's
    // first blocks) are not.
    Record* r = local().record;
    if (!r) return;
    r->translate_ns.fetch_add(translate_ns, std::memory_order_relaxed);
    r->translate_wait_ns.fetch_add(wait_ns, std::memory_order_relaxed);
    // Only this thread writes its record's maximum.
    if (wait_ns > r->translate_wait_max_ns.load(std::memory_order_relaxed))
        r->translate_wait_max_ns.store(wait_ns, std::memory_order_relaxed);
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
        t.translate_ns = r->translate_ns.load(std::memory_order_relaxed);
        t.translate_wait_ns = r->translate_wait_ns.load(std::memory_order_relaxed);
        t.translate_wait_max_ns = r->translate_wait_max_ns.load(std::memory_order_relaxed);
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
    uint64_t total[KINDS] = {}, cpu = 0, life = 0, translate = 0, translate_wait = 0;
    for (const auto& t : threads) {
        cpu += t.cpu_ns;
        life += t.lifetime_ns;
        translate += t.translate_ns;
        translate_wait += t.translate_wait_ns;
        for (size_t k = 0; k < KINDS; ++k) total[k] += t.kind_ns[k];
    }
    std::fprintf(out, "[%s] waiting, %zu thread(s), %.1f thread-seconds alive, cpu %.1f s: blocked in futex %.1f s, poll %.1f s, "
                 "sleep %.1f s; inside io %.1f s, ioctl %.1f s, gl/egl thunks %.1f s, other syscalls %.1f s; translating %.1f s, "
                 "waiting to translate %.1f s\n",
                 CODENAME, threads.size(), seconds(life), seconds(cpu), seconds(total[F]), seconds(total[P]),
                 seconds(total[S]), seconds(total[I]), seconds(total[C]), seconds(total[T]), seconds(total[O]), seconds(translate), seconds(translate_wait));
    for (size_t i = 0; i < threads.size() && i < SHOWN; ++i) {
        const WaitThreadReport& t = threads[i];
        const std::string name = name_of ? name_of(t.guest_tid) : std::string();
        std::fprintf(out, "[%s]   tid %d%s%s%s: alive %.1f s, cpu %.1f%% | blocked: futex %.1f%% poll %.1f%% sleep %.1f%% | "
                     "inside: io %.1f%% ioctl %.1f%% thunk %.1f%% other %.1f%% | jit: translating %.1f%% waiting %.1f%%, longest %.1f ms\n",
                     CODENAME, t.guest_tid, name.empty() ? "" : " \"", name.c_str(), name.empty() ? "" : "\"",
                     seconds(t.lifetime_ns), percent(t.cpu_ns, t.lifetime_ns), percent(t.kind_ns[F], t.lifetime_ns),
                     percent(t.kind_ns[P], t.lifetime_ns), percent(t.kind_ns[S], t.lifetime_ns),
                     percent(t.kind_ns[I], t.lifetime_ns), percent(t.kind_ns[C], t.lifetime_ns),
                     percent(t.kind_ns[T], t.lifetime_ns), percent(t.kind_ns[O], t.lifetime_ns),
                     percent(t.translate_ns, t.lifetime_ns), percent(t.translate_wait_ns, t.lifetime_ns),
                     static_cast<double>(t.translate_wait_max_ns) / 1e6);
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
    // Call sites that stalled more often than their lines printed, longest total first.
    struct Repeated {
        SiteKey key;
        Site site;
    };
    std::vector<Repeated> repeated;
    {
        std::unique_lock<std::mutex> sites_lock(g_sites_mutex, std::try_to_lock);
        if (lock_briefly(sites_lock))
            for (const auto& entry : g_sites)
                if (entry.second.count > MAX_LINES_PER_SITE) repeated.push_back({entry.first, entry.second});
    }
    std::sort(repeated.begin(), repeated.end(), [](const Repeated& a, const Repeated& b) { return a.site.total_ns > b.site.total_ns; });
    for (size_t i = 0; i < repeated.size() && i < SHOWN; ++i) {
        const uint64_t number = std::get<1>(repeated[i].key), caller = std::get<2>(repeated[i].key);
        const Site& site = repeated[i].site;
        const int tid = site.tid;
        const std::string name = name_of ? name_of(tid) : std::string();
        std::string where;
        {
            std::unique_lock<std::mutex> lock(g_describers_mutex, std::try_to_lock);
            if (lock_briefly(lock) && g_describers.describe_address && caller) where = g_describers.describe_address(caller);
        }
        const char* call = aarch64_syscall_name(number);
        std::fprintf(out, "[%s]   repeated stalls: tid %d%s%s%s %s x%llu, %.1f s in all, longest %.2f s, caller=%s\n", CODENAME, tid,
                     name.empty() ? "" : " \"", name.c_str(), name.empty() ? "" : "\"",
                     number == 0x1000 ? "graphics thunk" : (call ? call : "syscall"), static_cast<unsigned long long>(site.count),
                     seconds(site.total_ns), seconds(site.max_ns), where.empty() ? "?" : where.c_str());
    }
    if (repeated.size() > SHOWN) std::fprintf(out, "[%s]   and %zu more call sites with repeated stalls\n", CODENAME, repeated.size() - SHOWN);
    State& s = state();
    std::unique_lock<std::mutex> lock(s.mutex, std::try_to_lock);
    if (lock && s.dropped) std::fprintf(out, "[%s]   %llu thread(s) not counted: the table of %zu is full\n", CODENAME,
                                static_cast<unsigned long long>(s.dropped), MAX_THREADS);
}

}  // namespace arm64emu
