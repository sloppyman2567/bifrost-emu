# Android native runtime bootstrap

Checkpoint: October 6, 2026. Bifrost can enter a real guest ARM64 Android
linker and execute Bionic-linked programs. Signed APK installation and native-only
NativeActivity launch now work through this runtime; see [ANDROID_APK.md](ANDROID_APK.md).
A separate same-image ART/libcore provisioner and DEX/JNI bootstrap now create
a real ART VM and pass their JNI fixture; see the bounded checkpoint below.
The separate installed-app Activity probe now reaches SpongeBob's real
`CleverTapOverrideActivity.onCreate` and IL2CPP's JNI startup in both engines;
the normal APK runner now also launches managed SurfaceView Activities through
`ManagedLauncher`, with persistent app data and supervised process lifetime (see below).
The APK native-load diagnostic now checks real packaged dependencies separately:
19/21 SpongeBob libraries complete loading/cleanup in all three execution modes.
Firebase Messaging's direct probe loads successfully, then double-destroys a
shared mutex on exit. Explicitly preloading Firebase App fixes that probe's
cleanup in all three modes and QEMU. See [the APK checkpoint](ANDROID_APK.md)
for the load-order evidence and official-header/zlib build.
An optional checksum-pinned real Media NDK bundle now supplies the genuine
library and its isolated native dependencies. Real metadata operations pass
the signed APK gate in all modes. With this bundle, the full EGL adapter and
the per-thread looper, choreographer and sensor exports, every Unity import
resolves and all 21 libraries load in the native probe. That diagnostic does
not invoke JNI. The framework app probe below invokes the game's startup;
media services/decoding remain bounded; the managed launcher now supplies a
window, input and presentation for the SurfaceView path described below.
See the APK guide for provisioning and namespace boundaries.

## Android 15 framework probe

The same pinned API-35 image now supplies `app_process64`, framework boot jars,
and the mainline APEX native libraries. Framework provisioning schema 5
verifies 919 files and exports a linker namespace for every installed boot
APEX, including `com_android_tethering`. Each delegates to the same canonical
native-library scope so JNI loading does not instantiate a second ART VM.
Schema 4 adds `framework-res.apk`, `fonts.xml`, `font_fallback.xml` and
`/system/fonts`: `Resources.getSystem()` and text layout need them before an
Activity draws. Schema 5 adds the `system_ext` and `product` `build.prop`
files (see the CMC note below). This bootstrap layout does not reproduce production Android
namespace isolation. Older schema-2/3 framework roots must be re-provisioned
in a fresh destination; preserve the old directory if it is still useful for
comparison.

```bash
python3 tools/android_framework.py
mkdir -p build/framework-probe/classes
javac --release 8 -d build/framework-probe/classes guest/android/FrameworkProbe.java
java -cp tools/android-tools/d8.jar com.android.tools.r8.D8 --min-api 26 \
  --output build/framework-probe build/framework-probe/classes/FrameworkProbe.class
timeout -s KILL 165 python3 scripts/test_android_framework.py --mode jit --timeout 150
timeout -s KILL 165 python3 scripts/test_android_framework.py --mode interp --timeout 150
```

`--services` enables Bifrost's Binder service registry
(`--android-services`). `--library` copies a probe JNI `.so` into
`/data/local/tmp`. `BIFROST_BINDER_TRACE=1` logs Binder transactions,
registry calls and failures.

Both Bifrost engines reach `FRAMEWORK_OK` and exit zero with ART's nterp and
its own JIT enabled; no `-Xint` or `dalvik.vm.usejit=false` override is needed.
The probe enters Android's main Looper and dispatches a Handler message. It
exits from that callback because Android forbids quitting the main MessageQueue.
It checks framework class loading, Build metadata, liblog and Process.myPid.
Logs, logd messages and JSON results are in `build/android-framework-results/`.
The runner's internal deadline kills its process group with SIGKILL.

The nterp failure was an emulator `BLR X30` ordering error: overwriting LR
before reading the target skipped `Float.isNaN`, so HashMap interpreted a
method pointer as a boolean. ART's JIT then exposed independent copies of its
shared RW/RX file aliases and missing PC-relative literal loads. Shared file
backings now retain kernel aliases across unmap, replacement, remap and fork;
`msync` writes back, and `MADV_DONTNEED` refaults file data. Binder arenas stay
in the low 4 GiB even after high ART reservations. Native Bifrost
callers unwind after guest process exit instead of reaching libc's fallback trap.

The Android profile leaves guest page 0 unmapped, as Linux does
(`mmap_min_addr`). ART's implicit null checks need that fault: its SIGSEGV
handler converts it into `NullPointerException`. Bifrost's Linux profiles
still map a readable zero page for legacy guests. With it mapped, nterp read
a zero method entry point from a null `ArtMethod*` and jumped to address 0.
`/proc/self/maps` reports the linker's real extent and omits an empty
`[heap]`, so lines never overlap. Bionic takes the first line containing the
initial SP as the main stack; the old 256 MiB `[interp]` guess could win.
ART then computed a negative `madvise` length below the stack. `madvise` now
returns `EINVAL` for wrapping ranges like Linux `do_madvise`; it previously
zeroed every page above the start, including libc text. This caused the
layout-dependent early SIGILL recorded in [test evidence](TESTS.md).

With userfaultfd available, ART uses the CMC collector, and its linear-alloc
arena release (`TrackedArena::ReleasePages` in
`runtime/base/gc_visited_arena_pool.cc`) first tries `MADV_REMOVE`. It zeroes
the arena itself only when that fails with `EINVAL`, as it does on Linux for
private anonymous memory. `madvise` used to report success for
`MADV_REMOVE` without doing anything. Recycled arenas kept stale boot-image
string references, and new DexCache resolved-type arrays were carved from
them. Method resolution then treated a `"Alg.Alias.Signature..."` string as
a class (SIGSEGV at `0x2e676c51`). `MADV_REMOVE` now follows the kernel's
`madvise_remove`. QEMU 11.1 crashes the same way: it returns `EOPNOTSUPP`
for every `MADV_REMOVE`, so it is no reference for this path.

CMC needs userfaultfd in SIGBUS mode, and the boot image needs CMC: the
image was built for it, so under the CC fallback ART rejects the image and
starts from bytecode. Provisioning schema 5 extracts the `system_ext` and
`product` `build.prop` files; `product` sets `ro.dalvik.vm.enable_uffd_gc`.
Each compaction pause (`KernelPrepareRangeForUffd` in `mark_compact.cc`)
moves the moving space with `mremap(MAYMOVE|FIXED|DONTUNMAP)` into a
from-space reservation, registers the emptied range, and fills pages from
its SIGBUS handler with `UFFDIO_COPY`. The heap is in the low 4 GiB, the
from-space and the 1 GiB linear-alloc pool above it; since guest memory became
one host reservation (2026-10-07) both sides use the same per-page state. The short framework probe
never compacts; the Activity probe does.

### Activity creation checkpoint

`guest/android/ActivityProbe.java` creates a real Activity through `onCreate`.
It compiles against the checksum-pinned API-35 `android.jar` from
`tools/fetch-android-package-tools.py`; hidden APIs use reflection.

```bash
python3 tools/fetch-android-package-tools.py
make android-managed-launcher
timeout -s KILL 310 python3 scripts/test_android_framework.py --services \
  --probe build/android-managed-launcher/classes.dex --main-class ActivityProbe \
  --mode jit --timeout 300 --output build/android-activity-results
```

Both engines print `FRAMEWORK_OK` on a schema-5 root with CMC: 8.4 s JIT
and 18.0 s interpreter on an idle host (2026-10-07; 22.6 s and 45.0 s before
guest memory became one host reservation, when ART's heaps above 4 GiB used a
locked sparse map). The probe
calls `ActivityThread.systemMain()` and loads the preinstalled font map. It
then runs `Instrumentation.newActivity` and `callActivityOnCreate`.
`FRAMEWORK_OK` requires that `onCreate` ran and the decor view exists,
including the inflated title `TextView`.
`Typeface.loadPreinstalledSystemFontMap()` stands in for
`handleBindApplication`, which loads fonts when lazy Typeface initialization is
enabled. Missing system services are in-process Java Binder stubs installed in
`ServiceManager.sCache`. Each answers only the calls Activity creation makes,
logs them as `LOCAL_SERVICE`, and throws on anything else. Display returns one
480×800 display; package reports no features; settings returns empty values.
The `activity` and `activity_task` stubs return only the provider and Activity
controller. Window reports no navigation bar and animator scale 1.0. Activity
creation alone does not resume or attach a drawable window. The optional
`--play` path described below delivers additional lifecycle and surface events;
real system services remain unimplemented.

In installed-app mode the package service also answers `getServiceInfo` from
the APK's parsed service declarations, including service metadata requested
with `GET_META_DATA`. This supplies Firebase's component registrar discovery
without fabricating services. Replies copy the metadata and normalize the
ApplicationInfo to the installed package's paths and UID. Unknown components
or users return null (the framework raises `NameNotFoundException`); disabled
components and direct-boot matching follow the request flags. As in
`ComputerEngine.updateFlags`, requests without explicit direct-boot flags
match both kinds for this profile's unlocked user; Firebase's ordinary
`GET_META_DATA` lookup relies on this default. The
`APP_PACKAGE_SERVICES_OK` checkpoint checks every parsed service through the
framework API, metadata values and copy isolation, metadata omission without
the flag, implicit direct-boot queries, and missing-service/package behavior.

`resolveContentProvider` resolves the parsed APK's authority declarations,
including semicolon-separated authorities, with the same component filters
and copied metadata. Unknown authorities return null. Install-source queries
describe a host-installed APK with no Android installer of record; unknown
packages remain absent. The package checkpoint exercises these queries too.
`getRunningAppProcesses` returns this launcher's one app process with its real
guest PID/UID, manifest process name, package list and lifecycle importance;
`APP_PROCESS_LIST_OK` checks it before and after resume. These are launcher
profile services, not a full Android system-server process registry.

References: Android 15
[ApplicationPackageManager](https://github.com/aosp-mirror/platform_frameworks_base/blob/android15-release/core/java/android/app/ApplicationPackageManager.java),
[ComputerEngine](https://github.com/aosp-mirror/platform_frameworks_base/blob/android-15.0.0_r1/services/core/java/com/android/server/pm/ComputerEngine.java),
[PackageUserStateUtils](https://github.com/aosp-mirror/platform_frameworks_base/blob/android15-release/services/core/java/com/android/server/pm/pkg/PackageUserStateUtils.java),
and Firebase's
[ComponentDiscovery](https://github.com/firebase/firebase-android-sdk/blob/master/firebase-components/src/main/java/com/google/firebase/components/ComponentDiscovery.java).

### Framework display events (2026-10-07)

The Activity launcher profile publishes `SurfaceFlingerAIDL` with the Android 15
`ISurfaceComposer.createDisplayEventConnection` and `IDisplayEventConnection`
protocols. Each connection transfers the two native BitTube descriptors for a
nonblocking `SOCK_SEQPACKET` channel. Real `libgui` and `libandroid_runtime`
initialize `DisplayEventReceiver`, register it with the client's native Looper,
and dispatch Java Choreographer callbacks on that Looper's thread.

The single virtual display has a synthetic 60 Hz `CLOCK_MONOTONIC` cadence,
matching the existing NDK adapter's nominal rate. It sends initial hotplug,
coalesced one-shot vsync, and periodic vsync at the requested divisor; rate zero
disables periodic delivery. Events use Android 15's 216-byte layout with one
valid frame timeline, a presentation timestamp, and a deadline two milliseconds
earlier. These are virtual-display estimates, not host-compositor timing.
Backpressure never blocks the clock. Closed peers and collected connections
release their server descriptors. This is display-event support, not a complete
SurfaceFlinger: layer-bound connections, compositor transactions, display-mode
changes, and scheduling-policy queries remain unsupported.

Binder now transfers `BINDER_TYPE_FD` objects when the destination accepts FDs
(or the original caller sets `TF_ACCEPT_FDS` for a reply). Queued messages retain
the open Node; receiver descriptor numbers are installed on delivery, so closing
and reusing a sender FD cannot redirect the transfer. Rejected messages do not
install descriptors. Binder-device FD transfer and scatter/gather FD arrays
remain unsupported, as does the FdTable's pre-existing per-descriptor CLOEXEC
tracking gap. Device destruction happens outside the FdTable lock to avoid
inverting the Binder-domain/table lock order during concurrent close/transfer.

Native Looper disposal also exposed incorrect epoll marshalling: AArch64 events
are 16 bytes with data at offset 8; x86-64 events are packed to 12 bytes. Both
epoll wait paths now marshal the guest layout, and `EPOLL_CTL_DEL` ignores its
event pointer, including NULL, as Linux does.

The wire contracts were checked against AOSP Android 15:
[display receiver](https://android.googlesource.com/platform/frameworks/native/+/android-15.0.0_r1/libs/gui/DisplayEventReceiver.cpp),
[connection AIDL](https://android.googlesource.com/platform/frameworks/native/+/android-15.0.0_r1/libs/gui/aidl/android/gui/IDisplayEventConnection.aidl),
[BitTube](https://android.googlesource.com/platform/frameworks/native/+/android-15.0.0_r1/libs/gui/BitTube.cpp),
[vsync data](https://android.googlesource.com/platform/frameworks/native/+/android-15.0.0_r1/libs/gui/VsyncEventData.cpp),
and Linux's [epoll UAPI](https://github.com/torvalds/linux/blob/v6.12/include/uapi/linux/eventpoll.h).

Observed verification: the JIT framework probe checks hotplug, one-shot and
periodic delivery, rate zero, timeline data, callback cancellation, and two
independent Java Choreographer owners (`ACTIVITY_FRAMEWORK_VSYNC_OK`). These
self-checks do not run in `--play` mode. Four focused Binder/syscall fixtures
pass in both engines; the syscall fixture also passes natively as a kernel
oracle. The interpreter framework run remains failing: a worker reports an
unmapped NULL read and the callback check times out. SpongeBob still fails in
live JIT runs with guest `SEGV_ACCERR`, including with the new self-checks
disabled. The runner preserves Unity tombstones in its output directory.
Neither these checks nor this bounded Activity/service harness establish a
complete Android launcher or playable SpongeBob.

### Installed-app creation checkpoint (2026-10-07)

The same Activity probe can load a verified installed managed APK. It constructs
`LoadedApk` from the manifest, selects the ARM64 and feature splits using the
installer's actual filenames, creates the Application and calls its `onCreate`,
then attaches and creates the real launcher Activity. The runner requires
`APP_CLASSLOADER_OK`, `APP_APPLICATION_STARTED`, `APP_ACTIVITY_CREATED` and
`FRAMEWORK_OK` plus exit zero. `--installed-app` checks the existing installer
integrity gate; `--app` remains available for a manually prepared code directory.

After compiling ActivityProbe as above:

```bash
python3 scripts/test_android_framework.py --services \
  --probe build/android-managed-launcher/classes.dex --main-class ActivityProbe \
  --installed-app android-state/choreo-sensor/com.tiltingpoint.spongebob \
  --mode jit --timeout 150 --output build/android-spongebob-results
```

SpongeBob 5.11.5 now reaches its real CleverTap Application and Activity,
Unity's Java initialization and IL2CPP's `JNI_OnLoad` in JIT and interpreter
modes. The first fatal framework call was `getRequestedOrientation`; the local
Activity controller now tracks each token's manifest orientation and subsequent
requests. Its get/set behavior follows Android 15's
[ActivityClientController](https://android.googlesource.com/platform/frameworks/base/+/android-15.0.0_r1/services/core/java/com/android/server/wm/ActivityClientController.java).
The built-in probe checks a requested-orientation round trip.

Native libbinder then waited indefinitely for `servicemanager.ready`. Bifrost
now publishes it as true only when its Binder registry is selected by
`--android-services`, and false otherwise, matching libbinder's
[readiness gate](https://android.googlesource.com/platform/frameworks/native/+/android-15.0.0_r1/libs/binder/IServiceManager.cpp).
The Bionic gate reads the property with services both on and off and verifies
the Binder device and prctl path in all three execution modes and both ASLR
layouts. Its test environment is explicitly forwarded into the guest.

This probe exits immediately after Activity creation. It does not install
manifest content providers, start/resume the Activity, attach a drawable window,
or deliver input. Logs still contain missing user/clipboard services, SQLite
locking errors and missing SurfaceFlingerAIDL transaction failures. Those are
remaining compatibility gaps, not evidence of a playable game. Normal
`android_package.py run` now uses the managed launcher below; the creation-only
probe remains available for diagnostics.

#### Managed play diagnostic (2026-10-07)

For a white window after the splash, including loss of content after minimizing
and restoring, use `BIFROST_FRAME_TRACE=1 BIFROST_INPUT_TRACE=1` on the play
runner. `[present]` lines bracket host EGL swap/create/destroy/make-current,
terminate and guest-issued error queries with monotonic timestamps, guest thread
IDs, return values and call durations. An unmatched begin can locate a blocked
host call; a zero swap result is failure. The existing `[frame]` summary counts
calls, including failed swaps, and does not establish visible rendering.
`[android-window]` records SDL window events before filtering and a two-second
event-pump heartbeat. Event timestamps use SDL's clock; pump and EGL timestamps
use the host monotonic clock. These diagnostics observe restore/expose events;
they do not implement lifecycle recovery. Error state is not consumed by the
logger. Add `BIFROST_THUNK_TRACE=1` for the GL call stream. Avoid blanket
`BIFROST_TRACE=1` here: it also enables instruction logging and diagnostic modes
that change unsupported-instruction behavior.

Play-mode SurfaceView setup reports the holder's requested pixel format and
retains the same JNI Surface identity for the Activity and app. Following
[Android 15 SurfaceView.updateOpaqueFlag](https://android.googlesource.com/platform/frameworks/base/+/android-15.0.0_r1/core/java/android/view/SurfaceView.java),
formats without alpha request opaque composition independently of the app's
EGLConfig. The host EGL bridge carries this policy through
[EGL_EXT_present_opaque](https://registry.khronos.org/EGL/extensions/EXT/EGL_EXT_present_opaque.txt)
without changing framebuffer alpha or other creation attributes. Translucent
surfaces retain per-pixel alpha. Hosts without the extension can use alpha-free
configs; an opaque surface with alpha bits fails creation rather than exposing
the desktop. Changing opacity on a live EGL surface requires recreation and is
rejected with EBUSY. Dynamic SurfaceView replacement remains unsupported by the
play-mode traversal adapter.

Adding `--play` opens a host window, starts/resumes the Activity, reports
SurfaceView holder callbacks and pumps host input. These callbacks use the
managed graphics adapters; they are not a WindowManager attachment. The runner
also provisions `/storage/emulated/0`. A strict local `mount` service answers
`getVolumeList` with this single mounted, writable emulated volume for user 0
(no removable storage or other users), using Android 15's
[StorageVolume layout](https://android.googlesource.com/platform/frameworks/base/+/android-15.0.0_r1/core/java/android/os/storage/StorageVolume.java)
and [VolumeInfo semantics](https://android.googlesource.com/platform/frameworks/base/+/android-15.0.0_r1/core/java/android/os/storage/VolumeInfo.java).
Other storage service methods still throw. Installed-app mode checks the real
`Application.getExternalFilesDir(null)` path and its writability, and requires
the `APP_STORAGE_READY` checkpoint.

The SpongeBob play crash first appeared in `strlen(nullptr)` during PlayerPrefs
setup. A relocated Unity block trace and debugger read of its recorded Java
exception localized the earlier failure to `IStorageManager.getVolumeList`
on a null service. The Context global reference was valid; the pending exception
made Unity skip `getPackageName` method lookup and cache a null method ID.
Providing the volume makes that lookup return a nonzero method ID and clears
this crash. The JIT run then reaches a later native call to address zero
(Unity return offset `0xcd9234`). It also records the framework exception that
SharedPreferences requires a UserManager; whether that exception causes the
later native call failure remains unverified. This is not a gameplay pass.
The interpreter passes the external-directory checkpoint but still reports
unmapped worker reads. Play timeout alone is not success when a worker exception
or Unity fatal signal is logged; the runner now rejects both cases.

#### Signals at syscall entry (2026-10-07)

A white-screen investigation caught a linker write to `0x10` at
`ld-android.so+0xd88d4`, in `LinkerBlockAllocator::alloc` after `mmap(NULL, ...)`.
A later fault in IL2CPP's conservative collector (`libil2cpp.so+0x2b5eb08`)
read a protected page while other game threads waited in its suspend handler.
These observations do not establish that every white screen has the same cause.

Both engines advance PC past SVC before entering the syscall dispatcher. Its
pending-signal drain used to save that return PC even when it skipped the
unstarted syscall. After sigreturn, the caller saw its original X0 argument as
the result: NULL for mmap, a positive address for munmap, or an arbitrary ignored
argument for getpid. A signal-storm reproduction observed all three failures in
the interpreter; the JIT also aborted in libc's futex error path. The extended
`ctest/test_signal_semantics.c` fails against the old dispatcher in both engines
and passes with the fix and natively on Linux.

The dispatcher now delivers these signals before SVC, saving the instruction PC
and unchanged arguments so sigreturn executes the call. It restores SVC+4 if no
handler runs. This is distinct from interrupting a blocking syscall already in
progress, whose existing -EINTR and return-PC behavior is retained. Linux arm64's
[signal restart path](https://github.com/torvalds/linux/blob/v6.12/arch/arm64/kernel/signal.c#L1308)
likewise preserves original arguments and rewinds PC when restarting a call.
This is a syscall correctness fix; interactive SpongeBob gameplay remains an
independent acceptance check.

#### Binder registration ordering checkpoint (2026-10-07)

Native service registration now preserves the Binder stability word appended
by Android 15's `Parcel::finishFlattenBinder`, including null lookup replies.
Initial `BR_INCREFS`/`BR_ACQUIRE` notifications for an exported local Binder
are queued on the publishing thread before its completion/reply, following
[Linux `binder_translate_binder`](https://github.com/torvalds/linux/blob/v6.6/drivers/android/binder.c).
Previously the notifications went to the process looper queue while the
publishing thread received its reply first. The real libbinder registration
Parcel could then recycle its last strong reference while a pool thread
acquired the same object, aborting with `RefBase: ... strong count 1 deleted`.

`android_service_manager` checks ownership before reply, acknowledgements,
and split reads during service replacement; `android_binder_objects` checks
the same publishing-thread ownership path through a guest service manager.
Both pass in JIT and interpreter modes. ActivityProbe also registers/replaces
and looks up a real Java Binder eight times, checking its identity outside
the Java service cache (`ACTIVITY_SERVICE_REGISTRATION_OK`). Recompile its
DEX after updating the source; the runner requires that marker with services.
The full JIT Activity probe passes. The interpreter reaches both this marker
and `FRAMEWORK_OK`, but the runner rejects its existing null worker reads
(threads 6 and 8); reaching the markers alone is not a full interpreter pass.

#### Managed audio checkpoint (2026-10-07)

With `--play`, ActivityProbe registers `media.audio_policy` and
`media.audio_flinger` stand-ins. They answer only the Android 15 transactions
native AudioSystem makes here (policy 44/49/51/52/53, flinger 23/45/46; codes
checked against `IAudioPolicyService.aidl`/`IAudioFlingerService.aidl` at
`android-15.0.0_r1`): no HAL ports or patches, and the primary output's rate
and frames per buffer come from Bifrost's host Audio engine
(`APP_AUDIO_ROUTING_READY`). Playback itself never uses AudioFlinger: the
managed `libaaudio.so` adapter sends AAudio to the AudioThunk. Like real
libaaudio, the adapter runs each data callback stream on a Bionic pthread of
its own (`guest/android/aaudio.c`), created at open and joined at close; the
host callback pump cannot make Bionic TLS. That thread asks the thunk when the
host ring has room for one more callback (`bifrost_aaudio_wait_callback`),
calls the app, and hands the buffer back. `test_android_bionic.sh` checks
this in all three engine modes (`ctest/android_aaudio_thread.c`).

SpongeBob's FMOD now opens its AAudio stream and the callback thread feeds
the host engine. A 120-second JIT play run still presents zero frames: about
43 seconds in, Unity's main thread makes a virtual call through a null object
in `libunity.so` (address `0x6ab890`) and the process dies. The cause is not
known; it is not the audio path. The interpreter stalls earlier, after
PlayCore's `registerReceiverWithFeature`. These are diagnostic observations,
not a rendering or gameplay pass.

With CMC the boot image loads. Without userfaultfd ART falls back to CC,
rejects the image (`ValidateOatFile found read barrier state mismatch`) and
runs imageless. QEMU gets past the repaired mainline
namespace/native loading and aborts when framework initialization requires
`/dev/binder`, which this host reference does not provide. The probe proves a
framework initialization/event-loop checkpoint, not Activity launch, Android
service completeness, APK compatibility or booting a full Android system.

## Reproduce

The fixture needs Python 3, `debugfs` from e2fsprogs, Clang and LLD. It does
not require an Android SDK or download a complete system image.

```bash
make -j6
python3 tools/fetch-android-runtime.py
bash scripts/test_android_bionic.sh
```

The script also rebuilds the Android adapters and runs the managed AAudio
callback-thread test against them (`SDL_AUDIODRIVER=dummy`, no sound device).

The fetcher pins the official AOSP `prebuilts/runtime` APEX at revision
`8aeb37cca394ce39c1311744c60960cbd466aa77` (the `android-15.0.0_r1` repository
revision), checks archive and extracted-file SHA-256 hashes, retains its
license notices, and installs only linker64, libc, libdl, libdl_android and
libm under `tools/android-runtime/rootfs`. A repository tag is provenance;
these reused prebuilts alone do not establish an API 35 platform build.
The directory is ignored by Git. An existing verified installation is reused;
modified runtime files are rejected rather than overwritten.

The test compiles a 16 KiB-aligned ARM64 fixture against these actual libraries,
then exercises fixed/random ASLR with interpreter, default JIT and opt-in Tier 2.
It checks constructors, initial auxv, Bionic page size, pthread creation/join,
independent ELF TLS and errno, runtime dlopen/dlsym/dlclose, a libm result,
Binder device opening/mapping and no-new-privileges through Bionic wrappers.
It requires a success marker and rejects fatal-signal/crash output, since a
guest fatal handler can otherwise exit with code zero.

Manual invocation after building the fixture:

```bash
./bifrost-emu --android-runtime --rootfs tools/android-runtime/rootfs \
    /data/local/tmp/bootstrap
```

This bootstrap command requires an absolute guest executable path and a
rootfs containing `/system/bin/linker64` (or its runtime-APEX equivalent).
The guest linker owns relocation, library resolution and TLS. Bifrost does
not create its Linux custom DynamicLinker or synthetic TLS block for this
profile. ELF load bias respects PT_LOAD alignment; interpreter reservation
protects holes and prevents later mmap allocations overlapping the image.

## Experimental same-image ART bootstrap

The managed-runtime experiment uses a separate root from the small Bionic
bootstrap and the Media NDK namespace. It extracts ARM64 ART, Bionic, libcore,
ICU, Conscrypt, timezone data and native dependencies from the official
[API-35 ARM64 SDK image, revision 2](https://dl.google.com/android/repository/sys-img/android/arm64-v8a-35_r02.zip).
Its build identity is `AE3A.240806.019/12368160`, with image SHA-256
`1e6ae4fca6449c07278a333311f0ef29c5101d1e3e18ba48fbe9bd2dc5006926`.
The provisioner verifies the archive and all 529 extracted file hashes,
symlink targets and executable flags. Besides the DT_NEEDED closure it extracts
the image's `public.libraries.txt` preloads (libnativeloader opens them at VM
startup) and their dependencies. It keeps the upstream notices and records the
dependency graph and preload list in `bifrost-art.json` (schema 2; older
runtime directories must be re-provisioned). The generated linker config gives
each boot APEX its own namespace linked to one shared default namespace, so
JNI loads never create a second libart. This is a bootstrap artifact set,
without a framework installation or boot image.

```bash
python3 tools/android_art.py
python3 tools/build-android-jni.py --javac /path/to/javac
BIFROST_NO_ASLR=1 python3 scripts/test_android_jni.py --timeout 60
python3 scripts/test_android_jni.py --mode qemu   # qemu-aarch64 reference run
```

Requirements: Python 3.11+, `debugfs`, `lpunpack`, `readelf`, Clang/LLD and a
JDK 17+. The image download is approximately 734 MiB; extraction needs several GiB
of scratch disk space. An existing verified Media NDK source archive is reused.
The DEX compiler is checksum-pinned D8 from AOSP `android-15.0.0_r1`.
Generated runtime files and fixtures stay under ignored `build/` directories.

The runner copies the verified runtime into a temporary root, adds writable
data and fixtures, enforces a timeout, and saves logs plus JSON results in
`build/android-jni-results/`. The launcher calls the real `JNI_CreateJavaVM`
with ART JIT disabled, CheckJNI enabled and a small 16/64 MiB heap. The fixture
is intended to load an ARM64 JNI library, register a native method, access an
array/string, call back into Java and shut down cleanly. Success requires
`ART_LOAD_OK`, `ART_VM_OK`, the fixture's marker and a zero exit; ART logs go
to stderr (`-Xuse-stderr-logger`). `--mode qemu` runs the same root under
`qemu-aarch64` as a reference: with this pinned image it reaches `ART_VM_OK`
and passes in about a second. Bifrost passes too, in about 25 s under the
JIT; dispatch between short blocks (~50% of samples) and interpreter
fallbacks (~28%) dominate the remaining time.

October 5 checkpoint: real ART creates its VM (`ART_VM_OK`) and the DEX/JNI
fixture passes in both JIT and interpreter modes. Getting there needed the
pairwise-widening SIMD ops (`UADALP` and family), 16× index scaling for
`ldr/str q, [xn, xm, lsl #4]` (it corrupted the class linker's method
records), a real `rt_sigtimedwait` (ART's signal catcher spun on the EAGAIN
stub) and the `FUTEX_CMP_REQUEUE` requeue count (ART's condition-variable
broadcast never woke the main thread). GC, thread attachment, exceptions and
managed Activity startup remain unverified.
This experiment neither registers a fake JavaVM nor changes the existing
native-only APK launch contract. JNI objects and calls remain guest-owned,
consistent with [Android's JNI guidance](https://developer.android.com/ndk/guides/jni-tips).

The experiment exposed and repaired ordinary mmap hint handling, ARM64 open
flag translation, executable procfs opens and main-stack metadata. Low hints
try a free range at or above the requested address below 4 GiB,
then use the ordinary allocator when none fits. They never replace occupied
mappings; fixed mapping behavior remains separate. Virtual hinted reservations
discard old contents lazily. `/proc/self/stat` reports the actual initial guest
SP, needed by [Bionic's main-stack lookup](https://android.googlesource.com/platform/bionic/+/refs/tags/android-15.0.0_r1/libc/bionic/pthread_internal.cpp).
Other stat accounting fields remain approximate or zero, and maps permissions
remain the existing coarse view. `/proc/self/exe` follows the selected executable
file; `O_PATH|O_NOFOLLOW` symlink descriptors are explicitly unsupported, and
reopening an executable after it is deleted/replaced is not modeled.

## Pointer tags and page sizes

Bionic's Scudo allocator uses top-byte tags internally even without MTE.
The Android profile ignores the top byte on data accesses, preserves the
pointer's register value, and keeps mapping/protection checks on the resolved
address. It covers direct/sparse memory, atomics, thunk pointer translation and
forked memory state. Instruction addresses are not untagged. This is data TBI,
not MTE support or permission to claim memory-tag checking works.

Runtime pages remain 4096 bytes. The fixture uses 16384-byte ELF alignment;
the real Android linker, pthreads and dlopen work with this combination in
Bifrost and QEMU. This agrees with [AOSP's documented support for 16 KiB
alignment on 4 KiB and 16 KiB kernels](https://source.android.com/docs/core/architecture/16kb-page-size/16kb).
A 16 KiB runtime requires coherent syscall, allocator and protection semantics,
not an auxv-only change.

The Binder transport and syscall-option checkpoint is documented in
[ANDROID_BINDER.md](ANDROID_BINDER.md), including its unsupported operations.

## APK inspection

```bash
python3 tools/fetch-aapt2.py
python3 scripts/android_preflight.py /path/to/package.apkm --output report.json
python3 scripts/test_android_preflight.py
```

The preflight tool reads manifests/splits, DEX inventory, ARM64 native imports,
entry points and PT_LOAD alignment. It never runs the package. An alignment
report is not a compatibility verdict or signature verification.

SpongeBob 5.11.5 requires a managed `CleverTapOverrideActivity` launcher and
Android framework services despite using IL2CPP for its engine. All 21 packaged
ARM64 libraries have 16 KiB alignment. Its installed-app creation probe above
now executes DEX and JNI startup; the native-load diagnostic alone does not
establish those capabilities or gameplay compatibility.

## Additional verification

```bash
bash scripts/test_android_loader.sh
./scripts/run_tests.sh --filter 'simd_addlv|crc32_instructions'
./scripts/run_tests.sh --no-jit --filter 'simd_addlv|crc32_instructions'
```

`android_loader_host` checks aligned image reservation, malformed ELF rejection,
tagged data access/protection behavior and exact CRC opcode validity.
`jit_simd_addlv` covers 2,624 signed/unsigned widening reductions and pairwise
adds, including operand aliases and all valid widths. `jit_crc32` covers
2,048 IEEE/Castagnoli cases with an independent bitwise oracle. Both instruction
fixtures also pass QEMU and Tier-1 differential verification.

After audio fix `9403911`, the strict ART-foundation Linux suite passes 275/275
in default JIT mode and 268/268 in interpreter mode, with seven expected
JIT-only skips. AAudio passes in both full runs. The earlier native
checkpoint passed 273/273 and 266/266 respectively. All six Bionic bootstrap
variants pass after the mmap/procfs changes; see [test evidence](TESTS.md).
The subsequent AAudio wake-up fix passes 20 callback stress runs, including
injected delayed waits in both engines, and four neighboring audio/thread tests
per engine. This resolves a reproducible lost-notification race without raising
the guest fixture's callback deadline. The older interpreter checkpoint's
267/268 result remains recorded in [test evidence](TESTS.md).


### First SpongeBob rendering milestone (2026-10-07)

This supersedes the earlier managed-audio checkpoint's unknown null-object
failure. Unity's aligned stack frame was broken by ignored AND-to-SP writes;
logical immediates now honor SP/WSP in both engines. The next crash came from
GLES returning a host mapped-buffer pointer to guest code; GLES now uses the
existing guest-buffer map/unmap/flush policies. See [JIT.md](JIT.md).

ActivityProbe now implements the newly observed calls explicitly:

- `reportActivityFullyDrawn` records the first completion for a known Activity
  token; unknown tokens are ignored, as in AOSP's ActivityRecord lookup.
- `getMyMemoryState` fills PID/UID and documented process-state fields, using
  the actual Activity `isResumed()` state for foreground importance. The
  single-process profile has no trim callbacks or OOM-adjustment dependencies.
- `handleApplicationCrash` records the supplied crash report. RuntimeInit still
  terminates an uncaught-exception process; crash reporting does not hide it.
- `power` answers the implemented window/interactivity and disabled power-saving
  queries. `thermalservice` reports no samples, NONE status and unavailable
  (NaN) thermal headroom; it retains listeners, delivers the initial status,
  and supports removal. This describes the virtual device, not host sensors.
- `mount.mkdirs` checks the installed package and canonical path, then really
  creates directories below its external data/obb/media roots. Other package
  paths and traversal outside those roots are rejected.

Contracts were checked against AOSP `android-15.0.0_r1`:
[ActivityClientController](https://android.googlesource.com/platform/frameworks/base/+/android-15.0.0_r1/services/core/java/com/android/server/wm/ActivityClientController.java),
[ActivityManagerService](https://android.googlesource.com/platform/frameworks/base/+/android-15.0.0_r1/services/core/java/com/android/server/am/ActivityManagerService.java),
[StorageManagerService](https://android.googlesource.com/platform/frameworks/base/+/android-15.0.0_r1/services/core/java/com/android/server/StorageManagerService.java),
and the `IPowerManager`/`IThermalService` interfaces. Unimplemented methods still
throw; this is a bounded launcher profile, not a complete system_server.

Observed verification: the framework probe passes in JIT. Both engines pass
its explicit process/power/thermal-listener and crash-report checks; the full
interpreter probe still rejects the existing null reads in workers 6 and 8.
The real-audio SpongeBob run renders animation (670 frames in the captured
run), then hits unhandled `frecps v3.4s,v16.4s,v18.4s` (0x4e32fe03) and guest
SIGILL. SQLite locking errors are another separate remaining gap. Neither
animation nor passing service checks establishes interactive gameplay.
The play runner now also recognizes Java `FATAL EXCEPTION` lines as guest
failures, including the two earlier automatic-exit logs.

The reciprocal-family follow-up clears the FRECPS native SIGILL (see
[JIT.md](JIT.md#reciprocal-estimates-and-refinement-spongebob-2026-10-07)).
The installed game now remains alive for the 120-second bounded run and
submits 540 frames, but the user reports a white screen after animation and
frame submission stops. `sensorservice` is absent from the native Binder
registry, and Unity's native thread repeatedly waits for it. The existing
libandroid ASensorManager thunk's empty sensor list does not satisfy calls
through the framework's native SensorManager. An absent service differs
from a present service reporting an empty list: AOSP
[SensorManager](https://android.googlesource.com/platform/frameworks/native/+/master/libs/sensor/SensorManager.cpp)
waits for the service during construction. No sensor-service implementation
is included in the reciprocal patch. A subsequent GDB trace confirms guest
thread 19 in `waitForSensorService` (`libsensor.so+0x11574`), called by
`getInstanceForPackage` (`+0x11080`); both return PCs match the actual
library's disassembly. This service wait blocks that Unity thread.

### Native sensor-service profile (2026-10-07)

ActivityProbe now publishes `sensorservice` through the native Binder registry
when `--services` is selected. Its `android.gui.SensorServer` Binder implements
Android 15's static, dynamic and per-device list queries with empty lists, and
the three injection-mode queries with disabled results. The descriptor,
transaction numbers, String16 inputs and raw count/bool replies were checked
against AOSP
[ISensorServer.cpp](https://android.googlesource.com/platform/frameworks/native/+/android-15.0.0_r1/libs/sensor/ISensorServer.cpp).
This manual C++ protocol has no AIDL exception prefix. Interface tokens and
trailing data are checked; event/direct connections, operation parameters and
other unsupported transactions return UNKNOWN_TRANSACTION. There is no sensor
HAL, sensor sample generation, event queue or direct channel in this profile.

The probe checks registry discovery, the six query replies, rejection of an
unsupported connection, bad interface tokens and extra query arguments. It also
constructs the framework's real SystemSensorManager and calls through JNI into
libsensor to query static/dynamic lists and default accelerometer/gyroscope.
`ACTIVITY_SENSOR_SERVICE_OK` is required by the framework runner with services
enabled. This verifies that native clients discover the service, rather than
only finding a Java service-cache entry. See [test evidence](TESTS.md#native-sensor-service-follow-up-2026-10-07).


## Managed APK launcher (2026-10-08)

`ManagedLauncher` contains the Activity bootstrap previously exercised through
`ActivityProbe --play`. Normal app launches use a real ActivityThread, LoadedApk,
Application and Instrumentation, the verified installed base/splits, and the
app's launcher Activity. Diagnostic registration tests and probe preferences
are disabled during ordinary launches. `ActivityProbe` remains a compatibility
entry point for the diagnostic checks.

```bash
make android-managed-launcher
python3 scripts/android_package.py --state-root android-state run PACKAGE
# Optional bounded run and title statistics, without a separate stats window:
python3 scripts/android_package.py --state-root android-state run PACKAGE \
  --timeout 180 --window-stats --no-window-stats-window
```

The explicit build needs `javac`, Java and the provisioned API-35 android.jar/D8.
Set `JAVAC=/path/to/javac` if needed. Launching never invokes a compiler: stale
launcher sources or modified DEX/adapter files cause an error. The framework and
ART roots must already be provisioned as described above.

Each package owns `managed/rootfs`, `managed/last-run.log` and
`managed/last-logd.log` under its installed package directory. The runtime is
cached by code, framework, adapter and launcher hashes; immutable runtime files
are checked before reuse. Replacement copies `data/user`, `data/user_de` and
`storage` before publishing the new root, preserving saves across runs and
runtime upgrades. The installed package's verified root is kept separate.
A temporary short root alias avoids Linux's Unix-socket pathname limit without
making app data temporary. A per-package lock covers preparation and the entire
session. The supervisor forwards interruption, terminates owned descendants,
removes its log socket, and propagates guest failure; a requested timeout returns
124 and interruption returns 130. Normal runs have no time limit.

The managed launcher collects the APK's real signing certificates through
Android's archive parser. Its package service returns legacy `signatures` only
for `GET_SIGNATURES` and `signingInfo` only for `GET_SIGNING_CERTIFICATES`, with
independent package records and certificate arrays. The diagnostic app probe
checks both query flags and queries without them against the installed APK.
This follows AOSP's
[PackageParser signing fields](https://android.googlesource.com/platform/frameworks/base/+/android-15.0.0_r1/core/java/android/content/pm/PackageParser.java).
Previously the service returned a manifest-only record with null certificates,
including when SDKs requested them. [Facebook Unity's initialization callback](https://github.com/facebook/facebook-sdk-for-unity/blob/sdk-version-18.0.0/facebook-android-wrapper/src/com/facebook/unity/FB.java)
reads `signatures` to calculate its key hash before sending `OnInitComplete`;
a null array interrupts that callback.

Activity metadata queries now use the installed manifest, preserving aliases,
orientation, enabled/direct-boot filters, and `GET_META_DATA` behavior, as service
and provider queries do. Permission checks grant only the installed app's
requested normal permissions, using protection levels parsed from the runtime's
`framework-res.apk` and the app's own permission definitions. Unknown names,
other UIDs/devices, runtime permissions and signature permissions are denied;
runtime grant dialogs and signature grant policy are not implemented. Package
permission flags agree with this bounded policy. See Android's
[permission types](https://developer.android.com/guide/topics/permissions/overview#types_of_permissions).
An isolated call into SpongeBob's actual Facebook SDK exposed
`package.getActivityInfo`, then `activity.checkPermissionForDevice` throwing
before initialization; with both services implemented, `isInitialized()` returns
true and its asynchronous initialization callback runs. This verifies SDK startup
independently of game loading and resize safety.

Managed launches and ActivityProbe share the installed split-argument builder.
The installer assigns `split-N.apk` after sorting all manifests by split name,
while the saved inventory retains archive order. Launch arguments use that same
complete sorted list before excluding other ABIs, retaining filename-index gaps.
Enumerating archive order previously paired some asset-pack names with unrelated
APKs (for example `gamedata` with the fonts APK), so Play Asset Delivery could
recognize the pack name but fail to locate its local asset files.
Observed with SpongeBob's actual SDK: `getAssetLocation` returned null for
`gamedata` and `fonts` under the old arguments. The corrected arguments return
the corresponding installed APK paths, offsets and sizes. The regression covers
archive reordering and retaining the index of an excluded 32-bit split.

Window close and Activity.finish drive pause, SurfaceView destruction, stop,
queued preference writes and destroy before releasing the native surface.
This is a bounded SurfaceView launcher, not a complete Android system_server or
WindowManager implementation: arbitrary framework View drawing, dynamically
replacing SurfaceViews, every service, and every Activity transition are not
implemented. Unsupported service calls remain explicit failures.

The managed window uses the launcher Activity label and icon from the APK's
real resources (with Android's application/default fallbacks). Android renders
the icon into ARGB pixels; the JNI bridge supplies standard UTF-8 and RGBA to
the host. SDL3 applies title/icon updates on its window thread. Compositor icon
support varies; window plugins without the optional identity operations retain
their own defaults. The native-only launcher still uses its existing identity.
Android EGL presentations now feed the shared [window statistics](window-stats.md)
without passing SDL3 pointers through SDL2 APIs. The title is the default;
the independent stats window remains opt-in.

The signed managed fixture verifies two launches, persistent preferences,
resource label resolution, a real SurfaceView, Activity.finish and ordered
pause/surface destruction/stop/destroy/close. These checks do not establish
compatibility with arbitrary managed games.

### ART field lookup restart correction

A SpongeBob crash in ART's Class::FindField came from a post-index
`ldr w8,[x1],#4` on a userfaultfd missing page. The interpreter advanced X1
before the memory read. After the SIGBUS handler filled the page, retry loaded
an ArtField's declaring-class pointer as the array length and walked out of
bounds. JIT guarded-access recovery uses this interpreter path too.
Indexed scalar/vector and paired transfers now commit base writeback only after
the access succeeds. `test_userfaultfd` reproduces the exact header retry and
checks pre/post-index stores, loads and page-crossing pair/vector accesses in
both engines. This fixes the observed corruption; an info-button click is a
separate live interaction check, not evidence supplied by the memory regression.


The post-fix SpongeBob live run presented more than 2,000 frames over a
220-second session, rendered the age dropdown and consent screen, and reached
the subsequent loading screen. The user confirmed the info button opened
without crashing. This verifies that interaction; completing a gameplay session
remains unverified. The native-only launcher now acquires its owned window via
private launcher operation 7 rather than passing a null Java Surface to the
public ANativeWindow_fromSurface API.


The diagnostic ActivityProbe still reaches FRAMEWORK_OK in JIT mode after the
launcher promotion. Its interpreter-only framework-vsync check currently fails
with an unmapped null read in the callback thread and a vsync timeout. The older
pre-promotion ActivityProbe DEX fails the same check on the current emulator,
so this is not established as a launcher-promotion regression. A separate live
SpongeBob restart also encountered ART's thread-suspension timeout before its
first frame. These failures remain unresolved; the successful age/info session
does not establish reliable startup on every run.
