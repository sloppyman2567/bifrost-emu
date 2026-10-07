# Tests

The test runner is [`scripts/run_tests.sh`](../scripts/run_tests.sh). It does
not build guest fixtures; use `make setup-tests` for the C and assembly
fixtures, or `make cross SRC=... OUT=...` for one C test. Missing fixtures are
skipped by default and fail with `--strict`. An ELF older than its matching
C or assembly source fails with a rebuild instruction, so ignored local
fixtures cannot silently test outdated code. Some integration tests also
require SDL2, a display, or a configured guest rootfs.

## Latest full validation

### SpongeBob managed Activity creation (2026-10-07)

The installed SpongeBob 5.11.5 app probe reaches `APP_CLASSLOADER_OK`,
`APP_APPLICATION_STARTED`, `APP_ACTIVITY_CREATED` and `FRAMEWORK_OK`, exiting
zero in both engines (26.2 s JIT, 79.5 s interpreter in the reviewed runs).
The real launcher is `com.clevertap.unity.CleverTapOverrideActivity`; logs also
show native `libmain.so` loading and IL2CPP `JNI_OnLoad`. No readiness-property
override is needed after the fix. See [the runnable command and bounds](ANDROID_RUNTIME.md#installed-app-creation-checkpoint-2026-10-07).

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
bootstrap variants pass. See [ANDROID_RUNTIME.md](ANDROID_RUNTIME.md).

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
- `test_group_exit`: forked children with live threads: main SIGKILLing
  itself, a worker's `kill(getpid(), SIGTERM)`, and a worker's
  `exit_group(7)` (main sleeping or spinning) each end the process within
  10 s; a fork child's new thread reports the child's pid; main's
  `pthread_exit` keeps the process until a worker's `exit(5)`. Passes
  natively on the host kernel; the pre-change binary fails all 6.
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
- `jit_simd_fp_nan` (`INTERP` row, run with `--no-jit`): vector FP NaN
  choice and SNaN priority, +qNaN for invalid operations, FMAX/FMIN signed
  zero, FMAXNM/FMINNM with a quiet NaN, fused FMLA, FMLS negation, FMULX
  inf*0, FABD NaN sign, FADDP pairs, two `.2d` cases. 14/14 under
  qemu-aarch64 and the fixed interpreter; the old interpreter fails 11.
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
[ANDROID_RUNTIME.md](ANDROID_RUNTIME.md) for provisioning, exact commands and
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
development gate. See [the ART checkpoint](ANDROID_RUNTIME.md#experimental-same-image-art-bootstrap).

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
[window backend validation](ANDROID_WINDOW_PLUGINS.md).

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
bash scripts/test_audio_callbacks.sh
SDL_AUDIODRIVER=dummy ./scripts/run_tests.sh --filter '^(android_audio|linux_audio|audio_test|sdl_thread)$'
SDL_AUDIODRIVER=dummy ./scripts/run_tests.sh --no-jit --filter '^(android_audio|linux_audio|audio_test|sdl_thread)$'
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
existing transport/object/exit fixtures. See [APK limits](ANDROID_APK.md) and
[service-manager scope](ANDROID_BINDER.md).

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
`ctest/hello.elf`. Both host targets and the CLI gate pass with SDL/GL enabled
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
BIFROST_EMU=build/release-sdl0-gl0/bifrost-emu bash scripts/test_jit_mt.sh
bash scripts/test_jit_mt_publication.sh
```

This is synthetic correctness and concurrency coverage, not a gameplay frame
rate measurement or complete code-reclamation implementation.

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
installation. `scripts/test_musl_fetch.py` checks failure propagation,
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

`scripts/check_headless_headers.sh` poisons graphics SDK include paths and
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
emulator/timeout exit status. `scripts/test_verify_gate.py` exercises failure
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
`ctest/capi_testlib.so` for integer, FP and syscall call probes, preserving the
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
see [the vkQuake reproduction](vkquake.md).

The fully provisioned suite contains 275 configured test runs:

| Category | Runs | Notes |
|---|---:|---|
| Unit | 97 | Focused guest instruction/JIT regressions and registered host JIT gates |
| Integration | 86 | Guest programs and subsystem checks in `ctest_real/` and `test/` |
| Sandbox | 1 | `BIFROST_ROOT` path-boundary regression |
| Toybox | 9 | Commands run through the committed AArch64 Toybox binary |
| Real-world static | 49 | BusyBox and Toybox command checks; BusyBox may be fetched by the runner |
| Real-world dynamic (glibc) | 7 | Part of the real-world fixture set; requires rootfs libraries |
| Dynamic | 16 | Dynamically linked musl/glibc and XWayland tests; requires rootfs and built fixtures |
| Benchmarks | 5 | Performance smoke benchmarks; omitted by `--quick` |
| Interactive | 5 | Stdin-driven programs run with scripted input |
| **Full configured suite** | **275** | Includes the sandbox and rootfs-dependent dynamic runs |

Without a configured rootfs, the 7 dynamic real-world runs and 16 dynamic
tests are not selected, for 252 configured runs (247 with `--quick`). These
figures describe test definitions selected by the runner; missing guest
fixtures and unavailable display/SDL support can result in skips.

## Dispatch cache and profiling regression (2026-10-04)

`jit_dispatch_cache` is registered in the main suite. It checks exact results
for indirect calls across three guest threads, code replacement after workers
join, and data-only mmap/munmap churn. QEMU and both engines agree. The bounded
cache-pressure script also passes at 1 MiB and 64 MiB, including cached
fallback decisions and logical growth. SDL thread lifecycle/nonlocal-return
checks pass in both engines.

`bash scripts/test_jit_profiler.sh` passes the independent host regression for
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

- `ctest/jit_simd_fp_indexed.c`: 28 indexed FMUL lane, aliasing and upper-half
  checks, independently validated in QEMU and both engines.
- `ctest/jit_round_away.c`: 6,984 FRINTA/FCVTAS fixed-oracle cases for ties,
  adjacent values, signed zero, NaNs and saturation.
- `ctest/jit_scalar_dup.c`: 60 scalar lane-copy cases covering all 30 valid
  lane/width combinations with and without source/destination aliasing.
- `ctest/jit_doom_gap.c`: 2,054 oracle checks, including all conditional
  compare conditions/NZCV inputs in single/double precision, FCVTL, SHLL,
  ADDV result width and fused indexed FMLA.
- `ctest/native_doom_gaps.cpp` with `ctest/doom_gap_opcodes.inc`: actual JIT
  execution for all 625 captured gap encodings, 32 randomized states each.
  Checks registers, flags, memory, post-index writeback and exclusive-monitor
  success/failure. Interpreter comparison complements the independent fixed
  oracles; it is not itself a hardware oracle. Also passes with
  `BIFROST_TIER2_HITS=1` to exercise early promotion decisions.
- `ctest_real/test_sdl_thread.c`: create/wait, eight concurrent workers,
  detach, and 16 nested `setjmp`/`longjmp` returns. Tests both the JIT SDL
  runner and the explicit `--no-jit` path.
- `scripts/test_fallback_profile.sh`: exact concurrent snapshot retention
  after worker exit, with 192 complete entries and 192,000 counts.
- `scripts/test_window_stats.sh`: presentation clock, default rendering log,
  default title/idle FPS and enable/disable controls. UI placement, compositor stacking
  and overlay capture require separate desktop checks; see
  [window-stats limits](window-stats.md).

```bash
make -j8 USE_SDL2=1 USE_THUNK_GL=1 build/release-sdl1-gl1/bifrost-emu
make cross SRC=ctest/jit_doom_gap.c OUT=ctest/jit_doom_gap.elf
make cross SRC=ctest_real/test_sdl_thread.c OUT=ctest_real/test_sdl_thread.elf
BIFROST_IR_VALIDATE=1 scripts/run_tests.sh --emu build/release-sdl1-gl1/bifrost-emu --strict
BIFROST_IR_VALIDATE=1 scripts/run_tests.sh --emu build/release-sdl1-gl1/bifrost-emu --no-jit --strict
bash scripts/test_native_doom_gaps.sh
BIFROST_JIT_VERIFY=1 BIFROST_JIT_VERIFY_MEM=1 BIFROST_IR_VALIDATE=1 build/release-sdl1-gl1/bifrost-emu ctest/jit_doom_gap.elf
build/release-sdl1-gl1/bifrost-emu ctest_real/test_sdl_thread.elf
build/release-sdl1-gl1/bifrost-emu --no-jit ctest_real/test_sdl_thread.elf
bash scripts/test_fallback_profile.sh
EMU=build/release-sdl1-gl1/bifrost-emu bash scripts/test_window_stats.sh
```

The standalone captured-opcode harness links the built SDL/GL objects and
expects that configuration. Build all registered guest fixtures with
`make setup-tests` before running the complete suite on a new checkout.

`ctest/bench_doom_vector_fp.c` and `ctest/bench_doom_round_lane.c` are optional
instruction-throughput probes, not full-game FPS benchmarks. See
[Doom 3 validation](doom3.md) for measured scope and the longer user gameplay
run. Neither the tests nor the recorded presentation counter establish the
planned v2.0 60+ FPS target across demanding gameplay scenes.

## Game compatibility follow-up (2026-10-03)

`ctest/jit_shift_register.c` adds 182 SSHL/USHL checks for signed low-byte
counts, logical/arithmetic right shifts, lane widths, Q forms, aliasing, and
out-of-range counts. It passes under QEMU and both emulator engines.
The six selected shift/permute regressions also pass in both engines.

`scripts/run_thunk_compat.sh` exercises SDL event-filter registration,
accept/reject returns, nested callbacks, signed mouse deltas, and exact-size
output bounces, alongside GL ABI and Vulkan mapped-memory upload/readback
checks. These focused tests do not establish correct interactive vkQuake
sensitivity; see [vkQuake status](vkquake.md).

## Memory and signal regressions

`ctest/test_mem_guard.c` checks wrapping `munmap`, stack overlap rejection,
bounded huge `madvise`, and sequential above-window fixed mapping cycles.
On 2026-10-02, the failing local ELF still contained the older threaded
implementation. Rebuilding the current source passed 40/40 runs in each of
JIT and interpreter mode; this does not establish correctness of that older
threaded workload.

`ctest/test_memory_permissions.c` checks existing read-only protections,
permissions and unmapped holes inherited across fork, isolation of child
repairs, syscall `EFAULT`, and brk shrink/regrowth/collision behavior. Fault
probes check the guest PC and bound retries. Page sizes come from `sysconf`;
LSE checks run only when advertised by `AT_HWCAP`.

`ctest/test_sigreturn_context.c` checks handler edits to registers, PC, SP,
NZCV, FP/SIMD state and signal masks, nested alternate stacks, and nine
malformed signal-frame cases. These exercise the Linux AArch64 ABI; they
are not a complete Arm architecture conformance suite.

For independent QEMU runs, see the
[reference signal-mask patch](../tools/qemu-reference/README.md). It fixes
QEMU's handling of unblockable mask bits while keeping the Linux assertions
strict.

`ctest/test_vm_signal_contracts.c` checks fixed `PROT_NONE` mapping contents,
file-offset preservation with concurrent reads during `mmap`, `mremap` move
permission and fixed destinations, and alternate-stack changes during a
handler. Its 47 checks also pass on native Linux. QEMU 11.1.50 disagrees on
the zero-size `mremap` errno (`ENOMEM` instead of Linux's `EINVAL`); that
assertion remains strict.

## zstd, shift, and bitfield regressions

`ctest/jit_variable_shift_source.c` checks that all four variable shifts and
rotates preserve their count source, including packed high bits, at both
32-bit and 64-bit widths. `ctest/jit_shifted_operand_width.c` checks W-form
shifted operands with nonzero upper X bits and block-local constant folding.
Build these sources with `make setup-tests` or `make cross`.

`ctest/jit_bitfield_regalloc.c` checks bitfield results and live source values
under register pressure, memory stores/reloads, and Tier-2 side exits.

`scripts/run_zstd_regression.sh` uses the bundled static AArch64 zstd fixture
and requires the host `zstd` CLI. It compresses a deterministic corpus using
both JIT and interpreter, with default workers and `--single-thread`; host
zstd validates each archive and restores bytes for comparison. Both guest
engines also decode a host-generated archive. The main suite runs this host
harness once through `HOST_JIT`, and skips it under `--no-jit` because the
harness already exercises both engines.

```bash
./scripts/run_tests.sh --filter 'variable_shift_source|shifted_operand_width|zstd'
./scripts/run_tests.sh --no-jit --filter 'variable_shift_source|shifted_operand_width|bitfield_regalloc'
./scripts/run_zstd_regression.sh
```

The full working-tree suite passed 240/240 on 2026-10-01 with
`BIFROST_IR_VALIDATE=1` and `BIFROST_REGALLOC_CHECK=1`; that run included the
bitfield regression described above. Both focused shift tests
also passed JIT verification and memory verification. Applying those settings
to threaded zstd logged divergences and exceeded its 90-second compression
timeout, so that diagnostic run remains unresolved. See [zstd.md](zstd.md)
for the two fixes, fixture scope, and verification limits.

`ctest/jit_memory_guard_address.c` exercises cached constant addresses across
permission guards, stack saves, pointer loads, and a conditional exit. It
checks the value after repeated calls, including JIT verification and the
interpreter.

## SDL fullscreen regression

`ctest_real/test_sdl_fullscreen.c` checks SDL2 desktop/current display-mode
status returns and caller-owned output structs, invalid display indices,
three fullscreen/windowed recreation cycles, fullscreen flag transitions,
restored dimensions, and resize-event payloads. It exercises the sequence used
by Neverball to save its dimensions. Run it on a desktop, or use SDL's dummy
video driver for a bounded headless check:

```bash
make cross SRC=ctest_real/test_sdl_fullscreen.c OUT=ctest_real/test_sdl_fullscreen.elf
SDL_VIDEODRIVER=dummy ./scripts/run_tests.sh --filter '^sdl_fullscreen$'
SDL_VIDEODRIVER=dummy ./scripts/run_tests.sh --no-jit --filter '^sdl_fullscreen$'
```

The regression creates hidden windows so compositor placement rules and an
interactive game do not interfere with its requested-size assertions.
It returns exit 77 when SDL video cannot initialize. The pre-fix build fails
because the mode-query thunk does not fill the guest's dimensions. The repaired
build passes with the dummy driver and real desktop in both execution modes.
See [neverball.md](neverball.md) for the root cause and current interactive status.

## JPEG SIMD narrowing regression

`ctest/jit_simd_sat_narrow.c` covers the six signed/unsigned saturating narrowing
shift forms and nonsaturating RSHRN, each at 16→8, 32→16, and 64→32 bits.
Its 672 checks cover minimum/maximum shift amounts, both destination halves,
source/destination aliasing, rounding overflow, and sticky FPSR.QC.

```bash
make cross SRC=ctest/jit_simd_sat_narrow.c OUT=ctest/jit_simd_sat_narrow.elf
./scripts/run_tests.sh --filter '^simd_sat_narrow$'
./scripts/run_tests.sh --no-jit --filter '^simd_sat_narrow$'
qemu-aarch64 ctest/jit_simd_sat_narrow.elf
```

On 2026-10-02, all 672 checks passed under QEMU, JIT with IR validation and
register-allocation checks, and interpreter. The saved pre-fix interpreter
failed 632 checks. The related SIMD suite passed 19/19 in each Bifrost mode.
Direct libjpeg decodes of four Neverball JPEGs, including the hard-set preview
and two Help images, matched QEMU's raw RGB output byte-for-byte in both modes.
The affected JIT narrowing family now uses the corrected interpreter handler.
See [neverball.md](neverball.md) for the diagnosis and image coverage.

## Stability follow-up: 2026-10-02

At the October 2 checkpoint, the working-tree quick suite passed 238/238
in JIT mode and 236/236
in interpreter mode, with two intentional interpreter skips (`tier2_smov`
and the dual-engine `zstd_compression` host harness). Four stale fixtures were
rebuilt before the clean full JIT rerun. `test_mem_guard` separately passed
100 consecutive runs in each mode with no failures; its high-address mapping
coverage is single-threaded. See [neverball.md](neverball.md) for the associated
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
./scripts/test_late_tls.sh          # late GD/TLSDESC and rejected initial-exec
./scripts/test_late_tls_pthread.sh  # existing/new threads, reuse, pointer/alignment/weak cases
./scripts/run_thunk_compat.sh       # SDL/GL/Vulkan ABI probes in both modes
./scripts/run_thunk_soak.sh 64      # repeated SDL window/mapping/thread lifecycle
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

./scripts/run_tests.sh --unit                 # focused JIT regressions
./scripts/run_tests.sh --unit --strict        # fail if a selected fixture is missing
./scripts/run_tests.sh --integration          # integration tests
./scripts/run_tests.sh --dynamic              # dynamic-linker tests
./scripts/run_tests.sh --filter 'sig|brk|pipe' # filter test names
./scripts/run_tests.sh --no-jit --quick       # interpreter quick suite
./scripts/run_tests.sh --help                 # all runner options
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

For repeated performance runs, use `scripts/run_benchmarks.py`. It performs
warmups, validates result markers, measures host wall time, and saves host,
compiler, emulator, environment, and sample metadata to JSON:

```bash
make USE_SDL2=0 USE_THUNK_GL=0
make setup-tests
./scripts/run_benchmarks.py --emulator build/release-sdl0-gl0/bifrost-emu \
  --warmups 2 --runs 7 --mode both
```

CoreMark is added to the default benchmark set when `ctest/coremark.elf` is
available; it can also be selected explicitly with `--bench coremark`. Its
four CRCs are checked. The CoreMark source is not bundled with the repo.

## Adding a test

1. Add a self-checking AArch64 source under `ctest/` or `ctest_real/`.
2. Add its `.elf` path and expected output pattern to the matching array
   in `scripts/run_tests.sh`.
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
semantics remain separate gaps. See [runtime details](ANDROID_RUNTIME.md)
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
in [ANDROID_RUNTIME.md](ANDROID_RUNTIME.md#native-sensor-service-profile-2026-10-07).
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
