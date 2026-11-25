# Tests

How the suite is organized and run. Passing fixtures do not establish untested application behavior, and a smoke
launch is not game playability. The dated checkpoints that used to live here (what was run, when, and what it found)
are kept unchanged in [the test history](archive/TESTS-history.md); the changelog records what changed.

## Last full run

On 2026-11-01, with every fixture provisioned, the strict suite passed **377/377** under the JIT and **362/362** under the interpreter
(15 JIT-only tests are not run there), and `make verify` passed 100 differential fixtures plus ten threaded oracle runs. Missing
fixtures fail strict mode; environment skips (exit 77) stay visible. This is a dated figure, not a claim about the current tree.

## Test inventory

Original regression sources ship under Bifrost's Unlicense. Generated guest
ELFs are rebuilt locally; included and downloaded third-party fixtures keep
their upstream licenses. See [fixture distribution](THIRD_PARTY.md) for the
source/binary distinction and game-data boundaries.

The `fcsel_high_regs` regression has 61 checks for high FCSEL source registers,
FP destination 31, single/double precision, aliasing, GPR preservation, and
discarded FP-to-GPR writes to XZR. It passes under QEMU, JIT, and interpreter;
see [the vkQuake reproduction](Apps/vkquake.md).

The runner's categories are: unit (focused guest instruction and JIT regressions and host gates), integration (guest programs in `Tests/Apps/` and
`Tests/Asm/`), sandbox, per-thread JIT, uncached FP registers (the by-element FP fixtures with `BIFROST_NO_VEC_CACHE=1` and
`BIFROST_NO_FP_CACHE=1`), interactive (scripted stdin), toybox, real-world static and dynamic programs,
dynamic linking (musl/glibc and XWayland; needs a rootfs) and benchmarks (omitted by `--quick`). The runner prints
each category's count when it starts, and `--filter REGEX` runs only the tests whose names match. Without a configured rootfs the
dynamic runs are not selected; missing guest fixtures and unavailable display or SDL support appear as skips, never as passes.

A fully provisioned `run_tests.sh --strict` run on 2026-11-14 (an SDL/GL build, offscreen display, no window tests) ran 402 tests with the JIT, all
passing, and 374 with `--no-jit`, all passing, with 30 skipped there (the JIT-only tests, and `android_surface` and `vulkan_texture`, which need a
window). `make verify` and the host tests passed. In the first JIT run `jit_mt_publication` timed out at its 120 s limit, and
it passed (2 s) on every rerun. The likely cause is that, started as `--emu ./bifrost-emu` after a version change, it first
builds the headless library inside that limit (the library's timestamps fit; the log does not say). To avoid it, run with
`--emu build/release-sdl1-gl1/bifrost-emu`, or build the headless library first (`make USE_SDL2=0 USE_THUNK_GL=0 lib`).

### Saturating narrowing shifts (2026-11-23)

`Tests/Unit/jit_shrn_sat.c` (`run_tests.sh` entry `jit_shrn_sat`) builds the instruction word for every one of SQSHRN, UQSHRN,
SQRSHRN, UQRSHRN, SQSHRUN and SQRSHRUN at every shift for 16-, 32- and 64-bit source lanes, in both destination halves and with
`Vd == Vn`, runs each on lanes at the rounding, saturation and sign boundaries plus pseudo-random ones, and compares the
result and FPSR.QC with an exact `__int128` model (32,257 checks); a last loop keeps the source in a vector register. The
model and the encodings pass under QEMU, the interpreter and both JITs, and the fixture fails on a JIT without the QC update
or without the rounding add. It does not run a host without SSE4.1.

### In-place pin updates (2026-11-23)

`Tests/Unit/jit_pin_inplace.c` (`run_tests.sh` entry `jit_pin_inplace`) runs self-loops that update pinned registers in the
shapes the optimisation takes: a counter in `x0` with a later reader of the new value, 64-bit add/sub/eor/orr, 32-bit
add/sub/eor/mul, the old value read before the update, a `madd`, each checked against plain C for several trip counts, and a
faulting load on either side of the update (the registers in the signal frame must show the update done or not). It passes
under the interpreter and the JIT, with and without `BIFROST_NO_PIN_INPLACE`, and it fails (7 `madd_shape` checks) with the
`ADD` guard in `Arithmetic.cpp` removed. It does not run threads or a game.

### Stall lines (2026-11-19)

`Tests/Unit/test_wait_stall.c` and `Scripts/Testing/test_wait_stall.sh` (`run_tests.sh` entry `wait_stall`) check, with the JIT and with
`--no-jit`, that `BIFROST_PROF` prints one `[stall]` line for a busy thread's futex wait and for a busy thread's `nanosleep` (with the
thread name, the futex address, a caller and the time of day), for a busy thread blocked in `read` on a pipe and on a unix socket (with
what the descriptor is), only five lines for a call site that stalls nine times (the report counts all of them, lists twelve such sites and says how many more there are), none for a thread that only waits, none without `BIFROST_PROF`, and none
for calls under a second. It fails on a build without the lines, and without the busy-thread rule. Its threads spin in CPU time and the waiters are timed from a
handshake, so it gives the same result on one core as on sixteen. It does not cover a real game.

### Guest identity (2026-11-23)

`Tests/Unit/test_uname_consistency.c` (`run_tests.sh` entry `uname_consistency`) checks that `uname`, `gethostname()`,
`/proc/sys/kernel/hostname`, `/proc/sys/kernel/osrelease` and `/proc/version` agree. It also passes on a real Linux kernel. It fails on
the build before the fix (the hostname file said `bifrost`, `/proc/version` lacked the version string). The Android DNS test
`test_guest_hostname_matches_the_emulators_uname_nodename` ties the DNS service's copy of the nodename to the same header.

### Syscall names (2026-11-19)

`Scripts/Testing/test_syscall_names.sh` (`run_tests.sh` entry `syscall_names`) compares every entry of the syscall name table with
`<asm-generic/unistd.h>` on the machine (skipped without it; an entry newer than that header cannot be judged), and runs a guest that calls
`sendto`, `recvfrom`, `sendmsg` and `recvmsg` under `BIFROST_SYSCALL_TRACE` to see them under 206, 207, 211 and 212. It fails on the
build before the fix (the trace part) and on a table with `getuid` or `getrandom` under their old wrong numbers (the table part).

### Tracy option (2026-11-19)

`Scripts/Testing/test_tracy_profile.sh` (`run_tests.sh` entry `tracy_profile`) checks that `BIFROST_PROF=tracy` is accepted by any
build: a normal build says it needs `make USE_TRACY=1`, a Tracy build starts the profiler for the process or only for a matching
target, a target that matches nothing starts none, and neither runs the sampler. If `build/release-sdl1-gl1-tracy` exists it also runs a
program that forks 40 times with the profiler started (a smoke check: short-lived children pass without the child-side guard too).
It does not check the timeline. That was checked by hand with Tracy's `capture` and `csvexport` (built from the 0.15.0 release): zone and
thread names, GL symbol zones, frames, and the fork program with a viewer connected.

### Mid-block conditional exits (2026-11-24)

`Tests/Unit/jit_side_exit.c` and `Scripts/Testing/test_jit_side_exit.sh` (`run_tests.sh` entry `jit_side_exit`, also a threaded oracle in
`make verify`) run hand-written loops (a list walk, a byte scan with four carried registers, two exits in one loop, a loop faulting
mid-iteration, one loop per ARM condition for `b.cond` on edge values, and `tbz`/`tbnz` on bits 0 and 63) against C, check that a fault shows the finished iterations' registers in the signal frame, and repeat the loops on five threads, with
exits on and off, chaining off, the ordinary frame layout and IR validation. With exits on, `-v` must report at least forty blocks formed with a
mid-block exit. It does not cover a `b.cond` after an `FP_CMP`, loops with an inner branch, a game, or the persistent cache (such blocks are not recorded).

### Carry follows the hot side of an exit (2026-11-24)

`Tests/Unit/jit_carry_follow.c` and `Scripts/Testing/test_jit_carry_follow.sh` (`run_tests.sh` entry `jit_carry_follow`, also a threaded oracle in
`make verify`) run loops with a forward `cbnz`, `tbnz` or `b.ne` whose direction comes from a pattern array (always taken, never, taken for bit 1
only, alternating, mostly), in phases so a branch is flipped and flipped back, and a loop that reads a flag set before the branch on both sides of
it, and a loop with a call before the branch and one at its target (the flipped block reaches the two-call limit at that call), against C, then repeat them on four threads (each with its own pattern array: a shared one made the fixture fail with Carry off). They run with
`BIFROST_CARRY=1`, with `BIFROST_NO_CARRY_FOLLOW=1`, with Carry off, with chaining off, in the ordinary frame layout, with IR validation and with
register passing off; with the feature on, `-v` must report at least one flipped branch. `jit_side_exit` gained `flag_live`, `flag_dead`,
`flag_live_bit` and `cover_exit` for the exit flag store and the guard cover. They do not cover a game or the persistent cache (blocks with mid-block
exits are not recorded).

### Register passing (2026-11-24)

`Tests/Unit/jit_reg_pass.c` and `Scripts/Testing/test_jit_reg_pass.sh` (`run_tests.sh` entry `jit_reg_pass`, also a threaded oracle in `make verify`) run
hand-written block chains (unconditional branches, a diamond, registers moved between each other across edges, a call in the middle of a chain,
a block reading registers three predecessors write, a register handed over blocks that do not touch it, a block whose predecessors hold different
parts of its entry registers, and a loop faulting mid-iteration) against C, check that the signal frame shows the finished
iterations' registers, and repeat them on four threads. They run with `BIFROST_CARRY=1`, with `BIFROST_NO_REG_PASS=1`, with `BIFROST_REG_PASS_THROUGH=1`, with `BIFROST_NO_REG_PASS_PARTIAL=1`, with Carry off, with
chaining off, in the ordinary frame layout, with IR validation and with mid-block exits off; with the feature on, `-v` must report at least
twenty chain edges into a register entry and at least one into a partial-entry stub. It does not cover the persistent cache (blocks with register entries are not recorded) or a game.

### Carry loop report (2026-11-24)

`Tests/Unit/carry_loop.c` and `Scripts/Testing/test_carry_report.sh` (`run_tests.sh` entry `carry_report`) run a hand-written four-block loop over five
registers with `BIFROST_PROF=1 BIFROST_PC_HIST=1` and check that the first `[carry] loop` line has at least two blocks, at least 50% of the JIT
samples and five to eight registers, and that nothing prints without `BIFROST_PC_HIST`. It fails on a build without the report. It does not
check the exit counts, loops through calls or any real program.

### Guest instruction counts (2026-11-23)

`Tests/Unit/jit_stats_count.c` and `Scripts/Testing/test_jit_instruction_count.sh` (`run_tests.sh` entry
`jit_instruction_count`) check that `BIFROST_PROF` prints its report at exit without `-v` (the waiting report with the JIT
and with `--no-jit`, the JIT sample report with the JIT), that nothing prints without it, and that the JIT's `-v`
guest-instruction count for a self-loop-and-call workload matches the interpreter's count for the same program (it is
exactly equal on this build; the check allows 2 %). It fails on the build before the fix (a report only under `-v`, and a
count around a thousandth of the interpreter's). It does not check that `BIFROST_PROF` alone leaves the counters out
(that is timing, not output), and it does not cover blocks served from the persistent JIT cache, which
are not counted.

### Waiting report (2026-11-19)

`Tests/Unit/test_wait_profile.c` runs five threads that each sleep, wait on a futex, wait in poll, block on a pipe or compute for
0.6 s. `Scripts/Testing/test_wait_profile.sh` (`run_tests.sh` entry `wait_profile`) runs it under `BIFROST_PROF=1`, with the JIT and
with `--no-jit`, and checks that the report charges each thread at least half of its lifetime to the right kind, and that nothing
prints without `BIFROST_PROF`. It fails on a build without the report. It does not cover the GL thunk column, Binder ioctls, or an app.

### `/proc/self/maps` stays current (2026-11-19)

`Tests/Unit/test_proc_maps_live.c` (`run_tests.sh` entry `test_proc_maps_live`) reads `/proc/self/maps` after each anonymous
mmap, mprotect, MAP_FIXED replacement, file mapping, munmap and `brk` change and expects the next read to show it, 300 map and unmap
pairs in a row, 50 reads with no change that must return the same text, and 40 forks while two threads read maps (every child
must read maps and exit). It passes natively on Linux. With the invalidation calls removed from `Memory`'s mapping index it fails.
The maps list is cached between changes of the guest's mappings (`Memory/AGENTS.md`), so this is the guard against a stale list.

### Host-native memcpy, memmove and memset (2026-11-16)

`Tests/Unit/jit_native_libc.c` (`make verify` runs it too, with the native path off) checks results against byte loops at
12 sizes from 1 KiB to 1 MiB and several alignments, overlapping moves both ways, an overlapping `memcpy`, the end of a
mapping, a read-only destination and unreadable or `PROT_NONE` ranges (which must still fault in the guest routine), live
callee-saved values and nested calls; `Tests/Unit/test_native_libc_mt.c` does it on five threads.
`Scripts/Testing/test_jit_native_libc.sh` (`run_tests.sh` entry `jit_native_libc`, under a minute with the interpreter run)
runs them with the native path on (the stats line must show calls), off (`BIFROST_NO_NATIVE_LIBC=1`), under `--no-jit` and
under `BIFROST_JIT_VERIFY` (the last two must show none), and the glibc dynamic build
`Tests/Unit/jit_native_libc_glibc.elf` (ifunc-resolved routines, needs the rootfs).
`Scripts/Android/test_android_native_libc.sh` (by hand, like `test_android_bionic.sh`; needs clang, lld and the pinned
runtime, about 7 s) builds `Tests/Unit/android_bionic_memory.c` against the actual Bionic `libc.so` and runs it through
`linker64` with the native path on (410 checks, 414 native calls), off and under the interpreter.

### JIT emitter slow paths (2026-11-17)

`Tests/Unit/jit_emitter_slow_paths.cpp` (`run_tests.sh` entry `jit_emitter_slow_paths`, a few seconds) emits the JIT's
helper-call sequences into a code buffer, runs them and checks them against the plain x86 result: `emit_call_aligned` keeps
the flags and the stack alignment for 0 to 3 pushes, the store slow path passes the value for any source register, and the
load slow path matches the fast path for 1, 2, 4 and 8 bytes with and without sign extension. Guest code does not reach
these paths today, so no guest fixture covers them. It does not check the other emitters.

### Persistent JIT cache (2026-10-26)

`Scripts/Testing/test_jit_disk_cache.sh` (`run_tests.sh` entry `jit_disk_cache`, about 2 s) runs
`Tests/Unit/jit_dispatch_cache_growth.elf` with the cache in a temporary directory: a recording run, a reuse
run (over 90 % hits), verify mode (no mismatch), damaged `main.bin` and truncated segments (rejected, results
correct), a 1 MiB budget (file bytes on disk stay within it), two concurrent writers and their merge, a codegen
switch (separate fingerprint), `Tests/Unit/jit_disk_cache_relocated.elf` mapping one code page from a file at
0x10000000 and then 0x7000000000 (verify finds the record equal to a fresh compile, the high mapping reuses it;
the previous distance-based immediate rule failed this), a fourth build whose budget holds about two of the three existing directories
(older ones pruned, nothing it records dropped) and a group-writable cache directory (cache off, program
unaffected). It passed 10 runs in a row when it was written. It does not cover Android apps, code-cache renewals in a game or
network filesystems; [JIT.md](JIT.md#persistent-translation-cache)
records the manual checks.

### Thread memory and the guest's RAM view (2026-10-25)

`test_thread_memory.sh` runs 1 and 200 idle guest threads and reads the emulator's peak RSS from `/proc`; the 200-thread run
may exceed the 1-thread run by at most 120 MB (28 MB now, 352 MB before the decode cache became lazy; the script
fails on the old build). `test_guest_memory.sh` checks the policy function on the host (cap, pressure, inconsistent
and empty hosts) and then `/proc/meminfo` against `sysinfo` inside the guest at the default cap, 2048 MB and no cap
(toybox's field order, `free <= available <= total`, no swap, the expected total).

### Code-cache generations (2026-10-25)

`Tests/Unit/jit_cache_generations.c`, run by `Scripts/Testing/test_jit_generations.sh` (about 8 s), forces a
1 MiB code budget so the shared JIT is replaced about 500 times while threads sleep in nanosleep and futex,
one waits in a futex inside a nested call, two run nested call loops that compile throughout, the main thread
rewrites code with instruction-cache maintenance, a 3 ms timer delivers signals, and a child forks afterwards;
every result is checked. The default layout must release each retired generation; the ordinary-frame layout
(`BIFROST_NO_CHAIN_SKIP=1`) keeps only the first and falls back, which the script accepts. `test_jit_cache.sh` now
expects renewals at a 1 MiB budget.

Run on the final build: the 333 unit, integration, toybox and real-world runs (`--quick`) passed at the default
budget. With `BIFROST_JIT_CACHE_MB=1`, 332 of those 333 passed (the one failure was a Tier-2 test, removed since), and the 22 dynamic-linking runs selected with
`--filter 'test_dyn|hello_dyn|dlopen|dladdr'` passed. The generations fixture also passed 20 of 20 repeated runs
pinned to different CPU sets, three at a time. `make verify` reported two failures, both present before this
change: `jit_bfm_extract` times out (exit 124; the 95 s against the 30 s limit recorded on 2026-10-23, not
re-measured on the old tree), and `jit_dispatch_cache_growth` is a threaded fixture that `verify_jit.sh` runs
under differential replay, which logs a PC divergence and "suspended" (identical output from the pre-change
binary). The full strict suite and the Android tests were not run. One real game, Melon Playground, ran five
minutes on this build while being played and stayed up (memory numbers in [JIT.md](JIT.md)).

## Running tests

### The whole suite on a fresh machine

On a new Linux machine, two commands run everything, with no display, GPU or sound needed:

```sh
Scripts/Setup/install-deps.sh --yes --no-asgard --tests    # packages (apt, dnf, zypper or pacman; sudo; no questions)
make check-fresh                                     # builds, provisions, runs every stage, prints a summary
```

`make check-fresh` builds the emulator, fetches the glibc and musl cross toolchains, the Debian arm64 libraries and programs the
tests need, builds the rootfs, then runs the host tests, the JIT runner, the interpreter runner and
`make verify`. A failing stage does not stop the later ones. It ends with a table of stages and the name of every skipped
test, and exits 1 if a stage failed or a test skipped for a reason other than the six below. Logs are in
`build/check-fresh/`. `--quick` runs only the host tests and the JIT runner. Run it a second time and it reuses what it
fetched.

Five tests skip on a machine without a window system, an input device or a GPU, because they cannot run there:
`gles_native_fence` (needs an EGL extension that software rendering lacks), `android_surface` (needs SDL3 and a window),
`vulkan_texture` (needs a window that can present Vulkan) and `wayland_bridge` and `wayland_input` (need a Wayland
compositor; the second also needs an input seat). Run them on a desktop session. A sixth, `xwayland_wire`, is not run
because it needs the real arm64 X11 libraries in the rootfs, which no setup script installs; CI treats it as optional too. The other GL tests run on any
machine: with no display the runner uses Mesa's display-less EGL, on the CPU if there is no GPU.

The dnf, zypper and pacman package names in `install-deps.sh --tests` have not been run.

Additional standalone regressions build their own fixtures in temporary
directories and exercise the current emulator (`BIFROST_EMU` can override it):

```sh
./Scripts/Testing/test_rootfs_discovery.sh  # dynamic programs find a rootfs without flags
./Scripts/Testing/test_rootfs_install.sh    # install-rootfs.sh output runs from an installed layout
./Scripts/Testing/test_guest_cwd.sh         # getcwd/cd follow the host cwd without a sandbox
./Scripts/Android/test_android_super.py        # dynamic-partition unpacker vs a synthetic image (--compare SUPER.img: vs lpunpack)
./Scripts/Android/test_android_adapter_exports.py # the built managed libGLESv2.so exports every core ES 2.0-3.2 command and libandroid.so exports ASurfaceControl_createFromWindow (an app library's missing import fails its whole dlopen)
./Scripts/Android/test_android_provisioning.py  # asgard setup: the steps a tree lacks and running them (fake commands), tied to the Makefile and the installer
./Scripts/Android/test_android_pairing.py       # an installed Asgard warns when the emulator running it is not the build it was installed for (recorded digest, silencing, what is not judged)
./Scripts/Android/test_android_gamepad.py     # the library's gamepad control: pad states to actions, repeat, stick, keys per widget (offline)
./Scripts/Android/test_download_progress.py    # asgard-setup's download progress lines (terminal and piped), and resumable fetch against a local HTTP server
./Scripts/Android/test_file_digests.py         # launch-time file hashes are reused only while size, mtime, ctime and inode are unchanged (same-size edit with mtime restored, replacement, recent file, unusable cache)
./Scripts/Android/test_asgard_stale_broker.py  # a broker an upgrade left running: restarted when idle, refused when busy; realm stop --force; the realm idle timeout (an idle realm ends, an attached app keeps it, a control request restarts the clock, 0 and a bad value, an emulator without the option); a broker that cannot start reports its exit and output (both start paths)
./Scripts/Android/test_asgard_process_host.py   # service children carry --asgard-service-parent and die with a killed supervisor (real emulator, plus a control)
./Scripts/Android/test_media_publish.py         # a MediaStore picture is copied to ~/Pictures/Bifrost/<app> byte for byte under a free name, and nothing outside DCIM/Pictures, no symlink at any step, no special or oversized file
make test-shader-index-conversion               # the int() rewrite glCompileShader retries a rejected ES 3.x source with; gles_uint_index (run_tests.sh) checks it through the thunk on the host driver
make test-android-wheel-gestures               # wheel/pinch/middle-drag as two-finger gestures (SDL-free); test-android-sdl-backend checks them through the backend
make test-android-pad-cursor                   # the gamepad touch cursor (SDL-free): Start+Select chord, modes, stick, fingers, edge crop; test-android-sdl-backend plays it through the backend
make test-android-window-size                  # the remembered window size file (SDL-free): load/store, damaged files, fitting to the room; test-android-sdl-backend restores and saves it through the backend
./Scripts/Testing/test_window_stats.sh         # window stats modes, overlay and sensors (SDL probes open windows unless SDL_VIDEODRIVER=offscreen; exit 77 without host SDL2)
./Scripts/Testing/test_rootfs_profile.sh   # named rootfs profiles: install, list, select, errors, uninstall (exit 77 without the fetched toolchains)
./Scripts/Testing/test_setup_rootfs.sh     # bifrost-emu --setup-rootfs installs, uninstalls, runs a packaged installer from the cache and reports a missing one
./Scripts/Testing/test_guest_env.sh         # the guest gets the caller's environment (curated one in a sandbox)
./Scripts/Testing/test_interp_elf_validation.sh # a malformed PT_INTERP file (bad segment range, header table past the file, non-AArch64) ends the launch with an "interpreter:" error, not a host fault; a valid one still loads
./Scripts/Testing/test_exec_jit.sh          # programs started by execve run under the JIT (exit 77 without the busybox fixture)
./Scripts/Testing/test_apk_launch.sh        # bifrost-emu app.apk hands the package to Asgard (stub)
./Scripts/Testing/test_host_cpu_models.sh   # JIT fixtures on older x86-64 CPUs under qemu-x86_64; exit 77 if it cannot run
python3 Scripts/Testing/sequence_fuzz.py --seed 1 --batches 5  # random A64 sequences (immediate, register, FP/SIMD groups) under qemu-aarch64, the JIT and the interpreter, every register compared; mismatches are classified (jit, shared, split, signal), shrunk and written to build/sequence-fuzz; needs qemu-aarch64 and the musl cross compiler; QEMU is a reference, not the Arm manual
make test-audio-declick                     # a stream leaving or rejoining the mix never steps the output (no sound device needed)
./Scripts/Testing/run_tests.sh --filter vulkan_astc  # ASTC textures through the Vulkan thunk, headless: queries, device creation, uploads of four footprints with mips and edge blocks, sampled back (needs a Vulkan driver; exit 77 without ASTC and BC7)
make test-astc-bc7-transcode                # ASTC LDR decoder (hand-built blocks, unquantization tables, fuzz), BC7 encoder (every mode packed and decoded by an independent decoder) and the strip transcoder; not compared with ARM's decoder or a GPU
./Scripts/Testing/test_asgard_install.py    # install-asgard.py install, update, removal, provisioning checks and microG download (fake checkout, offline)
./Scripts/Testing/test_install_deps.py      # install-deps.sh per package manager, with fake managers (offline); tools/libraries found without their package, find-host-tools.sh, fetch-sdl3.py's window-system check
./Scripts/Testing/test_rootfs_library_fetch.py # the rootfs library fetchers and install-rootfs.sh --full against a broken or changed mirror (fake curl, offline)
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
remains an environment skip for display/driver tests, SDL tests in a build
without host SDL2, and host scripts whose fetched fixtures or toolchains are
absent; a missing emulator stays a failure. JIT-only cases remain skipped
under `--no-jit`. Host harnesses that link `libbifrost.a` build and link the library of the
configuration under test (`Scripts/Testing/host_library.sh`: the build directory of `BIFROST_EMU`,
headless when run by hand), which the job has already compiled. A failing test's output shows its first
20 lines, the later lines that name a failure or an error, and its last 10. CI uses strict mode so fixture compilation
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

A fixture must not depend on the machine that built or runs it. CI builds with the pinned Bootlin toolchain (GCC 13, musl
1.2.5, `fetch-musl-toolchain.sh --bootlin`) under an account that is not UID 1000; a build with `musl.cc`'s GCC 11 and older musl,
run as UID 1000, hid all three of these:

- Inline asm declares every register it writes, `v8`-`v15` included: the compiler keeps values there across the block.
  Check a recorded hash against more than QEMU, since QEMU reproduces the fixture's own mistake.
- `clone()` in musl 1.2.5 refuses `CLONE_THREAD`, `CLONE_SETTLS` and `CLONE_CHILD_CLEARTID` with `EINVAL`, without a
  syscall. Use `svc` for a raw thread.
- `getuid()` is the host account unless `--android-runtime` is on, while Binder's sender UID is 0 without it. Do not
  compare one with the other.
- A check of something the host may lack exits 77, and only for that cause: a missing Vulkan driver, an SDL video driver
  without Vulkan surfaces or a missing window backend skips; a failure on a host that has them still fails. Do not
  require variables such as `SHELL` or `TERM`.

CI's scheduled and manual `make check-all` runs on Ubuntu 24.04 (glibc 2.39, SDL 2.30, a toolchain linking `--as-needed`)
with no display, GPU or sound, so it uses Mesa's software GL and Vulkan. Every fixture it needs is built from git, so a
stray local `.elf` cannot stand in for a missing source. A run here proves little about it: test a clean clone with
Bootlin's toolchain, as a UID other than 1000, on that system. `xwayland_wire` (needs X11 in the rootfs) and
`rw_iperf3_version`/`rw_coreutils_*` are its optional fixtures.


