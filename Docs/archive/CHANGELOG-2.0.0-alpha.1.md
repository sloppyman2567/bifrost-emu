# Changelog archive: 2.0.0-alpha.1

Older releases moved out of [the changelog](../CHANGELOG.md), unchanged and in their original order. Version labels in the headings are historical.

## [2.0.0-alpha.1] — first v2.0 pre-release (2026-10-12)

**2.0.0-alpha.1** is the first alpha release toward the
[v2.0 roadmap](../roadmap.md). This section records its changes since 1.5.5
(2026-10-04 onward). The stable v2.0 goals remain in progress; older version
labels below are historical records.

### Generated native SIMD table and coverage sweep (2026-10-12)

- FrostJIT's native SIMD classifier, kind enum and dispatch list are
  generated from `Tools/CodeGen/native_simd.txt` (`make nativegen`). They
  are identical to the hand-written classifier over all 2^32 words. A
  generated test runs every accepted encoding against QEMU hashes
  (`make nativegen-expect`). It found 605 unallocated encodings that ran
  natively: shift-by-immediate rows missing bit 23, `.1d` FP/integer
  compares and conversions, double FMOV (immediate) without Q, and ADDV
  `.2s`.
- `Scripts/Testing/simd_coverage.py` sweeps the FP/SIMD data-processing
  space under QEMU and Bifrost. Reserved DUP (element) and INS (general)
  encodings crashed both engines with a host SIGFPE (a zero element size
  used as a divisor); they now raise SIGILL. DUP (general) accepts the
  ignored high imm5 bits and rejects `.1d` without Q.
- Vector FCVTZS/FCVTZU saturate ±infinity (only NaN gives 0) in both
  engines, and `.1d` raises SIGILL. `jit_simd_misc` had encoded the old
  result, which QEMU rejects.

### App ops service (2026-10-12)

- `getSystemService(APP_OPS_SERVICE)` returns an `AppOpsManager`. The new
  strict `appops` service implements `checkPackage` per Android 15
  AppOpsService, which Firebase measurement uses to verify its caller.
- `IActivityManager.getContentProvider` returns no holder for authorities
  no package declares (such as Play services' phenotype provider), as
  AOSP does, instead of throwing. Firebase measurement now initializes
  without errors.

### Window growth and SIGKILL exit (2026-10-12)

- Growing the host window no longer leaves full-window software views
  undrawn. `ViewConfiguration`'s cached drawing-cache limit is rebuilt
  after each resize, which fixes the black screen after SpongeBob levels
  (a Kidoz ad WebView).
- A guest SIGKILL to its own process (`Process.killProcess`) ends the host
  process immediately with status 137. The cooperative group exit waited
  for every thread to unwind, and SpongeBob kept running at full CPU until
  terminated by hand. Other fatal signals still use the group exit.

### Acquire-load and trial-timing overhead (2026-10-12)

- Emit aligned acquire loads through the guarded native memory path, avoiding
  the exclusive-monitor helper/mutex while retaining atomic IR ordering,
  permissions, userfaultfd and precise faults. Initialize the direct-window
  base in acquire-only blocks; keep existing exclusive and fallback paths.
- Start linear timing trials with a guest-work-based quota and cap them at
  8,192 entries. Keep CPU-time clocks and the existing evidence thresholds.
  Add width/alias/reservation/fault and bounded-timing regressions. An isolated
  acquire loop improves 13.43x; post-change game FPS remains unmeasured.

### Codegen roadmap documentation (2026-10-12)

- Record the planned v2 compiler direction: explicit guest state, mixed
  FP/vector allocation, precise recovery/helper contracts, region register
  layouts and bounded layout selection. Separate these targets from the
  implemented superblock and adaptive-size checkpoints.
- Define a 10,000-iterations/s CoreMark target and conditional 26%-of-native
  comparison, with correctness, duration, native-baseline and gameplay gates.
  This update changes documentation only and establishes no new speedup.

### Bounded superblocks and region vector caching (2026-10-12)

- Make `BIFROST_TIER2_TRACE=1` concise: compiled candidates and timing
  decisions. Move discovery, per-block and compiler details to the cached
  `BIFROST_TIER2_TRACE_VERBOSE=1` diagnostic switch.
- Measured losing Tier-2 superblocks retry shorter prefixes (8 -> 4 -> 2)
  after fresh Tier-1 hotness and branch profiling. Work mismatches and
  inconclusive trials retain Tier-1. Existing compilation/phase budgets bound
  search; phase changes can reconsider longer paths. Timing regressions cover
  shorter winners, terminal fallback, invalidation and immutable MT code.
- Experimental Tier-2 fuses hot paths of up to eight blocks/256 instructions,
  including linear prefixes, with cross-block scalar SSA and measured live-work
  admission. `BIFROST_JIT_SUPERBLOCK_LIMIT=2..8` bounds smaller comparisons.
- Region codegen retains compatible vector/scalar-FP cache layouts across
  internal edges. Side exits, timing exits and fault recovery preserve
  loop-carried XMM state, including writes later on a previous iteration.
- Preserve invariant preheader homes during destructive destination reuse,
  repairing a pre-existing Tier-2 single-thread zstd crash.
- Add eight-block exit/invalidation, vector state, invariant lifetime and
  linear timing regressions. Synthetic FP ADD regions improve by roughly
  2–4x with caching; eight-block CoreMark is approximately neutral. Tier-2
  remains opt-in and no game-FPS improvement is claimed.

### Indirect branches, watchdogs and lazy controllers (2026-10-12)

- Chain-skip register branches use a per-host-thread IBL cache, with generation
  revocation, callback stop/depth guards and the existing 256-edge lease.
  Ordinary frames and `BIFROST_NO_CHAIN=1` retain dispatcher exits. Tier-2
  timing trials keep their private graph by unbinding the table.
- Differential replay now unbinds IBL while executing the measured block.
  Static-edge unpatching alone let warmed indirect targets escape the replay
  boundary and caused false PC/register divergences in the verification gate.
- Audited signal return: the handler may chain into the executable trampoline;
  its unchainable SVC returns the restored PC to dispatch. Added warmed
  trampoline and executable host-boundary checks to the existing publication
  fixture for ST/MT and both frame layouts.
- Same-PC/frozen-loop and JIT dispatch watchdogs are disabled by default;
  `BIFROST_HANG_WATCHDOG=1` enables these diagnostics. A valid main/worker
  60-million-call loop is covered by `test_long_call_loop`.
- SDL controllers initialize on first input poll, joystick read or controller
  query, avoiding device enumeration during construction for programs that
  never use input. Already-connected and later hot-plugged devices retain
  the existing event path. See [validation and limits](../TESTS.md#indirect-branches-watchdogs-and-lazy-controllers-2026-10-12).

### Yggdrasil sandbox and path syscall fixes (2026-10-12)

- **Sandbox escape fixed.** Under `BIFROST_ROOT` (including Android
  `--rootfs` runs) several path syscalls rewrote the path lexically and let
  the host follow symlinks. A guest symlink could make `truncate`,
  `renameat2`, `utimensat`, `fchownat` and following `linkat` modify or
  hard-link files outside the root, and the stat family, `statfs`,
  `faccessat`/`faccessat2`, `readlinkat` and `inotify_add_watch` could
  inspect them. These calls now resolve the path confined
  (`openat2 RESOLVE_IN_ROOT`) to an O_PATH fd and act on that file; renames
  use the confined parent. `umount2` returns EPERM like `mount`.
  Unsandboxed runs keep their direct host calls.
- `chmod()` failed with EINVAL for every guest: the raw `fchmodat` syscall
  has three arguments, but x3 was forwarded as flags.
- cwd-relative `mkdir`, `rmdir`, `unlink`, `rename`, `symlink`, `link` and
  `mknod` failed with a stale errno since the sandbox hardening.
  `ParentDir::valid()` rejected `AT_FDCWD`.
- `fchownat` and `faccessat2` passed the raw guest dirfd to the host, and
  `statx(fd, "", AT_EMPTY_PATH)` dropped `AT_EMPTY_PATH`.
- New `test_path_syscalls` (qemu-aarch64 reference passes; the previous
  binary fails 13 of 14 checks). `test_sandbox` now covers every confined
  call (84 checks), and the runner confirms on the host that the outside
  canary is untouched. The previous binary emptied, renamed next to and
  hard-linked that canary.

### Version, guest layout and descriptive names (2026-10-12)

- Every current-version reference (`Version.hpp`, C API, CLI, window titles,
  generated config, rootfs `os-release`, README, Makefile) reports
  2.0.0-alpha.1. Inline release tags such as `(1.5.4-alpha)` were removed from
  code comments; release history stays in this changelog and `Version.hpp`.
- `Guest/` is grouped by role: Android `Include/`, `Adapters/`, `Launchers/`,
  `Managed/` (with `Framework/`) and `Probes/`, plus `Guest/VDSO/` (moved from
  `Tools/VDSO/`; a byte-identical copy of the embedded vDSO header was dropped).
  Adapter, JNI and managed-launcher builders use the new paths.
- The catch-all `Syscalls/Miscellaneous.cpp` (`syscall_misc*`) is gone. Calls
  moved verbatim to their families: socket calls to `Files/Sockets.cpp`,
  inotify/signalfd to `EventIO.cpp`, exit/waitid/unshare to `Process/`, file
  calls into a split `Files/` (`Descriptors`, `Paths`, `Metadata`, `Transfer`),
  and Bifrost-private 0x1000+ calls to `HostCalls/`. Every moved case body is
  unchanged apart from public-accessor use; a dead duplicate `setresgid` case
  (always shadowed by `Compatibility.cpp`) was removed. The 0x1000 thunk fast
  path now calls the thunk handler directly.
- Other generic names: thunk `Core.cpp` files are named after their classes,
  `DispatchHelpers.cpp` is `Streams.cpp`, the interpreter's SIMD
  `Miscellaneous.cpp` is `TwoRegisterPermute.cpp`, the legacy
  `Core/Common.hpp` umbrella was removed and `test_misc_extended` is
  `test_compat_syscalls`. First-line comments naming pre-restructure paths
  (`jit/…`, `yggdrasil/…`, `syscalls/misc_…`) now name the actual files.
- Large files split by responsibility: `Memory.cpp` (+`Allocation.cpp`,
  `Discard.cpp`), `IR.hpp` (+`Parameters.hpp`, `Factories.hpp`; includers are
  unchanged) and `ManagedLauncher.java` (native platform Binder services moved
  to `Managed/Framework/FrameworkNativeServices.java`).
- The Makefile `test` target and suite comments use `Tests/Asm/` instead of the
  pre-restructure `test/` path.
- Both builds, full JIT/interpreter suites, `make verify`, host API and Android
  adapter/framework/Bionic/Asgard checks pass. The JNI fixture runner now
  stages its DEX read-only: API 35 ART rejects writable DEX files for the app
  UID that Android-runtime guests use, which had failed the fixture since app
  identities were added. See
  [the validation record](../TESTS.md#200-alpha1-layout-names-and-splits-2026-10-12).

### Bounded multithreaded JIT loops (2026-10-11)

- Conditional Tier-1 self-loops retain their frame, eligible GPR pins and cached
  vectors in MT mode. Immutable guards use revocable body-target cells, preserving
  the 256-edge lease, callback boundaries and code invalidation. Body cells cannot
  be redirected into a different translation's register/frame layout.
- MT flag-setting branches commit NZCV before checking the loop guard. Cold exits
  save carried register/vector state; both frame layouts and existing opt-outs work.
- In a matched synthetic batch, counter-loop CPU time falls 18%, and vector-loop
  throughput rises 6.4x; a flag-heavy loop is unchanged. These are loop results,
  not game-performance claims. Four-worker zstd passes native round-trip validation.
- Full JIT, differential/SSA, MT/Tier-2 and expanded immutable-publication oracles
  pass. See [validation and timing limits](../TESTS.md#bounded-mt-self-loops-2026-10-11).

### JIT flag-arithmetic spills (2026-10-11)

- ADDS/SUBS/TST retain allocated operand registers, reusing last-use scratch
  inputs where possible. Unused SUBS results use CMP without overwriting their
  inputs; W results retain zero-extension and W comparisons preserve full inputs.
- Two-input allocation protects a cached operand while loading its partner,
  avoiding needless spill/reload pairs under pressure.
- CoreMark's 354 benchmark-body blocks emit 455 → 394 spill instructions
  (13.4% fewer, including cold fault paths). This is a static count, not a
  dynamic traffic or gameplay measurement. Guards and block limits are unchanged.
- Extended `jit_same_operand` covers live inputs, both destination aliases and
  NZCV. Full JIT, differential/SSA and Tier-2 checks pass; see
  [the validation record](../TESTS.md#flag-arithmetic-register-pressure-2026-10-11).

### Source layout and contributor guidance (2026-10-11)

- Grouped implementation by `Source/FrostJIT/`, `Source/Interpreter/` and
  `Source/System/`, with implementation headers beside their source. Public and
  generated headers now live in `Include/Bifrost/` and `Include/Generated/`.
  Guest adapters, tools, scripts, tests, docs, assets and vendored dependencies
  have their own purpose-based directories; see [the source map](../ARCHITECTURE.md).
- Split emulator startup, dynamic linking, Tier-2, SIMD code generation,
  audio/graphics/display thunks and DisplayProxy by responsibility. Tier-1
  fallback/leaf analysis and verification-store metadata are separate from the
  block compiler. This changes organization, not the supported guest ISA.
- Repaired adapter/probe/downloader/audit paths and recursive headless header
  checks. Framework contract fixtures now install the required Activity JNI bridge.
  A follow-up audit repaired SDL2 SDK directory capitalization and printed build
  paths, verified with an isolated extraction/cache fixture, and refreshed stale
  source navigation comments.
- Source manifests trigger relinking after removals/renames; libraries are
  archived fresh so obsolete members cannot survive an incremental refactor.
- Reduced root AGENTS.md to project-wide rules/navigation, with scoped subsystem
  contracts and existing detailed documentation linked from it.
- Moved existing GLES mapping policy rows outside generic generated coverage,
  preserving their runtime policies and repairing pre-existing coverage drift.
- Clean GPU/headless builds, full JIT/interpreter suites, generated checks,
  differential/SSA, host API and managed Android contracts pass. See
  [the validation bounds](../TESTS.md#subsystem-layout-and-source-splits-2026-10-11).

### JIT native-code reduction (2026-10-11)

- Chain-skip is the default frame layout (`BIFROST_NO_CHAIN_SKIP=1` opts out;
  `run_tests.sh --no-chain-skip`). CoreMark: 4,365 → ~7,400 it/s with the
  changes below (QEMU 11.1 TCG: 7,699).
- NZCV commit uses LAHF/SETO/IMUL and the pstate→flags load uses SAHF; both
  drop PUSHFQ/POPFQ, and the separate CF-normalize helper is folded in.
- The sticky software-guard flag moved to a header page below the guest
  window, tested with one `cmp byte [r10-8], 0` per native access.
- Fixed: a mid-block interpreter fallback that delivered a signal left
  through the chain slot and ran the static successor instead of the
  handler (both layouts). Regression: `ctest/jit_interp_exit.c`.
- `run_benchmarks.py` runs CoreMark at 110,000 iterations; 60,000 now
  finish under CoreMark's ten-second minimum.

### Installed managed Activity creation (2026-10-07)

- The existing Activity probe now loads installed managed APKs through the real
  framework class loader, Application and launcher `onCreate`. SpongeBob's
  CleverTap Activity and IL2CPP JNI startup pass in both engines; rendering,
  input, provider installation and Activity resume are not covered.
- The strict local Activity controller tracks manifest/requested orientation.
  The native Binder registry publishes `servicemanager.ready` only when
  selected, allowing native libbinder startup to proceed without a debug override.
- `test_android_framework.py --installed-app` verifies installed content and
  derives sorted split paths. App success requires app-specific checkpoints,
  preventing an older built-in-only probe from reporting APK startup success.
- The Bionic bootstrap tests services on/off across all execution modes and
  ASLR layouts, explicitly forwards its test environment into the guest, and
  requires the Binder/prctl checks to execute.

### JIT host-fault recovery (2026-10-07)

- Scalar loads/stores, single-vector accesses and native LSE atomics use
  registered host-fault sites instead of always reading guest page flags.
  Bounds checks remain. Recovery commits pre-instruction state and leaves
  the block after precise guest signal delivery, including file EOF SIGBUS.
- Pair/multi-register accesses and exclusive helpers retain software checks.
  A sticky fallback preserves userfaultfd and W/X-only guest permissions;
  `BIFROST_JIT_SOFTWARE_GUARD=1` forces the previous path for comparison.
- Stable atomic fault metadata supports concurrent compilation, codegen
  rollback and cache exhaustion. Unregistered host faults retain the prior
  host handler or default disposition.
- `ctest/jit_host_fault.c` checks four-CPU fault recovery, saved registers,
  NZCV and vectors, with a main-CPU interpreter mode and optional native-JIT
  file SIGBUS probes. QEMU independently passes those fixture modes.

### Guest memory as one host reservation (2026-10-07)

- The guest address space is a single host reservation (64 TiB, halving to
  512 GiB when the host refuses): guest VA x is at `base + x`, and host
  mmap(MAP_FIXED)/mprotect/madvise/munmap implement guest mappings. The
  sparse tier that held everything above 4 GiB (`pages_`, high mapping and
  userfaultfd range lists, separate shared backings) is gone; `memory.cpp`
  shrinks from 2506 to about 1100 lines. A coalesced `vmas_` index answers
  range queries, and the per-page flag mirror is committed lazily, so
  TiB-scale PROT_NONE reservations cost no per-page work.
- Memory above 4 GiB is now as fast as below: `ld.so`, thunk libraries and
  ART's heaps no longer go through a locked map. The Android Activity probe
  runs in 8.4 s JIT / 18.0 s interpreter (22.6 s / 45.0 s before), the
  framework probe in about 5 s JIT (about 11 s). Programs that stay below
  4 GiB (the benchmarks, static musl games) are unchanged.
- MAP_SHARED files are mapped straight into the reservation at every
  address, MADV_DONTNEED uses the host call, and munmap above 4 GiB returns
  memory to the host. Virtual reservations (MAP_NORESERVE or PROT_NONE) are
  no longer capped at 64 GiB.
- Guest addresses beyond the reservation cannot be mapped (MAP_FIXED fails,
  hints fall back); Linux allows up to 256 TiB on 48-bit AArch64.
- `mremap` of a shared file only extends in place when the tail mapping
  lands right after it; it used to accept a hint placed elsewhere.
- Regression: `ctest/test_mem_reservation.c`.

### FP/SIMD interpreter split, ARM NaN rules, JIT MSR NZCV (2026-10-07)

- `interp_fp.cpp` (4951 lines) is split into a dispatcher plus
  `interp_simd_three.cpp`, `interp_simd_misc.cpp`, `interp_simd_imm.cpp`,
  `interp_simd_arith.cpp` (SIMD_DP groups, run in the old order) and
  `interp_fp_scalar.cpp`, with shared helpers in `interp_fp_common.hpp`.
  A 300k random-encoding differential against the pre-split binary shows no
  change on QEMU-legal encodings except NaN payloads (next item).
- Vector FADD/FSUB/FMUL/FDIV/FMAX/FMIN/FMAXNM/FMINNM/FMULX/FABD/FMLA/FMLS and
  the FP pairwise ops follow the ARM ARM in the interpreter: first
  signalling, then first quiet NaN; +qNaN for invalid operations; +0 above
  -0; FMAXNM/FMINNM prefer a number to a lone quiet NaN; FMLA/FMLS are
  fused. Host arithmetic let GCC's operand order choose the NaN, which the
  split changed. On a NaN-heavy fuzz of these ops the interpreter now
  matches qemu-aarch64 on 106084/106084 samples (was 86819).
  Regression: `ctest/jit_simd_fp_nan.c` (interpreter-only `INTERP` row;
  the JIT's native vector FP still uses x86 NaN rules).
- JIT `msr nzcv, xN` ran through the interpreter, which read `cpu.regs[N]`.
  `optimize_ir` had dropped that store when the next instruction rewrote
  xN, so NZCV took the old value. It is now native from the IR operand; DC
  and IC become `CALL_INTERP` for the same reason. Regression:
  `ctest/jit_msr_nzcv.c` (the old binary fails 30005/64000 checks).
- `BIFROST_BLOCK_TRACE` lines include NZCV and the raw pstate.

### Teeworlds: /dev/urandom no longer runs dry, vector fixed-point conversions (2026-10-07)

- `/dev/random` and `/dev/urandom` pass through to the host character
  devices, like `/dev/null` and `/dev/zero`. They were memfds holding 256
  random bytes, so a guest that keeps the device open reached EOF: a
  Teeworlds client draws an 8-byte token seed per network client every
  16 s and hit its short-read trap in `secure_random_fill` after about four
  minutes. Reads of any size now return fresh bytes, `fstat` reports the
  1:8/1:9 character device, `lseek` returns 0 and writes are accepted.
  Regression: `ctest/test_dev_urandom.c`.
- The interpreter implements the vector fixed-point conversions
  `SCVTF`/`UCVTF`/`FCVTZS`/`FCVTZU` (`.2s`/`.4s`/`.2d`, `#fbits`), decoded
  as QEMU's `*_vf` patterns. Dragging a quad point in the Teeworlds map
  editor ran `fcvtzs v31.2s, v30.2s, #10` and raised SIGILL. Half precision
  (FEAT_FP16, not advertised) and `.2d` with Q=0 stay UNDEF. The JIT
  reaches it through CALL_INTERP. Regression: `ctest/jit_simd_fixconv.c`.

### libandroid loopers, choreographer and sensors (2026-10-06)

- `ALooper` is per guest thread, following AOSP `Looper::getForThread`:
  `ALooper_forThread` is new, `pollOnce`/`pollAll` use the calling thread's
  looper (`ALOOPER_POLL_ERROR` without one), `addFd` rejects a NULL
  callback unless the looper was prepared with `ALLOW_NON_CALLBACKS`, and a
  callback returning 0 unregisters its fd. The old process-wide looper let
  any polling thread run every thread's callbacks.
- `AChoreographer_getInstance` and `postFrameCallback`/`64`/`Delayed`/
  `Delayed64`: one-shot frame callbacks run inside the owning thread's poll
  at the next 60 Hz `CLOCK_MONOTONIC` tick, with that tick as frame time.
- `ASensorManager`/`ASensorEventQueue`/`ASensor` report a device with no
  sensors (AOSP `sensor.cpp` NULL-sensor and empty-queue results).
- Every SpongeBob `libunity.so` import now resolves.
  Regression: `ctest_real/test_android_looper.c`.
- The interpreter implements the vector FP pairwise family (`FADDP`,
  `FMAXP`, `FMINP`, `FMAXNMP`, `FMINNMP`; `.2s`/`.4s`/`.2d`). A Unity
  constructor's `faddp v17.2s, v17.2s, v18.2s` raised SIGILL; the native
  probe now loads all 21 SpongeBob libraries. The JIT reaches it through
  CALL_INTERP. Regression: `ctest/jit_simd_faddp.c`.

### Android EGL exports (2026-10-06)

- The EGL thunk and the Android `libEGL.so` adapter gain the rest of EGL 1.5
  core (`eglSurfaceAttrib`, `eglGetConfigs`, current-state queries,
  `eglQueryContext`, fence syncs, images, texture binding) and the KHR image,
  fence/reusable sync and damage functions that Android's libEGL exports.
  Extension functions resolve through host `eglGetProcAddress`. Unity
  (SpongeBob) now finds all its EGL and GLES imports and stops at
  `AChoreographer_getInstance` in `libandroid.so`.

### userfaultfd and ART's CMC collector (2026-10-06)

- `userfaultfd` (SIGBUS mode) is implemented: `UFFDIO_API`/`REGISTER`/
  `UNREGISTER`/`COPY`/`ZEROPAGE`/`WAKE`, with missing registered pages
  raising SIGBUS `BUS_ADRERR`. `mremap` accepts `MREMAP_DONTUNMAP`.
- Both work on either side of the 4 GiB window. ART's compaction pause
  moves its window heap into a from-space reservation above the window,
  and registers its 1 GiB linear-alloc pool there; the window-only first
  version failed with `EINVAL` and aborted on the first compaction.
- Framework provisioning schema 5 loads the `system_ext` and `product`
  `build.prop` files, which select CMC. With it, ART keeps the boot image:
  the Activity probe passes in 26.8 s JIT and 47.2 s interpreter (was
  131.4 s and 226.1 s), the framework probe in 10.2 s JIT (was 63.5 s).

### Self-loop SMOV pinning (2026-10-06)

- A self-loop block could pin a register that `smov` writes, so the next
  iteration read the value from before the `smov`. Tier-2 regions already
  excluded SMOV; both pin selectors now share one list of ops that write a
  general register directly.

### Per-thread JIT crash (2026-10-06)

- With `BIFROST_NO_SHARED_JIT=1`, another thread's code invalidation (any
  `mmap`/`munmap`/`mprotect` over translated code) toggled the running
  thread's W^X code buffer to RW, and the emulator died with a host SIGSEGV.
  Per-thread JITs and the main JIT now enter multithreaded mode, whose code
  buffer stays RWX and whose instructions are never patched in place.

### Group-wide process exit (2026-10-06)

- `exit_group` and fatal default signals end the whole process from any
  thread. A fatal signal used to stop only the receiving thread, so ART's
  `Process.killProcess(myPid(), 9)` left its daemon threads running and the
  emulator never exited; `exit_group` from a worker never stopped the main
  thread. Cross-thread fatal signals now take effect at send time instead of
  waiting in a queue the blocked target never drains.
- Threads created in a fork child return the child's pid from `getpid`
  (they returned their own tid, so `kill(getpid(), ...)` failed).

### /proc/self/fd reopen and openat2 (2026-10-06)

- `open("/proc/self/fd/N")` reopens the file behind guest descriptor N.
  The path used to reach the host procfs, which named the emulator's own fd
  N (a memfd reopen returned `ENXIO` from an emulator socket). Paths below a
  directory descriptor resolve inside the sandbox like a relative openat.
- `openat2` now translates guest open flags and honors `dirfd` like openat;
  it passed AArch64 flag bits straight to the x86_64 host.

### MADV_REMOVE (2026-10-06)

- `madvise(MADV_REMOVE)` now punches a hole in writable shared file mappings
  and returns `EINVAL`/`EACCES`/`ENOMEM` like Linux, instead of reporting
  success without doing anything. With userfaultfd, ART's CMC collector
  zeroes a released arena only when `MADV_REMOVE` fails with `EINVAL`.
  Recycled arenas kept stale strings that became DexCache type entries, and
  `app_process64` crashed on a string used as a class pointer.

### Activity probe and glibc main-stack bounds (2026-10-06)

- Add `guest/android/ActivityProbe.java` and the runner's `--main-class`.
  Re-provisioned default framework root: schema 4.
- glibc dynamic programs: `/proc/self/maps` no longer reports one ELF line
  from address 0 to the interpreter, and the native linker sets
  `__libc_stack_end`; `pthread_getattr_np` on the main thread works.

### Android Activity creation (2026-10-06)

- Leave guest page 0 unmapped in the Android profile so ART's implicit null
  checks raise `NullPointerException` instead of jumping to address 0.
- Report the linker's real extent in `/proc/self/maps` and omit an empty
  `[heap]`. Return `EINVAL` from `madvise` for wrapping ranges. Together these
  fix the layout-dependent early SIGILL in `app_process64`.
- Answer Binder `PING_TRANSACTION` in the host registry and accept
  `FLAG_COLLECT_NOTED_APP_OPS`. Add `BIFROST_BINDER_TRACE`.
- Framework provisioning schema 4 adds framework resources and fonts.
- A local probe creates a real Activity through `onCreate` in both engines.

### Android 15 framework initialization (2026-10-06)

- Fix BLR X30 target capture in interpreter/IR; ART nterp method calls now
  invoke the intended callee instead of treating a method pointer as its result.
- Preserve shared file aliases across low/high mappings, fork/remap and partial
  unmap/replacement, including file writeback and read-only protection checks.
- Add literal W/X/LDRSW and SIMD loads used by ART-generated code; reset their
  addressing flag when interpreter decode-cache entries are reused.
- Export mainline APEX namespaces and extract their native libraries (framework
  provisioning schema 3). Keep Binder arenas in available direct-window holes.
- Unwind native callers after guest exit_group and finish the main-Looper probe
  from its dispatched callback. The real framework probe passes with ART JIT on.
- Add focused regression coverage for these paths and `bc58e6a` instruction
  repairs; build real dlopen fixtures with glibc rather than musl's static stub.

### Validation of the current checkpoint

- Strict guest suite: 258/258 JIT; 255/255 interpreter with three JIT-only skips.
- Focused units: 81/81 JIT and 79/79 interpreter; differential gate: 56
  fixtures and five threaded oracles. All 17 additional verification groups pass.
- Doom 3 tier-1 gameplay remains scene-dependent; tier-2 profitability and the
  reported renderer fault remain unresolved. No controlled FPS claim is made.

### JIT block chaining no longer scans every block (2026-10-05)

- `chain_back_references` fell back to scanning all translated blocks
  whenever its back-reference index had no entry for a new block, which is
  the normal case for call and return targets. Translation cost grew with
  the square of the block count: ART's JIT bootstrap spent ~96% of its
  translate samples there. The index is complete, so the scan is gone; its
  size cap now rebuilds from unpatched slots instead of clearing. ART's JIT
  bootstrap drops from 84 s to ~25 s.

### Real ART VM startup: rt_sigtimedwait and futex requeue (2026-10-05)

- Implement `rt_sigtimedwait` (dequeue a pending member of the set, block
  until one arrives or the timeout expires, EINTR for other handlers). The
  EAGAIN stub made ART's signal catcher spin on `sigwait()`.
- `FUTEX_CMP_REQUEUE` takes its requeue count from the timeout slot, not
  `val3`; ART's first condition-variable broadcast never woke the main
  thread. Negative counts return EINVAL.
- The pinned ART bootstrap now reaches `ART_VM_OK` and passes its DEX/JNI
  fixture under the JIT and the interpreter. Add `ctest/test_sigtimedwait.c`
  and `ctest/test_futex_requeue.c` (QEMU-checked).

### OpenSSL 3 correctness: JIT register pressure and glibc main thread (2026-10-05)

- JIT: `ensure_two_vregs()` could evict its other operand under full
  register pressure, and its fallback passed `force_two_vregs_to()`
  arguments in the wrong order, clobbering a live host register. Every
  OpenSSL SHA-256 digest was wrong under the JIT. Add
  `ctest/jit_two_operand_pressure.c` (1,920 checks; QEMU-checked).
- Native dynamic linker: set the glibc main thread's `struct pthread` tid
  and list link, as `__tls_init_tp` does. Main-thread rwlocks and
  error-checking/recursive mutexes no longer fail with EDEADLK, so OpenSSL 3
  providers load. Read glibc's `.symtab` libthread_db descriptors for all
  offsets; NPTL stack-list heads now use glibc 2.42's real offsets instead
  of the 2.36 fallback. Add `ctest_real/test_dyn_main_locks.c`.
- Verified with OpenSSL 3.0.17 SHA-1/SHA-256/AES-128-GCM over 4 MiB against
  host Python, scalar/NEON/crypto paths, JIT and interpreter.

### AArch64 crypto and pairwise-widening SIMD (2026-10-05)

- Correct the PMULL (`0x0EE0E000`) encoding, add 8-bit PMULL/PMULL2, and
  narrow the XTN-family masks so SADDLP/UADDLP/SADALP/UADALP reach their new
  interpreter implementation. JIT PMULL uses MOVQ instead of SSE4.1
  PINSRQ/PEXTRQ.
- Fix SHA1C/P/M (no round constant, Sn read-only), SHA1SU0/SU1 and
  SHA256SU0/SU1 to match the architectural pseudocode (checked against QEMU).
- Add `ctest/jit_simd_pairlong.c` and `ctest/jit_simd_pmull.c`; replace the
  SHA-256 tests with QEMU-checked literal vectors.

### SIMD&FP register-offset Q scaling (2026-10-05)

- Scale the index of `ldr/str qN, [xn, xm, lsl #4]` by 16 in the interpreter
  and IR translator; both used the size field (0), so Q accesses landed at
  base + i. This corrupted ART's class-linker method records (`ptr << 32`
  SIGSEGV, `Unexpected state: 6656`). The pinned ART bootstrap now completes
  boot class linking but does not yet reach `ART_VM_OK`.
- Add `ctest/jit_simd_ldst_regoff.c` (9 checks; passes JIT, interpreter, QEMU).

### Window statistics defaults

- Show emulator version and presentation stats in game titles by default;
  retain title/all-stats disable switches and opt-in overlay/separate window.

### Multithreaded Tier-2 and chaining

- Default to tier-1 JIT with chaining; make tier-2 promotion experimental and
  opt-in via `BIFROST_TIER2=1`, retaining explicit promotion coverage in tests.

- Publish fall-through, taken and region-exit edges through atomic data cells;
  keep published MT instructions immutable and retain helper-based BL calls.
- Enable atomic hot-head promotion and whole-iteration MT regions, with
  bounded chains, cross-CPU cache revocation and constituent-range invalidation.
- Retain code/metadata until JIT destruction; reclamation remains deferred.
- Add eight-thread arithmetic/SMC/MAP_FIXED oracles and CI publication checks;
  preserve single-thread differential verification separately.

### glibc 2.39 dynamic-loader hook repair

- Detect the dlfcn hook through the relocated `_rtld_global_ro` GOT pointer,
  supporting glibc 2.39's +672 layout and existing +368/+376 layouts.
- Fix the null native-loader call exposed by the CI `dladdr` regression;
  validate ten dynamic-link tests with glibc 2.39 and 2.42 in both engines.
- Run the selected dynamic-link CI tests in interpreter mode as well as JIT.

### Toolchain download availability repair

- Use checksum-pinned Bootlin musl GCC in CI, with matching compiler/runtime
  paths and SDK relocation. Keep musl.cc as the default local source with a
  Bootlin fallback; preserve existing installed toolchains.
- Report actual download exit codes and test checksum/failure handling.

### CI guest-libc and proc-executable repair

- Return absolute guest `/proc/self/exe` paths, preserving the sandbox
  namespace and original argv/AT_EXECFN; correct readlink truncation, NUL and
  zero-buffer semantics. Add API/sandbox checks and a glibc-static CI repro.
- Provision musl for musl fixtures while retaining explicit glibc checks.

### Fresh-runner CI build repair

- Feature-gate the display proxy's SDL include and remove the build-time
  Wayland SDK dependency for dynamic protocol descriptors.
- Use portable overloaded `<cmath>` names in FP/audio code and add a CI
  header-independence check that catches SDK leaks on provisioned machines.

### Verification gate and screenshot follow-up

- Suspend unsafe differential replay after the multithread transition and
  report that limitation. Verify 56 single-thread fixtures plus four unchanged
  threaded oracle workloads; reject missing/stale fixtures, divergences and
  failed/timeout exits. Add CI tests for gate failure propagation.
- Fix headless audio math includes and Android SDL compile guards, and carry
  the fixture compiler override into CI host API checks.
- Add all eight October 4 user-supplied screenshots across Neverball, Minecraft
  Weekend, vkQuake and Doom 3, with captions that distinguish sampled FPS from
  sustained performance. Preserve the supplied game windows and title stats.

### Changes since the published August 18 checkpoint

- **JIT and IR:** Tier-2 region compilation, typed/private IR parameters,
  wider virtual-register IDs, W-form/bitfield and FP/SIMD correctness fixes,
  default register forwarding, stable bounded code-cache growth and larger
  per-thread dispatch caches. Shared multithreaded execution disables live
  code patching and Tier-2 publication; native SDL workers now use JIT with
  callback return sentinels.
- **Graphics and audio:** registry-generated GL/EGL/Vulkan coverage and deep
  argument staging, mapped-buffer upload/readback synchronization, Wayland
  display/input bridging, legacy GL/Doom compatibility and SDL surface formats.
  Audio work covers stream mixing, guest callback CPUs, sample formats,
  accepted-frame accounting, shutdown/fork handling and OSS playback.
- **Linux compatibility:** memory permissions, lazy reservations, mmap/remap
  contents and offsets, signal context restoration and alternate stacks,
  concurrent dynamic linking, glibc pthread/TLS layout and late TLS, syscall
  compatibility and rootfs symlink confinement. BIFROST_ROOT remains a VFS
  boundary rather than an independent host security boundary.
- **Applications and diagnostics:** Neverball font/JPEG/fullscreen repairs,
  zstd shift regressions, vkQuake pointer/upload fixes and user-reported mouse
  resolution, Doom 3 rendering/audio/animation bring-up, per-worker profiling,
  fallback inventories and optional window statistics. Detailed game docs
  retain their remaining correctness and performance limits.
- **Android:** native surface/lifecycle/input/audio adapters and native-bridge
  API probes exist. Bionic/package loading, integrated ART/JNI and selected
  Android game support remain v2.0 targets, not arbitrary-APK compatibility.
- **Validation and publication:** source-built guest fixtures, strict/stale
  fixture checks, CI configurations, independent QEMU oracles, reproducible
  benchmark metadata, current support/security docs and third-party notices.
  Current ordinary suites pass 257/257 JIT and 254/254 interpreter; concurrent
  differential failures remain unresolved. See [TESTS.md](../TESTS.md).

The full commit list below includes superseded experiments and reverts;
its subjects are history, not claims that every intermediate feature remains
active. The sections that follow record focused checkpoints and measurements.

### Dispatch, helper overhead and regression fixtures (2026-10-04)

- Expand per-thread dispatch lookup to 1,024 two-way sets, retain entries
  across data-only mapping changes, and use shared-lock call-target hits in
  multithreaded mode. Keep multithreaded chaining/Tier-2 publication disabled.
- Register profiler code ranges per host thread/JIT owner, covering cache
  growth; add optional batched dispatch-cache telemetry.
- Batch nested guest-call statistics and remove redundant BL/BLR host flag
  saves and BL return-PC writes. Targeted dispatch and short-call benchmarks
  improve 2.32× and 1.93×; these are not whole-game FPS measurements.
- Repair assembly fixtures' writable sections and REPL capacity, correct the
  histogram fixture's halfword input, and load executable ELF call probes in
  the host C API test instead of writing code onto a non-executable stack.
- Full suites pass 257/257 JIT and 254/254 interpreter, with three JIT-only
  skips. After the last helper cleanup, 80/80 JIT units, SDL nonlocal-return
  checks and the 20,000-case native Doom harness pass. Concurrent differential
  warnings/timeouts remain documented in [TESTS.md](../TESTS.md).
- Refresh the README's Android and game compatibility scope; record the
  user-reported vkQuake mouse resolution without assigning an unverified cause.
  Document [fixture licenses and distribution](../THIRD_PARTY.md), preserving
  Toybox's upstream license alongside its committed binary.

### Native Doom JIT and v2.0 roadmap (2026-10-04)

- Close all 625 explicit IR fallback encodings captured in the Doom run.
  Add inline conditional FP compares/Q=0 logical operations, native atomic
  monitor paths and specialized SIMD/structured-memory helpers. Correct
  shared interpreter FCVTL, SHLL, ADDV width and indexed fused FMLA semantics.
- Run SDL guest threads with JIT, with per-CPU return sentinels and safe
  nested call unwinding. Retain interpreter mode and add nonlocal-return
  regressions. Keep structured-memory fault exits out of Tier-2 regions.
- Add complete per-thread interpreted-opcode inventories and graphics
  presentation statistics: title, optional stats window/desktop overlay,
  and default first-render logging. See [window stats](../window-stats.md)
  for platform and presentation-counter limits.
- Validation: full JIT 255/255; interpreter 252/252 with three JIT-only skips;
  20,000 captured-opcode randomized checks; 2,054 QEMU-checked oracle cases;
  separate SDL lifecycle, fallback-profiler and window-stats checks pass.
  Document approximately 27 minutes of user-reported crash-free gameplay,
  while retaining frame-pacing, save-loading and profiler-attribution limits.
- Add the planned [v2.0 roadmap](../roadmap.md) for Bionic/Android platform
  compatibility, ART/JNI, Android games, safer cache reuse, sustained Doom 3
  performance and broader input support. These goals are not shipped features.

### Game compatibility and TLS follow-up (2026-10-03)

- Interpreter: require both PC and the fetched instruction word for decode-cache
  hits, preventing stale execution after another thread rewrites code. Improve
  the cross-thread regression diagnostics; full interpreter suite 249/249
  (three JIT-only skips), JIT suite 252/252.
- Gameplay follow-up: Neverball and Minecraft Weekend remain user-confirmed
  working; Doom 3 works well after the animation math repair. vkQuake still
  has texture corruption and camera issues. See the game-specific notes.
- Neverball: user-confirmed stable gameplay, replay, menus, configuration,
  JPEG previews, and fullscreen/windowed mouse control. Corrected SIMD
  narrowing semantics and routed affected JIT instructions through the
  interpreter; 672 focused checks and four JPEG reference decodes verify
  the repair. See [Neverball notes](../Apps/neverball.md).
- vkQuake: removed an interpreter FCSEL/conversion encoding collision that
  corrupted XZR and Vulkan pointers. Default JIT gameplay and menus work
  in the user's observed session, with no sudden crashes reported. Camera
  behavior and texture corruption remain unresolved. Added 61 regression
  checks and cropped screenshot evidence in [vkQuake notes](../Apps/vkquake.md).
- vkQuake follow-up: preserve pending CPU uploads when merging Vulkan mapped
  memory readback. Texture colors improved, with remaining artifacts reported.
  Implement SDL event filters with nested callback scratch stacks and isolated
  host-thread guest state; correct event/output bounce sizes and above-window
  callback-stack guards. Add opt-in input diagnostics.
- SIMD: fix SSHL/USHL signed low-byte register counts and negative USHL right
  shifts in the shared interpreter fallback; add 182 regression checks. The
  user still reports excessive vkQuake sensitivity after these repairs.
- Documentation: remove old planning, audit, and session-history documents;
  keep current game status and operational references.
- Dynamic linking: late TLS supports reserved static slots and per-thread
  dynamic DTV allocations, including TLSDESC, template relocation, alignment,
  pthread initialization/reuse, and clean rejection of unsupported
  initial-exec placement. Added isolated late-TLS regression scripts.
- Graphics thunks: correct floating-point SDL math returns, guest keyboard
  snapshots, SDL_RenderCopyEx argument marshalling, integer/long vertex
  attribute pointers, and nested Vulkan pNext data staging. Added ABI probes
  and repeated SDL lifecycle checks.
- Tests: reject stale guest fixtures instead of silently running outdated
  binaries; add JPEG narrowing and high-register FCSEL regressions.

### Additional correctness and game checkpoints (September 24–October 3)

- September 24: add isolated build profiles, strict fixture setup, generated
  checks and CI; repair IR vreg-ID truncation, JIT bitfield/FP fusion and GL
  argument handling. Add checksum-bearing benchmark tooling.
- September 29: enforce guest memory permissions and executable fetch checks,
  restore signal-handler edits to architectural context, correct display
  handle translation, and strengthen QEMU-backed memory/signal probes. Fix
  anonymous mapping contents, file offsets, remap flag handling and active
  alternate-signal-stack behavior.
- October 1: repair Neverball's conditional-compare register forwarding,
  fullscreen dimensions and guarded addresses; fix zstd variable-shift source
  preservation and W-form shifted operands with independent output checks.
- October 3: bring up dhewm3 through dynamic-loader, SDL surface/thread and
  legacy GL repairs, including `glMap1d`; audit its mandatory GL imports.
  Correct the 32-bit vector multiply encoding, OSS sample negotiation and
  playback. Repair FMOV immediates, scalar lane copies and vector square roots
  that affected animation. Bound JIT cache exhaustion without recycling code
  referenced by running threads.

### Commit history included in this update

Remote `main` was checked on October 4, 2026 at
[`fe90d98`](https://github.com/sloppyman2567/bifrost-emu/commit/fe90d98148a96a5c51f9a5da1bde26b408cb6998).
The following 211 reachable commits cover `fe90d98..aa5dcc7`, including merge
history. This list excludes the documentation edits being prepared here.

<details>
<summary>All 211 commits, newest first</summary>

- [aa5dcc7](https://github.com/sloppyman2567/bifrost-emu/commit/aa5dcc7a201e7d2381d761e06b82cc60fea743ed) — 2026-10-04: Optimize JIT dispatch and guest-call helpers; repair regression fixtures
- [49ad198](https://github.com/sloppyman2567/bifrost-emu/commit/49ad1986c40b437a20d3a5c90b03e1509373e638) — 2026-10-04: Update test inventory and document current validation and Android scope
- [d56a2f4](https://github.com/sloppyman2567/bifrost-emu/commit/d56a2f4db4f4b803fb6839ddc128d7a05877d98e) — 2026-10-04: Expand native JIT coverage, enable SDL thread JIT and plan v2.0
- [cd3db0b](https://github.com/sloppyman2567/bifrost-emu/commit/cd3db0bfa51c339e4d74b66d493c027b6be5a618) — 2026-10-03: Fix interpreter decode-cache coherence and update game validation
- [581b5f8](https://github.com/sloppyman2567/bifrost-emu/commit/581b5f8017b20e4e7747d03bc9a9f7b4451b0348) — 2026-10-03: Fix Doom 3 animation math and shared JIT cache exhaustion
- [39973d2](https://github.com/sloppyman2567/bifrost-emu/commit/39973d220c68d4e969e7b495c0d7e24814469ac2) — 2026-10-03: Fix Doom 3 GL compatibility, OSS playback and SIMD multiply
- [834a378](https://github.com/sloppyman2567/bifrost-emu/commit/834a3783813780246df0fa99b1cf80ad8b3fb0bf) — 2026-10-03: Improve Doom 3 startup compatibility and add glMap1d thunk
- [115dbfb](https://github.com/sloppyman2567/bifrost-emu/commit/115dbfb5ca8f7c884f0b0132cc64bc87cadaf780) — 2026-10-03: Fix Vulkan upload synchronization and SDL filters; refresh game docs
- [fd3ac84](https://github.com/sloppyman2567/bifrost-emu/commit/fd3ac84fadfa7a4bd9b49ce35bff7095eb150fe7) — 2026-10-02: Fix game compatibility, late TLS, and vkQuake pointer corruption
- [8ee379b](https://github.com/sloppyman2567/bifrost-emu/commit/8ee379bac2af5db2bfb510a4a1840cc5e987431e) — 2026-10-01: Fix Neverball fullscreen dimensions and guard address preservation
- [0046d62](https://github.com/sloppyman2567/bifrost-emu/commit/0046d62be7b1260c866ea3205b3bbaf27a787ee5) — 2026-10-01: Fix zstd shift corruption and add regression coverage
- [3e835d6](https://github.com/sloppyman2567/bifrost-emu/commit/3e835d64763e95863f0f2e2e0a5ea191d07ca4c3) — 2026-10-01: Fix Neverball font rendering and document regressions
- [5d4e2b7](https://github.com/sloppyman2567/bifrost-emu/commit/5d4e2b77265d5c9540de386f4782b9702a35ceaf) — 2026-09-29: Fix mmap contents, file offsets, remap flags, and active signal stacks
- [18f0355](https://github.com/sloppyman2567/bifrost-emu/commit/18f035580dac457fb56a66b559b3ef2e03216586) — 2026-09-29: Strengthen memory and signal regressions and fix QEMU reference masks
- [5bf60b6](https://github.com/sloppyman2567/bifrost-emu/commit/5bf60b6f6656c83936fb138cf9047ec0306f0a13) — 2026-09-29: Fix guest memory permissions, signal return, and display handles
- [69fa2a9](https://github.com/sloppyman2567/bifrost-emu/commit/69fa2a95ef3500efac2eb02a1f593aa02f661768) — 2026-09-24: Fix JIT bitfields, FP fusion, and GL thunks
- [86c3652](https://github.com/sloppyman2567/bifrost-emu/commit/86c3652b28837705304b23e092c571b96a822e32) — 2026-09-24: Fix IR vreg ID truncation
- [ce65796](https://github.com/sloppyman2567/bifrost-emu/commit/ce65796fa2d43004952e2d0af2d0800c1ddf4df6) — 2026-09-24: Improve emulator reliability and production readiness
- [07e37ef](https://github.com/sloppyman2567/bifrost-emu/commit/07e37efdb76f7dd9852b8b339b14309496ae6f52) — 2026-09-22: Refresh README and test documentation
- [8372fbe](https://github.com/sloppyman2567/bifrost-emu/commit/8372fbe04e4d4a40313c39f13952aabc6f643680) — 2026-09-22: Document SDL input refinements
- [2c556c9](https://github.com/sloppyman2567/bifrost-emu/commit/2c556c9a8ead25e39b1bea63d6222dae9f6449c7) — 2026-09-22: Refine host input event handling
- [8edb86c](https://github.com/sloppyman2567/bifrost-emu/commit/8edb86cc95f08ee51accd65e50e63435826567cf) — 2026-09-22: Fix unsafe Tier-2 register hoisting
- [a225a2b](https://github.com/sloppyman2567/bifrost-emu/commit/a225a2be7c833dd2d0d9c8d7d182c1dae82d1038) — 2026-09-22: Harden audio callback worker and fork handling
- [9f22fd8](https://github.com/sloppyman2567/bifrost-emu/commit/9f22fd816ca72ee34edee111c5167c59311c2db6) — 2026-09-22: Add SIMD PNG regression coverage
- [9be7154](https://github.com/sloppyman2567/bifrost-emu/commit/9be71544e51c9ec3e57970e17d3fc89baab1717d) — 2026-09-22: Fix SIMD decode correctness
- [3869958](https://github.com/sloppyman2567/bifrost-emu/commit/386995851759e5f673d645b0c4ace2d24369dfea) — 2026-09-22: Sandbox hardening: symlink confinement via openat2 RESOLVE_IN_ROOT
- [1fd3948](https://github.com/sloppyman2567/bifrost-emu/commit/1fd3948c0e1a1718cf96fe776a71678f66d84bd9) — 2026-09-22: docs: session history entries 73-74 (1.5.5 cut, commit split)
- [52b6137](https://github.com/sloppyman2567/bifrost-emu/commit/52b61379d1aa538e098e94bd7f2a1d358d953f4d) — 2026-09-22: Migrate IR to typed per-op params + privatize raw fields (1.5.5)
- [6567151](https://github.com/sloppyman2567/bifrost-emu/commit/6567151719ddeb62f2f782a2b5df01e6260ab4ec) — 2026-09-22: Collect pre-existing working-tree changes (predate typed-IR migration)
- [6305b00](https://github.com/sloppyman2567/bifrost-emu/commit/6305b00e9df8d5672079a7249d7e0fa8c05aac37) — 2026-09-12: chore(debug): remove app-specific TEMP traces
- [ff65646](https://github.com/sloppyman2567/bifrost-emu/commit/ff656469abfff406e300d13a89d941d5cc4363e1) — 2026-09-12: fix(vfs,input): BIFROST_ROOT traversal, /proc-/dev prefix, FIONREAD null
- [86a35f7](https://github.com/sloppyman2567/bifrost-emu/commit/86a35f73cd4d0096b27ad57ea4c83fd28469234e) — 2026-09-12: chore(config): remove dead \[thunk\] knobs
- [3b13c0d](https://github.com/sloppyman2567/bifrost-emu/commit/3b13c0d56cf202ef326d171b84173a7f755b0a71) — 2026-09-12: feat(thunk): auto-generated SDL render-family rows
- [dfe5edb](https://github.com/sloppyman2567/bifrost-emu/commit/dfe5edb5d2bdcf450e694392ac6e528767cb5fe5) — 2026-09-12: fix(jit): shared-JIT multithread safety (no code writes under a running core)
- [46ae56b](https://github.com/sloppyman2567/bifrost-emu/commit/46ae56b59b4fa21434bbdf438115bed62a2be44a) — 2026-09-12: chore(version): stamp generated/sample config with the canonical VERSION
- [4b7b8a2](https://github.com/sloppyman2567/bifrost-emu/commit/4b7b8a2ba820da5e51ffb93b1e9eaf71a61403e8) — 2026-09-12: docs: sync test counts and add PRFM/LDAPR + syscall contracts
- [cd4d2cf](https://github.com/sloppyman2567/bifrost-emu/commit/cd4d2cfcba7816c5aaa2ced3de50736d34ffa522) — 2026-09-12: docs: session history for the hardening sweep and decode/syscall work
- [a910330](https://github.com/sloppyman2567/bifrost-emu/commit/a910330fd2fa64d5410b2b3f4ebd0b79d62586e4) — 2026-09-12: test: register the new regression suites in run_tests.sh
- [45144ee](https://github.com/sloppyman2567/bifrost-emu/commit/45144eec9fa4b3212781aad71fa3d88946e2d47a) — 2026-09-12: feat(syscalls): compatibility stubs + fanotify number fix
- [b95998a](https://github.com/sloppyman2567/bifrost-emu/commit/b95998a4db8863c5ed27a3d48bc0f0801a7f5252) — 2026-09-12: fix(decode): PRFM is a NOP; LDAPR/LDAPRB/LDAPRH are acquire loads
- [78a78ed](https://github.com/sloppyman2567/bifrost-emu/commit/78a78eda3ea8627b6bcd4b762858d24077c51b44) — 2026-09-12: fix(core,signal,threads,mem): correctness + lifetime hardening sweep
- [31b1320](https://github.com/sloppyman2567/bifrost-emu/commit/31b13209926443d2b7e24f745815f41c9cb0b1e8) — 2026-09-11: docs: tighten the 2026-09-11 session entry (TLS fix fixed, neverball not)
- [b847e0d](https://github.com/sloppyman2567/bifrost-emu/commit/b847e0dcd63cdf39d41ffa9052a51b48349b58ea) — 2026-09-11: fix(tls): reserve glibc struct pthread below TPIDR_EL0
- [d1b6edd](https://github.com/sloppyman2567/bifrost-emu/commit/d1b6edd4784ca2bbce4f7e64a64b9651f2e3a3bd) — 2026-09-11: debug(mem, jit, thunk): store/watch tracing, guard pages, SMC invalidation, SDL detach
- [997e231](https://github.com/sloppyman2567/bifrost-emu/commit/997e23124ce4356eb7fa4bf19a5db230936d36a8) — 2026-09-05: debug(mem): BIFROST_WATCH store tracer with interp pc
- [d45e116](https://github.com/sloppyman2567/bifrost-emu/commit/d45e116873d728257b9e7a706b9c3c944004c6ef) — 2026-09-05: Reapply "fix(mem): atomic MAP_FIXED replace via mmap_fixed_replace"
- [5aaebb6](https://github.com/sloppyman2567/bifrost-emu/commit/5aaebb64432c5ed966bb0f62ea3c7dcd63657d8b) — 2026-09-05: Revert "fix(mem): atomic MAP_FIXED replace via mmap_fixed_replace"
- [777cf0e](https://github.com/sloppyman2567/bifrost-emu/commit/777cf0ee6c5529f7fc89d2bfdf4a9b6ddffe96f4) — 2026-09-05: fix(mem): atomic MAP_FIXED replace via mmap_fixed_replace
- [4a105bd](https://github.com/sloppyman2567/bifrost-emu/commit/4a105bd1c579cd706bb371990d4c8d3896f903e0) — 2026-09-04: fix(build): track api + host-test header deps; sync version to 1.5.5-alpha
- [a4a1881](https://github.com/sloppyman2567/bifrost-emu/commit/a4a188191512782e1364fa0e0eb1c9ece9cbd0fd) — 2026-09-04: fix(syscalls, mem, thunk): timedwait ETIMEDOUT, guard pages, bounce alias, vfs order
- [d85ddf3](https://github.com/sloppyman2567/bifrost-emu/commit/d85ddf32d0d3db2cf223b89dbc3cb4167172ee7c) — 2026-09-04: docs: final full seal from clean tree (221/221, capi 54/54, nb 61/61)
- [d5284dd](https://github.com/sloppyman2567/bifrost-emu/commit/d5284dd5cbaa8e663b1fb2f5c7250ae76285fde2) — 2026-09-04: fix(review): vregkey widening, sigign preserve + comment accuracy sweep
- [6702412](https://github.com/sloppyman2567/bifrost-emu/commit/67024121f10f78f7ac2b9354cd5ba054fddeb7f3) — 2026-09-04: fix(review): tls size branch, 0x1001 anchor, shim lock, fmaxp nan
- [35d7117](https://github.com/sloppyman2567/bifrost-emu/commit/35d7117c2384a1b03883e25675d3e5ea66917834) — 2026-09-04: fix(tls): unify tcb base to 0x10 per upstream tcbhead_t
- [7cdbf79](https://github.com/sloppyman2567/bifrost-emu/commit/7cdbf7987e0d37c1b0c905d31529529079979154) — 2026-09-04: fix(execve): kill other threads, reset mmap cursors
- [0b21da7](https://github.com/sloppyman2567/bifrost-emu/commit/0b21da712e52e4a2e60e7c8ddd2debc715397401) — 2026-09-04: fix(signals, vfs): sigsuspend rt range, frame tp, pread/pwrite, dup2 guard
- [724e85a](https://github.com/sloppyman2567/bifrost-emu/commit/724e85a61923d4229356361e34c723a35d04378b) — 2026-09-04: fix(interp): scalar integer addp Dd,Vn.2d
- [dcc38dc](https://github.com/sloppyman2567/bifrost-emu/commit/dcc38dcc04b1dbc4a7392b7078c7cdc3f53b76ce) — 2026-09-04: fix(dynlink): dlopen tls surplus, static shim, runpath threading
- [d1077a1](https://github.com/sloppyman2567/bifrost-emu/commit/d1077a12d081aae209804655424f968734e49a78) — 2026-09-04: docs: check-all green note + stale-build lesson (test-capi false alarm)
- [772a308](https://github.com/sloppyman2567/bifrost-emu/commit/772a30810d66e5c9019558275ff977e35de9dbda) — 2026-09-03: fix(mremap): reclaim shrink tail, charge in-place grow pages
- [3cedeba](https://github.com/sloppyman2567/bifrost-emu/commit/3cedebaee4f76890089720d7c6116b836add865c) — 2026-09-03: fix(frinta, execve, dynlink): ties-away round, process reset, jmprel parity
- [5152ae8](https://github.com/sloppyman2567/bifrost-emu/commit/5152ae897ab951807ed28eb9e8cbe6da19810533) — 2026-09-03: chore(hygiene): drop dead simd handlers, cache trace flags
- [d4b32a6](https://github.com/sloppyman2567/bifrost-emu/commit/d4b32a6c1d860fef4900362f3cf6d5644e0b10f0) — 2026-09-03: fix(signals, dynlink, futex): one-at-a-time drain, unique thunk handles, locked wake
- [846f445](https://github.com/sloppyman2567/bifrost-emu/commit/846f44523cc110c1af001977e34054a48033dea6) — 2026-09-03: fix(dynlink): main range via map_size, not 16m cutoff
- [6d6faaa](https://github.com/sloppyman2567/bifrost-emu/commit/6d6faaac609d48bb1b0034552faf0ab8efca0270) — 2026-09-03: fix(dynlink): thunk fallback for abs-path dlopen
- [468eeb2](https://github.com/sloppyman2567/bifrost-emu/commit/468eeb2cab806829973a38929abff4056288d3eb) — 2026-09-03: fix(simd): vector fmax/fmin propagate nan, nm keeps number
- [de9dd48](https://github.com/sloppyman2567/bifrost-emu/commit/de9dd484cb87f97c2291bbf633be7d3f2be2a298) — 2026-09-03: fix(mremap): return ENOMEM on move-path oom, keep old mapping
- [81dc343](https://github.com/sloppyman2567/bifrost-emu/commit/81dc343d220bc018a52f1496ab05e482c49e33fc) — 2026-09-03: fix(interp): fixed-point fp-dest unsigned via saturation helper
- [d37ebfa](https://github.com/sloppyman2567/bifrost-emu/commit/d37ebfa9b08f56c807e6332045a3926057c18d80) — 2026-09-03: fix(fp): fmax/fmin propagate nan, nm keeps number; jit falls back
- [baab13c](https://github.com/sloppyman2567/bifrost-emu/commit/baab13ced13f505528f87359b2bad279f3ea7776) — 2026-09-03: fix(memory): pagecache epoch, atomic split+align, fork cursors
- [60b7d54](https://github.com/sloppyman2567/bifrost-emu/commit/60b7d549e189cf44ec774c7f5440137e9e7467fc) — 2026-09-03: fix(review): VAO element refresh, sse41 gate, reset(), drop vkq debug
- [80e2d4b](https://github.com/sloppyman2567/bifrost-emu/commit/80e2d4b69a7a3760e655712d6570d9ce9581f9f8) — 2026-09-03: perf(thunk): phase-2 VAO-modeled buffer elision + pixel regression test
- [0a160f7](https://github.com/sloppyman2567/bifrost-emu/commit/0a160f753f4a980a66639f98364aad9624729229) — 2026-09-03: docs: elision A/B numbers on free machine
- [3e19674](https://github.com/sloppyman2567/bifrost-emu/commit/3e19674085e3c3b9fea5ea5eb15b103126a96cb3) — 2026-09-03: perf(thunk): elide redundant binds (UseProgram/ActiveTexture/Texture/VAO)
- [0ce02d5](https://github.com/sloppyman2567/bifrost-emu/commit/0ce02d5bd27eba2e71a0c853183643e30e8d4a2c) — 2026-09-03: perf(thunk): skip writeback snapshot when nothing bounced
- [83046bb](https://github.com/sloppyman2567/bifrost-emu/commit/83046bbfe302686638ee2bc3abb9e4280d8e3364) — 2026-09-03: fix(tests, thunk): un-skip vulkan_pnext + clamp display writebacks to safe room
- [0577ca7](https://github.com/sloppyman2567/bifrost-emu/commit/0577ca718e6bf397f1b59216c3ac5060b3ec2bb3) — 2026-09-03: feat(jit): FWD load-forwarding on by default (~10%), fix SMOV cache bug
- [765ab89](https://github.com/sloppyman2567/bifrost-emu/commit/765ab890982cba9564fbb402b46020687aff93b8) — 2026-09-03: fix(emulator): vk display writeback clamp + signed fcvt saturation (allocblock hunt)
- [fcc3a93](https://github.com/sloppyman2567/bifrost-emu/commit/fcc3a93b5308af8e34dea4304f416e41471f27cc) — 2026-09-03: feat(vulkan): corrupt-bind diagnostics, trap mode, handle registry (audit-only)
- [3dffd94](https://github.com/sloppyman2567/bifrost-emu/commit/3dffd9449d36d7a06fe152aae56c1f40f4375f05) — 2026-09-03: fix(vulkan): empty-bind guard for misaligned handle arrays + deep-path diagnostics
- [249331a](https://github.com/sloppyman2567/bifrost-emu/commit/249331a2a8e145c1719df5947ab9da7d851facb4) — 2026-09-03: fix(vulkan): keep 64-bit handle in mixed int+float thunk calls
- [25b8610](https://github.com/sloppyman2567/bifrost-emu/commit/25b8610ed826fb6b5bf4c0e0e5791046c1bf0335) — 2026-09-03: fix(vulkan/input): present fail-closed + pnext staging, live EVIOCGKEY
- [73e9467](https://github.com/sloppyman2567/bifrost-emu/commit/73e9467dc334f025612a63c7dd7a4597a0dbc57e) — 2026-09-03: fix(input): positional keycodes, evdev ioctl len-mask, queue fixes
- [623da32](https://github.com/sloppyman2567/bifrost-emu/commit/623da32c7d34a84d1e37e03aa2e0fa2f8f95e676) — 2026-09-03: fix(audio-thunk): drain ring on ALSA close so tails play out
- [5c0e394](https://github.com/sloppyman2567/bifrost-emu/commit/5c0e394f152d9d7ba54653e60db923f59824540f) — 2026-09-03: fix(vulkan): cached unmap fn, range-wrap guard, free-before-unmap order
- [ce3cc8a](https://github.com/sloppyman2567/bifrost-emu/commit/ce3cc8a32e60d5b21ea713746f93b1aedf16b899) — 2026-09-03: fix(audio-thunk): blocking ALSA/Pulse writes wait out full ring
- [64225a8](https://github.com/sloppyman2567/bifrost-emu/commit/64225a8ce177a83ae0c8bb0fa014ac7b48e54ab2) — 2026-09-03: fix(audio-thunk): honest short-write returns, undef R macro
- [2489b5b](https://github.com/sloppyman2567/bifrost-emu/commit/2489b5bcdb2f08b7db174659c3638ec70ec74166) — 2026-09-03: fix(audio): report accepted frames on partial ring writes, not requested
- [3e1d494](https://github.com/sloppyman2567/bifrost-emu/commit/3e1d494428136703c0f9db5ba0619b793b113b5b) — 2026-09-03: docs: rudolf-cart 2x-pitch diagnosis (chain exonerated)
- [9c4ee8e](https://github.com/sloppyman2567/bifrost-emu/commit/9c4ee8ed43808040981d17dd43981c4cfa022569) — 2026-09-03: fix(audio): real S32 playback, unify sample-size accounting
- [7947006](https://github.com/sloppyman2567/bifrost-emu/commit/794700651122de94b2674dc464c4520e527eaf0d) — 2026-09-03: fix(audio-thunk): lock dispatch maps vs pump thread and guest threads
- [af0b28a](https://github.com/sloppyman2567/bifrost-emu/commit/af0b28a01728bc0f9da54d122a215f7d9c9a700a) — 2026-09-03: fix(audio-thunk): stale pump audio, legacy pause reg, close/free leaks
- [f45ee45](https://github.com/sloppyman2567/bifrost-emu/commit/f45ee452e4180d77baa4e62b7235697dd0dc3186) — 2026-09-03: docs: true timestamps on older session entries
- [28fdcdd](https://github.com/sloppyman2567/bifrost-emu/commit/28fdcdde8ab1765a4bfa7e6018aa660275859caf) — 2026-09-03: docs: timestamps on today's session entries
- [2d40807](https://github.com/sloppyman2567/bifrost-emu/commit/2d40807e9c85a31175b1d83c4206e7fce9e33c3a) — 2026-09-03: docs: plan-GO completion entry
- [c628d7a](https://github.com/sloppyman2567/bifrost-emu/commit/c628d7a2c86406af45c84d2f478c1c1e86404118) — 2026-09-03: fix(opgen): hand-tuned rows live outside glcoverage blocks
- [8f505c7](https://github.com/sloppyman2567/bifrost-emu/commit/8f505c7dba6b7056f97ab37fffe4ea1a75899a34) — 2026-09-03: docs: draw-row + race-hardening session entry
- [1bb8a60](https://github.com/sloppyman2567/bifrost-emu/commit/1bb8a602e6cfe48268f3aa7f023a1db9a652933a) — 2026-09-03: feat(thunk): draw-row EBO policies, dispatch race hardening
- [5f755d2](https://github.com/sloppyman2567/bifrost-emu/commit/5f755d2fa764f96c7d6ea9abcd95b4945067c51f) — 2026-09-03: feat(thunk): EL_PTR for EBO draw rows, EL_PTR_ARRAY + INDIRECT_PTR policies
- [2c20be8](https://github.com/sloppyman2567/bifrost-emu/commit/2c20be8637347dd0432d92f946099dd66d73b004) — 2026-09-03: docs: expand wayland + thunk-review history into full entries
- [6e57400](https://github.com/sloppyman2567/bifrost-emu/commit/6e5740015f6fb7116f3dd4c97ea16ad6e2c4f729) — 2026-09-03: fix(gfx): thunk review round - display-mode src, glbitmap arm, rw null, calloc overflow, input mods, state keys
- [405a6eb](https://github.com/sloppyman2567/bifrost-emu/commit/405a6ebd1538d715bcc97251c0ae88b87a7da542) — 2026-09-03: fix(wl): guest fd for display socket, no phantom sdl, x11 input queue, flush on commit/ack
- [dea8bf6](https://github.com/sloppyman2567/bifrost-emu/commit/dea8bf6f563f357b5ef2c43021ab1e9ea055f613) — 2026-09-03: docs: dlopen_mt + wayland input session entries
- [511a34a](https://github.com/sloppyman2567/bifrost-emu/commit/511a34adf01d141f95ef103e1790e826e7f0d424) — 2026-09-03: feat(wl): seat input, fd-passing, get_class, surface commit
- [14a9575](https://github.com/sloppyman2567/bifrost-emu/commit/14a957588326b5b3e70ba8c3642c3476be2a956c) — 2026-09-03: fix(dynlink): thread-safety for concurrent dlopen/dladdr/iterate_phdr
- [71fdab6](https://github.com/sloppyman2567/bifrost-emu/commit/71fdab615bdf0bfc45caade9fc77859a87bbeeda) — 2026-09-03: feat(wl): registry dialog over the host compositor
- [64e7187](https://github.com/sloppyman2567/bifrost-emu/commit/64e718737662a393c2a94f428bdfb1c008da0b24) — 2026-09-03: feat(wl): real compositor bridge for display lifecycle
- [aebb5dc](https://github.com/sloppyman2567/bifrost-emu/commit/aebb5dcf54c81983b09d3e3397d88ca165bb1666) — 2026-09-03: fix(jit): flush dirty GPRs on taken-path exits
- [649effe](https://github.com/sloppyman2567/bifrost-emu/commit/649effeaaa49352b83270d4ae376c1c5526e69fc) — 2026-09-03: feat(wl): wayland.xml signature tables + proxy coverage automation
- [fcc1988](https://github.com/sloppyman2567/bifrost-emu/commit/fcc1988e8a00fd31c15bbd0cc818e09685159af9) — 2026-09-03: fix(thunk): widen dispatch arg ceiling 12 -&gt; 16
- [aa83a6a](https://github.com/sloppyman2567/bifrost-emu/commit/aa83a6a587c6462064b72136ad1939e3d46d2104) — 2026-09-03: feat(thunk): intercept glDebugMessageCallback (GL_DEBUG_CB)
- [b73aabb](https://github.com/sloppyman2567/bifrost-emu/commit/b73aabb37cef9322f8638b535c62f5001da27a03) — 2026-09-02: feat(scripts): rootfs DT_NEEDED completeness check
- [5133c0d](https://github.com/sloppyman2567/bifrost-emu/commit/5133c0d468cc1592ab6bf0be88a40a5c3a1da7a6) — 2026-09-02: fix(dynlink): search rootfs multiarch triplet dirs
- [2166c5c](https://github.com/sloppyman2567/bifrost-emu/commit/2166c5cde46abe84c8550d20f5aa42189e9bde94) — 2026-09-02: fix(jit): zero v_lo high dword on single-precision fp stores
- [310e86b](https://github.com/sloppyman2567/bifrost-emu/commit/310e86b0283098b6202b83d4a69a748d7e7a9918) — 2026-09-02: fix(jit): quarantine unsound blocks from verify re-run
- [9f6940a](https://github.com/sloppyman2567/bifrost-emu/commit/9f6940abd21da0a9fa5d5e576e9134abcdd2d435) — 2026-09-02: fix(jit): verify snapshot holds full 16 bytes for wide stores
- [1f6f1b0](https://github.com/sloppyman2567/bifrost-emu/commit/1f6f1b07dd8cd599af55077086ef1287c062e5b2) — 2026-09-02: docs: slim AGENTS.md to live contracts, move history out
- [515a805](https://github.com/sloppyman2567/bifrost-emu/commit/515a80559e9d24e33ee2fef4bb0f5eeae9f76427) — 2026-09-02: feat(core): module names and last syscall on crash lines
- [8126b7a](https://github.com/sloppyman2567/bifrost-emu/commit/8126b7aab7ee9836330865fc0a73670d5eaf600f) — 2026-09-02: feat(core): shared guest-crash reporter with cached flags
- [5c33194](https://github.com/sloppyman2567/bifrost-emu/commit/5c331942ca94b715523d4cd55051511ab957153c) — 2026-08-26: fix(thunks): clamp GraphicThunk bounce writebacks to live allocations; guarded scratch stacks; drop signal.cpp TEMP probes
- [e85451d](https://github.com/sloppyman2567/bifrost-emu/commit/e85451dc9b8e8fd7ffb45fe04a09974cbcff91c3) — 2026-08-26: fix(mem): MAP_NORESERVE/PROT_NONE mmaps are virtual reservations — lazy pages, no page-cap charge
- [52077b8](https://github.com/sloppyman2567/bifrost-emu/commit/52077b82e541ef83bc11fa164ddec45a936f7d1c) — 2026-08-26: fix(interp): FCVTN/FCVTXN matcher missed Rn &gt;= v8 — mask kept Rn bits \[9:8\]
- [fd7f352](https://github.com/sloppyman2567/bifrost-emu/commit/fd7f35253f6932a0761cd934d5c21a3d338990dd) — 2026-08-26: docs: session history 2026-08-25/26 — neverball demon root cause, width-bug audits, hunt tooling
- [83c0ed4](https://github.com/sloppyman2567/bifrost-emu/commit/83c0ed47f77527adc924d5d51e8f67cd65a80159) — 2026-08-26: fix(gl): normalize glMapBuffer access enum before the MAP_WRITE_BIT test
- [232e0eb](https://github.com/sloppyman2567/bifrost-emu/commit/232e0eb75b14337ecf317067f025271878405267) — 2026-08-26: fix(core,audio): guarded callback scratch stacks + multi-threaded malloc for the audio pump vCPU
- [d67ba00](https://github.com/sloppyman2567/bifrost-emu/commit/d67ba002f851252dfe9070f85dc39e81d6e559d0) — 2026-08-26: fix(jit): W-form width bugs - CBZ/CBNZ dirty-upper test (neverball), TST N-flag, shifts, EXTR/BFM, ADCS/SBCS, CLZ
- [6c1ee5f](https://github.com/sloppyman2567/bifrost-emu/commit/6c1ee5f18ead1c49679a03a910b1256134de1d6b) — 2026-08-26: feat(jit): deep-verification tooling (VERIFY_EVERY + MEMFULL mem diff)
- [51c6876](https://github.com/sloppyman2567/bifrost-emu/commit/51c68765f70f5894841c1bef0c5bed37addf40b2) — 2026-08-26: fix(opgen): parse IRSUB column base-aware; registry-checker fixes
- [cc09f2d](https://github.com/sloppyman2567/bifrost-emu/commit/cc09f2ddd2031fef98d928a4a2ea1f17540f17f9) — 2026-08-25: feat(gl): EGL registry audit + GLES2&lt;=3.2 coverage expansion (+245 rows)
- [ee57054](https://github.com/sloppyman2567/bifrost-emu/commit/ee570540857e04eaa738fd61d34e43ef5aeb8a97) — 2026-08-25: docs: neverball pre-existing JIT heap-corruption hunt state
- [5142851](https://github.com/sloppyman2567/bifrost-emu/commit/5142851c204a1d57758b7e254ad5451e261d114b) — 2026-08-25: feat(gl): bulk-pixel SizeKinds + native GLdouble rows (+65 fns, table 1493 -&gt; 1558)
- [cb26a53](https://github.com/sloppyman2567/bifrost-emu/commit/cb26a53b54ec114f9ad56ff2e4c7de4ae65321ef) — 2026-08-25: fix(test): de-hardcode test_capi version check; doc stale-build lesson
- [4b59fce](https://github.com/sloppyman2567/bifrost-emu/commit/4b59fceeffffcfff27f49ae8edd5617492192db5) — 2026-08-25: chore: ignore __pycache__
- [5772dcb](https://github.com/sloppyman2567/bifrost-emu/commit/5772dcbb2aa9bf36b34bf36d7ed0eb00a88ff248) — 2026-08-25: docs: AGENTS.md session history 2026-08-23..24 (audio, vk.xml milestones, vkQuake hunts, GL automation)
- [fd44a4f](https://github.com/sloppyman2567/bifrost-emu/commit/fd44a4f9e9281401767056df497f026cff6acd81) — 2026-08-25: feat(gl): registry-driven automation — glxmlcheck audit, coverage expansion, table-driven dispatch
- [e523a4e](https://github.com/sloppyman2567/bifrost-emu/commit/e523a4e370cd0562ef7e733126639f73e3622d8b) — 2026-08-25: fix(vk): MAP_MEMORY remap-offset + vk.xml enum-sized-array layout; vendor registries
- [31b076b](https://github.com/sloppyman2567/bifrost-emu/commit/31b076bdf7c1997e769ea379155fc1a845411728) — 2026-08-25: fix(mem): mmap allocator hardening — stack-skip, above-window fallback, VMA-split munmap
- [a08ec46](https://github.com/sloppyman2567/bifrost-emu/commit/a08ec46613abbe06f2169f7510ae265fa625256b) — 2026-08-24: docs: version bump to 1.5.5-alpha
- [fd22eba](https://github.com/sloppyman2567/bifrost-emu/commit/fd22eba402e872e50849d5048a04151c3101157e) — 2026-08-24: test(vk): vulkan_pnext — chain-query coverage (the gap vkQuake exposed)
- [f8a28c0](https://github.com/sloppyman2567/bifrost-emu/commit/f8a28c0ab7979ad467359583ee931ce7ade8dbf7) — 2026-08-24: fix(vk): enum-sized C arrays parsed correctly; bitfield structs excluded
- [aac476d](https://github.com/sloppyman2567/bifrost-emu/commit/aac476db15cb70299d54e7aaddf0e71e22c84a5d) — 2026-08-24: feat(vk): Phase B batch 5 — instance/device generated; hand-arm migration complete
- [12abbcc](https://github.com/sloppyman2567/bifrost-emu/commit/12abbccd0aab0a5e5594531d87dffb9cef4f68c4) — 2026-08-24: feat(vk): Phase B batch 4 — SUBMIT/SUBMIT2 + descriptor-set pair generated
- [0a7c8b9](https://github.com/sloppyman2567/bifrost-emu/commit/0a7c8b9779e7b286d33fc602d4a625e5d4d9d3b3) — 2026-08-24: feat(vk): Phase B batch 3 — Graphics/Compute pipelines via generated plans
- [dc02fe2](https://github.com/sloppyman2567/bifrost-emu/commit/dc02fe23c2802f893271d6e277d2660b93670d23) — 2026-08-24: feat(vk): Phase B batch 2 — RenderPass + DescriptorSetLayout via generated recursion
- [7a40df8](https://github.com/sloppyman2567/bifrost-emu/commit/7a40df8047a019afa9ced804fc6d92d964b49999) — 2026-08-24: feat(vk): Phase B batch 1 — create-style generated plans, delete 5 hand arms
- [a39d97f](https://github.com/sloppyman2567/bifrost-emu/commit/a39d97f3e37e51cbaa1fc8101a718a2f6d0f1069) — 2026-08-24: feat(vk): vk.xml milestone 3 — generated pNext chains (A1), OUT plans (A2), C1 rows
- [3d3c63f](https://github.com/sloppyman2567/bifrost-emu/commit/3d3c63f8e49756514a5b8ae7aaa8f3552cba4931) — 2026-08-24: docs: session history — FP-NOP fixes, audio rewrite + pump, vk.xml pipeline
- [82f8147](https://github.com/sloppyman2567/bifrost-emu/commit/82f8147fac3868b3269896489ad14e933d32be6e) — 2026-08-24: vk.xml: registry-driven signature audit + generated deep-marshal (VK_CMD_DEEP)
- [fd83924](https://github.com/sloppyman2567/bifrost-emu/commit/fd839245fa115fb3a9bf3ebc55078312479f403f) — 2026-08-24: audio: multi-stream mixer rewrite + dedicated-vCPU callback pump
- [cf0a8af](https://github.com/sloppyman2567/bifrost-emu/commit/cf0a8aff5c89adcaf53c75c73ada62d1f3ae524f) — 2026-08-24: interp: scalar-pairwise FADDP/FMAXP/FMINP + scalar-x-indexed FMLA/FMLS/FMUL/FMULX
- [5d61800](https://github.com/sloppyman2567/bifrost-emu/commit/5d6180008a5339d8dcc49ac3b31448acf2740f9d) — 2026-08-22: docs: session history — mambo Vulkan test + real-audio embed; suite counts 211
- [67e71c6](https://github.com/sloppyman2567/bifrost-emu/commit/67e71c62c7f531d0b9fe50d6ba665ffe8f89122d) — 2026-08-22: test: mambo Vulkan textured-quad + real-audio e2e test
- [e20fdab](https://github.com/sloppyman2567/bifrost-emu/commit/e20fdab35cdb09a594077eaad3233d7052865c41) — 2026-08-22: audio: working audio path — SDL2/ALSA/Pulse/OpenAL + AAudio/OpenSL ES arms on a shared AudioEngine
- [360e4e2](https://github.com/sloppyman2567/bifrost-emu/commit/360e4e2bfb7fb7f53dfb08ebe351dd42ae5a9889) — 2026-08-22: simd: SMOV + B/H-lane saturating add/sub native in JIT; sat-family interp ops + test_simd_sat
- [4214290](https://github.com/sloppyman2567/bifrost-emu/commit/4214290e4cdf16b171128e4bb364d92db4460a7c) — 2026-08-22: feat(android): NativeActivity lifecycle layer v2 — --android driver + ALooper/AInputQueue/AConfiguration/liblog
- [d38a019](https://github.com/sloppyman2567/bifrost-emu/commit/d38a019af52205e17b9efee0d310e0dac49e5d09) — 2026-08-22: feat: full ASLR (ET_DYN load bias + stack jitter); BIFROST_TIER2 defaults ON
- [97f2519](https://github.com/sloppyman2567/bifrost-emu/commit/97f2519540c0dafa3a44b52fd1372a5040112164) — 2026-08-22: thunk: pixel-bounce row alignment + tracked depth range; point-lookup SDL_ALLOC/FREE
- [ed07733](https://github.com/sloppyman2567/bifrost-emu/commit/ed0773347aa9308d8dc6799873c55cffdde183a9) — 2026-08-21: thunk: drop the \[szdbg2\]/\[szdbg3\] window-size probes — including the accidental DOUBLE host call
- [3bebb40](https://github.com/sloppyman2567/bifrost-emu/commit/3bebb40f448c2db40e2f45bbc0c5f0352e18c0d9) — 2026-08-21: fix: BIFROST_STATS_PERIOD reporter (3 stacked causes) + debug probe cleanup + docs/ reorganization
- [1ab6c58](https://github.com/sloppyman2567/bifrost-emu/commit/1ab6c5892501e4585e87a6066903fb7601f3b6b9) — 2026-08-21: feat(vulkan): vkMapMemory guest-window bounce — the Vulkan PCWFC
- [0e3be61](https://github.com/sloppyman2567/bifrost-emu/commit/0e3be61113006e6c0c837d3ccd711947b401e9b0) — 2026-08-21: feat(vulkan): graphics pipelines + descriptor sets deep-marshal; real vkCmdDraw triangle (ROADMAP #12)
- [8ccc57f](https://github.com/sloppyman2567/bifrost-emu/commit/8ccc57f6f6802b47c29471add3ba81da098038a1) — 2026-08-21: feat: Neverball support batch — GL fixed-function/SDL math thunk rows + widening/narrowing SIMD families
- [7945799](https://github.com/sloppyman2567/bifrost-emu/commit/79457993b16727aac95b5df1c279cbbdc619cddf) — 2026-08-21: Merge csel-cmov-repair: native CMOVcc CSEL + flag-loader scratch fix (6032bca)
- [6032bca](https://github.com/sloppyman2567/bifrost-emu/commit/6032bcad46b88488394a2f6989f8db0be3ffbeec) — 2026-08-21: jit: flag-loader scratch contract — RDX-only C^from_sub, no R8 (fixes cset-after-fcmp)
- [270bb8a](https://github.com/sloppyman2567/bifrost-emu/commit/270bb8af24afdbca0c999a5ccae321f13d71d8a5) — 2026-08-21: csel-cmov WIP: fix direct-carry HI/LS inversion — cmc assumed SUB convention
- [6bbde2d](https://github.com/sloppyman2567/bifrost-emu/commit/6bbde2d1c92d77b93d905057451200303fea82ac) — 2026-08-21: Reapply "jit: indexed window addressing — guest address rides the SIB index into \[WIN_REG+idx\]"
- [db0df07](https://github.com/sloppyman2567/bifrost-emu/commit/db0df072ae664afdb454c7111f1a13cefdf6a68a) — 2026-08-21: Revert "jit: indexed window addressing — guest address rides the SIB index into \[WIN_REG+idx\]"
- [7671af3](https://github.com/sloppyman2567/bifrost-emu/commit/7671af339a62320e783867205ce3bb6e0c19efc8) — 2026-08-21: Revert "jit: CSEL family via CMOVcc — no full flush, no staging, no mispredicted branch"
- [7b23940](https://github.com/sloppyman2567/bifrost-emu/commit/7b23940c3de0da33b28603a2ce1cb2bec6e4173e) — 2026-08-21: jit: CSEL family via CMOVcc — no full flush, no staging, no mispredicted branch
- [dc5b78e](https://github.com/sloppyman2567/bifrost-emu/commit/dc5b78eac1ac18ae47e2582be2fb81b4c0a9f992) — 2026-08-21: regalloc: ALU-emitter variant-2 audit complete — ensure_two_vregs seals the stale-operand class
- [99d39b9](https://github.com/sloppyman2567/bifrost-emu/commit/99d39b99d16667db9563533649662b2a3aaf1b21) — 2026-08-21: jit: indexed window addressing — guest address rides the SIB index into \[WIN_REG+idx\]
- [51276f3](https://github.com/sloppyman2567/bifrost-emu/commit/51276f39c8f25fcc52d398147fc614fa79883959) — 2026-08-21: tier2: P2-lite keep residency — pin hottest LICM roots into free callee-saved regs across the Lback
- [b812657](https://github.com/sloppyman2567/bifrost-emu/commit/b81265789b07da4f6635865e7a1bdfb828bd4904) — 2026-08-21: jit: emit_store_mem in-place address — drop the R8 copy and RAX save/restore
- [c4cabdf](https://github.com/sloppyman2567/bifrost-emu/commit/c4cabdf6b695ea2965ec2ed9b1edf7fd7829bcdc) — 2026-08-21: tier2: dest-sourcing rename — rewrite fresh dests to dying src1 for in-place ALU/shift codegen
- [813a635](https://github.com/sloppyman2567/bifrost-emu/commit/813a63539a0c08b1bc94beb7883657eec8942110) — 2026-08-20: docs: Android surface layer Phase 1 contract — shim architecture, EGL row fixes, Wayland connection identity
- [9f3d5cd](https://github.com/sloppyman2567/bifrost-emu/commit/9f3d5cda78eb1993e0d2933ccf6f3ea1fd4b5c47) — 2026-08-20: android: ANativeWindow shim + EGL window-surface interception (Phase 1)
- [868bc05](https://github.com/sloppyman2567/bifrost-emu/commit/868bc051e9b3003bb37dc526690d670b70333020) — 2026-08-20: docs: emitter bug variant-1 fixed; variant-2 documented as fenced limitation
- [afab35d](https://github.com/sloppyman2567/bifrost-emu/commit/afab35d3c2494e29f271706ea3c89bd7bfd61713) — 2026-08-20: regalloc: fix alloc_reg_excluding desperate fallback — silent exclusion violation
- [2f10b38](https://github.com/sloppyman2567/bifrost-emu/commit/2f10b382e415425bed017504b43847db9adc14f3) — 2026-08-20: docs: P2 reverted — latent ALU-emitter stale-operand-reg bug found (add %r11,%r11 disassembly evidence); audit required before P2 retry
- [674a641](https://github.com/sloppyman2567/bifrost-emu/commit/674a641f385689049e29e48ecf65636918b6290b) — 2026-08-20: docs: review finding — region exits have no dirty scratch in practice; arch-only exit flush measured ~zero and reverted
- [29f80bb](https://github.com/sloppyman2567/bifrost-emu/commit/29f80bbda5d37f229fbae25b3ee11d1b54230cea) — 2026-08-20: docs: CHANGELOG Phase 4 — call-aware regions, chain-skip composition, loop-only, exit chaining (CoreMark +7.8% under tier2)
- [88c380f](https://github.com/sloppyman2567/bifrost-emu/commit/88c380fc8cd842ab68ced0f219fa64e6d3772309) — 2026-08-20: tier2: chain-out exits — regions jump straight to translated targets
- [49fc209](https://github.com/sloppyman2567/bifrost-emu/commit/49fc2099ec8a2d3c951e74d82de2214f935bb8e7) — 2026-08-20: tier2: default hot-head threshold 10000 -&gt; 1000
- [e1e5ee2](https://github.com/sloppyman2567/bifrost-emu/commit/e1e5ee2603813aeb8369045ec6245a920b79d276) — 2026-08-20: docs: plan.md — all tracks resolved
- [0c30ce6](https://github.com/sloppyman2567/bifrost-emu/commit/0c30ce6929e7d6208ad40b935ec0a2622659671c) — 2026-08-20: docs: Track 5 closed — LICM now +16-17% on its target shapes, pins net-positive; no changes warranted
- [d0fcc7d](https://github.com/sloppyman2567/bifrost-emu/commit/d0fcc7df2da43c87946328a396944578622e29ce) — 2026-08-20: docs: Track 3 shared-exit-tail contract + measurement trap note
- [10b0b4d](https://github.com/sloppyman2567/bifrost-emu/commit/10b0b4d175f7a829ee34c6f19e9c764aee29c45a) — 2026-08-20: tier2: shared exit tail — dedupe cold-exit epilogues (Track 3)
- [da390d7](https://github.com/sloppyman2567/bifrost-emu/commit/da390d7429d9178d0bc18d2d5d8dea5ce23a4302) — 2026-08-20: docs: loop-regions-only contract + counter-tax measurements; Track 2 superseded by regions
- [0c518c5](https://github.com/sloppyman2567/bifrost-emu/commit/0c518c5565e939f95f897eca4bb07b70314a5c7a) — 2026-08-20: tier2: loop regions only — linear regions were net-negative (CoreMark +3.5%)
- [7dff908](https://github.com/sloppyman2567/bifrost-emu/commit/7dff9084128e5684160bfd43a7e7e85e763641f9) — 2026-08-20: docs: tier-2 Track 4 session notes — regions x chain-skip composition contract
- [b84cc22](https://github.com/sloppyman2567/bifrost-emu/commit/b84cc223a8484213881d990a321d11f8faa9b051) — 2026-08-20: tier2: compose regions with chain-skip (Track 4)
- [00d8b3d](https://github.com/sloppyman2567/bifrost-emu/commit/00d8b3d260eb7051e761b47496d8a673c83b46ca) — 2026-08-20: docs: tier2 counter wrap-form rejected — cmp+jcc macro-fusion makes the old 3-inst counter already ~2 uops
- [ebd2764](https://github.com/sloppyman2567/bifrost-emu/commit/ebd2764a62351b2a0ab721e1e0b9d1fb803c7618) — 2026-08-20: docs: tier-2 Track 1 session notes — minecraft A/B methodology lessons
- [054a4d5](https://github.com/sloppyman2567/bifrost-emu/commit/054a4d5993455ee40c720b357723642b6403ce8f) — 2026-08-20: tier2: call-aware regions — fuse BL/BLR into traces (Track 1)
- [2c04f77](https://github.com/sloppyman2567/bifrost-emu/commit/2c04f773462428a92c07f0b56e5cdce653f2a1c2) — 2026-08-20: docs: 1.5.4-alpha tier-2 M2 region optimizations + self-loop fusion (CHANGELOG/ROADMAP/TESTS)
- [28c82e6](https://github.com/sloppyman2567/bifrost-emu/commit/28c82e69a348be67587bb686955c8f59d1c2332f) — 2026-08-20: tier2: M2 region optimizations (DCE/LICM/const-prop) + self-loop fusion + chain-repatch fixes
- [85a09e9](https://github.com/sloppyman2567/bifrost-emu/commit/85a09e9b2bf708c1da64041d825bd50fc4b85f52) — 2026-08-20: tier2: fix region pin correctness (CSEL invalidate drop + cold-exit stale pins)
- [56643a2](https://github.com/sloppyman2567/bifrost-emu/commit/56643a2268896a05fec33e064d116b6cb2436ba6) — 2026-08-19: docs: 1.5.4-alpha — tier-2 JIT milestone (CHANGELOG/ROADMAP/version + full version sweep)
- [64ccf49](https://github.com/sloppyman2567/bifrost-emu/commit/64ccf499d9074967c3a20d7a2f49039b97342ff4) — 2026-08-19: jit: tier-2 Phase 2 step 1 — loop-carried arch-GPR pinning + CBZ/TBZ tight self-loop slot
- [63d042a](https://github.com/sloppyman2567/bifrost-emu/commit/63d042a6b6faf75c830f960a19aafd4eca58ac5a) — 2026-08-19: docs: tier-2 in-code hot-head counter contract (AGENTS.md + ROADMAP.md #14)
- [ad5098a](https://github.com/sloppyman2567/bifrost-emu/commit/ad5098acfd7c4d474883e326859a5250bb49a9ce) — 2026-08-19: tier2: in-code hot-head counter + back-edge region firing (Phase 1 step 4)
- [f39a32b](https://github.com/sloppyman2567/bifrost-emu/commit/f39a32bc65154dda7c3fb49a165d1e2879cbd775) — 2026-08-19: docs: tier-2 Phase 1 step 3 status (region compiler M1, c848391)
- [c848391](https://github.com/sloppyman2567/bifrost-emu/commit/c8483911a132dd05492d6227095c7dc02ef21ae2) — 2026-08-19: jit: tier-2 Phase 1 step 3 — compile_tier2_region M1 (one x86 fn per trace)
- [410aea1](https://github.com/sloppyman2567/bifrost-emu/commit/410aea11ed3faf9071c1f9018f670988dcec9484) — 2026-08-19: jit: tier-2 Phase 1 step 2 — collect_tier2_trace walker (pure collection)
- [377aad8](https://github.com/sloppyman2567/bifrost-emu/commit/377aad8c7d309cace40d0f20544ecd05b72a97ef) — 2026-08-19: jit: tier-2 Phase 1 step 1 — per-block hot-head counters + env gates
- [928f3da](https://github.com/sloppyman2567/bifrost-emu/commit/928f3dac231281c6154d1f13bcd46827f6ca5202) — 2026-08-19: roadmap: tier-2 JIT region/trace compilation spec
- [8e39f40](https://github.com/sloppyman2567/bifrost-emu/commit/8e39f4047461ff7045c84e3208b73b0e08b16133) — 2026-08-19: thunk: precompute GLStateTracker::tracks_state on SymbolEntry (drop per-call string hash)
- [5d8e751](https://github.com/sloppyman2567/bifrost-emu/commit/5d8e7512a45ec607781732d36c54b1f2d7e571c9) — 2026-08-19: Vulkan command-buffer rendering through DisplayThunk
- [cacc317](https://github.com/sloppyman2567/bifrost-emu/commit/cacc31711792e9bb8fc68a917a5f714b42dda721) — 2026-08-19: feat(capi): Android native bridge adapter + guest dl* wrappers
- [9b4e7a3](https://github.com/sloppyman2567/bifrost-emu/commit/9b4e7a392c51e03e662099b05956d3fe0c82f2fa) — 2026-08-19: docs: document C API refinement (bifrost_call, svc hook, breakpoints)
- [371ae96](https://github.com/sloppyman2567/bifrost-emu/commit/371ae96c2d9f176b9e5f6404be001b319cecef5b) — 2026-08-19: feat(capi): bifrost_call, svc hook, real breakpoints, JIT default

</details>

