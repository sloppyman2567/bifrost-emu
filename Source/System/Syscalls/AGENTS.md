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
- Process identity comes from `Yggdrasil::process_ids()`: the PID-1 model of
  a plain run, or the host process's identity for fork children and
  Binder-broker apps. getpid/getppid/getpgid/getsid, `kill` and
  `/proc/self/{stat,status}` must agree with it; a leader's TID equals its
  PID, including after execve.
- `rt_sigtimedwait` really blocks for its requested bounded wait; futex requeue
  reads its count from the correct ABI slot. Preserve the relevant regressions.
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
