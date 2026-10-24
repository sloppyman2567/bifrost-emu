# Tests

Dated checkpoints record the revision and scope tested at that time. Earlier
failures and totals remain as history; use later checkpoints for superseding
results. Passing fixtures do not establish untested application behavior.

## Installable Asgard (2026-10-24)

`Scripts/Testing/test_asgard_install.py` (10 tests, pass) runs the installer from a small fake checkout: the
first install (links, empty and read-only directories, no tests or build-only sources, marker, wrapper), an
unchanged re-run, `--dry-run`, an update that copies only what changed and removes what went away (an untouched file
keeps its inode and time), repair of a tampered read-only tree, size-only and time-only changes, `--verify` finding a
same-size edit, an unprovisioned checkout, uninstall refusing foreign or symlinked locations. Four deliberate faults
(never removing stale files, ignoring size, ignoring time, never comparing contents) each fail a test. `AndroidPathsTests`
(3) and `RunFromFileTests` pass; `test_apk_launch.sh` now also finds Asgard beside `bin/` and in `XDG_DATA_HOME`.
With the real checkout, a first install (1,927 entries, 1.1 GiB, under a second with reflinks) was updated in place after
drift (changed, deleted and stray files) and a read-only tree; a real SpongeBob bundle installed through the read-only
copy into a temporary state in about 5 s, and the launch-time reads (managed launcher dex check, both microG
archives, the framework image check, the installed-app integrity gate) succeeded from it. **No guest was launched
from an installed tree, nothing was tried on another distribution, and a system-wide install as root was not run.**

## Skipping an unchanged install (2026-10-24)

`RunFromFileTests` (3 tests, pass) now includes `test_run_skips_installing_a_file_that_is_already_installed`: the
installer is not started for a file whose hash an installed app recorded, and is started for a different file,
an app without a recorded hash, a record under the wrong directory name, a missing root, unreadable metadata and a
file changed after install; disabling the skip fails it. The real signed-install fixture
(`test_invalid_install_does_not_replace_existing_package`, real aapt2/apksigner, no window) passes and checks
`source_sha256` equals the bundle's SHA-256; the persistence/launch test has the same assertion but was **not
run**, because it launches a guest. The speedup itself was not measured, and no real game was started. A bundle
reordered into a different file with the same contents records its own hash, so the original file installs once
more the next time it is run.

## APK launch (2026-10-24)

`Scripts/Testing/test_apk_launch.sh` (14 of 14 pass; the binary without the change exits 126 at the first run) uses a
stub `asgard.py` in a temporary tree and checks the exact argument list, the launch directory, every package
extension (upper case too), `--android-window-plugin`, the returned exit status, `BIFROST_ROOT` removal,
`BIFROST_ASGARD`, status 127 without Asgard or for a missing file, status 126 for a non-ZIP, and that a plain ELF
still runs. By hand, a bogus ZIP through the real `asgard.py` reached `android_package.py install` and was rejected
without touching the Android state directory. **No real APK was installed or run**: that opens a window and changes
installed-app state, so end-to-end launch of an actual game through this path is untested.

## Rootfs install (2026-10-24)

`Scripts/Testing/test_rootfs_install.sh` (11 of 11 pass) installs from the toolchains already in `Tools/` into a
temporary prefix, copies the emulator to `PREFIX/bin`, and runs both hello fixtures with no flags and no rootfs
argument, which exercises the `<exe dir>/../share/bifrost/rootfs` layout. It also checks the loaders, `ld.so.cache`,
`/etc`, leftover staging directories, reinstall replacing the old tree, uninstall refusing a directory without a
loader, and uninstall. By hand: a rebuild that fails (bad `AARCH64_SYSROOT`) kept the existing install and removed
its staging directory; `make install-rootfs`/`uninstall-rootfs` work with `DESTDIR`; missing toolchains with
`--no-fetch` stop with a message and create nothing. Not tested: a real system-wide install as root, `--full` (needs
the network), `--user` into a real home, and the downloads themselves. Installed files carry the installing user's
timezone, as `setup-rootfs.sh` always has.

## Rootfs discovery (2026-10-24)

`make test-rootfs-discovery` (host unit test, passes) covers the loader-file check, candidate order, relative
`XDG_DATA_HOME` and `BIFROST_ROOT` precedence. `Scripts/Testing/test_rootfs_discovery.sh` copies the emulator to a
temporary directory, then runs `hello_dyn_musl` and `hello_dyn_glibc` with no `--rootfs`/`BIFROST_ROOT` through
`XDG_DATA_HOME`, `~/.local/share` and an executable-adjacent rootfs, and requires a hint, exit status 1 and no
signal 11 when none exists, and now a fixture whose `PT_DYNAMIC` lies outside every load segment must stop with the
native-link reason and exit status 1 (the old binary printed "falling back to guest ld.so" and crashed, status 139):
seven of seven pass. The unmodified binary fails the musl cases and the missing-rootfs case
(status 139); its glibc cases pass only because of the checkout's bundled toolchain directory. A throwaway dynamic
glibc fixture (outside the repository) saw a host file by absolute path under a discovered rootfs and not under
`--rootfs`. `--guest-ld`, `BIFROST_ROOT` and a static toybox run were rechecked by hand; the full
`run_tests.sh` suite, `make verify` and a headless build were not run (the five touched sources pass
`-fsyntax-only`). Only the two hello fixtures exercised discovery end to end; Qt, thunked-library and Android
programs were not.

## Desktop library redesign (2026-10-23)

`LauncherLibraryTests` (13 tests) builds the real library window on Qt's offscreen platform (an
800 x 600 screen) with a temporary state folder and settings file, and passes together with the existing
`LauncherProfileTests` and `LauncherArgumentTests` (24 tests, three runs, exit status 0):
grid metrics for every width from 208 to 3000 px, cards filling the viewport on first show and after
resizes (the layout bug above), sort order and its persistence with the window size across a restart,
the per-card activity line, Enter/Space/double-click/arrow keys including a disabled game, the context
menu's enabled states, the polling policy (minimized, idle, focused, running), the install queue (one
at a time, a queued extra, a mid-batch failure, the announcement of a single install), dropped-file
filtering, icon cropping and tile colors read back from the rendered pixmaps, the empty-state text height,
palette-driven styling, and a real `asgard.py launcher` process that must answer an activation request.
Four injected regressions (no reflow on viewport resize, polling while minimized, acting on a disabled game,
starting a second install while one runs) each failed the matching test, and dropping an import in `show()` failed
the process test.

I viewed offscreen renders (Fusion style, dark and light palettes, 720-1700 px wide, 1.5x scale, with real installed
icons and running/starting/failed cards, the drawer, notice, empty, no-match and drop states) to judge the
design. They are not renders of a KDE desktop: the launcher has not been opened on a real desktop, drag-and-drop from
a file manager and the context menu popup were not exercised there, and Breeze's own widget chrome (scroll bars,
menu arrows) will differ from Fusion's.

```sh
cd Scripts/Android && QT_QPA_PLATFORM=offscreen python3 test_android_package.py LauncherLibraryTests
```

## MT preemption epoch, forward edges and cell calls (2026-10-23)

Tested on a Ryzen 7 5800X3D at `f5b67318` plus the working tree; no game was run. Overhead numbers are from
`mtbench` (a throwaway guest fixture of eleven kernels kept outside the repository), median of five, ST vs the same
kernel with an idle second vCPU, against a clean `e7ed1b3c` build; they are in [JIT.md](JIT.md#mt-preemption-epoch-forward-edges-and-cell-calls-2026-10-23).

Passed: `test_jit_mt` (all five modes), `test_jit_mt_publication` (default and `BIFROST_NO_CHAIN_SKIP=1`),
`test_jit_tier2_discovery/feedback/measure/mutex` (20 promotion-stress runs), `test_jit_cache`,
`run_tests.sh --unit --toybox --strict` with the full SDL/GL build (172 of 172) and with `--no-jit` (164 passed, 8
JIT-only skipped), `make generated-check`, the headless build and `check_headless_headers.sh`. `make verify` runs
108 sections; its only failure is the known `jit_bfm_extract` limit (see below).

`jit_mt_publication` changed with the contract. The self-loop fixture now covers four ways out of five loop shapes:
a fresh lease running to completion, a stale epoch (exit at the first back-edge), the callback sentinel, and a thread
advancing the epoch during the run (200 runs per shape, leaving at arbitrary iterations); all four check registers,
flags and vector accumulators at the exit, and the last requires at least one mid-loop exit. The retired-loop check
now grants the lease before the invalidation, which is what an in-flight CPU holds. The bounded-run check expects
a 10^12-iteration loop to return within 500 ms. `check_forward_chains()` is new: a chain of forward branches runs
end to end under a stale epoch (it fails at `f5b67318`, where every edge consumed the lease).

Mutation checks: making `emit_preempt_poll` never fire (`jo` instead of `jne`) hangs the bounded-run check until the
30 s kill, so `test_jit_mt_publication.sh` fails. Two guest fixtures (kept outside the repository) show a thread spinning in a
tight native loop receives a cross-thread `pthread_kill` in about 1.4 ms (the clean build takes 1.3 to 2.4 ms) and a
`fork` child exits on a signal; the fork case does not exercise the ticker, because the child is interpreter-only.

Not covered: Tier-2 sampling trials with epoch-polled loops (sampling code keeps the budget by design); a callee
invalidated while `call [cell]` is in flight beyond what the guest concurrency oracle (`jit_mt_tier2`) and
`test_long_call_loop` (60 million calls on two CPUs) do; the stale-NZCV-at-a-faulting-loop-head case for MT (it matches ST,
and no fixture reads flags in a handler there); hosts other than this one; and any application workload. The generated
instruction encodings were decoded with `objdump`, not compared against an assembler.

Also fixed: the `simd_minmax` and `simd_pixel` rows took 14 s and 20 s under `--no-jit` against limits of 5 s and
10 s (added at `b0e4b1b8` and `f448d3a2`); the limits are 40 s and 60 s.

## Pixel and media vector forms, self-loop reconcile (2026-10-23)

Tested on a Ryzen 7 5800X3D (SSE4.2, AVX2, FMA3) at `e7ed1b3c` plus the working tree; no game was run.
`jit_simd_minmax` (432,000 checks), `jit_simd_pixel` (376,104), `jit_simd_round` (158,628) and
`jit_vec_pressure_loop` (32 lanes) compare against lane models. The first three pass under QEMU, the
interpreter, the JIT, `BIFROST_JIT_VERIFY=1`, `BIFROST_TIER2=1`, `BIFROST_IR_VALIDATE=1` and (pixel, round)
`BIFROST_NO_AVX2=1`; the pressure fixture passes under QEMU and the JIT, and under verify only with the fix
(one vector divergence at `e7ed1b3c`, none after). `jit_native_simd_gen` passes 2,169 encodings (689 new, hashes
recorded under QEMU by `make nativegen-expect`) in the JIT and the interpreter, and `make test-native-simd-helpers`
(`Tests/Host/native_simd_helpers.cpp`) passes the same 2,169 against every `run<K>` body; it fails when one hash
is altered. `make test-ir-ssa` still passes 21,839 checks; `run_tests.sh --unit --toybox --realworld` passes
228 of 228 with the full SDL/GL build, the headless build (`USE_SDL2=0 USE_THUNK_GL=0`) passes the new fixtures,
and `make generated-check` passes.

`make verify`'s only failure is `jit_bfm_extract` in verify mode: it takes 95 s against the gate's 30 s limit on
this machine, and a clean build of `e7ed1b3c` takes the same 95 s. The other 107 sections pass, including the four
new fixtures.

Mutation checks: nine single-point defects in the inline emitters (wrong `pavg` width, swapped `max`/`min`,
dropped rounding bit and constant, un-inverted `CMTST`, `MLS` adding, swapped `pcmpgt` operands, 32-bit signed
halving with logical shifts) each fail the matching fixture and the generated test; a dropped `Vd` read
declaration for `SABA/UABA` in `vec_inst_operands` fails the loop kernel's accumulators. Dropped `Rm` read
declarations were not caught by any loop shape I tried.

Not covered: hosts without SSE4.1, SSE4.2 or AVX2 (only the helper bodies, the eligibility predicates and
`BIFROST_NO_AVX2` were exercised); whether Tier-2 promoted these loops (the opt-in path ran and passed); the
saturating family, which still steps the interpreter and does not set `FPSR.QC` in this change; and any
application workload.

```sh
make cross SRC=Tests/Unit/jit_simd_pixel.c OUT=Tests/Unit/jit_simd_pixel.elf
qemu-aarch64 Tests/Unit/jit_simd_pixel.elf
make test-native-simd-helpers
```

## Per-process compatibility defaults (2026-10-23)

`FrameworkCompatibilityTests` runs separate real ART VMs targeting API 29, 33,
34 and 35. Each loads a read-only DEX and attempts the same writable DEX:
targets 29/33 allow it, while 34/35 throw `SecurityException`. The fixture
executes a method from the loaded DEX, checks the compatibility gate's boundary
and verifies disabled-by-default metadata. Both JIT and interpreter are tested
without desktop windows. `FrameworkServiceRoutingTests` additionally verifies
that normal binding installs API-29 VM and compatibility state in its parent
and both real service children before their service callbacks run.
All eight direct DEX cases pass. The three-process interpreter gate also passes
with its DEX correctly aligned inside the fixture APK. Two earlier runs with
unaligned DEX missed the existing 30-second child-bootstrap deadline while ART
extracted code and waited for GC. The runtime deadline was not changed.

```sh
python Scripts/Android/test_android_package.py FrameworkCompatibilityTests FrameworkServiceRoutingTests
```

This exercises the pinned image's ART/libcore metadata, not every platform
compatibility change, developer overrides, or the real DroidGuard payload.
An approved Japanese Uma restart verified the original request in session
`1a609cf3b8c64fc1a6a93bf1c5b1731b`: microG and Phonesky both load the cached
DroidGuard APK's native library successfully. The legacy-target warning about
writable DEX remains, but no writable-DEX `SecurityException` or DroidGuard
fallback failure occurs. Phonesky logs a response from the integrity endpoint,
a nonempty opaque integrity token and its success callback, with no request
exception or error callback. Token contents and device-integrity labels were
not inspected; API success does not establish Cygames acceptance, a certified
device verdict or gameplay.
An initial fixture using Java NIO `Files.copy` failed with `EINVAL` before DEX
loading; the final fixture uses stream copying. The NIO copy path remains a
separate unaddressed failure.

## Child service routing (2026-10-23)

`FrameworkServiceRoutingTests` runs `FrameworkServiceRoutingProbe` headlessly
under real ART and Binder in three supervised processes. A service child starts,
binds, peeks, unbinds and stops a service in its parent, and starts/binds a helper
in a third process. The helper calls `stopSelfResult` while remaining bound.
The gate checks distinct PIDs, absence of the host-supervisor capability in the
child, rejection of a forged caller package, export checks on an existing service
record, isolated-caller rejection and outgoing-binding cleanup after child death,
including a child that binds its own service.

```sh
python Scripts/Android/test_android_package.py FrameworkServiceRoutingTests
```

Both JIT and interpreter reach `SERVICE_CHILD_ROUTING_OK` and `FRAMEWORK_OK`
with no presented frames. The production managed launcher also builds. Caller
permission and stop-token behavior were reviewed against Android 15's
[ActiveServices](https://github.com/aosp-mirror/platform_frameworks_base/blob/android-15.0.0_r1/services/core/java/com/android/server/am/ActiveServices.java).
This gate does not validate a real isolated-process lifecycle, sticky restarts,
the complete Android background-execution policy, or Google integrity verdicts.
An approved Japanese Uma restart also verified the target path: Phonesky reports
`bindService=true` and receives DroidGuard's `onServiceConnected`; the missing
process-supervisor exception no longer occurs. Session
`f760d4d8368c4f73b6f432e6e0328685` subsequently fails in DroidGuard's dynamic
class loader because ART rejects its writable cached `the.apk`. microG's fallback
throws `NotImplementedError`, and Phonesky reports `DroidGuard failed`. This is a
separate code-loading failure, not a device-integrity verdict or gameplay success.

## Guest ptrace (2026-10-22)

`test_ptrace` covers TRACEME, attach to a parent blocked in read, real wait stops,
SIGCHLD identity/status, PEEK/POKE (including private read-only pages and CoW),
invalid memory/option errors, GPR/FPSIMD/TLS regsets, exact one-instruction
stepping, signal suppression/reinjection, traced exec, SEIZE/INTERRUPT,
waitid WNOWAIT, detach, tracer death and EXITKILL. Native Linux passes the
portable process/memory/signal checks; Bifrost JIT, interpreter, Tier-2 and
headless JIT also pass the AArch64 register and stepping checks. This does not
test every instruction under single stepping, native ARM register validation,
cross-process executable-code patching, or every signal/job-control ordering.
Unsupported ptrace contracts are listed in the
[runtime continuation](Android/ANDROID_RUNTIME.md#guest-ptrace-continuation-2026-10-22).

The selected process/signal/futex/procfs runner gate passes 14/14 in both engines;
the filesystem confinement selection passes 2/2 in both engines. SDL and
headless `all lib` builds and `make generated-check` pass. The real Global
windowless run reaches Application creation, Activity creation and resume,
then reports a surface-preparation error; it times out during shutdown. It
does not demonstrate visible rendering or gameplay.

`make verify` runs 96 differential fixtures and eight threaded oracles. One
fixture, `jit_bfm_extract`, times out at 30 seconds, also at a separate
90-second replay limit; the other runs report no failures. Its ordinary JIT
and QEMU runs both complete all 3,102,720 checks. The replay timeout remains
unresolved, so the full verification gate has not passed. IR SSA passes
21,839 checks. No claim is made that this timeout predates the change.

## Procfs inode identity (2026-10-22)

`test_proc_rewind` now compares directory entries, path stat, independent open
file descriptors, relative directory-fd stat, AT_EMPTY_PATH, and statx for
`/proc/filesystems`, `/proc/cpuinfo`, `/proc/self/maps`, and the process's own
numeric-PID environ path. It also checks trailing-slash ENOTDIR and unknown
stat-flag EINVAL. Native Linux, Bifrost JIT/interpreter/Tier-2 and the headless
build pass. The four selected procfs runner cases pass 4/4 in both engines, and the
filesystem confinement gate passes 2/2 in both engines. SDL and headless `all lib` builds pass. QEMU agrees on the target
`/proc/filesystems` identity, but its special cpuinfo/maps open handling
produces different descriptor identity from path stat in the broader fixture.

The real Global client advances past the recorded inode error, then reaches
forked self-debugging: ptrace is still an unconditional EPERM stub, and related
PID status queries remain unsupported. A watchdog exits the main process with
status 0 before Application startup completes. This does not establish gameplay.
Excluded from the identity fix: SysFS/DevFS inode identity, general procfs
symlink/parent-component resolution, and metadata changes during an already-open
snapshot. The existing procfs directory listing's legacy d_type constants are
also not corrected by this change.

## Guest procfs environment (2026-10-22)

`test_proc_cmdline` now compares the complete NUL-separated initial environment
with `/proc/self/environ` and `/proc/<getpid()>/environ`, checks that `setenv`
does not replace that block, and verifies direct edits to the original memory.
It passes on native Linux, Bifrost JIT/interpreter/Tier-2 and the headless build.
The selected `proc_exe` and `test_proc_cmdline` runner gate passes 2/2 in both
engines; SDL and headless `all lib` builds pass. QEMU passes initial contents
and `setenv`, but fails direct-memory environment/argv visibility checks.
This covers startup block contents and reopening procfs; it excludes
PR_SET_MM relocation and concurrent edits while reading an already-open fd.
The real Global client now passes its formerly failing module-2 initialization
but aborts at module `0x60` after its `ro.kernel.qemu` check rejects `"1"`;
see [the runtime trace](Android/ANDROID_RUNTIME.md#global-umamusume-after-the-null-call-fix-2026-10-22).

## Signal queries and fork-child lifetime (2026-10-22)

`test_signal_semantics` now queries SIGKILL/SIGSTOP through raw `rt_sigaction`
with and without an output buffer, while checking that setters still fail
EINVAL. `test_fork_threads` adds three workers blocked on one private futex:
the child must see zero inherited waiters, exit with the expected status, and
leave the parent able to wake and join its workers. The signal query failed
and blocked-worker child exit hung before the fixes; QEMU passed. Both updated
fixtures pass in JIT, interpreter and Tier-2, and on the headless JIT build.

The existing process/signal/futex/exit selection passes 10/10 in each engine:
`run_tests.sh --unit --strict --quick --filter
'signal|sigtimedwait|sigreturn|fork|futex|exit_blocked|thread_fault|proc_exe'`,
also with `--no-jit`. SDL and headless `all lib` builds pass. Global Umamusume
now reaps its child and exits 127 itself after requesting HOME; startup and
gameplay remain unconfirmed. See [the traced path](Android/ANDROID_RUNTIME.md#global-umamusume-after-the-null-call-fix-2026-10-22).
These tests exclude arbitrary host-library fork locks and successful shell/AM
execution; the runtime does not yet provision `/system/bin/sh`.

## AudioTrack playback and shared futexes (2026-10-21)

- `futex_shared_memfd` (new, registered in `run_tests.sh`): a non-private `FUTEX_WAIT` on one
  mapping of a memfd is woken by `FUTEX_WAKE` through a second mapping, in a thread and across
  `fork`. QEMU passes; the emulator timed out after 5 s (`woken=0`) before the change and passes
  now in the JIT and the interpreter. `run_tests.sh --unit --strict --quick --emu
  build/release-sdl1-gl1/bifrost-emu`: 158 of 158 pass (JIT, 71 s). A first run through the stale
  root `./bifrost-emu` failed only this test. The interpreter and other categories were not run.
- `ManagedServiceBatch.audioTracks` (new): a real `android.media.AudioTrack` plays a stereo
  tone through `createTrack`; the server consumes every frame, the per-channel peaks match the
  written amplitudes (about 250000 and 500000 millionths), and stop, restart with a lowered
  start threshold, pause and flush work. Passes in JIT (4 s) and the interpreter (18 s) as a
  separate probe; the full `ManagedServiceBatch` passes in JIT with 580 checks (89 s).
- Umamusume (JP), 75 s run on the real app: zero `AudioTrack` errors, against about 60 before.
  Not confirmed: audible output, sync, gameplay.
- `GoogleAccountStateTests.test_games_profile_choice_defaults_off_and_reaches_every_root` (no
  emulator): the choice defaults to off and leaves roots untouched, `true`/`false` is written into
  a root's microG preferences with its other entries kept and is idempotent, a fresh root gets a
  file, and a DOCTYPE file and a symlinked preferences file are refused. That the profile is then
  created and sign-in succeeds was not observed (it needs the user's decision and account).
- Not covered: static-buffer tracks, SoundPool, MediaPlayer, ExoPlayer, several tracks at
  once, long-running underrun behavior, timestamps.

## JIT branch flags, immediates and Q pairs (2026-10-20)

Revision: `f0575666` plus the uncommitted change under test.

- `jit_branch_flags` (new): 14 conditions x 16 NZCV values x 3 repetitions, with the flags set by
  MSR in the previous block (EQ/NE/CS/CC/MI/PL/VS/VC test one `pstate` bit); five-branch chains
  (N, Z, V, HI, LT/GE in separate blocks) on CMP flags and a three-branch chain on ADDS flags
  (LS/CS/GT), 12 x 12 value pairs x 3 repetitions, each ending in MRS and checking path and NZCV;
  LDP/STP Q copies of 32..1,024 bytes, four source offsets, destinations that cross a page. Passes
  under QEMU, the JIT, the interpreter and the old build. With the clean-flags bit left set after
  ADDS/SUBS it fails (checked).
- `jit_imm_flags_zext` (new): SUBS/ADDS/CMP/CMN with 0, 1, 7, 255, 4095 and the two 12-bit shifted
  immediates, TST/ANDS with six 64-bit and five 32-bit logical immediates, X and W forms, against 22
  values (garbage in bits 63:32 among them), each with its result, NZCV and the branch taken; 32-bit
  loads and W-form ALU results feeding 64-bit consumers; 64-bit results read through W operations.
  One hash, QEMU's `68b2df78cbaec314`, equal on the JIT, the interpreter and the old build. It
  fails when a 64-bit `subs` result is treated as 32 bits (checked); a first version of it did
  not, because it never read a 64-bit result through a W operation.
- `jit_host_fault` gained `ldp q`/`stp q` probes (second half unmapped, wholly unmapped, a store
  pair) and passes under QEMU, the new JIT and the old one.
- `make verify` (95 fixtures, 8 threaded oracle runs); `run_tests.sh --unit --strict` 155 with the
  JIT, `--no-chain-skip`, `--fwd`, and 147 (8 skipped) under `--no-jit`; Toybox and real-world 65;
  `test_jit_cache`, `test_jit_mt`, `test_jit_mt_publication`, `test_jit_tier2_discovery/feedback/
  measure/mutex`, `test_jit_profiler`; the zstd regression; `make generated-check` (errors=0). The
  build has no new warnings.
- Not run: an ASan build, `--integration`/`--dynamic` (they open windows), any game. Timings are
  in the guide section of this date.

## JIT native LL/SC atomics and the exclusive monitor (2026-10-20)

Revision: `ed3acd04` plus the uncommitted change under test.

- `jit_llsc_native` (new). Single CPU, one hash over 2,492 values, equal under QEMU, the
  JIT and the interpreter: twelve LDXR/STXR forms (every width, acquire/release mixes)
  against a seed table, covering success, no reservation, another address, CLREX, a
  repeated store-exclusive, a plain store of a different value between the pair, a
  replaced reservation, STLR/LDAR of each width, and LDXP/STXP. Four CPUs, exact
  totals: LL/SC counters of 64, 32, 16 and 8 bits, a spinlock from LDAXR/STXR with STLR
  release guarding a plain counter, four cells in one cache line, and an ABA-prone LIFO
  of eight recycled nodes (it fails under QEMU, which compares values only, so only
  the single-CPU hash is a QEMU oracle). Faults: `stxr` to a read-only page, `ldaxr`
  from a PROT_NONE page, `stlr` to a read-only page (one SIGSEGV each, the loop
  completes) and a store-exclusive after a signal (the reservation is gone).
  Breakage checks that fail it: the native version check removed (the LIFO corrupts in
  every run), and the pre-fix `clrex` (the old build fails the hash). It passed twelve
  times in a row, with `BIFROST_NO_CHAIN_SKIP=1`, and as six copies at once (24
  threads on the machine's cores).
- `jit_host_fault` gained `ldxr`, `ldaxr`, `ldaxrb`, `stlr` and `stlrh` probes and passes
  under QEMU, the new JIT and the old one.
- The first lock-free version of the monitor passed everything except this fixture's
  LIFO, which it failed on every multi-core run and passed pinned to one core.
- `make verify` (93 fixtures, 8 threaded oracle runs; `jit_llsc_native` joins
  `jit_host_fault` in running as a threaded oracle only, since replay cannot handle its
  threads and faults); `run_tests.sh --unit --strict` 153 with the JIT, `--no-chain-skip`,
  `--fwd`, and 145 (8 skipped) under `--no-jit`; Toybox and real-world 65;
  `test_jit_cache`, `test_jit_mt`, `test_jit_mt_publication`,
  `test_jit_tier2_discovery/feedback/measure/mutex`, `test_jit_profiler`; the zstd
  regression; `make generated-check` (errors=0). The existing `test_atomic_stress`,
  `test_exclusive_pair`, `test_lse_inline`, `jit_lse_widths` and the pthread tests pass.
- Not run: an ASan build (`make debug` fails on a missing Vulkan video header),
  `--integration`/`--dynamic` (they open windows), any game. Performance numbers
  are in the guide section of this date.

## JIT scalar FP cache and dead NZCV exit stores (2026-10-20)

Revision: the two commits above plus the uncommitted change under test.

- `jit_fp_ldst_cache` (three hashes from QEMU: compiler-generated D/S loops with
  conversions, compares and struct fields; B/H/S/D loads, stores and S/D pairs,
  offset and pre/post-index, into registers preloaded with ones so a missed upper-lane
  clear shows; thirty-two live FP registers) passes under QEMU, the new JIT, the
  old binary (it is a behavior check, not a regression for the old code), the
  interpreter and verify mode. `jit_host_fault` gained `ldr d0/d8/s8/h8` and
  `str d8/s0` probes and passes under QEMU, the new and the old JIT.
- `jit_flags_exit_dead`: 12 values squared, 3 repetitions, ten case groups (fall-through
  and taken successors that overwrite or read the flags, neutral instructions before
  the overwrite, B.HI/B.LS after `adds`, CBZ/TBZ, direct `B`, FCMP/FCMPE) plus a
  warmed block whose successor, on another cache line, is patched to a NOP and back
  with unchanged-word maintenance in between. It passes under QEMU, the JIT, the
  interpreter and verify mode. Breakage checks, each of which fails it: no overlap
  of the dependency range in `invalidate_range`, no word comparison in
  `block_code_unchanged`, and `CSEL`/`CSINC`/`CCMP`/`ADC` treated as flag-neutral.
  Faulting loads before the overwrite are not distinguishable by it (only a signal
  handler could see the flags); `jit_host_fault` is the fixture for fault state.
- `jit_mt_publication` expected the committed flags at an MT self-loop expiry exit for
  `add; add; subs; b.ne`; it now expects the entry flags there (the loop head
  overwrites them) and keeps expecting the committed carry for the `adcs` loop.
  It failed with the first version of the change and passes on `HEAD`'s tree
  and on this one with that expectation (checked against a clean export of `HEAD`).
- `make verify` (93 differential fixtures, 7 threaded oracle runs); `run_tests.sh
  --unit --strict` with the JIT (152), `--no-chain-skip` (152), `--no-jit` (144, 8
  skipped) and `--fwd` (152); `--toybox --realworld --strict` (65); `test_jit_cache`,
  `test_jit_mt`, `test_jit_mt_publication`, `test_jit_tier2_discovery/feedback/measure/
  mutex`, `test_jit_profiler`, `test_fallback_profile` and the zstd regression pass.
  `--integration` and `--dynamic` were not run (they open SDL windows); the
  headless library build (`USE_SDL2=0`) compiled; the ASan debug build did not
  build (`vk_video/vulkan_video_codec_h264std.h` missing, unrelated); no game ran.
- Performance (`HEAD` against the tree, same machine, a game running alongside,
  minimum of five runs): see the guide sections of this date for the numbers and
  what they do and do not show. A seven-percent `atomics` slowdown that the first
  version showed (identical generated code) was the `BlockEntry` size and is gone.

## Google accounts and microG sign-in (2026-10-20)

- `test_managed_accounts_service` (`ManagedAccounts`) drives `AccountManager`
  through the account service: an authenticator in the app's own package adds
  an account, issues tokens (the second `getAuthToken` is the cached one;
  `invalidateAuthToken` forces a new one), answers `hasFeatures` and
  `getAccountsByTypeAndFeatures`, confirms credentials, stores extras as user
  data and is removed; the accounts-updated listener fires; the store file
  exists. A `com.google` account seeded into the shared file is visible to the
  app (visibility granted) and microG's real authenticator, in its own process
  (UID 19998), answers `hasFeatures` for it (`service_ac2dm` true,
  `service_calendar` false), while the app is denied that authenticator's
  secrets (`SecurityException`). A first version failed here with "uid 10000
  cannot get user data": an authenticator serving a Binder transaction read
  `Binder.getCallingUid()` as the session's uid; services here are
  in-process, so the identity is `Process.myUid()`. The test also sends a media
  key through `AudioManager`, which needs the new `media_session` service.
  The no-service failure was seen live (WebView in the sign-in window aborted
  the process), not re-run in the fixture without the service.
- `GoogleAccountStateTests` (no emulator): publishing from one root and
  importing into another carries the accounts and microG's device
  registration but not microG's settings; unchanged roots do not re-import or
  re-publish; `google remove` reaches the next import; an account added in a
  game's root is published; the system app has UID 19998 and `system: true`.
- `test_managed_google_activity_window` (`ManagedGoogleActivity`): an app
  starts microG's `AuthSignInActivity` by component and by its
  `com.google.android.gms.auth.GOOGLE_SIGN_IN` action, then
  `AskPermissionActivity` with an `AccountAuthenticatorResponse` extra. Each
  runs in a window of the system app's session and returns to the app:
  `RESULT_OK` with `errorCode=10` (the Activity's answer to a request without
  its configuration), then `RESULT_CANCELED` with the response cancelled. Between
  the first two, a `:sender` process service hands the app two PendingIntents:
  the one for `AuthSignInActivity` started with `startIntentSenderForResult` gives
  the same `RESULT_OK errorCode=10`, and the one for the app's own Activity is
  refused (`SendIntentException`). It fails on the previous guest code at that
  marker. The test starts a realm broker and runs the app and the Google window
  in it, as Asgard does; 12 s after the first window closes, a `:late` process
  must get a Choreographer frame (`MANAGED_GOOGLE_LATE_FRAME 1`), which the
  previous emulator fails with `DeadObjectException` (without a shared realm the
  stage passes on both, so the broker is what makes it discriminate). The
  session reports the fixture's `Activity` as the caller. Run alone it took
  83 s with the PendingIntent stage (four Play services requests).
  `GoogleAccountStateTests.test_activity_requests_are_validated_staged_and_answered`
  covers the host side: request-path validation (including a symlink out of the
  root), staging, the answer returning with `done` last, and the session options.
- JIT flags (2026-10-20): `jit_flags_overwrite` (CMP/CMN/TST/ADDS, flags read by
  CSET/CSEL/CINC, B.NE and MRS, and flags surviving an intervening load and ADD)
  passes under QEMU, the JIT, the interpreter and verify mode; `make verify`, the
  150 unit tests and the 9 Toybox tests pass with the change.
- JIT pairs (2026-10-20): `jit_host_fault` gained 12 pair probes (second half
  faults, base-register destination, pre- and post-index writeback) and passes
  under QEMU, the new JIT and the old one; with the load-before-write ordering
  reverted it fails. A float pair's first destination is deliberately not
  asserted (QEMU itself updates it before the second half faults). `make verify`
  (90 fixtures + 7 threaded), 150 unit tests, 9 Toybox tests and the real-world
  filter (30 of 31; the failure is `test_sandbox_in_root`, which fails in that batch
  with the old binary too and passes alone).
- JIT widening SIMD (2026-10-20): `native_simd_gen` and `native_simd_alias`
  (26 new aliasing forms, hash `5dd49e555995750b` from QEMU) pass with the inline
  `WidenThree`/`PairLong` bodies, as do `make verify` (90 differential fixtures,
  7 threaded oracle runs) and the 150 unit tests under `run_tests.sh --unit
  --strict` with the JIT. The interpreter run of the unit suite was not repeated
  (the change is JIT-only). `test_sandbox_in_root` failed once when run with the
  three SIMD tests in one invocation and passed alone with the same binary,
  before and after, so it was not attributed to this change.
- `FrameworkStallWatch` (stall reports) and the web links: a 358 s Hungry Shark
  World session with ads and taps on the game's links produced 12 stall reports
  with usable stacks and `APP_ACTIVITY_BROWSER` for its links (live, by hand; not
  an automated test). The Settings no-op was not triggered. A fixture that blocks
  the main thread would automate the stall reports but opens a window.
- `BrowserLinkTests` (no emulator): the host opens only well-formed http(s) links
  (`javascript:`, `file:`, `intent:`, option-like, whitespace, control characters,
  oversize and non-string values are refused), one per interval, with a fake
  launcher, and a missing `xdg-open` is a clear error. The guest side (the
  5-second input window, `market://` mapping, a real browser opening) is not
  automated; the guest build compiles, and the live check is the user's.
  `test_guest_maps_play_install_links_to_the_public_store_page` runs
  `FrameworkLinks` on the host JVM (no emulator; needs a JDK): the live
  `play.google.com/d?id=…&inline=true&enifd=…` ad link and variants map to the
  store's `/store/apps/details?id=` page, and other hosts and paths, a missing or
  malformed `id`, and non-URLs stay unchanged. The rule itself is checked; that
  the browser then opens the store page was not rerun live. The
  Settings no-op (`START_ABORTED`, checked against Android 15's
  `Instrumentation.checkStartActivityResult`) is also untried live.
- `GoogleAccountStateTests.test_activity_relay_never_follows_guest_symlinks` (no
  emulator): a symlinked request file is refused and not staged, a symlinked
  `done` is replaced and its target left intact, a symlinked result file is not
  read, and `../../etc/x` is not accepted as a package name. Found by reviewing
  the relay for a hostile app; not exercised against a real malicious APK.
- `test_managed_parse_cache_skips_manifest_parsing` (`ManagedSplash`): the first
  launch parses the WebView provider and writes cache entries; the second has
  no `Parse times for` line; after every entry is overwritten with garbage the
  third launch parses again and rewrites them. The timing quoted in the runtime
  guide is from three launches of that fixture on one machine, not a benchmark.
  The Play services, accounts, Google window, services and receiver, activity
  stack and splash tests passed with the cache in place. A last full run of the
  suite on the final tree was started and stopped before it finished (it opens
  windows on the desktop), so the suite as a whole was not rerun after the
  cache and the draw-time fix; the earlier full run (42 of 44) predates both.
- `test_managed_view_window_and_surface_lifecycle` (`ManagedViews`) now fades
  its button in with an `AlphaAnimation` and requires its end callback. Without
  the draw-time fix the animation never ends (reproduced first, then fixed). A
  probe inside microG's settings window showed the same state: the animated
  container's ancestors stayed dirty and its animation never ended, while every
  row's view was present, visible, alpha 1 and sized. The click-through itself
  is not automated; the user confirmed the screens render afterwards.
- `test_managed_service_execution` also sends a broadcast to a manifest receiver
  declared for the fixture's `:remote` process and checks it ran in another PID
  with the Intent's extras (`REMOTE_RECEIVER_OK`). Reproduced first: the old
  code threw `UnsupportedOperationException`; with only the throw removed the
  receiver was still lost, because the child's last Service stopped it (found by
  tracing the child's main-thread dispatch).
- The Asgard notice for a game's request was checked once headless offscreen
  (a seeded request showed "<game> is asking you to sign in with Google");
  there is no automated test for it.
- Not covered by an automated test: a completed Google login (needs
  credentials), `LoginActivity` started from `AccountManager.addAccount` and its
  response relay (an attempt to drive it needed synthetic input on the live
  desktop and was dropped), the Asgard dialog beyond headless offscreen runs,
  clicking "Sign in" through the dialog, typing in Google's page after the
  `media_session` fix, and a Google Sign-In button in a real game.
- The whole `test_android_package.py` now has 44 tests: 42 pass (run on the
  final tree: the same two failures; one more test died copying a root when
  the 16 GB `/tmp` filled up, as the suite's roots reach about 12 GB, and
  passes alone — clear `/tmp/bifrost-apk-test-*` after an interrupted run). The two
  failures are the ones recorded below as pre-existing
  (`test_managed_activity_api_report` expects `getAppTasks` to throw;
  `test_managed_dialog_and_popup_windows` never exits, timeout 124); they
  were not re-checked on a pristine checkout this time. No native code
  changed, so the strict suites and `make verify` were not rerun.

## Keyboard text input (2026-10-20)

- `ActivityProbe$InputMethodProbe` (JIT and interpreter) now also binds an
  editor with a fake `IRemoteInputConnection`: the host session is returned
  (`SUCCESS_WITH_IME_SESSION`, a session, no channel), composition and commit
  arrive under session id 0, `invalidateInput` moves later commands to the new
  id, and a non-editor start or `finishInput` ends the session
  (`INPUT_METHOD_HOST_TEXT_OK`).
- `test_managed_text_input` in `test_android_package.py` runs a real
  `EditText` Activity with the built-in window and `BIFROST_TEXT_SCRIPT`, so
  the events take the whole path (SDL queue, backend, surface manager, op 3,
  launcher, IME session, `RemoteInputConnectionImpl`, `TextView`). `H` pressed
  with its text types `h` once (the key is dropped, not doubled); `é` commits;
  `ni` composes with an underline at position 2 and `你` replaces it;
  Backspace, a key event, deletes it through `TextKeyListener`; `!` follows.
  The same run asserts the virtual keyboard is `FULL` US QWERTY. Before the key
  map change the Backspace did nothing (the empty map made `TextKeyListener`
  pick `NullKeyListener`); that failing run was observed. The double-typing
  guard was not shown failing without it. The test ran with the default JIT
  engine only, not the interpreter or Tier-2.
- `make test-android-window-plugin` (`android_window_plugin_host`) covers the
  plugin contract: no text before it starts, an ordered script of text and key
  events with payloads, `TEXT` without a start, a bad kind and an overlong
  payload as `-EPROTO`, a failed start, and a descriptor without the callbacks
  as `-ENOTSUP`.
- `python3 Tools/Android/generate-key-layout.py --check` reports the committed
  table current (it needs the network). The strict suites pass 334/334 with the
  JIT and 325/325 with the interpreter (9 skips), `make verify` and
  `make generated-check` pass, and the headless configuration compiles.
- The whole of `test_android_package.py` passes 36 of 38 tests. The two
  failures are not from this work: both fail identically on a pristine HEAD
  checkout built and run alongside. `test_managed_activity_api_report` expects
  `ActivityManager.getAppTasks()` to throw in trace mode, but it has been
  implemented since `aa1272b3`; `test_managed_dialog_and_popup_windows` runs
  every marker through `APP_ACTIVITY_DESTROYED` and `APP_POPUP_REMOVED 7`, then
  the process never exits and the 90 s timeout kills it (exit 124).
- Roblox: the user tried typing in it (login and chat were suggested) and
  reported that it works; no finer detail (which fields, accents, composition)
  was recorded, and the log was not inspected.
- Not tested: a WebView (Chromium) editor, the interpreter and Tier-2 engines
  for the Activity test, other host keyboard layouts or input methods (IBus,
  Fcitx), and the Wayland/X11 differences in SDL's text events.

## Roblox audio output (2026-10-20)

`Tests/Unit/android_aaudio_thread.c` now resolves every name of Android 15's
`libaaudio.map.txt` (70) with `dlsym`, checks the hardware/content-type
getters, the MMAP policy rules and `AAudioStream_read` on an output stream,
and opens a stream with a 32768-frame capacity request (FMOD's), which must
report at least that capacity; a 2^20-frame request still answers
`UNAVAILABLE`. The AAudio section of `test_android_bionic.sh` (run from a copy
outside the repository, not the whole script) passes in JIT, interpreter and
Tier-2. With the previous export list the fixture cannot link (`cannot locate
symbol "AAudioStream_getHardwareSampleRate"`); the capacity check was not run
against the old binary, the previous behavior being the `-889` that
`BIFROST_AUDIO_TRACE` showed for Roblox's 0x8000 request.

`test_audio_callbacks.sh` passes (JIT and interpreter, normal and delayed host
waits, 5 runs each). The strict suite passes 334/334 with the JIT (230 s) and
325/325 with the interpreter (9 skips, 106 s); `make generated-check` passes.
`make verify`, the headless build and the other Android scripts were not run.

Roblox itself, with the JIT and all optimizations on: the trace shows FMOD's
probe stream, then its reopen with `setBufferCapacityInFrames(0x8000)`
succeeding (capacity 32768), `requestStart`, state STARTED, and no
`FMOD_RESULT:51`; before the change that `openStream` returned `-889` and
`System::init` failed. The user confirmed audio plays and reports roughly
20-40 fps in a baseplate experience with a few characters (not measured here).
The server still disconnects the client with reason 304 about a minute after
it joins; the client's Play Integrity request fails as before.

## Roblox from startup to its login screen (2026-10-19)

Each fix below has a focused regression that fails on the previous code:

- `test_signal_semantics` (both engines, and under qemu-aarch64) adds a
  process-directed `kill()` that every thread blocks: `sigpending` reports it
  and another thread's `sigtimedwait` receives it, sent from the main thread
  and from a sibling. The previous emulator timed out (`-5`). A second case
  runs three `udf #0` in a worker with a SIGILL handler that skips them; the
  thread must continue. Previously the worker vanished and its join hung.
- `test_lse_inline` adds CASP/CASPL (X pairs, match and mismatch), CASPA on W
  pairs with garbage upper bits, and a four-thread CASPAL 128-bit counter.
  The previous decoder raised SIGILL.
- `test_sdl_gl_mapbuffer` was rewritten to valid GL only and also builds
  natively (`cc ... -ldl`); its 26 checks pass on the host's Mesa 26.2 and in
  both engines with the GL and GLES rows. It covers write-back to immutable
  `glBufferStorage`, mapping-relative flushes, persistent coherent writes seen
  while mapped, unmap ending a persistent mapping, and the host's errors for
  invalid maps. The previous mapping code fails at the relative flush.
- `test_managed_activity_stack` adds `startIntentSenderForResult`: a sender
  this process did not create throws `SendIntentException`, and the app's own
  PendingIntent returns its result (`RESULT 44 0 null`). This fixture's fixed
  1.5 s layout wait failed once while another game was running and passed on
  the rerun.
- `glxmlcheck` rejects GL rows that drop stack-passed arguments; it flags the
  old `glBlitFramebuffer` and the `glBlitFramebufferEXT` row it found.
- The `Managed` fixture checks that `AudioManager.isBluetoothScoOn()` and
  `isBluetoothA2dpOn()` are false (`MANAGED_FIXTURE_BLUETOOTH_ROUTES_OK`).

The strict suite passes 334/334 with the JIT and 325/325 (9 skips) with the
interpreter; `make verify` and `make generated-check` pass.

Roblox itself, with the JIT and all optimizations on: its main Activity
resumes, the GLES 3.2 fallback renders the UI (sampled back-buffer pixels
`#111216`/`#272930` instead of black; the user confirmed the UI), and it
reaches the login screen; the user logged in and joined live servers. About
170 s after joining, the server disconnects the client with reason 304
("missing or corrupted files"); the client logs no file or hash failure, and
its Play Integrity token request fails ("Standard Integrity API is not
available"). That attestation gate is not emulator behavior and is not worked
around. Vulkan finds no device (no Android Vulkan HAL), so Roblox uses GLES.
Most 3D material vertex shaders (`DefaultUnified*VS`, terrain, grass) fail to
compile on Mesa ("could not implicitly convert operands"); in-experience
rendering is not established.

## Splash hand-off and installer launchers (2026-10-19)

`test_managed_splash_hands_off_to_main_activity` launches `ManagedSplash`. Its
launcher Activity wraps its base context, starts `Main` and finishes at once.
The app must keep running in `Main`; markers must show the splash finishing,
`Main` resuming, then both being destroyed, with no fatal exception.
`ApkInspectionTests` checks two installer cases. The first is
`apksigner` output with v3.1 rotated signers: only the API 35 signer is
recorded. The second is a launcher declared only by an enabled
`activity-alias`. `test_android_package.py` passes 35 of 37, with the same
two failures as the checkpoint below. Roblox itself reaches its main Activity,
then its main thread blocks; that hang is not covered here.

## TextureView native-buffer images (2026-10-19)

`test_managed_hardware_canvas` now shows a TextureView. It reads back two
producer frames through `TextureView.getBitmap`:

- a `lockCanvas` RGBA frame: (200, 40, 90) exactly;
- an `ImageWriter` YUV_420_888 frame (Y=81, U=90, V=240): (238, 14, 14).

`ImageWriter` tags YUV frames JFIF (libui `PublicFormat.cpp`), so full-range
BT.601 is the expected result. With the previous adapter HWUI aborted on the
first frame ("Could not create EGL image, err = (0x300c)", then
`Invalid GrBackendTexture`), as the Fyber ad did in Hungry Shark World.

The strict JIT suite passes 334/334. `EglProbe` passes. `make generated-check`
passes. The headless build compiles. `test_android_package.py` passes 35 of
37, with the same two failures as the checkpoint below. Decoder output
(Codec2 into a TextureView) and the game's ad were not run.

## Hardware window layer release (2026-10-19)

`ActivityProbe$EglProbe` prepares an ImageReader surface and replays HWUI's
`setSurface` window handling. HWUI looks up `ANativeWindow_fromSurface` in
libandroid and releases the window through libnativewindow. The probe then
checks that releasing the preparation removes the host backing
(`ANDROID_SURFACE_RELEASE_OK`). It passes with the fixed adapter and fails
with the previous one ("host layer outlived its window"). A gdb trace of the
host refcount during `test_managed_activity_stack` showed every GPU window
layer reaching zero and hidden only with the fix. Hungry Shark World's
rewarded ad now returns to gameplay on close (user-confirmed).
`test_android_package.py` passes 35 of 37. Both failures also occur with the
previous adapter:

- `test_managed_activity_api_report` still expects
  `ActivityManager.getAppTasks` to be unsupported.
- `test_managed_dialog_and_popup_windows` passes (31 s, 2026-10-21). The
  earlier "never exits (timeout 124)" was the fixture: it started the `Engine`
  Activity and finished only the root, so `Engine` stayed in the task and the
  app correctly kept running (a thread dump showed the main Looper idle and no
  blocked thread). The fixture now finishes `Engine`, then the root. It also
  shows a text toast, and the test asserts the `type=2005` window,
  `APP_TOAST_SHOWN` and the seven popup layers.

```bash
python3 Scripts/Android/test_android_framework.py --services --managed-graphics \
  --probe build/android-managed-launcher/classes.dex --main-class 'ActivityProbe$EglProbe'
```

## Socket interface ioctls (2026-10-17)

`socket_msg_layout` adds `struct ifreq` queries on an `AF_INET` socket:
loopback by `SIOCGIFINDEX`, `if_nametoindex`, `SIOCGIFFLAGS` and
`SIOCGIFNAME`, and `ENODEV` for an unknown name. It passes in both engines
and under qemu-aarch64; the previous emulator fails at `SIOCGIFINDEX`
(`-ENOTTY`). Strict suites pass JIT 333/333 and interpreter 324 (9 skipped).
The `Managed` fixture's `NetworkInterface` loopback check passes. Ad delivery
in Hungry Shark World is not established by these checks.

```bash
make cross SRC=Tests/Unit/socket_msg_layout.c OUT=Tests/Unit/socket_msg_layout.elf
```

## GPU-rendered Activity windows (2026-10-17)

`test_managed_hardware_canvas` draws 30 frames with `lockHardwareCanvas` on a
SurfaceView (HWUI RenderThread, Skia GL and EGL over the thunk) and animates
the Activity window's label. The window must report `APP_WINDOW_GPU` and at
least 10 frames through `Window.addOnFrameMetricsAvailableListener`, which
counts only frames HWUI swapped; the run reported 30 of 30. Before the
fixes it died with SIGSEGV at 0xdd0 in libGLESv1_CM's `glGetString`, then
aborted in HWUI's `DeviceInfo`, then logged `EGL_BAD_DISPLAY` from
`eglBeginFrame` every frame (found with `debug.egl.callstack` in a kept
fixture root's `debug_ramdisk/adb_debug.prop`). The view, dialog and
activity-stack tests now expect `APP_WINDOW_GPU` for Activity windows.
Frame metrics prove HWUI swapped; no host-pixel readback checks the layer
content, and the game's ad window is not yet observed on this path.

## microG processes and remote providers (2026-10-17)

`test_managed_play_services_available` now binds
`com.google.android.gms.measurement.START` by action, which starts GmsCore in
its own process (uid 19998, `ASGARD_PROCESS_READY`). It then receives
microG's `MeasurementService` broker Binder alive, queries
`content://com.google.android.gsf.gservices` (microG accepts the fixture
because it requests `READ_GSERVICES`) and runs basement's chimera
`serviceIntentCall`, which resolves to `MeasurementService`. Before the change
the bind returned false and the chimera acquisition threw
`activity.getContentProvider`, as in SpongeBob's crash.

The full `PackageTests` class passes 13/13 in its final run. In an earlier run,
`test_managed_launcher_persists_data_and_finishes_activity` timed out in its
180 s interpreter start, inside the WebView APK parse before any changed
code. It timed out the same way with the committed launcher, so it is a
timing margin, not a regression. `test_android_framework.py --services
--microg --main-class ActivityProbe --probe build/android-managed-launcher/classes.dex`
reaches `ACTIVITY_MICROG_PACKAGES_OK` and then the known `checkSensorService`
failure. No native code changed, so the strict suites and `make verify` were not rerun.

Game: two 180 s JIT SpongeBob runs on the saved child-age profile reached Unity
tracker setup with no fatal exception. The second run, after the signature-grant and child
forwarding fixes, had no FirebaseInstanceId "Play services missing" error and
no exceptions in microG's child processes. The adult-age path, microG
broadcasts and account sign-in are untested.

## microG Play services packages (2026-10-16)

`test_managed_play_services_available` builds a fixture APK with Google's
`play-services-basement` 18.12.0 and runs `GoogleApiAvailabilityLight`
in a managed launch: status 0 (SUCCESS) with apk version 252432034. With
microG provisioning removed from `prepare_root` the same fixture logs
"requires the Google Play Store, but it is missing" and returns 9
(SERVICE_INVALID), as SpongeBob's logs do. `test_android_framework.py
--services --microg --main-class ActivityProbe` reaches
`ACTIVITY_MICROG_PACKAGES_OK` (spoofed `signatures`/`signingInfo`, UID
lookups, system flag through the real PackageManager client).

The full `test_android_package.py` suite passes 33/33. Eager parsing at
startup had failed two launches: the interpreter-mode persistence start
exceeded its 180 s bound. Lazy loading fixed it. The `ActivityProbe`
services probe fails with or without microG, and with the committed
launcher, at `checkSensorService`: `registerReceiver` from the system
context is rejected as a foreign receiver package. That failure is
unrelated and not fixed here. No game was run.

```bash
JAVAC=/usr/lib/jvm/java-25-openjdk/bin/javac python3 Scripts/Android/test_android_package.py \
  --emu $PWD/bifrost-emu --plugin $PWD/build/release-sdl1-gl1/plugins/android-window-sdl.so \
  PackageTests.test_managed_play_services_available
```

## Forced synchronous faults (2026-10-16)

`test_group_exit` adds a blocked SIGSEGV with a handler installed, an
ignored SIGSEGV and a blocked SIGILL (`udf #0`); each must kill the child by
that signal. They pass on qemu-aarch64 and in both engines; the previous
source fails all three. Strict suites pass JIT 333/333 and interpreter 324
with nine expected skips; `make verify` passes 90 differential fixtures and
seven threaded oracles. No game was run on this change.

## BRK SIGTRAP (2026-10-16)

`test_group_exit` adds three aarch64 cases: a worker's default BRK ends the
process by SIGTRAP; a SA_SIGINFO handler sees `TRAP_BRKPT` with `si_addr`
and the saved pc at the `brk #0x123` encoding and resumes past it; a blocked
SIGTRAP still terminates. They pass on qemu-aarch64 and in both engines.
Rebuilding with the previous `BranchSystem.cpp` fails all three. Strict
suites pass JIT 333/333 and interpreter 324 with nine expected skips; `make
verify` passes 90 differential fixtures and seven threaded oracles. Only
BRK used `force_signal` at this checkpoint (superseded above).

## Rootfs cwd, signal deaths, thread names and input times (2026-10-16)

`test_sandbox_in_root` runs a copy of `test_sandbox.elf` from inside the
scratch root while launched from the repository: the guest starts at `/`,
its relative create lands in the root, and `run_tests.sh` checks that no
`cwd-canary` reached the launch directory. A manual run with a relative
`BIFROST_ROOT=../root` passed in both engines. `test_group_exit` now
requires `WIFSIGNALED` deaths and adds a default-disposition SIGSEGV from a
`PROT_NONE` store. `test_prctl` adds eight per-thread name checks
(inheritance, leader isolation, `/proc/self/comm`, `/proc/thread-self/comm`,
task comm read/rename, unknown TID). Both fixtures pass on qemu-aarch64
(host kernel; `test_prctl`'s existing basename check fails there because
qemu's comm is `qemu-aarch64`). `test-android-window-plugin` checks that a
1234 ms plugin pointer event reads back as 1,234,000,000 ns from the NDK
getters.

Strict suites pass JIT 333/333 and interpreter 324 with nine expected
skips. `make verify` passes 90 differential fixtures and seven threaded
oracles; `make generated-check` and the headless build pass. The new checks
were not run against the pre-change binary. No game or managed app was run,
so touch gestures and the Android cwd change are untested in real apps.

## Inline Select, Dup, ADDV, MLA and FRINT (2026-10-16)

`jit_native_simd_gen` runs all 1,480 encodings of the native SIMD rows,
including every new inline form, against QEMU hashes. Swapping the BSL
selector made two encodings fail, which confirms the test covers the
bodies. `jit_native_simd_alias` adds 29 forms whose Vd aliases Vn or Vm
(the generated test uses distinct registers), against a QEMU hash. Both
pass on the JIT with and without the vector cache, on the interpreter and
under `BIFROST_JIT_VERIFY=1`. `jit_native_simd_inline`,
`jit_simd_widen_reduce` and `jit_simd_fp_lanes` pass unchanged. `make
verify` passes 90 differential fixtures and seven threaded oracles. Strict
suites pass JIT 329/329 and interpreter 320 with nine expected skips. The
headless build passes the alias fixture. Hosts without SSE4.1 keep the
helper for 32-bit MLA and FRINT; that path was not run here.

## Vector FMAX/FMIN signed zeros (2026-10-16)

`jit_simd_fminmax_zero` checks FMAX/FMIN/FMAXNM/FMINNM in `.4s`, `.2s`
(inactive lanes hold NaNs) and `.2d` over every pair of signed zeros, ±1,
subnormals, infinities and quiet/signalling NaNs: 3,456 lanes against an
Arm ARM reference. The reference passes on qemu-aarch64. The fixture passes
on the JIT with the vector cache (cached path) and with
`BIFROST_NO_VEC_CACHE=1` (uncached path), on the interpreter, and with
`BIFROST_JIT_VERIFY=1`. `make verify` passes 89 differential fixtures and
seven threaded oracles. Strict suites pass JIT 328/328 and interpreter 319
with nine expected skips. The clamp micro-benchmark is not a game frame-time
measurement.

## Batched DONTUNMAP protection (2026-10-16)

`test_userfaultfd` adds a read-only DONTUNMAP move: contents arrive intact,
both ranges keep read-only permissions (a forked writer faults) and the
source reads zero. It passes on the host kernel (84 checks) and in both
engines (138 checks). A sparse 512 MiB MREMAP_FIXED|DONTUNMAP move takes
about 240 ms before and 29 ms after (three runs each, `BIFROST_NO_ASLR=1`).
The read/write case now makes no protection calls; the old loop made four
per page. This count comes from reading the code: perf syscall tracepoints
were unavailable. A 60-second SpongeBob profile showed no remap work among
the hot functions; kernel time and GC pause durations were not captured.

A guest killed by a fatal signal reports `exit(139)` to its parent instead
of a signalled wait status. The regression's child therefore reports the
SIGSEGV through its own handler.

## Cold-path vector validity (2026-10-16)

`jit_vector_cold_step` fails on the unfixed allocator commit (`9309076`) and
passes on the fixed JIT, the interpreter and qemu-aarch64. `make verify`
passes 88 differential fixtures and seven threaded oracles. The strict JIT
suite passes 327/327, including `native_simd_inline` (137,700 checks) and
`jit_simd_fp_nan`. The strict interpreter suite passes 318 with nine expected
JIT-only skips. A manual SpongeBob session on the fixed build had working
touch input and no sprite corruption. That observation does not establish
other games or long-session playability.

## Allocator effects and region demand layouts (2026-10-16)

The expanded discovery fixture compares all vector bits against the
interpreter for demand-only full overwrites, mixed scalar/vector upper lanes,
NaN recovery, aliased inputs, helper boundaries and precise pre-load fault
state. Integer SIMD pressure now checks cached Tier-2 regions with 25 vectors.
An early-exit region reads demand entry values twice before late definitions;
its second trip detects stale loop-carried homes. Linking a temporary object
with only the entry-dirtiness fix removed fails at `region=1 n=2`; the fixed
compiler passes. All four ST/MT and ordinary/skip-chain configurations pass.
Separate ST/MT runs pass with vector caching disabled, FMA3 disabled and
software memory guards forced; AVX2-disabled runs pass all four modes.

`test_jit_profiler.sh` checks real compiled output and nonzero guest provenance
through translation/SSA as well as the existing per-thread sampler ranges.
The opt-in allocation TSV distinguishes measured IR deltas from range-only
entry/exit/recovery rows. `make verify` passes 87 differential fixtures and
seven threaded oracles. Full strict suites pass JIT 326/326 and interpreter
317/317, with nine expected JIT-only skips. Generated checks and separate
desktop SDL/GL and headless executable/library builds pass. The final
schema and focused regression refinements were rechecked with their focused
tests; they do not change guest arithmetic semantics.

Five alternating measured one-million-call Unity-block replays, after one
warm-up per executable, have median thread CPU times 91.012 ms before and
63.127 ms after. Complete vector, NZCV and output-memory dumps have the same
SHA-256 as the independent QEMU oracle. Emitted bytes fall 3,128 to 1,463;
tracked vector spills/reloads fall 27/99 to 8/15, including cold code.
This replays the saved block with controlled inputs, not a live outfit action.

The earlier 14-accumulator loops were rebuilt and independently passed QEMU:
FADD medians 36.074/36.431 ms before/after, FMLA 37.207/37.143 ms. Five
alternating measured samples follow warm-up. Three measured alternating
CoreMark pairs at 110,000 iterations validate all five CRCs, with median
scores 9,971.897/9,911.696 iterations/s. Five measured zstd compression
pairs on a deterministic 16 MiB input take median process CPU
177.342/179.026 ms; each decompression round trip matches SHA-256. Compile
time and measurement limits are in the [JIT guide](JIT.md#allocation-costs-mixed-fp-and-region-layouts-2026-10-16).
Temporary harnesses, logs, binaries and JSON results remain under `/tmp`.
Actual gameplay and hitch reduction on this executable are not established.

## Scoped interpreter profile reasons (2026-10-16)

`bash Scripts/Testing/test_fallback_profile.sh` passes 384 distinct inventory
rows and 768,000 counts, with concurrent writers, separate reasons at the same
PC, nested scopes and exception restoration. The script includes the current
`Source/` header root. A real JIT run of `jit_simd_fp_nan.elf` also records 25
`fp-special` steps and three `general` steps, checking the generated recovery
bridge rather than only the standalone inventory. These are execution counts,
not elapsed-time attribution or game frame-pacing evidence.

## Cached integer SIMD (2026-10-15)

The discovery host fixture now also checks 253 supported arithmetic,
comparison and immediate-shift forms across element sizes and Q widths.
Destination aliases cover neither input, Vn, Vm and identical binary inputs.
Thirteen resident vectors with twelve dirty accumulators exercise high XMM
registers and assert that only the thirteen prologue reloads are emitted.
Twenty-five vectors exercise Tier-1
evictions and Tier-2's retained uncached pressure path. One and seven loop
iterations compare every vector and architectural NZCV with the interpreter.
All four ST/MT and ordinary/skip-chain configurations pass. Existing pressure,
NaN, helper-boundary, early-exit, precise-fault and invalidation checks pass.
Separate vector-cache-disabled, FMA3-disabled, AVX2-disabled and forced
software-guard runs of the same host fixture also pass.

`make verify TARGET=build/release-sdl1-gl1/bifrost-emu` passes 87 differential
fixtures and seven threaded oracles. The full strict JIT suite passes 326/326;
the focused SIMD/NEON/fault/image selection passes 43/43 in JIT and 42/42 in
the interpreter with one expected skip. The full interpreter suite was not
repeated. Independent QEMU runs pass `jit_neon_cmp`, `jit_neon_roundingshift`,
`jit_simd_mul32` and `jit_neon_pngfix`. `make generated-check` passes, and
separate SDL/GL and headless executable/library builds succeed.

A temporary harness executes the actual 31-instruction Unity block from the
SpongeBob capture. Its complete vector outputs, architectural NZCV and written
memory match QEMU and the interpreter before and after the change. Internal
JIT `pstate` bit 27 is excluded from the architectural NZCV comparison. The
[JIT guide](JIT.md#cached-integer-simd-2026-10-15) records generated-code and
timing results with their scope. The already-running SpongeBob session was
preserved on its original executable; hat-change latency on the new binary
has not been measured.

```sh
bash Scripts/Testing/test_jit_tier2_discovery.sh
make verify TARGET=build/release-sdl1-gl1/bifrost-emu
BIFROST_EMU=build/release-sdl1-gl1/bifrost-emu BIFROST_IR_VALIDATE=1 \
  BIFROST_REGALLOC_CHECK=1 bash Scripts/Testing/run_tests.sh --strict \
  --filter 'simd|neon|round|host_fault|userfaultfd|window_bounds|memory_guard|jpeg|png'
```

## Tier-1 vector allocation under pressure (2026-10-15)

The existing Tier-2 discovery host fixture now also directly compiles
Tier-1 pressure loops: fourteen accumulators used twice, and twenty-eight
used once, with a shared source. It requires active partial allocation and
real evictions, then compares every vector with the interpreter for 1, 7
and 257 iterations, finite/NaN inputs and FPCR.DN. Variants insert a native
FP helper, high-XMM scratch code, an interpreter fallback or a scalar FP
operation into the vector block. A scalar fault guard precedes later XMM
reuse and checks precise pre-fault vector state. The existing region
checks continue to verify cache admission, side exits, aliases, NaN steps,
invalidation and region layouts.

All four ST/MT and ordinary/skip-chain configurations pass. Additional
cache-disabled, FMA3-disabled and forced-software-guard runs pass. The
broader SIMD/FP/fault selection passes 41/41 with IR and scalar allocator
validation. `make verify TARGET=build/release-sdl1-gl1/bifrost-emu` passes
87 differential fixtures and seven threaded result oracles, and
`make generated-check` passes. Independent QEMU runs validate both temporary
pressure benchmarks; [JIT.md](JIT.md#tier-1-vector-allocation-under-pressure-2026-10-15)
records the FMA gain and the remaining FADD regression explicitly.
The final strict suite passes 326/326 in JIT and 317/317 in the interpreter
with nine expected skips. SDL/GL executable/library and separate headless
executable/library builds pass. `make nativegen-expect` reproduces the
committed expectations without generated-file changes.

An installed Azur Lane run on the final allocator reaches Unity initialization
and ends at the configured timeout (status 124), without a logged fatal guest
exception. Host GPU access was available. Evidence: managed session
`8a5f9689f0a14c74b6065bebb2f57960`. This does not establish continued rendering,
login, gameplay or a change in outfit-repacking/GC pause time. Running
SpongeBob and other existing game processes were preserved.

```sh
bash Scripts/Testing/test_jit_tier2_discovery.sh
BIFROST_EMU=build/release-sdl1-gl1/bifrost-emu BIFROST_IR_VALIDATE=1 \
  BIFROST_REGALLOC_CHECK=1 bash Scripts/Testing/run_tests.sh --strict \
  --filter 'simd|fp_|round|host_fault|userfaultfd|window_bounds|memory_guard'
make verify TARGET=build/release-sdl1-gl1/bifrost-emu
```

## Inline pixel-conversion helpers (2026-10-15)

`jit_native_simd_inline` passes 137,700 checks under QEMU, JIT and interpreter.
It covers every binary16 bit pattern with FPCR.DN clear/set, both source
halves and aliased FCVTL destinations; signed/unsigned widening shifts,
SHLL/SHRN and FP narrowing aliases; and thirteen-vector loops with NaN
recovery before this iteration's accumulator writes. Its `--mt` mode starts
a second guest CPU before the checks. Cache-off, AVX2-off, FMA3-off,
ordinary chaining, MT and Tier-2-enabled runs also pass. The generated SIMD
test passes all 1,480 encodings; `make nativegen-expect` reproduced the
committed QEMU expectations without changing the spec or generated files.

The Tier-2 discovery oracle directly compiles widening/conversion regions,
asserts cache activation, and compares every vector with the interpreter
across early exits, aliases and NaN recovery. All four ST/MT and
ordinary/skip-chain configurations pass. A temporary build forcing F16C
detection off passes the 137,700-check guest fixture, exercising the retained
helper path. Running under `qemu-x86_64 -cpu Haswell,-f16c` was rejected by
the host's x86-64-v3 glibc before startup, so it supplies no compatibility
evidence. Actual execution on older hardware is untested.

The SDL/GL executable/library and separate headless executable/library build;
`make generated-check` and `make verify TARGET=build/release-sdl1-gl1/bifrost-emu`
pass (87 differential fixtures and seven threaded oracle runs). The broader
SIMD/FP/memory-recovery selection passes 42/42 in JIT and 41/41 in the
interpreter with one JIT-only skip. Synthetic performance observations and
their bounds are recorded in [JIT.md](JIT.md#inline-pixel-conversion-helpers-2026-10-15).
The full strict JIT suite also passes 326/326, including its real-world
binaries and dynamic-linker checks; the complete interpreter suite was not
repeated for this change.

An installed Azur Lane run using the rebuilt SDL/GL binary reached Unity
initialization with host GPU access available and ended with the runner's
intentional status 124. No fatal guest exception was found in the session
log. This is bounded startup stress, not verified continued rendering,
login or gameplay. Evidence: session
`3374705daa904b5881c9887dbbfb20b3` under the installed package's managed
sessions directory. The already-running SpongeBob process was preserved;
outfit-change timing on the new executable remains unmeasured.

```sh
make cross SRC=Tests/Unit/jit_native_simd_inline.c OUT=Tests/Unit/jit_native_simd_inline.elf
qemu-aarch64 Tests/Unit/jit_native_simd_inline.elf
BIFROST_EMU=build/release-sdl1-gl1/bifrost-emu bash Scripts/Testing/run_tests.sh \
  --strict --filter 'simd|fp_|round|host_fault|userfaultfd|window_bounds|memory_guard|jpeg|png'
bash Scripts/Testing/test_jit_tier2_discovery.sh
```

## Polynomial instructions and Wi-Fi display routing (2026-10-15)

`jit_simd_pmull` now checks 10,240 cases: byte/wide PMULL and PMULL2,
distinct and identical sources, destination aliases and varied lane contents.
`jit_crc32` checks 8,192 cases across both CRC polynomials, all four input
widths, register aliases, identical sources, nonzero upper accumulator bits
and all NZCV combinations. Both pass QEMU, JIT, interpreter and Tier-2 runs.
JIT fallback inventories record no interpreted instructions for either fixture.
A temporary emulator built with SSE4.2 detection disabled also passes both,
covering the portable CRC backend. The distribution's x86-64-v2 glibc prevents
masking SSE4.2 with qemu-x86_64 before startup, so that method was not used as
evidence. This does not validate execution on an actual older x86-64 machine.

`FrameworkDisplayProbe` passes 47 checks in each engine through the provisioned
Android framework. It checks unavailable Wi-Fi display status, fresh replies,
privileged/unprivileged controls and the real local MediaRouter initialization,
as well as trusted/untrusted client state, package/user rejection and unregister.
It retains resize, display-listener and window-context coverage. Remote Binder
death delivery and wireless display hardware are not covered.

Requested-orientation regression checks add portrait/landscape, reverse rotation,
locked and sensor-axis policy, and release to the display contract; the focused
contract passes in both JIT and interpreter modes. `test-android-sdl-backend`
checks fixed and resizable panel rotation, invalid requests, aspect-setting
failure and release, alongside its 256 teardown cycles. Its mock exports include
the SDL functions required by task visibility and text input; input timestamps
are checked against host monotonic time. SDL/GL and separate headless builds
pass. Uma Global advanced past its cover art in the approved live verification,
as confirmed by the user; full gameplay and successful Google sign-in were not
tested by these checks.

Persistent app property coverage in `LauncherArgumentTests` checks CLI save,
query/reset paths and rejects malformed names, injected lines and oversized
UTF-8 values. `SettingsIdentityTests` exercises the managed root preparation
and inventory path with a small framework fixture: overrides survive launcher
replacement, reset restores the image default, app data survives, and manual
property-file edits fail inventory validation. This test mocks framework/microG
installation and graphics adapter patching; it does not launch a guest app.
The existing real signed APK identity test also verifies explicit downgrade,
default rejection, signer mismatch rejection with the flag, same-version
reinstall and subsequent upgrade, retaining native/managed app data and settings.
`RunFromFileTests` checks downgrade option forwarding to installation without
passing the option to the game runtime.
All 14 focused tests pass. Uma's actual installed managed root was then
reprovisioned headlessly with its saved `property.ro.kernel.qemu=0` setting;
the generated property read back as `0` and a second preparation accepted
the cached runtime inventory. The game was not relaunched for this check.

`LauncherProfileTests` instantiates the real Qt profile editor using the offscreen
platform. It verifies the CLI-compatible save format, preservation of custom
and empty property values, staged default restoration and cancellation, rejection
of duplicate/injected properties, and disabled device controls for native-only
apps. These three tests, the eight `LauncherArgumentTests`, three settings identity
tests and two run-from-file tests pass (16 total). An offscreen
render of Uma's actual saved profile confirms that its emulator flag is Off and
an unchanged save preserves its saved values. No desktop window or game was
opened; interactive desktop use remains unverified by these checks.

Startup visibility regression: Uma session `9a8317263cfb4941978b8b73d1625355`
remained in preparation for about 108 seconds, with `Compiling ... (speed-profile)`
in its preparation log and no guest output. The drawer previously cached the
unchanged log size and displayed a frozen elapsed time. The focused
`SessionResultTests` now verifies that the refresh key advances without output,
phase changes refresh immediately, preparation logs are selected until guest
output appears, old session records still find their preparation log, and
finished-session clocks remain stable. Session and launcher argument tests pass
(11 tests); no game restart or live desktop interaction was performed for this
visibility change.

ART cache scope regression (`DexoptCacheTests` and the managed-root settings
fixture): launcher, microG, media and unrelated property changes reuse compiled
output; APK, framework/ART image, boot classpath, ART properties, reference profile
and filter changes invalidate it. Missing/tampered artifacts regenerate, legacy
records migrate only with a matching original key, unchanged failed compilations
are not repeated, and generated code survives runtime replacement without
entering the immutable package inventory. The compiler is mocked in these
focused tests; 18 cache, settings, launcher-argument and session tests pass.

A separate temporary headless app was compiled with the real guest `dex2oat64`,
then moved to a freshly copied framework root with a changed launcher identity,
media mask and `ro.kernel.qemu=0`. Cache hits were checked with compiler calls
forbidden. The real ART runtime mapped the retained `base.odex` and ran its
Java main successfully in both JIT and interpreter modes, for both `verify` and
`speed-profile` compilation (a generated full profile). The probe exits explicitly
after checking ART's mapping; it does not exercise Activity shutdown or game
playability. Uma's existing speed-profile cache also matched the migration
criteria in a read-only check; the running game was not restarted or modified.

Final broad checks: the SDL/GL build and library, separate headless build,
`make generated-check`, and `make verify` pass (87 differential fixtures and
seven threaded oracle runs). The strict JIT suite passes 326/326; the strict
interpreter suite passes 317/317 with nine suite skips. The headless executable
also passes both polynomial fixtures, including interpreter CRC execution.

The live Azur Lane captures use the installed game, GPU access, boundary traces
and per-process fallback inventories. The final profile has 9,451 samples with
zero loss; see [JIT measurement limits](JIT.md#azur-lane-polynomial-instructions-2026-10-15).
The native profiling launch used the installed managed root with a temporary
read-only launcher DEX and a native host DNS bridge, without the normal Python
session/process supervisor. Cross-process WebView behavior is therefore outside
this run's coverage. Bounded stops are not crash or gameplay evidence.

## Desktop Android library and Surface identity fixture (2026-10-14)

`test_android_preflight.py` passes four tests, including dense daytime launcher
bitmap selection and an adaptive resource's own fallback. Nine selected package
tests cover launch arguments, malformed/saved launcher settings, bounded icon
copying, older install metadata, session results, persistent Settings identity
and the actual Surface adapter. Three signed-package tests pass install/launch,
rollback and signer/upgrade checks; the native launch case includes all six
JIT/interpreter/Tier-2 × built-in/plugin Wayland combinations with GPU access.

```sh
python3 Scripts/Android/test_android_preflight.py
python3 Scripts/Android/test_android_package.py \
  LauncherArgumentTests SessionResultTests SettingsIdentityTests SurfaceIdentityTests
python3 Scripts/Android/test_android_package.py \
  PackageTests.test_signed_split_native_launch_and_persistence \
  PackageTests.test_invalid_install_does_not_replace_existing_package \
  PackageTests.test_signer_identity_upgrade_and_code_integrity
```

`SurfaceIdentityTests` builds `Tests/Host/android_surface_identity.c` against the
real `Guest/Android/Adapters/surface.c`, pinned AOSP JNI headers and guest Bionic,
then runs it in both engines. Controlled JNI handles check alias identity,
distinct surfaces, collected weak-handle retirement, weak-reference failure and
null inputs. The public native-window lookup must not fabricate a window when
the platform returns none. This is adapter bookkeeping coverage, not real ART
GC, libgui lifetime, rendering or SurfaceView recreation coverage.

A temporary offscreen Qt check exercised old/new metadata, cards, search and
empty results, resize/reflow, developer visibility, session details and a saved
settings dialog. Python parsing and whitespace checks pass.
An installed Azur Lane base APK also passes read-only AAPT2 icon selection,
bounded copy and bitmap decoding checks; its game code is not executed by that
check. The full package and Linux runtime suites were not repeated for this
launcher/tooling change; desktop visual review and game interaction through
the new launcher are unverified.

## X11 wire readback synchronization (2026-10-14)

The intermittent `xwayland_wire` failure was X `BadMatch` on `GetImage`
(request 73). The fixture called `XMapWindow` and immediately issued its
draw/readback sequence. XSync round-trips do not wait for a window manager's
redirected map request. The fixture now selects StructureNotify and waits
for its window's MapNotify before drawing, preserving the 100 round-trips
and real guest libX11/server pixel check.

This enforces the
[Xlib mapping and readback contracts](https://www.x.org/releases/X11R7.5/doc/libX11/libX11.html):
window readback requires a viewable window. A temporary probe with an extra
attribute query passed ten runs, so it did not independently capture an
unmapped window at failure. After the mapping wait, the final strict JIT
suite passed 326/326 and the interpreter suite passed 317/317 with nine
JIT-only skips. This is a test synchronization correction, not an emulator
rendering or gameplay fix; other desktop/readback error causes remain outside
this bounded check.

## Tier-1 hot-path code reduction (2026-10-14)

The codegen/dispatch batch described in [JIT.md](JIT.md#tier-1-hot-path-code-reduction-2026-10-14)
passed the following checks with host-device access:

- SDL/GL executable/library and separate headless executable/library builds;
  `make generated-check`.
- `make verify`: 87 differential fixtures and seven threaded oracle runs,
  including its independent scalar SSA host oracle.
- Serial strict JIT suite: 326/326. Interpreter: 317/317, nine JIT-only skips.
  An initial concurrent run failed only `xwayland_wire`; it passed alone and
  in the subsequent serial full run. The cause of that transient failure was
  not independently established.
- Ordinary frame layout: 36 selected JIT, userfaultfd, permission, shared-alias
  and LSE checks, all passing (`BIFROST_NO_CHAIN_SKIP=1`).
- Tier-2 discovery, feedback and timing oracles in ST/MT and both layouts;
  20 mutex promotion-stress runs; all five `test_jit_mt.sh` configurations;
  publication/lease checks in both layouts.

`jit_mt_publication.cpp` now warms a loop's interior entry before its head.
It checks five-instruction heads for B.cond, CBZ/CBNZ, TBZ/TBNZ and B,
independent arithmetic results for the finite conditional loops, retained
function identity after unchanged protection, and retirement/retranslation
after rewriting an absorbed interior word. A differently targeted loop and
a tail exceeding the remaining instruction budget must keep their split.
These run in ST/MT and both frame layouts. The unconditional loop is checked
for shape and dependencies only, not executed indefinitely.

CoreMark used the saved pre-batch executable, the same guest ELF, normal JIT
defaults, one warm-up each and three interleaved runs each at exactly 110,000
iterations. Throughput samples were 9,054.24/8,928.57/9,049.03 before and
9,893.87/10,011.83/9,620.43 after. All seed/list/matrix/state/final CRCs
matched the benchmark runner's QEMU-checked expectations. The combined
median improvement was 9.3%; it is a local benchmark observation. These final
runs include the compaction-accounting correction below and supersede the
initial 12.1% three-pair checkpoint. No new
game FPS/stability, complete FPCR/FPSR, arbitrary concurrent mapping-race or
cross-host performance claim follows from these checks.

## Dead-code compaction accounting (2026-10-14)

The expanded managed service probe exposed `vector::reserve` during ART
translation, before its first manager assertion. A bounded GDB backtrace
localized it to `optimize_ir`. The saved pre-batch binary reached the expected
unsupported `user.isUserRunning` call; a temporary binary changing only
known-entry loop growth still hit the reserve exception. Source tracing found
DSE/DCE double-counting plus unsigned capacity subtraction. The fix reserves
the actual retained count and reports physical removals once.

`Tests/Host/ir_ssa.cpp` adds a legacy block with sixteen constant stores and
a helper boundary. It must retain the final value/store/helper, count thirty
removals and survive a second optimization with cumulative statistics intact.
`test-ir-ssa` links the real optimizer and passes 21,839 checks; `make verify`
passes 87 differential fixtures and seven threaded oracle runs. SDL/GL and
headless executable/library builds pass. The managed service batch then
completed in both engines (174 assertions before the final power-query
additions), removing the native exception on the concrete failing path.

The final full-suite and service results are recorded below; this fix does
not repair unrelated mapping races or establish game stability.

## User and idle-policy queries (2026-10-14)

`ManagedServiceBatch.usersAndIdle` drives genuine UserManager and PowerManager
clients. It checks the running/unlocked user, profiles and serial mapping,
restriction/profile reply isolation, redacted personal fields, admin versus
application privileges, foreign-user denial, missing-user restrictions,
battery-optimization membership, disabled standby policy queries and AOSP's
default battery-saver Parcelable. Administrative user-info and permanent/temp
power-exemption requests must throw SecurityException for the ordinary app.

The final complete batch passed 176 assertions in JIT (86.9 s) and interpreter
(104.3 s), including existing settings, PendingIntent, receiver, service,
notification-channel, audio, wake-lock, Wi-Fi and two real repeating-alarm
deliveries. Native exceptions and missing success markers fail the runner.
The live service inventory passed and reports 26/104 supported UserManager
signatures, 13/22 device-idle signatures and 18/67 PowerManager signatures.
Unsupported user-administration and exemption mutations are classified as
unsupported, rather than being advertised from their permission-denial gates.

The APK suite passed 28/29 initially. Its native Bionic fixture included the
standalone service-manager test, which incorrectly expected OOM scores under
an ordinary app UID. The native service correctly returned EX_SECURITY.
`android_service_manager.c` now checks denial for app UIDs and keeps successful
queries for root/system/media/cameraserver. Its standalone 104 checks pass in
both engines; the isolated APK case passes all six JIT/interpreter/Tier-2 and
builtin/plugin Wayland combinations, including six persistent save increments.
The other 28 APK cases include real managed Service creation/bind/stop,
service-process exit, persisted/namespaced jobs and window/surface lifecycle.
The full 29-case suite was not rerun after this fixture-only correction.

Permission/return contracts were checked against the pinned Android 15 server
sources linked in the [runtime guide](Android/ANDROID_RUNTIME.md#user-and-idle-policy-queries-2026-10-14).
The score-query denial was checked against
[ActivityManagerService.ProcessInfoService](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-15.0.0_r1/services/core/java/com/android/server/am/ActivityManagerService.java)
and the provisioned `platform.xml` grants.
The numbers above are observed runtime results. They do not cover every
advertised method, administrative grants/mutations, cross-user operation,
native wire clients, Doze transitions, foreground services, notification
posting or gameplay.

```bash
PATH=/usr/lib/jvm/java-25-openjdk/bin:$PATH python3 -c \
  'from pathlib import Path; import sys; sys.path.insert(0,"Scripts/Android"); from android_managed import build_launcher; build_launcher(Path("/tmp/bifrost-service-probe"), (Path("Tests/Host/ManagedServiceBatch.java"),))'
python3 Scripts/Android/test_android_framework.py --services --managed-graphics \
  --probe /tmp/bifrost-service-probe/classes.dex --main-class ManagedServiceBatch \
  --emu build/release-sdl1-gl1/bifrost-emu --timeout 240
# Repeat with --mode interp.
JAVAC=/usr/lib/jvm/java-25-openjdk/bin/javac make android-managed-launcher
PATH=/usr/lib/jvm/java-25-openjdk/bin:$PATH python3 Scripts/Android/test_android_package.py
# Focused native APK case:
PATH=/usr/lib/jvm/java-25-openjdk/bin:$PATH python3 Scripts/Android/test_android_package.py \
  PackageTests.test_signed_split_native_launch_and_persistence
```

## Realm-owned host media decoding (2026-10-14)

Two Java MediaCodec clients (UIDs 10000/10001) shared one realm, each
decoding five 64x64 frames. AVC and HEVC were exact, but VP8 and VP9
returned their first frame with PTS 200000. The host trace showed every
VPX image arriving one packet late, with guest `user_priv` work addresses
reused. FFmpeg frame threading delayed output by a frame, while the real
`C2GoldfishVpxDec` reads each shown frame immediately and then releases
the work. Once output was immediate, frame 0 had exact luma but swapped
chroma. Disassembly of the provisioned `libcodec2_goldfish_vp8dec.so`
matches the android-15.0.0_r36 source: it reads the host buffer as YV12
(Y, V, U). The pinned r1 source and the emulator's libvpx host helper both
use I420. VPX now uses slice-only threading. It also writes a packed Y, V, U
frame at the decoded size, ignoring the guest's stale output size, as the
emulator's `getImage` does. Contexts are keyed by codec family, because both
VPX modules use ID 1. See the
[runtime guide](Android/ANDROID_RUNTIME.md#host-media-decoding-2026-10-14).

`make test-android-goldfish-media` (`Tests/Unit/android_goldfish_media_host.cpp`,
165 checks, needs `USE_MEDIA=1` with FFmpeg) drives the device through
`libbifrost.a`. It runs VP8 and VP9 contexts with ID 1 on separate slots
and passes a stale 320x240 output size. Each image is fetched immediately
after its decode, must carry that packet's opaque `user_priv`, and must
match an independent single-threaded libavcodec decode in Y, V, U order.
A second fetch must find no frame. Restoring frame threading fails check
13; I420 output fails check 33.

Temporary realm probes (`/tmp`, not regression gates) passed:
- All four codecs concurrently, with exact MD5/PTS/EOS and unchanged
  service PIDs after app exit.
- An ImageReader Surface with `render=true`.
- NDK `AMediaCodec` to `ANativeWindow` under forced GPU (VA-API H.264),
  with X11 pixel presentation.
- Earlier realm probes: service registration, restart of a killed daemon
  with a new PID, and synchronous `STOP` reaping every child before
  acknowledgement.

`android_service_manager` has 104 checks, including `processinfo`. Also passed:
- Full `run_tests.sh --strict`: JIT 326/326; interpreter 317/317 with 9
  JIT-only skips.
- `ActivityProbe$GrallocProbe` in JIT and interpreter.
- `test_asgard.py`.
- `make test-android-window-backend` and `test-android-sdl-backend`
  (256 teardown cycles).
- `generated-check`.
- The headless `USE_MEDIA=0` build, which reports no codecs.

The JIT host-test scripts now link with the `LDFLAGS` recorded in the
library's `.build-config`, because a media-enabled `libbifrost.a` needs
FFmpeg.

None of this covers SurfaceView recreation/resize, Wayland readback,
native EGL into an aliased Surface, flush/reconfigure, B-frames, HDR or
large resolutions, malformed-transport fuzzing, or game playback.

## Binder nested synchronous callbacks (2026-10-17)

Hungry Shark World's Google Mobile Ads requests timed out after 60 s (error
2). GMA's GmsClient thread calls microG's `IAdRequestService` synchronously
while holding a lock; microG answers with a synchronous listener callback.
That callback went to an app pool thread, which blocked on the same lock: a
deadlock that lasted until the session ended. Linux delivers such a callback
to the waiting caller thread. `android_binder_features` now has 115 checks:
a non-looper client thread must read the nested synchronous callback while
waiting for its reply (the previous binary gets `EAGAIN`), a one-way
callback must not reach it, and both replies must complete. It passes in
both engines; `test_asgard.py` passes. In the game, ad requests then
completed within about 100 ms with microG's "Not supported" (error 8).

## Binder one-way callback ordering (2026-10-14)

The real Codec2 H.264 service decoded five frames on the host, but Android
received missing/reordered outputs or EOS first. Slowing just that service
with the interpreter hid the failure. The cause was concurrent dispatch of
one-way callbacks on the same Binder object, contrary to Android's
[per-node serialization contract](https://source.android.com/docs/core/architecture/ipc/binder-threading#serialization-of-asynchronous-transactions).
The transport now holds that object's asynchronous execution slot until
`BC_FREE_BUFFER`; other objects and synchronous calls can bypass its queue.
Invalidating the receive mapping releases outstanding slots.

`android_binder_features` now has 95 checks, including a second looper that
must receive `EAGAIN` while the first call's buffer is held, ordered delivery
after release, and progress on unrelated objects and synchronous calls.
The old binary fails that deterministic case; both repaired engines pass.
The actual all-JIT Java MediaCodec diagnostic then returned five 64x64 H.264
frames with reference-identical pixels, timestamps 0 through 800000 us and
EOS, under both forced native CPU and forced VA-API GPU decoding. These
are diagnostic-root observations, not integrated launcher/Surface playback
coverage or a complete media-service implementation.

## ARM64 write-only mappings (2026-10-14)

The real Goldfish H.264 component maps its command/reply arena with
`PROT_WRITE`, then reads replies. Bifrost previously faulted on that read.
Effective page-access flags now include read access when write is requested;
the VMA retains its requested permissions for `/proc/maps`. This follows
Linux ARM64's [protection table](https://github.com/torvalds/linux/blob/v6.12/arch/arm64/mm/mmap.c),
which maps both private and shared `VM_WRITE` pages to readable PTEs.

The added `test_shared_alias` cases cover a shared write-only load and
`mprotect(PROT_NONE)` followed by `mprotect(PROT_WRITE)`. The unchanged
default binary faulted at the first load; the rebuilt graphics binary and
QEMU each passed all 22,058 checks, including both Bifrost engines.
`test_memory_permissions` passed 106 checks in each engine, retaining
read-only/PROT_NONE faults, fork isolation and syscall `EFAULT` coverage.
`make verify` passed 87 differential fixtures and seven threaded oracle runs.
These are observed regression results; they do not establish video playback.

## Inherited host open-file limit (2026-10-13)

`test_syscall_regress` now holds 1,536 separate regular-file opens
simultaneously, then closes them before the remaining syscall checks. This
consumes real host descriptors; virtual `/dev/null` nodes or `dup` aliases
would not reproduce the failure. Before the startup fix, a bounded
1,600-open probe and a native control both stopped at 1,021 successful opens
with `EMFILE` under soft 1024 / hard 1048576.

The rebuilt graphics and headless executables passed the 1,600-open probe in
JIT and interpreter mode with inherited soft 1024 and hard limits 4096 and
1048576. With both limits 1024, each correctly retained the host ceiling and
the probe returned `EMFILE`. An owned broker launched with soft 1024 showed
actual host limits 1048576 / 1048576 in `/proc/PID/limits`. The expanded syscall
fixture passed in both engines from soft 1024 / hard 1048576.

Azur Lane session `8fa5a4ce1a5b456a976638e201bbfc79`, launched through the
package runner under inherited soft 1024 / hard 1048576, reached resume,
window, successful media-JNI loading and a logged Account Recovery popup.
It ended at the 150 s limit (124); stdout and logd contained no fatal-crash
or `EMFILE`/too-many-open-files markers. This is startup/runtime evidence,
not a visual gameplay check or proof of opening one million files.

```bash
make cross SRC=Tests/Unit/test_syscall_regress.c OUT=Tests/Unit/test_syscall_regress.elf
prlimit --nofile=1024:1048576 build/release-sdl1-gl1/bifrost-emu \
  Tests/Unit/test_syscall_regress.elf
prlimit --nofile=1024:1048576 build/release-sdl1-gl1/bifrost-emu --no-jit \
  Tests/Unit/test_syscall_regress.elf
```

The startup policy follows Linux's
[soft/hard resource-limit contract](https://man7.org/linux/man-pages/man2/getrlimit.2.html);
the counts and broker limits above are observed tests. The fixture requires
a host hard limit sufficient for its regular files plus emulator overhead.
It does not establish full guest get/setrlimit semantics or readiness APIs
with high descriptor numbers. Source review separately found the existing
`pselect6` bridge still restricts fd sets to `FD_SETSIZE`; that is not repaired
by raising a host limit.

## Real media JNI (2026-10-13)

After provisioning framework schema 7 and rebuilding the launcher,
`ActivityProbe$MediaJniProbe` exercises seven real android.media class
initializers plus native MP4 muxing of three known AAC silence access units.
The runner requires both media markers as well as `FRAMEWORK_OK`.
JIT passed in 3.0 s and interpreter in 10.4 s. The probe logs the small MP4
as Base64 after `ANDROID_MEDIA_MUXER_MP4`; an independent host FFmpeg check
decoded both outputs to exactly 6,144 bytes of zero PCM (8 kHz mono,
three 1,024-sample AAC frames). Fresh schema-7 extraction verified 931 files,
adding eight native dependencies without changing prior files; the old
schema was correctly rejected rather than silently upgraded.

```bash
JAVAC=/usr/lib/jvm/java-25-openjdk/bin/javac make android-managed-launcher
python3 Scripts/Android/test_android_framework.py --services --managed-graphics \
  --probe build/android-managed-launcher/classes.dex \
  --main-class 'ActivityProbe$MediaJniProbe' --output /tmp/media-jni-jit
python3 Scripts/Android/test_android_framework.py --services --managed-graphics \
  --probe build/android-managed-launcher/classes.dex --mode interp --timeout 90 \
  --main-class 'ActivityProbe$MediaJniProbe' --output /tmp/media-jni-interp
```

This is a JNI/muxing regression, not decoder, DRM, AudioTrack or playback
coverage. Codec discovery still returns zero without media services. A
separate native host FFmpeg H.264 feasibility check produced matching CPU and
VA-API frame hashes for a generated five-frame 64x64 clip. It does not exercise
guest Android buffers, codec callbacks or a Bifrost host-decoder bridge.

Azur Lane session `c4507e78efc749aea94c80a0acd91b84` confirmed successful
`libmedia_jni.so` loading, reached a logged Account Recovery popup and ended
at its 150 s limit (124). Stdout and logd had no fatal-crash markers. This
was a bounded runtime regression, with no visual gameplay or video check.
The installed-app persistence, Activity-stack and service-execution package
regressions passed 3/3 against schema 7 (258.7 s).

## Settings and PendingIntent contracts (2026-10-13)

`ManagedServiceBatch` adds real Android Settings/PendingIntent clients to the
common-device probe. It checks fresh AOSP defaults (`accelerometer_rotation=0`,
screen timeout, location and bubbles), absence of Google's `limit_ad_tracking`,
namespace/user isolation, System write grants and validators, persistence after
provider recreation, read-only generation transport/cache invalidation and
ContentObserver delivery. PendingIntent checks include identity through Parcel,
lookup, replacement/cancellation, extras updates, immutable and mutable sends,
one-shot and cancellation listeners, creator metadata, service and Activity
result delivery, dynamic and manifest broadcasts and ordered result propagation.
Protected-only receiver filters can omit export flags; custom and mixed filters
must supply them. This regresses SpongeBob WebView's system receiver registration.
Alarms exercise exact-permission denial, operation cancellation and two actual
repeating deliveries with the 60-second minimum interval.

The installed `ManagedActivities` fixture launches an additional translucent
Activity through a PendingIntent, checks that it has no result caller, and
returns through its real window/lifecycle. The signed `Managed` fixture now
creates/looks up/cancels senders and checks fresh System values; its forced
runtime replacement preserves an injected System override. `SettingsIdentityTests`
continues to cover SSAID keys separately.

The defaults generator downloads only pinned `android-15.0.0_r1` inputs into a
temporary cache. Source/spec checks cover loader table ownership, resources,
reviewed fresh-device upgrades, public validators, sender keys/flags, protected
broadcast exemption and Android 15 receiver signatures. Runtime tests exercise
the cases above; they do not prove all framework API contracts, OEM/property
overrides, cross-app/process delivery, background restart, foreground services,
notification taps or full gameplay.

Observed on the final source: all 153 service assertions passed in JIT
(86.3 s) and interpreter (105.1 s), including two real repeating alarm
deliveries. The complete APK package suite passed 29/29 (401.0 s). Platform
permission definitions are cached independently of app grants: before that
change, repeated framework APK parsing in the expanded interpreter probe
encountered an unmapped null read. Its native cause remains unlocalized;
passing the cached path is not a native interpreter repair.

GPU-enabled real-app runs reached Application/Activity resume, window and
surface checkpoints. Hungry Shark ran its bounded 150 s session (exit 137
after guest process termination on closure); SpongeBob ran 150 s (timeout
124), including WebView initialization after the protected-receiver fix.
Neither reported the original PendingIntent/rotation failures. Azur Lane
passed those startup paths but exited 127 when MediaCodec initialization
could not load `libmedia_jni.so`; media provisioning is separate work.
Google's `limit_ad_tracking` remains absent. These are lifecycle observations,
not visual gameplay verification.

```bash
python3 Tools/Android/generate-settings-defaults.py --check
PATH=/usr/lib/jvm/java-25-openjdk/bin:$PATH python3 -c \
  "from pathlib import Path; import sys; sys.path.insert(0,'Scripts/Android'); from android_managed import build_launcher; build_launcher(Path('/tmp/bifrost-service-probe'), (Path('Tests/Host/ManagedServiceBatch.java'),))"
python3 Scripts/Android/test_android_framework.py --services --managed-graphics \
  --probe /tmp/bifrost-service-probe/classes.dex --main-class ManagedServiceBatch \
  --emu build/release-sdl1-gl1/bifrost-emu --timeout 240
# Repeat with --mode interp.
JAVAC=/usr/lib/jvm/java-25-openjdk/bin/javac make android-managed-launcher
PATH=/usr/lib/jvm/java-25-openjdk/bin:$PATH python3 Scripts/Android/test_android_package.py
```

## App ops (2026-10-21)

`ManagedServiceBatch.appOps` drives the whole Android 15 `IAppOpsService`
through the registered `appops` Binder and the real `AppOpsManager`:
the framework interface's method list equals `FrameworkAppOps`'s contract;
every op's default mode (`opToDefaultMode(opToSwitch(op))`, restricted-read
ops raise `SecurityException`); the ops 24/43 regressions; note, start and
finish accounting with nested starts, two clients and a client that dies;
mode, active, started, noted and async-noted callbacks (arguments, ordering,
silence after `stopWatching*`); package overrides, uid overrides,
`MODE_FOREGROUND` and `MODE_DEFAULT` with `startIfModeDefault`; proxy
operations; user restrictions; `getOpsForPackage`/`getPackagesForOps`/
`getHistoricalOps` results; `SecurityException` for every privileged method
and `IllegalArgumentException` for bad codes and history arguments. It is
part of the batch below (JIT and `--mode interp`).

```bash
python3 Scripts/Android/test_android_framework.py --services --managed-graphics \
  --probe /tmp/bifrost-service-probe/classes.dex --main-class ManagedServiceBatch
```

## Common device services (2026-10-13)

`ManagedServiceBatch` drives the real Android 15 `UiModeManager`,
`PackageManager`/`PermissionManager`/`PermissionChecker`, `BatteryManager`,
`Vibrator`/`VibratorManager`, `KeyguardManager`, `ClipboardManager`,
`AlarmManager`, `AccountManager`, `LocationManager` and `NotificationManager`.
It checks AOSP defaults, grant/denial agreement with the install-time model,
copy isolation of permission and clip replies, sticky battery delivery to a
receiver, focus-gated clipboard reads and listeners, a listener alarm firing
no earlier than 5 s while a cancelled one never fires, and SecurityException
for privileged calls and for revoked VIBRATE/DISABLE_KEYGUARD. The original
106-check probe passes in JIT and interpreter mode. `--service-inventory` lists 32 services.
`test_android_package.py` passes 29/29 (with `javac` on `PATH`).

Hungry Shark Evolution, which had thrown on a null `UiModeManager`, ran its
full 180-second session to its window, surface, popups and resume, without
null-manager or unsupported-service exceptions. SpongeBob ran its 150-second
session through the same checkpoints. Both still report missing Google Play
services (`SERVICE_INVALID`). At that earlier checkpoint they also reported
PendingIntent creation and missing settings, addressed by the contracts above;
Google's `limit_ad_tracking` intentionally remains absent. Gameplay was not
checked in those bounded runs.

```bash
python3 - <<'PY'
from pathlib import Path
import sys
sys.path.insert(0, 'Scripts/Android')
from android_managed import build_launcher
build_launcher(Path('/tmp/bifrost-service-probe'), (Path('Tests/Host/ManagedServiceBatch.java'),))
PY
python3 Scripts/Android/test_android_framework.py --services --managed-graphics \
  --probe /tmp/bifrost-service-probe/classes.dex --main-class ManagedServiceBatch
```

## Selective invalidation and shared MT compiles (2026-10-13)

`Scripts/Testing/test_jit_cache.sh` adds `jit_cache_invalidation retain` in
default and `BIFROST_NO_CHAIN_SKIP=1` modes: a caller on one page BLs a callee
on the next; the callee is rewritten by a line publish, then by mprotect
without cache maintenance, and an unchanged page is re-protected. Results and
`[JIT] invalidate` counters must show rewritten code retired and unchanged
same-line/same-page code kept. The pre-change binary fails the direct-call
mode with exit 4 (stale callee); the default mode passed before too, because
chain-skip disables native `BL_CALL`.

On the final binary: strict JIT suite 326/326, `--no-jit --strict` 317/317
with nine JIT-only skips, `make verify` (87 differential fixtures, seven
threaded oracles), `make generated-check`, the headless build, `test_jit_mt`,
`test_jit_mt_publication` and the Tier-2 discovery/feedback/measure scripts
pass. CoreMark (110,000 iterations, CRC 0x33ff) executed identical user-space
instruction counts (77.70 G) with overlapping cycle counts; eight interleaved
wall-clock pairs on a loaded desktop varied by about 2%. The ART framework
probe passed 5/5 per binary at about 3.05 s. Not covered: gameplay stutter,
ART JIT publication under load and Tier-2 enabled in Android games.

## Provider startup and Activity API reports (2026-10-13)

`PackageTests.test_managed_launcher_persists_data_and_finishes_activity` now
checks two real manifest providers before `Application.onCreate`, descending
`initOrder` despite reversed manifest order, an attached provider/application
context with connectivity access, semicolon authority aliases and a single
provider instance. A provider returning false from `onCreate` is still
published by ActivityThread. Disabled and remote-process provider classes throw
if incorrectly instantiated. Two JIT launches and an interpreter launch pass,
including data persistence and runtime replacement. The interpreter attempt
first exceeded 90 seconds during WebView/package startup, before reaching this
path; the 180-second interpreter budget passes. The same fixture checks absent
NO_CREATE broadcast/activity/service PendingIntents, successful sender creation,
identity lookup/cancellation and System overrides across runtime replacement.

`PackageTests.test_managed_activity_api_report` uses a signed APK and the real
framework. The earlier trace cataloged 428 signatures in the three Activity
Binder interfaces, 11 distinct observed methods and 17 calls, including two
deliberately unsupported calls: `getIntentSenderWithFeature` creation and
`getAppTasks`. Creation now succeeds; the remaining `getAppTasks` failure retains exception
and caller stacks; repeated `getMyMemoryState` calls aggregate correctly. All
calls complete, the capture stops cleanly, and no global thread-stack, library
map or ART method records are emitted in `activity` mode. The saved example is
under `build/android-activity-trace-results/apk-trace-*/`; its JSON and text
reports distinguish available signatures from observed outcomes. The focused
host parser regression checks overload separation, unseen declarations, thrown
and unfinished calls, truncation and a partial JSON record.

The existing Activity stack gate and untraced service execution/restart gate
also pass. Full ART service tracing initially hit its 15-second callback
deadline twice; a temporary launcher built from the prior commit passed.
Catalog generation and generic signature formatting are now restricted to
`activity` mode, preserving the earlier full-trace record behavior. The final
Activity-report plus full-ART service-trace run passes 2/2 (58.5 seconds).
This comparison does not establish that formatting alone caused the deadlines.

Reproduce the focused runtime checks with:

```bash
python3 Scripts/Android/test_android_package.py \
  PackageTests.test_managed_launcher_persists_data_and_finishes_activity \
  PackageTests.test_managed_activity_api_report \
  PackageTests.test_managed_service_trace \
  PackageTests.test_managed_activity_stack
```

Provider ordering, publication and PendingIntent lookup behavior were checked
against Android 15 source linked in the runtime guide. These fixtures do not
cover remote provider processes, PendingIntent creation/delivery or every
cataloged API. Real Azur Lane 9.3.7 writes growing asset files after the fix
(over 3.5 GB observed), but download completion, login and gameplay remain
unverified. The game was left downloading during validation. This change does
not establish a full Android package-suite or JIT differential result.

## Scalar FP scratch preservation (2026-10-13)

`jit_fp_scalar` now checks full scalar writeback bits across fourteen live
FP registers, including partial caching, high host XMM registers, aliases,
NZCV, signed zero, subnormals, infinities and quiet/signaling NaN payloads.
It also keeps integer results live across FADD, FSQRT and FMOV, including
their NaN/invalid cold paths. QEMU, JIT and the reference interpreter pass;
a temporary host driver disabling the JIT's SSE4.1 feature bit also passes.
The emitter audit checked that the retained hot paths use only XMM scratch
and that `emit_fp_interp_step` preserves RAX. These are local-source checks
and executable results, not broader proof of FP exception/FPCR coverage.

`make verify` passes 87 differential fixtures, seven threaded oracle runs
and 21,836 SSA checks. The selected FP/SIMD/flags/memory tests pass 37/37.
Tier-2 discovery/recovery checks pass in single/shared-thread mode with
ordinary and chain-skip frames.
The release executable/library and headless executable/library compile and
link. The headless `all` target's final copy to the running `./bifrost-emu`
was refused with `Text file busy`; the built headless artifacts are intact.

A temporary generated loop alternates `add x9,x9,#1` and
`fadd sN,sN,s31` for N=0..7, then decrements X0 and branches back. Six
interleaved process runs (three per build), each with one excluded warmup
and five measured trials of 16 million iterations, validate all eight
accumulators at 8 million, X9 at 128 million and cleared upper lanes.
Median time was 103.516 ms before and 93.049 ms after (10.1% less time).
Total emitted function size, including cold code, was 3,627 versus 3,593
bytes. The actual profiled IL2CPP block at file offset `0x58eb6f4`, replayed
at a fixed guest address, shrank from 5,067 to 5,060 bytes.

Separate three-run CoreMark medians were 12.729 s before and 13.045 s after;
matrix medians were 79.4 ms and 77.7 ms. All result checks passed, but
background game/build activity varied between these general benchmark
windows, so they establish neither a general speedup nor a controlled
regression. The mixed-loop result is also host-specific. An updated
SpongeBob session was started; the user reported it was still loading.
It later entered Kidoz's ad path, logged a thread-161 unmapped null read and
122-146 skipped frames per report while remaining alive. The earlier
pre-change profiling session had the same ad/null-read signature; its
cause was not investigated in this codegen change. Cooking compatibility
and FPS improvement remain unverified.

## Covered Activities stop (2026-10-13)

`PackageTests.test_managed_activity_stack` now checks that the opaque second
Activity stops the root (`onStop`), removes its SurfaceView's host surface and
restarts it (`onRestart` before `onActivityResult` and `onResume`), and that
the translucent second round leaves the root paused and visible. The previous
build never stops the root. The full Android package suite ran 26/27: the
method-traced `test_managed_service_trace` hit its service-callback deadline
once, during a run with about 11 emulator processes alive (its fixture starts
no Activity); it passed alone. One 180 s Azur Lane run (JIT):
`PrePermissionActivity` stops when `MainActivity` resumes, its blue window no
longer reaches the host, and the game's surface shows (user-confirmed); the
game then passed its update check, showed its download-group panel and quit
through `GameMain:OnApplicationQuit`. Gameplay was not reached.

## AAudio API coverage (2026-10-13)

`Tests/Unit/android_aaudio_thread.c` now also covers channel layouts and
counts (index masks hidden), invalid mask/count/content type, attribute
defaults and setters, state/result text, flush rules, release (CLOSING, no
restart) and release/flush from the data callback; JIT, interpreter and
Tier-2 pass in `test_android_bionic.sh`. The managed `libaaudio.so` exports
every AAudio name CRIWARE looks up (59). JIT suite 326/326, interpreter
317/317 (9 skipped), `make verify` and `make generated-check` pass; the
headless build compiled and linked (its copy to `./bifrost-emu` was refused
while that binary was running).

## Managed OpenSL ES players (2026-10-13)

`Tests/Unit/android_opensles_player.c` (run by `test_android_bionic.sh`)
passes in JIT, interpreter and Tier-2 through the real Bionic linker, the
managed libaaudio and libOpenSLES adapters and the pinned wilhelm headers. It
checks the shared engine, creation validation codes, configuration before and
after Realize, both buffer-queue IIDs, queue limits and state, callbacks on a
separate thread with Bionic TLS, position, HEADATNEWPOS/HEADATEND, pause, stop,
clear, Destroy from the player's own callback, and a mono float `PCM_EX`
player. It does not inspect the rendered samples (format conversion, gain and
pan are unverified by tests). The adapter's exports equal AOSP's
`libOpenSLES.map.txt`. JIT suite 326/326, interpreter 317/317 (9 skipped),
`make verify`, `make generated-check`, the full `test_android_bionic.sh` and the
headless build pass.

One 180 s Azur Lane 9.3.7 run (JIT): CRIWARE initializes without the earlier
`CEngine_Realize`/`createTrack` errors or `failed to realize OpenSLES player`,
and a host audio device is open for the session. Startup still logs nothing
after the update checker's `MAP: No Split File Need To Download.`; the intro
movie and title were not confirmed from the logs.

## Azur Lane startup: exclusive pairs, onCreate launches, libandroid (2026-10-13)

`test_exclusive_pair` (registered in `run_tests.sh`) passes under QEMU 11.1
user mode and in Bifrost's JIT and interpreter; the previous build raises
SIGILL at the first LDXP. JIT suite 326/326, interpreter 317/317 (9 skipped),
`make verify` and `test_android_bionic.sh` pass after all three changes.
Azur Lane 9.3.7 (APKM base APK; OBB placed by hand, as the installer has no
OBB support) previously exited at 14 s (`activity_task.startActivity`), then
raised SIGILL on `LDXP x10, x11, [x0]`. It now runs to the 180 s session
limit: Unity renders, CRIWARE loads, and the game reaches its version gateway
and CDN update check. Title screen, login and gameplay were not verified from
the logs. No dedicated regression covers the onCreate launch or the libandroid
forwarders beyond the Bionic and package checks above.

## Consistent process identity (2026-10-13)

`test_proc_exe` checks, in the parent (PID-1 model) and a fork child (host
identity), that `getpid() == gettid()` and that `getpid`, `getppid`,
`getpgid(0)`, `getsid(0)` and the comm equal `/proc/self/stat`,
`/proc/<pid>/stat` and `/proc/self/status` (`Pid`, `Tgid`, `PPid`). The same
checks pass natively on the host kernel, also under `setsid`; QEMU user mode
is not a reference here (it synthesizes stat with session 0 and passes the
host comm through). The previous build fails them (`ppid=0`, comm
`bifrost`, status `PPid=0`). JIT suite 325/325, interpreter 316/316,
`make verify` and `test_android_bionic.sh` pass. Managed Binder-broker apps
were not run for this change.

## Kernel-format /proc/self/maps (2026-10-13)

`test_proc_exe` now also checks a private file mapping split by `mprotect`
(`r--p 00001000`, `rw-p 00002000`), a `MAP_SHARED` view (`rw-s`), each with
the file's `major:minor`, inode and real path at column 74; a `PROT_NONE`
anonymous reservation (exactly `---p 00000000 00:00 0 `); and an `r-x` line
naming `/proc/self/exe`. The file-line checks pass under QEMU 11.1 user mode
(names from host kernel mappings). QEMU pads anonymous lines, unlike the
kernel, so the anonymous check follows the host kernel's own maps. Running
the original test under QEMU truncates the fixture: QEMU's `/proc/self/exe`
open with `O_TRUNC` has no `ETXTBSY`.

Bifrost passes it, the JIT suite (324/325; `xwayland_wire` failed once with an
X `GetImage` BadMatch and passed 3/3 alone), the interpreter suite (316/316),
`make verify`, the headless build and `test_android_bionic.sh` (bootstrap and
AAudio in all modes; one earlier AAudio STOP failure did not recur and nothing
in that fixture reads maps). Bifrost-copied images: the main ELF, Android
`linker64`, the legacy interpreter and native-linker libraries are named after
their files; native-linker libraries stay `rwxp` because that linker never
applies PT_LOAD permissions. The vDSO is reported as its three real
`[vdso]` ranges; `[vvar]` and anonymous VMA names (`PR_SET_VMA`) are not
modelled.

## Software Gralloc5 and allocator lifetime (2026-10-13)

`ActivityProbe$GrallocProbe` uses real Android 15 AHardwareBuffer/libui clients
and an AImageReader BufferQueue, with `--services --managed-graphics` and the
current managed launcher dex. It covers eleven explicit CPU buffer formats,
NDK handle transfer, independent imports and retained pixels/identity after
the original import is released; YUV planes; unsupported protected/multi-layer/
unknown formats; two queued RGBA frames, latest-image selection, full pixel
comparison and a positive timestamp. It also checks the service manager's
positive/negative HAL declarations and instance lists. JIT and interpreter pass.
The Java Parcel allocator client additionally requests packed RGB888 with a
64-byte reserved region and libui's two retained legacy allocation hints.
The native mapper checks aligned/shared reserved bytes, shared dataspace,
malformed metadata rejection and the specified rejection of non-CPU lock flags.
An invalid lock with an unsignaled fd must return promptly and close that fd.

`--binder-broker SOCKET --app-id UID` lets independent framework runners share
one broker. `--probe-arg 15` repeats Gralloc checks in the same VM after 15 s.
The UID-10003 survivor passed after the UID-10002 peer exited; UID 10004 then
passed after both earlier clients exited. This verifies the realm-owned
allocator survives app disconnects, not simultaneous full-game compatibility.

`android_binder_process` additionally exchanges writes through independent
`MAP_SHARED` mappings of a Binder-transferred memfd across distinct guest UIDs.
`test_asgard.py` passes all four JIT/interpreter pairs, peer-death cases and the
eight-process stress test. EGL native-buffer/external-texture import and media
decoding are excluded. Commands and source references are in the
[runtime guide](Android/ANDROID_RUNTIME.md#software-gralloc5-2026-10-13).

Final validation for this milestone: SDL/GL and headless emulator/library builds,
guest adapters and launcher build, generated-table check, strict GPU-enabled JIT
suite (325/325), strict interpreter suite (316/316, nine runner exclusions),
`make verify` (87 differential fixtures and seven threaded oracles), and the
Bionic/bootstrap/AAudio gate pass. Interpreter exclusions were `jit_cache`,
`jit_host_fault`, `jit_tier2_discovery`, `jit_tier2_feedback`, `jit_tier2_measure`,
`jit_tier2_mutex`, `test_long_call_loop`, `tier2_smov` and `zstd_compression`.
The normal-JIT untraced Trickcal run still reports the existing Scudo corrupted
chunk header failure (saved session `fa1bca4868624ab98eee6d2341638179`, exit 127);
these passes do not establish video playback.

## Activity windows and SurfaceView lifecycle (2026-10-13)

`PackageTests.test_managed_view_window_and_surface_lifecycle` (`ManagedViews`)
launches an Activity with only a button, requires its window on the host
layer with the button text, then adds, hides, shows and removes a SurfaceView
and requires exactly two create/destroy pairs in that order. The persistence,
dialog/popup, activity-stack and service tests pass with the root Activity
as a window (5/5). The dialog test now expects three host windows (the
Activity's action bar, the dialog, the PopupWindow) and no initial focus
loss; the activity-stack test no longer orders the second window's layer
against its asynchronous focus callback. It also requests a permission from a
background thread and a second one from the first result; the
few-instruction race that motivated the delivery wait is too narrow for the
fixture to reproduce, so the fixture covers only the request contract.

Before the `SurfaceFlinger` stand-in all four play tests hung in libgui's
`waitForService("SurfaceFlinger")`. In SpongeBob and Trickcal (JIT, 75-150 s)
Unity's SurfaceView got its surface, rendering started, and the Activity
window showed only Unity's startup placeholder (`visible=3543040`) until
Unity removed it (`APP_POPUP_HIDDEN 1 transparent`). Game frame rates were
not measured. Trickcal still aborts in Scudo ("corrupted chunk header") about
45 s in on guest thread 11 (2 of 2 untraced runs; not with `--trace-apk`);
that crash is unrelated to these changes and remains open.

## Base ARMv8.0 FP/SIMD sweep fixes (2026-10-13)

`jit_simd_base_forms` (registered as `simd_base_forms`) hashes 139 base forms
over 96 boundary inputs each (result and FPSR.QC) against a table generated by
QEMU 11.1 with `-cpu cortex-a53` (`-cpu max` gives the same table). It also
checks that 30 reserved or unadvertised encodings raise SIGILL. It passes
169/169 under QEMU, the JIT, `--no-jit`, `BIFROST_JIT_VERIFY=1`,
`BIFROST_IR_VALIDATE=1` and the headless build. The committed build before
this change fails 133 (JIT) and 135 (interpreter) of the checks.
`jit_neon`, `jit_neon_fixups`, `test_simd_sat` and `jit_neon_cmp` had encoded
results that QEMU rejects. They executed SHL `#64` and `.1d` compares, which
are SIGILL; read TBL's table as 8 bytes; wrapped UHSUB before halving; and
treated the UQSHL amount as unsigned. They now follow QEMU. The JIT's SSE2 QC
fallback was checked in a temporary build with SSE4.1 disabled, not in this
suite. The strict JIT suite passed 323/324; its `neon_cmp` run predated that
fixture's rebuild and passes in a filtered rerun (35/35 neon/simd tests).
`--no-jit --strict` passes 315/315 (nine JIT-only skips); `make verify` passes 86 differential fixtures and 7 threaded
oracles; `make generated-check` passes. CoreMark (110,000 iterations, three
runs, crc 0x33ff) ran 8,968 / 9,012 / 9,061 iterations/s, unchanged within
noise. The sweep and fixtures do not compare FP cumulative exception flags,
and no game was run for this change.

## Window limits in the header page (2026-10-12)

`jit_window_bounds` (registered as `window_bounds`) runs 72 checks: byte, half,
word and doubleword loads and stores, with and without immediate offsets, at
two bases in the top page of the address space and at negative offsets that
wrap past zero, three warm-up rounds each. Every access must raise SIGSEGV with
the access address in `si_addr` and resume through the handler. It passes under
QEMU 11.1, the JIT, `BIFROST_JIT_VERIFY=1`, `--no-jit` and
`BIFROST_JIT_SOFTWARE_GUARD=1`. Addresses from 2^47 to 2^56 were left out: QEMU
reports `si_addr` 0 for them, and tagged addresses expose a separate, pre-existing
`si_addr` tag difference (see the Android runtime's pointer-tag notes). Strict
suites pass 318/318 (JIT) and 309/309 (`--no-jit`); `make verify` passes 80
differential fixtures and 6 threaded oracles.

## Single-instruction extensions (2026-10-12)

`jit_extend_width` (registered as `extend_width`) runs 442 checks over 13
values: SXTB/SXTH/SXTW and UXTB/UXTH to X and W, AND with byte/half/word masks,
extended-register ADD/SUB operands, LDRSB/LDRSH/LDRSW, NZCV consumed across an
extension or mask, and a block with twelve live values. It passes under QEMU
11.1, the JIT, `BIFROST_JIT_VERIFY=1`, `BIFROST_IR_VALIDATE=1` and `--no-jit`.
A disassembly of its emitted code shows MOVSX/MOVZX on R8-R15 sources and no
AH..DH operands; RSI/RDI byte sources were not exercised (not allocated for
guest values). Strict suites pass 317/317 (JIT) and 308/308 (`--no-jit`);
`make verify` passes 79 differential fixtures and 6 threaded oracles.

## Register moves through XZR (2026-10-12)

`jit_mov_alias` (registered as `mov_alias`) runs 77 checks over seven values:
MOV X/W, ORR/EOR/ORN/AND with XZR/WZR, shifted and rotated sources, W-form
upper-half clearing and destination aliasing. It passes under QEMU 11.1, the
JIT, `BIFROST_JIT_VERIFY=1`, `BIFROST_IR_VALIDATE=1` and `--no-jit`. The strict
suite passes 316/316 (JIT) and 307/307 (`--no-jit`, 9 JIT-only skips); `make
verify` passes 78 differential fixtures and 6 threaded oracles. CoreMark's
median rose from 8,565 to 8,763 iterations/s; game FPS was not measured.

## Half-precision conversions (2026-10-12)

`jit_fp_half_convert` (registered as `fp_half_convert`) checks 12 fixed
results: FCVTL/FCVTL2 normals, subnormals, infinity and a quieted NaN payload;
FCVTN/FCVTN2 ties-to-even, overflow, tiny values and NaN; scalar FCVT S/D<->H;
FRINTN NaN quieting; scalar FADDP invalid-sum and NaN-order results. It passes
under QEMU 11.1, the JIT and `--no-jit`; the pre-fix binary fails its first
check and then raises SIGILL on FCVTN. The strict suite passes 315/315 (JIT)
and 306/306 (`--no-jit`, 9 JIT-only skips); `make verify` passes with 77
differential fixtures and 6 threaded oracles. The user confirmed SpongeBob's
outfit, avatar portrait and lobby character after the fix. FPCR rounding
modes other than nearest, `FPCR.AHP` and FPSR flags remain untested.

## Integer ADDP widths (2026-10-12)

`jit_simd_addlv`'s existing ADDP oracle (all element sizes, both Q, Q=0 high
halves filled with 0xFF, wrapping patterns, Vd aliasing Vn or Vm) now runs
natively: 2,624 checks pass under the JIT, `BIFROST_IR_VALIDATE=1`,
`BIFROST_JIT_VERIFY=1` and `--no-jit`, and its fallback profile contains no
ADDP encodings. The JIT/SIMD/FP regression subset passes 65/65 and
`make generated-check` reports no drift. SpongeBob was not re-profiled.

## Ordered allocation overlap lookup (2026-10-12)

A 60-second `perf` capture of SpongeBob gameplay attributed 4.2% of sampled CPU
to `Memory::untrack_allocation`, which scanned every tracked allocation on each
unmap. With the ordered overlap lookup, a throwaway guest benchmark (20,000
live 8 KiB mappings, 20,000 partial and 20,000 remaining unmaps, contents
verified) measured unmap-phase medians of 0.995 s before and 0.132 s after
over three alternating runs (7.6x). The full strict suite passes 314/314.
SpongeBob was not re-profiled after the change; its FPS gain is unmeasured.

## Native-density play windows (2026-10-12)

`test-android-window-backend` checks that a backend reporting 1.5 pixels per
desktop unit opens at 1280x720 desktop units, the manager adopts 1920x1080 px and
240 dpi without a host resize, and an unscaled backend reports 160 dpi.
`test-android-sdl-backend`'s SDL3 fixture now doubles pixels only for windows
created with high pixel density; the backend requests it, reports 1600x1200 px
and ratio 2.0 for an 800x600 window, and no longer resizes on open (256 teardown
cycles pass). `make generated-check` regenerates the viewporter glue. The
package suite passes 26/26 and the headless (`USE_SDL2=0 USE_THUNK_GL=0`) build
compiles. SpongeBob runs at 2560x1384/3440x1384 px, 320 dpi, on the user's 2x
monitor and at 1920x1038 px on a 1.5x monitor. No test drives a real compositor's
viewporter: subsurface and popup scaling were checked only by the user's view of
the running game, and the integer buffer-scale fallback is untested.

## Notification enablement (2026-10-12)

ActivityProbe's resumed installed-app service check now requires
`NotificationManager.areNotificationsEnabled()` to answer, and to match the
package's `POST_NOTIFICATIONS` grant (false in this profile). It runs only in
play mode, where the installed package is known:

```bash
python3 Scripts/Android/test_android_framework.py --probe build/android-managed-launcher/classes.dex \
    --main-class ActivityProbe --services --installed-app android-state/com.tiltingpoint.spongebob \
    --play --timeout 90
```

With SpongeBob 5.12.0 this passes in JIT (1,995 frames, `APP_RUNTIME_SERVICES_OK
resumed=true`). The game's log now shows `Called Notification enabled from
Native : false` after each check, and the user confirmed the main HUD stays
after closing Settings. The foreign-package `SecurityException` path is not
exercised by a test.

The same resumed check prints `APP_APP_OPS_OK`.
`AppOpsManager.checkPackage` accepts the installed package for the app uid
and throws `SecurityException` for a missing package, `root` and
`com.android.shell`. It passes in JIT play mode (1,725 frames); interpreter
mode was not run.

## Configurable Android back key (2026-10-12)

`test-android-window-backend` checks key-name lookup (`escape`, `f12`, `a`,
`0`, `none`, rejected `f13`/`esc`) and that host events through
`pump_host_events` deliver the configured key as `KEYCODE_BACK` while other
keys keep their codes, and unchanged with `none`. `bifrost-emu
--print-config` shows `[android] back_key`, the environment override applies
and an unknown name exits with status 2. `test_managed_activity_stack` adds a
round closed by `KEYCODE_BACK` through the stacked window's view tree, which
exercises `onBackPressed` and `getTaskForActivity`; the package suite passes
26/26. Pressing Escape in a real game window was not exercised by a test.

## Managed Activity stack (2026-10-12)

`PackageTests.test_managed_activity_stack` (`ManagedActivities`) starts a
second Activity for a result above a SurfaceView Activity. It checks that an
implicit browser Intent raises `ActivityNotFoundException`, then the order:
root paused, second created (extra and caller delivered), resumed, focused,
its display-sized window on the host layer with "Close second", the button
click, second paused, `onActivityResult(42, RESULT_OK, answer=42)`, root
resumed, second destroyed and its layer removed. The full Android package
suite passes 26/26. That run also repaired two regressions from the
session-owned device services commit (`FrameworkDisplayProbe` and
`FrameworkPackagesProbe` reflected the old `installServices()` signature) and
an older test defect: the NativeActivity launch test read the app's output
from the CLI's stdout/stderr, which the session has written to its log since
`df3a944`. A real ad has not yet been shown: Kidoz returned no fill during
the SpongeBob session after the change.

## Session-owned device services and JIT perf map (2026-10-12)

`PackageTests.test_managed_service_execution` now also requires the app's
`sensorservice` entry to remain a local Binder while its `:remote` service
child runs. With service children publishing again it fails with
`service child replaced sensorservice: android.os.BinderProxy`; with the fix
it passes. SpongeBob 5.12.0 no longer freezes after its WebView ad process
exits (no `sensorservice died` or service waits in the log).

A 25-second `perf` capture of SpongeBob's main restaurant screen with
`BIFROST_PERF_MAP=1` shows a single guest thread at ~98% of one core:
~24% of its samples in `libil2cpp.so`, ~20% in `libunity.so`, ~19% emulator
side (`run_block` ~9%) and the remainder spread over blocks too small to
rank. The hottest block is a two-instruction il2cpp float getter (~3%), so
call/return cost dominates rather than one hot loop. This is one capture on
one screen, not an FPS benchmark.

## Self-loop writeback and framework popup windows (2026-10-12)

`jit_selfloop_writeback` runs Android 15 Bionic's `__strrchr_aarch64`
instruction words unchanged: unaligned scans first, then 896 checks across
offsets, path lengths and slash/non-slash surroundings against a C reference.
Before the fix 168 checks failed (aligned 104-byte path: index 73, expected
94, the value captured from SpongeBob); after it all pass, as under QEMU. With
the fix, SpongeBob 5.12.0's first launch extracts `il2cpp/Resources/*.dat` and
`il2cpp/Metadata/global-metadata.dat` correctly and no longer shows "Not
enough storage space to install required resources."

`PackageTests.test_managed_dialog_and_popup_windows` (`ManagedDialog`) checks
an `AlertDialog` over a SurfaceView: layout at the framework-computed frame,
the host layer being shown, focus moving to the dialog and back, a button
click through the dialog's view tree, then a `PopupWindow` at `[40,60][280,140]`
and removal of both layers. It passes with `SDL_VIDEODRIVER=wayland` and
`x11`. The plugin host test accepts an original-size v2 descriptor (popups
`-ENOTSUP`) and forwards popup pixels unchanged when the callbacks are present.

The strict JIT suite passes 314/314 and `--no-jit --strict` 305/305 with nine
JIT-only skips. `make verify` (21,836 SSA checks, 76 differential fixtures,
six threaded oracles), `make generated-check`, the headless build,
`test-android-window-plugin`, `test-android-window-backend`,
`test-android-sdl-backend` and the Android session/launcher package tests
pass. Not covered: dialog visuals were confirmed by the user on one desktop
only, and long gameplay sessions were not run.

## Per-page code invalidation index (2026-10-12)

`jit_cache_invalidation churn` adds a code-page phase: 4,096 rewrites of one
warm function among 32,768, each published with IC maintenance and checked,
followed by same-page neighbour checks. Three runs per binary measured 1.11 s
before and 0.057 s after; the data-only phase stays near 0.007 s. All runs
pass, as does `BIFROST_NO_CODE_PAGE_FILTER=1` (full-scan control).

The strict JIT suite passes 312/313 and `--no-jit --strict` 304/304 with nine
JIT-only skips. The one JIT failure, `xwayland_wire` (X `BadMatch` on
`GetImage`), is intermittent and predates this change: ten alternating runs
failed 4/10 on the new build and 5/10 on the previous one, with other emulator
windows open. `make verify` (21,836 SSA checks, 75 differential fixtures, six
threaded oracles), `make generated-check` and the headless build pass.

SpongeBob 5.11.5 `perf` captures (`cpu-clock:u`, 199 Hz, 30 s) attributed
54.3% of user samples to `invalidate_range` before the change, 90 s into
loading. A capture on the new build about 3.5 minutes in, under a lighter
load (1,973 versus 5,670 samples), contains no `invalidate_range` samples and
70% generated code. The captures are not phase-matched; this is not an FPS or
load-time measurement.

The managed launcher's PixelCopy teardown handling was rebuilt into the DEX.
Its clean window-close exit has not yet been observed in a live session.

## Native acquire loads and trial timing (2026-10-12)

The atomic-stress fixture now checks acquire-load byte/halfword/word/doubleword
results, source/destination aliasing, NZCV, XZR and preservation/non-creation
of exclusive reservations. An acquire-only function exercises direct-window
initialization without an ordinary load/store. Initial native-path stress
exposed a missing window-base preload; baseline and software-guard paths
passed the same fixture, and the corrected native entry passes too.

The fault fixture checks all four acquire widths at warmed protected-page
sites on four CPUs and at a shared-file EOF boundary. JIT and QEMU pass the
SIGSEGV and optional native SIGBUS probes. The existing software/interpreter
EOF coverage limitation remains. Atomic results also pass in QEMU and the
reference interpreter, with forced software guards checked separately.

The real-timing oracle checks that eight-block linear candidates finish
sixteen controlled windows within 128 live visits, and that slot collisions
still terminate within the new 8,192-entry budget. Its timing clocks remain
real. The saved baseline fails the new visit-bound regression; ST/MT and both
frame layouts pass after the change. Discovery, feedback and MT publication
oracles also pass. Both build configurations, `make generated-check` and
`make verify` pass (21,836 SSA checks, 75 differential fixtures, six threaded
oracles). The strict JIT suite passes 313/313; 56 Tier-2-enabled utility,
SIMD/fault/atomic checks and three selected interpreter checks pass.

One warm-up and five interleaved before/after samples of a temporary
two-million-load probe give 32.4066/2.4128 ms median times (13.43x throughput).
Tier-2 is disabled, W^X disabled and guard settings matched; every sample
checks the final sum. This is an isolated acquire-loop result. The repeat
Doom 3 profile was stopped at the user's request before loading completed;
there is no post-change gameplay capture, frame-time comparison or new
CoreMark result. Local artifacts are in `/tmp/bifrost-atomic-trials/`.

## Concise Tier-2 logging (2026-10-12)

The existing discovery fixture passes in ST/MT with both frame layouts under
compact and verbose tracing. Compact output contains only compiled-candidate
and timing-decision Tier-2 records; verbose tracing alone also enables those
records and restores discovery/compiler details. The fixture batch emits
40 compact versus 1,064 verbose Tier-2 lines. Existing real-timing regressions
pass with compact logging, and both SDL/GL and headless library builds pass.
`make verify` passes 21,836 SSA checks, 75 differential fixtures and six
threaded oracle runs. This is a logging change; no gameplay/FPS result is
established by these checks.

## Planned codegen and CoreMark acceptance

The [state-retention codegen plan](JIT.md#planned-codegen-retaining-guest-state-across-regions)
and [roadmap milestones](roadmap.md#jit-codegen-retain-guest-state-across-hot-regions)
are future work. Use these acceptance gates as each stage is implemented:

- Add focused result/reference checks for mixed scalar/vector aliases,
  partial writes, Q=0 clearing, NZCV preservation, register-copy cycles,
  spills and loop-carried values. Exercise helper read/write/clobber effects
  and full-synchronization fallback for unclassified helpers.
- Verify precise state at faulting instructions and signal/host boundaries,
  including writes carried from earlier iterations. Cover interpreter
  fallback, userfaultfd, mapping changes and non-contiguous region dependency
  invalidation. Recovery metadata and adapter targets must outlive execution.
- Check canonical and specialized entries in ST/MT and both frame layouts,
  concurrent publication/revocation, bounded leases and stale variant calls.
  Snapshot published MT bytes to verify instruction immutability.
- Run `make verify`, relevant JIT/interpreter suites and real applications.
  Report coverage exclusions explicitly; the current differential replay
  gate disables Tier-2 and cannot independently validate region handoffs.
- Measure eliminated state loads/stores and spills, code/metadata growth,
  compile time and equivalent live-work trials. Follow with matched cold/warm
  gameplay routes reporting frame times, demanding-scene minimums and stutter.
  Use representative Linux hosts and available guard/ISA fallback modes.

CoreMark acceptance uses the existing benchmark runner and validates every
labelled CRC, seed and minimum ten-second duration under the
[upstream reporting rules](https://github.com/eembc/coremark/blob/main/README.md).
Record the binary/revision, compiler flags, host, thread count, Tier-2/W^X
settings, warm-ups and all measured runs. Preserve the fixed 110,000-iteration
guest comparison while it meets the duration rule; report guest-timer
iterations/s and process wall time separately. The roadmap target is at least
10,000 reported iterations/s. The current 16.0753 s median wall time is a
recorded baseline, not a prediction for the planned compiler.

The 26%-of-native target is conditional on a validated native reference near
38,460 iterations/s. Match source, seeds, workload and thread count, document
the native build settings, and choose enough iterations for each mode to meet
the duration rule. Compare normalized iterations/s, retaining CRC validation
for both modes. No new native baseline or architecture-performance result is
established by this documentation update.

## Adaptive superblock size retries (2026-10-12)

The measurement oracle reproduces the previous permanent rejection of an
eight-block candidate, then exercises real CPU-time losses through 8 -> 4 -> 2
and selection of a faster four-block candidate. It checks fresh counters and
branch profiles between trials, inert retired wrappers, terminal fallback,
the 32 MiB compilation stop, head invalidation between translations and
reopening the eight-block search after a selected region changes phase.
The controlled delays exercise policy rather than predicting application
speedup. All cases pass in ST/MT with ordinary and chain-skip frames; MT
publication bytes remain unchanged.

Both SDL/GL and headless library builds, generated-code checking and
`make verify` pass (21,836 SSA assertions, 75 differential fixtures and six
threaded oracles). Discovery, feedback, MT publication, MT execution and
twenty mutex promotion stress runs pass. With Tier-2 enabled and W^X disabled,
56 selected SIMD/fault/atomic and utility tests pass, including host-validated
zstd round trips and BusyBox/toybox. These are local source and executable
checks, with GPU access for emulator runs; no new instruction semantics or
external specification claims are involved.

The strict default JIT suite passes 313/313 with GPU access. Fixed
110,000-iteration CoreMark passes every labelled CRC and the ten-second
minimum: one warm-up and three runs give 16.0753 s median wall time
(16.0606–16.0850 s). Tier-2 is enabled and W^X disabled. The earlier eight-block
batch was 15.8224 s; these separate batches do not establish a throughput gain
or isolate the cause of the roughly 1.6% difference. The controlled timing
tests establish candidate selection, not game FPS or universal speedup.

## Eight-block superblocks and region vector state (2026-10-12)

`test_jit_tier2_discovery.sh` now checks eight-block linear promotion, all eight
conditional side exits, interior-code invalidation, scalar/vector cached
regions, Q=0 clearing, aliased arithmetic and NaN helper recovery. A fault at
the head on the second iteration verifies that state written later on the
preceding trip is committed before guest fault delivery. FP memory transfers
also check independent GPR/FP registers sharing the same index. Twelve
invariant/accumulator pairs check preheader-home preservation under pressure.
The pre-fix planner redefined an invariant home; the saved original emulator
also crashed on single-thread zstd with Tier-2 enabled. Both checks pass after
the preheader-home repair.

`test_jit_tier2_measure.sh` covers pooled linear windows with real controlled
CPU work: faster candidates are selected, slower and mismatched-work candidates
are rejected, and MT code bytes stay unchanged. Discovery, measurement and
feedback pass in ST/MT with ordinary and chain-skip frames. MT publication,
eight-CPU arithmetic/invalidation, opt-out checks and twenty mutex promotion
stress runs pass. Cache-disabled, forced-software-guard and no-FMA3 discovery
paths pass as additional compatibility checks.

The SDL/GL and headless library builds pass, along with the C API (57/57)
and native bridge (61/61) checks. `make generated-check` and
`make verify` pass (21,836 SSA assertions, 75 differential fixtures and six
threaded oracles). With host GPU access, the strict suite passes 313/313 in
JIT and 304/304 in the interpreter, with nine intentional JIT-only skips.
The sandbox hid `/dev/dri`; its seven graphics initialization failures were
resolved by rerunning with real GPU access. A separate Tier-2-enabled run
passes 56 selected SIMD/fault/atomic and real-world utility checks, including
host-validated zstd compression/decompression and BusyBox/toybox workloads.
These tests establish neither arbitrary game stability nor game FPS.

The discovery executable's `benchmark` mode validates exact final lane values
for two million ADD iterations. Five alternating cache-on/off measurements
gave uncached/cached medians of 7.240/1.789 ms (2D vector), 5.428/2.767 ms
(2S vector), 5.536/2.261 ms (scalar D), and 5.433/2.263 ms (scalar S).
Fixed 110,000-iteration CoreMark runs validated every labelled CRC and the
ten-second minimum: three-run wall-time medians were 15.8101 s before,
15.8224 s with eight-block superblocks, and 15.5748 s with four blocks.
Promotion was enabled and W^X disabled consistently in those comparisons.
The vector measurements isolate caching; CoreMark shows no material gain
from the eight-block setting in this batch.

The test runner is [`Scripts/Testing/run_tests.sh`](../Scripts/Testing/run_tests.sh). It does
not build guest fixtures; use `make setup-tests` for the C and assembly
fixtures, or `make cross SRC=... OUT=...` for one C test. Missing fixtures are
skipped by default and fail with `--strict`. An ELF older than its matching
C or assembly source fails with a rebuild instruction, so ignored local
fixtures cannot silently test outdated code. Some integration tests also
require SDL2, a display, or a configured guest rootfs.

## Latest full validation

### Indirect branches, watchdogs and lazy controllers (2026-10-12)

This follows commit `1878b2b` and includes the IBL differential-replay fix.
Both SDL/GL and headless executable/library configurations build, fixtures
were rebuilt, and `make generated-check` passes. With GPU access, the strict
default JIT suite passes **313/313** and interpreter **304/304** with nine
JIT-only skips. The strict ordinary-frame JIT suite also passes **313/313**.
This includes BusyBox/Toybox, zstd round trips, input/gamepad
probes, signal-context restoration and the main/worker long-call regression.
`make verify` passes **75 differential fixtures and six threaded oracle runs**,
plus the SSA gate. MT opt-out/publication and Tier-2 discovery, measurement,
feedback and twenty mutex promotion-stress runs pass.

The concrete verifier failure was reproduced with
`BIFROST_JIT_VERIFY=1 BIFROST_NO_DIRECT_CALL=1` on `jit_bitfield_regalloc.elf`:
14 divergence messages before the fix, none with ordinary frames, and none
after unbinding IBL only during the measured native block. The gate previously
failed despite that fixture's arithmetic oracle returning success. The fix
preserves normal IBL execution outside the replay scope.

The extended `jit_mt_publication.cpp` fixture warms both the real signal-return
trampoline and its continuation. Repeated signal deliveries prove that RET
can take an IBL hit into the trampoline, while its SVC returns with restored
PC, SP, LR, NZCV and mask before the continuation increments a register.
Executable host stop PCs and nested helper depth also prevent a warmed target
from executing. These checks pass in ST/MT and chain-skip/ordinary layouts.
The existing signal-context guest test covers nested alternate stacks,
handler-edited context and malformed frames in both guest engines.

Source verification: Linux v6.12's
[AArch64 signal implementation](https://github.com/torvalds/linux/blob/v6.12/arch/arm64/kernel/signal.c)
and [vDSO trampoline](https://github.com/torvalds/linux/blob/v6.12/arch/arm64/kernel/vdso/sigreturn.S)
confirm the handler LR/trampoline and saved-PC restoration contract. The
dispatch/SVC boundary and callback-runner audit are Bifrost source inspection
and host-test observations, not Linux ABI claims. The installed unpatched
QEMU passes 46 of the signal fixture's 48 checks but fails the two unblockable
mask checks described [below](#memory-and-signal-regressions); it is not a
passing reference run. Physical controller hot-plug, startup/branch timings
from the commit message and game playability were not revalidated here.
Initial sandbox graphics failures disappear when rerun with GPU device access.

### Yggdrasil sandbox and path syscall fixes (2026-10-12)

Both configurations build and fixtures were rebuilt. The strict JIT suite passes
**312/312** and the interpreter suite **304/304** (eight JIT-only skips). This
includes the new `test_path_syscalls` and the extended `test_sandbox`
(**84 checks**), followed by the host-side canary check. `make verify` passes 75
differential fixtures and six threaded oracle runs, and the C API passes 57/57.
Under `--rootfs` (the same sandbox), the twelve Bionic bootstrap and three
AAudio callback combinations pass. The JNI fixture passes in JIT and
interpreter modes, and the four framework display/package contracts, managed
service execution, the five network/launcher tests, Asgard and the framework
probe pass.

Reference comparison: `test_path_syscalls` passes on qemu-aarch64 and on both
engines, and fails 13 of 14 checks on the previous commit's binary. On the
previous binary, `test_sandbox` failed 24 of 84 checks, and the guest emptied
the outside canary, renamed a sandboxed file out next to it and hard-linked it
into the root. The fixed binary leaves the canary's content, mode, link count
and directory unchanged. ART's behavior was not re-derived beyond these runs.
The kernel interfaces used (`openat2 RESOLVE_IN_ROOT`, O_PATH with
`AT_EMPTY_PATH`, `/proc/self/fd` magic links) were checked against the man
pages and kernel source; `utimensat AT_EMPTY_PATH` is avoided because older
kernels reject it. Kernels without `openat2` keep the documented textual-only
confinement, and that fallback was not exercised. Confined metadata calls cost
an extra open/close per call under `BIFROST_ROOT` only; this was not timed.

### 2.0.0-alpha.1 layout, names and splits (2026-10-12)

Both SDL/GL and headless configurations build with no new warnings, and
`make generated-check` reports no errors. Fixtures were rebuilt with `make
setup-tests`. The GPU-enabled strict suite passes **311/311**; the interpreter
suite passes **303/303**, with eight JIT-only skips (including the renamed
`test_compat_syscalls`). The JIT gate passes **75 differential fixtures and six
threaded oracle runs**, and the MT oracle passes. C API (**57/57**), native
bridge (**61/61**), SSA (**21,836 checks**), audio frame counters, Android
window/backend/plugin checks, twelve plugin CLI cases and 256 SDL3 teardown
cycles pass. `--version` and `bifrost_version()` report 2.0.0-alpha.1.

Moved code was checked separately from the tests. All 139 syscall case bodies
moved out of `Miscellaneous.cpp`, `FileSystem.cpp` and `EventIO.cpp` are
byte-identical to their originals after the private-member-to-accessor rewrite.
The top-level case-number multiset across all handlers is unchanged except for
the removed dead `setresgid` duplicate. Splitting `Memory.cpp` and `IR.hpp`
preserved every original line; only boilerplate (headers, includes, namespace
and an `extern` for the memstats counters) was added. All 155 library sources
are present as archive members.

On the guest side, `make android-runtime-adapters` builds every adapter and
launcher from the new paths, and `make android-managed-launcher` builds and
dexes the split launcher. Its manifest also satisfies the staleness check.
Bionic bootstrap passes all twelve services/ASLR/engine combinations, and
managed Bionic AAudio callback threads pass JIT, interpreter and Tier-2.
Framework display and package contracts pass in both engines (four tests),
including the moved SurfaceComposer/vsync services. Managed service execution,
the five network/launcher-argument tests, Asgard (eight processes, 1,600 RPCs,
broker restart), the three preflight tests and the JIT framework probe pass.

`test_android_jni.py` had failed before VM creation, both on this change and on
a binary built from the previous commit. Android-runtime guests run as app UID
10000, and API 35 ART's `DexFile_openDexFileNative` rejects a DEX for which
`access(W_OK)` succeeds unless the UID is root, system or shell ("Writable dex
file ... is not allowed"). The runner copied `classes.dex` writable. It now
stages the file 0444, as the managed launcher already does, and the fixture
passes in JIT, interpreter, `BIFROST_NO_ASLR=1` and qemu-aarch64 reference
modes. When the host runs as root, `faccessat` passthrough reports even 0444
files as writable, so both this fixture and the managed launcher would still be
rejected; that case is untested. No game was run; these checks do not establish
playability.

### Bounded MT self-loops (2026-10-11)

The strict GPU-enabled JIT suite passes **311/311** with IR validation and
register-allocation checks. `make verify` passes **75 differential fixtures,
six threaded oracle runs and 21,836 SSA host checks**. Concurrent execution
uses independent oracles rather than shared-memory differential replay.

The expanded `jit_mt_publication.cpp` oracle covers SUBS/B.NE, CBNZ, TBZ,
vector FADD and loop-carried ADCS carry. Eight CPUs execute each loop while
the host invalidates its code; final values, NZCV and vector lanes are checked.
Separate checks cover lease expiry, an initially zero lease, callback stop
sentinels, stale body revocation after retranslation and synchronized code
rewrites. Published instruction bytes remain unchanged. Its runner now checks
both frame layouts. The oracle also passes with self-loops disabled, GPR pins
disabled, and AVX2/FMA3 disabled.

`test_jit_mt.sh` passes default, Tier-2 enabled/disabled, chaining disabled and
ordinary-frame modes, checking both results and actual edge/region publication.
The full suite also includes Tier-2 discovery, feedback, measurement and
twenty mutex promotion-stress runs. Headless publication checks and the final
GPU-enabled executable/library build pass.

A temporary host harness times 200 million iterations using
`CLOCK_THREAD_CPUTIME_ID`, checking a closed-form integer sum and exact vector
results. The first before/after batch has four measured samples per loop after
one initial sample, with default guards, block cap and Tier-2 disabled:

| Loop | Before median CPU ms | After median CPU ms |
|---|---:|---:|
| SUBS/B.NE integer sum | 285.9 | 286.0 |
| CBNZ integer sum | 239.2 | 195.2 |
| CBNZ with vector FADD | 1535.5 | 241.0 |

A second batch measured 329.1/286.1, 284.4/195.2 and 1848.0/241.1 ms,
respectively. Background load was not controlled; thread CPU time excludes
descheduling but cannot eliminate frequency/cache interference. The vector
and counter loops benefit consistently; the flag-heavy loop has no stable
gain established by these samples.

Real AArch64 zstd compression with four workers on a temporary 16 MiB corpus
passes native decompression and byte-for-byte comparison in both before/after
runs; verbose logs confirm MT mode. Two wall-time samples were 0.433/0.420 s
before and 0.400/0.416 s after, too few and too short to establish a reliable
application gain. The standard zstd regression also passes both engines.
No interactive game was launched for this change, and game FPS, complete MT
scaling and the reported Doom freeze remain unverified.

### Flag arithmetic register pressure (2026-10-11)

The allocator-aware ADDS/SUBS/TST emission and two-input pinning pass the strict
GPU-enabled JIT suite **311/311**, with IR validation and register-allocation
checks. `make verify` passes **75 differential fixtures, six threaded oracle
runs and 21,836 SSA host checks**. Tier-2 discovery, feedback and measured
promotion pass with single/multithreaded execution and both frame layouts;
mutex promotion stress passes twenty runs, and immutable-code publication
passes its host oracle.

`jit_same_operand` now holds twelve independent values across eighteen W/X
ADDS/SUBS/CMP/CMN/TST forms, including destinations aliasing either source.
It checks 1,152 operation/input combinations, repeated four times, against a
C result/NZCV oracle. The rebuilt fixture passes the interpreter, QEMU and
JIT verification, as well as the ordinary frame layout, block cap 128, SSA
disabled and forwarding disabled. The complete interpreter suite was not
repeated for this backend-only change.

The x86 CMP encoding and its SUB-equivalent flags were checked against the
[Intel SDM](https://cdrdv2-public.intel.com/868140/253666-089-sdm-vol-2a.pdf#page=279).
The test counts and performance figures are local observations. CoreMark
allocator diagnostics count **455 → 394 emitted spills** over the same 354
benchmark-body blocks (`0x400ba0..0x4035b0`, excluding libc), including cold
fault paths; this does not count executed memory traffic or prove a game FPS
improvement. Block sizing and memory guards retain their existing defaults.

Three alternating before/after pairs at 110,000 CoreMark iterations validated
all CRCs and produced median reported rates of 7,322 → 7,602 iterations/s.
These timings are **provisional**: the user was running Doom 3 concurrently,
so they do not establish an isolated performance gain. Seven further pairs
of short MIPS, sorting and matrix benchmarks validated their result markers;
their wall-time medians differed by about 0–2%, also under competing load.
A controlled performance comparison remains outstanding. No game FPS or
freeze fix is claimed.

### Subsystem layout and source splits (2026-10-11)

The completed layout builds from empty object directories in both SDL/GL and
headless configurations. The GPU-enabled strict suite passes **311/311**, with
no skips; the interpreter suite passes **303/303**, with eight JIT-only skips.
All optional fixture binaries were present for these runs. The JIT gate passes
**75 differential fixtures and six threaded oracle runs**, with IR validation
and register-allocation checks; the SSA host oracle passes **21,836 checks**.

C API (**57/57**), native bridge (**61/61**), audio frame counters, Android
window/backend/plugin checks, twelve plugin CLI cases and 256 SDL3 teardown
cycles pass. Bionic bootstrap passes all twelve services/ASLR/engine combinations;
managed Bionic AAudio callback threads pass JIT, interpreter and Tier-2. Framework
package and display contracts pass in both engines (four tests). Android network,
identity, DNS and launcher-argument checks pass ten tests, with three preflight
tests and the verification-gate unit test also passing.

Asgard passes mixed-engine RPC, peer-death/notification and broker-restart
checks, including eight concurrent processes and 1,600 RPCs. Audio callback
stress passes twenty runs across both engines and normal/delayed callbacks.

All nine generated consistency targets and the recursive headless graphics SDK
check pass. An isolated fixture using the actual Makefile verifies source
rename/removal, removal of obsolete archive members, executable relinking and
an unchanged incremental rebuild. Python and shell tooling syntax checks pass.

A follow-up path audit found and repaired the optional SDL2 downloader's
incorrect capitalization of the SDK's `include/SDL2` directory and stale build
instructions. An isolated synthetic `.deb` fixture fails on the prior script
and passes fresh extraction, configuration-header compilation, runtime-library
symlink and cached rerun checks on the repaired script. This does not test a live
APT download. Source navigation comments now point to the split implementations.

These are observed local test results, not a new ISA/API specification audit.
Graphics/application smoke tests do not establish sustained gameplay or resolve
the previously reported game freezes. Clean builds still emit existing warnings
about a possible uninitialized SIMD arithmetic value and an unused input helper;
those paths were not changed by this refactor.

### Native vector FP NaNs and exceptional arithmetic (2026-10-09)

The fixed JIT passes **308/308** strict tests with IR validation. The interpreter
passes **300/300**, with eight JIT-only skips. These counts describe the local
working tree, including its separate CPU-topology changes. Differential replay
passes **74 fixtures**, with **six threaded oracle runs**; the runner selects
`no-trap` for the Android semantics fixture and uses the concurrent host-fault
fixture's own oracle, since shared-memory replay is unsupported.

`jit_simd_fp_nan` passes **33/33** against QEMU, JIT, interpreter, cache opt-out,
no-FMA, Tier-2/threaded mode, and register/memory verification with allocation
checks. A temporary build forcing JIT CPU detection to SSE2 also passes all 33
with verification. This tests the MOVMSK fallback without requiring old host
libraries; the installed host libraries fault before startup under a Core 2
QEMU CPU model. All five MT gate modes pass, and the captured Doom instruction
harness passes **20,000** comparisons with no mismatches or IR fallbacks.

The installed SpongeBob app reaches all nine framework/app checkpoints and
`onCreate` in **17.5 s**, without a guest failure. This headless probe presents
zero frames and does not validate gameplay or the long loading screen.

The finite indexed-FMUL/FNEG benchmark retains correct output. Seven alternating
before/after runs give median **9.163 → 9.837 ms** in single-threaded mode and
**49.944 → 57.310 ms** after entering sticky MT mode: about **7%/15% more time**
for the semantic guards. These are isolated throughput costs, not gameplay FPS.
Finite arithmetic remains native, but this correctness fix has a measured cost.

### JIT invalidation and cached vector code generation (2026-10-09)

The JIT unit suite with IR validation passes **128/128**, including the
per-thread invalidation row (82 s). All five `test_jit_mt.sh` modes pass their
arithmetic/invalidation oracles and require the expected edge/region
publication. The captured Doom harness passes **20,000** comparisons with zero
mismatches and zero IR fallbacks. On this checkout its standalone link needs
`-lX11 -lwayland-client` in addition to the script's SDL/GL libraries.

The unary/FABD fixture passes 541 assertions and the indexed-FMUL fixture
passes 28 under normal JIT, vector-cache opt-out, register/memory verification,
Tier-2 opt-in, QEMU and the non-FMA fallback. Their `--mt` modes pass QEMU,
JIT and interpreter. The six pre-fix cached FABD failures erased the upper
lanes; QEMU and the uncached JIT agreed with the expected results.

The installed SpongeBob launcher initialization passes all app/framework
checkpoints in 17.5 s on the new build, exiting zero without a guest failure.
This is the headless `onCreate` checkpoint, with zero presented frames; it
does not establish gameplay performance or rendering correctness. Existing
interactive sessions were left running on their original binaries.
The scoped vector-throughput and data-mapping benchmarks, their results,
and the remaining native-FP semantic gaps are documented in [JIT.md](JIT.md).

### SpongeBob managed Activity creation (2026-10-07)

The installed SpongeBob 5.11.5 app probe reaches `APP_CLASSLOADER_OK`,
`APP_APPLICATION_STARTED`, `APP_ACTIVITY_CREATED` and `FRAMEWORK_OK`, exiting
zero in both engines (26.2 s JIT, 79.5 s interpreter in the reviewed runs).
The real launcher is `com.clevertap.unity.CleverTapOverrideActivity`; logs also
show native `libmain.so` loading and IL2CPP `JNI_OnLoad`. No readiness-property
override is needed after the fix. See [the runnable command and bounds](Android/ANDROID_RUNTIME.md#installed-app-creation-checkpoint-2026-10-07).

The original first fatal call was the strict local Activity controller's
`getRequestedOrientation`. Its manifest initialization and request tracking
were checked against AOSP Android 15, and the built-in Activity probe checks
the get/set round trip in both engines (7.9 s JIT, 19.1 s interpreter).
Native libbinder's subsequent wait for `servicemanager.ready` was reproduced
as a 90 s timeout, isolated with a property override, and checked against AOSP's
readiness gate. The runtime now reflects whether the registry is selected.
All **12/12** Bionic variants pass: services off/on × fixed/random ASLR ×
JIT/interpreter/Tier-2. Binder device/prctl checks are required, rather than
being skipped when the host test environment is not forwarded into the guest.
The neighboring Binder/syscall gate passes **5/5**.
An older built-in-only ActivityProbe DEX still exits zero with `FRAMEWORK_OK`,
but the installed-app runner correctly rejects it for missing app checkpoints.

These are initialization and regression observations on this Linux host, not
gameplay or cross-distribution validation. The probe exits after `onCreate`.
Content providers, Activity start/resume, drawable window attachment and input
are untested. Logs still report missing user/clipboard services, SQLite locking
errors and SurfaceFlingerAIDL transaction failures. The normal APK runner still
rejects managed launchers; only this diagnostic path executes them.

### JIT host-fault recovery (2026-10-07)

The final full JIT suite passes **303/303**, zero failures/skips (116 s),
including the added main-CPU interpreter row. The final focused memory/cache
gate passes **8/8** after review. The full interpreter suite passes **295/295**,
zero failures and eight expected JIT-only skips (156 s). A final read-only
CAS-loop case was added afterward and both affected rows pass again.

The four-CPU `jit_host_fault` regression completes 3,588 recoveries with LSE,
including scalar/vector faults, crossing a page boundary, saved registers,
NZCV and vector state. `BIFROST_FAULT_TRACE=1` confirms all 3,588 went through
registered host sites. It also passes with Tier-2 at threshold 32, chain-skip
and IR validation, and with the software guard forced. Its `single` mode passes
under the interpreter; both modes and the optional `bus` probe pass QEMU.
The four scalar/vector file EOF SIGBUS probes pass native JIT. They do not cover
paired file EOF faults or software-only/interpreter EOF fault handling.

The interpreter's worker-thread loop reports faulting accesses as uncaught
exceptions. This was reproduced with the previous binary as well, so the
four-CPU regression is JIT-only and the main-CPU row checks the interpreter.
Temporary host checks confirm an unregistered C++ access into the reservation
still raises host SIGSEGV and a previously installed host handler is called.
Those helpers live in `/tmp`, not the repository.

The real Android Activity probe passes in 8.6 s. A Teeworlds client initialized
graphics/audio, logged editor activity and exited cleanly after about 54 s;
the intended five-minute timeout was not reached, so this is bounded application
validation rather than five-minute stability evidence.

Five alternating executions per mode on the same host give these median wall
times, including startup and translation, using the same binary with/without
`BIFROST_JIT_SOFTWARE_GUARD=1`:

| Fixture | Software guard | Host recovery |
|---|---:|---:|
| bench_mips | 0.334 s | 0.332 s |
| bench_memcpy | 1.594 s | 1.473 s |
| bench_sort | 0.629 s | 0.585 s |
| bench_matrix | 0.277 s | 0.251 s |
| bench_fib | 0.765 s | 0.716 s |

These synthetic observations show about 0–9% shorter execution in this sample;
they do not establish game FPS gains or results on another Linux host. The
signal API/context and async-signal-safety design were checked against Linux
man-pages linked from [JIT.md](JIT.md#registered-native-memory-faults).

### Guest memory as one host reservation (2026-10-07)

The default suite passes **301/301** (114 s), `--quick` **296/296** and
`--no-jit` **294/294** with seven expected JIT-only skips (155 s). The
Android framework probe passes in both engines (JIT ~5 s, was ~11 s) and the
Activity probe in 8.4 s JIT / 18.0 s interpreter (22.6 s / 45.0 s with the
previous binary, same host). bench_mips/memcpy/sort/matrix/fib are within
noise of the previous binary (their memory stays below 4 GiB).

### FP/SIMD interpreter split, NaN rules, MSR NZCV (2026-10-07)

The default suite passes **300/300** (159 s), `--quick` **295/295** and
`--no-jit` **293/293** with seven expected JIT-only skips (145 s).
A 300k random AdvSIMD/FP encoding differential (scratch harness, not in the
suite) matches the pre-split interpreter on every QEMU-legal encoding except
two-NaN payload choices, which now follow the ARM ARM; it also showed that
some reserved encodings run instead of raising SIGILL, and two
QEMU-legal encodings (0x4ED970E7, 0x4EC17117) leave host addresses in the
destination (open, see AGENTS.md).

### Teeworlds fixes (2026-10-07)

The default suite passes **298/298** (116 s), `--quick` **293/293** and
`--no-jit` **291/291** with seven expected JIT-only skips (145 s).
`test_dev_urandom` and `jit_simd_fixconv` pass under the JIT and the
interpreter; `jit_simd_fixconv` also under `BIFROST_JIT_VERIFY=1`.
Teeworlds was run twice: one run hit the `fcvtzs` SIGILL in the map editor
at 43 s (now fixed), the other was closed after about 2 minutes, so the
four-minute urandom exhaustion was not reproduced in the real game.

### Vector FP pairwise ops (2026-10-06)

The default suite passes **296/296** (117 s), `--quick` **291/291** and
`--no-jit` **289/289** with seven expected JIT-only skips (151 s).
`jit_simd_faddp` passes 14/14 under the JIT, the interpreter,
`BIFROST_JIT_VERIFY=1` and QEMU user mode; the pre-fix binary SIGILLs on its
first check. SpongeBob's native probe loads `libunity.so` in both engines and
all 21 libraries with the Firebase App preload (JIT, 21/21 clean exits).
The Android package tests pass 6/6.

### libandroid loopers, choreographer and sensors (2026-10-06)

The default suite passes **295/295** (128 s), `--quick` **290/290** and
`--no-jit` **288/288** with seven expected JIT-only skips (149 s).
`test_android_looper` (39 checks) passes 10/10 runs in each engine; its
frame-timing checks allow for vsync alignment. It covers per-thread loopers,
AOSP `addFd` rules, one-shot and reposted frame callbacks, delayed
callbacks, thread isolation, cross-thread posts and the no-sensor manager.
`test_android_activity` now prepares its looper with `ALLOW_NON_CALLBACKS`,
which its NULL-callback fd registration requires. SpongeBob's `libunity.so`
resolves every import in a fresh install; ART probes were not rerun.

### userfaultfd and CMC (2026-10-06)

The default suite passes **294/294** (179 s) and `--no-jit` **287/287**
with seven expected JIT-only skips (144 s). `test_userfaultfd` passes 67
checks under the JIT, the interpreter and natively on the host kernel. On the
schema-5 root ART runs CMC: `ActivityProbe` passes in 26.8 s JIT and 47.2 s
interpreter, including a full background mark-compact; the framework probe
passes in 10.2 s JIT. The probes ran concurrently with the test suite.

### Android Activity creation (2026-10-06)

The strict Linux suite passes **287/287 JIT** and **280/280 interpreter**,
with seven expected JIT-only skips (152 s and 141 s). On the re-provisioned
schema-4 default root, four concurrent runs pass. `ActivityProbe` takes
131.4 s JIT and 226.1 s interpreter; the framework probe takes 63.5 s and
119.0 s. Wall time is 227 s. The ART JNI fixture passes in both engines. The
loader fixture passes, and the package tests pass 6/6. All six Bionic
bootstrap variants pass. See [ANDROID_RUNTIME.md](Android/ANDROID_RUNTIME.md).

Focused additions:

- `jit_tier2_smov`: an SMOV-written, loop-carried `x3` must reach the next
  iteration both when the loop is fallen into (Tier-2 path) and when it is
  branched into, so the self-loop block sees the pre-SMOV value first. The
  pre-change binary fails the branch-in loop (4000 instead of 7999); QEMU
  and the interpreter pass.
- `test_jit_thread_invalidate`: two threads keep calling a JIT-translated
  page while main replaces it (`MAP_FIXED`, write, `mprotect` RX) 400 times
  on alternating pages; every call returns a published value. Runs with the
  shared JIT and again with `BIFROST_NO_SHARED_JIT=1`. QEMU passes; the
  pre-change binary dumps core in per-thread mode.
- `test_thread_exit_unmapped_tls`: 1,500 raw-clone threads on mmap'd stacks each
  unmap their own stack and TLS block and then `exit`, as Bionic's detached
  `exit_with_stack_teardown` does, while the next thread reuses the freed hole
  and two threads recycle same-size ranges. A canary at `TPIDR_EL0 - 0xa0` in
  every new thread must survive the previous thread's exit. The pre-change
  binary host-faults (`SEGV_ACCERR` in `Memory::write` from `syscall_exit`) or
  clobbers the canary; QEMU passes.
- `test_group_exit`: forked children with live threads: main SIGKILLing
  itself, a worker's `kill(getpid(), SIGTERM)`, and a worker's
  `exit_group(7)` (main sleeping or spinning) each end the process within
  10 s; a fork child's new thread reports the child's pid; main's
  `pthread_exit` keeps the process until a worker's `exit(5)`. Passes
  natively on the host kernel; the pre-change binary fails all 6. Signal
  deaths, including a default-disposition SIGSEGV from a `PROT_NONE`
  store, must reach the parent as `WIFSIGNALED`, not exit 128+n. On
  aarch64 it also checks BRK's SIGTRAP (default, handled and blocked).
  Blocked or ignored SIGSEGV and a blocked SIGILL must still terminate.
- `test_fcntl_lock`: POSIX `F_SETLK`/`F_SETLKW` input marshalling and
  `F_GETLK` writeback, unmapped/read-only buffers, forked lock contention,
  blocking unlock, signed offsets and ranges above 4 GiB. Passes on native
  Linux and under JIT/interpreter; the pre-change binary fails 9 checks
  because it passed guest flock pointers directly to host `fcntl`.
- `test_proc_rewind`: rewinds static `/proc/cpuinfo` and CPU `online`
  files and lazy `/proc/self/maps`, checking the actual offset and the
  reread content both before and after EOF. Passes on native Linux and
  under JIT/interpreter; the pre-change binary fails 6 checks because
  static memfd nodes reported a successful rewind without seeking.
- `test_proc_fd_reopen`: `open("/proc/self/fd/N")` reopens the guest
  descriptor's memfd or file with its own offset, including a guest-only fd
  number (100) and `/proc/self/fd/<dirfd>/name`; a closed fd gives `ENOENT`
  and a socket `ENXIO`. `openat2` resolves the same way and translates
  `O_DIRECTORY`. Passes natively on the host kernel; the pre-change binary
  fails 9 checks.
- `test_madv_remove`: `MADV_REMOVE` punches a hole in writable shared memfd
  mappings below and above 4 GiB (page reads zero, file size
  kept), returns `EINVAL` for private anonymous memory without touching it,
  `EACCES` for a read-only shared file, and `ENOMEM` for holes. The same
  source passes natively on the host kernel; the pre-change binary fails 8
  of 18 checks. QEMU 11.1 is not a reference (it returns `EOPNOTSUPP`).
- `test_mem_reservation`: guest memory above 4 GiB on the single host
  reservation: MAP_FIXED_NOREPLACE at 1 TiB with a JIT-hot loop, ACCERR after
  a read-only mprotect, MAPERR after munmap, a 1 TiB PROT_NONE reservation
  with a committed piece (under 2 s), a memfd seen through a 2 TiB and a low
  MAP_SHARED view, MADV_DONTNEED and mremap growth above 4 GiB. Passes
  natively; the previous binary fails the 1 TiB reservation (64 GiB cap).
- `jit_simd_fp_nan`: vector FP NaN choice and SNaN priority, +qNaN for
  invalid operations, signed zeros, fused FMLA/FMLS, FMULX inf*0, FABD and
  FADDP, plus cached high-register aliases, indexed operations, Q=0 inactive
  NaNs and a loop-carried source/NZCV check. All 33 expected results are
  checked against QEMU. The fixture is selected in both engine modes;
  `--mt` enters sticky multithreaded JIT mode. The old JIT fails 9 of the
  original 14 checks. Coverage excludes complete FPCR/FPSR and trap semantics.
- `jit_simd_widen_reduce`: native `S/UADDLV`, `S/U MAX/MIN V`, widening
  three-different ops (`ADDL`, `SUBL`, `ADDW`, `SUBW`, `MLAL`, `MLSL`,
  `MULL`) and `S/UADDLP`/`S/UADALP` for every size, Q, signed/unsigned and
  upper-half form against a C reference; 3312 checks pass under QEMU, both
  engines, JIT verify and IR validation. The old interpreter sign-extended
  `SADDW`/`SSUBW` byte lanes incorrectly.
- `jit_native_simd_gen` (generated by `make nativegen` from
  `Tools/CodeGen/native_simd.txt`; do not edit): every enumerated encoding
  the native SIMD classifier accepts (1,480) runs over 64 special inputs
  with FPCR.DN clear and set. Hashes are compared with
  `Tools/CodeGen/native_simd_expected.txt`, recorded under QEMU by
  `make nativegen-expect`. An encoding QEMU rejects with SIGILL (hash 0)
  fails, which caught 605 unallocated encodings the old classifier ran
  natively. Passes in both engines, JIT verify and IR validation.
- `jit_native_simd_inline`: USHL/SSHL `.2s/.4s/.2d` for all 256 count
  bytes (junk in the upper count bits) on four value patterns, checked
  against an ARM-pseudocode reference. Also EXT, FP compares, CMEQ #0,
  REV64, USHL and SSHL with Vd aliasing Vn or Vm, compared with the same
  instruction on distinct registers (6,166 checks). Passes under QEMU, both
  engines and `BIFROST_NO_AVX2=1`. Dropping SSHL's negative-count select
  from the inline body fails 2,578 checks.
- `jit_simd_reserved_forms`: reserved DUP (element/general), INS (general)
  and FCVTZS `.1d` encodings raise SIGILL. DUP (general) with ignored imm5
  bits executes. Vector FCVTZS/FCVTZU of ±inf, NaN and out-of-range values
  saturate (`.2d` interpreted, `.4s` native). The pre-change binary dies
  with a host SIGFPE; QEMU and both engines pass all 17 checks.
- `jit_simd_base_forms`: 139 base ARMv8.0 forms from the coverage sweep
  (compares with zero, saturating, doubling, narrowing and shift ops,
  conversions, estimates, AES, by-element with M:Rm, scalar element-0
  forms) hashed with FPSR.QC against QEMU cortex-a53, plus 30 reserved
  encodings that must raise SIGILL.
- `Scripts/Testing/simd_coverage.py` (not part of the suite): sweeps the
  FP/SIMD data-processing group under QEMU and Bifrost and writes
  `build/simd-coverage/report.txt` (see the JIT guide for its buckets and
  current baseline).
- `jit_simd_fp_lanes`: 196 encodings: FP pairwise max/min, FCMxx #0,
  seven vector FRINT modes, half FCVTL/FCVTN(2), all nine byte shift forms,
  vector FP arithmetic/FMLA/FABD/FSQRT, and scalar S/D 2-source ops, FABD,
  FSQRT, FRINT*, FCVT, vector FCVTXN(2) and the FMADD family (Ra aliases
  the preloaded Vd).
  Each runs over 96 NaN/±0/inf/tie/subnormal/out-of-range inputs with
  FPCR.DN clear and set. Per-encoding hashes are compared with a table
  generated by QEMU 11.1 (`gen` prints it). The previous build fails 67
  forms: scalar NaN sign/priority, ignored DN, and FCVTXN Q=0 not clearing
  the top half.
- `jit_lse_widths`: every LD/ST LSE op, SWP and plain/ordered CAS at all
  four sizes with dirty upper register bits and guarded neighbouring bytes
  (22848 checks against a C reference), then two threads running LDADDB on
  adjacent bytes and STUMAXH on a shared halfword. `single` skips the threads
  for `make verify`. Passes under QEMU and both engines. The old JIT kept
  stale upper bits after a successful 32-bit CAS, and it interpreted byte
  atomics, sub-word CAS and the MAX/MIN ops.
- `jit_msr_nzcv`: `msr nzcv, xN` followed in the same block by a write to
  xN, read back with `mrs` and with `cset` after a pending `cmp`; 64000
  checks. The old JIT fails 30005; qemu-aarch64 and both engines pass.
- `jit_simd_fixconv`: vector `SCVTF`/`UCVTF`/`FCVTZS`/`FCVTZU` with
  `#fbits` on `.2s`/`.4s`/`.2d`: truncation, saturation, NaN → 0, Q=0
  upper-half zeroing, an in-place form and Teeworlds' `fcvtzs v31.2s,
  v30.2s, #10`. Expected values match qemu-aarch64 11.1; the pre-change
  binary raises SIGILL on the first check.
- `test_dev_urandom`: one `/dev/urandom` stdio stream serves 4096 16-byte
  `fread`s (Teeworlds' `secure_random_fill` pattern), a 1 MiB `read` is
  complete and not zero-filled, `lseek` returns 0, writes are accepted,
  both devices `stat` as character devices 1:9/1:8, and `/dev/random`
  reads. Passes natively on the host kernel; the pre-change binary fails 5
  of 12 checks.
- `test_mem_guard`: wrapping `madvise` ranges return `EINVAL` and keep a
  mapped page above the start intact; a huge valid range still returns
  promptly. The pre-change binary fails the wrap check. QEMU passes.
- `android_bionic_bootstrap`: `/proc/self/maps` lines must not overlap, and
  the main thread's `pthread_getattr_np` stack must contain the current frame.
  Restoring only the old 256 MiB `[interp]` line fails the overlap check.
- `test_dyn_main_locks` (glibc dynamic): `/proc/self/maps` lines are disjoint,
  `__libc_stack_end` is set even when the executable COPY-relocates it, and
  main-thread `pthread_getattr_np` contains the current frame. The pre-change
  binary fails all three in both engines; QEMU with real ld.so passes.
- `android_service_manager`: Java-style `PING_TRANSACTION` with
  `FLAG_COLLECT_NOTED_APP_OPS | TF_ACCEPT_FDS` (55 checks, both engines).

The early `app_process64` SIGILL below was reproduced and explained: the
linker landed within 256 MiB below the stack, the stack lookup selected the
`[interp]` line, and ART's wrapped `madvise` zeroed libc text.

### Android framework regressions (2026-10-06)

The complete strict Linux suite passes **287/287 JIT** and **280/280
interpreter**, with seven expected JIT-only skips. These runs use normal
SDL/GL builds and host graphics access. A graphics-disabled diagnostic build
fails ten graphics/audio tests; sandboxed Vulkan cannot enumerate a device.
Those diagnostic failures are not counted as passing full-suite results.
The final reviewed build completes these suites in 164 s and 134 s,
respectively (`build/framework-final-full-{jit,interp}.log`). The added FMOV
register-copy oracle was subsequently checked in both engines, QEMU and
JIT differential verification.

Three consecutive framework JIT stress runs also pass with ART JIT enabled
(54.010 s, 53.676 s and 57.605 s). The interpreter repeat passes in 103.846 s.
These are bounded correctness runs, not a controlled performance benchmark.
The final reviewed framework runs pass in 54.397 s (JIT) and 106.818 s
(interpreter), recorded under `build/android-framework-results/final-reviewed/`.
One earlier JIT trial stopped at an early guest SIGILL in `app_process64`
(`build/android-framework-results/final-art-jit/jit.json`), before
`libopenjdk` loaded. Its layout-dependent cause is fixed in the Activity
checkpoint above.

The bounded real Android 15 `app_process64` probe reaches `FRAMEWORK_OK` and
exits zero in both Bifrost engines with ART nterp and ART JIT enabled. See
[ANDROID_RUNTIME.md](Android/ANDROID_RUNTIME.md) for provisioning, exact commands and
remaining framework boundaries. QEMU on the same schema-3 root loads the
mainline connectivity JNI library, then stops at the missing host Binder device.

Focused additions:

- `jit_blr_lr`: 20,000 checks of BLR X30 target/link ordering versus a scratch
  register target; default JIT, interpreter and QEMU, plus native-call opt-out.
- `jit_android_semantics`: 58 oracles for the instruction repairs in `bc58e6a`,
  passing both engines and QEMU. Its `no-trap` argument omits SIGILL delivery
  during JIT differential replay (55 remaining checks).
- `jit_literal_load`: 111 checks for W/X/LDRSW and S/D/Q literal forms, positive
  and negative displacements, XZR/prefetch, and decode-cache victim reuse.
  Both engines, QEMU, IR validation and JIT differential verification pass.
- `jit_call_exit`: nested BL/BLR calls into exit_group; traps after each return
  make process status zero the oracle. Native-call opt-out and chain-skip pass.
- `test_shared_alias`: 22,053 checks of low/high RW/R/RX coherence, offsets,
  msync, MADV_DONTNEED, replacement, closed fds, fork, repeated remap, read-only
  fd permissions and execution through a high RX alias. Both engines and QEMU pass.
- `android_binder`: 61 checks, now including arena placement after a high fixed
  mapping and preservation of that existing allocation.

The two earlier dlopen failures also occur with the pre-change main binary
(`f6020a2`): their local fixtures were musl static-pie binaries using musl's
unsupported dynamic-loading stub. Rebuilding them as glibc dynamic makes both
pass on the pre-change and updated binaries. `GLIBC_DYN_SRCS` now selects the
correct toolchain/link flags; Makefile changes invalidate its stale fixtures.

### Concurrent real-game profiling (2026-10-05)

Doom 3, Neverball and Minecraft Weekend were launched as detached desktop
processes on commit `25725e1`, with default tier-1 JIT, `BIFROST_PROF=1`,
`BIFROST_STATS_PERIOD=30` and separate `BIFROST_FALLBACK_PROFILE` inventories.
All three initialized rendering, remained running through the first reporting
interval and wrote periodic profiles and fallback inventories. The local
capture is `bench-results/games-profile-20261005-064633/`; `processes.json`
records commands, working directories and PIDs. Doom's command requests the
existing underground autosave; the other games use their ordinary launch paths.

These are live concurrent captures, not completed stress-test results or
controlled FPS comparisons. The games share CPU/GPU resources; startup, focus,
scene and debug logging affect the measurements. Close each game normally to
retain shutdown reports. No emulator code or hardware-specific policy changed.

### Complete regression suites

The October 5 strict reruns after audio fix `9403911` pass **275/275 in JIT
mode** (147 s, zero skips) and **268/268 in interpreter mode** (132 s, seven
expected JIT-only skips). `android_audio` passes in both complete workloads.

The October 5 ART-foundation checkpoint passes **275/275 in JIT mode** (192 s).
The interpreter full run passes **267/268** (165 s), with seven JIT-only skips:
`android_audio` misses its AAudio callback/frame-count checks within the
fixture's 100 one-millisecond waits. Its isolated rerun passes, but the full
run remains recorded as a failure; this is not a clean 268/268 claim.
It adds `proc_exe` (ELF-backed proc opens, flag translation, ordered maps and
guest startstack) and `mmap_hint` (64 collision/reuse cycles plus lazy 1 GiB
reservations). The final focused memory/procfs/sandbox gate passes six cases;
the Bionic bootstrap passes all six fixed/random-address execution variants.
A fresh extraction independently reproduces the 309-file ART inventory and
the 156-file Media NDK inventory after sharing their disk extractor.
Real ART loads in both engines but encounters unsupported UADALP before
VM creation; the experimental DEX/JNI test deliberately remains a failing
development gate. See [the ART checkpoint](Android/ANDROID_RUNTIME.md#experimental-same-image-art-bootstrap).

The earlier October 5 native APK/service-manager checkpoint passes **273/273 in
JIT mode** with no skips (223 s) and **266/266 in interpreter mode**, with seven
JIT-only skips (204 s). Guest fixtures were refreshed with `make setup-tests`
before testing. The previous `700e8e0` window-plugin checkpoint passed 272/272
and 265/265 respectively.
These runs include the loadable window-plugin refinement, Android Binder/syscall
foundations, widening SIMD and CRC instructions, BFM insertion masks, chained NZCV preservation, socket message
layout and real guest libX11/XWayland rendering. They supersede the October 4
258/255 full-suite checkpoint; the additional verification results below retain
their original scope.

The BFM oracle passes 184,320 cases in JIT, interpreter and QEMU, and under
Tier-2 and differential verification. The chained flags oracle passes 20,000
checks in both engines, QEMU and Tier-2. The XWayland fixture draws a window,
performs 100 round-trips and verifies server pixels through actual guest libX11.
The later window-plugin refinement passes 33/33 native Wayland Android surface
checks in both engines, with both built-in and loadable SDL/EGL backends. Downloaded
Mesa demos still fail at EGL/GLX integration; see the
[window backend validation](Android/ANDROID_WINDOW_PLUGINS.md).

The refreshed checks also pass:

- Generated-source consistency; existing Vulkan generator warnings remain.
- C API **57/57** and native bridge **61/61**, plus backend/plugin contracts
  and twelve CLI cases, in both SDL/GL and headless builds. The SDL helper
  lifetime oracle and graphics SDK independence check pass.
- Differential verification: **60 fixtures and five threaded result oracles**.
- Tier-2 discovery, feedback and timing measurements in single-threaded and
  multithreaded modes, with ordinary chaining and chain skipping; twenty
  mutex promotion-stress runs; MT mode controls and immutable publication.
- Cache pressure/growth, CPU/fallback profiling, Doom native/compatibility
  probes, SDL formats, late TLS/threaded TLS and window statistics.
- Android loader checks with fixed/random ASLR, preflight tests, all six
  Bionic bootstrap configurations, toolchain-fetch and verification-gate tests.
- SDL/GL/Vulkan thunk compatibility in both engines, 64 SDL lifecycle cycles
  in JIT mode, and zstd host-validated compression/decompression in both engines.

All 25 standalone regression groups pass. Additional headless checks pass
**94/94 JIT units**, **88/88 interpreter units** with six JIT-only skips,
the sandbox boundary test, and static glibc startup in both engines. Native
Wayland passes **33/33** surface checks with each built-in/plugin and
JIT/interpreter combination. Suite timings include concurrent validation;
they are not comparative performance measurements. Tier-2-specific checks
explicitly enable promotion even though normal execution defaults to tier 1.

The retained Doom gap harness passes 20,000 randomized generated-code checks
for 625 captured encodings, with zero mismatches and zero explicit IR
fallbacks. The fixed-oracle guest passes 2,054 checks in QEMU, interpreter and
JIT with register/memory verification and no divergence logs. SDL thread
lifecycle/nonlocal-return checks pass separately in both modes. The complete
fallback-profiler and window-stats regressions also pass independently.

Restricted sandbox runs that cannot initialize desktop/audio/Vulkan resources
do not replace these integration checks. Rebuild stale guest fixtures before
interpreting failures as emulator regressions.

### Audio callback wake-up regression

The subsequent audio fix reproduces these two checks failing in both engines
by delaying the host wait entry, and fixes the worker's lost-notification race.
The focused audio/thread gate passes **4/4 in each engine**; the new callback
stress gate passes **20/20 runs** across both engines and normal/delayed waits.
A 12-second offscreen Neverball run with the SDL dummy sound backend keeps
the mixer active through 553 callbacks, with nonempty stream backlogs at each
sample. The bounded run was ended by timeout; it is not extended gameplay or
an audible-output check. Subsequent strict full-suite reruns pass 275/275 JIT
and 268/268 interpreter, including Android audio; see the totals above.

```bash
bash Scripts/Testing/test_audio_callbacks.sh
SDL_AUDIODRIVER=dummy ./Scripts/Testing/run_tests.sh --filter '^(android_audio|linux_audio|audio_test|sdl_thread)$'
SDL_AUDIODRIVER=dummy ./Scripts/Testing/run_tests.sh --no-jit --filter '^(android_audio|linux_audio|audio_test|sdl_thread)$'
```

The callback worker formerly changed its atomic wake generation and notified
without taking the wait mutex. Notification could land after a false predicate
read and before the condition variable's atomic unlock-and-wait, leaving the
worker asleep until its 250 ms idle timeout. Generation changes and shutdown
now take the wait mutex; stream state retains its separate mutex, which the
waiter never acquires while holding the wait mutex. The worker-enabled flag is
atomic because dispatch can inspect it from another guest thread.

The stress script builds a test-only host interposer that delays the first
timed wait per thread by 200 ms, widening this interval without changing guest
deadlines or emulator scheduling. The original binary fails all three captured
injected runs in each engine, while the fixed binary passes. Both
`pthread_cond_timedwait` and `pthread_cond_clockwait` are intercepted for older
and newer glibc/libstdc++ combinations; this is not a substitute for running on
those versions. See the [C++ condition-variable contract](https://eel.is/c++draft/thread.condition.condvar).

The guest fixture checks initially paused startup, 256-frame callbacks and
continued delivery. It now returns AAudio's integer CONTINUE result rather than
using a void callback, and tests blocking writes on a separate callback-free
stream, matching the [AAudio callback contract](https://developer.android.com/ndk/reference/group/audio#AAudioStream_dataCallback).

### Signed native APK and service-manager gate (2026-10-05)

`make test-android-package` builds and signs a real base/split NativeActivity
APK and runs it through guest linker64/Bionic. All six JIT/interpreter/Tier-2
and built-in/plugin combinations pass on native Wayland, checking constructor
and TLS state, pthread isolation, split assets, persistent saves, lifecycle
callbacks, EGL pixel readback and a real invocation through a discovered Binder
handle. Six host test cases also exercise signature/signer/version rejection,
installation rollback, path/member validation, snapshot growth bounds, reordered
reinstall, downgrade prevention, modified-code rejection and managed-launcher
rejection. This gate requires the provisioned Android tools, JDK, Clang/LLD,
native Wayland desktop and GPU; it is separate from the main suite.

`android_service_manager` is registered in the main runner, which supplies
`--android-services` for this fixture. Its 52 checks pass in both engines and
through Bionic inside the APK. Raw Binder behavior remains covered by the
existing transport/object/exit fixtures. See [APK limits](Android/ANDROID_APK.md) and
[service-manager scope](Android/ANDROID_BINDER.md).

The six real Bionic bootstrap configurations pass again after adapter
initialization. The SDL/GL and headless C API, native bridge and window/backend
contracts, twelve plugin CLI cases, SDL helper lifetime and graphics SDK
independence checks also pass. These focused checks supplement the complete
273/266 regression runs; the older standalone results retain their original
scope and counts.

### Window plugin contracts (2026-10-05)

`make test-android-window-backend test-android-window-plugin-cli` checks owned
backend lifetime, failure rollback, independent adapter contexts, malformed C
plugin descriptors, partial-create cleanup, binding failure and unload ordering.
The CLI gate additionally checks twelve startup/help/error cases using the cross-built
`Tests/Unit/hello.elf`. Both host targets and the CLI gate pass with SDL/GL enabled
and in headless builds; SDK independence also passes. CI runs these gates.

The focused Android/Binder/syscall, SDL triangle, socket message and XWayland
subset passes 10/10 in each engine after this refinement. The real pinned
Bionic bootstrap passes all six ASLR/mode configurations. These are focused
checks; the full-suite rerun above also includes this refinement.

Review follow-up also checks that config-only NativeActivity invocations avoid
plugin loading, and guest config flags do not change host startup. The SDL
helper-lifetime oracle passes wrapper destruction before library close,
simultaneous backend references and recovery from transient binding failures.
It uses the real SDL declarations with a stub WM query and a separately loaded
helper library, while native Wayland pixel readback remains the rendering check.

### Fresh-fixture corrections (2026-10-04)

Rebuilding the freestanding assembly fixtures exposed invalid memory-permission
assumptions in `count`, `fib_basic` and `repl`: writable buffers had been placed
in a read/execute text segment. They now reside in `.data`; the REPL buffer
also matches its 63-character capacity, and count prints the documented 1–5.
All three pass in QEMU and both emulator engines. The builder publishes from
a temporary directory beside the fixtures, avoiding cross-filesystem rename
failures when `/tmp` is on another mount.

The extra histogram fixture used halfword loads from a byte array, producing
out-of-bounds count-table indices. Its input now uses halfwords and checks the
complete table against an expected histogram; QEMU, interpreter and JIT with
register/memory verification agree on hash `0x0d693c2a`.

### MT Tier-2 and chaining validation (2026-10-04)

After the MT publication change, the strict unit suites pass **81/81 JIT**
and **79/79 interpreter** (two intentional interpreter skips). The gate passes
**56 differential fixtures and five threaded oracle workloads**. The new
eight-thread region/invalidation oracle also passes under QEMU and interpreter.
Publication diagnostics prove MT edges and regions are enabled; opt-outs and
chain-skip configurations retain the arithmetic oracle. Twenty repeated runs
with `BIFROST_TIER2_HITS=1` pass. Code-cache pressure/growth checks pass at 1
and 64 MiB, and SDL callback/nonlocal-return tests pass in both engines with
the dummy video driver. Headless and SDL/GL builds pass. The host publication
test confirms immutable published bytes through promotion, range/full
invalidation and retranslation, plus bounded chain execution.

Tier-2 promotion is now opt-in (`BIFROST_TIER2=1`); the MT gate tests both
the default tier-1 path and explicit tier-2 promotion. The publication and
SMOV regressions explicitly enable tier-2 so their coverage is retained.

Run the dedicated publication gate after building fresh fixtures:

```sh
BIFROST_EMU=build/release-sdl0-gl0/bifrost-emu bash Scripts/Testing/test_jit_mt.sh
bash Scripts/Testing/test_jit_mt_publication.sh
```

This is synthetic correctness and concurrency coverage, not a gameplay frame
rate measurement or complete code-reclamation implementation.

### Guest linker pass (2026-10-21)

`run_tests.sh --guest-ld --dynamic --realworld` runs the dynamic suites under
each program's own guest `ld.so`. With a regenerated `ld.so.cache`
(`Scripts/Setup/make-ld-cache.py`, checked with `ldconfig -p -C`), 74/74 pass in
JIT and in interpreter mode. A trace confirmed the guest linker opens
`/etc/ld.so.cache` and `/lib/libc.so.6` itself (2 `[dynlink]` lines versus 140
for the native linker). Thunked libraries are served as in-memory stubs;
`test_dyn_thunk_stub` (`dlopen("libGL.so.1")`, state calls answered by the GL
tracker) passes under both linkers on the GL-thunk build in JIT and interpreter
mode (75/75 with the dynamic and real-world suites), and a one-off program with
`libSDL2-2.0.so.0` as a `DT_NEEDED` dependency printed the host SDL version
through the stub. It fails on a headless (`USE_THUNK_GL=0`) build, like
`gl_state`. Not covered: windowed rendering, audio output, Android, Doom 3 and
large-application cold-start time.

### Bitfield moves (2026-10-21)

`jit_bfm_extract` covers `UBFM`/`SBFM` exhaustively (3,102,720 checks, ~1 s): all
`immr`/`imms`, W and X, aliased and distinct registers, and a register-pressure
section (0-9 live temporaries) so the result is allocated in `R8`-`R15` as well.
Expected values come from the Arm ARM bitfield-move pseudocode; the test passes
under QEMU and the interpreter, so the reference is not just the JIT's own opinion.
A deliberate REX bug for the high registers passes the section-1 loops but fails the
pressure section; the check is therefore part of the test. `verify_jit.sh` (96
differential fixtures, 8 threaded oracle runs) and `make test-ir-ssa` (21,839
assertions) pass. Not in the repo: a throwaway random-program fuzzer (C programs of
random shifts/masks/casts over 12-18 live variables, compared with QEMU) passed 420
programs and caught the same deliberate bug.

### Legacy-start constructor regression (2026-10-20)

`test_dyn_legacy_start` is a dynamic glibc fixture built with `-nostartfiles`
whose `_start` calls `__libc_start_main@GLIBC_2.17` with a non-NULL `init` that
runs `.init_array`, as binaries from before glibc 2.34 do. Its constructor must
run exactly once. Before the fix it ran twice (the linker ran it, then glibc's
`init` did), which is what aborted Geekbench 6.5.0 for Linux/AArch64 in LLVM's
option registry. It passes in JIT and interpreter modes and is part of
`make test-dynamic`. The other dynamic fixtures use the modern start, so they
do not cover this path.

### glibc 2.39 loader-hook regression (2026-10-04)

The first provisioned Ubuntu CI run passed the build, API, unit and verification
checks, but `test_dladdr_glibc` aborted with an unmapped read at zero. An
isolated Bootlin glibc 2.39 runtime reproduced the failure in both engines:
`dlopen` read `_rtld_global_ro`'s hook at +672, outside the detector's previous
+352..400 window, and entered the uninitialized native-loader path.

The detector now follows the relocated ADRP/GOT load and requires the actual
`_rtld_global_ro` address before selecting the hook field. All ten selected
dynamic-link tests pass in both engines against glibc 2.39 and 2.42, including
`dladdr` and the eight-thread test. Headless and SDL/GL builds pass. CI now runs
these ten dynamic-link tests in interpreter mode as well as JIT mode.

### Toolchain bootstrap availability (2026-10-04)

The musl.cc download endpoint timed out from GitHub runners. CI now selects
Bootlin's checksum-pinned `aarch64--musl--stable-2024.05-1` SDK (GCC 13.3,
musl 1.2.5), published in its [official toolchain list](https://toolchains.bootlin.com/releases_aarch64.html).
The default local fetch retains musl.cc and falls back to the same pinned SDK;
existing installations are preserved. Compatibility aliases keep compiler and
runtime lookup paths intact, and Bootlin's relocation script runs at install.
Download failures report the actual status, and a bad checksum fails before
installation. `Scripts/Setup/test_musl_fetch.py` checks failure propagation,
checksum rejection, temporary-file cleanup and existing-install preservation
without network access. The actual published archive was downloaded, verified,
installed in a temporary directory and used to rebuild fixtures in an isolated
checkout. Those fresh fixtures pass 80 JIT units, 78 interpreter units, 56
differential fixtures plus four threaded oracles, 57 C API checks and 61
native-bridge checks; the installed developer toolchain was not replaced.

### CI guest-libc and proc-executable follow-up (2026-10-04)

The build portability repair passed both GitHub build configurations. The next
CI failure was glibc-static startup in the host C API probe: a relative loader
argument became a relative `/proc/self/exe` target, violating Linux semantics
and glibc's `_dl_get_origin` assertion. The target is now absolute, with the
sandbox host prefix removed for in-root executables; argv and AT_EXECFN retain
the original invocation. Synthetic readlink now truncates without adding NUL
and rejects zero-sized buffers with EINVAL.

The glibc-static hello repro fails before the repair and passes afterward in
both engines. Host C API checks now include the proc readlink regression and
pass **57/57**; native bridge checks pass **61/61**. The sandbox fixture checks
absolute proc targets, truncation and raw-syscall zero-size errors in both
engines. CI provisions the actual musl compiler for musl-labelled fixtures
instead of substituting glibc, and separately runs glibc-static startup plus
its existing glibc dynamic-link checks.

### Fresh-runner build portability (2026-10-04)

The first pushed CI run failed before tests: the display proxy unconditionally
included SDL/Wayland SDK headers, and Ubuntu's compiler rejected several
`std::*f` math names. The SDL include is now feature-gated; dynamically loaded
Wayland protocol descriptors use their fixed public wire ABI without requiring
a host development SDK. FP handlers use standard overloaded `<cmath>` names,
which retain float precision for float operands. The audio rounding call uses
the same portable form.

`Scripts/Testing/check_headless_headers.sh` poisons graphics SDK include paths and
syntax-checks graphics, FP and audio sources; CI runs it even where SDL headers
are installed. A separate Clang check passes too. Both host builds and the
JIT/interpreter unit and differential gates are rerun locally. Remote CI results
remain authoritative for the Ubuntu runner; a local pass does not establish
remote success.

### Verification gate repair (2026-10-04)

The earlier concurrent replay warnings were reproduced. The verifier restored
shared guest memory and patched live code while other guest CPUs could execute;
this is unsafe and cannot provide a sound differential comparison. Replay now
stops at the sticky multithread transition, with an explicit diagnostic.

`make verify` passes **56 single-thread differential fixtures plus four threaded
oracle workloads**. Cache invalidation, pressure, dispatch and call-helper
fixtures have single-thread modes for differential checks; the gate also runs
the original concurrent workloads at their original loop counts. Single-thread
checks force the BL/BLR helper path. Cache pressure has a 120-second replay
budget; other differential fixtures have 30 seconds, and threaded oracles 60.
These are separate forms of coverage, not multithread differential verification.

The gate discovers fixtures from source, rejects missing/stale ELFs and any
reported PC/register/memory divergence or suspended replay, and preserves
emulator/timeout exit status. `Scripts/Testing/test_verify_gate.py` exercises failure
propagation and runs in CI. Follow-up headless units pass **80/80 JIT** and
**78/78 interpreter**, with two explicit JIT-only skips. Host API checks pass
**55/55 C API** and **61/61 native bridge**.

Both headless and SDL/GL configurations build. The headless Android surface
probe requires SDL and fails without it; activity and audio probes pass there.
After approval review recovered, the desktop Android surface/activity/audio
probes passed **3/3 in both JIT and interpreter modes** with the SDL/GL build.
CI's host API step now uses the same system AArch64 compiler as fixture setup;
headless compilation no longer depends on indirect SDL math includes or
unconditionally linked Android SDL event/window functions.

The host C API probe likewise assumed an executable guest stack. It now loads
`Tests/Unit/capi_testlib.so` for integer, FP and syscall call probes, preserving the
non-executable-stack contract. `make test-capi` passes 55/55 checks;
`make test-nb` passes 61/61. These host API tests are separate from the main
guest inventory at that checkpoint.

Android surface, activity/looper and audio integration probes pass in both
engines. This verifies those existing native Android contracts, not integrated
APK launch, a complete Android framework, or ART runtime execution.

## Test inventory

Original regression sources ship under Bifrost's Unlicense. Generated guest
ELFs are rebuilt locally; included and downloaded third-party fixtures keep
their upstream licenses. See [fixture distribution](THIRD_PARTY.md) for the
source/binary distinction and game-data boundaries.

The `fcsel_high_regs` regression has 61 checks for high FCSEL source registers,
FP destination 31, single/double precision, aliasing, GPR preservation, and
discarded FP-to-GPR writes to XZR. It passes under QEMU, JIT, and interpreter;
see [the vkQuake reproduction](Apps/vkquake.md).

The fully provisioned suite contains 313 configured test runs:

| Category | Runs | Notes |
|---|---:|---|
| Unit | 132 | Focused guest instruction/JIT regressions and registered host JIT gates |
| Integration | 87 | Guest programs and subsystem checks in `Tests/Apps/` and `Tests/Asm/` |
| Sandbox | 1 | `BIFROST_ROOT` path-boundary regression |
| Per-thread JIT | 1 | Cross-thread invalidation with separate JIT instances |
| Toybox | 9 | Commands run through the committed AArch64 Toybox binary |
| Real-world static | 49 | BusyBox and Toybox command checks; BusyBox may be fetched by the runner |
| Real-world dynamic (glibc) | 7 | Part of the real-world fixture set; requires rootfs libraries |
| Dynamic | 17 | Dynamically linked musl/glibc and XWayland tests; requires rootfs and built fixtures |
| Benchmarks | 5 | Performance smoke benchmarks; omitted by `--quick` |
| Interactive | 5 | Stdin-driven programs run with scripted input |
| **Full configured suite** | **313** | Includes the sandbox and rootfs-dependent dynamic runs |

Without a configured rootfs, the 7 dynamic real-world runs and 17 dynamic
tests are not selected, for 289 configured runs (284 with `--quick`). These
figures describe test definitions selected by the runner; missing guest
fixtures and unavailable display/SDL support can result in skips.

## Dispatch cache and profiling regression (2026-10-04)

`jit_dispatch_cache` is registered in the main suite. It checks exact results
for indirect calls across three guest threads, code replacement after workers
join, and data-only mmap/munmap churn. QEMU and both engines agree. The bounded
cache-pressure script also passes at 1 MiB and 64 MiB, including cached
fallback decisions and logical growth. SDL thread lifecycle/nonlocal-return
checks pass in both engines.

`bash Scripts/Testing/test_jit_profiler.sh` passes the independent host regression for
per-thread/JIT-owner sampler registration and the full reserved growth range.
It uses deterministic range checks rather than statistical sample thresholds
and is not another guest-inventory entry. Optional dispatch statistics expose
both cache ways and table lookups; see [JIT.md](JIT.md).

### Guest-call helpers

`jit_call_helpers` checks nested arithmetic calls and callee-written NZCV
across three threads; QEMU, JIT and interpreter agree. A single-thread
helper-forced run (`1000 single`, `BIFROST_NO_DIRECT_CALL=1`) passes register
and memory verification with no divergence reports. Concurrent verification
reports library replay divergences also reproduced by the baseline, so it is
not claimed as a clean differential run.

After the final duplicate-PC-store removal, the complete JIT unit inventory
passes 80/80 and SDL callback/nonlocal returns pass again. The full suites
above passed immediately before that last narrowing; the retained native Doom
harness passes 20,000 checks with no mismatches on the final build.

## Doom JIT, SDL threads and diagnostics

The following focused tests accompany the native instruction and SDL worker
changes. Standalone host/SDL probes are not additional entries in the main
inventory unless explicitly registered in `run_tests.sh`.

- `Tests/Unit/jit_simd_fp_indexed.c`: 28 indexed FMUL lane, aliasing and upper-half
  checks, independently validated in QEMU and both engines.
- `Tests/Unit/jit_round_away.c`: 6,984 FRINTA/FCVTAS fixed-oracle cases for ties,
  adjacent values, signed zero, NaNs and saturation.
- `Tests/Unit/jit_scalar_dup.c`: 60 scalar lane-copy cases covering all 30 valid
  lane/width combinations with and without source/destination aliasing.
- `Tests/Unit/jit_doom_gap.c`: 2,054 oracle checks, including all conditional
  compare conditions/NZCV inputs in single/double precision, FCVTL, SHLL,
  ADDV result width and fused indexed FMLA.
- `Tests/Unit/native_doom_gaps.cpp` with `Tests/Unit/doom_gap_opcodes.inc`: actual JIT
  execution for all 625 captured gap encodings, 32 randomized states each.
  Checks registers, flags, memory, post-index writeback and exclusive-monitor
  success/failure. Interpreter comparison complements the independent fixed
  oracles; it is not itself a hardware oracle. Also passes with
  `BIFROST_TIER2_HITS=1` to exercise early promotion decisions.
- `Tests/Apps/test_sdl_thread.c`: create/wait, eight concurrent workers,
  detach, and 16 nested `setjmp`/`longjmp` returns. Tests both the JIT SDL
  runner and the explicit `--no-jit` path.
- `Scripts/Testing/test_fallback_profile.sh`: exact concurrent snapshot retention
  after worker exit, with 192 complete entries and 192,000 counts.
- `Scripts/Testing/test_window_stats.sh`: presentation clock, default rendering log,
  default title/idle FPS and enable/disable controls. UI placement, compositor stacking
  and overlay capture require separate desktop checks; see
  [window-stats limits](window-stats.md).

```bash
make -j8 USE_SDL2=1 USE_THUNK_GL=1 build/release-sdl1-gl1/bifrost-emu
make cross SRC=Tests/Unit/jit_doom_gap.c OUT=Tests/Unit/jit_doom_gap.elf
make cross SRC=Tests/Apps/test_sdl_thread.c OUT=Tests/Apps/test_sdl_thread.elf
BIFROST_IR_VALIDATE=1 Scripts/Testing/run_tests.sh --emu build/release-sdl1-gl1/bifrost-emu --strict
BIFROST_IR_VALIDATE=1 Scripts/Testing/run_tests.sh --emu build/release-sdl1-gl1/bifrost-emu --no-jit --strict
bash Scripts/Testing/test_native_doom_gaps.sh
BIFROST_JIT_VERIFY=1 BIFROST_JIT_VERIFY_MEM=1 BIFROST_IR_VALIDATE=1 build/release-sdl1-gl1/bifrost-emu Tests/Unit/jit_doom_gap.elf
build/release-sdl1-gl1/bifrost-emu Tests/Apps/test_sdl_thread.elf
build/release-sdl1-gl1/bifrost-emu --no-jit Tests/Apps/test_sdl_thread.elf
bash Scripts/Testing/test_fallback_profile.sh
EMU=build/release-sdl1-gl1/bifrost-emu bash Scripts/Testing/test_window_stats.sh
```

The standalone captured-opcode harness links the built SDL/GL objects and
expects that configuration. Build all registered guest fixtures with
`make setup-tests` before running the complete suite on a new checkout.

`Tests/Unit/bench_doom_vector_fp.c` and `Tests/Unit/bench_doom_round_lane.c` are optional
instruction-throughput probes, not full-game FPS benchmarks. See
[Doom 3 validation](Apps/doom3.md) for measured scope and the longer user gameplay
run. Neither the tests nor the recorded presentation counter establish the
planned v2.0 60+ FPS target across demanding gameplay scenes.

## Game compatibility follow-up (2026-10-03)

`Tests/Unit/jit_shift_register.c` adds 182 SSHL/USHL checks for signed low-byte
counts, logical/arithmetic right shifts, lane widths, Q forms, aliasing, and
out-of-range counts. It passes under QEMU and both emulator engines.
The six selected shift/permute regressions also pass in both engines.

`Scripts/Testing/run_thunk_compat.sh` exercises SDL event-filter registration,
accept/reject returns, nested callbacks, signed mouse deltas, and exact-size
output bounces, alongside GL ABI and Vulkan mapped-memory upload/readback
checks. These focused tests do not establish correct interactive vkQuake
sensitivity; see [vkQuake status](Apps/vkquake.md).

`Tests/Apps/test_gles_egl_image.c` resolves `eglCreateImageKHR` and
`glEGLImageTargetTexture2DOES` through `eglGetProcAddress` on a host EGL
pbuffer context. It shares a 2x2 texture through an EGLImage and reads the
target back. The thunk used to return NULL for the advertised target entry
point, which Android WebView called. It skips without host EGL/GLES 2 or the
extensions; it does not cover AHardwareBuffer or dma-buf image sources.

`Tests/Apps/test_gles_contexts.c` checks that GL binding state is per EGL
context. Context A keeps a pixel unpack buffer bound; thread B, in its own
context, unbinds the target and uploads client memory; then A uploads from
the buffer at offset 0. Both textures must read back. The previous binary,
with one tracker for all contexts, fails A's upload (in Hungry Shark World
the same pattern crashed in the host driver's `memcpy` from NULL). It passes
in both engines and skips without host EGL/GLES 3. Threads without an EGL
context (SDL/GLFW) still share the default tracker.

`Tests/Apps/test_gles_extensions.c` checks, on a host ES 3 context, that the
filtered `glGetString(GL_EXTENSIONS)`, `GL_NUM_EXTENSIONS` and `glGetStringi`
agree and that an index past the list is `GL_INVALID_VALUE`. Hidden fd-import
extensions must stay hidden, and every entry point of 12 common extensions
must resolve when advertised. It calls OES vertex-array, discard-framebuffer
and KHR debug-group alias rows. It checks resolution of the other generated rows,
not their behavior.

## Memory and signal regressions

`Tests/Unit/test_mem_guard.c` checks wrapping `munmap`, stack overlap rejection,
bounded huge `madvise`, and sequential above-window fixed mapping cycles.
On 2026-10-02, the failing local ELF still contained the older threaded
implementation. Rebuilding the current source passed 40/40 runs in each of
JIT and interpreter mode; this does not establish correctness of that older
threaded workload.

`Tests/Unit/test_memory_permissions.c` checks existing read-only protections,
permissions and unmapped holes inherited across fork, isolation of child
repairs, syscall `EFAULT`, and brk shrink/regrowth/collision behavior. Fault
probes check the guest PC and bound retries. Page sizes come from `sysconf`;
LSE checks run only when advertised by `AT_HWCAP`.

`Tests/Unit/test_sigreturn_context.c` checks handler edits to registers, PC, SP,
NZCV, FP/SIMD state and signal masks, nested alternate stacks, and nine
malformed signal-frame cases. These exercise the Linux AArch64 ABI; they
are not a complete Arm architecture conformance suite.

`Tests/Unit/test_thread_fault_signal.c` faults three times on an unmapped page
from a worker thread. Each fault must reach that thread's SIGSEGV handler with
the page as `si_addr`, and the thread must keep running. Under the interpreter,
such faults used to end the worker silently.

For independent QEMU runs, see the
[reference signal-mask patch](../Tools/Reference/QemuReference/README.md). It fixes
QEMU's handling of unblockable mask bits while keeping the Linux assertions
strict.

`Tests/Unit/test_vm_signal_contracts.c` checks fixed `PROT_NONE` mapping contents,
file-offset preservation with concurrent reads during `mmap`, `mremap` move
permission and fixed destinations, and alternate-stack changes during a
handler. Its 47 checks also pass on native Linux. QEMU 11.1.50 disagrees on
the zero-size `mremap` errno (`ENOMEM` instead of Linux's `EINVAL`); that
assertion remains strict.

## zstd, shift, and bitfield regressions

`Tests/Unit/jit_variable_shift_source.c` checks that all four variable shifts and
rotates preserve their count source, including packed high bits, at both
32-bit and 64-bit widths. `Tests/Unit/jit_shifted_operand_width.c` checks W-form
shifted operands with nonzero upper X bits and block-local constant folding.
Build these sources with `make setup-tests` or `make cross`.

`Tests/Unit/jit_bitfield_regalloc.c` checks bitfield results and live source values
under register pressure, memory stores/reloads, and Tier-2 side exits.

`Scripts/Testing/run_zstd_regression.sh` uses the bundled static AArch64 zstd fixture
and requires the host `zstd` CLI. It compresses a deterministic corpus using
both JIT and interpreter, with default workers and `--single-thread`; host
zstd validates each archive and restores bytes for comparison. Both guest
engines also decode a host-generated archive. The main suite runs this host
harness once through `HOST_JIT`, and skips it under `--no-jit` because the
harness already exercises both engines.

```bash
./Scripts/Testing/run_tests.sh --filter 'variable_shift_source|shifted_operand_width|zstd'
./Scripts/Testing/run_tests.sh --no-jit --filter 'variable_shift_source|shifted_operand_width|bitfield_regalloc'
./Scripts/Testing/run_zstd_regression.sh
```

The full working-tree suite passed 240/240 on 2026-10-01 with
`BIFROST_IR_VALIDATE=1` and `BIFROST_REGALLOC_CHECK=1`; that run included the
bitfield regression described above. Both focused shift tests
also passed JIT verification and memory verification. Applying those settings
to threaded zstd logged divergences and exceeded its 90-second compression
timeout, so that diagnostic run remains unresolved. See [zstd.md](Apps/zstd.md)
for the two fixes, fixture scope, and verification limits.

`Tests/Unit/jit_memory_guard_address.c` exercises cached constant addresses across
permission guards, stack saves, pointer loads, and a conditional exit. It
checks the value after repeated calls, including JIT verification and the
interpreter.

## SDL fullscreen regression

`Tests/Apps/test_sdl_fullscreen.c` checks SDL2 desktop/current display-mode
status returns and caller-owned output structs, invalid display indices,
three fullscreen/windowed recreation cycles, fullscreen flag transitions,
restored dimensions, and resize-event payloads. It exercises the sequence used
by Neverball to save its dimensions. Run it on a desktop, or use SDL's dummy
video driver for a bounded headless check:

```bash
make cross SRC=Tests/Apps/test_sdl_fullscreen.c OUT=Tests/Apps/test_sdl_fullscreen.elf
SDL_VIDEODRIVER=dummy ./Scripts/Testing/run_tests.sh --filter '^sdl_fullscreen$'
SDL_VIDEODRIVER=dummy ./Scripts/Testing/run_tests.sh --no-jit --filter '^sdl_fullscreen$'
```

The regression creates hidden windows so compositor placement rules and an
interactive game do not interfere with its requested-size assertions.
It returns exit 77 when SDL video cannot initialize. The pre-fix build fails
because the mode-query thunk does not fill the guest's dimensions. The repaired
build passes with the dummy driver and real desktop in both execution modes.
See [neverball.md](Apps/neverball.md) for the root cause and current interactive status.

## JPEG SIMD narrowing regression

`Tests/Unit/jit_simd_sat_narrow.c` covers the six signed/unsigned saturating narrowing
shift forms and nonsaturating RSHRN, each at 16→8, 32→16, and 64→32 bits.
Its 672 checks cover minimum/maximum shift amounts, both destination halves,
source/destination aliasing, rounding overflow, and sticky FPSR.QC.

```bash
make cross SRC=Tests/Unit/jit_simd_sat_narrow.c OUT=Tests/Unit/jit_simd_sat_narrow.elf
./Scripts/Testing/run_tests.sh --filter '^simd_sat_narrow$'
./Scripts/Testing/run_tests.sh --no-jit --filter '^simd_sat_narrow$'
qemu-aarch64 Tests/Unit/jit_simd_sat_narrow.elf
```

On 2026-10-02, all 672 checks passed under QEMU, JIT with IR validation and
register-allocation checks, and interpreter. The saved pre-fix interpreter
failed 632 checks. The related SIMD suite passed 19/19 in each Bifrost mode.
Direct libjpeg decodes of four Neverball JPEGs, including the hard-set preview
and two Help images, matched QEMU's raw RGB output byte-for-byte in both modes.
The affected JIT narrowing family now uses the corrected interpreter handler.
See [neverball.md](Apps/neverball.md) for the diagnosis and image coverage.

## Stability follow-up: 2026-10-02

At the October 2 checkpoint, the working-tree quick suite passed 238/238
in JIT mode and 236/236
in interpreter mode, with two intentional interpreter skips (`tier2_smov`
and the dual-engine `zstd_compression` host harness). Four stale fixtures were
rebuilt before the clean full JIT rerun. `test_mem_guard` separately passed
100 consecutive runs in each mode with no failures; its high-address mapping
coverage is single-threaded. See [neverball.md](Apps/neverball.md) for the associated
interactive fullscreen mouse confirmation and coverage limits.

Before the compatibility work was committed on the same date, the quick
suite was rerun with the added FCSEL regression: JIT passed 239/239 and
interpreter passed 237/237 with the same two expected skips. Standalone late
TLS and late-TLS pthread/edge scripts passed in both modes; SDL/GL/Vulkan
thunk compatibility probes passed in both modes, and the default JIT SDL
lifecycle soak passed 64 cycles. These checks use the current SDL2/GL build.

## Running tests

Additional standalone regressions build their own fixtures in temporary
directories and exercise the current emulator (`BIFROST_EMU` can override it):

```sh
./Scripts/Testing/test_rootfs_discovery.sh  # dynamic programs find a rootfs without flags
./Scripts/Testing/test_rootfs_install.sh    # install-rootfs.sh output runs from an installed layout
./Scripts/Testing/test_apk_launch.sh        # bifrost-emu app.apk hands the package to Asgard (stub)
./Scripts/Testing/test_asgard_install.py    # install-asgard.py install, update and removal (fake checkout)
./Scripts/Testing/test_late_tls.sh          # late GD/TLSDESC and rejected initial-exec
./Scripts/Testing/test_late_tls_pthread.sh  # existing/new threads, reuse, pointer/alignment/weak cases
./Scripts/Testing/run_thunk_compat.sh       # SDL/GL/Vulkan ABI probes in both modes
./Scripts/Testing/run_thunk_soak.sh 64      # repeated SDL window/mapping/thread lifecycle
```

The TLS scripts require the AArch64 glibc compiler; pthread tests also need
rootfs glibc libraries. Thunk scripts require an SDL2/GL-enabled emulator and
the musl cross compiler. The SDL soak uses the dummy video driver and does
not establish long-duration interactive game stability.

```bash
make check                 # standard JIT suite
make check-quick           # JIT suite without the five benchmarks
make check-nojit           # quick suite using the interpreter
make check-fwd              # quick suite with BIFROST_ENABLE_FWD=1
make ci                    # guest-correctness gate including sandbox and glibc dynamic checks
make verify                # strict JIT/interpreter differential gate

./Scripts/Testing/run_tests.sh --unit                 # focused JIT regressions
./Scripts/Testing/run_tests.sh --unit --strict        # fail if a selected fixture is missing
./Scripts/Testing/run_tests.sh --integration          # integration tests
./Scripts/Testing/run_tests.sh --dynamic              # dynamic-linker tests
./Scripts/Testing/run_tests.sh --filter 'sig|brk|pipe' # filter test names
./Scripts/Testing/run_tests.sh --no-jit --quick       # interpreter quick suite
./Scripts/Testing/run_tests.sh --help                 # all runner options
```

`make check-nojit` and `make check-fwd` use quick mode. To include
benchmarks in either mode, invoke the script directly with `--no-jit` or
`--fwd` and omit `--quick`.

The default runner includes interactive and real-world categories. It
attempts to fetch the BusyBox fixture when missing; set a name filter to
avoid this setup step. Dynamic tests are enabled when `rootfs/lib` and
the corresponding guest binaries are present. SDL/GL tests report a skip
when the host display or required APIs are unavailable.

`--strict` makes a missing selected guest binary a failure. Use
`--allow-missing REGEX` only for named external fixture bundles that are
intentionally optional; each such omission is printed as a skip. Exit 77
remains an environment skip for display/driver tests, and JIT-only cases
remain skipped under `--no-jit`. CI uses strict mode so fixture compilation
failures cannot silently reduce test coverage.

For repeated performance runs, use `Scripts/Bench/run_benchmarks.py`. It performs
warmups, validates result markers, measures host wall time, and saves host,
compiler, emulator, environment, and sample metadata to JSON:

```bash
make USE_SDL2=0 USE_THUNK_GL=0
make setup-tests
./Scripts/Bench/run_benchmarks.py --emulator build/release-sdl0-gl0/bifrost-emu \
  --warmups 2 --runs 7 --mode both
```

CoreMark is added to the default benchmark set when `Tests/Unit/coremark.elf` is
available; it can also be selected explicitly with `--bench coremark`. Its
four CRCs are checked. The CoreMark source is not bundled with the repo.

## Adding a test

1. Add a self-checking AArch64 source under `Tests/Unit/` or `Tests/Apps/`.
2. Add its `.elf` path and expected output pattern to the matching array
   in `Scripts/Testing/run_tests.sh`.
3. Build it with `make cross SRC=path/to/test.c OUT=path/to/test.elf`.
4. Run the focused category and relevant JIT/interpreter modes. Keep tests
   bounded; the runner enforces a per-test timeout and kills the process
   group if it hangs.
5. Update this inventory only when category definitions or setup
   requirements change.


## SpongeBob startup-animation milestone (2026-10-07)

Real installed APK play mode now renders the Nickelodeon startup logo and
animation, observed by the user. Captured JIT runs count up to 670 EGL frames;
the final run counts 295 before the same native blocker. Real SDL audio is
enabled in these play runs (`SDL_AUDIODRIVER` unset); audible output itself
was not independently verified. This is a rendering milestone, not a claim
that interactive gameplay works.

The fixed path covers AND-to-SP stack alignment, GLES mapped-buffer pointers,
and strict Java launcher replies for completion, process state, power/thermal
queries and app cache creation. Both pre-resume and resumed service checks pass,
including thermal callback registration/removal and rejection of path traversal.
The standalone JIT Activity probe passes; its service/crash-report checks also
pass in interpreter mode, whose existing worker-6/8 null reads still fail the
full framework probe.

Fresh focused Linux fixtures pass 11/11 in JIT and interpreter: Binder exit,
Binder device/objects/service manager, signal semantics, process cmdline, group
exit, sandbox, Linux/Android audio, and Android instruction semantics. The latter
passes 72 checks in QEMU/JIT/interpreter, with 69 in differential no-trap mode.
GL and GLES mapped-buffer modes each pass 21 checks, GLES also in interpreter;
the GL debug/EXT-label fixture passes. Generated thunk-table consistency and
`git diff --check` pass. The earlier real-Bionic callback-thread regression
passed in JIT, interpreter and Tier-2 during restoration of the audio path.

Local final game evidence:
[log](../build/android-spongebob-stubs-final/jit.log),
[result](../build/android-spongebob-stubs-final/jit.json).
The remaining stop is unhandled `frecps v3.4s,v16.4s,v18.4s` (0x4e32fe03),
raising guest SIGILL. SQLite file-locking errors and broader GL mapping
semantics remain separate gaps. See [runtime details](Android/ANDROID_RUNTIME.md)
and [instruction/graphics trace](JIT.md).


### Synchronous signal-wait follow-up (2026-10-07)

The milestone's 11-test selection did not include `test_sigtimedwait`. The
subsequent user-reported failure was reproduced in JIT and interpreter: the
cross-thread SIGUSR2 wait timed out, then restoring the mask delivered the
unconsumed fatal signal and ended the test with status 140. Native Linux
passed all 15 checks.

The new asynchronous pending-queue mask filter had also been used by the
synchronous waiter. `CPU::pop_pending` now accepts an explicit wait set, so
`rt_sigtimedwait` can dequeue its blocked requested signals while normal handler
delivery retains the mask filter. This follows the
[Linux sigwait contract](https://man7.org/linux/man-pages/man2/sigwaitinfo.2.html).
The existing regression passes 15/15 in both engines, including cross-thread
wake, no handler delivery, timeout and mask restoration. Twenty repeats per
engine pass, as do 14 neighboring signal/futex/group-exit tests per engine.
No new fixture or signal-handler default was needed.
The real-audio installed SpongeBob rerun still reaches its resumed service
checkpoint and renders 580 frames before the previously observed native SIGILL
blocker. Evidence: `build/android-spongebob-sigtimedwait-fixed/jit.log` and
`jit.json`. This follow-up fixes signal waiting, not that native instruction.

### SpongeBob reciprocal-family follow-up (2026-10-07)

`jit_simd_recip` passes **45/45** under QEMU cortex-a53, JIT and interpreter,
also JIT differential replay with memory/IR validation and Tier-2 threshold 32.
Its fixed QEMU hashes compare 61,440 instruction executions' full vector
results and FPSR across 20 forms, three destination aliases and eight FPCR
settings. Those settings cover all four rounding modes and selected FZ/DN
combinations; they do not exhaust every FPCR combination. Explicit cases
cover Unity's exact opcode, zero/infinity, estimate precision, fused
cancellation, tiny odd inputs, final scaling without intermediate overflow,
and 12 reserved/unsupported encodings. Enabled FP exception traps and newer
AFP/RPRES behavior are excluded. The neighboring FP/SIMD/Android selection
passes **37/37** in each engine.

The real installed game no longer dies at FRECPS. It submits **540 frames**
and stays alive until the intentional 120-second timeout, with no reported
fatal exception or missing instruction. The user observes a white screen
after the startup animation; frame submission stops after about 5.7 seconds
of rendering. Consequently the runner's `passed: true` is only its bounded
startup/rendering gate, not evidence of continued rendering or gameplay.
GDB then confirms guest thread 19 sleeping in
`libsensor.so+0x11574` (`waitForSensorService`), called from `+0x11080`
(`getInstanceForPackage`). Both offsets match the actual image's disassembly.
This is the confirmed service wait blocking that thread. The observed ~3.9 GiB RSS alone does not
establish a leak.
Evidence: [log](../build/android-spongebob-recip-fixed/jit.log),
[result](../build/android-spongebob-recip-fixed/jit.json),
[wait stack](../build/android-spongebob-sensor-wait-trace/jit.log).

An isolated temporary host harness checked decoder/interpreter acceptance of
**102,783 unique FP/SIMD words** disassembled from all **21 installed native
libraries**. It rejects 19 encodings in Unity: AESD (two unique words), AESMC
(one), FCVTN `.4h,.4s` (13), SQDMULH `.8h` (one), and immediate UQSHL
`.4h`/`.8h` (two). These appear in real code sequences; they are candidates
for further coverage, not proof the current scene executes them. Across the
binary, the rejected forms occur 14/13/107/1/2 times respectively. This
acceptance check uses synthetic register inputs, does not validate accepted
operations' numerical semantics or JIT lowering, and excludes system/runtime
libraries and non-FP/SIMD instruction classes. Helpers and disassembly stay
in `/tmp`; no inventory program was added to the repository.

### Native sensor-service follow-up (2026-10-07)

ActivityProbe's native-registry sensor service reports the sensorless virtual
device using Android 15's ISensorServer protocol. The source contract is linked
in [ANDROID_RUNTIME.md](Android/ANDROID_RUNTIME.md#native-sensor-service-profile-2026-10-07).
Observed checks cover all six supported transaction replies (exactly one raw
32-bit zero), a non-default device id, unsupported event-connection rejection,
bad-token/extra-argument rejection, and real framework JNI/libsensor queries
for static/dynamic lists and default accelerometer/gyroscope. Both engines
reach `ACTIVITY_SENSOR_SERVICE_OK` and `FRAMEWORK_OK`; the runner now requires
the sensor checkpoint for ActivityProbe with `--services`.

The final JIT framework run passes in **9.649 s**. The first interpreter run
passes in **17.374 s**; the final interpreter run reaches all checkpoints and
exits zero in **19.423 s**, but the strict runner rejects the already observed
unmapped reads at zero in workers 6 and 8. Its sensor checks pass; the complete
interpreter probe is not claimed clean. Four neighboring Binder/registry
fixtures pass **4/4 in each engine**. Sensor event queues, direct channels,
operation parameters and real sensor hardware remain unsupported and untested.
Evidence: [final JIT](../build/android-sensor-service-final-jit/jit.log),
[final interpreter](../build/android-sensor-service-final-interp/interp.log).

The installed real-audio SpongeBob run passes sensor initialization before
and after resume, and Unity thread 19 progresses into locale and later SDK
initialization instead of the previous sensor-service retry. It submits
**530 frames**, then frame submission stops at **5.59 s** of rendering. It
remains alive without a reported fatal guest exception until the intentional
**120-second timeout**. This clears the identified sensor wait but does not
establish continued rendering or gameplay. Missing `connectivity`/`shortcut`
services and SQLite I/O/locking errors are recorded; none is yet confirmed as
the cause of the remaining rendering stall. The game run started before the
runner's new mandatory sensor marker was added; both sensor markers are
present in its log, while the final built-in probes use the updated gate.
Evidence: [game log](../build/android-spongebob-sensor-fixed/jit.log),
[bounded-run result](../build/android-spongebob-sensor-fixed/jit.json).

The user reports a white screen, an 8–9 GB memory spike and a newly spawned
thread disappearing. Two short framework-test emulator processes ran alongside
the first game validation, so aggregate process-group memory is a possible
confounder, not a confirmed explanation. A subsequent **isolated 150-second**
game run under GDB and `/proc` sampling peaks at **3.456 GiB RSS** and remains
near that level through the timeout; **2.625 GiB** is resident in the guest
reservation at the peak. The JIT reports **381.35 MiB** emitted code at the end,
with no budget overflow. That run does not reproduce the reported 8–9 GB peak.
All **11 captured guest exit syscalls** have status zero, including IL2CPP
thread-pool workers. UnityMain (guest tid 19) remains present in sleeping state
with unchanged CPU ticks in the final samples. It submits 485 frames under the
debugger, then stops. This distinguishes normal worker exits from the render
thread's remaining wait; it does not identify that wait's cause or exclude
memory growth in a longer/different run. Evidence:
[thread/cache trace](../build/android-spongebob-sensor-memory-final/jit.log),
[memory samples](../build/android-spongebob-sensor-memory-final/memory.jsonl),
[result](../build/android-spongebob-sensor-memory-final/jit.json).

### Android HOME regression

`PackageTests.test_managed_home_and_resume` uses `Managed.Home` to request HOME
twice, with an offscreen Asgard launcher and a windowless plugin. It verifies
one Activity creation, two pause/stop/restart/resume cycles and final destroy.
Pass `--plugin build/release-sdl1-gl1/android_window_plugin_fixture.so` to
`Scripts/Android/test_android_package.py` to keep this test off the desktop.
`HomeCommandTests` checks command parsing, repeated HOME and launcher card
state. `android_bionic_bootstrap` checks TCGETS buffer boundaries and a real
fork/exec through Bionic (constructors, TLS, interpreter/vDSO auxv, executable
path, stack bounds and parent Binder-arena survival). `test-android-window-plugin` checks visibility calls
and old-descriptor ENOTSUP. These checks do not establish desktop focus,
Java Runtime.exec/vfork support or game playability.
