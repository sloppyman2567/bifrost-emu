// core/cpu.h — ARM64 register file + PSTATE + per-vCPU state.
//
// A CPU instance represents one virtual CPU (vCPU). The Emulator owns one
// main_cpu_ plus N cloned vCPUs (one per guest thread spawned via clone()).
//
// The CPU owns:
//   - The 31 general-purpose registers (X0..X30) + SP + PC + PSTATE
//   - The 32 SIMD/FP registers (V0..V31), stored as low/high 64-bit halves
//   - Per-vCPU decode cache (2-way set-associative, ~1.5 MiB)
//   - Per-vCPU page cache (for the Memory hot path)
//   - Local exclusive monitor state (for LDXR/STXR atomics)
//   - Thread-local storage pointers (TPIDR_EL0, TPIDRRO_EL0)
//   - Clear-child-tid pointer (for clone(CLONE_CHILD_CLEARTID) + futex)
#pragma once

#include "bifrost/types.hpp"
#include "core/memory.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace arm64emu {

class CPU {
public:
    // We store 32 entries; regs[31] is always 0 (XZR). This lets us index
    // safely without special-casing source operands. For destination
    // operands, callers must still check rd != 31 before storing, OR rely
    // on the fact that writing 0 to regs[31] is harmless (but it would
    // corrupt the XZR semantics if we then read it back expecting 0).
    // We treat writes to regs[31] as no-ops at the call sites.
    uint64_t regs[32] = {0};
    uint64_t sp = 0;
    uint64_t pc = 0;
    uint32_t pstate = 0;        // bits: 31=N, 30=Z, 29=C, 28=V (lowest 4 of NZCV)
    bool     running = true;
    int      exit_code = 0;

    // SIMD/FP register file. Each Vn is 128 bits (16 bytes). We store as
    // two 64-bit halves (low and high). FP scalar ops use the low bits.
    uint64_t v_lo[32] = {0};    // bits 63:0 of each V register
    uint64_t v_hi[32] = {0};    // bits 127:64 of each V register
    uint32_t fpcr = 0;
    uint32_t fpsr = 0;

    // Thread-local storage pointers. glibc's __libc_setup_tls sets
    // TPIDR_EL0 via MSR to point to the TCB (Thread Control Block).
    // All TLS variable access is relative to this register.
    // TPIDRRO_EL0 is the read-only variant (usually same as TPIDR_EL0
    // for the main thread).
    uint64_t tpidr_el0   = 0;
    uint64_t tpidrro_el0 = 0;

    // Thread ID (guest TID). Main thread is 1; cloned threads get 2, 3, ...
    // Forked children (clone without CLONE_VM) get the host PID as their
    // tid, and is_fork_process is set so getpid() returns the host PID
    // instead of 1. This lets the child detect it's a forked process
    // (getpid() != parent_pid) and reset signal handlers accordingly.
    int tid = 1;
    bool is_fork_process = false;  // true for fork() children (no CLONE_VM)

    // Per-CPU memory page cache for the hot path. Avoids mutex+hash on
    // every memory access to the same page. Each CPU (thread) has its
    // own cache, so no locking needed.
    Memory::PageCache page_cache;

    // ── Per-CPU decode cache ───────────────────────────────────────
    // Lives in CPU (not Emulator) so each vCPU has its own cache with
    // no locking. 2-way set-associative: 8192 sets × 2 ways = 16384
    // entries (~1.5 MiB per vCPU). This is the hot path — ~100% hit
    // rate for tight loops.
    //
    // upgraded from direct-mapped to 2-way
    // set-associative to reduce conflict misses.
    static constexpr size_t DECODE_CACHE_BITS = 13;   // 8192 sets
    static constexpr size_t DECODE_CACHE_WAYS = 2;
    static constexpr size_t DECODE_CACHE_SIZE = (1 << DECODE_CACHE_BITS) * DECODE_CACHE_WAYS;
    static constexpr size_t DECODE_CACHE_SETS = 1 << DECODE_CACHE_BITS;
    static constexpr size_t DECODE_CACHE_SET_MASK = DECODE_CACHE_SETS - 1;
    struct CacheEntry {
        uint64_t    tag = UINT64_MAX;  // PC; UINT64_MAX = empty slot
        DecodedInst d;
    };
    std::vector<CacheEntry> decode_cache{DECODE_CACHE_SIZE};
    uint64_t decode_cache_hits = 0;
    uint64_t decode_cache_misses = 0;
    // LRU bit per set: 0 = way 0 most-recently-used, 1 = way 1 MRU.
    std::vector<uint8_t> decode_cache_lru =
        std::vector<uint8_t>((DECODE_CACHE_SETS + 7) / 8, 0);

    // Address set via set_tid_address() — used by futex on child
    // termination (set_child_tid). When this thread exits, the kernel
    // writes the TID to *tid_address and performs a futex wake on it.
    // The exit-time write+wake is implemented in thread_entry.
    uint64_t set_tid_address_ptr = 0;

    // ── Robust futex list ───────────────────────────────────────────
    // set_robust_list(head, len) records the head of a linked list of
    // robust futexes held by this thread. When the thread exits (or is
    // killed), the kernel walks the list and unlocks each futex with
    // FUTEX_OWNER_DIED. This is how pthread_mutex with
    // PTHREAD_MUTEX_ROBUST behaves when a thread dies holding the lock.
    //
    // Each list entry is a struct robust_list_head + per-futex nodes:
    //   struct robust_list { struct robust_list *next; };
    //   struct robust_list_head {
    //       struct robust_list list;
    //       long futex_offset;
    //       struct robust_list __user *pending_list;
    //   };
    // The futex word is at (node + futex_offset). We walk via `next`
    // until we hit the head again (circular list) or a limit (to avoid
    // infinite loops on corrupt lists).
    uint64_t robust_list_head = 0;   // guest VA of robust_list_head.list
    uint64_t robust_list_len  = 0;   // len passed to set_robust_list (sanity)

    // Clear-child-tid pointer set via clone(CLONE_CHILD_CLEARTID, ...).
    // When this thread exits, the word at this address is zeroed and a
    // futex wake is performed on it. Required for pthread_join to work.
    uint64_t clear_child_tid = 0;

    // ── Per-CPU signal state ────────────────────────────────────────
    // The signal mask and altstack live in the CPU so each vCPU has its
    // own state (set by rt_sigprocmask/sigaltstack, read by
    // deliver_signal). Bit numbering is 1-based: bit `signo-1` is set
    // when `signo` is blocked/pending — this matches the Linux kernel
    // sigset_t layout that rt_sigprocmask/rt_sigpending read/write.
    uint64_t sigmask = 0;       // blocked-signal bitmask (bit `signo-1` set = blocked)
    uint64_t sigpending = 0;    // pending-signal bitmask (bit `signo-1` set = pending)

    // ── Per-CPU pending-signal queue ─────────────────────────────────
    // BUGFIX (Turn 57): tgkill/tkill/kill targeting another thread used
    // to call deliver_signal() directly on the target CPU while the
    // target's host thread was concurrently executing on it — a textbook
    // data race (regs/pc/sp/sigmask/sigpending mutated under the target's
    // feet). Now cross-thread signal delivery pushes (signo, si_code,
    // fault_addr) onto the target's pending queue under a mutex, and the
    // target's run loop drains its own queue at the next 4K-instruction
    // boundary (the existing drain_host_signals() call point). This
    // matches the kernel's per-task task->pending queue semantics.
    //
    // The queue is small (64 entries) — kernel task->pending is also
    // bounded (default 32, max _NSIG_WORDS*8). If overflow happens we
    // coalesce by setting the bit in `sigpending` so the signal is at
    // least not lost (the bit-based pending delivery path still works).
    struct PendingSig {
        int      signo;
        int      si_code;
        uint64_t fault_addr;
    };
    static constexpr size_t PENDING_QUEUE_CAP = 64;
    std::mutex              pending_mu;
    PendingSig              pending_queue[PENDING_QUEUE_CAP];
    std::atomic<size_t>     pending_head{0};  // consumer index
    std::atomic<size_t>     pending_tail{0};  // producer index
    // Producer (other thread delivering signal) — returns true if queued,
    // false if the queue is full (caller should fall back to setting the
    // bit in sigpending so the signal isn't completely lost).
    bool push_pending(int signo, int si_code, uint64_t fault_addr) {
        const size_t t = pending_tail.load(std::memory_order_relaxed);
        const size_t h = pending_head.load(std::memory_order_acquire);
        const size_t used = t - h;  // wraparound-safe
        if (used >= PENDING_QUEUE_CAP) return false;
        pending_queue[t % PENDING_QUEUE_CAP] = PendingSig{signo, si_code, fault_addr};
        pending_tail.store(t + 1, std::memory_order_release);
        return true;
    }
    // Consumer (this CPU's run loop) — pops one pending signal, or
    // returns false if the queue is empty.
    bool pop_pending(PendingSig& out) {
        const size_t h = pending_head.load(std::memory_order_relaxed);
        const size_t t = pending_tail.load(std::memory_order_acquire);
        if (h == t) return false;
        out = pending_queue[h % PENDING_QUEUE_CAP];
        pending_head.store(h + 1, std::memory_order_release);
        return true;
    }
    bool has_pending_signals() const {
        return pending_head.load(std::memory_order_acquire) !=
               pending_tail.load(std::memory_order_acquire);
    }

    // Set when SIGINT was received from the terminal (Ctrl+C) but the
    // guest has SIGINT set to SIG_IGN. The read() handler checks this
    // flag and injects a newline byte so the shell prints a new prompt
    // (mimicking bash/dash behavior). Without this, Ctrl+C at an empty
    // prompt would do nothing visible — the signal is silently dropped
    // and the shell stays blocked on read().
    bool sigint_ignored = false;
    struct AltStack {
        uint64_t sp    = 0;     // base address
        uint64_t size  = 0;     // size in bytes
        uint32_t flags = 0;     // SS_ONSTACK / SS_DISABLE
        static constexpr uint32_t SS_ONSTACK_EMU  = 1;
        static constexpr uint32_t SS_DISABLE_EMU  = 2;
        bool active() const { return (flags & SS_ONSTACK_EMU) != 0; }
        bool disabled() const { return (flags & SS_DISABLE_EMU) != 0 || size == 0; }
        uint64_t top() const { return sp + size; }
    } altstack;

    // ── Local Exclusive Monitor ─────────────────────────────────────
    // AArch64 LL/SC atomics use an "exclusive monitor" — a single-entry
    // hardware tag that records the address of the most recent LDXR/LDAXR.
    // STXR succeeds only if the monitor is still tagged for that address,
    // and clears the tag. Any other memory access, branch, or exception
    // also clears the tag.
    //
    // Per-thread monitor state (each thread has its own, like real HW).
    bool     excl_tag_valid = false;
    uint64_t excl_tag_addr  = 0;
    uint32_t excl_tag_size  = 0;

    void excl_mark(uint64_t addr, uint32_t size) {
        excl_tag_valid = true;
        excl_tag_addr  = addr;
        excl_tag_size  = size;
    }
    void excl_clear() { excl_tag_valid = false; }
    bool excl_check(uint64_t addr, uint32_t size) const {
        if (!excl_tag_valid) return false;
        uint64_t a_lo = excl_tag_addr;
        uint64_t a_hi = excl_tag_addr + excl_tag_size;
        uint64_t b_lo = addr;
        uint64_t b_hi = addr + size;
        return (a_lo < b_hi) && (b_lo < a_hi);
    }

    // ── PSTATE flag accessors ────────────────────────────────────────
    void set_flag_n(bool v) { if (v) pstate |= (1u<<31); else pstate &= ~(1u<<31); }
    void set_flag_z(bool v) { if (v) pstate |= (1u<<30); else pstate &= ~(1u<<30); }
    void set_flag_c(bool v) { if (v) pstate |= (1u<<29); else pstate &= ~(1u<<29); }
    void set_flag_v(bool v) { if (v) pstate |= (1u<<28); else pstate &= ~(1u<<28); }
    bool flag_n() const { return pstate & (1u<<31); }
    bool flag_z() const { return pstate & (1u<<30); }
    bool flag_c() const { return pstate & (1u<<29); }
    bool flag_v() const { return pstate & (1u<<28); }

    // Read a register operand; XZR (31) reads as 0.
    uint64_t r(int r) const { return regs[r & 31]; }
    // Write a register operand; writes to XZR (31) are discarded.
    void w(int r, uint64_t v) { if (r != 31) regs[r] = v; }

    // ── Architectural-state copy ─────────────────────────────────────
    // BUGFIX (Turn 57): CPU is non-copyable because the per-CPU pending
    // signal queue has mutex + atomic members. clone() and the ifunc
    // resolver need to copy/restore the architectural state (everything
    // except the pending queue, which is per-thread and shouldn't be
    // inherited anyway). copy_arch_state_from() copies all architectural
    // fields; the caller is responsible for resetting the pending queue
    // and exclusive monitor as needed (see thread_mgr.cpp spawn_thread).
    void copy_arch_state_from(const CPU& src) {
        std::memcpy(regs, src.regs, sizeof(regs));
        sp = src.sp;
        pc = src.pc;
        pstate = src.pstate;
        running = src.running;
        exit_code = src.exit_code;
        std::memcpy(v_lo, src.v_lo, sizeof(v_lo));
        std::memcpy(v_hi, src.v_hi, sizeof(v_hi));
        fpcr = src.fpcr;
        fpsr = src.fpsr;
        tpidr_el0 = src.tpidr_el0;
        tpidrro_el0 = src.tpidrro_el0;
        tid = src.tid;
        is_fork_process = src.is_fork_process;
        // page_cache, decode_cache: NOT copied (per-thread, fresh in child)
        set_tid_address_ptr = 0;   // child starts with no clear_child_tid
        robust_list_head = 0;
        robust_list_len = 0;
        sigmask = src.sigmask;
        // sigpending, altstack: reset by caller (per Linux semantics)
        // pending queue: untouched (each CPU has its own, empty at init)
        excl_tag_valid = false;    // fresh exclusive monitor
        excl_tag_addr = 0;
        excl_tag_size = 0;
    }
};

} // namespace arm64emu
