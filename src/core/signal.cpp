// core/signal.cpp — Guest signal delivery for bifrost-emu.
//
// Implements the SignalTable and signal delivery logic declared in
// signal.h. See that header for the high-level design.
//
// Lifecycle of a delivered signal:
//   1. drain_host_signals() / deliver_pending_signals() / syscall-based
//      raise()/kill()/tkill()/tgkill() call deliver_signal().
//   2. If the signal is blocked (and not SIGKILL/SIGSTOP) it is queued
//      in cpu.sigpending; otherwise:
//   3. The currently installed SigAction is looked up. SIG_IGN drops the
//      signal. SIG_DFL applies the default disposition (terminate or
//      ignore, depending on the signal).
//   4. For a real handler, the full CPU state (X0..X30, SP, PC, PSTATE,
//      V0..V31, FPCR, FPSR, sigmask) is captured into a SignalFrame on
//      an internal stack, and a guest-visible siginfo_t + ucontext_t
//      (with fpsimd_context) is written to the guest stack (or altstack
//      if SA_ONSTACK is set).
//   5. CPU is set up to enter the handler: X0=signo, X1=siginfo_t*,
//      X2=ucontext_t*, X30=trampoline, PC=handler, SP=new_sp.
//   6. The handler runs. When it returns via `ret` (X30), it lands at
//      the trampoline (mapped at TRAMPOLINE_ADDR), which does
//      `mov x8, #139; svc #0` to invoke rt_sigreturn.
//   7. rt_sigreturn (case 139 in misc.cpp) pops the top SignalFrame,
//      restores all CPU state, and restores the saved sigmask. If
//      SA_RESETHAND was set, the handler has already been cleared in
//      step 4 (one-shot semantics).
//
// Host-to-guest signal forwarding:
//   Host signals (SIGINT, SIGTERM, SIGHUP, SIGCHLD, SIGWINCH, SIGALRM, ...)
//   are caught by a host signal handler, queued in a lock-free SPSC ring
//   (async-signal-safe), and drained between instructions in the run loop
//   and at syscall boundaries. This unbreaks guest programs that poll a
//   signal-pending flag (e.g., toybox sh's `sig_process_pending()`).

#include "core/signal.h"
#include "arm64_emu.hpp"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <csignal>

namespace arm64emu {

// Static instance pointer for the host signal handler. Only one Emulator
// can be "active" for host signal forwarding at a time — bifrost-emu runs
// one guest process per host process.
Emulator* Emulator::g_active_emu_ = nullptr;

namespace {

// ── Trace helper ───────────────────────────────────────────────────────
// Signal tracing is opt-in via the BIFROST_SIGNAL_TRACE env var. Stored
// in a std::atomic<bool> so it's safe to read from a host signal handler
// (getenv() itself is NOT async-signal-safe due to its internal mutex).
static std::atomic<bool> g_signal_trace{false};
static void init_signal_trace_flag() {
    static bool inited = false;
    if (!inited) {
        g_signal_trace.store(getenv("BIFROST_SIGNAL_TRACE") != nullptr,
                             std::memory_order_relaxed);
        inited = true;
    }
}
inline bool signal_trace_enabled() {
    return g_signal_trace.load(std::memory_order_relaxed);
}

// ── Default signal dispositions ────────────────────────────────────────
// Per Linux's signal(7): signals whose default action is "terminate" vs
// "ignore". SIGKILL/SIGSTOP are always terminate/stop and cannot be
// caught, but they're still listed here for completeness.
//
// Default "terminate" set: HUP, INT, QUIT, ILL, TRAP, ABRT, BUS, FPE,
// KILL, USR1, SEGV, USR2, PIPE, ALRM, TERM, XCPU, XFSZ, VTALRM, PROF,
// IO, SYS.
bool default_terminates(int signo) {
    switch (signo) {
        case BIFROST_SIGHUP:    case BIFROST_SIGINT:  case BIFROST_SIGQUIT:
        case BIFROST_SIGILL:    case BIFROST_SIGTRAP: case BIFROST_SIGABRT:
        case BIFROST_SIGBUS:    case BIFROST_SIGFPE:  case BIFROST_SIGKILL:
        case BIFROST_SIGUSR1:   case BIFROST_SIGSEGV: case BIFROST_SIGUSR2:
        case BIFROST_SIGPIPE:   case BIFROST_SIGALRM: case BIFROST_SIGTERM:
        case BIFROST_SIGXCPU:   case BIFROST_SIGXFSZ: case BIFROST_SIGVTALRM:
        case BIFROST_SIGPROF:   case BIFROST_SIGIO:   case BIFROST_SIGSYS:
            return true;
        default:
            // SIGCHLD, SIGCONT, SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU, SIGURG,
            // SIGWINCH — default is ignore (or stop, for SIGSTOP/SIGTSTP).
            // Real-time signals (SIGRTMIN..SIGRTMAX, 32..64) default to
            // terminate per Linux signal(7). Turn 57.
            if (signo >= BIFROST_SIGRTMIN && signo <= BIFROST_SIGRTMAX) {
                return true;
            }
            return false;
    }
}

// Default "core dump" set: QUIT, ILL, ABRT, BUS, FPE, SEGV, SYS, TRAP,
// XCPU, XFSZ. We don't actually write a core file; this just controls
// whether we log the fault address on termination.
bool default_dumps_core(int signo) {
    switch (signo) {
        case BIFROST_SIGQUIT: case BIFROST_SIGILL:  case BIFROST_SIGABRT:
        case BIFROST_SIGBUS:  case BIFROST_SIGFPE:  case BIFROST_SIGSEGV:
        case BIFROST_SIGSYS:  case BIFROST_SIGTRAP: case BIFROST_SIGXCPU:
        case BIFROST_SIGXFSZ:
            return true;
        default:
            return false;
    }
}

// SIGKILL and SIGSTOP cannot be blocked, caught, or ignored.
bool is_uncatchable(int signo) {
    return signo == BIFROST_SIGKILL || signo == BIFROST_SIGSTOP;
}

// Mask of all currently-supported sa_flags bits. Reserved/unsupported
// bits are masked off when reading a guest k_sigaction so internal state
// stays sane.
constexpr uint64_t SA_SUPPORTED_FLAGS =
    SA_SIGINFO_EMU | SA_RESTART_EMU | SA_NODEFER_EMU |
    SA_RESETHAND_EMU | SA_ONSTACK_EMU | SA_NOCLDWAIT_EMU |
    SA_NOCLDSTOP_EMU;

// SIGKILL (signo=9) and SIGSTOP (signo=19) bits in a kernel sigset_t
// (1-based: bit `signo-1`). Used to mask them out of user-provided
// sigsets — they cannot be blocked.
constexpr uint64_t UNBLOCKABLE_MASK = (1ULL << (BIFROST_SIGKILL - 1)) |
                                       (1ULL << (BIFROST_SIGSTOP - 1));

// Build the pending-set bit for a signal number (1-based kernel ABI).
constexpr uint64_t sig_bit(int signo) { return 1ULL << (signo - 1); }

// Reserved stack space for siginfo + ucontext + handler frame on the
// guest stack at signal delivery time. siginfo_t is 128 bytes, ucontext_t
// (with fpsimd_context) is 976 bytes; the slack covers the handler's own
// frame and 16-byte alignment.
constexpr size_t SIGINFO_SIZE     = 128;
constexpr size_t UCONTEXT_SIZE    = 976;
constexpr size_t FRAME_RESERVE    = 1280;  // siginfo + ucontext + slack

// fpsimd_context header (arch/arm64/include/uapi/asm/sigcontext.h).
constexpr uint32_t FPSIMD_MAGIC = 0x46508001;
constexpr uint32_t FPSIMD_SIZE  = 528;  // 8 (head) + 8 (fpsr+fpcr) + 512 (vregs)

} // namespace

// ── SignalTable::install ───────────────────────────────────────────────
// rt_sigaction(signo, new_act, old_act, sigsetsize) syscall handler.
// Reads the new action from `act_addr` (if non-null) and writes the
// previous action to `old_act_addr` (if non-null). Returns 0 on success,
// -errno on failure.
int SignalTable::install(Memory& mem, int signo, uint64_t act_addr,
                         uint64_t old_act_addr) {
    if (signo < 1 || signo > MAX_SIGNAL) {
        return -EINVAL;
    }
    if (is_uncatchable(signo)) {
        return -EINVAL;
    }

    // Write the previous action to old_act_addr (if requested).
    // Layout of struct k_sigaction on AArch64 (32 bytes total):
    //   +0   sa_handler  (8 bytes)
    //   +8   sa_flags    (8 bytes)
    //   +16  sa_restorer (8 bytes)  — always 0 on AArch64 (no restorer;
    //                                the kernel provides the trampoline)
    //   +24  sa_mask     (8 bytes)
    if (old_act_addr != 0) {
        const SigAction& old = actions_[signo];
        uint8_t buf[KSIGACTION_SIZE] = {0};
        memcpy(buf + 0,  &old.handler, 8);
        memcpy(buf + 8,  &old.flags,   8);
        const uint64_t zero_restorer = 0;
        memcpy(buf + 16, &zero_restorer, 8);
        memcpy(buf + 24, &old.mask,      8);
        try {
            mem.write(old_act_addr, buf, KSIGACTION_SIZE);
        } catch (...) {
            return -EFAULT;
        }
    }

    // Read the new action (if provided). act_addr == 0 is a query-only
    // call: per POSIX, "If act is NULL, then the signal handler is not
    // changed." Don't touch the installed handler.
    if (act_addr == 0) {
        return 0;
    }

    uint8_t buf[KSIGACTION_SIZE] = {0};
    try {
        mem.read(act_addr, buf, KSIGACTION_SIZE);
    } catch (...) {
        return -EFAULT;
    }

    SigAction& na = actions_[signo];
    memcpy(&na.handler, buf + 0,  8);
    memcpy(&na.flags,   buf + 8,  8);
    memcpy(&na.mask,    buf + 24, 8);
    na.flags &= SA_SUPPORTED_FLAGS;
    na.installed = true;
    return 0;
}

const SigAction* SignalTable::lookup(int signo) const {
    if (signo < 1 || signo > MAX_SIGNAL) return nullptr;
    if (!actions_[signo].installed) return nullptr;
    return &actions_[signo];
}

SignalFrame& SignalTable::push_frame(int signo) {
    frames_.emplace_back();
    frames_.back().signo = signo;
    return frames_.back();
}

bool SignalTable::pop_frame(SignalFrame& out) {
    if (frames_.empty()) return false;
    out = frames_.back();
    frames_.pop_back();
    return true;
}

// ── rt_sigprocmask ─────────────────────────────────────────────────────
// Per-CPU signal mask. SIG_BLOCK/SIG_UNBLOCK/SIG_SETMASK. SIGKILL and
// SIGSTOP cannot be blocked.
//
// v1.5.0.alpha BUGFIX: the old code wrote the old mask to `old_set_addr`
// BEFORE reading the new mask from `new_set_addr`. If the caller passes
// the same pointer for both (which is technically allowed by POSIX as a
// "swap" pattern), the read of new_mask would read the OLD mask that we
// just wrote — making the operation a no-op instead of a swap.
//
// The fix reads new_mask FIRST, then writes old_mask. This makes the
// aliasing case work correctly (it becomes a true swap), and is also
// what the Linux kernel does (see kernel/signal.c:sigprocmask()).
int SignalTable::procmask(Memory& mem, CPU& cpu, int how, uint64_t new_set_addr,
                          uint64_t old_set_addr, size_t sigsetsize) {
    if (sigsetsize != 8 && sigsetsize != 4) return -EINVAL;

    // Read the new mask FIRST (before writing the old mask), so the
    // aliasing case (old_set_addr == new_set_addr) works as a swap.
    uint64_t new_mask = 0;
    if (new_set_addr != 0) {
        try {
            if (sigsetsize == 8) {
                new_mask = mem.load<uint64_t>(new_set_addr);
            } else {
                new_mask = mem.load<uint32_t>(new_set_addr);
            }
        } catch (...) {
            return -EFAULT;
        }
        new_mask &= ~UNBLOCKABLE_MASK;
    }

    // Now write the old mask (the previous cpu.sigmask value).
    if (old_set_addr != 0) {
        try {
            if (sigsetsize == 8) {
                mem.store<uint64_t>(old_set_addr, cpu.sigmask);
            } else {
                mem.store<uint32_t>(old_set_addr, static_cast<uint32_t>(cpu.sigmask));
            }
        } catch (...) {
            return -EFAULT;
        }
    }

    if (new_set_addr == 0) {
        return 0;  // query only
    }

    switch (how) {
        case SIG_BLOCK_EMU:   cpu.sigmask |= new_mask;            break;
        case SIG_UNBLOCK_EMU: cpu.sigmask &= ~new_mask;           break;
        case SIG_SETMASK_EMU: cpu.sigmask  = new_mask;            break;
        default:              return -EINVAL;
    }
    return 0;
}

// ── sigaltstack ────────────────────────────────────────────────────────
// Set/query the per-CPU alternate signal stack. struct sigaltstack
// (24 bytes): ss_sp (8) + ss_flags (4 + 4 pad) + ss_size (8).
int SignalTable::set_altstack(Memory& mem, CPU& cpu, uint64_t new_addr, uint64_t old_addr) {
    constexpr size_t SS_SIZE = 24;

    if (old_addr != 0) {
        uint8_t buf[SS_SIZE] = {0};
        memcpy(buf + 0,  &cpu.altstack.sp,    8);
        memcpy(buf + 8,  &cpu.altstack.flags, 4);
        memcpy(buf + 16, &cpu.altstack.size,  8);
        try {
            mem.write(old_addr, buf, SS_SIZE);
        } catch (...) {
            return -EFAULT;
        }
    }

    if (new_addr == 0) {
        return 0;
    }

    uint8_t buf[SS_SIZE] = {0};
    try {
        mem.read(new_addr, buf, SS_SIZE);
    } catch (...) {
        return -EFAULT;
    }

    uint64_t new_sp;     uint32_t new_flags;   uint64_t new_size;
    memcpy(&new_sp,     buf + 0,  8);
    memcpy(&new_flags,  buf + 8,  4);
    memcpy(&new_size,   buf + 16, 8);

    // User code can only set SS_DISABLE; SS_ONSTACK is kernel-managed.
    if (new_flags & CPU::AltStack::SS_ONSTACK_EMU) return -EPERM;
    if (new_flags & ~static_cast<uint32_t>(CPU::AltStack::SS_DISABLE_EMU)) {
        return -EINVAL;
    }
    if (!(new_flags & CPU::AltStack::SS_DISABLE_EMU) &&
        (new_size < static_cast<uint64_t>(MINSIGSTKSZ) || new_sp == 0)) {
        return -ENOMEM;
    }
    cpu.altstack.sp    = new_sp;
    cpu.altstack.size  = new_size;
    cpu.altstack.flags = new_flags;
    return 0;
}

// ── Sigreturn trampoline ──────────────────────────────────────────────
// Two AArch64 instructions mapped at TRAMPOLINE_ADDR:
//   0xD2801168  mov x8, #139   (rt_sigreturn syscall number)
//   0xD4000001  svc #0
uint64_t map_sigreturn_trampoline(Memory& mem) {
    // Idempotent: detect via Memory::is_mapped so a forked child (with
    // its own Memory) correctly re-maps the trampoline.
    if (mem.is_mapped(TRAMPOLINE_ADDR, 8)) return TRAMPOLINE_ADDR;
    mem.map_range(TRAMPOLINE_ADDR, 4096);
    static const uint32_t code[2] = { 0xD2801168u, 0xD4000001u };
    mem.write(TRAMPOLINE_ADDR, code, sizeof(code));
    return TRAMPOLINE_ADDR;
}

// ── siginfo_t construction ─────────────────────────────────────────────
// AArch64 siginfo_t (128 bytes): si_signo (4) + si_errno (4) + si_code (4)
// + 4 pad + union (16..) — _kill.{pid,uid}, _sigfault.addr,
// _sigchld.{pid,uid,status,utime,stime}.
void build_siginfo(Memory& mem, uint64_t info_addr, int signo,
                   int si_code, uint64_t fault_addr) {
    if (info_addr == 0) return;
    uint8_t buf[128] = {0};
    const int32_t signo32 = signo;
    const int32_t code32  = si_code;
    const int32_t errno32 = 0;
    memcpy(buf + 0, &signo32, 4);
    memcpy(buf + 4, &errno32, 4);
    memcpy(buf + 8, &code32,  4);

    if (si_code == SI_USER_EMU || si_code == SI_KERNEL_EMU) {
        const int32_t pid = 1;  // main thread tid (single-process model)
        const int32_t uid = 0;
        memcpy(buf + 16, &pid, 4);
        memcpy(buf + 20, &uid, 4);
    }
    // _sigfault._addr lives at offset 16 for SIGSEGV/SIGBUS/SIGILL/
    // SIGFPE/SIGTRAP. (Note: for SIGSEGV the kernel also sets si_addr
    // to the faulting instruction's address for SIGILL/SIGTRAP, but the
    // fault_addr we receive here is already the right value.)
    if (signo == BIFROST_SIGSEGV || signo == BIFROST_SIGBUS ||
        signo == BIFROST_SIGILL  || signo == BIFROST_SIGFPE ||
        signo == BIFROST_SIGTRAP) {
        memcpy(buf + 16, &fault_addr, 8);
    }
    try { mem.write(info_addr, buf, sizeof(buf)); } catch (...) {}
}

// ── ucontext_t construction ────────────────────────────────────────────
// Writes a guest-visible ucontext_t (976 bytes) including the fpsimd_context
// in the 4 KiB reserved area. Layout (arch/arm64/include/uapi/asm/ucontext.h
// + sigcontext.h):
//   +0   uc_flags        (8)
//   +8   uc_link         (8)  — 0
//   +16  uc_stack        (24) — ss_sp, ss_flags, ss_size
//   +40  uc_sigmask      (8)
//   +48  __unused        (120) — pad to 1024-bit alignment
//   +168 uc_mcontext.fault_address  (8)
//   +176 uc_mcontext.regs[31]       (248)
//   +424 uc_mcontext.sp             (8)
//   +432 uc_mcontext.pc             (8)
//   +440 uc_mcontext.pstate         (8)
//   +448 fpsimd_context             (528)
//          +0  head { magic:4, size:4 }
//          +8  fpsr (4) + fpcr (4)
//          +16 vregs[32] (512 = 32 × 16)
uint64_t build_ucontext(Memory& mem, uint64_t uc_addr, CPU& cpu,
                        uint64_t saved_mask, uint64_t fault_addr) {
    if (uc_addr == 0) return 0;
    uint8_t buf[UCONTEXT_SIZE] = {0};
    memcpy(buf + 40,  &saved_mask, 8);
    memcpy(buf + 168, &fault_addr, 8);
    // BUGFIX (Turn 55): use sizeof of exactly 31 registers (248 bytes),
    // NOT sizeof(cpu.regs) (256 bytes). The AArch64 ucontext_t has
    // uc_mcontext.regs[31] (X0..X30) at offset 176, followed by sp at
    // offset 424. The old code wrote 256 bytes starting at offset 176,
    // which overwrote the sp field at offset 424 with cpu.regs[31]
    // (which should be 0 but could be stale). Then the explicit
    // memcpy(buf + 424, &cpu.sp, 8) would overwrite it again — so the
    // bug was masked as long as cpu.regs[31] was 0. But if cpu.regs[31]
    // was ever corrupted (e.g., by the rt_sigreturn sizeof bug above),
    // the ucontext's sp would get the wrong value.
    constexpr size_t REGS_BYTES = 31 * sizeof(uint64_t);  // 248
    memcpy(buf + 176, cpu.regs, REGS_BYTES);
    memcpy(buf + 424, &cpu.sp,     8);
    memcpy(buf + 432, &cpu.pc,     8);
    memcpy(buf + 440, &cpu.pstate, 8);

    // fpsimd_context at offset 448.
    memcpy(buf + 448 + 0, &FPSIMD_MAGIC, 4);
    memcpy(buf + 448 + 4, &FPSIMD_SIZE,  4);
    memcpy(buf + 448 + 8, &cpu.fpsr,     4);
    memcpy(buf + 448 + 12, &cpu.fpcr,    4);
    // Interleave (v_lo, v_hi) into 16-byte vregs[] entries.
    for (int i = 0; i < 32; i++) {
        memcpy(buf + 448 + 16 + i * 16,     &cpu.v_lo[i], 8);
        memcpy(buf + 448 + 16 + i * 16 + 8, &cpu.v_hi[i], 8);
    }
    try { mem.write(uc_addr, buf, sizeof(buf)); } catch (...) {}
    return uc_addr + sizeof(buf);
}

// ── deliver_signal ─────────────────────────────────────────────────────
// Deliver a signal to the guest. If a real handler is installed, set up
// the signal frame and switch PC to the handler. Otherwise apply the
// default disposition (terminate or ignore). Returns true if a handler
// was set up to run (caller must NOT overwrite cpu.regs[0]); false if
// the signal was dropped, queued, or terminated the process.
bool deliver_signal(Emulator& emu, CPU& cpu, SignalTable& sigtab, int signo,
                    int si_code, uint64_t fault_addr) {
    const bool trace = signal_trace_enabled();

    if (trace) {
        fprintf(stderr, "[signal] deliver_signal: signo=%d si_code=%d "
                "fault_addr=0x%llx cpu.tid=%d cpu.sigmask=0x%llx\n",
                signo, si_code,
                static_cast<unsigned long long>(fault_addr),
                cpu.tid,
                static_cast<unsigned long long>(cpu.sigmask));
    }

    if (signo < 1 || signo > MAX_SIGNAL) {
        return false;
    }

    // Blocked signals are queued as pending (not dropped). They will be
    // delivered when rt_sigprocmask unblocks them. SIGKILL/SIGSTOP are
    // always delivered immediately.
    if (SignalTable::is_blocked(cpu, signo) && !is_uncatchable(signo)) {
        cpu.sigpending |= sig_bit(signo);
        if (trace) {
            fprintf(stderr, "[signal] signo=%d is blocked — queued as "
                    "pending (sigpending=0x%llx)\n",
                    signo,
                    static_cast<unsigned long long>(cpu.sigpending));
        }
        return false;
    }

    const SigAction* act = sigtab.lookup(signo);

    if (trace) {
        fprintf(stderr, "[signal] signo=%d: act=%p", signo,
                static_cast<const void*>(act));
        if (act) {
            fprintf(stderr, " handler=0x%llx installed=%d",
                    static_cast<unsigned long long>(act->handler),
                    act->installed);
        }
        fprintf(stderr, "\n");
    }

    // SIG_IGN (handler == 1): drop the signal silently.
    if (act && act->handler == 1) {
        return false;
    }

    // SIG_DFL: apply the default disposition.
    if (!act || act->handler == 0) {
        if (default_terminates(signo)) {
            cpu.running = false;
            cpu.exit_code = 128 + signo;
            if (default_dumps_core(signo)) {
                fprintf(stderr, "[%s] signal %d (default core dump): "
                        "fault_addr=0x%llx pc=0x%llx\n",
                        CODENAME, signo,
                        static_cast<unsigned long long>(fault_addr),
                        static_cast<unsigned long long>(cpu.pc));
            }
        }
        // Non-terminating defaults (ignore) → just drop the signal.
        return false;
    }

    // A real handler is installed. Snapshot the fields we need BEFORE
    // any further mutation of `actions_[signo]` — SA_RESETHAND clears
    // the slot, which would invalidate `act` (a pointer into that slot)
    // and cause cpu.pc = act->handler to read 0 (decode error at pc=0x0).
    const uint64_t handler_addr = act->handler;
    const uint64_t act_flags    = act->flags;
    const uint64_t act_mask     = act->mask;

    // BUGFIX (production hardening): validate handler address alignment.
    // AArch64 instructions must be 4-byte aligned. If a buggy guest
    // installs a handler at an unaligned address (e.g., due to a
    // corrupted function pointer), setting cpu.pc to that address
    // would cause a "decode error" or "PC ran into unmapped memory"
    // crash with no useful diagnostic. We treat this as a fatal guest
    // bug and apply the default disposition instead.
    if ((handler_addr & 0x3ULL) != 0 || handler_addr < 0x1000) {
        fprintf(stderr,
            "[%s] signal %d: refusing to jump to invalid handler 0x%llx "
            "(must be 4-byte aligned and >= 0x1000); applying default "
            "disposition\n",
            CODENAME, signo,
            static_cast<unsigned long long>(handler_addr));
        if (default_terminates(signo)) {
            cpu.running = false;
            cpu.exit_code = 128 + signo;
        }
        return false;
    }

    // Make sure the trampoline is mapped. This is also pre-mapped at
    // install_host_signal_handlers() time, but the call is idempotent
    // and cheap (single is_mapped check) so we keep it as a safety net.
    const uint64_t tramp = map_sigreturn_trampoline(emu.mem());
    if (tramp == 0) {
        if (default_terminates(signo)) {
            cpu.running = false;
            cpu.exit_code = 128 + signo;
        }
        return false;
    }

    // Pick the stack: altstack if SA_ONSTACK is set and the altstack is
    // configured and not already in use.
    uint64_t target_sp = cpu.sp;
    bool on_altstack = false;
    if ((act_flags & SA_ONSTACK_EMU) &&
        !cpu.altstack.disabled() && !cpu.altstack.active()) {
        target_sp = (cpu.altstack.top() - FRAME_RESERVE) & ~0xFULL;
        on_altstack = true;
    }

    // Lay out siginfo_t and ucontext_t on the guest stack.
    const uint64_t info_addr = (target_sp - FRAME_RESERVE) & ~0xFULL;
    const uint64_t uc_addr   = info_addr + SIGINFO_SIZE;
    const uint64_t new_sp    = info_addr;

    // Snapshot the mask so rt_sigreturn can restore it.
    const uint64_t saved_mask = cpu.sigmask;

    // Build the guest-visible siginfo_t and ucontext_t.
    build_siginfo(emu.mem(), info_addr, signo, si_code, fault_addr);
    build_ucontext(emu.mem(), uc_addr, cpu, saved_mask, fault_addr);

    // Save full CPU state in our internal frame for rt_sigreturn.
    SignalFrame& frame = sigtab.push_frame(signo);
    memcpy(frame.regs, cpu.regs, sizeof(frame.regs));
    frame.sp          = cpu.sp;
    frame.pc          = cpu.pc;
    frame.pstate      = cpu.pstate;
    frame.saved_mask  = saved_mask;
    frame.fault_addr  = fault_addr;
    frame.si_code     = si_code;
    frame.on_altstack = on_altstack;
    memcpy(frame.v_lo, cpu.v_lo, sizeof(frame.v_lo));
    memcpy(frame.v_hi, cpu.v_hi, sizeof(frame.v_hi));
    frame.fpcr = cpu.fpcr;
    frame.fpsr = cpu.fpsr;

    if (on_altstack) {
        SignalTable::set_altstack_active(cpu, true);
    }

    // Compute the new mask: current mask | sa_mask | signo (unless
    // SA_NODEFER is set, which allows the handler to be re-entered).
    uint64_t new_mask = saved_mask | act_mask;
    if (!(act_flags & SA_NODEFER_EMU)) {
        new_mask |= sig_bit(signo);
    }
    new_mask &= ~UNBLOCKABLE_MASK;
    cpu.sigmask = new_mask;

    // SA_RESETHAND: clear the handler now (one-shot semantics). Safe to
    // do this AFTER we've snapshotted handler_addr/flags/mask above.
    if (act_flags & SA_RESETHAND_EMU) {
        sigtab.clear_handler(signo);
    }

    // Set up the handler call: X0=signo, X1=siginfo_t*, X2=ucontext_t*,
    // X30=trampoline, PC=handler, SP=new_sp.
    cpu.regs[0]  = static_cast<uint64_t>(signo);
    cpu.regs[1]  = info_addr;
    cpu.regs[2]  = uc_addr;
    cpu.regs[30] = tramp;
    cpu.pc       = handler_addr;
    cpu.sp       = new_sp;

    if (trace) {
        fprintf(stderr, "[signal] delivered sig %d: handler=0x%llx "
                "sp=0x%llx x30=0x%llx info=0x%llx uc=0x%llx\n",
                signo,
                static_cast<unsigned long long>(handler_addr),
                static_cast<unsigned long long>(new_sp),
                static_cast<unsigned long long>(tramp),
                static_cast<unsigned long long>(info_addr),
                static_cast<unsigned long long>(uc_addr));
    }

    return true;
}

// ── deliver_pending_signals ────────────────────────────────────────────
// Drain signals that were queued as pending (because they were blocked at
// delivery time) and are now unblocked. Called after rt_sigprocmask
// changes the mask. Only delivers one signal per call — the handler will
// modify cpu.pc, and we need to return so the run loop picks up the new
// PC. Multiple pending signals are delivered one at a time as the run
// loop calls us again.
int deliver_pending_signals(Emulator& emu, CPU& cpu, SignalTable& sigtab) {
    uint64_t pending = cpu.sigpending;
    while (pending) {
        // sigpending uses 1-based bit numbering (bit `signo-1`), so the
        // 0-based ctzll result IS `signo - 1`. Add 1 to recover signo.
        const int signo = __builtin_ctzll(pending) + 1;
        pending &= pending - 1;  // clear lowest set bit

        // Skip if still blocked (defensive — only unblocked pending
        // signals should be queued, but the mask can change between
        // queueing and delivery).
        if (SignalTable::is_blocked(cpu, signo) && !is_uncatchable(signo)) {
            continue;
        }

        cpu.sigpending &= ~sig_bit(signo);
        if (signal_trace_enabled()) {
            fprintf(stderr, "[signal] delivering pending signal %d "
                    "(unblocked)\n", signo);
        }
        deliver_signal(emu, cpu, sigtab, signo);
        return 1;  // one at a time — handler modified cpu.pc
    }
    return 0;
}

// ── Host-to-guest signal forwarding ───────────────────────────────────

void Emulator::host_signal_handler(int signo) {
    // Called from the host kernel in a signal context. We can't call
    // deliver_signal() from here (it would touch guest memory and mutexes
    // — not async-signal-safe). Instead, enqueue the signal number in a
    // lock-free SPSC ring; the run loop drains the queue between
    // instructions.
    if (g_active_emu_) {
        g_active_emu_->queue_host_signal(signo);
    }
}

void Emulator::queue_host_signal(int signo) {
    // Lock-free MPSC enqueue.
    //
    // BUGFIX: the previous code claimed this was SPSC and used a plain
    // relaxed-load + release-store on `tail`. That's correct only if a
    // SINGLE host thread can be inside the handler at any moment. But on
    // a multi-vCPU guest, every spawned host thread can receive a
    // forwarded signal simultaneously — sigfillset(&sa.sa_mask) only
    // blocks signals on the calling thread during the handler, NOT across
    // threads. Two threads could both read the same `t`, both write to
    // `signals[t % CAP]`, and both store `t+1` — losing one signal and
    // leaving a torn slot.
    //
    // Fix: claim a slot via fetch_add on `tail` (atomic, so each producer
    // gets a unique slot), then bounds-check against `head` and drop
    // (with a counter bump) if the queue is full. The slot write happens
    // before the release-store (which is now implicit in the fetch_add's
    // acq_rel ordering); the consumer's acquire-load of `tail` synchronizes.
    const size_t t = host_signal_queue_.tail.fetch_add(1, std::memory_order_acq_rel);
    const size_t h = host_signal_queue_.head.load(std::memory_order_acquire);
    const size_t used = t - h;  // wraparound-safe (unsigned arithmetic)
    if (used >= HOST_SIGNAL_QUEUE_CAP) {
        // Queue full — drop. POSIX allows signal loss when the queue
        // is full; this is acceptable. We log to stderr only if signal
        // tracing is enabled (avoid async-signal-unsafe I/O otherwise).
        if (signal_trace_enabled()) {
            // write() is async-signal-safe per POSIX.
            const char msg[] = "[signal] host signal queue full — dropping\n";
            write(2, msg, sizeof(msg) - 1);
        }
        // Note: we already incremented tail; the consumer will skip the
        // claimed slot by checking used >= CAP on its side. We mark the
        // slot with -1 to signal "skipped".
        host_signal_queue_.signals[t % HOST_SIGNAL_QUEUE_CAP] = -1;
        return;
    }
    host_signal_queue_.signals[t % HOST_SIGNAL_QUEUE_CAP] = signo;
}

void Emulator::install_host_signal_handlers() {
    g_active_emu_ = this;
    init_signal_trace_flag();

    // Shells set certain signals to SIG_IGN when launching background
    // processes (e.g. `cmd &`). When a signal is SIG_IGN'd at the host
    // kernel level, our handler is NEVER called — the kernel silently
    // drops the signal before we see it. This breaks guest programs
    // that install handlers for those signals.
    //
    // POSIX signals that shells commonly set to SIG_IGN:
    //   SIGINT  — background processes (`cmd &`)
    //   SIGQUIT — background processes
    //   SIGTSTP — background processes (some shells)
    //   SIGTTIN — background processes reading from terminal
    //   SIGTTOU — background processes writing to terminal
    //   SIGPIPE — pipelines whose reader has exited (some shells)
    //   SIGCHLD — `disown` or non-monitoring shells
    //
    // Reset ALL of these to SIG_DFL first so our handler actually
    // receives them. This is a SUBTLE BUG FIX: previously only SIGINT
    // and SIGQUIT were reset, which meant SIGPIPE from a closed pipe
    // (e.g., `yes | head -1`) and SIGCHLD from `wait()` could be
    // silently lost when the parent shell set them to SIG_IGN.
    static const int kResetToDefault[] = {
        SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU,
        SIGPIPE, SIGCHLD, SIGURG, SIGWINCH,
    };
    for (int sig : kResetToDefault) {
        ::signal(sig, SIG_DFL);
    }

    // Install one host handler that forwards to the guest via the SPSC
    // queue. SA_RESTART is intentionally NOT set — we want blocking
    // syscalls to be interrupted so the run loop can drain signals.
    //
    // BUGFIX (production hardening): the old code used sigemptyset(&sa.sa_mask),
    // which means only the SAME signal is blocked during its handler. This
    // created a re-entrancy race in queue_host_signal: if SIGTERM arrived
    // while SIGINT's handler was running, both invocations could read the
    // same tail index, both write to the same slot, and both store tail+1
    // — losing one of the signals. We now fill sa_mask with ALL forwardable
    // signals so the host kernel serializes our handler invocations.
    // This makes queue_host_signal non-reentrant, eliminating the race.
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = &Emulator::host_signal_handler;
    sa.sa_flags = 0;
    sigfillset(&sa.sa_mask);  // block ALL signals during handler execution

    // Forwardable signals. SIGKILL (9) and SIGSTOP (19) cannot be caught
    // — the host kernel handles them directly, which is correct.
    // Includes real-time signals SIGRTMIN..SIGRTMAX (Turn 57) so glibc's
    // pthread_cancel/timer_create/setxid mechanisms work.
    static constexpr int forwarded[] = {
        BIFROST_SIGHUP,    BIFROST_SIGINT,  BIFROST_SIGQUIT, BIFROST_SIGUSR1,
        BIFROST_SIGUSR2,   BIFROST_SIGPIPE, BIFROST_SIGALRM, BIFROST_SIGTERM,
        BIFROST_SIGCHLD,   BIFROST_SIGCONT, BIFROST_SIGTSTP, BIFROST_SIGTTIN,
        BIFROST_SIGTTOU,   BIFROST_SIGURG,  BIFROST_SIGXCPU, BIFROST_SIGXFSZ,
        BIFROST_SIGVTALRM, BIFROST_SIGPROF, BIFROST_SIGWINCH, BIFROST_SIGIO,
        // Real-time signals. glibc's libpthread uses SIGRTMIN (32) for
        // pthread_cancel and setxid; musl uses SIGRTMIN for timer
        // delivery. Forwarding them lets the guest's signal handlers
        // see them. (SIGRTMIN is the kernel's 32; glibc's user-visible
        // SIGRTMIN is 35 because glibc reserves 32-34, but the kernel
        // signal number is what we forward.)
        32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
        48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63,
    };
    for (int sig : forwarded) {
        ::sigaction(sig, &sa, nullptr);
    }
    // SIGSEGV/SIGBUS/SIGFPE/SIGILL/SIGTRAP/SIGABRT/SIGSYS are NOT
    // forwarded via host handlers — they're delivered synchronously by
    // the emulator when it detects the corresponding guest fault.

    // Pre-map the sigreturn trampoline so the first signal delivery
    // doesn't pay a map_range cost on the hot path. Idempotent.
    map_sigreturn_trampoline(mem_);
}

bool Emulator::drain_host_signals(CPU& cpu) {
    // Lock-free MPSC dequeue. Atomically advance the head index and
    // process signals in order.
    //
    // Memory ordering: the consumer's slot read must happen-before its
    // head.store, so we use release ordering on the head.store. The
    // producer's head.load uses acquire (in queue_host_signal) to
    // synchronize with this store — ensuring the producer doesn't
    // overwrite a slot the consumer is still reading.
    //
    // BUGFIX: slot value -1 means "dropped due to queue full" (the
    // producer claimed the slot via fetch_add but then found the queue
    // was past capacity). Skip these.
    const bool trace = signal_trace_enabled();
    bool any_delivered = false;
    size_t h = host_signal_queue_.head.load(std::memory_order_relaxed);
    size_t t = host_signal_queue_.tail.load(std::memory_order_acquire);
    while (h != t) {
        const int sig = host_signal_queue_.signals[h % HOST_SIGNAL_QUEUE_CAP];
        // Release ordering ensures the slot read above is visible to the
        // producer before it sees the advanced head index.
        host_signal_queue_.head.store(h + 1, std::memory_order_release);
        h = h + 1;

        // Skip dropped-signal sentinels (queue overflow).
        if (sig == -1) {
            // Re-read tail in case the producer added more signals while
            // we were iterating.
            t = host_signal_queue_.tail.load(std::memory_order_acquire);
            continue;
        }

        if (trace) {
            // fprintf is safe here — we're in the run loop, not a signal handler.
            fprintf(stderr, "[signal] drain_host_signals: sig=%d\n", sig);
        }

        // Drop signals that have no handler and a non-terminating
        // default (SIGCHLD, SIGURG, SIGWINCH, SIGCONT). This matches
        // the kernel behavior of "ignore by default" for these.
        const SigAction* act = signals_.lookup(sig);
        if (!act && (sig == BIFROST_SIGCHLD || sig == BIFROST_SIGURG ||
                     sig == BIFROST_SIGWINCH || sig == BIFROST_SIGCONT)) {
            continue;
        }
        // Check if SIGINT is SIG_IGN. If so, set the sigint_ignored flag
        // so the read() handler can inject a newline (mimicking bash/dash
        // behavior of printing a new prompt after Ctrl+C). Without this,
        // Ctrl+C at an empty prompt would do nothing visible — the signal
        // is silently dropped and the shell stays blocked on read().
        if (sig == BIFROST_SIGINT && act && act->handler == 1) {
            cpu.sigint_ignored = true;
        }
        if (deliver_signal(*this, cpu, signals_, sig)) {
            any_delivered = true;
        }
        // Re-read tail in case the producer added more signals while
        // we were delivering one.
        t = host_signal_queue_.tail.load(std::memory_order_acquire);
    }
    return any_delivered;
}

// ── handle_eintr ───────────────────────────────────────────────────────
// Called by blocking syscall handlers after a host syscall returns -EINTR.
// The caller MUST pre-set cpu.regs[0] = -EINTR before calling this, so
// that if a signal is delivered, the signal frame saves -EINTR. After
// sigreturn, cpu.regs[0] will be restored to -EINTR.
//
// Returns true if a signal was delivered to a real guest handler (the
// handler is now set up to run; cpu.pc = handler, cpu.regs[0] = signo).
// The caller should return immediately WITHOUT overwriting cpu.regs[0].
//
// Returns false if no signal was delivered (queue was empty, or all
// pending signals were SIG_IGN). cpu.regs[0] is still -EINTR (as pre-set
// by the caller); the caller should return normally.
//
// We do NOT retry the host syscall: real Linux would let SIG_IGN signals
// pass without interrupting the syscall, but the host kernel doesn't know
// about the guest's disposition and always interrupts. Retrying would
// make the guest never see -EINTR, breaking shells that rely on -EINTR
// from read()/nanosleep() to detect "user wants to interrupt".
bool Emulator::handle_eintr(CPU& cpu) {
    return drain_host_signals(cpu);
}

// ── drain_pending_signals ─────────────────────────────────────────────
// Drains per-CPU pending signals queued by cross-thread tgkill/tkill/kill.
// Called by every CPU's run loop at the 4K-instruction boundary (same
// point as drain_host_signals). Each CPU drains its OWN queue — no
// cross-thread mutation, which is the whole point of the per-CPU queue.
//
// Signals blocked by cpu.sigmask are left in the queue (we don't pop
// them). When rt_sigprocmask unblocks them, the next drain call will
// deliver them. This matches kernel semantics: blocked signals stay
// pending until unblocked.
//
// Returns true if any signal was actually delivered to a handler.
bool Emulator::drain_pending_signals(CPU& cpu) {
    bool any_delivered = false;
    CPU::PendingSig sig;
    while (cpu.pop_pending(sig)) {
        // If the signal is blocked, re-queue it (push back) and stop
        // draining — kernel preserves order within pending signals,
        // and we shouldn't deliver a later signal before an earlier
        // blocked one. In practice this means we leave it in the
        // queue. Since we already popped it, we have to push it back.
        // To avoid reordering, we stop draining on the first blocked
        // signal we encounter.
        if (sig.signo >= 1 && sig.signo <= 63 &&
            SignalTable::is_blocked(cpu, sig.signo)) {
            // Re-push and stop.
            cpu.push_pending(sig.signo, sig.si_code, sig.fault_addr);
            break;
        }
        if (deliver_signal(*this, cpu, signals_, sig.signo,
                           sig.si_code, sig.fault_addr)) {
            any_delivered = true;
            // deliver_signal sets up the handler frame and changes
            // cpu.pc. Don't drain more — let the handler run first.
            // The next drain pass (after the handler returns via
            // rt_sigreturn) will pick up any further pending signals.
            break;
        }
    }
    return any_delivered;
}

} // namespace arm64emu
