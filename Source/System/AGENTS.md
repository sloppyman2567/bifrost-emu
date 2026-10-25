# Shared runtime and host integration

See [the source map](../../Docs/ARCHITECTURE.md) for subsystem entry points.
`Core/Emulator/` owns startup/lifetime, `Memory/` guest mappings, `Loader/`
ELF/dynamic linking, `Syscalls/` Linux ABI translation, `VFS/` guest paths and
devices, `Android/` Android host integration, and `Graphics/`/`Audio/` host APIs.

- Guest environment (`Emulator::build_default_guest_env`): without `BIFROST_ROOT` the guest gets the
  caller's own environment, read from `/proc/self/environ` (libraries linked into the emulator change
  `environ`), minus `LD_*`, `BIFROST_*` and `_`; PATH and HOME default only when missing, PWD names the
  real cwd. A sandbox gets the curated environment (HOME=/root and an allowlist of host variables).
  `test_guest_env.sh` covers both; do not add emulator-internal variables to the guest.
- Preserve the guest/host boundary: guest handles and addresses must round-trip
  through guest memory; host pointers are not valid guest pointers. Keep existing
  bounce/writeback and mapping lifetime contracts.
- Keep file mapping identity, discard/remap semantics, TLS layout, userfaultfd
  state, signal recovery and Binder arena ownership as specified in the scoped guides.
  A source split must preserve file-local callback state and initialization order.
- `Core/ExclusiveMonitor.hpp` is the one LL/SC monitor (version table); the interpreter,
  the JIT helpers and the native sequences share its protocol, so change them together
  (see the FrostJIT guide).
- Private `*Internal.hpp` headers stay within their subsystem. Public embedding
  interfaces are under `Include/Bifrost/` and `Source/System/API/`.
- Android guest shims/services live in `Guest/Android/` (see the Android guide
  for its subfolders); adapter provisioning
  reads the audio registry in `Audio/AudioThunk/AudioThunk.cpp`. When moving registry
  definitions, update `Tools/Android/build-android-adapters.py` too.
- Validate both engines and the affected host/guest boundary. Android AAudio and
  OpenSL ES need the managed Bionic tests, not just host audio checks.
  Display tests need GPU access; smoke frames do not establish game playability.

## Scoped contracts

- [Memory](Memory/AGENTS.md): mappings, discard/remap, userfaultfd and null pages.
- [Loader](Loader/AGENTS.md): ELF identity, library selection and thread TLS.
- [Graphics](Graphics/AGENTS.md): trampolines, guest handles and mapped buffers.
- [Audio](Audio/AGENTS.md): callback ownership and Bionic adapter wiring.
- [Android](Android/AGENTS.md): Binder, loopers, surfaces and service contracts.
- [Syscalls](Syscalls/AGENTS.md): process exit, ABI argument translation and signals.
- [VFS](VFS/AGENTS.md): host confinement and synthetic device/proc/sys views.

`Emulator::group_exit` stops every guest CPU, wakes blocking operations and
preserves the requested exit status. Fatal signals go through
`Emulator::signal_exit`. SIGKILL is not cooperative: it flushes stdio and
quick-exits (terminal restore via `at_quick_exit`). After every exit handler,
`Emulator::exit_by_guest_signal` (registered first in `main`) makes the host
process die by the guest's fatal signal, so parents see a signal death;
`run()` keeps returning 128+n. Crash handling must use the shared reporter
and terminate coherently; a worker failure must not leave the main CPU running.
Every guest run loop (main, worker and SDL threads) turns a synchronous fault
(`UnmappedMemory`, `DecodeError`) into `force_signal` inside the loop: when a
guest handler frame is set up the thread keeps running; only an unhandled
fault ends it.

`rt_sigaction` may query SIGKILL/SIGSTOP; only installing a new disposition
is forbidden. After host fork, private futex queues start empty and copied host
waiter objects must not be destroyed. `Emulator::fork_guest` must hold every lock the child, or the guest code it
runs, uses (comm table, futex shards, thread lists, guest descriptor table, guest memory index) across the host
`fork()`, taken all-or-none so no lock is waited for while another is held. Plain mutexes are then unlocked in the
child, but a read/write lock copies its queue of waiting writers and must be re-initialized there
(`Memory::reset_mutex_in_fork_child`); the child also drops the inherited JIT. Add any new process-wide lock that
guest syscalls take to that set. `test_fork_lock_contention` and `test_fork_exec_memory` cover it. A real host-fork child's `exit_group`
uses kernel termination without host resource teardown; track that ownership
separately from `CPU::is_fork_process`, which Binder-realm apps also use for PID
identity. The parent's display/audio connections must survive child exit.
