# Android Binder transport in Bifrost

Bifrost provides virtual `/dev/binder`, `/dev/hwbinder` and `/dev/vndbinder`
character devices. Each device has an independent context per emulator; no
request reaches the host Binder driver or the host's Android services.
The wire layout is AArch64 Binder protocol 8, checked against the
[Linux Binder UAPI](https://raw.githubusercontent.com/torvalds/linux/v6.12/include/uapi/linux/android/binder.h)
and [driver implementation](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/android/binder.c).

## Supported operations

- Device discovery through stat/fstatat/statx and access/faccessat2.
- Version queries, maximum-thread configuration and per-thread extended errors.
- Exclusive context-manager registration, including its pointer/cookie form.
- Looper entry/exit and explicit thread exit when no synchronous call is active.
- Transactions to context handle 0 and service handles, synchronous replies and one-way
  delivery between guest endpoints in one emulator memory space.
- Per-thread completion/reply queues, synchronous call stacks, and manager
  work delivered to registered loopers.
- Blocking reads, nonblocking `EAGAIN`, guest-signal interruption with `EINTR`,
  and wakeups during process exit/emulator teardown.
- `BR_DEAD_REPLY` when an endpoint closes with unhandled synchronous work.
- Read-only receive arenas, buffer release, zeroing and free-range coalescing.
- Flat Binder object translation, forwarding of handles, restoration of local
  pointer/cookie pairs, and strong/weak reference commands with owner notifications.

Receive arenas use offset-zero `MAP_PRIVATE` with `PROT_READ`, up to 4 MiB.
The driver writes through a separate writable host alias of a private memfd;
CPU stores remain blocked by real read-only host page permissions. There is
no writable interval in the guest mapping. The memfd is never exposed as a
host fd to guest polling/ioctl passthrough.

Payloads are limited to 1 MiB, pending transaction bytes must fit the receive
arena's remaining capacity, transaction admission checks a 256-event queue budget and
synchronous stacks have a 64-call limit. Malformed command sizes, unknown
commands, invalid pointers and duplicate buffer releases fail explicitly.
Partial command progress is reported in `write_consumed`/`read_consumed`.
Unmapping any part of an arena invalidates it before guest address reuse;
active Binder arenas cannot gain write/execute permission or move via mremap.

## Boundaries

This is a transport foundation, not a complete Android Binder driver. It does
not yet implement descriptor or scatter-gather
transfer, death-notification subscriptions, transaction security contexts,
automatic looper spawning, Binder polling/epoll, process freezing or Binder IPC
across guest processes. Unsupported ioctls return `ENOTTY`; unsupported command
streams return `EINVAL`; transactions with unsupported objects receive
`BR_FAILED_REPLY` and an `ENOTSUP` extended error. Malformed object offsets return
an `EINVAL` extended error. Handle 0 is pinned while its manager endpoint is open.
Each endpoint is limited to 1,024 objects/handles and each parcel to 64 objects.

Android servicemanager/framework compatibility remains unverified. This checkpoint
does not establish APK or SpongeBob startup.

## Syscall options

`prctl` now retains validated dumpability and THP-disable state, irreversible
per-thread no-new-privileges state, and per-thread timer slack. New threads
inherit their parent's no-new-privileges and timer slack; resetting slack to
zero restores the child's inherited default. THP disabling is recorded; Bifrost
does not independently allocate guest transparent huge pages. These controls
operate inside the guest model rather than changing the host process's security
policy. Other pre-existing compatibility stubs remain outside this change.

`membarrier` uses the host kernel's cross-thread barrier, supports available
GLOBAL/PRIVATE_EXPEDITED/SYNC_CORE commands, records guest registration state,
and reports a filtered support mask. RSEQ barriers and CPU-targeted requests
are not advertised. A local atomic fence is insufficient for this operation.

`PR_SET/GET_TAGGED_ADDR_CTRL` remain unsupported (`EINVAL`). Existing Android
CPU data-TBI support does not establish the full per-thread tagged-syscall ABI
or MTE semantics, so these options do not claim success.

## Verification

```bash
make -j6
make cross SRC=ctest/android_binder.c OUT=ctest/android_binder.elf
make cross SRC=ctest/android_binder_objects.c OUT=ctest/android_binder_objects.elf
make cross SRC=ctest/android_binder_exit.c OUT=ctest/android_binder_exit.elf
make cross SRC=ctest/android_syscall_options.c OUT=ctest/android_syscall_options.elf
./scripts/run_tests.sh --filter 'android_binder|android_syscall_options'
./scripts/run_tests.sh --no-jit --filter 'android_binder|android_syscall_options'
bash scripts/test_android_bionic.sh
```

The Binder fixture checks 57 behaviors including data round-trip, reply routing,
read-only protection, malformed requests, isolated device contexts, peer close,
invalidated receive mappings, blocking delivery and signal interruption. Mapping hooks track only virtual mapping devices;
ordinary Linux mmap/protection calls avoid scanning the file table. The
object fixture checks 43 behaviors including publishing a service, forwarding its handle, invoking it,
returning it to its owner, reference notifications and malformed parcels. The
exit fixture verifies that `exit_group` can terminate a thread blocked in a
Binder ioctl. The syscall fixture checks 22 state, inheritance, validation and
barrier behaviors on a host supporting the expedited commands. The pinned real
Bionic fixture also opens/maps Binder and checks `prctl` through actual Bionic
wrappers, in fixed/random ASLR and interpreter/JIT/Tier-2 configurations.

At the October 5 checkpoint, the complete Linux suites pass 272/272 in JIT
mode and 265/265 in interpreter mode (seven JIT-only tests skipped). These
runs include the mapping registry, device discovery, object transfer and
XWayland/JIT regressions. The separate real Bionic bootstrap passes its six
fixed/random ASLR and execution-mode configurations.
