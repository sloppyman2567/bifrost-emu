# Asgard Binder transport

Bifrost provides virtual `/dev/binder`, `/dev/hwbinder` and `/dev/vndbinder`
character devices. By default each device has an independent context per emulator; no
request reaches the host Binder driver or the host's Android services.
Asgard's optional shared broker places these contexts in a separate OS process,
allowing independent guests to communicate. See
[shared process setup and tests](ANDROID_RUNTIME.md#shared-guest-processes-october-11-2026).
The wire layout is AArch64 Binder protocol 8, checked against the
[Linux Binder UAPI](https://raw.githubusercontent.com/torvalds/linux/v6.12/include/uapi/linux/android/binder.h)
and [driver implementation](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/android/binder.c).

## Supported operations

- Device discovery through stat/fstatat/statx and access/faccessat2.
- Version queries, maximum-thread configuration and per-thread extended errors.
- Exclusive context-manager registration, including its pointer/cookie form.
- Looper entry/exit, `BR_SPAWN_LOOPER` requests, registration accounting and
  maximum spawned-worker limits. User space creates the requested thread.
  Explicit thread exit is supported when no synchronous call is active.
- Transactions to context handle 0 and service handles, synchronous replies and one-way
  delivery between guest endpoints, including independent emulator processes
  attached to an Asgard realm.
- Per-thread completion/reply queues, synchronous call stacks, and manager
  work delivered to registered loopers.
- Blocking reads, nonblocking `EAGAIN`, guest-signal interruption with `EINTR`,
  and wakeups during process exit/emulator teardown.
- `BR_DEAD_REPLY` when an endpoint closes with unhandled synchronous work.
- Per-handle death subscriptions, `BR_DEAD_BINDER`, `BC_DEAD_BINDER_DONE` and
  `BR_CLEAR_DEATH_NOTIFICATION_DONE`. Clear-before-death, clear while queued or
  delivered, acknowledgement-before-clear, and already-dead subscriptions are
  tracked separately. Releasing the final handle reference removes its watch.
- Read-only receive arenas, buffer release, zeroing and free-range coalescing.
- Flat Binder object translation, forwarding of handles, restoration of local
  pointer/cookie pairs, and strong/weak reference commands with owner notifications.
- Flat descriptor transfer, `BC_TRANSACTION_SG`/`BC_REPLY_SG`, copied pointer
  buffers, ordered nested-parent fixups and FD arrays. FD-array descriptors are
  released with their receive buffer; flat descriptors remain open until closed
  by the recipient. All source objects are validated before reference commit.

## Concurrency and broker channels

The domain uses an ordinary mutex for shared handle/ownership/queue metadata.
Promoted peer references are retained until after unlocking, so a last-reference
release cannot recursively enter teardown while the lock is held. Transaction
command peers are released before a blocking read, so an acquire-and-read ioctl
cannot pin the dying owner whose death notification it is waiting for. Transaction
payload and scatter-buffer staging runs outside that lock. Wakeups target the
receiving endpoint, rather than every Binder thread in the domain. A read returns
at most one transaction, matching the Linux driver; delivering multiple calls in
one read previously stacked unrelated callers and misrouted replies under load.

Asgard RPC connections stay open across operations. Each Binder open pools up
to eight idle channels, while concurrent blocking calls borrow separate channels.
The separate lifetime connection still owns the endpoint and detects process
exit. Imported sender FDs are scoped to each RPC, preventing concurrent sends
from overwriting each other's captured files at a reused guest descriptor number.
Cancelled or broken channels are discarded. Wire protocol 2 carries explicit
FD patch widths and buffer lifetimes. The CLI detects protocol-1 brokers and
requires their app sessions to stop before a realm restart; it does not silently
replace a live older broker.

Metadata updates and receive-arena allocation/copy still serialize within a
domain. The broker still uses one host worker per connected channel, bounded by
its connection limit. These changes remove per-operation connection/thread churn
and broad wakeups; they do not establish kernel-Binder throughput or eliminate
all lock contention.

Receive arenas use offset-zero `MAP_PRIVATE` with `PROT_READ`, up to 4 MiB.
Receive arenas reserve an unoccupied range below the direct-window boundary,
even after ART has installed high fixed mappings. The focused Binder fixture
checks this placement and verifies that a preceding high mapping survives.
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
not yet implement transaction security contexts, Binder polling/epoll, process
freezing or Android zygote/isolated-service process launch. Reentrant synchronous
callbacks do not yet have Linux's full transaction-stack thread targeting. Cross-process descriptor transfer requires
host-backed descriptors. Unsupported ioctls return `ENOTTY`; unsupported command
streams return `EINVAL`; transactions with unsupported objects receive
`BR_FAILED_REPLY` and an `ENOTSUP` extended error. Malformed object offsets return
an `EINVAL` extended error. Handle 0 is pinned while its manager endpoint is open.
Each endpoint is limited to 1,024 objects/handles, 1,024 outstanding death records,
64 objects and 64 descriptors per parcel. The 1 MiB transaction budget includes
aligned data, offsets and scatter buffers.

The opt-in service-manager subset below supports named native endpoints. Full
Android servicemanager/framework compatibility remains incomplete. The separate
[SpongeBob Activity creation probe](ANDROID_RUNTIME.md#installed-app-creation-checkpoint-2026-10-07)
passes through `onCreate`; it does not exercise playable window attachment.

## Native service-manager subset

`--android-services` reserves handle 0 on `/dev/binder` for a host-owned registry.
Without this flag, the existing guest context-manager contract is unchanged.
`/dev/hwbinder` and `/dev/vndbinder` retain independent raw contexts. Select
the mode before opening a Binder endpoint; an explicit guest context-manager
registration is rejected while the host manager is selected.
The Android property area reports `servicemanager.ready=true` in this mode and
`false` without it, so native libbinder can contact the selected registry.

The wire subset follows Android 15's kernel-Binder
[IServiceManager AIDL](https://android.googlesource.com/platform/frameworks/native/+/android-15.0.0_r1/libs/binder/aidl/android/os/IServiceManager.aidl)
and [Parcel interface header](https://android.googlesource.com/platform/frameworks/native/+/android-15.0.0_r1/libs/binder/Parcel.cpp):
StrictMode, WorkSource, `SYST` and the String16 interface descriptor.
It implements interface discovery, `getService`, `checkService`, `addService`
and dump-priority-filtered `listServices`, with bounded ASCII names and aligned
parcels. Lookups are nonblocking. `PING_TRANSACTION` returns status 0, as
libbinder's first use of handle 0 requires. Unknown transaction codes return
Android's `UNKNOWN_TRANSACTION` status; malformed parcels fail explicitly.
Transactions accept Java's `FLAG_COLLECT_NOTED_APP_OPS` (2) and `TF_ACCEPT_FDS`
(0x10); neither makes a call one-way. `BIFROST_BINDER_TRACE=1` logs
transactions, registry calls and failure replies.

The registry holds strong references to real guest Binder objects. Lookup
returns callable translated handles, whose receive-buffer references follow
the existing acquire/free/release rules. Owner teardown removes its services.
Limits are 256 registered services, 127-byte names and the existing per-endpoint
handle, queue and receive-arena budgets. No placeholder framework services
are registered. Notifications, lazy services, declarations, permission policy
and complete platform servicemanager behavior are outside this subset.

The package launcher enables this mode automatically; see
[signed native APK launch](ANDROID_APK.md).

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
make cross SRC=Tests/Unit/android_binder.c OUT=Tests/Unit/android_binder.elf
make cross SRC=Tests/Unit/android_binder_objects.c OUT=Tests/Unit/android_binder_objects.elf
make cross SRC=Tests/Unit/android_binder_features.c OUT=Tests/Unit/android_binder_features.elf
make cross SRC=Tests/Unit/android_binder_exit.c OUT=Tests/Unit/android_binder_exit.elf
make cross SRC=Tests/Unit/android_syscall_options.c OUT=Tests/Unit/android_syscall_options.elf
make cross SRC=Tests/Unit/android_service_manager.c OUT=Tests/Unit/android_service_manager.elf
./Scripts/Testing/run_tests.sh --filter 'android_binder|android_syscall_options'
./Scripts/Testing/run_tests.sh --no-jit --filter 'android_binder|android_syscall_options'
./Scripts/Testing/run_tests.sh --filter android_service_manager
./Scripts/Testing/run_tests.sh --no-jit --filter android_service_manager
bash Scripts/Android/test_android_bionic.sh
python3 Scripts/Android/test_asgard.py
```

The October 11 feature fixture exercises nested pointer copying/rebasing,
SG requests and replies, ordered-fixup rejection, insufficient scatter space,
bad FDs, FD-array buffer cleanup, death clear/ack order, duplicate watches,
already-dead objects, released watches, context-manager death, owner teardown
while an acquire-and-read ioctl waits for death, looper requests,
unsolicited/duplicate registration, thread exit and a zero worker limit. It runs
locally under both engines and through the broker with real Bionic.

`test_asgard.py` also exercises 1,000 ioctls on one persistent stream, all four
JIT/interpreter combinations of independent server/client processes with nested
SG buffers and descriptors, killed-server death recipients in both engines,
and eight concurrent clients completing 1,600 checked request/reply calls.
The observed stress duration is a local test measurement, not a comparison to
kernel Binder. These tests do not cover every nested synchronous callback,
Binder epoll integration, or a Chromium renderer process.

The Binder fixture checks 61 behaviors including data round-trip, reply routing,
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

The service-manager fixture checks 52 behaviors, including descriptor/name
validation, registration, lookup, a real invocation through the discovered
handle, reference ownership, priority filtering and service-owner teardown.
It also runs inside the signed APK through the real Bionic wrappers. These
are subset tests; they do not prove compatibility with guest libbinder or the
Android framework as a whole.

At the latest October 5 checkpoint, the complete Linux suites pass 273/273 in JIT
mode and 266/266 in interpreter mode (seven JIT-only tests skipped). These
runs include the mapping registry, device discovery, object transfer and
XWayland/JIT regressions. The separate real Bionic bootstrap passes its six
fixed/random ASLR and execution-mode configurations.
