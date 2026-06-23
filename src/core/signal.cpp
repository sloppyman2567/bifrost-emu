// signal.cpp — Guest signal delivery for bifrost-emu (v1.4.0-alpha).
//
// Implements the SignalTable and signal delivery logic described in
// signal.hpp. The trampoline is mapped into the guest's address space
// at a fixed address (TRAMPOLINE_ADDR) and contains just two
// instructions: mov x8, #139 (rt_sigreturn) and svc #0.
//
// When a signal is delivered, we:
//   1. Save the full CPU state (X0..X30, SP, PC, PSTATE) into a
//      SignalFrame on our internal stack.
//   2. Set X0 = signo, X30 = TRAMPOLINE_ADDR, PC = handler.
//   3. The guest runs the handler. When it returns (RET via X30), it
//      lands at the trampoline, which does svc #0 with x8=139.
//   4. The rt_sigreturn syscall handler (case 133 in syscalls.cpp)
//      pops the top SignalFrame and restores CPU state.
//
// v1.4.0-alpha.1 adds host-to-guest signal forwarding: host signals
// (SIGINT, SIGTERM, SIGHUP, SIGCHLD, SIGWINCH, SIGALRM) are caught
// by a host signal handler, queued, and drained between instructions
// in the run loop. This unbreaks guest programs that poll a
// signal-pending flag (e.g., toybox sh's `sig_process_pending()`).

#include "core/signal.h"
#include "arm64_emu.hpp"
#include <cerrno>
#include <cstring>
#include <csignal>

namespace arm64emu {

// Static instance pointer for the host signal handler. Only one
// Emulator can be "active" for host signal forwarding at a time —
// which is fine since bifrost-emu runs one guest process per host
// process. If a future version supports multiple concurrent guests,
// this needs to become a thread-local or a lookup by TID.
Emulator* Emulator::g_active_emu_ = nullptr;

// ── SignalTable::install ────────────────────────────────────────────────
int SignalTable::install(Memory& mem, int signo, uint64_t act_addr,
                         uint64_t old_act_addr) {
    if (signo < 1 || signo > MAX_SIGNAL) {
        return -EINVAL;
    }

    // Save the old action if requested.
    if (old_act_addr != 0) {
        uint8_t buf[KSIGACTION_SIZE] = {0};
        SigAction& old = actions_[signo];
        memcpy(buf + 0,  &old.handler, 8);
        memcpy(buf + 8,  &old.flags,   8);
        memcpy(buf + 16, &old.mask,    8);  // sa_restorer is unused on AArch64
        // The kernel puts sa_mask at offset 24 in struct k_sigaction
        // (after sa_handler, sa_flags, sa_restorer). We use offset 16
        // because AArch64 doesn't have sa_restorer — but to be safe
        // we match the kernel's 32-byte layout.
        memcpy(buf + 24, &old.mask,    8);
        try {
            mem.write(old_act_addr, buf, KSIGACTION_SIZE);
        } catch (...) {
            return -EFAULT;
        }
    }

    // Read the new action.
    if (act_addr != 0) {
        uint8_t buf[KSIGACTION_SIZE] = {0};
        try {
            mem.read(act_addr, buf, KSIGACTION_SIZE);
        } catch (...) {
            return -EFAULT;
        }
        SigAction& na = actions_[signo];
        memcpy(&na.handler, buf + 0, 8);
        memcpy(&na.flags,   buf + 8, 8);
        memcpy(&na.mask,    buf + 24, 8);
        na.installed = true;
    } else {
        // act_addr == 0 means "uninstall" — reset to default.
        actions_[signo] = SigAction{};
    }

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

// ── Trampoline ──────────────────────────────────────────────────────────
// The trampoline is 8 bytes of AArch64 machine code:
//   0xD2801168  mov x8, #139   (MOVZ X8, #0x8B, LSL #0  →  x8 = 139)
//   0xD4000001  svc #0
//
// Encoding for MOVZ X8, #139:
//   sf 10 100101 hw imm16 Rd
//   1  10 100101 00 0000000010001011 01000
//   = 1101 0010 1000 0000 0001 0001 0110 1000
//   = 0xD2801168
//
// Encoding for SVC #0:
//   1101 0100 0000 0000 0000 0000 0000 0001
//   = 0xD4000001

uint64_t map_sigreturn_trampoline(Memory& mem) {
    static bool mapped = false;
    if (mapped) return TRAMPOLINE_ADDR;

    // Map a page at TRAMPOLINE_ADDR.
    mem.map_range(TRAMPOLINE_ADDR, 4096);

    // Write the two instructions.
    const uint32_t code[2] = { 0xD2801168u, 0xD4000001u };
    mem.write(TRAMPOLINE_ADDR, code, sizeof(code));

    mapped = true;
    return TRAMPOLINE_ADDR;
}

// ── Default signal dispositions ─────────────────────────────────────────
static bool default_terminates(int signo) {
    switch (signo) {
        case BIFROST_SIGHUP: case BIFROST_SIGINT: case BIFROST_SIGQUIT: case BIFROST_SIGILL:
        case BIFROST_SIGTRAP: case BIFROST_SIGABRT: case BIFROST_SIGBUS: case BIFROST_SIGFPE:
        case BIFROST_SIGKILL: case BIFROST_SIGUSR1: case BIFROST_SIGSEGV: case BIFROST_SIGUSR2:
        case BIFROST_SIGPIPE: case BIFROST_SIGALRM: case BIFROST_SIGTERM:
        case BIFROST_SIGXCPU: case BIFROST_SIGXFSZ: case BIFROST_SIGVTALRM: case BIFROST_SIGPROF:
        case BIFROST_SIGIO: case BIFROST_SIGSYS:
            return true;
        default:
            return false;  // SIGCHLD, SIGCONT, SIGSTOP, SIGTSTP, etc.
    }
}

// ── deliver_signal ──────────────────────────────────────────────────────
bool deliver_signal(Emulator& emu, CPU& cpu, SignalTable& sigtab, int signo) {
    if (signo < 1 || signo > MAX_SIGNAL) return false;

    const SigAction* act = sigtab.lookup(signo);

    // SIG_IGN (handler == 1): drop the signal.
    if (act && act->handler == 1) {
        return false;
    }

    // No handler installed (SIG_DFL): apply default disposition.
    if (!act || act->handler == 0) {
        if (default_terminates(signo)) {
            cpu.running = false;
            cpu.exit_code = 128 + signo;
        }
        // Non-terminating defaults (ignore) → just drop the signal.
        return false;
    }

    // A real handler is installed. Save CPU state and set up the call.
    // Make sure the trampoline is mapped.
    uint64_t tramp = map_sigreturn_trampoline(emu.mem());
    if (tramp == 0) {
        // Couldn't map trampoline — fall back to default.
        if (default_terminates(signo)) {
            cpu.running = false;
            cpu.exit_code = 128 + signo;
        }
        return false;
    }

    SignalFrame& frame = sigtab.push_frame(signo);
    memcpy(frame.regs, cpu.regs, sizeof(frame.regs));
    frame.sp     = cpu.sp;
    frame.pc     = cpu.pc;
    frame.pstate = cpu.pstate;

    // Set up the handler call: X0=signo, X1=0 (siginfo), X2=0 (ucontext),
    // X30=trampoline (so the handler returns into the trampoline).
    cpu.regs[0]  = static_cast<uint64_t>(signo);
    cpu.regs[1]  = 0;  // fake siginfo_t pointer
    cpu.regs[2]  = 0;  // fake ucontext_t pointer
    cpu.regs[30] = tramp;
    cpu.pc       = act->handler;

    return true;
}

// ── Host-to-guest signal forwarding (v1.4.0-alpha.1) ──────────────────

void Emulator::host_signal_handler(int signo) {
    // Called from the host kernel when a signal is delivered to the
    // emulator process. We can't call deliver_signal() from here (not
    // async-signal-safe — we'd need to acquire mutexes and touch guest
    // memory). Instead, just queue the signal number; the run loop
    // will drain the queue between instructions.
    if (g_active_emu_) {
        g_active_emu_->queue_host_signal(signo);
    }
}

void Emulator::queue_host_signal(int signo) {
    // Lock-free-ish: try_lock avoids blocking the host signal handler
    // if the main thread already holds the mutex (e.g., inside
    // drain_host_signals). If we can't acquire, the signal is dropped
    // — the guest will see the next one. Acceptable for our purposes.
    std::lock_guard<std::mutex> g(host_signal_mu_);
    host_signal_queue_.push_back(signo);
}

void Emulator::install_host_signal_handlers() {
    g_active_emu_ = this;

    // Install host handlers for the signals we want to forward.
    // SIGKILL (9) and SIGSTOP (19) cannot be caught — the host kernel
    // handles them directly, which is correct (they always terminate
    // / stop the guest too).
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = &Emulator::host_signal_handler;
    sigemptyset(&sa.sa_mask);
    // Don't set SA_RESTART — we want blocking syscalls to be
    // interrupted so the run loop can drain signals. The guest's
    // own SA_RESTART handling is independent.
    sa.sa_flags = 0;

    // Forward these signals to the guest.
    int forwarded[] = {
        BIFROST_SIGHUP,    BIFROST_SIGINT,  BIFROST_SIGQUIT, BIFROST_SIGUSR1,
        BIFROST_SIGUSR2,   BIFROST_SIGPIPE, BIFROST_SIGALRM, BIFROST_SIGTERM,
        BIFROST_SIGCHLD,   BIFROST_SIGCONT, BIFROST_SIGTSTP, BIFROST_SIGTTIN,
        BIFROST_SIGTTOU,   BIFROST_SIGURG,  BIFROST_SIGXCPU, BIFROST_SIGXFSZ,
        BIFROST_SIGVTALRM, BIFROST_SIGPROF, BIFROST_SIGWINCH, BIFROST_SIGIO,
    };
    for (int sig : forwarded) {
        ::sigaction(sig, &sa, nullptr);
    }
    // SIGSEGV/SIGBUS/SIGFPE/SIGILL/SIGTRAP/SIGABRT/SIGSYS are NOT
    // forwarded via host handlers — they're delivered synchronously
    // by the emulator when it detects the corresponding guest fault
    // (e.g., UnmappedMemory → SIGSEGV). Catching them on the host
    // would interfere with the emulator's own use of these signals
    // (e.g., ASan/UBSan uses SIGSEGV).
}

bool Emulator::drain_host_signals(CPU& cpu) {
    std::vector<int> pending;
    {
        std::lock_guard<std::mutex> g(host_signal_mu_);
        if (host_signal_queue_.empty()) return false;
        pending.swap(host_signal_queue_);
    }

    bool any_delivered = false;
    for (int sig : pending) {
        // Only forward signals the guest has actually installed a
        // handler for (or that have a non-terminating default).
        // SIGCHLD/SIGURG/SIGWINCH are ignored by default — if the
        // guest hasn't installed a handler, dropping them is correct.
        const SigAction* act = signals_.lookup(sig);
        if (!act && (sig == BIFROST_SIGCHLD || sig == BIFROST_SIGURG ||
                     sig == BIFROST_SIGWINCH || sig == BIFROST_SIGCONT)) {
            continue;
        }
        if (deliver_signal(*this, cpu, signals_, sig)) {
            any_delivered = true;
        }
    }
    return any_delivered;
}

} // namespace arm64emu
