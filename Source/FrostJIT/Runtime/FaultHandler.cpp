// Registered native access faults resume in generated recovery code. No
// allocation, locks, interpreter work or guest signal delivery in the handler.
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include "System/Core/Emulator.hpp"
#include "System/Core/Signals.hpp"
#include "System/Core/DebugFlags.hpp"
#include <csignal>
#include <cerrno>
#include <cstdio>
#include <new>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

namespace arm64emu {
thread_local std::atomic<FrostJIT*> FrostJIT::fault_jit_{nullptr};
static thread_local std::atomic<int> pending_signo{0};
static thread_local std::atomic<int> pending_code{0};
static thread_local std::atomic<uint64_t> pending_address{0};
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<size_t>::is_always_lock_free);
static_assert(std::atomic<int>::is_always_lock_free);
static_assert(std::atomic<FrostJIT*>::is_always_lock_free);
static_assert(sizeof(std::atomic<bool>) == 1);

static struct sigaction previous_segv, previous_bus;
static std::once_flag install_once;
static bool handlers_installed = false;

static void append(char*& out, char* end, const char* text) {
    while (*text && out < end) *out++ = *text++;
}
static void append_number(char*& out, char* end, uint64_t value, unsigned base) {
    char digits[20];
    int n = 0;
    do { digits[n++] = "0123456789abcdef"[value % base]; value /= base; } while (value);
    if (base == 16) append(out, end, "0x");
    while (n && out < end) *out++ = digits[--n];
}
static uint64_t parse_hex(const char*& p) {
    uint64_t value = 0;
    for (;; ++p) {
        unsigned c = static_cast<unsigned char>(*p);
        if (c - '0' < 10) value = value * 16 + (c - '0');
        else if ((c | 32) - 'a' < 6) value = value * 16 + ((c | 32) - 'a' + 10);
        else return value;
    }
}
// The /proc/self/maps line containing `pc`, as "path+0xoffset" (async-signal-safe).
static void append_mapping(char*& out, char* end, uint64_t pc) {
    int fd = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    char buffer[4096], line[512];
    size_t used = 0;
    bool found = false;
    ssize_t n;
    while (!found && (n = ::read(fd, buffer, sizeof(buffer))) > 0) {
        for (ssize_t i = 0; i < n && !found; ++i) {
            if (buffer[i] != '\n') { if (used < sizeof(line) - 1) line[used++] = buffer[i]; continue; }
            line[used] = 0;
            used = 0;
            const char* p = line;
            uint64_t start = parse_hex(p);
            if (*p++ != '-') continue;
            uint64_t stop = parse_hex(p);
            if (pc < start || pc >= stop) continue;
            found = true;
            while (*p == ' ') ++p;  // permissions
            while (*p && *p != ' ') ++p;
            while (*p == ' ') ++p;
            uint64_t offset = parse_hex(p);  // then device, inode and path
            for (int field = 0; field < 2 && *p; ++field) { while (*p == ' ') ++p; while (*p && *p != ' ') ++p; }
            while (*p == ' ') ++p;
            append(out, end, " map=");
            append(out, end, *p ? p : "[anonymous]");
            append(out, end, "+");
            append_number(out, end, pc - start + offset, 16);
        }
    }
    ::close(fd);
}
// A host fault outside registered guest accesses (a host library, thunk or
// the emulator itself) terminates the process. Name its PC first: the
// re-raised signal alone leaves only the raise() frame in a core dump.
static void report_host_fault(int signo, const siginfo_t* info, const void* context) {
    const uint64_t pc = context
        ? static_cast<uint64_t>(static_cast<const ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP]) : 0;
    char text[768];
    char* out = text;
    char* end = text + sizeof(text) - 1;
    append(out, end, "[host-fault] sig=");
    append_number(out, end, static_cast<unsigned>(signo), 10);
    append(out, end, " code=");
    append_number(out, end, info ? static_cast<uint64_t>(static_cast<uint32_t>(info->si_code)) : 0, 10);
    append(out, end, " addr=");
    append_number(out, end, info ? reinterpret_cast<uint64_t>(info->si_addr) : 0, 16);
    append(out, end, " pc=");
    append_number(out, end, pc, 16);
    append(out, end, " tid=");
    append_number(out, end, static_cast<uint64_t>(::syscall(SYS_gettid)), 10);
    if (pc) append_mapping(out, end, pc);
    *out++ = '\n';
    (void)!::write(STDERR_FILENO, text, static_cast<size_t>(out - text));
}

static void native_fault_handler(int signo, siginfo_t* info, void* context) {
    const int saved_errno = errno;
    if (FrostJIT::redirect_memory_fault(signo, info, context)) {
        errno = saved_errno;
        return;
    }
    // Respect an embedder's prior handler for faults outside registered guest
    // accesses. Default faults still terminate with the original host signal.
    const auto& previous = signo == SIGBUS ? previous_bus : previous_segv;
    if (previous.sa_handler == SIG_DFL || previous.sa_handler == SIG_IGN) {
        report_host_fault(signo, info, context);
        ::sigaction(signo, &previous, nullptr);
        ::raise(signo);
    } else {
        if (previous.sa_flags & SA_RESETHAND) ::signal(signo, SIG_DFL);
        if (previous.sa_flags & SA_SIGINFO) previous.sa_sigaction(signo, info, context);
        else previous.sa_handler(signo);
    }
    errno = saved_errno;
}

FrostJIT::FaultScope::FaultScope(FrostJIT* jit)
    : saved_(fault_jit_.load(std::memory_order_relaxed)) {
    fault_jit_.store(jit, std::memory_order_relaxed);
}
FrostJIT::FaultScope::~FaultScope() {
    fault_jit_.store(saved_, std::memory_order_relaxed);
}

void FrostJIT::init_fault_recovery() {
    if (!code_buf_ || dbg().jit_software_guard) return;
    // Even a two-byte native instruction can have at most one site. This
    // reserves metadata VA, not physical memory; records are constructed only
    // as code is appended. The table never moves while guest CPUs run.
    const size_t bytes = (code_buf_limit_ / 2) * sizeof(*fault_sites_);
    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return; // allocation failure keeps software guards
    std::call_once(install_once, [] {
        struct sigaction sa{};
        sa.sa_sigaction = native_fault_handler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        ::sigfillset(&sa.sa_mask);
        if (::sigaction(SIGSEGV, &sa, &previous_segv) != 0) return;
        if (::sigaction(SIGBUS, &sa, &previous_bus) != 0) {
            ::sigaction(SIGSEGV, &previous_segv, nullptr);
            return;
        }
        handlers_installed = true;
    });
    if (!handlers_installed) { ::munmap(p, bytes); return; }
    fault_sites_ = static_cast<std::atomic<uint64_t>*>(p);
}

void FrostJIT::destroy_fault_recovery() {
    // The owner joins its CPUs before destroying the JIT, just as for code.
    if (fault_sites_)
        ::munmap(fault_sites_, (code_buf_limit_ / 2) * sizeof(*fault_sites_));
}

void FrostJIT::register_memory_fault_site() {
    if (!memory_fault_stub_ || code_buf_overflow_) return;
    const size_t n = fault_site_count_.load(std::memory_order_relaxed);
    if (n >= code_buf_limit_ / 2) { code_buf_overflow_ = true; return; }
    const uint64_t entry = (uint64_t(code_buf_used_) << 32) | memory_fault_stub_;
    if (n == fault_site_constructed_) {
        new (fault_sites_ + n) std::atomic<uint64_t>(entry);
        ++fault_site_constructed_;
    } else {
        fault_sites_[n].store(entry, std::memory_order_relaxed);
    }
    fault_site_count_.store(n + 1, std::memory_order_release);
}

void FrostJIT::rewind_codegen(size_t offset) {
    size_t n = fault_site_count_.load(std::memory_order_relaxed);
    while (n && (fault_sites_[n - 1].load(std::memory_order_relaxed) >> 32) >= offset) --n;
    fault_site_count_.store(n, std::memory_order_release);
    // Only an unpublished tail is reused. Readers with an older count can
    // see old/new atomic tail entries, all above every executable prefix site.
    // Direct-call records in the tail would later rewrite reused bytes.
    if (call_site_end_ > offset) {
        auto purge = [offset](auto& sites, auto slot_of) {
            for (auto it = sites.begin(); it != sites.end();) {
                auto& v = it->second;
                v.erase(std::remove_if(v.begin(), v.end(),
                    [&](const auto& s) { return slot_of(s) >= offset; }), v.end());
                it = v.empty() ? sites.erase(it) : std::next(it);
            }
        };
        purge(pending_call_sites_, [](size_t s) { return s; });
        purge(direct_call_sites_, [](const std::pair<size_t, size_t>& s) { return s.first; });
        call_site_end_ = offset;
    }
    code_buf_used_ = offset;
}

bool FrostJIT::redirect_memory_fault(int signo, void* raw_info, void* context) {
    FrostJIT* jit = fault_jit_.load(std::memory_order_relaxed);
    auto* info = static_cast<siginfo_t*>(raw_info);
    if (!jit || !jit->fault_sites_ || !jit->guest_memory_ || !info ||
        info->si_code <= 0 || !context) return false;
    auto* uc = static_cast<ucontext_t*>(context);
    const uintptr_t rip = uc->uc_mcontext.gregs[REG_RIP];
    const uintptr_t base = reinterpret_cast<uintptr_t>(jit->code_buf_);
    const uintptr_t win = reinterpret_cast<uintptr_t>(jit->window_base_);
    const uintptr_t addr = reinterpret_cast<uintptr_t>(info->si_addr);
    if (rip < base || rip - base >= jit->code_buf_limit_ ||
        static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_R10]) != win ||
        addr < win || addr - win >= jit->guest_memory_->window_size()) return false;
    const uint32_t offset = static_cast<uint32_t>(rip - base);
    size_t lo = 0, hi = jit->fault_site_count_.load(std::memory_order_acquire);
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        const uint64_t entry = jit->fault_sites_[mid].load(std::memory_order_relaxed);
        const uint32_t site = static_cast<uint32_t>(entry >> 32);
        if (site < offset) lo = mid + 1;
        else if (site > offset) hi = mid;
        else {
            pending_address.store(addr - win, std::memory_order_relaxed);
            pending_code.store(info->si_code, std::memory_order_relaxed);
            pending_signo.store(signo, std::memory_order_relaxed);
            uc->uc_mcontext.gregs[REG_RIP] = base + static_cast<uint32_t>(entry);
            return true;
        }
    }
    return false;
}

extern "C" void jit_fault_step(Emulator* emu, CPU* cpu, uint64_t block_pc) {
    const int signo = pending_signo.exchange(0, std::memory_order_relaxed);
    if (signo && dbg().fault_trace)
        fprintf(stderr, "[jit-fault] tid=%d host-signal=%d guest-pc=0x%llx addr=0x%llx\n",
                cpu->tid, signo, static_cast<unsigned long long>(cpu->pc),
                static_cast<unsigned long long>(pending_address.load(std::memory_order_relaxed)));
    if (signo == SIGBUS) {
        // File-backed EOF faults cannot be replayed through Memory::read:
        // the host memcpy would fault again outside registered native code.
        const int code = pending_code.load(std::memory_order_relaxed);
        if (dbg().fault_trace) fault_trace_block_pc() = block_pc;
        force_signal(*emu, *cpu, emu->signals(), BIFROST_SIGBUS,
                       code, pending_address.load(std::memory_order_relaxed));
        if (dbg().fault_trace) fault_trace_block_pc() = 0;
    } else if (dbg().fault_trace) {
        jit_guard_step(emu, cpu, block_pc);
    } else {
        // Revalidate in normal execution to distinguish guest MAPERR/ACCERR
        // and allow an access repaired concurrently with the host fault.
        jit_interp_step(emu, cpu);
    }
}
} // namespace arm64emu
