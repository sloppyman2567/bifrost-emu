// core/signal.h — guest signal delivery for bifrost-emu.
//
// Provides minimal, experimental signal delivery for guest programs.
// Most AArch64 Linux user-mode emulators punt on signals entirely
// because the full semantics (siginfo_t, ucontext_t, signal stacks,
// SA_RESTART, real-time signals, etc.) are very complex. We implement
// just enough to make common patterns work:
//
//   1. rt_sigaction(signo, new_act, old_act) — stores the guest's
//      handler address and flags in a table.
//   2. When the emulator detects a fault that the guest has registered
//      a handler for (currently just SIGSEGV from unmapped memory
//      accesses), it pushes a signal frame and jumps to the handler.
//   3. The handler runs with:
//        X0 = signal number
//        X1 = 0 (fake siginfo_t pointer — we don't construct one)
//        X2 = 0 (fake ucontext_t pointer — we don't construct one)
//        X30 = address of the sigreturn trampoline
//   4. When the handler returns, it returns to the trampoline, which
//      is a small piece of AArch64 code (mapped at a fixed guest
//      address) that does:
//        mov x8, #__NR_rt_sigreturn
//        svc #0
//   5. The rt_sigreturn syscall restores the saved CPU state from
//      our internal stack of signal frames.
//
// Limitations (clearly documented for the user):
//   - Only signals 1..31 are supported (no real-time signals 32+).
//   - No siginfo_t / ucontext_t contents — handlers that inspect
//     them will see zeros.
//   - No SA_RESTART (syscalls don't auto-restart).
//   - No signal masks (rt_sigprocmask is still a no-op).
//   - No alternative signal stacks (sigaltstack not supported).
//   - Nested signal delivery: a signal handler can be interrupted by
//     another signal, but we don't enforce per-signal masking.
//
// The trampoline lives at TRAMPOLINE_ADDR (a fixed high address that
// doesn't collide with the guest's stack, heap, or mmap region).
//
// This is a private header — only Emulator and the syscalls layer
// include it. The public surface is the SignalTable class itself.
#pragma once

#include "bifrost/types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace arm64emu {

// Linux signal numbers (asm-generic/signal.h).
constexpr int BIFROST_SIGHUP    = 1;
constexpr int BIFROST_SIGINT    = 2;
constexpr int BIFROST_SIGQUIT   = 3;
constexpr int BIFROST_SIGILL    = 4;
constexpr int BIFROST_SIGTRAP   = 5;
constexpr int BIFROST_SIGABRT   = 6;
constexpr int BIFROST_SIGBUS    = 7;
constexpr int BIFROST_SIGFPE    = 8;
constexpr int BIFROST_SIGKILL   = 9;
constexpr int BIFROST_SIGUSR1   = 10;
constexpr int BIFROST_SIGSEGV   = 11;
constexpr int BIFROST_SIGUSR2   = 12;
constexpr int BIFROST_SIGPIPE   = 13;
constexpr int BIFROST_SIGALRM   = 14;
constexpr int BIFROST_SIGTERM   = 15;
constexpr int BIFROST_SIGCHLD   = 17;
constexpr int BIFROST_SIGCONT   = 18;
constexpr int BIFROST_SIGSTOP   = 19;
constexpr int BIFROST_SIGTSTP   = 20;
constexpr int BIFROST_SIGTTIN   = 21;
constexpr int BIFROST_SIGTTOU   = 22;
constexpr int BIFROST_SIGURG    = 23;
constexpr int BIFROST_SIGXCPU   = 24;
constexpr int BIFROST_SIGXFSZ   = 25;
constexpr int BIFROST_SIGVTALRM = 26;
constexpr int BIFROST_SIGPROF   = 27;
constexpr int BIFROST_SIGWINCH  = 28;
constexpr int BIFROST_SIGIO     = 29;
constexpr int BIFROST_SIGSYS    = 31;

constexpr int MAX_SIGNAL = 31;

// Fixed guest address where we map the sigreturn trampoline.
// Chosen to be far from the stack (0x8000000000) and mmap region
// (0x5000000000+) so it never collides.
constexpr uint64_t TRAMPOLINE_ADDR = 0x7000000000ULL;

// Linux struct k_sigaction (AArch64 layout, simplified):
//   void  (*sa_handler)(int)         — at offset 0
//   unsigned long sa_flags           — at offset 8
//   void  (*sa_restorer)(void)       — at offset 16  (unused on AArch64)
//   sigset_t sa_mask                 — at offset 24  (8 bytes on AArch64)
//
// Total size: 32 bytes. Matches the kernel's struct k_sigaction.
constexpr size_t KSIGACTION_SIZE = 32;

// Saved CPU state for signal delivery.
struct SignalFrame {
    uint64_t regs[31];   // X0..X30
    uint64_t sp;
    uint64_t pc;
    uint32_t pstate;
    int      signo;      // which signal this frame is for
};

// Per-signal action recorded by rt_sigaction.
struct SigAction {
    uint64_t handler    = 0;       // 0 = SIG_DFL, 1 = SIG_IGN, else handler addr
    uint64_t flags      = 0;
    uint64_t mask       = 0;
    bool     installed  = false;   // has the guest set a handler?
};

class SignalTable {
public:
    SignalTable() = default;

    // Install a handler for `signo` from a guest-side struct k_sigaction
    // at address `act_addr`. If `old_act_addr` is non-zero, write the
    // previous action there. Returns 0 on success, -errno on failure.
    int install(Memory& mem, int signo, uint64_t act_addr, uint64_t old_act_addr);

    // Look up the installed action for `signo`. Returns nullptr if no
    // handler is installed (caller should apply default behavior).
    const SigAction* lookup(int signo) const;

    // Push a signal frame onto the internal stack and return a reference
    // to it. The caller fills in the saved CPU state.
    SignalFrame& push_frame(int signo);

    // Pop the most recent signal frame.
    bool pop_frame(SignalFrame& out);

    bool has_pending() const { return !frames_.empty(); }
    size_t frame_count() const { return frames_.size(); }

private:
    SigAction actions_[MAX_SIGNAL + 1];  // indexed by signo (1..31)
    std::vector<SignalFrame> frames_;
};

// Map the sigreturn trampoline into the guest's address space.
// Writes the 8-byte AArch64 sequence:
//   mov x8, #139      // __NR_rt_sigreturn
//   svc #0
// at TRAMPOLINE_ADDR. Idempotent — safe to call multiple times.
// Returns TRAMPOLINE_ADDR on success, 0 on failure.
uint64_t map_sigreturn_trampoline(Memory& mem);

// Deliver a signal to the guest. If a handler is installed, saves
// the current CPU state, sets up X0=signo / X30=trampoline / PC=handler,
// and returns true. If no handler is installed (SIG_DFL), applies
// the default disposition:
//   - SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGSYS → terminate
//     the guest with exit code 128 + signo (matching shell convention).
//   - SIGINT, SIGTERM, SIGHUP, SIGQUIT → terminate with 128 + signo.
//   - SIGPIPE, SIGCHLD, SIGURG, SIGWINCH, SIGCONT → ignored by default.
//   - Other signals → terminate with 128 + signo (safe default).
//
// Returns true if the signal was delivered (handler invoked), false
// if the default disposition was applied.
bool deliver_signal(Emulator& emu, CPU& cpu, SignalTable& sigtab, int signo);

} // namespace arm64emu
