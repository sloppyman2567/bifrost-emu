// signal.cpp — Guest signal delivery for bifrost-emu.
//
// Implements the SignalTable and signal delivery logic described in
// signal.hpp. The trampoline is mapped into the guest's address space
// at a fixed address (TRAMPOLINE_ADDR) and contains just two
// instructions: mov x8, #139 (rt_sigreturn) and svc #0.
//
// When a signal is delivered, we:
//   1. Save the full CPU state (X0..X30, SP, PC, PSTATE) plus the
//      current signal mask and fault info into a SignalFrame on our
//      internal stack.
//   2. Build a guest-side siginfo_t and ucontext_t on the guest stack
//      (or alternate stack if SA_ONSTACK is set and sigaltstack is
//      configured). This lets guest handlers that read ucontext_t
//      (e.g., to inspect/modify the saved PC) work correctly.
//   3. Add the signal's sa_mask + the signo itself to the current
//      signal mask (so the handler is not preempted by itself unless
//      SA_NODEFER is set).
//   4. Set X0 = signo, X1 = siginfo_t ptr, X2 = ucontext_t ptr,
//      X30 = TRAMPOLINE_ADDR, PC = handler.
//   5. The guest runs the handler. When it returns (RET via X30), it
//      lands at the trampoline, which does svc #0 with x8=139.
//   6. The rt_sigreturn syscall handler (case 139 in misc.cpp) pops the
//      top SignalFrame, restores CPU state, and restores the saved
//      signal mask.
//
// Also adds host-to-guest signal forwarding: host signals
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
    // SIGKILL (9) and SIGSTOP (19) cannot be caught — return EINVAL
    // to match Linux behavior. (Some kernels return 0 and silently
    // ignore; we choose the stricter behavior so guest code can detect
    // the error.)
    if (signo == BIFROST_SIGKILL || signo == BIFROST_SIGSTOP) {
        return -EINVAL;
    }

    // Save the old action if requested.
    if (old_act_addr != 0) {
        uint8_t buf[KSIGACTION_SIZE] = {0};
        SigAction& old = actions_[signo];
        memcpy(buf + 0,  &old.handler, 8);
        memcpy(buf + 8,  &old.flags,   8);
        // BUGFIX: the old code wrote `old.mask` to BOTH offset 16 (sa_restorer,
        // which is unused on AArch64) AND offset 24 (sa_mask). That was
        // sloppy — guest code reading sa_restorer would see a garbage
        // pointer (the sa_mask value). Per the AArch64 struct k_sigaction
        // layout, offset 16 is sa_restorer (always 0 on AArch64 — there is
        // no restorer; the trampoline is provided by the kernel at
        // TRAMPOLINE_ADDR) and offset 24 is sa_mask. Write 0 to offset 16
        // and the actual mask to offset 24.
        uint64_t zero_restorer = 0;
        memcpy(buf + 16, &zero_restorer, 8);
        memcpy(buf + 24, &old.mask,      8);
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
        // Mask out reserved/unsupported bits to keep internal state sane.
        static constexpr uint64_t SA_SUPPORTED_FLAGS =
            SA_SIGINFO_EMU | SA_RESTART_EMU | SA_NODEFER_EMU |
            SA_RESETHAND_EMU | SA_ONSTACK_EMU | SA_NOCLDWAIT_EMU |
            SA_NOCLDSTOP_EMU;
        na.flags &= SA_SUPPORTED_FLAGS;
        na.installed = true;

        // SA_RESETHAND: clear the handler after first delivery. We
        // record the flag; deliver_signal() performs the reset.
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

// ── Signal mask (rt_sigprocmask) ───────────────────────────────────────
// BUGFIX: this is now a static method that operates on `cpu.sigmask`
// instead of a shared `mask_` member. This fixes multi-threaded signal
// handling — each vCPU has its own mask.
int SignalTable::procmask(Memory& mem, CPU& cpu, int how, uint64_t new_set_addr,
                          uint64_t old_set_addr, size_t sigsetsize) {
    // Linux allows sigsetsize up to 8 bytes for our 64-bit mask.
    if (sigsetsize != 8 && sigsetsize != 4) return -EINVAL;

    // Save the old mask first.
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

    if (new_set_addr == 0) return 0;  // only query

    uint64_t new_mask = 0;
    try {
        if (sigsetsize == 8) {
            new_mask = mem.load<uint64_t>(new_set_addr);
        } else {
            new_mask = mem.load<uint32_t>(new_set_addr);
        }
    } catch (...) {
        return -EFAULT;
    }

    // SIGKILL (9) and SIGSTOP (19) cannot be blocked.
    new_mask &= ~((1ULL << 9) | (1ULL << 19));

    switch (how) {
        case SIG_BLOCK_EMU:
            cpu.sigmask |= new_mask;
            break;
        case SIG_UNBLOCK_EMU:
            cpu.sigmask &= ~new_mask;
            break;
        case SIG_SETMASK_EMU:
            cpu.sigmask = new_mask;
            break;
        default:
            return -EINVAL;
    }
    return 0;
}

// ── sigaltstack ────────────────────────────────────────────────────────
// BUGFIX: this is now a static method that operates on `cpu.altstack`
// instead of a shared `altstack_` member.
int SignalTable::set_altstack(Memory& mem, CPU& cpu, uint64_t new_addr, uint64_t old_addr) {
    // struct sigaltstack { void *ss_sp; int ss_flags; size_t ss_size; }
    // AArch64 layout: ss_sp at 0, ss_flags at 8 (with 4-byte padding),
    // ss_size at 16. Total 24 bytes.
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

    if (new_addr != 0) {
        uint8_t buf[SS_SIZE] = {0};
        try {
            mem.read(new_addr, buf, SS_SIZE);
        } catch (...) {
            return -EFAULT;
        }
        uint64_t new_sp;
        uint32_t new_flags;
        uint64_t new_size;
        memcpy(&new_sp,    buf + 0,  8);
        memcpy(&new_flags, buf + 8,  4);
        memcpy(&new_size,  buf + 16, 8);

        // Cannot set SS_ONSTACK via sigaltstack (the kernel sets/clears
        // it; user code only sets SS_DISABLE).
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
    }
    return 0;
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
    // Use the Memory object's own state to detect whether the trampoline
    // page is already mapped, instead of a process-lifetime static bool.
    // This is fork-safe: after fork(), the child has its own Memory, and
    // is_mapped() will correctly report whether the trampoline is present
    // in the child's address space.
    if (mem.is_mapped(TRAMPOLINE_ADDR, 8)) return TRAMPOLINE_ADDR;

    // Map a page at TRAMPOLINE_ADDR.
    mem.map_range(TRAMPOLINE_ADDR, 4096);

    // Write the two instructions.
    const uint32_t code[2] = { 0xD2801168u, 0xD4000001u };
    mem.write(TRAMPOLINE_ADDR, code, sizeof(code));

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

static bool default_dumps_core(int signo) {
    switch (signo) {
        case BIFROST_SIGQUIT: case BIFROST_SIGILL: case BIFROST_SIGABRT:
        case BIFROST_SIGBUS: case BIFROST_SIGFPE: case BIFROST_SIGSEGV:
        case BIFROST_SIGSYS: case BIFROST_SIGTRAP: case BIFROST_SIGXCPU:
        case BIFROST_SIGXFSZ:
            return true;
        default:
            return false;
    }
}

// ── siginfo_t and ucontext_t construction ──────────────────────────────
// AArch64 struct siginfo_t layout (128 bytes), per
// include/uapi/asm-generic/siginfo.h:
//   int si_signo   at 0
//   int si_errno   at 4
//   int si_code    at 8
//   (4 bytes padding)  at 12  (to align union to 8 bytes)
//   union __sifields at 16:
//     _kill:    pid_t _pid (4) + uid_t _uid (4)        → at 16, 20
//     _sigfault: void *_addr (8)                       → at 16
//     _sigchld: pid_t (4) + uid_t (4) + int status (4)
//               + clock_t utime (8) + clock_t stime (8) → at 16..40
//   rest is zero-padded to 128 bytes.
void build_siginfo(Memory& mem, uint64_t info_addr, int signo,
                   int si_code, uint64_t fault_addr) {
    if (info_addr == 0) return;
    uint8_t buf[128] = {0};
    int32_t signo32 = signo;
    int32_t code32 = si_code;
    int32_t errno32 = 0;
    memcpy(buf + 0,  &signo32, 4);
    memcpy(buf + 4,  &errno32, 4);
    memcpy(buf + 8,  &code32,  4);
    // For SI_USER/SI_KERNEL, the _kill union: si_pid at 16, si_uid at 20.
    if (si_code == SI_USER_EMU || si_code == SI_KERNEL_EMU) {
        int32_t pid = 1;  // main thread tid
        int32_t uid = 0;
        memcpy(buf + 16, &pid, 4);
        memcpy(buf + 20, &uid, 4);
    }
    // For SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGTRAP, the _sigfault union
    // has _addr (the faulting address) as its first member at offset 16.
    if (signo == BIFROST_SIGSEGV || signo == BIFROST_SIGBUS ||
        signo == BIFROST_SIGILL || signo == BIFROST_SIGFPE ||
        signo == BIFROST_SIGTRAP) {
        memcpy(buf + 16, &fault_addr, 8);
    }
    try { mem.write(info_addr, buf, sizeof(buf)); } catch (...) {}
}

// AArch64 struct ucontext_t per arch/arm64/include/uapi/asm/ucontext.h:
//   unsigned long  uc_flags       (8)  offset 0
//   struct ucontext *uc_link      (8)  offset 8
//   stack_t uc_stack              (24) offset 16
//       void *ss_sp                   offset 16
//       int   ss_flags                offset 24  (4 bytes + 4 padding)
//       size_t ss_size                offset 32
//   sigset_t uc_sigmask           (8)  offset 40
//   __u8 __unused[1024/8 - 8]     (120) offset 48   // pad to 1024 bits
//   struct sigcontext uc_mcontext      offset 168
//       __u64 fault_address        (8) offset 168
//       __u64 regs[31]           (248) offset 176
//       __u64 sp                   (8) offset 424
//       __u64 pc                   (8) offset 432
//       __u64 pstate               (8) offset 440
//       __u8 __reserved[4096]   (4096) offset 448   // fpsimd context
// Total meaningful size written: 448 + 528 = 976 bytes (sigcontext +
// fpsimd_context). The 4 KiB reserved area's first 528 bytes hold the
// fpsimd_context; the rest is zero-padded.
//
// BUGFIX: the old code skipped the 4 KiB reserved area entirely, so
// FP/SIMD state was NOT preserved across signal handlers. Handlers
// using NEON (crypto, DSP, image processing) would see corrupted V
// registers. Now we write a proper fpsimd_context at offset 448.
//
// fpsimd_context layout (arch/arm64/include/uapi/asm/sigcontext.h):
//   struct _aarch64_ctx head     (8)   offset 0
//       __u32 magic                  // FPSIMD_MAGIC = 0x46508001
//       __u32 size                   // sizeof(fpsimd_context) = 528
//   __u64 fpsr                   (8)   offset 8
//   __u64 fpcr                   (8)   offset 16
//   __uint128_t vregs[32]        (512) offset 24  (32 * 16 bytes)
// Total: 8 + 8 + 8 + 512 = 536 bytes? The kernel header says
// sizeof(struct fpsimd_context) = 528, which means the vregs array is
// 512 bytes and the header (magic+size) is 8 bytes, fpsr is 4 bytes,
// fpcr is 4 bytes — totaling 8+4+4+512 = 528. We use the kernel's
// 64-bit fpsr/fpcr fields for simplicity (the high 32 bits are
// zero-padded; the kernel does the same on AArch64).
uint64_t build_ucontext(Memory& mem, uint64_t uc_addr, CPU& cpu,
                        uint64_t saved_mask, uint64_t fault_addr) {
    if (uc_addr == 0) return 0;
    // BUGFIX: bumped from 448 to 976 to include the fpsimd_context.
    constexpr size_t UCONTEXT_SIZE = 976;
    uint8_t buf[UCONTEXT_SIZE] = {0};
    // uc_flags = 0 (no UC_FP_ALL etc.)
    // uc_link = 0
    // uc_stack: zeroed (guest can read its own sigaltstack).
    // uc_sigmask:
    memcpy(buf + 40, &saved_mask, 8);
    // uc_mcontext.fault_address:
    memcpy(buf + 168, &fault_addr, 8);
    // uc_mcontext.regs[0..30]:
    memcpy(buf + 176, cpu.regs, sizeof(cpu.regs));  // 31 * 8 = 248 bytes
    // uc_mcontext.sp/pc/pstate:
    memcpy(buf + 424, &cpu.sp, 8);
    memcpy(buf + 432, &cpu.pc, 8);
    memcpy(buf + 440, &cpu.pstate, 8);
    // fpsimd_context at offset 448:
    // Per arch/arm64/include/uapi/asm/sigcontext.h:
    //   struct _aarch64_ctx head  (magic:4 + size:4 = 8 bytes) at +0
    //   __u32 fpsr              (4 bytes) at +8
    //   __u32 fpcr              (4 bytes) at +12
    //   __uint128_t vregs[32]   (512 bytes) at +16
    // Total: 8 + 4 + 4 + 512 = 528 bytes.
    constexpr uint32_t FPSIMD_MAGIC = 0x46508001;
    constexpr uint32_t FPSIMD_SIZE  = 528;
    memcpy(buf + 448 + 0, &FPSIMD_MAGIC, 4);
    memcpy(buf + 448 + 4, &FPSIMD_SIZE,  4);
    // fpsr (4 bytes):
    memcpy(buf + 448 + 8, &cpu.fpsr, 4);
    // fpcr (4 bytes):
    memcpy(buf + 448 + 12, &cpu.fpcr, 4);
    // vregs[32] — 32 * 16 bytes = 512 bytes. Each V reg is stored as
    // (v_lo, v_hi) = 16 bytes. We interleave them.
    for (int i = 0; i < 32; i++) {
        memcpy(buf + 448 + 16 + i * 16,     &cpu.v_lo[i], 8);
        memcpy(buf + 448 + 16 + i * 16 + 8, &cpu.v_hi[i], 8);
    }
    try { mem.write(uc_addr, buf, sizeof(buf)); } catch (...) {}
    return uc_addr + sizeof(buf);
}

// ── deliver_signal ──────────────────────────────────────────────────────
bool deliver_signal(Emulator& emu, CPU& cpu, SignalTable& sigtab, int signo,
                    int si_code, uint64_t fault_addr) {
    if (signo < 1 || signo > MAX_SIGNAL) return false;

    // Blocked signals are not delivered (except SIGKILL/SIGSTOP, which
    // we don't block — handled in procmask). Pending blocked signals
    // are dropped in this simplified model (no pending queue).
    // BUGFIX: read the mask from the per-CPU state, not the shared
    // SignalTable mask. Each vCPU has its own mask now.
    if (SignalTable::is_blocked(cpu, signo) &&
        signo != BIFROST_SIGKILL && signo != BIFROST_SIGSTOP) {
        return false;
    }

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

    // A real handler is installed. Save CPU state and set up the call.
    // Make sure the trampoline is mapped.
    uint64_t tramp = map_sigreturn_trampoline(emu.mem());
    if (tramp == 0) {
        if (default_terminates(signo)) {
            cpu.running = false;
            cpu.exit_code = 128 + signo;
        }
        return false;
    }

    // Pick the stack: altstack if SA_ONSTACK is set and the altstack
    // is configured and not already in use. BUGFIX: read altstack from
    // the per-CPU state.
    uint64_t target_sp = cpu.sp;
    bool on_altstack = false;
    if ((act->flags & SA_ONSTACK_EMU) && !cpu.altstack.disabled() &&
        !cpu.altstack.active()) {
        // Align down to 16 bytes and reserve enough space for siginfo +
        // ucontext (now 976 bytes with fpsimd_context) + handler frame.
        // Reserve 1280 bytes (siginfo 128 + ucontext 976 + 176 slack).
        target_sp = cpu.altstack.top() - 1280;
        target_sp &= ~0xFULL;
        on_altstack = true;
    }

    // Build siginfo_t and ucontext_t on the guest stack (below
    // target_sp). Reserve 128 (siginfo) + 976 (ucontext with fpsimd)
    // = 1104 bytes, rounded up to 1280 for alignment + handler frame.
    constexpr size_t SIGINFO_SIZE = 128;
    constexpr size_t FRAME_RESERVE = 1280;
    uint64_t info_addr = (target_sp - FRAME_RESERVE) & ~0xFULL;
    uint64_t uc_addr   = info_addr + SIGINFO_SIZE;
    uint64_t new_sp    = info_addr;

    // Save current mask so rt_sigreturn can restore it. BUGFIX: read
    // from per-CPU state.
    uint64_t saved_mask = cpu.sigmask;

    // Build the guest-visible structures.
    build_siginfo(emu.mem(), info_addr, signo, si_code, fault_addr);
    build_ucontext(emu.mem(), uc_addr, cpu, saved_mask, fault_addr);

    // Save the CPU state in our internal frame (for rt_sigreturn).
    // BUGFIX: now also saves FP/SIMD state (v_lo, v_hi, fpcr, fpsr)
    // so handlers using NEON don't corrupt the saved state.
    SignalFrame& frame = sigtab.push_frame(signo);
    memcpy(frame.regs, cpu.regs, sizeof(frame.regs));
    frame.sp         = cpu.sp;
    frame.pc         = cpu.pc;
    frame.pstate     = cpu.pstate;
    frame.saved_mask = saved_mask;
    frame.fault_addr = fault_addr;
    frame.si_code    = si_code;
    frame.on_altstack = on_altstack;
    memcpy(frame.v_lo, cpu.v_lo, sizeof(frame.v_lo));
    memcpy(frame.v_hi, cpu.v_hi, sizeof(frame.v_hi));
    frame.fpcr = cpu.fpcr;
    frame.fpsr = cpu.fpsr;

    // Mark altstack as in-use if we used it. BUGFIX: update per-CPU state.
    if (on_altstack) {
        SignalTable::set_altstack_active(cpu, true);
    }

    // Compute new mask: current mask | sa_mask | signo (unless NODEFER).
    uint64_t new_mask = saved_mask | act->mask;
    if (!(act->flags & SA_NODEFER_EMU)) {
        new_mask |= (1ULL << signo);
    }
    // SIGKILL/SIGSTOP cannot be blocked.
    new_mask &= ~((1ULL << 9) | (1ULL << 19));
    // BUGFIX: write to per-CPU state.
    cpu.sigmask = new_mask;

    // SA_RESETHAND: clear the handler after delivery (one-shot).
    if (act->flags & SA_RESETHAND_EMU) {
        sigtab.clear_handler(signo);
    }

    // Set up the handler call:
    //   X0=signo, X1=siginfo_t*, X2=ucontext_t*, X30=trampoline,
    //   PC=handler, SP=new_sp.
    cpu.regs[0]  = static_cast<uint64_t>(signo);
    // For SA_SIGINFO, X1=siginfo, X2=ucontext. For old-style handlers
    // (no SA_SIGINFO), Linux still passes both — the handler just
    // ignores X1/X2. We always set them.
    cpu.regs[1]  = info_addr;
    cpu.regs[2]  = uc_addr;
    cpu.regs[30] = tramp;
    cpu.pc       = act->handler;
    cpu.sp       = new_sp;

    return true;
}

// ── Host-to-guest signal forwarding ──────────────────

void Emulator::host_signal_handler(int signo) {
    // Called from the host kernel when a signal is delivered to the
    // emulator process. We can't call deliver_signal() from here (not
    // async-signal-safe — we'd need to acquire mutexes and touch guest
    // memory). Instead, just enqueue the signal number in a lock-free
    // SPSC ring; the run loop will drain the queue between instructions.
    // BUGFIX: the old code called queue_host_signal which used
    // std::mutex::lock — NOT async-signal-safe, UB on contention.
    // The new ring buffer uses only std::atomic operations, which ARE
    // async-signal-safe on POSIX platforms.
    if (g_active_emu_) {
        g_active_emu_->queue_host_signal(signo);
    }
}

void Emulator::queue_host_signal(int signo) {
    // Lock-free SPSC enqueue. The host signal handler is the sole
    // producer; the run loop (drain_host_signals) is the sole consumer.
    // We use memory_order_relaxed for the head read (we don't need to
    // see the consumer's latest progress — if the queue is full, we
    // drop the signal, which is acceptable per POSIX signal semantics)
    // and memory_order_release for the tail write (so the consumer
    // sees the written slot when it observes the new tail).
    size_t t = host_signal_queue_.tail.load(std::memory_order_relaxed);
    size_t h = host_signal_queue_.head.load(std::memory_order_relaxed);
    size_t used = t - h;  // works with wraparound since size_t is unsigned
    if (used >= HOST_SIGNAL_QUEUE_CAP) {
        // Queue full — drop the signal. This matches the old "drop on
        // contention" behavior, but without the UB. POSIX allows signal
        // loss when the queue is full.
        return;
    }
    host_signal_queue_.signals[t % HOST_SIGNAL_QUEUE_CAP] = signo;
    host_signal_queue_.tail.store(t + 1, std::memory_order_release);
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
    // Lock-free SPSC dequeue. We atomically swap the head index forward
    // and process signals in order. memory_order_acquire on the tail
    // ensures we see the producer's writes; memory_order_relaxed on head
    // updates is fine because we're the sole consumer.
    bool any_delivered = false;
    size_t h = host_signal_queue_.head.load(std::memory_order_relaxed);
    size_t t = host_signal_queue_.tail.load(std::memory_order_acquire);
    while (h != t) {
        int sig = host_signal_queue_.signals[h % HOST_SIGNAL_QUEUE_CAP];
        host_signal_queue_.head.store(h + 1, std::memory_order_relaxed);
        h = h + 1;

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
        // Re-read tail in case the producer added more signals while
        // we were delivering one.
        t = host_signal_queue_.tail.load(std::memory_order_acquire);
    }
    return any_delivered;
}

} // namespace arm64emu
