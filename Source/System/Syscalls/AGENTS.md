# Linux syscall translation

- Decode the AArch64 ABI's actual slots, widths, signed values, structures and
  errno rules. Read only the arguments the raw syscall defines: e.g. raw
  `faccessat` and `fchmodat` take three, so x3 is caller garbage (the flags
  forms are `faccessat2`/`fchmodat2`). Translate every guest dirfd through the
  FdTable before a host call. Compare the concrete failing syscall with the native kernel or
  a trusted reference before adding compatibility behavior.
- Preserve Linux fd/poll/epoll ownership and timeout semantics. A ready socket
  must not lose readiness when translating guest event structures or masks.
- `exit_group` uses Emulator::group_exit, waking blocked CPUs and preserving
  the successful exit code through native JIT callers. kill/tkill/tgkill
  default-fatal signals use Emulator::signal_exit, never `group_exit(128+n)`
  directly. Signal delivery uses
  the shared reporter/recovery path, not uncaught worker exceptions.
  Synchronous faults and traps (memory faults, SIGILL, BRK's SIGTRAP, a bad
  sigreturn frame) use `force_signal`, which reverts a blocked or ignored
  signal to SIG_DFL; only asynchronous kill/tgkill/queued signals use
  `deliver_signal` directly.
  A real host-fork child's `exit_group` instead uses host `_exit`: copied host
  waiter/display resources must not be torn down. Use Emulator's fork ownership
  flag, not the CPU's host-PID identity flag (also set for Binder-realm apps).
- Process identity comes from `Yggdrasil::process_ids()`: the PID-1 model of
  a plain run, or the host process's identity for fork children and
  Binder-broker apps. getpid/getppid/getpgid/getsid, `kill` and
  `/proc/self/{stat,status}` must agree with it; a leader's TID equals its
  PID, including after execve.
- Host-forwarded signals and `kill` of the own process are process-directed:
  route them through `Emulator::send_process_signal` (Linux
  `complete_signal`), never to whichever thread drained them. A signal every
  thread blocks stays in the process-wide set, which `rt_sigtimedwait`,
  `rt_sigpending`, unblocking and pending delivery also consult; ART's
  SIGQUIT thread dump depends on it. tkill/tgkill stay thread-directed.
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
