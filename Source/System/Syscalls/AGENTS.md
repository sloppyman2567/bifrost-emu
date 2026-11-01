# Linux syscall translation

- Decode the AArch64 ABI's actual slots, widths, signed values, structures and
  errno rules. Read only the arguments the raw syscall defines: e.g. raw
  `faccessat` and `fchmodat` take three, so x3 is caller garbage (the flags
  forms are `faccessat2`/`fchmodat2`). Translate every guest dirfd through the
  FdTable before a host call. Compare the concrete failing syscall with the native kernel or
  a trusted reference before adding compatibility behavior.
- Preserve Linux fd/poll/epoll ownership and timeout semantics. A ready socket
  must not lose readiness when translating guest event structures or masks.
- Thread `exit` writes the musl tid word (`TPIDR_EL0 - 0xa0`) and `clear_child_tid`
  only while the guest still maps them: `munmap` marks a thread that freed its own
  TLS block (`CPU::exit_tid_slot_freed`), since another thread may reuse the range
  before the exit runs. `test_thread_exit_unmapped_tls` covers it.
- `exit_group` uses Emulator::group_exit, waking blocked CPUs and preserving
  the successful exit code through native JIT callers. kill/tkill/tgkill
  default-fatal signals use Emulator::signal_exit, never `group_exit(128+n)`
  directly. Signal delivery uses
  the shared reporter/recovery path, not uncaught worker exceptions.
  Synchronous faults and traps (memory faults, SIGILL, BRK's SIGTRAP, a bad
  sigreturn frame) use `force_signal`, which reverts a blocked or ignored
  signal to SIG_DFL; only asynchronous kill/tgkill/queued signals use
  `deliver_signal` directly.
  `execve` discards the old image's pages (`madvise_dontneed`, then `untrack_allocation`); never write zeros through
  them: a fork child shares them copy-on-write with its parent and the write copies every page.
  A real host-fork child's `exit_group` instead uses host `_exit`: copied host
  waiter/display resources must not be torn down. Use Emulator's fork ownership
  flag, not the CPU's host-PID identity flag (also set for Binder-realm apps).
- Process identity comes from `Yggdrasil::process_ids()`: the PID-1 model of
  a plain run, or the host process's identity for fork children and
  Binder-broker apps. getpid/getppid/getpgid/getsid, `kill` and
  `/proc/self/{stat,status}` must agree with it; a leader's TID equals its
  PID, including after execve.
  `setsid` is real for a host-fork child of a plain run (forkpty, daemons: `getsid`, `/proc` and the pty's `TIOCGSID` then agree); the PID-1 model
  answers 1, and the Android runtime keeps EPERM because its supervisor owns the session its helpers share.
- Host-forwarded signals and `kill` of the own process are process-directed:
  route them through `Emulator::send_process_signal` (Linux
  `complete_signal`), never to whichever thread drained them. A signal every
  thread blocks stays in the process-wide set, which `rt_sigtimedwait`,
  `rt_sigpending`, unblocking and pending delivery also consult; ART's
  SIGQUIT thread dump depends on it. tkill/tgkill stay thread-directed.
- `execve` lives in `Process/Exec.cpp`. It reads `envp` (a NULL one is empty; the new image gets exactly
  that), follows `#!` interpreters (four levels, `[interp, arg, script, args]`), returns ENOENT, EACCES
  (directory or no execute permission, checked through the opened descriptor) and ENOEXEC, closes
  `FD_CLOEXEC` descriptors at the point of no return, records the real file for `/proc/self/exe`, and
  gives AT_RANDOM real entropy. Targets are opened through the VFS, never the raw host path
  (`/proc/self/exe` would be the emulator binary). A foreign ELF (a host `/bin/ls`) is stood in for by
  the running program only when that is a multi-call binary (`program_is_multicall`: BusyBox, toybox)
  and the file exists: a missing file must stay ENOENT, or `execvp` over a `PATH` re-runs the current
  program as the missing name (an exec loop). The Android branch keeps its own checks (no permission
  test, no descriptor closing). `test_execve` covers it.
- `clone`/`clone3` with CLONE_VFORK and without CLONE_THREAD (glibc `vfork`, both libcs' `posix_spawn`,
  hence `system` and `popen`) run as a copy-on-write fork on the caller's stack (`clone_is_vfork`): the
  parent is not suspended and does not see the child's writes. Other CLONE_VM-without-THREAD processes
  stay ENOSYS. `test_posix_spawn` covers it.
- Resource limits live in `Emulator::get_rlimit/set_rlimit` (`ResourceLimits.cpp`): seeded from the host,
  per process, changed only in that table, never on the host. RLIMIT_NOFILE is enforced by `FdTable`
  (EMFILE). `kill(pid, 0)` asks the host for any pid but the caller's own (ESRCH/EPERM).
  `test_rlimit` and `test_signal_semantics` cover them.
- fcntl owner, signal and lease state (`F_SETOWN`/`F_SETSIG`/`F_SETLEASE`, `Node::async_owner`/`async_sig`/`lease`) belongs
  to the open file description, so dup()ed descriptors share it. Nothing is delivered and leases are never broken: do not
  forward these commands to the host fd, whose lease break would raise SIGIO against the emulator itself.
  `test_fcntl_owner` covers it (the `emu` argument selects the pre-6.15 procfs lease behaviour).
- FD_CLOEXEC is per guest descriptor (`FdTable::set_cloexec`): set it wherever a creating call takes
  O_CLOEXEC-style flags, never forward F_SETFD to the shared host fd.
- A syscall that delivers a signal to its own caller (`kill` of self, `tgkill`, `tkill`)
  builds the handler frame inside the syscall: the frame saves x0 as the result and loads
  x0 = signo. Write the result (`ret_host(0)`) BEFORE `deliver_signal` / `send_process_signal`
  and never touch x0 afterwards, or the handler sees signal 0 and the call returns its first
  argument. `test_signal_semantics` (`test_self_signal`) covers it.
- POSIX timers (107-111) are `GuestTimers` (`Core/Emulator/`): one host timerfd per timer, so arming, absolute times,
  intervals and expiry counts are the kernel's, and a service thread queues each expiry (`Emulator::send_timer_signal`).
  A timer keeps one signal queued; later expiries raise its overrun. A forked child inherits none and `execve` destroys
  them. CPU-time clocks return ENOTSUP. A signal every thread blocks goes to the process-wide set, which keeps no
  timer details. `test_posix_timers` passes on a native kernel too; qemu-aarch64 is not a reference (it rewrites ids).
- `rt_sigtimedwait` really blocks for its requested bounded wait; futex requeue
  reads its count from the correct ABI slot. Preserve the relevant regressions.
- A futex word in a writable MAP_SHARED file mapping (`Memory::shared_futex_word`:
  memfd, ashmem, files) waits, wakes and requeues through the host futex on its
  host address, so another mapping of the page, in this or another process,
  reaches the same waiters (AudioTrack's control block). Other words keep the
  in-process table keyed by guest address. A host wait interrupted by
  `KICK_SIGNAL` with no pending guest signal returns 0, a spurious wake.
  `futex_shared_memfd` covers it. `BIFROST_NO_SHARED_FUTEX=1` (DebugFlags) switches the
  host path off, to A/B a suspected futex problem on the same build.
- JIT invalidation calls name their `CodeInvalidation` reason. Only cache
  maintenance and mprotect may keep translations with unchanged bytes;
  mapping replacement, unmap, remap and discard always retire them.
- Memory operations retain alias/file identity and userfaultfd registration;
  TLS allocation stays consistent with the loader's layout. Follow those scoped
  guides when changing their syscall entry points.
- Compatibility families belong in `Compatibility.cpp`; unsupported semantics
  should fail deliberately rather than returning guessed successful values.
- Each family handler (`Files/`, `Process/`, `HostCalls/`, ...; see the source
  map) owns a disjoint set of syscall numbers. Add a case to the family that owns
  that kind of operation and never duplicate a number in another handler: an
  earlier handler would silently shadow it. Bifrost-private calls (>= 0x1000)
  belong in `HostCalls/`. Families outside the `Emulator` friend list use its
  public accessors.

- Android execve enters the confined Bionic interpreter with fresh vDSO/auxv and reset TLS; never reuse the previous vDSO address or relocate the executable before linker64.
