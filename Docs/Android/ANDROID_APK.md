# Signed APK installation and native launch

The emulator executable raises its own host `RLIMIT_NOFILE` soft limit to
the inherited hard limit before startup, including its Binder-broker mode.
Desktop sessions commonly supply 1024 soft / 1048576 hard; large managed
apps therefore no longer need a preceding `ulimit -n 1048576`. This uses
the permitted ceiling rather than assuming a particular distribution's
limit. It leaves the hard limit and parent desktop/shell unchanged. A
lower hard limit still constrains open files; a failed adjustment prints
one warning and startup continues under the inherited limit. This is a
host-executable startup policy, not complete guest resource-limit emulation
or a change to resource limits of library-embedding callers.

Current tooling scope: October 14, 2026. Bifrost installs signed APK sets and executes a
`android.app.NativeActivity` through the real guest Bionic linker, and managed
SurfaceView Activities through the ART/framework
[managed launcher](ANDROID_RUNTIME.md#managed-apk-launcher-2026-10-08).
The regression APK uses split assets, pthread/TLS, persistent storage, EGL
pixel readback and an actual Binder service call. Managed Activities still
require guest ART and Android framework startup.

## Setup and use

Provision Python 3.12 or newer, Clang/LLD, `debugfs` from e2fsprogs and a JDK containing
`java` and `keytool`. The signing and manifest tools are checksum-pinned AOSP
downloads; no full Android SDK installation is required.

```bash
make -j6 USE_SDL2=1 USE_SDL3=1 USE_THUNK_GL=1
python3 Tools/Android/fetch-android-runtime.py
python3 Tools/Android/fetch-aapt2.py
python3 Tools/Android/fetch-android-package-tools.py
make android-runtime-adapters android-window-plugin

python3 Scripts/Android/android_package.py install /path/to/package.apkm
python3 Scripts/Android/android_package.py run org.example.package

# Diagnose native loading even when the package has a managed launcher.
# Each library gets a fresh process; constructors run, JNI_OnLoad does not.
python3 Scripts/Android/android_package.py probe org.example.package
python3 Scripts/Android/android_package.py probe org.example.package --library libunity.so

# Use the loadable SDL3 window backend instead of the built-in backend.
python3 Scripts/Android/android_package.py run org.example.package \
    --android-window-plugin build/release-sdl1-gl1/plugins/android-window-sdl.so
```

Installation accepts a single APK or a ZIP/APKM/APKS containing one consistent
base/split set. It does not select among bundletool device variants. Use
`--state-root PATH` before `install`, `run` or `probe` to choose a different state directory.
`run --no-jit` uses the interpreter; `--frames N` bounds redraw cycles for tests,
and zero runs until the window closes. Bifrost flags precede its guest ELF.
`probe --no-jit` also uses the interpreter. Repeat `--library` to select a subset;
the default checks all installed ARM64 libraries. Repeat `--preload BASENAME` to
load packaged prerequisites in the given order before each target, with Bionic's
normal `RTLD_LOCAL` scope. No application-specific ordering is inferred.
`--timeout` bounds each process
to 1..300 seconds (45 by default), including its descendants. Diagnostics print
a bounded output tail, reject fatal markers even with exit zero, and return
nonzero if any library fails. A flushed `LOAD_OK` marker records successful
`dlopen` separately from process exit: `loaded, process failed` still fails the
command. This executes native constructors and cleanup;
it is a loading diagnostic, not an APK launch or a side-effect-free inspection.

Adapter changes require a fresh `--state-root`; the installer deliberately
rejects replacing different adapters inside an existing app root. Keep the
original app state when evaluating a new runtime.

## Desktop library

`python3 Scripts/Android/asgard.py launcher` opens the optional PyQt6 game
library using the same install and session supervisor as the CLI. Cards show
the app label, version, install date and bitmap icon, with Play/Stop actions,
search, per-game settings and a last-session drawer. Package IDs, library paths,
interpreter launch and Java call tracing are grouped under Developer.

Install records now retain the base APK's label/version and a bounded launcher
bitmap; older records recover labels and versions from their saved base report.
Icon selection prefers a dense daytime bitmap from the declared launcher
resource. Adaptive/vector icons use that resource's bitmap fallback when present;
otherwise the card uses a placeholder. This does not compose adaptive icons.

Per-game `launcher-settings.json` stores explicit window-stats and Back-key
choices. With no saved choice, launches keep the emulator defaults. Changes
apply on the next start. The last-session drawer shows saved outcome/timing and
recognized error signatures from at most the final 16 MiB of its log; those
counts are a bounded log summary, not a diagnosis or gameplay result.

## Optional real Media NDK bundle

The Media NDK can run as guest ARM64 code through Bionic. Provision the actual
Android 15/API-35 implementation from the official AOSP emulator system image;
the NDK's `.so` link stubs are not runtime libraries:

```bash
python3 Tools/Android/android_media.py
python3 Scripts/Android/android_package.py --state-root android-state/real-media \
    install /path/to/package.apkm --media-root build/android-media
```

Provisioning requires `debugfs`, `lpunpack` (Android partition tools) and
`readelf`. The first download is approximately 734 MiB; extraction needs several
GiB of temporary disk space. It reads GPT/dynamic partitions and APEX payloads
without mounting filesystems. The image and complete extracted file set are
SHA-256 pinned. The source image cache and existing bundles are verified before
reuse; a modified bundle is rejected. Upstream notices are retained.

The bundle contains the genuine `libmediandk.so`, its 154-library `DT_NEEDED`
dependency closure (including itself), and ICU data. These libraries load in
an isolated Bionic `media` namespace under `/system/lib64/media`, sharing only
the existing Bionic C/math/dynamic-link libraries. The app namespace can access
`libmediandk.so` through a declared linker namespace link. Its existing
`libandroid`/EGL/GLES/logging adapters continue to serve application calls.
Installation verifies the copied libraries before publishing the root, records
their hashes in the package manifest, and rechecks the files and complete
namespace configuration on launch/probe/reinstall. Enabling, disabling or
changing the media bundle requires a fresh app state; reinstall with the same
`--media-root` to preserve an existing media-enabled installation.

Real `AMediaFormat` allocation, typed fields, copied strings/buffers, format
copy/clear/description and deletion are exercised in the signed APK gate.
No Media NDK replacement functions are supplied. The managed framework's later
[host media checkpoint](ANDROID_RUNTIME.md#host-media-decoding-2026-10-14)
adds real Codec2 services owned by the Binder realm, host decoding and genuine
ImageReader/native-window output. Temporary Java MediaCodec probes decoded
exact 64x64 AVC/HEVC/VP8/VP9 frames with checked timestamps and EOS; separate
NDK AMediaCodec/native-window probes checked forced-GPU output. These checks
are separate from the signed APK gate's MediaFormat assertions. Provisioning
the Media NDK bundle alone does not create those codec services.

The realm-owned software Gralloc5 allocator and guest mapper are also checked
through AHardwareBuffer and AImageReader CPU pixels (see
[allocation validation](ANDROID_RUNTIME.md#software-gralloc5-2026-10-13)).
`media.player` and `media.extractor` remain absent; unavailable service calls
may block. Full stream reconfiguration, HDR, native EGL rendering into an aliased
Surface and in-game video playback remain unverified. This is a bounded media
implementation, not arbitrary codec/player/DRM compatibility.

## Package and runtime ownership

The installer snapshots the bounded input, checks archive paths/member types,
verifies every APK with [official apksigner](https://developer.android.com/tools/apksigner),
and requires matching signer certificates, package/version identities and split
dependencies. It rejects duplicate/conflicting native providers and attempts
to replace runtime or adapter libraries. Input and aggregate expanded APK size
are capped at 2 GiB. Updates require the same signer identity and cannot
downgrade; certificate-rotation lineages are not implemented.

Each package has an independent rootfs under ignored `android-state/`:

- `/data/app/<package>-<digest>` retains signed APKs, extracted ARM64 libraries
  and merged assets. Code is published by rename and made read-only.
- `/data/user/0/<package>/files` and `cache` retain application data.
- `/storage/emulated/0/Android/data/<package>/files` provides external app storage.
- The root contains copied, verified Bionic libraries and guest adapter DSOs.

An install lock and atomic metadata publication keep failed validation from
replacing the active package. Reinstalling a reordered set is idempotent and
preserves saves. Launch verifies stored package/runtime/adapter hashes before
execution. This layout uses Bifrost's VFS root; it is not a validated host
security sandbox or a complete Android permission/UID model. Concurrent app
instances share their package's data directories.

The guest `bifrost-native` launcher calls `dlopen` through Bionic and supplies a
guest-owned NDK LP64 Activity and callback table. Guest linker64 owns relocation,
constructors, dependency loading and TLS. The API-35 linker configuration uses
an isolated app namespace; APK code resides in its permitted `/data/app` tree.
The pinned runtime's provenance and 4 KiB pages/16 KiB ELF alignment are covered
in [ANDROID_RUNTIME.md](ANDROID_RUNTIME.md). This does not establish a complete
API-35 platform implementation.

Before app loading, the guest launcher gives Bionic the packaged library
directory through the pinned `libdl_android` platform entrypoint
`android_update_LD_LIBRARY_PATH`. This makes sibling `DT_NEEDED` libraries
searchable within the isolated process. It is an internal platform API tied to
this runtime, not a public NDK compatibility promise. Bionic's descriptor-path
checks use translated guest `/proc/self/fd` identities and guest-root paths;
the host descriptor with the same number must never decide namespace access.

Guest `libandroid`, `liblog`, `libEGL` and `libGLESv2` adapters resolve explicit
exports into Bifrost's existing thunks. Assembly wrappers preserve AAPCS64
arguments and cache trampoline addresses with acquire/release operations.
Graphics remain behind the [window plugin interface](ANDROID_WINDOW_PLUGINS.md).
The build automatically fetches AOSP Bionic, NativeActivity, window, rectangle,
JNI-type, logging and Media NDK headers at pinned Android-15 repository revisions.
Public ABI declarations come from those headers. Downloaded archives live under
ignored `build/android-dependencies`; bounded extraction accepts regular files
and directories only. Gitiles adds changing archive timestamps, so AOSP pins
cover sorted regular-file names, lengths and contents with SHA-256. Static
upstream zlib archives use their published archive SHA-256. Every cache reuse
is reverified; adapter metadata records source URLs and checksum kinds.

Guest `liblog` formats print/vprint/buffer-print arguments using real Bionic
`vsnprintf`, preserving the ARM64 `va_list` ABI, then sends a fixed string to
the existing stderr bridge. Successful writes return one, as specified by
[Android's logging API](https://developer.android.com/ndk/reference/group/logging).
Formatting is capped at 1023 bytes; buffers share one sink. This does not supply
logd, property-based filtering or the full logging API. Log assertions terminate
through Bionic `abort` rather than reporting success.

`libz.so` is genuine guest ARM64 [zlib 1.3.2](https://zlib.net/), compiled from
the pinned upstream release against the real Bionic library and AOSP headers.
Compression/decompression executes in the guest; no host zlib pointer or
callback ABI crosses this boundary. Its license accompanies the installed DSO.
Private syscalls `0x100D` (adapter resolution) and `0x100E` (event pump/input
queue) require the Android runtime profile. They are not Android kernel APIs.

## Compatibility boundaries

The native-only path accepts the exact `android.app.NativeActivity` class.
On that path, `JavaVM`, `JNIEnv` and the Java Activity object are null; apps must
avoid JNI. Managed launcher classes instead use the separate ART/framework path.
The native-only launcher invokes startup, window/input, redraw and teardown callbacks.
That path does not implement full Activity lifecycle transitions, resource-table
resolution, Java class loading, DEX execution or Android property services.

`ALooper` is per guest thread, as AOSP's `Looper::getForThread` is:
`ALooper_forThread` returns NULL until the thread calls `ALooper_prepare`,
`pollOnce`/`pollAll` use the calling thread's looper (`ALOOPER_POLL_ERROR`
without one), `addFd` rejects a NULL callback unless the looper allows
non-callback sources, and a callback returning 0 unregisters its fd.
`AChoreographer_getInstance` returns the thread's choreographer, or NULL
without a looper. `postFrameCallback`/`64`/`Delayed` callbacks are one-shot and
run inside the owning thread's poll, at the first 60 Hz `CLOCK_MONOTONIC` tick
after they fall due. They receive that tick as the frame time. Vsync callbacks
with frame-timeline data, refresh-rate callbacks and real display timing are
not implemented. The sensor manager reports a device with no sensors: an
empty list, NULL default sensors, and event queues that never have events.
Enabling a sensor fails with `BAD_VALUE`, and direct channels are not
exported.

Asset file open/read/seek/length/buffer/close work on extracted files, capped
at 64 MiB per asset. Directory enumeration, asset file descriptors and OBB
mounting are not implemented. Graphics and platform exports cover the existing
thunk subset; the adapter route does not establish arbitrary NDK or audio
compatibility. Missing adapter exports fail explicitly.

`--android-services` enables the bounded native service-manager subset described in
[ANDROID_BINDER.md](ANDROID_BINDER.md). That flag alone does not start the managed
framework. The managed launcher separately supplies Activity/Package manager
contracts and limited SurfaceFlinger endpoints; it is not a full Android system.
See [framework service scope](ANDROID_RUNTIME.md#managed-framework-service-modules-2026-10-09).

Historical native-loading results: the user-provided SpongeBob
5.11.5 set installs successfully: nine signed APKs
and 21 ARM64 libraries. The normal APK runner now launches
`com.clevertap.unity.CleverTapOverrideActivity` through ManagedLauncher.
Its SurfaceView path provides a window and input; this remains a bounded
framework profile rather than a complete Android system. A separate
[installed-app framework probe](ANDROID_RUNTIME.md#installed-app-creation-checkpoint-2026-10-07)
now reaches the real launcher's `onCreate`, including IL2CPP JNI startup, in
both engines. It exits after creation and does not draw or accept input.
Installation is not game-play compatibility evidence. The earlier native probe
completed loading and process
cleanup for 19/21 libraries under JIT, interpreter and opt-in Tier 2, including
`libmain`, `libil2cpp`, Firebase Analytics/App/AppCheck/Crashlytics and the native
Crashlytics libraries. Unity reaches a missing `libmediandk.so` dependency.
Firebase Messaging finishes `dlopen` but aborts during process exit when probed
directly. Its cause is now traced: App and Messaging both export
`firebase::messaging::ListenerImpl::g_mutex`. Loading Messaging and its App
dependency together makes both constructors register the same mutex address
with `__cxa_atexit`, using distinct DSO handles. The App destructor
`firebase::Mutex::~Mutex` at offset `0x1936a0` destroys it twice; the observed
Bionic state changes from `0x4000` to the destroyed marker `0xffff` before the
second call, matching the [pinned Bionic mutex implementation](https://android.googlesource.com/platform/bionic/+/361ba86734fb2821a6adcfdf775db8abd04e0de0/libc/bionic/pthread_mutex.cpp).
The registrations belong to App (`__dso_handle` at `0x3ddb88`) and
Messaging (`0x139f8`); the shared object resolves to Messaging at `0x140d8`.
These offsets refer to this packaged version, not a general Firebase ABI.

The current SpongeBob 5.12.0 gameplay and remaining WebView/Google Play gaps
are recorded in [its application page](../Apps/spongebob.md). The earlier
19/21-library result above is a native-load diagnostic, not the current managed
game's compatibility status.

Explicitly loading App first with `RTLD_LOCAL` gives each DSO its own mutex
address and completes Messaging cleanup under JIT, interpreter and opt-in
Tier 2. QEMU reproduces both the double destruction and the clean App-first
exit. This isolates a load-order problem in the standalone probe; it does not
establish the managed game's initialization order or successful JNI startup.
The probe preserves normal Bionic checks and exit handlers. JNI entrypoints are
located, never invoked. Media services, ART and framework startup remain required.
With the explicit App preload, the full 21-library JIT probe completes 20/21
loads and clean exits in the earlier adapter-only checkpoint; Unity remains
blocked on `libmediandk.so` there. With the optional real-media bundle, that
dependency resolves. The guest EGL adapter now exports every EGL function in
Android's `libEGL.map.txt` that has a host equivalent, which covers all of
Unity's EGL and GLES imports. `libandroid.so` now also exports
`ALooper_forThread`, `AChoreographer_*` frame callbacks and the `ASensor*`
API, so every Unity import resolves. With vector `FADDP` implemented (a
Unity constructor runs `faddp v17.2s, v17.2s, v18.2s`), `libunity.so` loads
and exits cleanly under the JIT and the interpreter, and the full probe with
the Firebase App preload loads 21/21 libraries with 21/21 clean exits (JIT,
fresh `android-state/choreo-sensor`). JNI entrypoints are still not invoked.
The local real-media installation uses `android-state/real-media`.

The earlier header checkpoint uses `android-state/bionic-headers`. The updated
launcher installation uses a fresh `android-state/bionic-mutex-final`:

```bash
python3 Scripts/Android/android_package.py --state-root android-state/bionic-mutex-final \
    probe com.tiltingpoint.spongebob --library libFirebaseCppMessaging.so \
    --preload libFirebaseCppApp-13_8_0.so
```

## Verification

```bash
make test-android-package
./Scripts/Testing/run_tests.sh --strict --filter android_service_manager
./Scripts/Testing/run_tests.sh --strict --no-jit --filter android_service_manager
```

The APK gate requires a native Wayland desktop and GPU and provisions the
pinned real-media bundle above. It builds and signs a
base/split NativeActivity fixture with actual AAPT2/API-35 manifest resources
and apksigner. All six JIT/interpreter/Tier-2 and built-in/plugin combinations
pass lifecycle, pthread/TLS, split-asset, save persistence, EGL pixel readback
and service-handle invocation checks. The signed fixture also verifies sibling
dependency loading, mixed integer/FP/string/stack variadic logs, a known CRC,
an independently generated compressed stream and zlib round trips. Its two
libraries pass the standalone loading probe. Ordered preloads and a real guest
exit-handler failure also pass: the latter preserves `LOAD_OK` while failing
the command with guest exit 23. The six host test cases also cover path
and symlink rejection, input growth bounds, signature tampering, signer mismatch,
version mismatch, downgrade prevention, reordered reinstall, modified installed
code, and native-launcher boundaries. An additional signed managed fixture now tests
ART Activity startup, resource labels, persistent preferences and ordered
Activity.finish teardown over two launches; see the managed launcher guide.
The real Media NDK additionally passes 256 repeated metadata ownership/type
cycles per fixture process in all six backend/execution-mode combinations.
These checks exercise metadata, not Android media services or decoding.
Modified installed media libraries and namespace configuration are rejected
before the emulator can execute.

The complete Linux regression suite passes 273/273 JIT runs and 266/266
interpreter runs with seven JIT-only skips. The real Bionic bootstrap separately
passes all six fixed/random ASLR and execution-mode configurations.
For the preceding Bionic/header refinement, the full Linux runs initially
rejected two stale fixtures;
after rebuilding, both cases passed in each mode. Aggregate coverage remains
273 passing JIT runs and 266 passing interpreter runs with seven JIT-only skips.
