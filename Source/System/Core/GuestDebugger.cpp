#include "System/Core/GuestDebugger.hpp"
#include "System/Core/Emulator.hpp"
#include "System/Core/CPU.hpp"
#include "System/Core/Signals.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <new>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace arm64emu {
namespace {
constexpr unsigned kTasks = 1024;
constexpr unsigned kPacket = 1024;
constexpr unsigned kRunning = 1, kStopRequested = 2, kStopped = 3, kExited = 4;
constexpr uint32_t kStopWork = 1, kStepWork = 2;
constexpr uint64_t kExitKill = 0x00100000, kSysgood = 1;
struct Task {
    GuestDebugControl control;
    int pid = 0, tid = 0, host_pid = 0, host_tid = 0, parent = 0, pgid = 0;
    uint32_t uid = 0, gid = 0;
    int dumpable = 1, tracer = 0, tracer_tid = 0, tracer_pid = 0;
    int tracer_slot = -1;
    unsigned state = 0, stepping = 0;
    int stop_signal = 0, resume_signal = 0, wait_status = 0;
    int requested_signal = SIGSTOP;
    bool seized = false, interrupt = false;
    bool pending = false, bypass_signal = false;
    bool child_notification = false;
    int child_pid = 0, child_code = 0, child_status = 0;
    uint32_t child_uid = 0;
    uint64_t options = 0;
    unsigned command = 0, request = 0, reply = 0, count = 0;
    uint64_t address = 0;
    int result = 0;
    uint8_t packet[kPacket]{};
    uint8_t siginfo[128]{};
    char name[16]{};
};
struct Shared {
    pthread_mutex_t mutex;
    std::atomic<unsigned> count{0};
    std::atomic<uint32_t> epoch{0};
    std::atomic<bool> forked{false};
    Task tasks[kTasks];
};
struct Lock {
    pthread_mutex_t* mutex;
    explicit Lock(Shared* s) : mutex(&s->mutex) {
        int r = pthread_mutex_lock(mutex);
        if (r == EOWNERDEAD) pthread_mutex_consistent(mutex);
    }
    ~Lock() { pthread_mutex_unlock(mutex); }
};
void wake(Shared* shared) {
    shared->epoch.fetch_add(1, std::memory_order_release);
    ::syscall(SYS_futex, &shared->epoch, 1 /* FUTEX_WAKE, process-shared */, INT32_MAX, nullptr, nullptr, 0);
}
void pause(Shared* shared, uint32_t epoch) {
    const timespec timeout{0, 10 * 1000 * 1000};
    ::syscall(SYS_futex, &shared->epoch, 0 /* FUTEX_WAIT */, epoch, &timeout, nullptr, 0);
}
bool dead_process(int pid) {
    // A zombie still answers kill(pid, 0). Check its state as well so a
    // tracee whose tracer was killed can resume before reaping that child.
    char path[64], buf[1024];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE* file = fopen(path, "r");
    if (!file) return errno == ENOENT;
    bool dead = false;
    if (fgets(buf, sizeof(buf), file)) {
        char* end = strrchr(buf, ')');
        dead = end && end[1] == ' ' && (end[2] == 'Z' || end[2] == 'X');
    }
    fclose(file);
    return dead;
}
void notify_tracer(Shared* shared, const Task& t) {
    if (t.tracer_slot < 0) return;
    auto& tracer = shared->tasks[t.tracer_slot];
    if (tracer.state == kExited || tracer.child_notification) return;
    tracer.child_notification = true;
    tracer.child_pid = t.tid; tracer.child_uid = t.uid;
    tracer.child_code = WIFSTOPPED(t.wait_status) ? CLD_TRAPPED : WIFEXITED(t.wait_status) ? CLD_EXITED : CLD_KILLED;
    tracer.child_status = WIFSTOPPED(t.wait_status) ? t.wait_status >> 8 : WIFEXITED(t.wait_status) ? WEXITSTATUS(t.wait_status) : WTERMSIG(t.wait_status);
}
void stop_event(Shared* shared, Task& t, int signo, int code, uint64_t address) {
    t.state = kStopped;
    t.stop_signal = signo;
    t.wait_status = (signo << 8) | 0x7f;
    t.pending = true;
    memset(t.siginfo, 0, sizeof(t.siginfo));
    memcpy(t.siginfo, &signo, 4);
    memcpy(t.siginfo + 8, &code, 4);
    memcpy(t.siginfo + 16, &address, 8);
    if (t.interrupt) {
        t.wait_status |= 128 << 16; // PTRACE_EVENT_STOP
        int event = SIGTRAP | (128 << 8);
        memcpy(t.siginfo + 8, &event, 4);
        t.interrupt = false;
    }
    notify_tracer(shared, t);
}
// Commands are executed by the stopped guest CPU, so register and mapping
// changes are serialized with its interpreter/JIT state.
enum Command { ReadMemory = 1, WriteMemory, ReadRegs, WriteRegs, ReadFP, WriteFP, ReadTLS, WriteTLS };
}

struct GuestDebugger::Impl {
    Emulator& emu;
    Shared* shared;
    std::atomic<bool> shutdown{false};
    std::unique_ptr<std::thread> service;
    std::unique_ptr<Lock> fork_lock;
    std::array<CPU*, kTasks> local_cpus{};
    explicit Impl(Emulator& e) : emu(e) {
        void* mapping = ::mmap(nullptr, sizeof(Shared), PROT_READ | PROT_WRITE,
                               MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (mapping == MAP_FAILED) throw std::bad_alloc();
        shared = new (mapping) Shared;
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
        pthread_mutex_init(&shared->mutex, &attr);
        pthread_mutexattr_destroy(&attr);
    }
    ~Impl() { ::munmap(shared, sizeof(Shared)); }
    Task* own(CPU& cpu) {
        if (cpu.debug_slot < 0 || static_cast<unsigned>(cpu.debug_slot) >= shared->count) return nullptr;
        auto& t = shared->tasks[cpu.debug_slot];
        return t.tid == cpu.tid && t.host_pid == ::getpid() ? &t : nullptr;
    }
    Task* find(int tid) {
        Task* found = nullptr; Task* dead = nullptr;
        for (unsigned i = 0; i < shared->count; ++i) {
            auto& t = shared->tasks[i];
            if (!t.state || t.tid != tid) continue;
            if (t.state == kExited) { dead = &t; continue; }
            // Process leaders are globally unique. Existing synthetic worker
            // IDs can overlap after fork; refuse an ambiguous worker ID.
            if (t.tid == t.pid) return &t;
            if (found) return nullptr;
            found = &t;
        }
        return found ? found : dead;
    }
    bool selected(const Task& t, int selector) const {
        if (selector > 0) return t.tid == selector;
        if (selector == -1) return true;
        int group = selector == 0 ? emu.vfs().process_ids().pgid : -selector;
        return t.pgid == group;
    }
    void start_service() {
        if (service) return;
        service = std::make_unique<std::thread>([this] {
            while (!shutdown.load(std::memory_order_acquire)) {
                uint32_t epoch = shared->epoch.load(std::memory_order_acquire);
                std::vector<int> kicks;
                {
                    Lock lock(shared);
                    for (unsigned i = 0; i < shared->count; ++i) {
                        auto& t = shared->tasks[i];
                        if (t.host_pid == ::getpid() && t.state != kExited && t.child_notification && local_cpus[i]) {
                            t.child_notification = false;
                            auto act = emu.signals().lookup(SIGCHLD);
                            CPU* tracer = local_cpus[i];
                            if ((!act || act->handler != 1) &&
                                ((act && act->handler > 1) || (tracer->sigmask.load() & (1ULL << (SIGCHLD - 1))))) {
                                if (!tracer->push_pending(SIGCHLD, t.child_code, 0, t.child_pid, t.child_uid, t.child_status))
                                    tracer->sigpending.fetch_or(1ULL << (SIGCHLD - 1));
                                if (t.host_tid) kicks.push_back(t.host_tid);
                            }
                        }
                        if (t.host_pid == ::getpid() && t.tracer && t.state != kExited) {
                            bool gone = t.tracer_slot < 0 || shared->tasks[t.tracer_slot].state == kExited || dead_process(t.tracer);
                            if (gone) {
                                if ((t.options & kExitKill) && local_cpus[i]) {
                                    if (!local_cpus[i]->push_pending(SIGKILL, 0, 0))
                                        local_cpus[i]->sigpending.fetch_or(1ULL << (SIGKILL - 1));
                                    if (t.host_tid) kicks.push_back(t.host_tid);
                                }
                                t.tracer = t.tracer_tid = t.tracer_pid = 0; t.tracer_slot = -1;
                                t.pending = false; t.command = 0; t.stepping = 0;
                                t.state = kRunning; t.control.work.store(0, std::memory_order_release);
                                wake(shared);
                            }
                        }
                        if (t.host_pid == ::getpid() && t.state == kStopRequested && t.host_tid)
                            kicks.push_back(t.host_tid);
                    }
                }
                if (!kicks.empty()) {
                    emu.wake_all_futexes();
                    for (int tid : kicks) ::syscall(SYS_tgkill, ::getpid(), tid, Emulator::KICK_SIGNAL);
                }
                pause(shared, epoch);
            }
        });
    }
    void command(CPU& cpu, Task& t) {
        t.result = 0;
        try {
            switch (t.command) {
                case ReadMemory: case WriteMemory: {
                    if (t.address >= emu.mem().window_size() || t.count > emu.mem().window_size() - t.address ||
                        !emu.mem().is_mapped(t.address, t.count)) { t.result = -EIO; break; }
                    // /proc/self/mem supplies Linux FOLL_FORCE semantics for
                    // RX/PROT_NONE guest pages without changing their mapping
                    // permissions or exposing an address outside the guest.
                    int fd = ::open("/proc/self/mem", O_RDWR | O_CLOEXEC);
                    if (fd < 0) { t.result = -EIO; break; }
                    off_t host = reinterpret_cast<uintptr_t>(emu.mem().direct_window()) + t.address;
                    ssize_t n = t.command == ReadMemory ? ::pread(fd, t.packet, t.count, host)
                        : ::pwrite(fd, t.packet, t.count, host);
                    ::close(fd);
                    if (n != static_cast<ssize_t>(t.count)) t.result = -EIO;
                    if (t.command == WriteMemory && n > 0)
                        emu.invalidate_jit_range(t.address, static_cast<uint64_t>(n), CodeInvalidation::CacheMaintenance);
                    break;
                }
                case ReadRegs: {
                    uint64_t regs[34];
                    memcpy(regs, cpu.regs, 31 * 8);
                    regs[31] = cpu.sp; regs[32] = cpu.pc; regs[33] = cpu.nzcv();
                    memcpy(t.packet, regs, t.count);
                    break;
                }
                case WriteRegs: {
                    uint64_t regs[34];
                    memcpy(regs, cpu.regs, 31 * 8);
                    regs[31] = cpu.sp; regs[32] = cpu.pc; regs[33] = cpu.nzcv();
                    memcpy(regs, t.packet, t.count);
                    // Linux validates EL0t and unmasked DAIF here; reserved
                    // PSTATE bits are sanitized. PC alignment faults when
                    // executed, rather than rejecting SETREGSET itself.
                    if (regs[33] & 0x3df) { t.result = -EINVAL; break; }
                    memcpy(cpu.regs, regs, 31 * 8);
                    cpu.sp = regs[31]; cpu.pc = regs[32]; cpu.set_nzcv(static_cast<uint32_t>(regs[33]) & 0xf0000000U);
                    cpu.excl_tag_valid = false;
                    cpu.jit_dispatch_requested = true;
                    break;
                }
                case ReadFP: case WriteFP: {
                    uint8_t fp[528]{};
                    for (unsigned i = 0; i < 32; ++i) {
                        memcpy(fp + i * 16, &cpu.v[i].lo, 8);
                        memcpy(fp + i * 16 + 8, &cpu.v[i].hi, 8);
                    }
                    memcpy(fp + 512, &cpu.fpsr, 4); memcpy(fp + 516, &cpu.fpcr, 4);
                    if (t.command == ReadFP) memcpy(t.packet, fp, t.count);
                    else {
                        memcpy(fp, t.packet, t.count);
                        for (unsigned i = 0; i < 32; ++i) {
                            memcpy(&cpu.v[i].lo, fp + i * 16, 8);
                            memcpy(&cpu.v[i].hi, fp + i * 16 + 8, 8);
                        }
                        memcpy(&cpu.fpsr, fp + 512, 4); memcpy(&cpu.fpcr, fp + 516, 4);
                    }
                    break;
                }
                case ReadTLS: memcpy(t.packet, &cpu.tpidr_el0, t.count); break;
                case WriteTLS: memcpy(&cpu.tpidr_el0, t.packet, t.count); break;
                default: t.result = -EIO; break;
            }
        } catch (const EmuError&) { t.result = -EIO; }
        t.command = 0;
        t.reply = t.request;
    }
    int stopped_loop(CPU& cpu, int signo, int code, uint64_t address) {
        {
            Lock lock(shared);
            Task* t = own(cpu);
            if (!t || !t->tracer) return signo;
            stop_event(shared, *t, signo, code, address);
            cpu.excl_tag_valid = false;
        }
        wake(shared);
        for (;;) {
            uint32_t epoch = shared->epoch.load(std::memory_order_acquire);
            int tracer = 0;
            {
                Lock lock(shared);
                auto& t = *own(cpu);
                tracer = t.tracer;
                if (shutdown.load() || emu.group_exit_requested() || !cpu.running || t.state == kExited) return 0;
                if (!tracer || t.state != kStopped) {
                    cpu.jit_dispatch_requested = true;
                    return t.resume_signal;
                }
                if (t.command) { command(cpu, t); wake(shared); }
            }
            if (dead_process(tracer)) {
                bool kill = false;
                {
                    Lock lock(shared);
                    auto& t = *own(cpu);
                    if (t.tracer != tracer) continue;
                    kill = (t.options & kExitKill) != 0;
                    t.tracer = t.tracer_tid = t.tracer_pid = 0; t.tracer_slot = -1;
                    t.state = kRunning; t.pending = false; t.command = 0; t.stepping = 0; t.control.work.store(0);
                }
                if (kill) emu.signal_exit(cpu, SIGKILL);
                continue;
            }
            pause(shared, epoch);
        }
    }
};

GuestDebugger::GuestDebugger(Emulator& emu) : impl_(std::make_unique<Impl>(emu)) {}
GuestDebugger::~GuestDebugger() { shutdown(); }
void GuestDebugger::shutdown() {
    auto& d = *impl_;
    d.shutdown.store(true, std::memory_order_release); wake(d.shared);
    if (d.service && d.service->joinable()) d.service->join();
    d.service.reset();
}
bool GuestDebugger::register_task(CPU& cpu) {
    auto& d = *impl_;
    Lock lock(d.shared);
    if (auto* own = d.own(cpu)) { own->host_tid = static_cast<int>(::syscall(SYS_gettid)); return true; }
    unsigned index = d.shared->count;
    for (unsigned i = 0; i < d.shared->count; ++i) {
        auto& candidate = d.shared->tasks[i];
        if (candidate.state != kExited || candidate.tracer) continue;
        bool referenced = false;
        for (unsigned j = 0; j < d.shared->count; ++j)
            if (d.shared->tasks[j].tracer_slot == static_cast<int>(i)) { referenced = true; break; }
        if (!referenced) { index = i; break; }
    }
    if (index == kTasks) return false;
    if (index == d.shared->count) ++d.shared->count;
    auto& t = d.shared->tasks[index];
    // A finished CPU can still hold its fast-poll pointer until destruction.
    // Keep that atomic alive while reinitializing the mutex-protected payload.
    Task fresh;
    t.control.work.store(0, std::memory_order_release);
    memcpy(&t.pid, &fresh.pid, sizeof(Task) - offsetof(Task, pid));
    const auto ids = d.emu.vfs().process_ids();
    t.pid = ids.pid; t.tid = cpu.tid; t.host_pid = ::getpid();
    t.host_tid = cpu.tid == ids.pid ? static_cast<int>(::syscall(SYS_gettid)) : 0; t.parent = ::getppid(); t.pgid = ids.pgid;
    t.uid = d.emu.vfs().process_uid(); t.gid = d.emu.vfs().process_gid();
    t.dumpable = d.emu.guest_dumpable.load();
    const std::string name = d.emu.task_comm(cpu.tid);
    snprintf(t.name, sizeof(t.name), "%s", name.c_str());
    t.state = kRunning;
    cpu.debug_slot = index;
    d.local_cpus[index] = &cpu;
    cpu.debug_control = &t.control;
    return true;
}
void GuestDebugger::before_fork() {
    impl_->shared->forked.store(true);
    impl_->start_service();
    // Quiesce service access to process-private signal-table/pending locks
    // while fork copies them. The shared lock itself remains process-shared.
    impl_->fork_lock = std::make_unique<Lock>(impl_->shared);
}
void GuestDebugger::fork_return(bool child) {
    if (child) (void)impl_->fork_lock.release(); // only the parent owns/unlocks it
    else impl_->fork_lock.reset();
}
void GuestDebugger::after_fork(CPU& cpu) {
    auto& d = *impl_;
    // No inherited host thread survived fork. Its std::thread handle must
    // not be joined or destroyed in this CoW child.
    (void)d.service.release();
    d.local_cpus.fill(nullptr);
    d.shutdown.store(false);
    cpu.debug_slot = -1; cpu.debug_control = nullptr; cpu.debug_skip_poll = false;
    register_task(cpu);
    d.start_service();
}
bool GuestDebugger::poll(CPU& cpu) {
    auto& d = *impl_;
    Task* task = d.own(cpu);
    if (!task) return false;
    bool stop = false;
    int signo = SIGSTOP, code = 0;
    {
        Lock lock(d.shared);
        task->host_tid = static_cast<int>(::syscall(SYS_gettid));
        if (task->state == kStopRequested) { stop = true; signo = task->requested_signal; }
        else if (task->stepping == 2) { stop = true; signo = SIGTRAP; code = 2 /* TRAP_TRACE */; task->stepping = 0; }
    }
    if (stop) {
        int injected = d.stopped_loop(cpu, signo, code, signo == SIGTRAP ? cpu.pc : 0);
        if (injected && cpu.running) {
            { Lock lock(d.shared); task->bypass_signal = true; }
            deliver_signal(d.emu, cpu, d.emu.signals(), injected);
        }
    }
    Lock lock(d.shared);
    if (task->stepping == 1) {
        if (cpu.jit_call_depth) { cpu.jit_dispatch_requested = true; return false; }
        task->stepping = 2;
        cpu.debug_skip_poll = true;
        return true;
    }
    if (task->state == kRunning) task->control.work.store(0, std::memory_order_release);
    return false;
}
bool GuestDebugger::signal(CPU& cpu, int& signo, int& code, uint64_t& address) {
    if (signo == SIGKILL) return false;
    auto& d = *impl_;
    {
        Lock lock(d.shared);
        auto* t = d.own(cpu);
        if (!t || !t->tracer) return false;
        if (t->bypass_signal) { t->bypass_signal = false; return false; }
    }
    signo = d.stopped_loop(cpu, signo, code, address);
    return true;
}
int64_t GuestDebugger::ptrace(CPU& cpu, uint64_t request, int tid, uint64_t addr, uint64_t data) {
    auto& d = *impl_;
    register_task(cpu);
    Task* t = nullptr;
    {
        Lock lock(d.shared);
        if (request == 0 /* TRACEME */) {
            auto* self = d.own(cpu); Task* parent = nullptr;
            for (unsigned i = 0; i < d.shared->count; ++i)
                if (d.shared->tasks[i].host_pid == self->parent && d.shared->tasks[i].tid == d.shared->tasks[i].pid)
                    parent = &d.shared->tasks[i];
            if (!parent || self->tracer || parent->state == kExited) return -EPERM;
            self->tracer = parent->host_pid; self->tracer_pid = parent->pid; self->tracer_tid = parent->tid;
            self->tracer_slot = static_cast<int>(parent - d.shared->tasks);
            return 0;
        }
        t = d.find(tid);
        if (!t || t->state == kExited) return -ESRCH;
        if (request == 16 /* ATTACH */ || request == 0x4206 /* SEIZE */) {
            auto* self = d.own(cpu);
            if (t->host_pid == ::getpid() || t->tracer || !t->dumpable || t->uid != self->uid || t->gid != self->gid)
                return -EPERM;
            if (request == 0x4206 && (addr || (data & ~(kExitKill | kSysgood)))) return -EINVAL;
            t->tracer = ::getpid(); t->tracer_tid = cpu.tid; t->tracer_pid = self->pid;
            t->tracer_slot = cpu.debug_slot;
            t->options = request == 0x4206 ? data : 0;
            t->seized = request == 0x4206;
            if (request == 16) {
                t->requested_signal = SIGSTOP; t->state = kStopRequested;
                t->control.work.store(kStopWork, std::memory_order_release);
            }
            wake(d.shared);
            return 0;
        }
        if (t->tracer != ::getpid() || t->tracer_slot != cpu.debug_slot) return -ESRCH;
        if (request == 0x4207 /* INTERRUPT */) {
            if (!t->seized || t->state != kRunning) return -EIO;
            t->requested_signal = SIGTRAP; t->interrupt = true;
            t->state = kStopRequested; t->control.work.store(kStopWork, std::memory_order_release);
            wake(d.shared); return 0;
        }
        if (t->state != kStopped) return -ESRCH;
        if (request == 7 || request == 9 || request == 17 /* CONT, SINGLESTEP, DETACH */) {
            if (data > 64) return -EIO;
            t->resume_signal = data; t->pending = false; t->state = kRunning;
            t->stepping = request == 9 ? 1 : 0;
            t->control.work.store(t->stepping ? kStepWork : 0, std::memory_order_release);
            if (request == 17) { t->tracer = t->tracer_tid = t->tracer_pid = 0; t->tracer_slot = -1; }
            wake(d.shared); return 0;
        }
        if (request == 0x4200 /* SETOPTIONS */) {
            if (data & ~(kExitKill | kSysgood)) return -EINVAL;
            t->options = data; return 0;
        }
        if (request == 0x4202 /* GETSIGINFO */) {
            try { d.emu.mem().write(data, t->siginfo, sizeof(t->siginfo)); }
            catch (const EmuError&) { return -EFAULT; }
            return 0;
        }
    }
    unsigned command = 0, count = 0;
    uint64_t output = data, iov = 0;
    bool writing = false;
    try {
        if (request == 1 || request == 2) { command = ReadMemory; count = 8; }
        else if (request == 4 || request == 5) { command = WriteMemory; count = 8; writing = true; }
        else if (request == 0x4204 || request == 0x4205) {
            writing = request == 0x4205;
            iov = data; output = d.emu.mem().load<uint64_t>(iov);
            uint64_t length = d.emu.mem().load<uint64_t>(iov + 8);
            unsigned size = 0, align = 0;
            if (addr == 1) { command = writing ? WriteRegs : ReadRegs; size = 272; align = 8; }
            else if (addr == 2) { command = writing ? WriteFP : ReadFP; size = 528; align = 16; }
            else if (addr == 0x401) { command = writing ? WriteTLS : ReadTLS; size = 8; align = 8; }
            else return -EINVAL;
            count = std::min<uint64_t>(length, size);
            if (count % align) return -EINVAL;
        } else return -EIO;
        std::array<uint8_t, kPacket> packet{};
        if (writing) {
            if (command == WriteMemory) memcpy(packet.data(), &data, 8);
            else d.emu.mem().read(output, packet.data(), count);
        }
        unsigned sequence;
        {
            Lock lock(d.shared);
            if (t->tracer != ::getpid() || t->tracer_slot != cpu.debug_slot || t->state != kStopped) return -ESRCH;
            if (t->command) return -EBUSY;
            t->address = addr; t->count = count; t->command = command;
            if (writing) memcpy(t->packet, packet.data(), count);
            sequence = ++t->request;
        }
        wake(d.shared);
        for (;;) {
            uint32_t epoch = d.shared->epoch.load(std::memory_order_acquire);
            {
                Lock lock(d.shared);
                if (t->reply == sequence) {
                    if (t->result) return t->result;
                    if (!writing) d.emu.mem().write(output, t->packet, count);
                    if (iov) d.emu.mem().store<uint64_t>(iov + 8, count);
                    return 0;
                }
                if (t->state == kExited || t->tracer != ::getpid() || d.emu.group_exit_requested()) return -ESRCH;
            }
            if (dead_process(t->host_pid)) return -ESRCH;
            pause(d.shared, epoch);
        }
    } catch (const EmuError&) { return -EFAULT; }
}
void GuestDebugger::exited(CPU& cpu, int code, int signo, bool group) {
    auto& d = *impl_;
    Lock lock(d.shared);
    for (unsigned i = 0; i < d.shared->count; ++i) {
        auto& t = d.shared->tasks[i];
        if (t.host_pid != ::getpid() || (!group && t.tid != cpu.tid)) continue;
        if (t.state == kExited) continue;
        t.state = kExited; t.wait_status = signo ? signo & 0x7f : (code & 255) << 8;
        t.pending = t.tracer != 0; t.control.work.store(0); t.command = 0;
        if (t.pending && t.parent != t.tracer) notify_tracer(d.shared, t);
    }
    wake(d.shared);
}
void GuestDebugger::exec(CPU& cpu) {
    if (cpu.debug_slot < 0) return;
    auto& d = *impl_;
    Lock lock(d.shared);
    auto* t = d.own(cpu);
    if (t && t->tracer) {
        t->requested_signal = SIGTRAP; t->state = kStopRequested;
        t->control.work.store(kStopWork, std::memory_order_release);
        wake(d.shared);
    }
}
void GuestDebugger::set_dumpable(int value) {
    auto& d = *impl_; Lock lock(d.shared);
    for (unsigned i = 0; i < d.shared->count; ++i)
        if (d.shared->tasks[i].host_pid == ::getpid()) d.shared->tasks[i].dumpable = value;
}
bool GuestDebugger::has_tracees(int selector) const {
    auto& d = *impl_; Lock lock(d.shared);
    for (unsigned i = 0; i < d.shared->count; ++i) {
        auto& t = d.shared->tasks[i];
        if (t.tracer == ::getpid() && d.selected(t, selector)) return true;
    }
    return false;
}
bool GuestDebugger::wait_polling() const { return impl_->shared->forked.load(std::memory_order_acquire); }
bool GuestDebugger::take_wait(int selector, bool stopped, bool exited, bool consume, int& tid, int& status) {
    auto& d = *impl_; Lock lock(d.shared);
    for (unsigned i = 0; i < d.shared->count; ++i) {
        auto& t = d.shared->tasks[i];
        if (t.tracer != ::getpid() || !d.selected(t, selector) || !t.pending) continue;
        if (t.state == kStopped ? !stopped : !exited) continue;
        // Let the host kernel reap real child exits. Non-child tracees have
        // their own parent, so only their tracer's notification is synthetic.
        if (t.state == kExited && t.parent == ::getpid()) continue;
        tid = t.tid; status = t.wait_status;
        if (consume) {
            t.pending = false;
            if (t.state == kExited) { t.tracer = t.tracer_tid = t.tracer_pid = 0; t.tracer_slot = -1; }
        }
        return true;
    }
    return false;
}
void GuestDebugger::reaped(int tid) {
    auto& d = *impl_; Lock lock(d.shared);
    for (unsigned i = 0; i < d.shared->count; ++i) {
        auto& t = d.shared->tasks[i];
        if (t.tid == tid && t.tracer == ::getpid()) {
            t.pending = false; t.tracer = t.tracer_tid = t.tracer_pid = 0; t.tracer_slot = -1;
        }
    }
}
void GuestDebugger::wait_change() { pause(impl_->shared, impl_->shared->epoch.load(std::memory_order_acquire)); }
bool GuestDebugger::status(int tid, std::string& text) const {
    auto& d = *impl_; Lock lock(d.shared);
    auto* t = d.find(tid);
    if (!t || t->state == kExited) return false;
    text = "Name:\t" + std::string(t->name) + "\nState:\t" + (t->state == kStopped ? "t (tracing stop)" : "R (running)") +
        "\nTgid:\t" + std::to_string(t->pid) + "\nPid:\t" + std::to_string(t->tid) +
        "\nPPid:\t" + std::to_string(t->parent) + "\nTracerPid:\t" + std::to_string(t->tracer_pid) + "\n";
    text += "Uid:\t" + std::to_string(t->uid) + "\t" + std::to_string(t->uid) + "\t" + std::to_string(t->uid) + "\t" + std::to_string(t->uid) + "\n";
    text += "Gid:\t" + std::to_string(t->gid) + "\t" + std::to_string(t->gid) + "\t" + std::to_string(t->gid) + "\t" + std::to_string(t->gid) + "\n";
    return true;
}
} // namespace arm64emu
