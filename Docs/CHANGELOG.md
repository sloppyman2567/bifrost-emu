# Changelog

All notable changes to **bifrost-emu** will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html)
with pre-release version labels (`-alpha.N`, `-beta.N`, `-rc.N`).

Older releases are in the archive, unchanged: [2.0.0-alpha.1](archive/CHANGELOG-2.0.0-alpha.1.md) and [1.x and earlier](archive/CHANGELOG-1.x.md).

## [Unreleased]

### Battery follows the host; usage and network stats services (2026-10-28)

- Battery: the guest now sees the host's `/sys/class/power_supply` (read-only), and `FrameworkBattery` reports the
  machine's real battery, status, charger and plug type, re-read every 30 s with Android's transition broadcasts
  (`ACTION_BATTERY_CHANGED`, power connected/disconnected, low/okay). A host without a battery (a desktop) is a full
  battery on AC mains instead of "absent, unknown status". See [Battery](Android/ANDROID_RUNTIME.md).
- YouTube resumed after a permission result and crashed with a `NullPointerException` in a constructor: the app asks
  for `NetworkStatsManager` (`netstats`) and `UsageStatsManager` (`usagestats`), which were not published, so
  `getSystemService` returned null. Both exist now, modelled on the Android 15 services for one app: standby bucket
  ACTIVE, empty history and traffic, the services' permission gates and argument checks, and
  `TrafficStats.UNSUPPORTED` for kernel counters. `batterystats.takeUidSnapshot` no longer throws.
- Also published, as YouTube looked them up at start: `media_metrics`, `locale`, `legacy_permission` and `auth`
  (see the service table in [the runtime guide](Android/ANDROID_RUNTIME.md)).

### Window shape: Roblox no longer opens near-square (2026-10-28)

- Roblox's window was a near-square 1440x1384 that the app then froze (it is not resizable): its splash declares no
  orientation, so a portrait 720x1280 window was requested and KWin clamped its height to the work area. A new window is
  now asked again for the requested shape at the largest size that fits, apps with no orientation open landscape, and a
  fixed-size app's rotation takes its panel's other shape (landscape 1230x692, portrait 389x692 desktop units on a 1440p
  panel) instead of swapping clamped pixels. Roblox's lobby surface matches its 2460x1384 window. A maximized 21:9 window
  letterboxes the 16:9 panel. See [Window shape](Android/ANDROID_RUNTIME.md). A landscape game was not run.

### Foreground services and notification posting (2026-10-28)

- YouTube played a video and a tap on pause ended the session: the playback service calls `Service.stopForeground`,
  `activity.setServiceForeground` was unsupported and nothing caught the exception (exit 1). Foreground services are now
  modelled on `ActiveServices.setServiceForegroundInnerLocked` (android-15.0.0_r1): `startForegroundService`,
  `startForeground` with the manifest-type and permission checks of the framework's own type policy, `stopForeground`
  (a no-op on a service that is not foreground), `getForegroundServiceType`, `FLAG_FOREGROUND` in
  `getRunningServices`, for services in the app and in child processes. Their notification, and an app's own, go to an
  in-memory store (`enqueueNotificationWithTag`, `cancelNotificationWithTag`, `getAppActiveNotifications`) that drops posts
  as `NotificationManagerService` does. Nothing is shown on the host yet. See
  [Foreground services](Android/ANDROID_RUNTIME.md).

### MediaSession: an app can create and publish its own sessions (2026-10-28)

- YouTube's home screen drew, and a tap then ended the session: the app creates an `android.media.session.MediaSession`
  and `media_session.createSession` was unsupported, so `new MediaSession(...)` threw and nothing caught it (exit 1). The
  `media_session` stand-in only accepted media keys. It now keeps real sessions (`FrameworkMediaSessions`, modelled on
  `MediaSessionRecord` and its `SessionStub`/`ControllerStub`, read from android-15.0.0_r1): an `ISession` and its
  `ISessionController` per session, the state the app publishes, controller callbacks delivered in order on one thread
  as the platform's handler does, a playing position that moves on with the clock, and a controller's transport
  commands, `sendCommand` and media buttons forwarded to the app's `ISessionCallback`. See
  [the service table](Android/ANDROID_RUNTIME.md).
- The next call YouTube made after the session was created, `audio.registerModeDispatcher` (`AudioManager.addOnModeChangedListener`),
  was also unsupported and was also uncaught. It is accepted like the loudness dispatcher (one shared register/unregister
  helper now): a listener is kept until removed or its process dies, and never called, because the audio mode is always
  `MODE_NORMAL` here.
- Checked in `ManagedServiceBatch` (660 checks, JIT and interpreter) through the real `MediaSession`, `MediaController`
  and `VolumeProvider`: metadata, paused and playing state with the moving position, a two-item queue and its title,
  extras and rating type, local and remote volume info, six commands reaching the session callback in order (play,
  skip, seek, a custom action, a rating, a media button), the global-priority flag refused, destruction telling a
  controller (and one registered later), and the 100-session-per-uid limit (checked against `MediaSessionService.createSessionInternal`) with a slot freed by `release()`. Not checked: YouTube
  itself with the fix (the tap that crashed it), playback, other packages' sessions, media key routing to a session,
  and the platform's content-URI grant check on metadata art (not applied: nothing is shared across apps here).

### YouTube gets past native library loading; two exports an app can fail to load without (2026-10-28)

- Tried the YouTube 21.42.247 APKMirror bundle from Downloads (`asgard.py install` accepts it: 35 signed APKs, 37
  ARM64 libraries). Its first run ended in `Application.onCreate` with `UnsatisfiedLinkError: cannot locate symbol
  "ASurfaceControl_createFromWindow" referenced by libgoogle3.so`. Bionic binds an app library's imports at
  `dlopen`, so one missing export fails the whole library even if it is never called. The managed `libandroid.so`
  deliberately leaves window-handle functions out of its forwarders, and nothing provided this one. It is now a
  wrapper in `surface.c`: NULL for a host window handle (creation failed, as the NDK allows), the image's own
  function for a framework Surface (also NULL while `ISurfaceComposer.createConnection` is not served).
- The next symbol was `glGetUniformBlockIndex`, and an audit of every core ES 2.0-3.2 command in gl.xml against the
  GLES rows found a second: `glTexImage3D` (every 3D texture upload). Neither had a GLES row, so the Android
  `libGLESv2.so` never exported them, and any app importing them failed to load. Cause: `glcoverage.py` skipped
  `glGetUniformBlockIndex` because gl.xml gives its `const GLchar *` the length `COMPSIZE()`, which it read as a
  bulk pixel buffer (the skip was printed and unnoticed), and `glTexImage3D` had no ES size entry. Both are fixed
  in the generator, and `glxmlcheck.py` now fails any core ES command without a GLES row (`make generated-check`).
  `test_android_adapter_exports.py` checks the built libraries; it fails on the previous Roblox-era libraries with
  exactly these three exports missing.
- Cause of the class loading failure, found by loading the failing classes in the live loader: `asga` and `aqrn`
  extend `org.apache.http.protocol.HttpRequestHandler`, the Apache HTTP legacy library that Android 9 removed from
  the boot classpath. YouTube declares `<uses-library org.apache.http.legacy required="false">`, and the platform
  puts a declared library's jar in the app's class loader. Bifrost had no shared libraries at all: the image's
  `/system/etc/permissions` was not provisioned and nothing resolved `<uses-library>`.
- Fix, in three layers. Provisioning (`android_framework.py`): the image's five `<library>` declarations
  (`org.apache.http.legacy`, `android.test.base/mock/runner`, `javax.obex`) and their jars are added with pinned
  digests, to a fresh runtime and, like the shell, to an existing one in place (1,053 to 1,063 files; the next
  `asgard run` does it). They are read-only, because ART refuses dex a process could write ("Writable dex file ...
  is not allowed"), and every managed root is checked on reuse. Runtime (`FrameworkSharedLibraries`): reads the
  declarations and resolves an app's `<uses-library>` entries as PackageManagerService does, including adding
  `org.apache.http.legacy` for apps targeting API below 28 (`OrgApacheHttpLegacyUpdater`, android-15.0.0_r1),
  recording `SharedLibraryInfo` objects in `ApplicationInfo.sharedLibraryInfos`: Android 15's `LoadedApk.makePaths`
  keeps only `.apk` entries of `sharedLibraryFiles`, so the jar had to arrive as a library loader. A required library the
  image lacks is logged and the app still starts (the platform would refuse the install).
- Then `ActivityManager.getDeviceConfigurationInfo` (`activity_task`) was the next wall, thrown into YouTube's startup. It
  is answered from the global configuration as `ActivityTaskManagerService` does (touchscreen, keyboard, navigation,
  the input features they imply, `ro.opengles.version`). That configuration now describes the device's input: a
  finger touchscreen, no physical keyboard, no navigation device, as WMS derives from the input devices; the platform
  defaults left all three undefined for every app. `ro.opengles.version` is 3.0 (196608), the floor the managed graphics
  need, with 3.1 and 3.2 hosts still reporting their real version through `glGetString`.
- Result: YouTube loads, creates its Application and Activity and shows its home screen (the maintainer's
  screenshot: search bar, Home, Shorts, Subscriptions and You tabs). A tap then ends the session: the app creates an
  `android.media.session.MediaSession` and the `media_session` stand-in does not implement `createSession`, so the
  exception is uncaught (exit 1). That needs a MediaSession service, not a stub; YouTube is recorded as `title`.
- Checked: 645 `ManagedServiceBatch` checks in both engines (the resolver against fixture declarations, the recorded
  `SharedLibraryInfo` objects, the device configuration), the provisioning tests (install from the image, idempotence,
  tampered and mismatched files, read-only), and Subway Surfers still starting, drawing and showing an ad Activity after
  the configuration change (one run, not played). Not checked: an app other than YouTube that needs a library, the
  system packages (GmsCore, microG), which do not get libraries resolved yet, and playback.

### Four more framework calls apps make are answered (2026-10-28)

- Found by ranking `SERVICE_CALL_UNSUPPORTED` lines across all 606 recorded sessions of 17 apps (the report exists
  since 2026-10-25, so the list is short): `queryIntentReceivers` (Magic Tiles, Umamusume; 29 sessions),
  `notification.getZenMode` (Geometry Jump Lite), `appwidget.notifyProviderInheritance` (Roblox) and
  `activity.getServices` (Magic Tiles). `PackageManager.queryBroadcastReceivers` resolves manifest receivers like
  the activity query (one query, now shared); `getZenMode` reports Do Not Disturb off; `notifyProviderInheritance`
  validates its packages as `AppWidgetServiceImpl` does and records nothing; `getRunningServices` reports the
  app's own created services from the service runtime, with pid, process, started flag, client count and
  times, instead of throwing, so the common "is my service running?" scan gets a true answer.
- Checked in `ManagedServiceBatch` through the real managers (629 checks, JIT): an explicit and a non-matching
  receiver query, the interruption filter, the inheritance cases (null, own package, foreign package), and the
  batch's own started `ProbeService` appearing with its uid, pid, process and flags. Not checked: an app that
  uses them, a receiver query that matches a manifest filter (the batch package has no `<receiver>` filters),
  `getRunningServices` from a child process (not routed), and a foreground service (still unsupported).
- Not added, and why: `ISurfaceComposer` transaction 3, `createConnection()` (5 apps, 60 sessions), hands libgui an
  `ISurfaceComposerClient`; answering it means the SurfaceControl layer model, not a stub.

### Android windows follow an app's pointer icon (2026-10-28)

- An Android app's `PointerIcon` (hand over a link, text beam over an editor, resize arrows, a hidden pointer) now
  changes the host cursor; before, every window showed the desktop's default cursor and a game that hid its pointer
  still showed one. Host input bypasses `ViewRootImpl`, which normally resolves the icon, so after a mouse hover the
  launcher asks the Activity's view tree (`View.onResolvePointerIcon`) and sends the nearest of twelve shapes to the
  window (`FrameworkCursors`, syscall op 24, optional plugin callback `set_cursor`). The SDL3 backend uses SDL system
  cursors. Only hover updates it, an app's own bitmap cursor shows the arrow, and the first failure turns the feature
  off with `APP_CURSOR_UNAVAILABLE`; see [pointer icons](Android/ANDROID_RUNTIME.md#pointer-icons-2026-10-28).
- Checked: the SDL3 backend against a fake SDL3 (create, reuse, hide, show, free before quit), the plugin host and its
  fixture (valid, out-of-range, refused, absent callback), and the Java mapping and send-on-change through a real
  `View` in `ManagedServiceBatch`. Not checked: a game that sets a pointer icon, or a live window.

### Roblox's vertex shaders compile on Mesa (2026-10-27)

- 29 of the 925 shaders in Roblox's `shaders_glsles3.pack` (grass, smooth terrain, mesh vertex shaders) failed on Mesa with
  "could not implicitly convert operands to arithmetic operator", and Roblox logged `failed to create shader GrassVSUnified`.
  Extracting every `#version 300 es` source from the installed pack and compiling it on the host (Mesa 26.2.3, headless EGL)
  reproduced exactly 29 failures, all in one pattern: SPIRV-Cross indexes a flattened uniform array with a `uint` scaled by an
  `int` literal, `CB3[(_500 & 63u) * 1 + 0]`. GLSL ES 3.00 has no implicit int to uint conversion; mobile drivers accept it.
- Not fixable by exposing `GL_EXT_shader_implicit_conversions`: the extension needs ES 3.1 (Khronos registry; Mesa answers
  "unsupported in vertex shader" for a `300 es` shader), and a `310 es` vertex shader does not link with a `300 es` fragment
  shader ("all shaders must use same shading language version", tested on Mesa). Instead `glCompileShader` has a new policy,
  `COMPILE_SHADER`: an ES 3.x source the driver rejects is compiled once more with the operand of each `[E * N + M]` index
  wrapped in `int()` (`ShaderIndexConversion`). If that fails too, the app's own source is restored and compiled, so the
  app sees its own status and log. A shader the driver accepts is never changed.
- Checked: all 29 compile on Mesa with the rewrite (host tool, not the game); `gles_uint_index` fails on the previous binary
  (3 failures) and passes now; `make test-shader-index-conversion` (it caught `#version 300   es` with extra spaces); the
  `gles_*` group passes; `make generated-check` has no errors; the headless build has no warnings. Not checked: Roblox itself
  with the fix, whether the shaders render correctly (compile success only), other drivers, and any shader that fails for a
  different implicit conversion. `glCreateShaderProgramv` and program binaries do not take the retry.

## [2.0.0-alpha.3] — installable runtime, fork and memory fixes, smaller JIT code (2026-10-27)

**2.0.0-alpha.3** collects the changes made after 2.0.0-alpha.2 (2026-10-21): installable Asgard and rootfs,
`bifrost-emu app.apk`, a Debian package, guest `ptrace` and pseudo terminals, fork/exec and memory fixes, less
generated JIT code, an experimental persistent JIT cache, the window stats overlay and more Android service
contracts. The [v2.0 goals](roadmap.md) remain in progress; the WebView, media and ad scope planned for this
milestone is not complete. Older version labels below are historical records.

### Release validation and a CoreMark runner fix (2026-10-27)

- The version is 2.0.0-alpha.3 in `Version.hpp`, the Makefile, the sample config, the Debian package and manual
  pages, the generated rootfs `os-release`, the C API comment and the stats-overlay test.
- `Scripts/Bench/run_benchmarks.py` ran CoreMark for 110,000 iterations, which the current build finishes in 8.4 s,
  under CoreMark's ten-second minimum (the program prints "Must execute for at least 10 secs"). It now runs
  160,000 iterations with `crcfinal` 0xa14c, the value QEMU 11.1.1 prints for that count.
- Measured medians (three interleaved runs, all CRCs valid): 13,156 iterations/s for the shipped build, 12,981
  for the same source built headless, 12,332 for the alpha.2 release commit built headless, 7,710 for QEMU 11.1.1.
  Method and limits: [JIT.md](JIT.md#coremark-checkpoint-2026-10-27). The `bench_*` table in the README was
  rerun the same day.
- `run_tests.sh --strict`: 375/375 with the JIT, 360/360 with `--no-jit` (15 JIT-only tests are not selected
  there). `make verify`: 100 differential fixtures and 10 threaded oracle runs. `make generated-check` passed.
  Not run: `Scripts/Testing/test_window_stats.sh`, the Android package suites (`make test-android-*`), the
  `USE_SDL2=0` headless test suite and `make debug`.

### A clearer stats overlay (2026-10-27)

- The overlay panel has lower case (with descenders), a double-size frame rate, right-aligned details, more line
  spacing and an 85% opaque body with an outline and anti-aliased corners, so dim text stays legible over bright
  scenes. Accented Latin titles print without accents instead of `?`, long names end in an ellipsis, and a title
  the font cannot show is left out. The X11 desktop HUD for Linux guests now draws the same panel instead of its
  own text block.

### Block exits drop two dead register loads (2026-10-26)

- Under chain-skip (the default) no chain target reads RDI/RSI, so exits no longer load them; the MT chain slot,
  the indirect-branch probe and the Tier-2 trial wrapper address the CPU through RBX. `qt_test` emits 95.2 bytes
  per guest instruction instead of 98.6.

### Smaller memory fault exits (2026-10-26)

- A guarded access's fault exit no longer reloads the PC (`jit_fault_step` returns it), loads the block PC only
  for `BIFROST_FAULT_TRACE`, aligns its call with one `push` instead of `pushf`/`popf`, and jumps to a frame
  return already emitted in the block. `qt_test` emits 98.6 bytes per guest instruction instead of 110.2. Block
  exits keep their own frame return: sharing it made the dispatch benchmark 10% slower.

### A fifth less generated code (2026-10-26)

- The checked path of each guarded load/store (run only while software page checks are on) is one compact
  sequence: the page-flag table's address comes from the header page instead of a 10-byte immediate, the flag
  tests are byte-sized and both pages share one check. `qt_test` emits 19% fewer bytes per guest instruction,
  Umamusume's game process 18% fewer (148 to 121).

### Guarded loads and stores no longer test the software-check flag (2026-10-26)

- The header-page bounds table doubles as the gate: Memory writes 0 into it while some page needs software
  checks, so one `cmp`/`jae` per scalar access replaces the bounds compare plus the flag test, and the checked
  path compares with a second table of real bounds. `bench_fib` 10%, `bench_memcpy` 6%, `bench_sort` 5% faster;
  emitted code 0.6% larger (the cold path gained the bound compare). `jit_window_bounds` checks out-of-window
  faults with the gate closed.

### ART apps no longer check page flags on every memory access (2026-10-26)

- ART calls `userfaultfd()` when libart loads, which used to switch every guest load and store in an Android
  app to the software page check for the rest of the process (1.8x slower on a load/store loop). The flag is
  now set only while a userfaultfd-registered page is missing, which for ART means during a compaction.
  Registration refreshes stale MISSING bits from `/proc/self/pagemap` plus page contents (host transparent
  huge pages populate untouched pages), and execute-only pages are readable, as Linux arm64 maps them on a CPU
  without Enhanced PAN (the emulated Cortex-A72). Umamusume ran 150 s and 240 s to its title with the flag
  clear between collections; no in-game frame rate was measured. `[mem] software page checks:` reports the
  state. Regressions in `test_userfaultfd` and `test_mem_guard`.

### Persistent JIT translation cache, experimental and off by default (2026-10-26)

- `--jit-disk-cache` (`BIFROST_JIT_DISK_CACHE=1`, `[jit] disk_cache`) stores Tier-1 translations of file-backed
  code in `~/.cache/bifrost/jit/<app>/<fingerprint>/` and installs them in later launches after checking the guest
  words; `--jit-disk-cache=verify` compares each stored record with a fresh compile. A block is stored only when a
  second compile at shifted guest PCs explains every difference; host addresses must be declared and lie inside what
  they name, and an undeclared one (a constant in mapped host memory) leaves the block uncached. The budget is
  `BIFROST_JIT_DISK_CACHE_MB` (512 per application); a new build prunes older builds' directories first. The cache
  stays off in a directory others can write to or without `flock`. The fingerprint's switch list is generated
  by the build. Measured: `qt_test` starts in about 105 ms from the cache against 185 ms without it, but the
  recording launch takes about 287 ms. In Umamusume, verify mode found every reused record equal to a fresh compile
  across the game and its helper processes (after making the imm64 choice depend on the value alone), and launches
  from the cache install 200,000 to 390,000 blocks instead of compiling them; no startup gain is established yet and
  play past the title from the cache is not yet checked. `test_jit_disk_cache.sh`
  covers it.

### fcntl owner, signal and lease commands: Umamusume no longer ends itself about 8 s in (2026-10-26)

- `fcntl` answered `F_SETOWN`, `F_GETOWN`, `F_SETSIG`, `F_GETSIG`, `F_SETLEASE` and `F_GETLEASE` with `EINVAL`. They now
  follow Linux `fs/fcntl.c` and `fs/locks.c` (v6.12): the owner (`Node::async_owner`) and signal belong to the open
  file description, so a `dup()` shares them; `F_SETOWN` of a pid or group that does not exist is `ESRCH`, `INT_MIN` is
  `EINVAL`, a group reads back negative, and `F_SETSIG` takes 0 to 64. `F_SETLEASE` works on regular files, procfs
  ones included (the memory file of `/proc/<pid>/mem`), and is `EINVAL` on pipes, sockets and directories; releasing a lease
  that is not held is `EAGAIN`; `F_GETLEASE` reads it back (`F_UNLCK` when none). `test_fcntl_owner` covers it
  natively, in the emulator and with `--no-jit`, and fails on the previous binary.
- Limits: nothing is delivered. Leases are recorded and never broken, because the emulator sees no other opener, and a
  read lease conflicts only with this description's own write mode (Linux counts every opener). The owner-permission
  check (`EACCES` for a file the caller does not own) is not made. Kernels from 6.15 refuse leases on procfs files
  (`simple_nosetlease`; the 7.2.8 host does), so `test_fcntl_owner` accepts that refusal natively and requires the
  older behaviour of the Android kernels only when run with the `emu` argument.
- **Why Umamusume ended itself.** The anti-tamper code (lib__4e06__.so unpacks it into anonymous memory) forks a helper
  that inherits the descriptor of `/proc/<app pid>/mem`, which the app opened just before. The helper calls
  `fcntl(fd, F_SETOWN, app pid)`, `F_SETSIG(SIGIO)` and `F_SETLEASE(F_WRLCK)` on it (a lease whose break signal would
  report anyone opening the file) and takes any failure as a detection: by the disassembly the first reported event
  is stored, ends the helper and becomes its exit status (event 142, `0x8e`), and the app ends itself with status 0, or is killed (137),
  about 8 s in. With the commands implemented, the helper no longer exits 142 and the app ran for the whole 90 s of the
  test, loading its WebView and services; no crash, abort or tombstone was logged.
- Checked against sources: the kernel behaviour above (fetched `fs/fcntl.c` and `fs/locks.c`) and, on the host kernel, the
  results of the same calls in a native program. Observed in the emulator only: the call sequence and its `EINVAL`
  (syscall trace), the exit status 142, the layout of the helper's result record (decoded from memory dumps of the
  helper), and that the app survives 90 s. Not established: that the game is playable past the title, that the lease
  step was the only check (the later helpers it forks were not examined), or the meaning of the helper's other event
  ids. The `stat("/proc")` / `stat("/proc/%d")` pair seen in the helper's trace is a `realpath` of the unformatted
  template that fails the same way on Linux; it is not the cause.

### Android audio: a missing libaaudio adapter fails the AudioTrack instead of the process (2026-10-26)

- With Android's own `libaaudio.so` in the process instead of the Bifrost adapter (the batch without `--managed-graphics`),
  `FrameworkAudioTracks.create` opened its host stream through the real library, which plays through `AudioTrack` and so
  back into `AudioFlinger.createTrack` on the same thread: unbounded recursion that ended in a `StackOverflowError` and
  the death of the process (run with the new mode and the old code: two overflow reports, exit -9). `createTrack` now
  refuses a request nested on its own thread (`NO_INIT`, logged as `AUDIOTRACK_REFUSED`), which fails the stream open and
  the app's AudioTrack creation. The normal path is unchanged (`ManagedServiceBatch`, 604 checks, JIT). It was not
  observed in a game: shipped sessions install the adapter. `ManagedServiceBatch` argument `no-audio-adapter` is the
  regression check.

### Android display: the DisplayManager queries apps reach no longer throw (2026-10-25)

- A probe of the display API an app sees found `DisplayManager.getMatchContentFrameRateUserPreference` and five hidden
  queries throwing from the stand-in (`getStableDisplaySize`, the HDR getters, the user preferred mode); the other
  six answered here follow the same AOSP code. Eleven `IDisplayManager` methods now answer as `DisplayManagerService` at `android-15.0.0_r1` does
  on a phone with one built-in panel; the stable size is the natural size of the first host window size. Brightness,
  virtual-display and mode-request calls stay unsupported and are named in the log. The fixed 60 Hz mode and the zero
  vsync offset and presentation deadline are unchanged, and are not host timing.
- `FrameworkDisplayProbe` grew to 70 checks and passes in both engines. This is not a fix for the Umamusume
  transparency, which none of these queries explain.

### Android services: unsupported calls are named in the log (2026-10-25)

- An app call that a service stand-in does not answer (an `UnsupportedOperationException` from a Java service, or a
  transaction a native Binder stand-in refuses) now writes `SERVICE_CALL_UNSUPPORTED <service> <call>` once per process,
  where it used to surface only as an exception in the app or an unknown-transaction status. Opt-in APK tracing was the
  only other way to see which method an app needed. The app's behaviour is unchanged. `FrameworkServices` holds the
  reporter (capped at 256 distinct calls); the seven native stand-ins in `FrameworkNativeServices`, the audio track,
  pending-intent sender and account-response binders report through it.
- `ManagedServiceBatch` (604 checks, real emulator, JIT mode) passes with the new check; its normal run names two
  deliberate limits, `uimode setApplicationNightMode` and `activity sendIntentSender`. The interpreter-mode run was not repeated.

### Asgard: service processes end with the session supervisor (2026-10-25)

- When a session's supervisor was killed mid-shutdown, the Play services and system helper processes it had started kept
  running: two were still resident 44 minutes after their game crashed (about 490 MB). `ProcessHost.spawn` now passes
  `--asgard-service-parent <supervisor pid>`, the flag the broker and codec helpers already use, so the kernel
  SIGKILLs each child when the spawning thread's process dies (`PR_SET_PDEATHSIG`, then a parent-pid recheck for the race).
  `Scripts/Android/test_asgard_process_host.py` checks the command line, kills a real supervisor and requires the emulator
  child to be gone, and runs a control without the flag to show the check can see a leak.
- `asgard realm stop --force` still ends those service processes: `broker_clients` keeps ignoring parent-bound helpers
  but counts `--service-child` processes as clients.
- `Scripts/Testing/verify_jit.sh` runs `jit_dispatch_cache_growth` (four caller threads and a thread mapping and unmapping
  code) in threaded-oracle mode, like the other concurrent fixtures; shared-memory replay is unsafe for it.

### Graphics thunks: pointer queries return the guest's pointer, not the driver's (2026-10-25)

- The seven commands the new registry guard listed as pending (`glGetPointerv`, `glGetBufferPointerv`,
  `glGetVertexAttribPointerv` and their `EXT`/`KHR`/`OES`/`NV` forms, ten rows) write a pointer into a guest `void *`, and
  the guest was handed the driver's value. Measured with `Tests/Apps/test_gles_pointer_queries.c` before the fix:
  `GL_BUFFER_MAP_POINTER` returned a host heap address (`0x555e8bc8d210`) instead of the pointer `glMapBufferRange` had
  returned (`0x19e70000`), a client vertex array came back as its host-side alias (`0x155e5c3d11d0` for `0x51581d0`), and
  `glGetPointerv(GL_DEBUG_CALLBACK_FUNCTION / _USER_PARAM)` returned 0 because the thunk keeps the callback to itself. A guest
  that compared or dereferenced those values got a host address or NULL. The rows now use a `POINTER_QUERY` policy that
  corrects the written value after the host call; the registry guard's pending list is empty.
- The test passes under the JIT and the interpreter; the unmapped, after-unmap and byte-offset cases already passed. Not
  covered: the legacy desktop `GL_*_ARRAY_POINTER` names of `glGetPointerv` get the same address correction but are not
  tested, and a client array that was bounced instead of aliased (a pointer the guest could not map) cannot be recovered.

### Graphics thunks: a guard against the next untranslated array, and crash reports that name the call (2026-10-25)

- `Tools/CodeGen/glxmlcheck.py` (in `make generated-check`) now fails a GL/GLES thunk row whose `gl.xml` parameter is a
  pointer-to-pointer when the row has no policy. It reports the seven commands (ten rows) whose `void **` parameter
  is an output pointer as PENDING instead (`glGetPointerv`, `glGetBufferPointerv`, `glGetVertexAttribPointerv` and their
  `EXT`/`KHR`/`OES`/`NV` forms): the driver writes a pointer value into guest memory there, which is likely a host
  address for `GL_BUFFER_MAP_POINTER`; whether any of them misbehaves has not been tested. Reverting either of
  the string-array fixes in a copy of the spec fails the check.
- A host fault inside a graphics thunk call now names it: `[host-fault] sig=11 … map=/usr/lib/libc.so.6+0x1b471d
  thunk=glGetUniformIndices`. Finding that call in the Geometry Dash Lite crash took a core dump and gdb. Checked by
  re-breaking the `glGetUniformIndices` row in a build and running `test_gles_string_arrays`; AudioThunk and DisplayThunk
  calls are not marked, and no automated test triggers a fault (that would need a deliberately broken row).

### Graphics thunks: arrays of strings reach the driver as host pointers (2026-10-25)

- A WebView ad in Geometry Dash Lite killed the whole game: Chromium's GPU thread called `glGetUniformIndices`, whose
  `const GLchar* const* uniformNames` the thunk translated only as far as the array itself. Every entry stayed a guest
  address, and the host driver's `strlen` took SIGSEGV on it (core dump: `Chrome_InProcGp`, thunk 1644). The same
  shape was unhandled in the GLES `glTransformFeedbackVaryings` and in `glCreateShaderProgramv` (GL, GLES, `EXT`);
  `glShaderSource` and the GL `glTransformFeedbackVaryings` already had a policy. The GLES `glTransformFeedbackVaryings` row
  now uses the existing `TF_VARYINGS` policy, and the others use a new `STRING_ARRAY` policy that rebuilds the array
  from host string pointers (bouncing a string that is not one accessible host range, any count up to 65,536 instead
  of 64) before the ordinary call, return value and output writeback. The array is read through guest memory.
- `Tests/Apps/test_gles_string_arrays.c` (headless EGL pbuffer, GLES 3): uniform lookup by name against the driver's own
  `glGetActiveUniform`, a missing name, 200 names, a transform feedback varying and a separable program. It crashes
  the previous build with the same `libc` fault (exit 139) and passes under the JIT and the interpreter.
- Checked against `gl.xml`: every command with a `const GLchar *const*` parameter now has a policy
  (`glShaderSource`, `glTransformFeedbackVaryings` and its `EXT` form, `glGetUniformIndices`, `glCreateShaderProgramv` and its
  `EXT` form, and `glCompileShaderIncludeARB`, found by that scan and covered by the same policy but not by the test: Mesa does not
  offer `GL_ARB_shading_language_include`). `glShaderSourceARB` has no row, so a guest cannot call it. Nested arrays of
  other kinds (for example Vulkan) were not examined.

### Android: three platform errors every game logged are gone (2026-10-25)

- Read from a Geometry Dash Lite session (134 error lines): `AudioSystem … Line: 1001 Failed` came from
  `getDeviceConnectionState`, which the `media.audio_policy` stand-in did not answer (it now answers `UNAVAILABLE`: the
  profile has no ports); `AudioTrack: binding to audio service failed` was native `AudioTrack` finding no `audio` service in
  the native ServiceManager (published now, with the calls native `IAudioManager` makes); and `ViewRootImpl:
  SensitiveContentProtectionService shouldn't be null` was the missing Android 15 service (a validating stub: nothing here
  shares the screen). Audio worked throughout; these were failed calls with a fallback. `ManagedServiceBatch` has 14 new
  checks (594 in all), each failing when its fix is removed; the `binding to audio service failed` lines in the batch's own
  log went from 2 to 0. An installed Asgard needs `asgard realm stop` and `sudo make install-asgard` to pick the launcher up.
- Looked at and left: the ad SDKs' own messages (AppLovin, LevelPlay, InMobi, Inneractive), a first-run SQLite table
  message, "Google Play services out of date" (microG reports its real version, and raising it would claim APIs it lacks),
  `OmxInfoBuilder`, the WebView relro file, and no sensors on a PC. The 96 `AconfigFlags` lines come from the missing
  `/system/etc/aconfig_flags.pb`; supplying the real file is not just cosmetic, because the framework then keeps the
  permission elements those flags guard (today every flag-guarded element is skipped), and it changes every app root's hash.

### Terminal: glibc 2.42+ guests see a tty (2026-10-25)

- glibc 2.42 and newer implement `isatty`, `tcgetattr` and `tcsetattr` with `TCGETS2`/`TCSETS2`/`TCSETSW2`/`TCSETSF2`
  (the 44-byte `termios2`), which were unhandled: on a real terminal `isatty()` returned 0 and `tcgetattr` failed with
  `ENOTTY`, so line editing, raw mode and colour detection broke for any guest built on glibc 2.42 or newer, including the
  bundled Arm GNU toolchain and rootfs. They are now forwarded to the host terminal, and the synthetic terminal that
  stdio presents for a redirected stdin/stdout answers `TCGETS2` too. Checked on a pty owned by a test harness against
  `qemu-aarch64` with the bundled glibc 2.42 (dynamic `isatty`/`tcgetattr`) and a glibc 2.44 static program (raw mode,
  `SIGWINCH`, `^C`, `^Z`, `^D`, canonical restore); `Tests/Unit/test_termios2.c` checks the ABI against the same expectations.

### Guests can create and drive their own pseudo terminals (2026-10-25)

- `openpty`, `forkpty`, `posix_openpt` and everything built on them (`script`, `tmux`, `ssh`, Python's `pty`) failed
  inside the guest: `/dev/ptmx` opened, but the pty setup requests were unhandled. The master's `TIOCGPTN`, `TIOCSPTLCK`,
  `TIOCGPTLCK` and `TIOCGPTPEER` (which returns a new descriptor) are now forwarded, with the tty controls programs use on
  them: `TIOCSWINSZ` (also missing for ordinary terminals, so `stty rows` and window resizes from a guest did nothing),
  `TCFLSH`, `TCSBRK`, `TCSBRKP`, `TCXONC`, `TIOCOUTQ`, `TIOCPKT` and the exclusive-mode requests. The value-argument requests
  accept 0 (the old null-pointer check turned `tcflush(fd, TCIFLUSH)` into `EFAULT`). `TIOCSCTTY` is forwarded (a plain
  run's `EPERM` for "not a session leader" counts as success because its session is virtual), `TIOCNOTTY` is never forwarded,
  and a host-identity process answers `TIOCGSID` from the host so it agrees with `getsid`.
- `setsid` is now real in a host-fork child of a plain run, which `forkpty` and every daemon need (it was `EPERM`). The PID-1
  model still answers 1 and the Android runtime keeps `EPERM`.
- Checked: `Tests/Unit/test_pty.c` (41 checks on raw ioctls: lock state, slave number, peer, window size, data both ways,
  flush, drain, packet mode, exclusive mode, errno for non-ptys, a forkpty-style child that leads a session and takes the slave,
  hang-up with `EIO`) gives the same result as a native x86-64 build on this machine's kernel, under the JIT and the
  interpreter. `Tests/Unit/test_forkpty.c` runs the libc view (posix_openpt/grantpt/unlockpt/ptsname_r, raw `tcsetattr`,
  `openpty`, `forkpty` + exec, an interactive `sh` on the pty with its exit status) and passes; the same program built
  against the bundled glibc 2.42 and run dynamically passes too. `qemu-user` lacks `TIOCGPTLCK`/`TIOCGEXCL`, so the kernel
  is the reference for those. Not run: bash, vi, tmux or screen themselves, and SIGWINCH delivery to a guest foreground group
  through a master-side resize (no foreground group exists in the unit test).

### Android buffer images: no per-image RGBA copy, no leaked textures (2026-10-25)

- An `EGL_NATIVE_BUFFER_ANDROID` image (HWUI, WebView, TextureView video) kept a full RGBA copy of its buffer for its
  whole life and converted into it on every bind. RGBA8 buffers now upload straight from the gralloc mapping (a row
  length carries the stride; contexts without one still convert), and other formats convert into one scratch shared by
  all images. A texture of an image destroyed while another context was current was never deleted; it is now freed at
  its context's next bind. Checked by the 11 windowless GL/EGL tests and `test_managed_hardware_canvas` (window, RGBA
  and YUV frames read back). The saving is one frame-sized copy per live image, not measured in a game.
- Found, not changed: four APEX entries (`runtime`, `virt`, `i18n`, `nfcservices`) link to `/system/lib64/libc++.so`,
  which the provisioned system image never carried, so `liblog.so` cannot load for an executable outside the `[art]`
  linker section (Crashlytics' crash helper under `/data/app` prints `libc++.so not found`). Linking it to the ART APEX
  copy fixes it but changes the framework manifest hash, which every prepared app root includes, so each installed app would
  rebuild its root and re-optimise once; left for the next framework provisioning change.
- Found, not reproduced: one Melon Playground session (2026-10-24, an older build) ended after its WebView renderer,
  showing an AdMob ad, logged a Chromium IPC validation error (`VALIDATION_ERROR_UNEXPECTED_STRUCT_HEADER`: a message
  that did not parse) and exited; the app then took a null-pointer SIGSEGV (`fault_addr=0x0 pc=0x724ef774`, last
  syscall 135). It is the only one in the 40 recorded sessions (80,000 log lines); the other frequent errors there are
  fallbacks or app-specific (audio policy queries, a host-driver fence message, missing `SensitiveContentProtectionService`,
  purchases and ads without Play services).

### `[mem]` line: where a process's resident memory is (2026-10-25)

- With `BIFROST_STATS_PERIOD=N` (and at exit with `--verbose`) each emulator process prints one `[mem]` line next to
  the `[JIT]` ones: resident MB split into the guest's own pages (inside the guest window: what the game uses),
  JIT code, host heap, glibc malloc arenas, host thread stacks, other anonymous memory and files, plus the thread
  count (`System/Core/Emulator/MemoryBreakdown.cpp`, from `/proc/self/smaps`; arenas and stacks are recognized by
  size and alignment, so they are approximate). Everything but `guest` is the emulator's. Checked on a code-heavy
  guest and an idle-threads guest (the classes add up to the process's measured RSS).

### Idle guest threads cost 0.15 MiB instead of 1.8 MiB, and the guest sees real RAM (2026-10-25)

- Every guest CPU built a 16,384-entry interpreter decode cache (1.7 MiB) when it was created, writing every entry,
  so each thread held 1.7 MiB resident whether or not it ever interpreted an instruction. Measured with idle
  threads: 100 threads 239 MB before, 76 MB after; 300 threads 594 MB before, 106 MB after (peak RSS of the
  emulator, one core pinned). The cache is now one anonymous mapping, mapped on the first interpreted
  instruction and faulted in page by page (an empty entry is an all-zero tag, the tag holds the complement of the
  PC), and released on `execve`. The interpreter is no faster or slower (two micro-benchmarks, interpreter only,
  within 1%). A game with 363 threads should drop by roughly 640 MB (363 x 1.7 MiB: calculated, not yet measured
  in the game), and each helper runtime by its own thread count.
- `/proc/meminfo` was a fixed fake (16 GiB total, 12 GiB available, no pressure, whatever the host had), so on a
  smaller machine a game, Unity and Chromium sized their caches for 16 GiB and Android's
  `ActivityManager.MemoryInfo` could never report low memory. `/proc/meminfo` and `sysinfo` now share one view
  (`System/Core/GuestMemoryView`): the host's real free and available memory under a total capped at 8 GiB
  (`BIFROST_GUEST_MEMORY_MB`, 0 = no cap). `sysinfo` reports no swap and `mem_unit` 1, as `/proc/meminfo` does. How much this
  trims a game is not measured.
- Tests: `thread_memory` (200 idle threads may add at most 120 MB; the old build adds 352 MB) and `guest_memory`
  (policy edge cases on the host, then both views inside the guest at three caps).

### A full JIT code cache is replaced, and syscalls leave generated code (2026-10-25)

- The JIT never freed or reused generated code, so a long session only grew (Melon Playground: 968 MB of
  code mapping after ten minutes, 4.7 to 5.0 GB for the game process) and a full 1 GiB cache left the process
  in the interpreter for good. When the budget (`BIFROST_JIT_CACHE_MB`, default now 512 MiB) runs out, the
  emulator replaces the whole `FrostJIT`; a reaper frees the old code mapping and per-block metadata once no
  thread can still run it. At most one retired instance may wait to be released, so the worst case, a thread that
  never leaves its generated frames, is twice the budget (the old 1 GiB) and then the old interpreter fallback.
  Non-thunk syscalls now leave the generated block before running (`cpu.jit_svc_pc`), so threads blocked in
  futex, epoll or sleep do not pin it; thunk calls stay inline.
- Measured on a guest that compiles 680 MB of code per pass: peak RSS 949 MB before, 179 MB at a 64 MiB budget,
  138 MB at 32 MiB; the price is recompiling code that no longer fits (20.7 s instead of 7.1 s for that
  guest, a cycle larger than the cache). A syscall costs about 12 ns more; indirect calls are unchanged. In one
  five-minute Melon Playground run (played, default budget) the game held 3.87 GB at 266 s against 4.70 GB in an
  earlier session, the JIT was replaced twice and both old copies were released; not a controlled comparison,
  and hitching was not recorded. `[JIT] generations:` (with `--verbose` or `BIFROST_STATS_PERIOD`) shows whether
  generations are released and how many threads delay it. Details in
  [JIT.md](JIT.md#code-cache-generations-and-syscall-exits-2026-10-25). New `jit_generations` test.

### Wheel, trackpad pinch and middle-drag zoom and pan touch apps (2026-10-25)

- Touch-driven games had no way to zoom or move their view with a mouse. The mouse wheel and a trackpad pinch now
  pinch-zoom around the cursor, Shift+wheel (or a sideways scroll) pans with two fingers, and holding the middle
  button drags both fingers with the cursor. The fingers are synthetic, go down together and lift when the wheel has
  been quiet for 160 ms, so nothing stays pressed; a zoom that runs out of room lifts and starts again, so a long
  scroll keeps zooming. They are off while the left or right button is held. Implemented in
  `Source/System/Android/Window/WheelGestures`; checked by `make test-android-wheel-gestures` and through the SDL3
  backend with a fake SDL3 (`make test-android-sdl-backend`). Not yet tried on a live window.

### `asgard-setup` shows download progress (2026-10-24)

- The setup's downloads (about 1.5 GB, 734 MiB of it the Android 15 system image) printed nothing until they were done.
  Each now shows what it is fetching, how much and how fast: on a terminal one line updates in place
  (`Android 15 system image  [=====>   ]  33%  8.0 MiB/24.0 MiB  6.4 MiB/s  ETA 0:02`), and in a log it prints a line per
  ten percent after two seconds, plus a one-line summary for anything of 1 MiB or more or taking a second. Small, fast
  files (the AOSP header archives) stay quiet. `asgard-setup` also prints a numbered heading for each of its nine steps.
  The helper is `Tools/Android/download_progress.py`, used by the Bionic runtime, AAPT2, signing tools, system image,
  microG and header downloads. Checked with a throttled local server on a pseudo-terminal and piped, and unit tests with
  an injected clock; not run against the real downloads (they are 1.5 GB and the pinned files already exist here).

### `BIFROST_FORK_STUCK_SECONDS`: where a fork child that never execs is blocked (2026-10-24)

- A game's `BGAd-Normal` fork child was still blocked in `futex_wait`, holding a copy of the game's memory and 1,887
  descriptors, after the fork fixes above were installed, and the game was gone before its stack could be read. With
  `BIFROST_FORK_STUCK_SECONDS=N` a fork child that has not finished an `execve` after N seconds prints its guest PC
  and return address as library+offset, and the host stack of its blocked thread (by signalling itself; it then carries
  on). A long-lived child that never execs is reported once as well. `test_fork_stuck_report` checks it.

### Helpers that leave an app's process group no longer outlive it (2026-10-24)

- A forked helper that calls `setsid` is not in the app's process group, so ending the session missed it: after a
  Subway Surfers session, seven processes (several holding a copy of the game, swapped out) were still running hours
  later. The session supervisor now becomes a child subreaper, so such orphans come to it, and ends the emulator
  processes among them with the app. The test fails with the old behavior. Processes already orphaned by earlier
  sessions are not found; `asgard realm stop --force` ends those joined to a broker.

### Asgard: a broker left running by an older build is restarted; `realm stop --force` and `realm restart` (2026-10-24)

- After an upgrade, a new app failed at startup with `IllegalArgumentException` in `ServiceManager.addService`: the
  broker from before the upgrade was still running (its program file had been replaced), the protocol number still
  matched, and the new Binder behavior did not. `start` and `run` now compare the broker's program (read through
  `/proc`, which still opens a replaced file) with the emulator being launched. An idle one is restarted; one with
  apps still joined is refused with a message naming `asgard realm stop --force`.
- `realm stop --force` ends the app processes still joined to the broker (SIGTERM, then SIGKILL after 3 s) before
  stopping it; a plain `stop` now warns when apps are joined. `realm restart [--force]` stops and starts it.
  `test_asgard_stale_broker.py` starts real brokers from a deliberately different copy of the emulator.

### Fork children no longer hang or copy the game, and an exec'd child drops the parent's memory (2026-10-24)

- Melon Sandbox's game process alone reached 3.5 GiB, with hung children on top. Its ad SDKs run helper commands from
  Java, which is a host fork of the emulator. The children (`pag_io_N`, `TBBGThread`, `default_npth_th`) never ran:
  each sat in `futex_wait`, holding a copy of the game's memory and about 2,000 descriptors. A stack of a hung child
  (read from a fixture, not from the game) showed two causes. `Memory::shared_futex_word` waited for a read lock on
  the guest memory index, and, with the JIT busy, `FrostJIT::invalidate_range` waited for the block table's lock. A
  host `fork()` copies lock state: a lock another thread held stays held, and a read/write lock also copies its queue of
  waiting writers and hands the lock to one that does not exist in the child.
- `fork_guest` now holds the comm table, futex shards, thread lists, guest descriptor table and guest memory index
  across the fork (all-or-none with backoff, falling back to the old behavior after 3 s), re-initializes the memory
  read/write lock in the child, and drops the inherited JIT there (the child ran interpreted anyway; an exec'd Linux
  image still gets a fresh one).
- `execve` in a fork child wrote zeros through every old mapping: a host buffer as large as each mapping and a private
  copy of every page, just before discarding them. It now drops the pages. A 256 MiB parent's exec'd child held
  262 MiB of private memory and now holds 6 MiB. The old zeroing also wrote through mappings shared with the parent.
- `test_fork_lock_contention`: fork and fork+exec with six kinds of contending threads; the first three forks hung on
  the previous build every time, now none of 200 per run do. `fork_exec_memory` measures the exec'd child from the
  host's `/proc`. 70 of 71 related repo tests pass on a headless build; `sdl_thread` fails identically on the
  unmodified headless build. Not checked: Melon Sandbox itself on this build, and any SDL/GL build of these changes.
  Zombie children the guest never waits for remain (they hold no memory).

### Closing a game window exits with status 0 (2026-10-24)

- Closing an Android game's window ends with the app killing itself (`Process.killProcess`, SIGKILL). The session
  supervisor already recorded that as state `closed` ("Closed normally" in the library), but `bifrost-emu app.apk` and
  `asgard.py run` still returned the raw 137, which a shell reports as a failure and the managed launcher printed as
  "Managed app exited with status 137". A close after `APP_HOST_CLOSED` now returns 0 and prints nothing; the record
  keeps `exit_code` 137. A SIGKILL without the marker, a crash and `--timeout` expiry still return their real
  statuses. Reported as exit code 127; the code paths here only produce 137 for a close, so the 127 itself was not
  reproduced (127 comes from a package or Asgard that cannot be found, `Source/System/Android/Package`).

### Window stats overlay: cleaner layout and real GPU data (2026-10-24)

- The overlay panel is now aligned rows with a meter each for CPU, GPU, VRAM and RAM, thin rules between sections
  and a dim footer, instead of a dense block of text; window size and frame count are gone. Rendered offscreen and
  looked at over bright and dark backgrounds; not yet seen on a live window.
- GPU readings were missing because the GPU was only identified from a GL context current on the presenting thread,
  and the lookup gave up after four frames. It is now found from the DRM node the process holds open (fdinfo), so
  usage, temperature and VRAM appear without a context. amdgpu's own counters give whole-GPU usage and VRAM; other
  drivers fall back to the kernel's per-process `drm-engine-*` times (this process's share). On this machine's AMD GPU
  both usage paths agree (about 20 and 24 percent for a running game against the whole GPU). Other drivers are
  untested on hardware; NVIDIA's proprietary driver is still unsupported (it needs NVML).

### Window stats are opt-in, with an in-window overlay and layout modes (2026-10-24)

- Window stats were on by default and filled the window title. They are now **off**: a window shows only the game's
  own title. `--window-stats` turns them on; `--window-stats-mode 1|2|3` (or `minimal|normal|overlay`) picks a layout
  and turns them on: 1 the title statistics, 2 (default) title and an overlay, 3 the overlay only. Per app:
  `window_stats` and `window_stats_mode` (`asgard.py config`, and the library's profile dialog, whose stats checkbox
  now starts unchecked). `BIFROST_WINDOW_STATS_MODE` picks the layout in the environment without turning stats on.
- The overlay is a small translucent panel inside the Android window's top-left corner, on Wayland and X11: the
  emulator name and version, app title, FPS and frame time, CPU and GPU usage and temperature, VRAM, system and
  emulator RAM, and the CPU and GPU names. It is an ARGB layer through the window backend's popup interface (reserved
  id `0xFFFF0001`), so it needs no GL state and takes no input; it refreshes four times a second.
- Readings come from `/proc`, hwmon, thermal zones and DRM sysfs (new `HostSensors`), once a second. Checked against
  `sensors` and `/proc/meminfo` on one AMD CPU and GPU; Intel, ARM, thermal-zone and other-GPU layouts are tested
  against fixture trees only, and NVIDIA's proprietary driver exposes none of these files, so it shows `--`. The
  overlay was checked as an offscreen rendering and through a recording backend, not yet on a live Wayland or X11
  window. SDL, GLFW and Vulkan windows of Linux guests keep the X11-only HUD (with CPU/GPU load lines added; it and
  the stats window need a desktop session, so those lines were compiled but not run); the embedded panel is
  Android-only. Layout numbers 1, 2, 3 follow the order minimal, normal, overlay.
- The desktop HUD helper is the emulator run again (`/proc/self/exe --window-stats-ui`). A native window no longer
  starts it (it draws its own overlay), and a helper never starts another (`BIFROST_WINDOW_STATS_HELPER`): a test
  binary that linked this code once re-ran itself without end and exhausted the machine. The tests that can reach
  it now run under a process and memory cap.

### Android: packages whose manifest has a backslash install again (2026-10-24)

- Installing Melon Sandbox (an XAPK) stopped with `Invalid \escape: line 1 column 4 (char 3)`. Its manifest has
  `android:pathPattern=".*\.melsave"`, and `android_preflight.xmltree_nodes` decoded AAPT2's `(Raw: "...")` text with
  `json.loads`. AAPT2 prints that value verbatim between the quotes without escaping (`tools/aapt2/Debug.cpp`, AOSP
  `android-15.0.0_r1`), so any regex path pattern, and any value containing a backslash, failed every install. The
  value is now taken literally. `test_android_preflight.py` fails on the old parser with the same error; the real
  package then installs into a scratch state root (2 signed APKs, 22 ARM64 libraries). Installed copies under
  `$PREFIX/share/bifrost/asgard` need `make install-asgard` to pick this up. Not checked: whether the game runs.

### Android: images without a `linker64` interpreter run instead of ending the emulator (2026-10-24)

- A Subway Surfers run on an Ubuntu 25.10 VM ended in Play services with a failure about `linker64` and a program that
  was not Bionic-linked. The log was not available here, so the exact program is unconfirmed. What was reproduced:
  `--android-runtime` and `execve` under it threw `Android runtime bootstrap requires a Bionic linker64 PT_INTERP` for
  every AArch64 image whose PT_INTERP is not `/system/bin/linker64` or the APEX `linker64`, which kills the guest
  process. That covers statically linked Bionic executables and `linker64` itself (`linker64 PROGRAM ARGS`, the idiom
  for running a binary from app data). The kernel runs an image without PT_INTERP itself with `AT_BASE` 0, and
  `linker64` expects exactly that (`__linker_init` locates itself from `AT_PHDR` when `AT_BASE` is 0; AOSP
  `android-15.0.0_r1`). Both loaders now do this; any other interpreter still fails, with an error naming the program
  and the interpreter it asked for.
- `Scripts/Android/test_android_bionic.sh` gains a section (JIT and interpreter): a freestanding static image as the
  bootstrap program, `linker64` as the bootstrap program, and both through `execve` from a Bionic process. It fails on
  the previous binary and passes now; the 12-run bootstrap matrix still passes. The static image has no libc, so this
  tests the loader contract, not a full static Bionic executable.
- Not fixed or checked: Hungry Shark World's black screen on the same VM (no log; host GL/EGL differences are the
  suspect), and whether this is what the VM hit.

### Memory: smaller JIT bookkeeping, small per-thread caches, no Play Store for billing (2026-10-24)

- Battle Cats used 5.25 GiB PSS over five Android runtimes; guest memory was about 1.1 GiB. The rest was mostly JIT
  code (1.4 GB, all live) and live host heap. A new `[JIT] metadata MiB:` stats line attributes the per-block heap.
  `BlockEntry` shrank from 232 to 160 bytes (fields grouped by size, 32-bit code offsets), MT keeps no back-reference
  index, and the per-edge slot map is gone (an edge records its slot). A 29,638-block MT fixture went from 658 to
  468 bytes per block. Per-thread inline dispatch caches start at 16 KiB and grow to the previous 512 KiB only after
  evicting one target per set (the game has ~200 threads). Generated code is unchanged.
- The Play Store companion's billing service is no longer bound (in-app purchases are not offered): it had started a
  ~370 MiB runtime per game; games now see billing as unavailable. Cached helper processes stop after 60 s with no
  service, provider reference or receiver (not yet observed to fire in a game).
- Battle Cats with both: 4.41 GiB peak (runs of different length; indicative). New `jit_dispatch_cache_growth`. Not
  addressed: JIT code size, the game's remaining non-JIT heap (~500 MiB), why the WebView service stays alive.

### `--guest-ld`: threads the emulator creates no longer corrupt the heap (2026-10-24)

- neverball under `--guest-ld` aborted within seconds with glibc "malloc(): unsorted double linked list corrupted"
  (5 of 5 runs). Threads the emulator creates itself (SDL_CreateThread, the audio pump, the SDL filter worker) must
  clear glibc's `__libc_single_threaded`, as `pthread_create` does; the address was only known to the built-in linker,
  so under `--guest-ld` the flag stayed set and malloc ran without its arena locks on two threads. The emulator now
  reads the symbol from the libc the guest `ld.so` mapped (`GuestLibcSymbol`) on first use, and stores exactly the
  char byte (the clone path stored four). Found with a fixture of two threads mallocing at once (crashed 3 of 3 under
  `--guest-ld`, passed under the built-in linker), then `test_dyn_sdl_thread_malloc`; neverball survived 40 s in two
  of three runs afterwards and was closed cleanly in the third. Both full suites pass 358/358. Not checked: other
  games under `--guest-ld`; supertux2 does not start there (its rootfs `libcurl-gnutls` wants glibc 2.43) and
  crashes under the built-in linker as well (not caused by the loader precedence change).

### Debian/Ubuntu package (2026-10-24)

- `debian/` builds `bifrost-emu_2.0.0~alpha.2_amd64.deb` (`make deb`): the binary in `/usr/bin`, the sample config, the
  project changelog, and the rootfs setup scripts in `/usr/share/bifrost/setup` (with the AArch64 `toybox` they install as the
  rootfs shell). `bifrost-emu --setup-rootfs` now also runs from an installed package, using a copy of those scripts in
  `~/.cache/bifrost/setup`. Asgard is not in the package.
- A second package, `bifrost-emu-asgard`, holds Asgard's tooling (the Android scripts, adapter and launcher sources, headers, and the
  GL registry the adapters are generated from) and one command, `asgard-setup`. It copies the tree to `~/.cache/bifrost/setup`,
  downloads and checks the pinned Android pieces, builds the adapters and launcher, provisions ART and the framework, fetches
  microG and runs `install-asgard.py`. The Android system image, ART, the framework and microG are downloaded on first use, not
  shipped. In a clean Ubuntu 25.10 userland both packages installed and `asgard-setup` completed (ART 532 files, framework 1,053
  files, install 1.1 GB, a second install a no-op, `asgard compat` runs); no Android app was run there. `install-asgard.py` no
  longer crashes when `git` is missing or the tree is not a checkout (the revision stamp is optional).
- Built and tested in a clean Ubuntu 25.10 userland (gcc 15.2, SDL 3.2.20, ffmpeg 7.1): the source compiled there for the first
  time; the installed package ran `--version`, an AArch64 `toybox`, `--setup-rootfs` (toolchain download and rootfs build),
  and dynamically linked glibc and musl hello programs, also under `--guest-ld`. Package builds set their own `CXXFLAGS`, which
  replaces the Makefile's default flags, so `debian/rules` passes the project's flags after the hardening ones (and turns LTO
  off). `test_setup_rootfs.sh` covers the packaged-scripts path.

### Android provisioning on a fresh Ubuntu machine (2026-10-24)

- Found by the first run on an Ubuntu 25.10 VM. `fetch-android-package-tools.py` failed with HTTP 404: it asked Google's
  `android-15.0.0_r1` tag for `Tools/linux/lib/d8.jar` and `apksigner.jar`, which live under lowercase `tools/`; this machine
  already had the files, so it never fetched them. With the corrected paths both match their pinned SHA-256 (checked against a
  fresh download into an empty tree).
- `lpunpack` is not packaged by Ubuntu 24.04, 25.10 or 26.04 (only `simg2img`), so ART, framework and media provisioning could
  not run there. `Tools/Android/android_super.py` now unpacks the dynamic partitions (liblp metadata, checksum-verified, linear
  and zero extents). All five partitions of the pinned image are byte-identical to `lpunpack`'s, and ART (532 files), framework
  (1,053) and media (154 libraries) provisioning pass their pinned inventories with `lpunpack` absent from `PATH`.
  `test_android_super.py` covers it with a synthetic image.
- Not an error: `clang`/`lld`/`libsdl3-dev` must be installed for the Android steps (the VM bundle's steps now say so).

### `/proc/<pid>/status` is a complete status file (2026-10-24)

- For a task the guest debugger knows (every process in the fork family, including the app itself), `/proc/<pid>/status`
  was the debugger's eight-field summary (about 150 bytes: no `Threads`, `VmRSS`, signal masks, `Seccomp`, ...). It is now the
  same complete text as `/proc/self/status` with the debugger's name, state, ids, tracer and credentials filled in
  (`Yggdrasil::proc_status_text`). Found while tracing Umamusume's anti-tamper thread, which reads `/proc/<own pid>/status` every
  100 ms and then kills the app (SIGKILL, exit 137, about 10 s in). **This did not stop that kill**: with the full file the
  game still ends itself at the same point, so the check that fails is something else. `test_proc_pid_status` covers the file.
- **Orphans see parent 1.** (The parent pid is taken in the parent before the fork; recording it in the child raced with a parent
  that exited first and failed `test_orphan_ppid` once in a full run.) `getppid()` and `/proc/self/stat` reported the host's adopter (a session manager or subreaper, pid 1283 here)
  once a forked process's parent had died; a guest orphan now sees parent 1, the guest's init, as on Linux and Android
  (`Yggdrasil::process_ids` compares against the parent recorded at start). `test_orphan_ppid` fails on the previous binary.
- **`/proc/self/stat` has real resource fields.** State, CPU times, nice, thread count, start time and resident size come from the
  host process (the guest is one), and `vsize` from the guest's mappings; they were `R` and zeros. `test_proc_pid_status` checks them.
- **`/proc/self/mem` and `/proc/self/pagemap` are the guest's.** `open("/proc/<own pid>/mem")` failed with ENOENT and pagemap reads
  came from the host's page table (`ProcMemoryNode` now serves both over the guest's memory: mem reads and writes it, EIO where
  unmapped; pagemap reports present, file-backed and exclusive flags with the frame number hidden). Umamusume opens both right before
  it starts forking helpers; with them fixed its self-destruct helper (SIGKILL, exit 137) no longer runs and the app instead exits
  with status 0 about 8 s in, after a forked child that stats `/proc` and `/proc/%d` exits with code 142. `test_proc_mem_pagemap`
  passes natively and in the emulator and fails on the previous binary.
- **Umamusume still does not run (fixed 2026-10-26, see the fcntl entry above).** Its anti-tamper code forks a helper, which forks a monitor that kills the helper,
  waits for it to vanish with `kill(helper, 0)`, reads `/proc/<own pid>/stat` and then kills the app. That self-destruct sequence
  only completed after `4dfec242` made `kill(pid, 0)` honest (restoring the old behaviour once let the game run; it was reverted
  because it hangs every `kill -0` polling loop). The status, orphan, `stat` and memory-file fixes above changed the outcome from
  SIGKILL to a clean exit about 8 s in; which environment check the game still fails is unknown. Checks seen before the exit:
  its own library, `/proc/self/maps` and `environ`, the three `libc.so` paths, `/proc/<pid>/status`, `libart.so`, pagemap, `mem`, and a
  forked child that stats `/proc` and `/proc/%d`.

### Rootfs profiles and a game-library rootfs (2026-10-24)

- Named rootfs profiles: `install-rootfs.sh --profile NAME` installs to `<data>/rootfs.d/NAME`; `--rootfs-profile NAME`,
  `BIFROST_ROOTFS_PROFILE` and `[paths] profile` select it for libraries (no path sandbox), `--rootfs-list` shows the
  installed ones, and an unknown profile is an error naming the places searched. `--rootfs`/`BIFROST_ROOT` still wins.
- `--games` fetches the libraries games need (OpenAL, SDL2_ttf, Vorbis, libpng, libjpeg, libcurl, ...) from pinned Debian
  trixie arm64 packages, checked against the `Release`/`Packages.xz` hashes, leaving out the C runtime and the host-backed
  libraries. Motivation: dhewm3 run without the development rootfs crashed at OpenAL setup, because the rootfs found had no
  OpenAL and the emulator's 29-function OpenAL stub left `alcIsExtensionPresent` and others unresolved. With a `games`
  profile dhewm3 ran 120 s under both the built-in linker and `--guest-ld`, with no crash. `test_rootfs_profile.sh` (14
  checks, no network) and the discovery unit test cover profiles; `fetch-game-libs.py` needs the network and is not in
  the suite (its resolver output was checked with `--list`, and the downloaded libraries by the dhewm3 runs).

### `--guest-ld`: emulator-created threads get their TLS from the guest ld.so (2026-10-24)

- A thread the emulator creates itself (SDL_CreateThread, the audio pump, the SDL filter worker) had its TLS laid out
  from the built-in linker's objects, which under `--guest-ld` do not include the libc the guest `ld.so` loaded: the
  block lacked libc's TLS images, so locale-dependent calls died (`strtod` asserted "decimal_len > 0" in an SDL thread).
  `Emulator::allocate_thread_tls_` now calls the guest `ld.so`'s `_dl_allocate_tls` (found by `GuestLibcSymbol`) on a
  throwaway vCPU and uses the block it returns, with the real DTV and TLS images. Without a glibc `ld.so` (musl) it
  keeps the built-in layout. `test_dyn_sdl_thread_malloc` now also calls strtod, ctype and printf in the SDL thread
  (fails with the old layout, passes with both linkers); both full suites pass 358/358; neverball and dhewm3 ran
  without errors under `--guest-ld`. Not covered: musl `--guest-ld` threads, which keep the old layout.

### `bifrost-emu --setup-rootfs` (2026-10-24)

- One command to install the guest rootfs: `bifrost-emu --setup-rootfs [--system|--prefix DIR|--full|--no-fetch|--uninstall]`
  runs the checkout's `install-rootfs.sh` and returns its status (127 when the executable has no checkout beside it).
  The "no rootfs found" message now points to it. It is a front end: the toolchain download and rootfs build
  stay in the scripts. `test_setup_rootfs.sh` covers install, a flag-free run from the installed layout, uninstall
  and the 127 case.

### Host GL thunks beat a discovered rootfs's Mesa (2026-10-24)

- With `./rootfs` next to the executable, `gles_*` and `wayland_*` programs loaded the rootfs's real AArch64
  `libEGL.so.1` instead of the host GL thunk and crashed. The loader fell back to the thunk only when the library
  file was missing or not AArch64; it now prefers the thunk for GL/EGL/GLES/Wayland/gbm/Vulkan libraries found in
  the rootfs or system directories. Program-bundled copies (RUNPATH/RPATH, `LD_LIBRARY_PATH`) and Android system
  libraries are unchanged. Checked with the loader trace on `test_gles_contexts`; the windowed tests were not rerun.

- **Test runner.** `run_tests.sh --filter` no longer reports "relative guest write escaped the root" when the
  sandbox tests were filtered out; the sandbox fixture and its post-checks run only if one of them is selected.

- **glibc thread-stack cache.** Overlapping threads with 8 MiB stacks overflow glibc's stack cache, which frees
  stacks through the versioned `_dl_deallocate_tls@GLIBC_PRIVATE`; the shim overrode only the unversioned name, so
  the real ld-linux version ran and called a NULL `__rtld_free`. Both names now map to the shim
  (`test_dyn_thread_stack_cache`).

- **Shell prompt.** The rootfs `/etc/profile` sets `PS1='\h:\w\$ '`, so toybox `sh` shows the host and working directory
  instead of a bare `$ ` (`\u` printed a blank in toybox, so it is not used). Existing rootfs installs keep the old
  profile until `install-rootfs.sh` is rerun.

### Real-world shell fixes found by differential testing (2026-10-24)

A differential run of BusyBox `ash` scripts against native Linux (and single BusyBox processes against `qemu-aarch64`)
found these; each has a regression test.

- **`mkdir -p` and trailing slashes.** `mkdir a/` and every BusyBox `mkdir -p` failed with ENOENT, because path
  splitting turned `a/` into an empty name; `/` and direct children of `/` (`mkdir -p /tmp/x/y`) failed the same
  way. Trailing slashes are now stripped, with the Linux rules kept where they differ: `unlink("file/")` and
  `rename("file/", ...)` are ENOTDIR, creating a symlink, link or node at `"x/"` is ENOENT.
- **Signals to yourself.** `kill(getpid(), SIGUSR1)`, `kill(0, ...)`, `tgkill` and `tkill` with an installed handler
  returned their first argument (so BusyBox reported "can't kill pid 1") and called the handler with signal 0: the
  result was written after the handler frame had been built. `raise()` and `pthread_kill` were unaffected.

- **The guest's environment.** Without a sandbox the guest always got a fixed environment (`HOME=/root`,
  `TERM=linux`, `PATH=/tmp/aarch64-bin:...`, no custom variables) instead of the caller's. It now gets the caller's
  own, as under qemu-user: `HOME`, `TERM`, `LANG`, `PATH` and anything else (`DEBUG=1 bifrost-emu prog` works,
  `cd $HOME/dir` stops failing, colour and terminal handling follow the real `TERM`). `LD_*`, `BIFROST_*` and `_` are
  not passed, `PATH` and `HOME` get a default only when missing, and `PWD` names the real directory. The block is
  read from `/proc/self/environ`, because sdl2-compat adds `SDL2_COMPAT` and `SDL3_VERSION` to the emulator's own
  environment before `main`. Programs in a `--rootfs` sandbox keep the curated environment.
- **`getcwd` after `cd` through a symlink.** `chdir` recorded its argument text, so `cd link; pwd -P` printed `link`;
  it now records the real directory.
- **`execve` rewritten** (`Process/Exec.cpp`): it ignored its `envp` argument, so `FOO=bar cmd`, `export` and
  `PATH=... cmd` never reached any program started from a shell, and it truncated arguments at 4 KiB. It now passes
  exactly the environment it was given (a NULL `envp` is empty) and arguments up to 128 KiB; follows `#!` scripts
  (BusyBox ran none, "applet not found"); returns ENOENT, EACCES and ENOEXEC like Linux; updates `/proc/self/exe`
  and `/proc/self/maps` for the new program; and gives `AT_RANDOM` real entropy (it was all zeros, so stack
  canaries in every program started from a shell were predictable). The old code answered a **missing** file by
  re-running the current program under the missing name, which made any `execvp` over a `PATH` with an early
  missing directory (`env`, `make`, `xargs`) run the wrong program, and made a non-multi-call program exec itself
  in a loop. That redirect now applies only to existing foreign ELF files and only when the running program is a
  multi-call binary (BusyBox, toybox). Android's exec branch keeps its previous checks.
- **`posix_spawn`, `system()` and `popen()` failed for every C program.** Both libcs start the child with
  `clone(CLONE_VM|CLONE_VFORK)`, which was refused with ENOSYS (musl's plain `vfork()` already worked, glibc's did
  not). It now runs as a copy-on-write fork on the caller's stack. Differences from a real `vfork`, documented in
  the code: the parent is not suspended, and the child's writes are not visible to it, so with glibc a missing
  program returns 0 from `posix_spawn` and the child exits 127 (musl returns ENOENT through its pipe).
- **Resource limits.** `getrlimit` answered "unlimited" for everything and `setrlimit` did nothing: `ulimit -n`
  printed `unlimited`, `ulimit -n 64` had no effect, and anything sizing itself by the descriptor limit saw infinity.
  The guest now has its own limit table seeded from the host's (NOFILE 1048576, STACK 8 MiB, as qemu-user reports),
  inherited by fork and kept across exec; `setrlimit`/`prlimit64` change only that table, validate (EINVAL, EPERM,
  ESRCH) and `RLIMIT_NOFILE` is enforced (EMFILE).
- **`kill(pid, 0)`** succeeded for any pid, so `kill -0 $pid` loops waiting for a process to exit never ended; it now
  reports ESRCH or EPERM like Linux.
- **Programs started by a shell ran about 70 times slower.** A fork child inherits a disabled copy of the parent's
  JIT and `execve` never switched it back on, so every command a shell launched ran in the interpreter: sorting
  100,000 numbers took 150 s through `ash` against 2.2 s run directly (and 119 s with `--no-jit`). `execve` now gives
  the new image a fresh JIT, and the preemption ticker is restarted in a fork child so an exec'd multi-threaded
  program can be preempted. Results are unchanged (a pipeline's checksum matches the host's); Android's exec branch
  and `--no-jit` are unchanged.
- **`FD_CLOEXEC`** was neither tracked nor honoured: descriptors opened with `O_CLOEXEC` (or `F_SETFD`,
  `F_DUPFD_CLOEXEC`, `pipe2`, `dup3`, sockets, `MSG_CMSG_CLOEXEC`...) stayed open in the new program, and
  `F_SETFD` changed the host descriptor shared by every `dup`. The flag is now kept per guest descriptor and
  `execve` closes flagged ones (not in the Android branch).
- **`signalfd`** returned the raw host fd to the guest, so reads failed; it is registered like other descriptors.

### `getcwd` reports the real directory without a sandbox (2026-10-24)

A program run without `BIFROST_ROOT` always saw `/` as its working directory, though its relative paths resolved
against the real one. `ls` worked, but BusyBox's `cd dir` builds `getcwd() + "/dir"` and failed with "can't cd to dir"
(found on a clean Ubuntu VM; toybox's `pwd` printed `/folder` after `cd folder`). The guest cwd now starts as the host cwd
when no sandbox is active; sandboxed programs are unchanged. This long predates the other changes in this release.

### Guest linker with a discovered rootfs (2026-10-24)

`--guest-ld` now works with an installed rootfs found automatically; it used to need an explicit `--rootfs`, which also
sandboxes the program. A library overlay in the VFS answers the guest's read-only opens, stats, access checks and
readlinks of `/lib`, `/lib64`, `/usr/lib`, `/usr/lib64` and `/etc/ld.so.cache` from that rootfs, and only from it: a
host x86-64 library at the same path (CachyOS's `/lib64` is `/usr/lib`) is never offered to the guest, so thunked
libraries such as `libGL.so.1` still get their in-memory stub. Resolution stays inside the rootfs (absolute and `..`
symlinks included); writes, other paths and sandboxed runs are unchanged. The guest interpreter and the thunk-stub
check read through the same opens. The built-in linker remains the default; the guest linker adds about 8 to 15 ms of
startup per run. The rest of this entry is what was checked: see [TESTS.md](archive/TESTS-history.md#guest-linker-with-a-discovered-rootfs-2026-10-24).

### Installable Asgard (2026-10-24)

`Scripts/Setup/install-asgard.py` (and `make install-asgard` / `uninstall-asgard`) installs a self-contained Asgard
(about 1 GiB: scripts, tools, framework and ART images, adapters, managed launcher, microG) under
`<prefix>/share/bifrost/asgard` with an `asgard` command, and `bifrost-emu app.apk` finds it beside its own `bin/`, in
`$XDG_DATA_HOME/bifrost/asgard`, or in `/usr/local` or `/usr/share`. Running the installer again copies only files whose
size, time, mode or link target changed (`--verify` compares contents), replaces each atomically, removes files the
checkout dropped, and reports an interrupted run. An installed Asgard keeps app state in
`~/.local/share/bifrost/android`; `BIFROST_ANDROID_STATE` selects another directory for every layout. Checkout
behavior is unchanged. See [ANDROID_APK.md](Android/ANDROID_APK.md#installing-asgard).

### Running an installed package file skips the installer (2026-10-24)

`asgard.py run FILE` (and so `bifrost-emu FILE`) ran `install` on every launch: signature verification,
`aapt2` inspection and extraction and hashing of every library and asset, only to find at the end that the same
package was already installed. `install` now records the file's SHA-256 (`source_sha256`), and `run` skips the installer
when an installed app with an existing root recorded the same hash. Apps installed earlier are installed once more,
which records it. Reinstalling by `asgard.py install` is unchanged.

### `bifrost-emu app.apk` (2026-10-24)

Giving `bifrost-emu` a `.apk`, `.apkm`, `.apks` or `.xapk` file now installs and runs it through Asgard, like
`asgard.py run`, instead of rejecting it as a non-ELF. Exit status and signals pass through (the process is replaced),
`-d` and `--android-window-plugin` are forwarded, and `BIFROST_ROOT` is dropped. Asgard is found next to the
executable or at `$BIFROST_ASGARD`; a binary installed on its own has no Android tooling and says so. Details in
[ANDROID_APK.md](Android/ANDROID_APK.md).

### Installable rootfs (2026-10-24)

`Scripts/Setup/install-rootfs.sh` builds a standard rootfs (glibc and musl, about 70 MB) and installs it where
discovery finds it: `--user` (default, `~/.local/share/bifrost/rootfs`), `--system`, or `--prefix DIR`;
`--full` adds the real-world libraries and `--uninstall` removes it. It builds beside the destination and renames
into place, so a failed run keeps the old install. `make install-rootfs` / `uninstall-rootfs` do the same under
`$(DESTDIR)$(PREFIX)/share/bifrost/rootfs` without downloading, and `make install` points to them. Layout and search
order are in [ROOTFS.md](ROOTFS.md).

### Dynamic programs start without `--rootfs` (2026-10-24)

`bifrost-emu prog` now finds a rootfs for a dynamic program by itself: `<executable dir>/rootfs`,
`<executable dir>/../share/bifrost/rootfs`, `$XDG_DATA_HOME/bifrost/rootfs` (default
`~/.local/share/bifrost/rootfs`), then `/usr/local/share` and `/usr/share`. A discovered rootfs supplies libraries
only; guest paths still resolve on the host, as with `qemu-aarch64 -L`. `--rootfs`, `BIFROST_ROOT` and
`[paths] rootfs` keep their meaning (library root plus path confinement).

- Before this, a musl program outside a checkout with no `--rootfs` ran into a null-pointer crash after "could not
  find libc.so" (glibc worked only through the checkout's bundled toolchain directory). A missing libc is now a
  fatal error that lists the places searched.
- Any native link failure now stops with its reason (exit 1). The loader used to print "falling back to guest
  ld.so" and carry on, but nothing ran the guest linker (`--guest-ld` is chosen before linking, and
  `BIFROST_NO_NATIVE_DYNLINK` before that), so the unrelocated program crashed with SIGSEGV.
- A `[paths] rootfs` in the config file only changed the launch pre-check; it now reaches `BIFROST_ROOT` like
  `--rootfs`. The pre-check error no longer says only static binaries run.

### Thread exit after the guest unmaps its own stack (2026-10-23)

A detached Bionic thread unmaps its stack and TLS block and then calls `exit`. The exit path still
wrote the musl-layout tid word (`TPIDR_EL0 - 0xa0`) and the registered `clear_child_tid` afterwards.
By then another thread could have mapped the freed range, so the late store either zeroed that
thread's TCB or took a host `SEGV_ACCERR` in `Memory::write` (Hungry Shark World crashed this way
in several runs). `munmap` now records that the calling thread freed those words and exit skips
them. `test_thread_exit_unmapped_tls` reproduces it (QEMU passes; the previous binary host-faults).

### Multithreaded JIT overhead (2026-10-23)

- Once a process runs a second vCPU, native code no longer decrements a shared budget at every back-edge, indirect
  branch and loop iteration. It reads a global preemption epoch (advanced by a 1 ms ticker and by every
  invalidation) and returns to the dispatcher when it moves. Forward static edges need no check at all, MT self-loops
  jump straight to their body, and `BL` calls the callee through a revocable cell instead of the helper.
- Measured against `e7ed1b3c`, the cost of an idle second vCPU fell from +21% to +10% on an arithmetic loop, from
  +9% to 0% on a branchy kernel, from +22% to +5% on recursive calls, from +53% to +14% on a byte copy, from +17% to +6% on
  a mutex loop and from +58% to +22% on `malloc`/`free`; single-threaded speed is unchanged. A two-instruction
  loop still pays +35%.
- Signals and exits still reach a thread spinning in native code (about 1.4 ms in a test fixture); the thread loops
  drain them on each tick.
- Fixed: the `simd_minmax` and `simd_pixel` rows in `run_tests.sh` timed out under `--no-jit`.

### Desktop library redesign (2026-10-23)

- The Asgard game library has a new visual design: icon-forward cards with calm tonal Play/Stop
  buttons, a status chip, a version and install-date line and a "last played / running for / last run
  failed" line; consistent rounded icon tiles (padded launcher bitmaps are cropped to fill; games
  without an icon get a fixed-palette tile with their initial); monochrome glyphs drawn by the
  launcher in the palette's colors instead of whatever icon theme or fallback the system supplies;
  cards that stretch to fill the window; a header with a search field, a sort menu, account and
  install buttons that no longer clips its title or placeholder at narrow widths; styled empty,
  no-match, notice, last-session and drag-and-drop states.
- Fixed: the grid computed its columns from the viewport width before the window's layout, so a
  freshly opened or resized window could show two columns and leave over half of itself empty. The
  empty-state hint is no longer clipped to one line.
- Added: double-click/Enter/Space to play, arrow-key navigation, a context menu, drag-and-drop and
  multi-file installation, a Recently-played sort, remembered window size and sort order, and polling
  that slows down when the window is idle and stops while minimized (it used to start a Python process
  every 1.5 s regardless).
- `android_launcher.build()` now builds the window without the single-instance socket or event loop,
  which `show()` adds; the new offscreen `LauncherLibraryTests` use it. Refactoring `show()` briefly dropped
  an import its activation handler needs; the new process test catches that.

### Pixel and media integer NEON forms (2026-10-23)

- Halving add/subtract (`SHADD/UHADD`, `SRHADD/URHADD`, `SHSUB/UHSUB`), rounding shifts (`SRSHR/URSHR`),
  `RSHRN/RSHRN2`, `CMTST`, `CMGT/CMGE/CMLE/CMLT #0`, `MUL .8b/.16b`, `MLS`, `SABA/UABA` and `REV32/REV16`
  compile to inline SSE sequences instead of stepping the interpreter (about 130 ns each, and the block lost the
  vector cache). Found by profiling the English Umamusume client and sweeping the FP/SIMD space against QEMU.
  A pixel-loop fixture with four such ops falls from 1.28 s to 0.07 s. Encodings are native kinds in
  `native_simd.txt` (1,480 to 2,169 QEMU-checked words); `make test-native-simd-helpers` now checks the helper
  bodies the JIT no longer reaches. New regressions: `jit_simd_pixel`, `jit_simd_round`.
- Still interpreted: the saturating family (`SQADD`, `SQSHRN`, `SQRDMULH`, ...), register rounding shifts and
  `CLS/CLZ`.

### Self-loop layout reconcile (2026-10-23)

- `BIFROST_JIT_VERIFY=1` mis-stored a vector register in a loop with more live vectors than XMM slots (`v28`
  received `v27`'s value) because the single-thread back-edge reconciled the demand-slot layout before its
  patchable `jmp`, so the unpatched slot used by replay ran the reconcile and then an exit written for the old
  layout. The reconcile is now emitted out of line and the slot patched to it. Production runs, where the slot
  is always patched, never reached the bad exit; the previous commit shows the same divergence. A 12-accumulator
  loop is unchanged in speed. New regression: `jit_vec_pressure_loop`.

### Native unsigned halfword min/max (2026-10-23)

- `UMAX`/`UMIN` `.4h`/`.8h` were interpreted one instruction at a time (about 130 ns each, and the
  block could not use the vector cache). They now compile to `pminuw`/`pmaxuw` on SSE4.1 hosts.
  Found by profiling the English Umamusume client and confirmed with a pixel-loop fixture
  (1.38 s to 0.64 s with two of its four interpreted ops removed). New regression:
  `Tests/Unit/jit_simd_minmax.c`.

### Per-process Android compatibility defaults (2026-10-23)

`FrameworkCompatibility` now initializes ART's target SDK and disabled compatibility changes for each app process
before `Application` creation, using the image's `etc/compatconfig/*.xml` metadata (`enableSinceTargetSdk`,
`enableAfterTargetSdk`, disabled-by-default). Main apps and service children use their own `ApplicationInfo`; a child no
longer inherits the game's target SDK. Before, the default delegate enabled every queried change, so microG's API-29
processes got `ENFORCE_READ_ONLY_JAVA_DCL`, which applies only to API 34+ targets.
`FrameworkCompatibilityTests` run real ART VMs targeting API 29, 33, 34 and 35 in both engines. Only the metadata in the
provisioned image is implemented, not `system_server`'s per-package overrides, and no hardware attestation or Google
verdict is supplied. A later approved Japanese Uma restart got a nonempty integrity token from Phonesky with no
writable-DEX exception; that is API success, not acceptance by the game.

### Child service routing (2026-10-23)

Service children now send start, stop, bind, unbind, peek and stop-self requests over the authenticated Binder
controller to the session parent, which alone holds the host process-supervisor capability. The controller checks the
child's key, Binder PID and UID, then resolution checks package, user, export status and permissions (cached service
records included). Isolated callers cannot start or bind other services, and a dead child releases its bindings. This
lets nested clients such as Phonesky call microG's DroidGuard. `FrameworkServiceRoutingTests` run three supervised
processes under real ART and Binder headlessly; real isolated-process lifecycles, sticky restarts and the full
background-execution policy are not covered.

### ART cache scope (2026-10-23)

The compiled-code cache is keyed by its compilation inputs only (`managed/dexopt.json`): framework/ART image identity,
APK contents and location, boot classpath, class-loader context, ART properties, compiler filter and reference profile.
Launcher, microG, media routing and unrelated property changes, `ro.kernel.qemu` included, no longer invalidate it.
Runtime replacement carries verified artifacts forward, older records migrate only when their original key still
matches, and an unchanged failed compile is not retried. The compiler is mocked in the cache tests; a separate
headless app compiled with the real `dex2oat64` ran from the retained odex in JIT and interpreter modes.

### Asgard app profiles and startup visibility (2026-10-23)

- The launcher's app settings open a compatibility profile editor: window stats, Back key, network mode, the bundled
  Android API and an emulator-flag selector (Runtime default, Off, On) with an editable property list.
- `asgard.py config APP property.NAME VALUE` saves per-app Android property overrides for managed apps (at most 91
  UTF-8 bytes, no control characters); they survive upgrades and runtime reprovisioning and rebuild the generated
  runtime at the next start without touching app data.
- `install --allow-downgrade` (`-d`; also `run FILE -d`) installs an older version. The signer must still match and every
  signature and integrity check still runs; saves are kept and are not converted to the older schema.
- A starting session shows its current preparation step (file verification, media, runtime, optimization, launch) with
  a running clock, and Open full log opens the preparation log until the game writes output; Uma had sat in
  preparation for about 108 seconds with a frozen display.

### Managed Activity orientation (2026-10-22)

The top Activity's requested orientation now controls the SDL window in play mode (portrait, landscape, release;
`BEHIND` inherits the next Activity), with display rotation, bounds and configuration published through the resize path.
Fixed-size apps keep their panel dimensions with the axes swapped. Before this, the request was stored and never
applied, which held Uma Global's `Screen.WaitDeviceOrientation` coroutine on its cover art. An approved live run
advanced past the cover art; gameplay and Google sign-in were not tested. Physical sensor rotation and Activity
recreation are not implemented.

### Remote Activity results (2026-10-22)

A remote Activity (microG's sign-in window) returns its result through the authenticated process host as soon as it is
destroyed (`activity_finished`), instead of waiting for its service processes to go idle. Waiting left Global Uma paused
behind a black microG window after cancellation. Process exit remains the fallback for crashes and older launchers.

### Guest ptrace (2026-10-22)

The unconditional `ptrace` EPERM is replaced by cooperative tracing of an emulator's fork family: TRACEME, ATTACH, SEIZE,
INTERRUPT, CONT, DETACH, SINGLESTEP (one interpreted instruction, then back to the JIT), PEEK/POKE, GETSIGINFO,
GETREGSET/SETREGSET for AArch64 GPR, FPSIMD and TLS, EXITKILL, `wait4`/`waitid` stops, and TracerPid in
`/proc/<tid>/status`. No host debugger is attached and no host register state is exposed. Syscall tracing,
fork/clone/exec event options, hardware debug registers and unrelated emulator processes are unsupported;
`BIFROST_PTRACE_TRACE=1` logs requests. `test_ptrace` matches native Linux on the portable checks and runs the AArch64
register and stepping checks in every Bifrost engine. Global's forked self-debugging child (a concrete startup
blocker) now attaches and reaches Application creation; the same run later fails in surface preparation, so
rendering and gameplay are unverified.

### Procfs inode identity (2026-10-22)

Synthetic procfs nodes now report the directory-entry inode from path stat, descriptor stat, relative directory-fd stat,
`AT_EMPTY_PATH` and `statx`, as one file should; path stat returned the host proc inode and a descriptor the backing
memfd's. A loader integrity check in Global compared `/proc/filesystems`' entry inode with `newfstatat`. `/proc/self/exe`
keeps its own path. `test_proc_rewind` passes on native Linux and every Bifrost engine mode; SysFS/DevFS identity and
procfs `d_type` constants are not changed.

### Crash reports name the library (2026-10-21)

- With `--debug fault` (`BIFROST_FAULT_TRACE`), a guest fault now also prints the mapping, file and
  file offset (or the `[anon lo-hi rwx]` range) of the faulting PC and of the return address, and for
  a jump to address 0 the eight instructions before the caller. `BIFROST_TRACE_UNIMPLEMENTED=1` reports each
  syscall that ends in ENOSYS and each ioctl request that ends in ENOTTY, once, with its caller (Binder's
  `BINDER_ENABLE_ONEWAY_SPAM_DETECTION` is one that every app triggers and libbinder ignores). `BIFROST_TRACE_MMAP` lines say who
  called `mmap`. Found with the global Umamusume build, whose loader called a null `__errno`
  pointer because the runtime lacked Android's standard Bionic compatibility links.

### Bionic compatibility paths (2026-10-21)

- Bootstrap, ART and framework provisioning now retain `/system/lib64/libc.so`, `libm.so`
  and `libdl.so` as links to the canonical APEX libraries. Verified cached ART/framework
  roots gain the links and updated inventories; managed roots refresh on their next start.
  Tracing global Umamusume confirmed that restoring these paths resolves `__errno` and
  removes its initial null-call crash. Later startup and gameplay remain unverified.
  See [runtime evidence](Android/ANDROID_RUNTIME.md#bionic-compatibility-paths-2026-10-21).

### Signal queries and fork-child shutdown (2026-10-22)

- Permit read-only SIGKILL/SIGSTOP disposition queries, matching Linux and
  Bionic's spawn requirements. Fork children start with empty private futex
  queues and terminate `exit_group` without tearing down copied host display
  resources. Focused signal/fork fixtures pass against QEMU and all engines;
  existing process/signal/futex/exit checks pass 10/10 in each engine.
  Global Umamusume now gets past the child-exit hang, requests HOME and exits
  127; the reason for that app-requested termination remains unconfirmed.
  See [the trace](Android/ANDROID_RUNTIME.md#global-umamusume-after-the-null-call-fix-2026-10-22).

### Play Games profile creation is a user choice (2026-10-21)

- Play Games sign-in fails with HTTP 404 on `games/v1/players/me` when the account has no Play Games
  profile; microG would create one but its setting is off. `asgard.py google games-profile
  on|off|status` stores the user's choice (default off, with a notice) and every app root gets it
  before its session starts. Nothing changes unless the user turns it on. See
  [Play Games sign-in answers 404](Android/ANDROID_RUNTIME.md#play-games-sign-in-answers-404-2026-10-21).

### AudioTrack playback and shared-memory futexes (2026-10-21)

- Native `AudioTrack` clients play: `media.audio_flinger` implements `createTrack` for linear PCM
  streaming tracks, with the shared control block and ring in a memfd and a native server
  (`audio_track.c`) that feeds the host engine through one AAudio callback stream per track.
  Umamusume's FMOD logged about 60 `AudioTrack` failures and now none. See
  [AudioTrack playback](Android/ANDROID_RUNTIME.md#audiotrack-playback-2026-10-21).
  Not yet: static (shared-buffer) tracks, kernel timestamps, rate changes after creation.
- Fix: a futex on a MAP_SHARED file mapping (memfd, ashmem, file) is now a host futex, so a wake
  through another mapping of the page, in the same process or another, reaches the waiter. The
  in-process table keyed words by guest address and never delivered it (`futex_shared_memfd`).
- `BIFROST_NO_SHARED_FUTEX=1` turns the host-futex path off for A/B tests. `make all` no longer fails to refresh
  the root `bifrost-emu` while a broker has it open (copy, then rename), which had left it stale.
- `media.metrics` is a stub service; native `LogItem::record()` no longer waits 5 s, twice, per track.

## [2.0.0-alpha.2] — Android compatibility and runtime fixes (2026-10-21)

**2.0.0-alpha.2** collects the changes made after 2.0.0-alpha.1 (2026-10-12): Android service
contracts, toasts, window scaling, memory and JIT measurements, and their tests. The
[v2.0 goals](roadmap.md) remain in progress; older version labels below are historical records.

### Toasts, NsdManager and Settings intents (2026-10-21)

- `Toast.show` no longer ends an app: `notification.enqueueTextToast`, `enqueueToast` and
  `cancelToast` follow `NotificationManagerService`'s queue (one toast at a time, 2 s or 3.5 s).
  A text toast is a `TYPE_TOAST` window of the framework layout; a custom toast gets `show`/`hide`.
  Azur Lane's sign-in failure path used to exit here. Seen in the `ManagedDialog` fixture; not
  background-toast policy or animations.
- `servicediscovery` is published (`FrameworkNsd`): `NsdManager` constructs, discovery starts and
  stops and finds nothing (no host mDNS responder), resolve and advertise fail. Uptodown dereferenced
  the missing manager in `onActivityCreated`; it now draws its home screen.
- An `android.settings.*` intent without a component is declined (`START_ABORTED`) for every app,
  not just Play services.
- Test: `ManagedDialog` finishes its `Engine` Activity so the task ends;
  `test_managed_dialog_and_popup_windows` passes again (its earlier timeout was the fixture's).

### JIT code size per guest instruction (2026-10-21, measurement only)

- New `[JIT] code-shape` lines in `BIFROST_STATS_PERIOD` output: guest instructions covered, host bytes
  emitted, and average bytes by block length. Battle Cats at the title screen: 698,327 blocks,
  3.5 M guest instructions, 531 MB emitted: **152 bytes per guest instruction, 5.0 instructions per
  block**; blocks of 1, 2, 4, 8+ instructions average 201, 283, 544 and 1,957 bytes, so the cost is
  per instruction (loads/stores and their cold paths) rather than a fixed per-block overhead. A
  disassembled 3-instruction block (361 bytes) was 40 prologue, 55 body, 75 exit/chain, 109 inline
  software page-check path and 79 fault exit.
- Tried and reverted: a shared page-check helper (`push address; call helper`) in place of the
  inline body shrinks the code but made a tight load/store loop 1.8x slower while the sticky
  software-check flag is set (0.66 s -> 1.20 s; flag clear: 0.36 s both), and sharing identical fault
  tails across a block's accesses saved nothing measurable. Any size reduction must keep the checked
  path as fast as the inline one. The software-check state itself costs 1.8x over the clear state
  (0.36 s vs 0.66 s) and is set by write-only mappings and userfaultfd; whether Android sessions
  stay in it was not measured.

### Large guest mappings are no longer committed when mapped (2026-10-21)

- Fix: `Memory::zero_range_locked` zeroed every reused or hinted anonymous mapping in the low
  4 GiB with `memset`, however large. ART maps its 512 MiB heap (the image's heap maximum)
  at a hinted address, so every runtime process had about 511 MiB resident at once,
  independent of THP. Ranges over 16 MiB are now discarded lazily (`MADV_DONTNEED`), which
  reads as zero and commits only what the guest touches. Battle Cats, 62 s into the title
  screen, four processes: helper runtimes 949 -> 463 MiB and 793 -> 372 MiB PSS; the sum fell
  from 4.9 GiB to 3.7 GiB (the runs are not identical, so the totals are indicative; the
  helper processes' 512 MiB block disappearing is not). New `test_mmap_resident` (fails on the
  old code, passes on the new, JIT and interpreter).
- Measured, not changed: THP-on versus THP-off made no systematic difference
  (`prctl(PR_SET_THP_DISABLE)` run), and the 1 GiB JIT buffer is real code, not slack:
  `[JIT] code-cache used=469 MB` for 560,501 blocks (about 840 bytes per block) with 448 MB
  resident, plus about 317 MB of host heap for block metadata in the same process. Shrinking
  that needs smaller per-block code or cold-block eviction, which is not done.

### GL_VIEWPORT before glViewport reports the drawable, not 640x480 (2026-10-21)

- Fix (not yet run on the game): Battle Cats' `appUpdateDraw` reads `glGetIntegerv(GL_VIEWPORT)`
  every frame and sizes its UI from it (the Help WebView is 10% in and 80% of it).
  `GLStateTracker` answered from its mirror, whose initial value is the invented
  `{0,0,640,480}`, so a context that had not called `glViewport` yet reported 640x480 and
  the WebView came out 512x384 at (64,48) in a 2560x1384 window (`APP_WEBVIEW`). A new
  context's viewport and scissor box are its drawable's size (GL spec), so the tracker now
  answers `GL_VIEWPORT`/`GL_SCISSOR_BOX` only after the app set them and otherwise lets the
  host GL answer. Found by disassembling `libnative-lib.so` (`appUpdateDraw` ->
  `glGetIntegerv(0xBA2)` -> engine size globals) and the Java `newWebView` call.

### Non-resizable apps keep their size and are scaled to the window (2026-10-21)

- Fix (`4e41f771`; seen working on the game 2026-10-27): Battle Cats declares
  `resizeableActivity=false`. Android never resizes such an app: it keeps its
  window size and scales it. Bifrost instead grew the SurfaceView to the maximized
  host window (3440x1384) while the game kept drawing its first 1920x1038 scene,
  which landed in the bottom-left corner on black (user screenshot; the earlier
  2560-wide "left part with a black bar" is the same cause). Now the launcher
  calls `fixWindow` (syscall op 21) for an `ActivityInfo.resizeMode`
  unresizeable app: the SDL window keeps the app's aspect ratio
  (`SDL_SetWindowAspectRatio`), all Wayland layers are scaled by one fit factor
  (`AppFit`: size, position, with centering if the compositor ignores the aspect
  hint), the app's reported size, `eglQuerySurface` and resize events stay
  fixed, and pointer and text-input coordinates are mapped back. The primary
  surface is scaled by SDL; X11 layers are not fitted. Removed the throwaway
  `[TMPVP]` diagnostics from `WindowBackend.cpp`, `GLState.cpp` and
  `GraphicThunk/Dispatch.cpp`.
- Resizable apps: the resize path now also runs
  `ActivityThread.handleConfigurationChanged` and each Activity's
  `onConfigurationChanged`, which an app that declares `configChanges` expects
  instead of a restart. New fixture `ManagedGeometry` (resizable; green and blue
  WebView corner divs, magenta root corners, every readable size) and
  `test_managed_window_geometry`. Maximizing it showed every size, the callback
  and the WebView page (`innerWidth` 1720 at dpr 2) following 3440x1384; the
  automated test checks the initial window only. Roblox and Battle Cats'
  WebView page were not re-run (see `roadmap.md`).

### Complete Android 15 app-ops service (2026-10-21)

- Fix: `appops` implemented only `checkPackage`, one `checkOperation` code and
  `startWatchingActive`; Battle Cats' Tapjoy threw
  `UnsupportedOperationException: appops.checkOperation 43` on a pool thread
  (ops 24 and 43 also hit gms, SpongeBob and Hungry Shark). `FrameworkAppOps`
  now implements all 59 `IAppOpsService` methods of android-15.0.0_r1 with
  AOSP's verification, modes, accounting, watchers and permission checks (see
  `ANDROID_RUNTIME.md`, "Complete IAppOpsService"). New `appOps` section in
  `ManagedServiceBatch` (570 checks in the batch): every op's default mode,
  note/start/finish accounting and callbacks, `SecurityException` for
  privileged calls, `IllegalArgumentException` for bad codes and history
  arguments.
- `appwidget.getInstalledProvidersForProfile` (Roblox threw on it 22 times)
  answers the own profile's empty provider list. The service audit (recorded in
  `ANDROID_RUNTIME.md`) lists the remaining unsupported methods per service;
  `notification.enqueueToast` (Azur Lane) stays unsupported.

### Legacy-start glibc programs run constructors once (2026-10-20)

- Fix: native dynlink ran the main executable's `DT_INIT`/`DT_INIT_ARRAY`
  itself, but a program that imports `__libc_start_main@GLIBC_2.17` (built
  before glibc 2.34, e.g. Geekbench 6 for Linux/AArch64) passes a non-NULL
  `init` that glibc also runs, so every static constructor ran twice and
  Geekbench aborted at startup (`LLVM ERROR: ... registered more than once`).
  The linker now leaves the main object's initializers to such programs,
  keeping the old behavior for glibc 2.34+ and musl starts. Checked against
  glibc 2.42 `csu/libc-start.c` (`init != NULL` legacy path vs `call_init`).
  `Tests/Apps/test_dyn_legacy_start.c` has its own `_start` with a non-NULL
  `init`; it reports two constructor runs without the fix and one with it.

### Faster bitfield moves (2026-10-21)

- `ubfx`/`sbfx`/`ubfiz`/`sbfiz`/`lsl`/`lsr`/`asr`/`uxt*`/`sxt*` (`UBFM`/`SBFM`)
  used `RAX`/`RCX`/`RDX` and built masks with `movabs`: six instructions per
  `ubfx`, and a spill/reload of whatever lived in `RAX`. Now immediate shifts and
  masks in the allocator's register. CoreMark: -4.6% host instructions, -9%
  cycles (IPC 4.0 -> 4.2), 11.4k -> 12.65k iterations/s, CRCs valid. New
  `jit_bfm_extract` test (3.1M checks incl. register pressure; passes under QEMU).
  Other emitters that force fixed registers are untouched and likely have the same
  cost.

### Cheaper Tier-2 branch profiling (2026-10-21)

- With `BIFROST_TIER2=1`, every Tier-1 conditional exit carried a six-instruction
  edge-profile sequence (disabled flag, saturate-at-4096 compare, `inc`). The
  saturation test stayed in the code forever, so each executed branch paid about
  five extra instructions and two extra branches. Now it is `mov r11, imm64;
  inc qword [r11+off]` with 64-bit counters (no wrap, no checks; `lock`ed in MT).
  The `enabled` flag, which only froze counts of promoted blocks, is gone; the
  collector reads lifetime ratios instead of capped ones.
- Measured with `perf stat` on CoreMark (40,000 iterations): Tier-2-on executed
  1,815k host instructions per iteration before, 1,737k after (Tier-1:
  1,576k); CoreMark with Tier-2 on went from about 8,800 to 9,750 iterations/s,
  CRCs validated. Tier-1, the default, is unchanged. Tier-2 still executes about
  10% more than Tier-1 on CoreMark (entry counters, profile increments) and its
  regions measure about 1.0x there, so it is not a speedup on this workload.
  All Tier-2 and MT scripts pass.

### Ad "Install" links open a real store page (2026-10-21)

- Fix: clicking Install on an ad opened the host browser at
  `https://play.google.com/d?id=<package>&inline=true&enifd=…`, the Play Store
  app's in-app deep link, which the web answers with 404. `FrameworkLinks`
  (new, plain Java) rewrites such a link to
  `https://play.google.com/store/apps/details?id=<package>`; other links are
  untouched. Covered by a host-JVM test of the mapping; not rerun with a browser.
  The running app picks it up after a restart (the managed launcher is rebuilt).

### Services a short-lived session published no longer take the app's with them (2026-10-21)

- Fix: Sessions in one Asgard realm share a Binder domain, and each publishes the
  device services. A name registered again replaced the earlier entry, and the
  owner's teardown then erased it, so closing a Play services window (a second
  session) removed the game's `SurfaceFlingerAIDL`, `sensorservice`,
  `SurfaceFlinger` and audio services. Processes the game started afterwards
  (Play services' `ChimeraServiceProvider`, ...) died in `androidx.startup`
  with `Failed to initialize display event receiver status=-19`, and each
  request for the provider waited about 20 s; in Hungry Shark World the Play
  Games sign-in then failed ("4 out of 4 underlying tasks failed"; the link to
  that failure is inferred, not proven). The registry now keeps the registration
  an owner replaced and restores it when the replacing owner goes away.
- `android_service_manager` covers the registry rule (fails without the change,
  115 checks with it). `test_managed_google_activity_window` now runs the app and
  the Google window in one realm broker, as Asgard does, and starts a process
  12 s after a window closes that must receive a Choreographer frame: with the
  previous emulator it fails (`DeadObjectException`), with this one it passes.
  Asgard keeps its broker across apps, so `realm stop` / `realm start` is needed
  to pick up the fix.

### Play services PendingIntents from another process (2026-10-21)

- `startIntentSenderForResult` with a PendingIntent another process created
  used to return `START_CANCELED` (Azur Lane's Play Games sign-in UI arrives
  that way). A sender's Binder now answers a
  transaction outside `IIntentSender` with the Intent, type, flags and owner of
  an Activity sender aimed at Play services, and the caller starts that Activity
  itself through the existing Google window relay, for its own `resultTo` and
  `requestCode`. Other foreign senders and cancelled ones are still refused;
  nothing else of another app's PendingIntent is readable this way. One-shot
  foreign senders are not spent in the creator, and a multi-Intent sender
  yields only its last Intent.
- `test_managed_google_activity_window` now has a `:sender` process service hand
  the app a PendingIntent for `AuthSignInActivity` (result `RESULT_OK
  errorCode=10` through the window) and one for the app's own Activity (refused).
  It passes in 83 s and fails against the previous guest code. A real Roblox or
  Azur Lane sign-in was not run.

### Exit no longer hangs on a blocked SDL thread (2026-10-21)

- Fix: after the guest exited, `Emulator::stop_sdl_threads()` joined SDL guest
  threads with no bound. A thread parked in a host `SDL_CondWait` (nothing woke
  conditions, only semaphores) or any other host wait made the emulator hang
  forever, found when dhewm3 died at a lazy-binding error under `--guest-ld`.
  Shutdown now broadcasts every guest-created `SDL_cond` and waits at most 3 s
  for the rest, then detaches them, prints
  `N SDL thread(s) still blocked in a host call; exiting without joining them`
  and leaves through `_exit` with the guest's exit status (skipping teardown
  they could race). `test_dyn_sdl_exit` (`cond`, `mutex`) hung on the old code
  and exits in 0.35 s / 3.4 s under both linkers; it needs the SDL thunk build.

### Experimental guest dynamic linker (`--guest-ld`) (2026-10-21)

- New `--guest-ld`: a dynamic glibc or musl program runs under the rootfs's
  own `ld.so`, as a kernel exec does, instead of the in-emulator
  `DynamicLinker`. The program loads unrelocated, the interpreter loads from the
  rootfs, and the guest linker does relocation, TLS and constructors, so
  link_map-dependent behavior matches real glibc. The older
  `BIFROST_NO_NATIVE_DYNLINK` debug path (host interpreter, unprotected
  segments, jumps to an unmapped entry) is still in the tree but broken;
  `--guest-ld` is the supported way.
- Thunked libraries (libGL, libEGL, libSDL2, libvulkan, libwayland, libX11,
  ALSA/Pulse/OpenAL, ...) work under `--guest-ld`. They have no rootfs file, so
  an `open` of one that fails with ENOENT returns an in-memory AArch64 shared
  object (`ThunkStub.cpp`) whose functions branch to the existing thunk
  trampolines; the guest `ld.so` maps it like any library, for `DT_NEEDED` and
  `dlopen` alike. A real AArch64 file in the rootfs still wins.
  `test_dyn_thunk_stub` (dynamic glibc, `dlopen("libGL.so.1")`, headless) passes
  under both linkers; it needs a GL-thunk build, like `gl_state`.
- A stub is served only if no real AArch64 library of that name exists in a
  directory the guest `ld.so` searches, and `--guest-ld` forwards
  `LD_LIBRARY_PATH` to the guest (the native linker still ignores it).
  dhewm3 under `--guest-ld` first failed on both: its private libraries were
  not found, then a stub shadowed the rootfs's real `libopenal.so.1`. After the
  fixes it loaded `game/demo_mars_city1` in 21.6 s in a manual windowed run
  (no automated check; no native-linker comparison yet).
- `Scripts/Setup/make-ld-cache.py` writes `rootfs/etc/ld.so.cache` (glibc's
  `ld.so` never reads `ld.so.conf`); `setup-rootfs.sh` and
  `setup-rootfs-all.sh` call it. Output parses under the host's
  `ldconfig -p -C`.
- `run_tests.sh --guest-ld` runs the dynamic suites through it: the 18 dynamic
  and 7 real-world glibc tests pass in JIT and interpreter modes, with the
  default linker unchanged (238 JIT / 230 interpreter + 8 JIT-only skips).
  Geekbench 6.5.0 AArch64 starts in this mode and, in a manual run, passed the
  workloads where the native linker crashed; a full run was not completed.

### Asgard app removal (2026-10-20)

- Fix: the per-app `network offline` setting only changed what Android's
  `ConnectivityManager` reported, so apps that simply opened sockets stayed
  online. It now sets `BIFROST_ANDROID_OFFLINE=1` (new `[android] offline`
  config key), which makes the emulator refuse `connect`/`sendto`/`sendmsg`/
  `sendmmsg` to any non-loopback IP with `ENETUNREACH`, makes the DNS proxy
  fail non-local names, and reports no network. Egress only: inbound traffic to
  a wildcard listener is not filtered. `Tests/Unit/net_offline.c` covers it.
- `asgard.py compat [PACKAGE]` reports the documented compatibility of tested
  apps from `Scripts/Android/compatibility.json` (`playable`, `title`, `runs`,
  `blocked`, else `untested`), each with its evidence and known issues; the
  library's game settings show it. Entries exist for SpongeBob, Azur Lane,
  both Hungry Shark games, Roblox and TrickCal. A test validates the data.
- Per-app settings apply to every start, not only library launches, and have a
  CLI: `asgard.py config PACKAGE [KEY [VALUE]] [--reset]` over `window_stats`,
  `back_key` and the new `network` (`auto`/`offline`). The shared
  `android_settings.py` replaces the library's own copy; the library's settings
  dialog gains a Network choice. Explicit options and exported environment
  variables still win over saved settings.
- `asgard.py --debug CHANNELS` enables named trace channels (`binder`, `futex`,
  `thunk`, `mmap`, ... `all`) over the emulator's `BIFROST_*_TRACE` switches,
  replacing remembering individual variables. A test checks every mapped switch
  against `DebugFlags.hpp`.
- `asgard.py run PATH.apk|.apkm|.apks|.xapk` installs the file, then runs its
  package. Installation reports which provisioning step is missing (runtime,
  adapters, aapt2, apksigner) instead of a missing-file error.
- `asgard.py uninstall PACKAGE` removes an app's install, runtime copies and
  data; `clear-data PACKAGE` resets its saves and settings to a first-run state
  and keeps the install. Both refuse a running app (the session holds the
  package lock), system apps and anything not installed, ask before deleting
  unless given `--yes`, and are available from the library window's game
  settings.
- Uninstalling renames the app out of the library first, then deletes (installed
  code is read-only), and frees the app's UID in `.app-ids.json`.
- `clear-data` removes the package's credential/device storage, ART profiles and
  `Android/data` and `Android/media` directories in both the install root and
  the managed root. It keeps OBBs and shared storage. Android 10+ also clears
  OBBs (per an AOSP installd commit message); Bifrost has no store to fetch
  them again. Whether Android clears `Android/media` was not confirmed against
  AOSP source.

### Google accounts and microG sign-in (2026-10-20)

- The `account` service is now Android 15's `AccountManagerService` (one
  user) instead of a no-account stub: authenticators discovered from
  `android.accounts.AccountAuthenticator` services, bound as the system,
  visibility by signature, permission and stored value, add, remove, confirm,
  update, `getAuthToken` and feature queries, and accounts, tokens, user data
  and visibility stored in one shared file that the session and microG's
  processes update under a lock (`FrameworkAccounts`, `FrameworkAccountStore`).
  microG's `com.google` authenticator, in its own process, answers the session.
- `asgard.py google sign-in|settings|accounts|remove` runs microG's
  `LoginActivity` or settings as a session of the provisioned system app (UID
  19998) and manages the accounts; the library window gains a "Google account…"
  dialog. One sign-in serves every app: `<state>/.google` holds the accounts
  and microG's device registration, imported into each root before launch and
  published after a session that changed them. Google's real sign-in page
  renders in the window; a completed login is not tested (needs credentials).
- New `media_session` service with no sessions. WebView forwards unhandled keys
  to `AudioManager.dispatchMediaKeyEvent`; without the service that threw in
  Chromium's key path and aborted the process (found in the sign-in window).
- Not yet: the credential-grant and account chooser dialogs; see the runtime
  guide.
- Google Activities started by a game (`GoogleSignIn`, `AccountPicker`,
  `AccountManager.addAccount`, token consent) open in a separate window of the
  Play services system app and return their result to the game
  (`FrameworkRemoteActivities`, `android_google.ActivityRelay`); the game's
  `AccountAuthenticatorResponse` is relayed, shared accounts are imported into
  the game's root and its Play services processes restart when they changed.
  The library window shows which game asked while the window is open. A real
  sign-in through a game is not tested (needs credentials).
- JIT: `cmp`, `cmn`, `tst`, `adds` and `subs` no longer save the previous host flags
  before overwriting all four (nine instructions and forced spills each time). A
  byte-count loop is about 29% faster; `jit_flags_overwrite` covers it.
- JIT: load and store pairs (`ldp`/`stp`, integer, W, float and `ldpsw`) now use the
  native guarded memory path instead of the old whole-footprint software check
  (about 7 host instructions per access instead of about 35, a float-struct
  update loop about 18% faster). Pairs complete both loads before writing either
  register so a fault on the second half restarts them correctly; `jit_host_fault`
  tests that, including a pair whose destination is its base register.
- JIT: widening SIMD (S/U ADDL, SUBL, ADDW, SUBW, MLAL, MLSL, MULL and ADDLP,
  ADALP) runs inline instead of through a helper call per instruction. A guest
  loop of them dropped from about 1464 ms to 5.2 ms with QEMU-identical results.
  Found in a Hungry Shark World profile, where the helper held 21% of the samples
  during the post-launch freeze. See the JIT guide.
- JIT: the two weakest game-style loops are fixed. A pointer-chasing list walk went from 0.64x
  to 0.92x of QEMU (1,905 → 1,282 ns/iter) and a 64-byte struct copy from 0.51x to 1.25x (753 →
  316 ns): `ldp q`/`stp q` use the native guarded path, a branch on flags that are still in
  `pstate` tests the bit (or reuses the loaded flags) instead of loading and re-storing NZCV,
  `cmp`/`subs`/`adds` fold immediates, redundant zero-extensions disappear and a flags store no
  longer spills the load address. `jit_branch_flags` and `jit_imm_flags_zext` cover them.
- JIT: LL/SC atomics are native. `ldxr`/`ldaxr`/`stxr`/`stlxr`/`stlr` were C calls that
  locked a mutex and updated a per-address registry; the exclusive monitor is now a
  hashed version table with a bucket lock (`System/Core/ExclusiveMonitor.hpp`) shared by
  the interpreter and the JIT, and the JIT inlines it. A guest `fetch_add` through
  `ldaxr/stlxr` went from 68 to 11 ns (QEMU 18), a CAS 65 to 13, `stlr` 20 to 3.6, the
  game-style atomics kernel 5.8x faster (now 1.3x faster than QEMU), and four threads on
  separate counters 245 to 4.4 ns/op. `jit_llsc_native` covers every form and width
  against QEMU, contended counters and spinlocks, an ABA-prone LIFO and faults.
- Fixed: `clrex` (the assembler's default `D5033F5F`) was decoded as a NOP and the JIT
  dropped even the recognised form, so a store-exclusive after CLREX succeeded. Both
  engines now clear the reservation.
- JIT: scalar FP loads and stores (`ldr`/`str` of B/H/S/D, S/D `ldp`/`stp`) no longer
  disable the XMM register cache for their block, so float and double code keeps its
  values in registers instead of about six memory operations per instruction. Same-machine
  game-style kernels: double math 2.1x, `sqrtf`/`floorf` 1.85x, PCM-to-float mix 1.7x
  faster than before, results identical to QEMU. `jit_fp_ldst_cache` covers the forms
  and `jit_host_fault` the faulting accesses.
- JIT: a conditional branch, CBZ/TBZ, direct `B` or block-limit exit skips the nine-
  instruction NZCV store when the successor overwrites all four flags first (a `cmp`,
  `cmn`, `tst`, `adds`, `subs`, `ands` or `fcmp` after only flag-neutral, fault-free
  instructions). The successor's words are tracked as a block dependency, so patching
  them retires the block. A few percent on compare-heavy loops; `jit_flags_exit_dead`
  covers it. In MT mode a self-loop whose head overwrites the flags no longer commits
  them at an expiry exit.
- Web links: the same link within 30 s opens one browser tab, not one per
  repeat (a Play ad link fired five times).
- Main-thread stall reports: when the UI loop stops for 1.5 s the session log
  gets `APP_MAIN_STALL` lines with the main thread's Java stack, once a second,
  so a hitch can be traced to its code afterwards. Always on, bounded.
- Web links (`https://`, `market://`, Play URLs) that an app starts now open in
  your default browser when you pressed or touched the game window in the last 5
  seconds; otherwise they are unresolved as before, so ads cannot open tabs on
  their own. The host accepts only plain http(s) URLs, one per second, through
  `xdg-open`. Also, microG's "Sync settings" request for the platform Settings
  app no longer ends the session (the start is declined, `START_ABORTED`).
- Hardening: the host side of the Google Activity relay no longer follows
  symlinks in the app's request directory. A malicious app could have made the
  host copy a host file into the Play services root, truncate a host file through
  `done`, or take a path for a package name. Reads use `O_NOFOLLOW` on regular
  files, `done` is replaced rather than written through, and the requesting
  package must be a package name.
- Startup: parsed manifests of immutable archives (WebView provider, app,
  microG, platform resources) and the intent filters derived from them are
  cached in the app's root (`FrameworkParseCache`). A fixture app reaches its
  window at about 4.9 s instead of 8.2 s from its second launch on; the first
  launch is unchanged. See the runtime guide, "Startup".
- Fixed: legacy view animations never advanced, because the window compositor
  drew without setting `AttachInfo.mDrawingTime`. Screens shown by a fragment
  transition (every microG settings page after the first) stayed at the
  animation's first frame: blank content, and blank again after Back.
- Fixed: a manifest broadcast to a receiver in another process of the app threw
  `UnsupportedOperationException`. microG's `LoginActivity` sends one right after
  Google accepts a login, so the first real sign-in ended with "problem
  connecting to Google services" (the account was stored). The receiver now runs
  in that process, started by the session, and the process stays cached; the
  delivered Intent also names the receiver component, as `handleReceiver` needs.

### Keyboard text input (2026-10-20)

- Text typed on the host reaches the focused Android editor through its
  `InputConnection`: committed text (`commitText`) and the desktop input
  method's composition (`setComposingText`), so layouts, dead keys, accents
  and CJK input work without any key mapping. An editor with a live
  `InputConnection` is bound to a host `IInputMethodSession` (no input
  channel; key events still reach the views); other editors keep `NO_IME`.
  The SDL3 window starts the host's text input for it (`SDL_StartTextInput`).
  A key press that SDL followed with text is delivered as that text only, not
  also as a key event, which would type every character twice.
- The virtual keyboard has Android 15's `Virtual.kcm` (type `FULL`, US
  QWERTY) instead of an empty map. With the empty map `TextKeyListener` chose
  its `NullKeyListener`, so Backspace, Delete and the arrow keys did nothing in
  an `EditText`, and `KeyEvent.getUnicodeChar()` was always 0.
  `Tools/Android/generate-key-layout.py` generates the table from the pinned
  file with libinput's parser rules.
- The window plugin ABI gains optional `set_text_input`/`poll_text` callbacks
  and a `BIFROST_WINDOW_TEXT` event (append-only; existing plugins keep
  loading). `BIFROST_TEXT_SCRIPT` pushes scripted key and text events into
  the built-in window's SDL queue for tests. No on-screen keyboard exists, and
  the composition is anchored at the bottom of the window until apps report a
  caret position.

### AAudio buffer capacity and NDK exports (2026-10-20)

- `AAudioStreamBuilder_setBufferCapacityInFrames` sizes the host stream ring
  to the request (up to 262144 frames; `UNAVAILABLE` above) instead of
  failing a request above the default 16384 frames. AudioTrack sizes its
  buffer to the request, so AAudio streams normally get at least what they ask
  for. Roblox's FMOD probes a stream, then reopens it with 32768 frames; the
  refusal made `System::init` fail with `FMOD_ERR_OUTPUT_INIT` (51, "no audio
  driver"). FMOD now initializes and Roblox's audio plays (user-confirmed).
- The six `libaaudio` exports of Android 15's `libaaudio.map.txt` that were
  missing are added: `AAudio_getMMapPolicy`/`setMMapPolicy` (UNSPECIFIED until
  set, only the four policies accepted), `AAudioStream_getContentType` and
  `getHardwareSampleRate`/`ChannelCount`/`Format` (the host device the engine
  opened). `AAudioStream_read` was registered but had no handler; it now
  validates its arguments and answers `UNIMPLEMENTED` on output streams, as
  `AudioStream::read` does. Roblox's FMOD never looked these up; they close
  the gap to the NDK table.
- `BIFROST_AUDIO_TRACE=1` traces only the audio thunks, with x0-x3 and the
  result of each call (`BIFROST_THUNK_TRACE` also traces the GL stream).

### Bluetooth SCO route (2026-10-19)

- `audio.isBluetoothScoOn` answers false, like `AudioService` on a device
  without Bluetooth. FMOD asks it before choosing low-latency output; Roblox
  logged a JNI exception from it when joining a game.

### Play services sign-in senders (2026-10-19)

- `startIntentSenderForResult` works for the app's own PendingIntents
  (`startActivityIntentSender`, as `PendingIntentRecord.sendInner`). One
  created by another process, such as Play services' sign-in UI, is reported
  cancelled, so the caller gets `SendIntentException` and continues. Roblox's
  login screen (Credential Manager) and Azur Lane's Play Games sign-in ended
  the app instead. Showing other processes' activities is on the roadmap.

### GL buffer mappings and framebuffer blits (2026-10-19)

- Mapped GL buffers reach the GPU. A guest mapping now mirrors a real host
  mapping of the same range; writes were pushed with `glBufferSubData` into
  whatever buffer was bound, which immutable `glBufferStorage` buffers reject.
  Roblox drew an all-black screen this way; its UI now renders. Unmap ends
  persistent mappings too, and `glFlushMappedBufferRange` offsets are relative
  to the mapping, as GL specifies.
- The desktop GL `glBlitFramebuffer` and `glBlitFramebufferEXT` rows read 8
  of their 10 arguments, so `mask` and `filter` (on the stack) were garbage.
  `glxmlcheck` now rejects rows that drop stack-passed arguments.
- `BIFROST_FRAME_TRACE` samples the back buffer before each traced swap and
  lists the frame's GL calls and `glGetError` results.

### Roblox main Activity (2026-10-19)

- `activity_task.registerScreenCaptureObserver`/`unregisterScreenCaptureObserver`
  (`DETECT_SCREEN_CAPTURE`, kept on the Activity record; no screenshot is
  ever detected). Roblox's main Activity resumes and reaches its landing and
  login screens.

### LSE compare-and-swap pairs (2026-10-19)

- CASP/CASPA/CASPL/CASPAL (part of the advertised FEAT_LSE) were undecodable.
  Roblox's BLAKE3 patch check ran CASPL on its startup thread.

### Process-directed signals and worker faults (2026-10-19)

- A host signal or `kill()` of the own process goes to a thread that does not
  block it, or stays pending for the process until a thread unblocks it or
  `sigwait`s for it (Linux `complete_signal`). It used to wait on whichever
  thread drained it, so ART's SIGQUIT thread dump never ran. `rt_sigpending`
  reads its size argument from the right register and reports process-wide
  signals; a fork child starts with none pending.
- A worker or SDL thread whose synchronous fault (SIGILL, SIGSEGV) is caught by
  a handler keeps running. It ended silently once the handler frame was set
  up; Roblox's startup thread vanished this way and its main thread waited
  forever.

### Vector UCVTF rounding and DUP flags (2026-10-19)

- JIT vector `UCVTF` (32-bit lanes) rounds once, as Arm specifies. It
  converted `x>>1` and `x&1` separately and rounded twice, so some odd inputs
  came out one ULP low (`0x324500a1` gave `0x4e491402`, not `0x4e491403`).
- JIT `DUP` (general) of 8/16/32-bit elements commits a preceding
  comparison's lazy NZCV before its shift chain overwrites the host flags;
  a following `CSET`/`B.cond` read the wrong condition.

### TextureView and video frames (2026-10-19)

- Android apps can sample gralloc buffers as GPU textures
  (`EGL_NATIVE_BUFFER_ANDROID` images), which TextureView and SurfaceTexture
  video use. A Fyber video ad in Hungry Shark World had crashed the game
  ("Invalid GrBackendTexture"). RGBA and YUV 4:2:0 frames convert with the
  buffer's color matrix and range.

### Hardware window layers after close (2026-10-19)

- A hardware-drawn (HWUI) Android window's host layer now goes away when the
  window closes. Before, it stayed on screen: closing a rewarded ad in Hungry
  Shark World left the ad's end card covering the game, which was still
  running. Host surfaces are now owned only by their framework preparation
  and EGL surfaces. App `ANativeWindow` references are no longer mirrored,
  because HWUI releases its window through libnativewindow, which the adapter
  cannot see.

### Multi-touch input (2026-10-18)

- Android apps receive multi-pointer touch gestures (`ACTION_POINTER_DOWN/UP`
  with every active pointer), assigned as InputReader does. Managed apps get
  up to eight pointers per `MotionEvent`, previously only the first.
- The right mouse button is a second finger held where it was pressed, so
  two-finger controls work by mouse (steer with the left button while
  holding boost in Hungry Shark World). Touchscreen fingers are no longer
  also delivered as SDL's synthetic mouse events.

### Shared EGL display lifetime (2026-10-18)

- `eglInitialize`/`eglTerminate` are reference-counted per display, as in
  Android's libEGL. Closing a rewarded ad (HWUI terminates the display with
  its last hardware window) no longer terminates Unity's display, which had
  left Hungry Shark World showing the ad's last frame
  (`EGL_NOT_INITIALIZED` on every Unity surface call).

### Binder realm address space (2026-10-18)

- A shared Binder realm served at most eight endpoints: each broker mirror
  reserved the largest guest window, exhausting the host's 47-bit user
  space. Realm processes and mirrors now use a 512 GiB (39-bit) guest
  address space, so the broker reaches its 128-endpoint limit. Hungry Shark
  World's Play Store process no longer dies opening `/dev/binder`
  (`ECONNREFUSED`), which had stalled startup at the age-signals check.

### Audio player tracking (2026-10-18)

- `IAudioService` player tracking (`trackPlayer` with unique piids, plus
  the one-way player state reports and `releasePlayer`). Creating a
  `MediaPlayer`, as Google Mobile Ads' video view does in Hungry Shark
  World, no longer ends the app.

### Roblox startup (2026-10-17)

- The installer accepts APK Signature Scheme v3.1 key rotation (records the
  signer covering API 35) and launchers declared only by an `activity-alias`.
- A launcher Activity that starts another and finishes (Roblox's splash)
  no longer ends the app; the started Activity becomes the task's root.
  Activity teardown cleans up only a `ContextImpl` base context, as AOSP
  does, so apps that wrap it no longer crash the launcher.
- `appwidget` service (`getAppWidgetIds`) and `FEATURE_APP_WIDGETS`.
- `config.disable_cameraservice=true` describes the camera-less device:
  libcamera2ndk no longer polls for `media.camera` forever.
- Roblox reaches its main Activity; its main thread then blocks (under
  investigation).

### Per-context GL state (2026-10-17)

- GraphicThunk keeps one GL state tracker per EGL context, selected by each
  thread's `eglMakeCurrent`. A shared tracker mixed the bindings of the
  app's, HWUI's and WebView's contexts, so an unpack-buffer upload at offset
  0 was treated as a NULL client pointer and the host driver crashed
  (Hungry Shark World's ad window froze black). `gles_contexts` covers it.
- A fatal host SIGSEGV/SIGBUS outside guest accesses now prints one
  `[host-fault]` line (signal, address, host PC and its mapping) before the
  process dies with the original signal.

### Local Google Mobile Ads requests (2026-10-17)

- microG's `AdRequestService`, which answers every ad request "Not
  supported", is no longer bound. Google Mobile Ads falls back to its own
  request path, as without Play services; Hungry Shark World's rewarded
  video ads now load.
- `IAudioService` loudness methods follow Android 15's LoudnessCodecHelper
  for the built-in speaker (CTA-2075 "small" AAC parameters), so
  ExoPlayer's `LoudnessCodecController` no longer fails ad video precaching.
  The `IAudioService` Spatializer queries report no spatializer
  (`STATE_NOT_SUPPORTED`). ExoPlayer still lacks Java `AudioTrack`, so the
  ad plays in its WebView (its black window: see Per-context GL state).

### Nested Binder callbacks (2026-10-17)

- A synchronous Binder transaction sent while serving one now goes to the
  waiting caller thread on the sender chain, as Linux's transaction stack
  routes it, instead of the target's pool. Google Mobile Ads deadlocked
  with microG's ad service (a pool thread blocked on a lock the waiting
  caller held) and every ad request timed out after 60 s; requests now
  complete with microG's "Not supported" answer. Calls queued for a thread
  fail with `BR_DEAD_REPLY` when it exits or its endpoint closes.

### App-ops active watchers (2026-10-17)

- `appops.startWatchingActive`/`stopWatchingActive` follow AOSP 15
  AppOpsService (op range check, callback registry). Google's ads SDK
  (play-services-ads 24.4.0) called it on an `AdWorker` thread and Hungry
  Shark World died with UnsupportedOperationException; a 150 s JIT run now
  has no fatal exception. No op becomes active here, so watchers are never
  called. AppLovin's `package.resolveIntent` remains unsupported (caught by
  the SDK).

### GPU-rendered Activity windows (2026-10-17)

- Hardware-accelerated Activity windows draw through their own HWUI
  `ThreadedRenderer` into a host layer instead of a main-thread software
  bitmap; SurfaceViews are restacked below their window (host op 19).
  Software popups redraw only ViewRootImpl's dirty region.
- HWUI runs on the GL thunk: a libGLESv1_CM adapter (libhwui binds
  `glGetString` there first), SurfaceFlinger display queries for HWUI's
  `DeviceInfo`, and the Android-private `eglBeginFrame` in the EGL adapter.
- Add `test_managed_hardware_canvas` (HWUI on a SurfaceView, and an Activity
  window whose frame metrics report GPU-rendered frames).

### microG processes and remote content providers (2026-10-17)

- microG runs in its own managed processes (GmsCore UID 19998, its
  `:persistent` process, the Play Store companion UID 19999). Content
  providers in other processes are started and acquired as Android 15's
  `ContentProviderHelper` does, with permission checks and connection
  reference counts. SpongeBob no longer crashes in `GoogleApiHandler` on
  `activity.getContentProvider`.
- Child processes install their process's providers before
  `Application.onCreate`, which they now run. Implicit service Intents
  resolve through manifest intent filters, so Play services SDKs bind
  microG services by action.
- microG sees the calling app's package, uid and grants. System packages
  hold the signature permissions of same-signer packages, which Firebase's
  Play services permission check requires.
- `test_managed_play_services_available` also binds microG's measurement
  service, queries its GServices provider and runs basement's chimera lookup.

### microG Play services packages (2026-10-16)

- Managed app roots carry microG v0.3.17.252432 (`com.google.android.gms`,
  `com.android.vending`) as privileged system apps, pinned by SHA-256 from
  microG's signed release (`Tools/Android/android_microg.py`). UIDs 19998 and
  19999 are reserved. Their archives are parsed on first lookup, so apps
  that never query Play services pay no startup cost.
- PackageManager reports their `fake-signature` certificates in
  `signatures` and `signingInfo`, following microG's signature-spoofing
  rule, and resolves system packages by UID. Google's own Play services
  check now returns SUCCESS instead of SERVICE_INVALID ("Play Store is
  missing"). microG's services, providers and broadcasts did not run yet.
- Add `test_managed_play_services_available` (Google `play-services-basement`
  18.12.0) and `test_android_framework.py --microg`.

### Synchronous faults cannot be blocked or ignored (2026-10-16)

- Memory faults (SIGSEGV/SIGBUS), undefined instructions (SIGILL) and a bad
  `rt_sigreturn` frame now use `force_signal` in the interpreter, JIT
  helpers, fault handler and every run loop. A blocked or ignored fault
  signal reverts to the default and ends the process, as Linux
  `force_sig_fault` does. Before, a blocked fault was queued and the
  instruction re-faulted forever; an ignored one was dropped.
- Add three cases to `test_group_exit`.

### BRK raises SIGTRAP (2026-10-16)

- `BRK` delivers a forced SIGTRAP (`TRAP_BRKPT`, `si_addr` and saved pc at
  the BRK), as Linux `do_el0_brk64` does. A handler can resume; a blocked or
  ignored SIGTRAP reverts to the default, which ends the whole process by
  SIGTRAP. Before, BRK stopped only the trapping thread with exit 133, so a
  worker's `__builtin_trap` or musl `a_crash` left the process running.
- Add `force_signal` (Linux `force_sig_info_to_task`) and three BRK cases to
  `test_group_exit`.

### Rootfs cwd, signal deaths, thread names and input times (2026-10-16)

- A guest whose executable lives inside `BIFROST_ROOT` starts with its cwd
  in the root (the launch directory when that is inside, otherwise `/`), as
  after `chroot` and `chdir("/")`. Its relative paths followed the host cwd,
  so an Android game wrote `arm64/base.art` into the directory bifrost-emu
  was started from. A relative `BIFROST_ROOT` becomes absolute, and
  emulator output paths (`--fb-dump`, `--audio-dump`, `BIFROST_XCAP`,
  `BIFROST_XSEQLOG`, `BIFROST_WRITE_TRACE`, `BIFROST_*_PROFILE`, the Binder
  broker socket) stay relative to the launch directory. An executable
  outside the root keeps the host cwd, as before.
- A process ended by a fatal guest signal now dies by that host signal
  (QEMU's `die_with_signal`, without a host core file), so its parent sees
  a signal death instead of exit status 128+n. `run()` still returns 128+n
  to library embedders.
- Task names are per thread. `PR_SET_NAME`/`PR_GET_NAME` name the calling
  thread, threads inherit their creator's name, `/proc/self/comm` is the
  leader's, and `/proc/self/task/<tid>/comm` and `/proc/thread-self/comm`
  read and rename one guest thread (Bionic's `pthread_setname_np` for other
  threads). Renames also name the host thread for host profilers. `execve`
  resets the name to the new program's basename.
- Native `AKeyEvent`/`AMotionEvent` time getters return `System.nanoTime()`
  nanoseconds, not milliseconds. The managed launcher divided those values by
  10^6, so framework MotionEvents and KeyEvents had times near zero. SDL
  event timestamps are rebased from SDL's clock onto `CLOCK_MONOTONIC`.
- Extend `test_sandbox` (`--in-root`), `test_group_exit`, `test_prctl` and
  `test-android-window-plugin`.

### GL extension entry points from the registry (2026-10-16)

- The host advertised extensions with 241 entry points that the thunk resolved
  to NULL (on Mesa 26.2, Radeon). `glcoverage.py` now generates rows for
  registry extension commands: aliases and promoted commands copy the core row
  (policy included), others get generic rows. 2,266 rows in total.
- OpenGL ES contexts hide extensions with commands that cannot be thunked
  (host fds, untyped data buffers, callbacks), so advertised functions
  resolve. Desktop GL keeps the host list.
- Update the vendored `gl.xml` from 2018 to Khronos commit `6af574a`.
- Graphics symbol ids gain a second bank (8192 entries); registration had
  reached the old 4096 limit.
- Add `gles_extensions`.

### OES_EGL_image entry points for GLES guests (2026-10-16)

- Add thunk rows for `glEGLImageTargetTexture2DOES` and
  `glEGLImageTargetRenderbufferStorageOES`, and resolve host GL/GLES symbols
  that glvnd does not export through host `eglGetProcAddress`. The guest got
  NULL for these advertised functions; SpongeBob's WebView GPU thread called
  it. Once worker faults became guest signals, Chromium's crash handler ended
  the game at 22% loading (`exit_group(191)`).
- Add `gles_egl_image` (EGLImage texture sharing with readback).

### Install-time dexopt and ART profiles for managed apps (2026-10-16)

- Before launch, compile the base APK with the image's `profman` and
  `dex2oat64`, as ART Service does: `verify` until a profile exists, then
  `speed-profile` with an app image. Previously all app Java was verified and
  interpreted at runtime (SpongeBob ships 68 MB of dex).
- Create installd's profile directories and register app code paths with ART,
  so ProfileSaver and androidx ProfileInstaller record profiles.
- Worker threads now receive faults raised outside generated code
  (interpreted instructions, instruction fetch from an unmapped PC) as guest
  signals, in both engines. Previously
  the thread ended silently, and ART's thread suspension then waited on it or
  aborted. Add `test_thread_fault_signal` (checked on QEMU).

### Inline Select, Dup, ADDV, MLA and FRINT (2026-10-16)

- Compile BSL/BIT/BIF, DUP (element), ADDV, MLA and FRINTN/M/P/Z inline
  instead of calling C++ helpers that synchronize the vector cache. These
  were most of the remaining helper time in SpongeBob avatar switches.
  32-bit MLA and FRINT need SSE4.1; FRINTA/I/X keep the helper.
- Add `jit_native_simd_alias` for aliased destinations (checked on QEMU).

### Vector FMAX/FMIN signed zeros inline (2026-10-16)

- Resolve signed-zero ties in native vector FMAX/FMIN/FMAXNM/FMINNM with an
  equal-lane mask instead of an interpreter step. Only NaN inputs still take
  the cold path. These clamps were most of the interpreted instructions in
  SpongeBob avatar switches; a zero-heavy clamp loop runs about 56× faster.
- Add `jit_simd_fminmax_zero` (3,456 lanes, reference checked on qemu-aarch64).

### Batched DONTUNMAP protection (2026-10-16)

- Change host protection once per run of equal permissions in
  `MREMAP_DONTUNMAP`, instead of four `mprotect` calls per page. Read/write
  runs need none. ART's CMC GC remaps 512 MiB–1 GiB pools; a sparse 512 MiB
  move fell from about 240 ms to 29 ms. Page flags and userfaultfd state are
  unchanged.
- Add a read-only move to `test_userfaultfd`, checked on the host kernel.

### Cold-path vector validity (2026-10-16)

- Keep the compile-time vector-slot validity after FP NaN cold steps and
  SIMD 16-byte slow paths. These paths reload XMMs that the hot path skips.
  After a native vector helper and a scalar FP operation, the hot path read
  stale vector registers and broke touch input in Android games.
- Add the `jit_vector_cold_step` regression (checked against qemu-aarch64).

### Interpreter recovery profiling (2026-10-16)

- Append scoped, thread-local `general`/`fp-special` reasons to fallback
  inventories and expose a distinct generated FP recovery entry to profilers.
  Preserve the first six TSV columns and the existing FP recovery semantics.
- Check concurrent counts, nested and exceptional scope restoration, and
  actual generated FP recovery classification. Counts do not establish the
  cause or duration of a game hitch.

### Allocation effects and region demand layouts (2026-10-16)

- Share pressure-aware GPR pin selection, rematerialize scratch constants,
  avoid demand-destination preloads and score bounded vector-core candidates.
- Retain mixed scalar/vector state with correct upper lanes and explicit
  known-helper clobbers. Reload synchronized values only when read again.
- Extend demand allocation to Tier-2, snapshot branch layouts and reconcile
  loop-entry demand values with parallel copies after revocation/trial checks.
  Preserve canonical external exits, precise guards and MT publication rules.
- Add optional per-PC allocation TSV ranges/deltas, compiler-local counters
  and complete block diagnostic totals including deferred recovery.
- Reduce CPU time of the captured Unity block replay by 30.6%; broader
  benchmarks are roughly unchanged. Actual game stutter gains are unmeasured.

### Android screenshots, release priorities and licenses (2026-10-14)

- Add all six distinct user-supplied Android screenshots: SpongeBob cooking,
  level-14 pause menu, lobby and Help Center, plus Azur Lane loading and login.
  Add an Azur Lane application page and link both galleries from the README.
- Update current support notes with visible screen evidence. Retain the limits:
  Azur Lane gameplay/sign-in, sustained frame pacing and working video/rewarded
  ads remain unverified; screenshot FPS values are momentary presentation stats.
- Define proposed alpha.2 compatibility/performance, alpha.3 WebView/media/ad
  investigation and beta.1 setup/library usability priorities with concrete
  acceptance gates. The current runtime version remains 2.0.0-alpha.1.
- Mark real guest ART/JNI integration as an implemented foundation; remaining
  roadmap work extends compatibility, stress coverage and performance.
- Add `LICENSES/` with the project Unlicense, the canonical Apache 2.0 text
  and pinned attribution for six AOSP Settings source inputs. Original Asgard
  integration stays Unlicense; reused Android components retain upstream terms.

### Desktop Android library (2026-10-14)

- Replace the launcher table with searchable game cards, bitmap icons, per-game
  Back-key/window-stat settings and last-session details. Keep CLI and GUI
  launches on the same session supervisor; developer launch options remain
  explicit. Retain install metadata and read older base-APK reports.
- Reject malformed saved settings without crashing the launcher and tolerate
  non-string icon metadata. Missing/adaptive-only artwork uses a placeholder.
- Integrate the pending JNI Surface identity fixture with the actual adapter
  and pinned Bionic/AOSP headers. Both engines pass its controlled-handle checks;
  this does not test real ART collection or Surface rendering.
- Focused preflight/launcher/package checks and the signed NativeActivity gate
  pass, including all six engine/window-backend combinations. Offscreen Qt
  checks exercise cards, filtering, settings and session details; desktop visual
  review and gameplay through the redesigned launcher remain unverified.

### Tier-1 codegen and validation (2026-10-14)

- Remove stale operands from folded constants, fuse W-form ALU/extension
  operations, defer scalar memory-check cold tails, avoid dead exit scratch
  spills and restore complete five-byte chain slots. Cached-entry loop growth
  uses branch metadata only as a hint while normal translation records every
  absorbed instruction and retains all budgets.
- Fix dead-code compaction accounting: DSE/NOP marking and physical removal
  no longer double-count stores or underflow the reserve size.
- Final interleaved CoreMark medians improve from 9,049.03 to 9,893.87
  iterations/s (9.3%), with every CRC validated. Strict JIT 326/326,
  interpreter 317/317 (nine JIT-only skips), 87 differential fixtures and
  seven threaded oracles pass. Game FPS gains remain unmeasured.
- Make the X11 wire fixture wait for its MapNotify before GetImage readback,
  satisfying the viewable-window contract under asynchronous window managers.

### User and idle-policy queries (2026-10-14)

- Expand fixed user-0 UserManager/profile/serial/restriction queries with
  caller checks, redacted personal fields and isolated replies. Add the
  device-idle service for the existing disabled-Doze profile and complete
  battery-saver/standby queries in power.
- UserManager supports 26/104 signatures, device-idle 13/22 and power 18/67;
  unsupported administration is not advertised as implemented. The complete
  managed service batch passes 176 assertions in each engine.

### Host Codec2 media and resized native windows (2026-10-14)

- Provision the real Codec2/Goldfish stack and native dependencies. Codec
  services and Gralloc belong to the Binder realm and survive app exit;
  host CPU/GPU decoder capabilities are reported only for probed codecs.
- Bounded realm probes check exact 64x64 codec frames, PTS/EOS and genuine
  ImageReader/native-window output. Larger streams, extended reconfiguration,
  HDR and in-game video playback remain unestablished.
- Aliased ANativeWindow dimensions follow the host SurfaceView geometry
  instead of the ImageReader's creation size. The user confirmed clean resize;
  backend checks cover dimension seeding, validation and geometry updates.
  Full SurfaceView recreation and callback sequences remain unverified.

### Launcher open-file limit (2026-10-13)

- Raise the emulator/broker host soft open-file limit to its inherited hard
  ceiling at executable startup. Apps opening more than 1024 files no longer
  need a shell `ulimit -n` workaround when the host permits a higher limit.
  The hard limit and parent process are unchanged.

### Real Android media JNI (2026-10-13)

- Framework schema 7 provisions the pinned image's real `libmedia_jni.so` and
  its native dependencies, fixing missing-library errors when Android media
  boot classes initialize. A real MediaMuxer probe writes valid AAC MP4 in
  JIT and interpreter modes. The later host Codec2 checkpoint above adds
  codec-service/decoder routing; in-game media playback remains unestablished.

### Asgard Settings and PendingIntent contracts (2026-10-13)

- Generate fresh user-0 phone Settings defaults from pinned Android 15
  DatabaseHelper/resources and reviewed SettingsProvider upgrades. System
  settings include `accelerometer_rotation=0`; Google-specific keys remain
  absent. Legacy `WRITE_SETTINGS` grants, public-key validation, persistent
  per-app overrides, cache generations and ContentObservers now work.
- Implement local Binder-backed PendingIntent identity, lookup/update/cancel,
  one-shot and mutability rules, cancellation callbacks, metadata and delivery
  to Activities, Activity results, services and dynamic/manifest receivers.
  Ordered broadcasts propagate completion/results and enforce platform
  protected actions. Foreground services, cross-app/process receiver delivery,
  process restart and notification interactions remain unsupported.
- Accept PendingIntent alarms, including repeating intervals of at least
  60 seconds and cancellation by operation; enforce exact-alarm permissions.
  Preserve System overrides across managed runtime upgrades.

### Common Android device services (2026-10-13)

- Managed apps get eleven more framework services, so `getSystemService`
  returns real Android managers instead of null: `uimode`, `permissionmgr`,
  `permission_checker`, `batteryproperties`, `batterystats`,
  `vibrator_manager`, `trust` (with the window manager's keyguard calls),
  `clipboard`, `alarm`, `account` and `location`. They model one plugged-in
  device without a battery, vibrator, lock screen, GNSS or Play services,
  following Android 15's services. Hungry Shark Evolution had crashed on a
  null `UiModeManager`.
- `registerReceiver` now returns and delivers the sticky
  `ACTION_BATTERY_CHANGED` broadcast, and configurations carry `uiMode`
  normal/night-off. Listener and PendingIntent alarms fire; host clipboard
  sharing and host battery state are not implemented.

### Fewer JIT recompiles and compile stalls (2026-10-13)

- A 64-byte `DC`/`IC` publish (one per line from ART's `__clear_cache`) now
  drops only translations overlapping that line, not every block on its page.
  Cache maintenance and `mprotect` keep a translation whose recorded
  instruction words are unchanged and still executable. Mapping replacement,
  unmap, `mremap` and discard still retire every overlap.
- In multithreaded guests, Tier-1 compiles hold the block table only shared;
  other CPUs keep finding and running published code. Invalidations that race
  a compile are logged, and the block is retranslated before publication.
- Single-thread direct `BL` calls fall back to their slow path when the callee
  is retired. A caller that survived its callee's rewrite had called the
  stale translation (only reachable with `BIFROST_NO_CHAIN_SKIP=1`).
- `BIFROST_STATS_PERIOD` and `--verbose` report Tier-1 compile time,
  per-block maximum, recompiles, contended lock wait and invalidations by
  reason (calls/retired/kept). A cold ART startup compiled about 40,000 blocks
  in 0.48 s of its first second; gameplay stutter has not been measured.

### Covered Activities stop (2026-10-13)

- An Activity started over another one that is opaque (theme neither
  translucent nor floating) now stops the Activity beneath and hides its
  window, as Android does; finishing it restarts the one below before its
  result and resume. Azur Lane's blue `PrePermissionActivity` screen no
  longer covers the game's Unity surface.

### AAudio API coverage (2026-10-13)

- AAudio adds the 18 functions apps such as CRIWARE look up: channel masks
  and samples per frame, content type, spatialization, capture policy,
  privacy sensitivity, package name/attribution tag, `requestFlush`,
  `release` and `AAudio_convertStreamStateToText`. Validation, defaults and
  state rules follow AOSP libaaudio (Android 15). `AAudio_convertResultToText`
  now returns its text; it previously had no implementation.
- Clearing a host audio stream counts the discarded frames as read.

### Managed OpenSL ES playback (2026-10-13)

- Managed Android apps get a `libOpenSLES.so` adapter: engine, output mix and
  PCM buffer-queue audio players with Play, Volume, EffectSend direct level and
  AndroidConfiguration, following AOSP wilhelm (Android 15). Players render
  through the managed AAudio adapter, so buffer-queue callbacks run on a Bionic
  thread. The image's own library needed AudioFlinger tracks and failed to
  realize players (Azur Lane's CRIWARE: `failed to realize OpenSLES player`).
  Decoding (URI/fd) players and recording are not supported.

### Azur Lane startup fixes (2026-10-13)

- LDXP/LDAXP/STXP/STLXP are decoded and executed (interpreter, shared
  exclusive monitor, 16-byte host compare-exchange for X pairs) instead of
  raising SIGILL; pairs with an XZR second register are no longer misdecoded
  as LDXR/STXR. Azur Lane's Unity code uses them for 128-bit atomics.
- An Activity started from the launcher Activity's `onCreate` now launches
  instead of failing with `activity_task.startActivity`.
- The managed `libandroid.so` exports the remaining public NDK functions
  (forwarded to the platform library), so app libraries such as CRIWARE that
  import `AAsset_*` load.
- `android_package.py install` accepts the Gralloc mapper adapter.

### Consistent process identity (2026-10-13)

- `/proc/self/stat` and `/proc/self/status` report the process's real PID,
  parent, process group, session and name instead of `1 (bifrost)` with
  `PPid: 0`, so they agree with getpid/getppid/getpgid/getsid. Android apps
  with the host's PID previously saw PID 1 there.
- `getpgid`/`getsid` follow the same identity; for a host-identity process
  `setsid` returns `EPERM` instead of a fake session 1, and `kill(1, …)` is
  `EPERM` instead of signalling the app itself. After execve the leader's TID
  equals its PID.

### Kernel-format /proc/self/maps (2026-10-13)

- `/proc/self/maps` lists every guest mapping with its real permissions,
  including `PROT_NONE` reservations, instead of one `rw-p` line per
  allocation. File mappings show `s`/`p`, their file offset, device, inode and
  resolved guest path, split and merged as the kernel does.
- ELF images Bifrost loads by copying (the main executable, Android
  `linker64`, the legacy interpreter, native-linker libraries) are named after
  their files; `[interp]`, which Linux never prints, is gone. `[vdso]` is
  labelled.

### Android Activity windows and SurfaceView lifecycle (2026-10-13)

- The launcher Activity's decor is now a WindowManager window like every
  other Activity's, so an Activity built only from Views (buttons, text,
  WebView) launches instead of failing with "Activity has no SurfaceView".
  ViewRootImpl lays it out; its Views draw in software above the game surface,
  and a fully transparent window (a SurfaceView punching through all of it)
  gets no host layer.
- SurfaceViews in any window follow SurfaceView's visibility contract: their
  surfaces are created, resized and destroyed as views are added, shown,
  hidden, resized or removed, not only at startup and host resize. A created
  surface lets the window punch its hole, so Views drawn after the
  SurfaceView stay above it.
- Activity windows stack below their own dialogs, as in WindowManagerService,
  and window focus no longer reports a loss to a window that never had it.
- The session publishes a strict legacy `SurfaceFlinger` service accepting
  only empty SurfaceControl transactions; SurfaceView sends one on each
  update, and libgui otherwise waited for that service forever.
- Not yet: positioned/clipped/z-ordered SurfaceViews and fixed buffer sizes
  (surfaces stay full-window layers below the Views), and event-driven
  wakeups (the UI loop still polls every 4 ms).

### Android permission results after off-main-thread requests (2026-10-13)

- A permission result for an Activity's own request is delivered only after
  `requestPermissions` has marked the request pending. Unity requests off
  the main thread, and an earlier result would have left the request
  pending, cancelling every later one with empty results.

### Android window max bounds and launcher tracing (2026-10-13)

- Android's `windowConfiguration` max bounds are now the host display's
  pixel size (new window op 12), in the process configuration as well as the
  Activity's. The process configuration had none, so ViewConfiguration's
  software drawing-cache limit could be 0 and full-window software layers,
  such as SpongeBob's ad WebView, refused to draw. The ad still stops at its
  loading screen (open issue).
- The desktop launcher enables "Trace calls" only when every selected app
  has a managed Activity; NativeActivity apps failed with "APK
  framework/method tracing requires a managed Activity".

### JIT compile time and inline SIMD (2026-10-13)

- Tier-1 and Tier-2 compiles no longer rebuild all 4,096 per-vreg use lists
  for every block; only the lists the previous compile filled are emptied,
  keeping their storage. In a fresh SpongeBob startup profile that rebuild
  was about a third of Tier-1 compile time (2.3% of sampled CPU).
  `jit_native_simd_gen`, which compiles thousands of small blocks, dropped
  from 118.5 to 108.5 ms of CPU.
- EXT, FCMEQ/FCMGE/FCMGT and their #0 forms, CMEQ #0, REV64, vector FMOV
  (immediate) and USHL/SSHL with 32/64-bit lanes (AVX2 hosts) are emitted
  inline instead of calling a C++ helper that flushed every cached GPR.
  Tier-2 no longer treats those words as calls. A synthetic loop of these
  ops fell from 1,682 to 104 ms of CPU; game FPS is not yet measured.
  `jit_native_simd_inline` checks every shift count byte and aliased
  destinations.

### Base ARMv8.0 FP/SIMD semantics from the coverage sweep (2026-10-13)

- Every base ARMv8.0 FP/SIMD word in the sweep now matches QEMU in both
  engines (the 2026-10-12 baseline had 12,448 differing words, most of
  them unadvertised extensions). Fixed results: CMLE #0 (ran as
  CMEQ), JIT UQSUB `.4h/.8h` (wrong x86 opcode), USQADD, scalar FCVTMS (ran
  as FCVTZS), URECPE/URSQRTE (ran as FP compares), AESE/AESD/AESMC/AESIMC,
  integer by-element `.s` with Vm in v16-v31, TBL/TBX with one table
  register, halving and rounding/saturating register shifts, ADDHN2 `.2d`,
  scalar FMLA by element and scalar compares.
- Added the missing base forms: vector and scalar FCVT{N,P,M,A}{S,U},
  SQDMULH/SQRDMULH and SQDMLAL/SQDMLSL/SQDMULL{2} in every shape, SABA/UABA,
  PMUL, FRECPX, scalar FMAXNMP/FMINNMP, and the scalar saturating, shift,
  narrow and absolute forms. The scalar forms run the vector implementation
  on element 0 through one table.
- FPSR.QC is set by saturating integer operations in both engines,
  including the JIT's native SQADD/UQADD/SQSUB/UQSUB and SQXTN family.
- Reserved and unadvertised encodings raise SIGILL instead of executing:
  accepted-unallocated words fell from 38,403 to 2,315. FP16 scalar FP
  (not in AT_HWCAP) now traps apart from the FCVT half-precision
  conversions; some of those instructions had returned wrong results.
- `jit_simd_base_forms` checks 139 forms and 30 reserved encodings against
  QEMU hashes. `jit_neon`, `jit_neon_fixups` and `test_simd_sat` expected
  the old wrong results (SHL `#64`, an 8-byte TBL table, a wrapped UHSUB,
  an unsigned UQSHL amount) and were corrected against QEMU.

