// core/signal.h — guest signal delivery for bifrost-emu.
//
// Provides production-quality signal delivery for guest programs.
// The full Linux signal semantics (siginfo_t, ucontext_t, signal stacks,
// SA_RESTART, real-time signals, etc.) are very complex; we implement a
// substantial subset that covers the common cases real programs use:
//
//   1. rt_sigaction(signo, new_act, old_act) — stores the guest's
//      handler address, flags, and per-signal mask in a table.
//      Supported flags: SA_SIGINFO, SA_RESTART, SA_NODEFER, SA_RESETHAND,
//      SA_ONSTACK (honored by sigaltstack), SA_NOMASK (alias for SA_NODEFER).
//   2. rt_sigprocmask(how, new, old, sigsetsize) — per-CPU signal mask.
//      Supported `how`: SIG_BLOCK, SIG_UNBLOCK, SIG_SETMASK.
//   3. sigaltstack(new, old) — alternate signal stack (SS_ONSTACK / SS_DISABLE).
//   4. When the emulator detects a fault that the guest has registered
//      a handler for (currently SIGSEGV from unmapped memory accesses,
//      and synchronous SIGFPE/SIGILL/SIGBUS from interpreter detection),
//      it pushes a signal frame and jumps to the handler.
//   5. The handler runs with:
//        X0 = signal number
//        X1 = siginfo_t pointer (with si_signo, si_code, si_addr, si_pid)
//        X2 = ucontext_t pointer (with uc_mcontext.regs, uc_mcontext.pc,
//              uc_mcontext.pstate, uc_sigmask)
//        X30 = address of the sigreturn trampoline
//   6. When the handler returns, it returns to the trampoline, which
//      is a small piece of AArch64 code (mapped at a fixed guest
//      address) that does:
//        mov x8, #__NR_rt_sigreturn
//        svc #0
//   7. The rt_sigreturn syscall restores the saved CPU state from
//      our internal stack of signal frames, including the saved signal
//      mask.
//
// Limitations (clearly documented for the user):
//   - Signals 1..31 (classic) and 32..64 (real-time) are supported.
//   - SA_RESTART is recorded but only honored for a small set of
//     blocking syscalls (read, write, poll, ppoll, futex). Other
//     syscalls return -EINTR when interrupted.
//   - No sigqueue()/rt_sigqueueinfo() (signal payload always defaults
//     to SI_USER with si_pid=getpid, si_uid=0).
//   - Nested signal delivery: a handler can be interrupted by another
//     signal, but per-signal masking during handler execution is
//     best-effort (we add the signal's sa_mask + the signo itself to
//     the mask while the handler runs, then restore on rt_sigreturn).
//
// The trampoline lives at TRAMPOLINE_ADDR (a fixed high address that
// doesn't collide with the guest's stack, heap, or mmap region).
//
// This is a private header — only Emulator and the syscalls layer
// include it. The public surface is the SignalTable class itself.
#pragma once
#include "bifrost/types.hpp"
#include "core/cpu.h"
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
// Real-time signal range (Linux SIGRTMIN..SIGRTMAX).
// SIGRTMIN is 32 on AArch64 Linux (glibc reserves 32-35 internally, but
// the kernel's SIGRTMIN is 32). SIGRTMAX is 64. We support all 33 RT
// signals (32..64) so glibc's pthread_cancel, timer_create, setxid,
// and musl's timer delivery all work.
constexpr int BIFROST_SIGRTMIN  = 32;
constexpr int BIFROST_SIGRTMAX  = 64;
// Maximum signal number supported (covers classic 1..31 + RT 32..64).
constexpr int MAX_SIGNAL = 64;
// Signal mask is 64-bit (covers signals 1..64, including RT signals).
using sigset_t_emu = uint64_t;
// sigprocmask `how` values.
constexpr int SIG_BLOCK_EMU   = 0;  // SIG_BLOCK
constexpr int SIG_UNBLOCK_EMU = 1;  // SIG_UNBLOCK
constexpr int SIG_SETMASK_EMU = 2;  // SIG_SETMASK
// sa_flags bits (Linux).
constexpr uint64_t SA_SIGINFO_EMU   = 0x00000004;
constexpr uint64_t SA_RESTART_EMU   = 0x10000000;
constexpr uint64_t SA_NODEFER_EMU   = 0x40000000;
constexpr uint64_t SA_RESETHAND_EMU = 0x80000000;
constexpr uint64_t SA_ONSTACK_EMU   = 0x08000000;
constexpr uint64_t SA_NOCLDWAIT_EMU = 0x00000002;
constexpr uint64_t SA_NOCLDSTOP_EMU = 0x00000001;
// siginfo_t si_code values (subset).
constexpr int SI_USER_EMU     = 0;
constexpr int SI_KERNEL_EMU   = 0x80;
constexpr int SEGV_MAPERR_EMU = 1;
constexpr int SEGV_ACCERR_EMU = 2;
constexpr int ILL_ILLOPC_EMU  = 1;
constexpr int FPE_INTDIV_EMU  = 1;
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
    int      signo;       // which signal this frame is for
    uint64_t saved_mask;  // signal mask to restore on rt_sigreturn
    uint64_t fault_addr;  // si_addr for SIGSEGV/SIGBUS
    int      si_code;     // si_code for siginfo_t
    bool     on_altstack; // was the handler entered on the altstack?
    // BUGFIX: FP/SIMD state was NOT preserved across signal handlers.
    // Handlers using NEON (crypto, DSP, image processing) would see
    // corrupted V registers. Now we save/restore the full FP file +
    // FPCR/FPSR. The guest-visible ucontext_t also gets an fpsimd_context
    // written into its 4 KiB reserved area (see build_ucontext).
    uint64_t v_lo[32];    // bits 63:0 of each V register
    uint64_t v_hi[32];    // bits 127:64 of each V register
    uint32_t fpcr;
    uint32_t fpsr;
};
// Per-signal action recorded by rt_sigaction.
struct SigAction {
    uint64_t handler    = 0;       // 0 = SIG_DFL, 1 = SIG_IGN, else handler addr
    uint64_t flags      = 0;
    uint64_t mask       = 0;       // additional signals to block during handler
    bool     installed  = false;   // has the guest set a handler?
};
// Alternate signal stack (sigaltstack).
// Kept as a free struct for backwards compat with code that constructs
// one directly. The per-CPU state lives in CPU::altstack.
struct AltStack {
    uint64_t sp       = 0;   // base address of stack
    uint64_t size     = 0;   // size in bytes
    uint32_t flags    = 0;   // SS_ONSTACK / SS_DISABLE
    static constexpr uint32_t SS_ONSTACK_EMU  = 1;
    static constexpr uint32_t SS_DISABLE_EMU  = 2;
    bool active() const { return (flags & SS_ONSTACK_EMU) != 0; }
    bool disabled() const { return (flags & SS_DISABLE_EMU) != 0 || size == 0; }
    uint64_t top() const { return sp + size; }
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
    // ── Per-CPU signal mask & altstack ──────────────────────────────
    // The signal mask and altstack are per-CPU state (in CPU::sigmask
    // and CPU::altstack). These helpers take a CPU& and operate on the
    // per-CPU state.
    //
    // Bit numbering: matches the Linux kernel ABI — bit `signo-1` (1-based
    // signal numbers). So SIGUSR1 (signo=10) is bit 9 in the sigset,
    // SIGKILL (signo=9) is bit 8, etc. This matches what
    // rt_sigprocmask/rt_sigpending read/write in guest memory.
    static bool is_blocked(const CPU& cpu, int signo) {
        if (signo < 1 || signo > 63) return false;
        return (cpu.sigmask >> (signo - 1)) & 1;
    }
    // Apply a rt_sigprocmask `how` operation to `cpu.sigmask`. Returns 0
    // on success, -EINVAL for an invalid `how`. If `old_set_addr` is
    // non-zero, writes the previous mask there.
    static int procmask(Memory& mem, CPU& cpu, int how, uint64_t new_set_addr,
                        uint64_t old_set_addr, size_t sigsetsize);
    // Set/query `cpu.altstack`. Returns 0 on success, -errno on failure.
    static int set_altstack(Memory& mem, CPU& cpu, uint64_t new_addr, uint64_t old_addr);
    static void set_altstack_active(CPU& cpu, bool active) {
        if (active) cpu.altstack.flags |= CPU::AltStack::SS_ONSTACK_EMU;
        else        cpu.altstack.flags &= ~CPU::AltStack::SS_ONSTACK_EMU;
    }
    // Clear a handler (SA_RESETHAND one-shot behavior).
    void clear_handler(int signo) {
        if (signo >= 1 && signo <= MAX_SIGNAL) {
            actions_[signo] = SigAction{};
        }
    }
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
// Build a guest-side siginfo_t at address `info_addr` for `signo` with
// the supplied si_code and fault address. The layout matches the AArch64
// struct siginfo_t (128 bytes): si_signo, si_errno, si_code, then a
// union containing si_addr/si_pid/si_uid/etc.
void build_siginfo(Memory& mem, uint64_t info_addr, int signo,
                   int si_code, uint64_t fault_addr);
// Build a guest-side ucontext_t at address `uc_addr`. The layout is
// simplified but covers the fields real handlers (glibc/musl/bionic)
// actually read:
//   uc_flags     (8 bytes) — UC_FP_ALL etc., we set 0
//   uc_link      (8 bytes) — 0
//   uc_stack     (24 bytes) — ss_sp, ss_flags, ss_size (altstack state)
//   uc_sigmask   (8 bytes) — saved mask (current mask before signal)
//   uc_mcontext  — see below
//   uc_mcontext.regs[31] (248 bytes)
//   uc_mcontext.sp   (8 bytes)
//   uc_mcontext.pc   (8 bytes)
//   uc_mcontext.pstate (8 bytes)
//   uc_mcontext.fault_address (8 bytes) — for SIGSEGV
//
// Returns the address just past the ucontext_t (for the caller to use
// as the next signal frame's stack pointer if needed).
uint64_t build_ucontext(Memory& mem, uint64_t uc_addr, CPU& cpu,
                        uint64_t saved_mask, uint64_t fault_addr);
// Deliver a signal to the guest. If a handler is installed, saves
// the current CPU state, sets up X0=signo / X1=siginfo / X2=ucontext /
// X30=trampoline / PC=handler, and returns true. If no handler is
// installed (SIG_DFL), applies the default disposition:
//   - SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGSYS → terminate
//     the guest with exit code 128 + signo (matching shell convention).
//   - SIGINT, SIGTERM, SIGHUP, SIGQUIT → terminate with 128 + signo.
//   - SIGPIPE, SIGCHLD, SIGURG, SIGWINCH, SIGCONT → ignored by default.
//   - Other signals → terminate with 128 + signo (safe default).
//
// `fault_addr` is the address that caused the fault (for SIGSEGV/SIGBUS),
// or 0 for signals raised by kill/raise. `si_code` is one of the
// SI_* / SEGV_* / ILL_* / FPE_* constants above.
//
// Returns true if the signal was delivered (handler invoked), false
// if the default disposition was applied.
bool deliver_signal(Emulator& emu, CPU& cpu, SignalTable& sigtab, int signo,
                    int si_code = SI_USER_EMU, uint64_t fault_addr = 0);
// Deliver any pending signals that are no longer blocked. Called after
// rt_sigprocmask unblocks signals. Returns the number of signals delivered.
int deliver_pending_signals(Emulator& emu, CPU& cpu, SignalTable& sigtab);
} // namespace arm64emu
