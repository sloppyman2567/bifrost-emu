# Asgard Android compatibility layer

Asgard is the Android layer of Bifrost: guest Bionic/ART, framework adapters,
APK installation, app sessions, and shared Binder communication. Existing
`android_package.py` commands remain supported. The Asgard entry point also
starts one shared Binder broker per state root:

```sh
python3 Scripts/Android/asgard.py --state-root android-state realm start
python3 Scripts/Android/asgard.py --state-root android-state start com.example.app
python3 Scripts/Android/asgard.py --state-root android-state list
python3 Scripts/Android/asgard.py --state-root android-state realm status
# Stop app sessions first; stopping the realm disconnects all its clients.
python3 Scripts/Android/asgard.py --state-root android-state realm stop
```

### Shared guest processes (October 11, 2026)

Independent emulator OS processes can join the same Binder realm using
`--android-binder-broker SOCKET`. The broker owns separate `/dev/binder`,
`/dev/hwbinder`, and `/dev/vndbinder` domains. `/dev/binder` includes the native
service registry; the other domains accept guest context managers. Receive
arenas are shared memfds mapped read-only in each guest. Transaction payloads
are copied through bounded RPCs; a pointer from one process is never dereferenced
in another process. Real host-backed descriptors use SCM_RIGHTS, and Binder
objects are translated to receiving-process handles and back to their original
pointer/cookie when returned to their owner.

Broker-connected Android processes use distinct host process IDs, matching
process-leader TIDs, and their configured `--android-app-id` credentials.
Thread clones retain their process identity; process-directed signals find the
actual leader rather than a hard-coded TID 1. Closing the owning connection removes
its registrations, delivers registered `BR_DEAD_BINDER` cookies and returns
`BR_DEAD_REPLY` for abandoned synchronous calls.
Blocking client calls observe guest cancellation. A fork child must open a new
Binder connection; an inherited connection rejects operations with EBADF.
The private socket checks the host UID, and endpoint keys are randomly generated.
Limits are 128 Binder opens, 1024 thread identities per open, 16 MiB per RPC,
256 memory chunks and 64 descriptors, with at most 512 simultaneous broker
connections and a five-second initial packet deadline. RPC streams are reused,
with up to eight idle channels per Binder open. Wire protocol 2 adds SG FD-array
patch widths and buffer cleanup; stop app sessions and restart older realms
before using the new executable. Transaction and arena limits of the local
Binder implementation still apply. This is a same-host-user compatibility
mechanism; virtual Android UIDs do not provide host DAC or SELinux isolation.

Managed service children (`ManagedLauncher --service-child`, including
WebView's `webview_service` and sandboxed renderers) share the session's
`/dev/binder` registry. Only the session process publishes the device services
`sensorservice` and `SurfaceFlingerAIDL`; children use the session's objects.
Previously a child re-registered both, and its exit delivered a death
notification to the app's libsensor client: SpongeBob's Unity main thread
then waited for `sensorservice` indefinitely after a WebView ad process
stopped. `ManagedServices` checks that the entry stays a local Binder while a
`:remote` child runs; it reports a `BinderProxy` without the fix.

`python3 Scripts/Android/test_asgard.py` builds a Bionic fixture and checks all four
JIT/interpreter server/client combinations, distinct sender identities,
leader PID/TID equality, worker-thread identity and main-thread signal delivery,
descriptor contents, Binder-object round trips, nested scatter/gather request and
reply buffers, FD-array cleanup, death subscriptions/acknowledgements, server
death, and broker restart. It also checks 1,000 operations on one persistent
stream, the focused looper/death/SG contracts in both engines, and eight
concurrent clients completing 1,600 verified RPCs. Domain metadata remains
serialized; payload staging and endpoint wakeups avoid holding/waking the whole
domain for that work. See [Binder contracts and remaining limits](ANDROID_BINDER.md).
The existing native service-manager fixture also passed 78 checks through the
broker in each engine. Read-only arena use, descriptor translation and dead-reply
behavior were compared with Android 15's
[ProcessState](https://android.googlesource.com/platform/frameworks/native/+/refs/tags/android-15.0.0_r1/libs/binder/ProcessState.cpp)
and [Binder driver](https://android.googlesource.com/kernel/common/+/refs/heads/android15-6.6/drivers/android/binder.c).
These tests establish native cross-process IPC, not complete Android application
process management. Manifest remote/isolated service launch and Chromium renderer
spawning now use fresh ART VMs; Zygote, process-wide framework service publication, arbitrary
virtual-device descriptor transfer, and SELinux policy remain unsupported.
Broker restart creates a fresh registry; live clients must reopen their Binder
connections. Staging pages are retained until the corresponding Binder open
closes; the RPC size limit is not a lifetime memory quota.

Game validation: a normal-chaining, host-GPU SpongeBob run completed its
600-second bound without the previous ART abort/monitor-ownership exception.
A subsequent broker-connected run reached Activity resume, SurfaceView creation
and WebView loading, then hit an ART `art_method.cc` virtual-method-index
assertion during GC. That managed-app failure remains under investigation;
passing native IPC tests does not establish working multi-process WebView or
playable SpongeBob.
After repairing the separately reproduced process-leader PID/TID mismatch,
a fresh broker-connected run reached its 120-second timeout without that
assertion. This shorter run does not establish the mismatch as the assertion's
cause or prove long-term stability.

## Android native runtime bootstrap

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

### App audio, notification, Wi-Fi and power services (2026-10-13)

`ManagedLauncher` installs four explicit method contracts from
`FrameworkAudio`, `FrameworkNotifications`, `FrameworkWifi` and
`FrameworkPower`. Unknown operations still throw. Their enums drive both
dispatch and `ManagedLauncher --service-inventory`: JSON lists each registered
Java service's actual image interface signatures as `implemented`,
`unsupported` or `unclassified`. Existing lambda services are unclassified;
native Binder endpoints are outside this inventory. Registration rejects a
declared method absent from the provisioned interface.

Audio focus follows the Android 15
[MediaFocusControl](https://android.googlesource.com/platform/frameworks/base/+/android-15.0.0_r1/services/core/java/com/android/server/audio/MediaFocusControl.java)
and FocusRequester stack rules: permanent/transient/duck losses, restoration,
duplicate requests, abandonment and client death. Callbacks are ordered and
delivered outside the state lock. This is cooperative focus within a guest VM;
it does not impose host mixer ducking, system-wide focus across independent VMs,
locked audio policies or telephony behavior. `isMusicActive` queries real host
output streams. Native `media.audio_policy` and `media.audio_flinger` publish
before app initialization, only from the session owner. Android 15 policy
transactions 8/80/82 and flinger 3/5/6 add stream-output, empty strategy/group
lists, sample-rate, frame-count and estimated output-quantum latency queries.
`AudioTrack.getMinBufferSize` now traverses those real native clients. This
does not implement AudioFlinger track creation or AudioTrack PCM playback;
AAudio and OpenSL ES play through the host audio adapter instead.

Notification channels/groups support creation, copied queries, app-allowed
updates, deletion and resurrection with preserved settings, following
[PreferencesHelper](https://android.googlesource.com/platform/frameworks/base/+/android-15.0.0_r1/services/core/java/com/android/server/notification/PreferencesHelper.java).
Metadata, including delegate selection/revocation, is stored below each app's
data directory in `.asgard-notifications/channels`. Atomic replacement and a
file lock serialize writers across processes; a JVM lock handles overlapping
local instances. The format contains Android 15 Parcel data and is tied to
that provisioned framework version. Limits are 5,000 channels, 6,000 groups
and 8 MiB. Package/user checks apply, channel creation requires no
`POST_NOTIFICATIONS` grant, and an uninstalled delegate is ignored, as in
NotificationManagerService. Runtime notification permission is not invented.
Desktop delivery, legacy automatic default-channel creation and notification
administration remain unsupported.

Wi-Fi uses the same host-route snapshot as connectivity. Registering the
service also advertises `FEATURE_WIFI`, required by WifiFrameworkInitializer
to construct the public manager. Connection details remain redacted, scans
and DHCP data are empty, and radio-changing operations return false. The scan
list uses the actual image interface's Parcelable return type, including
Mainline package relocation. Scan callbacks have bounded registration and
death cleanup; without a scan backend they produce no events. Access/change
permissions and package checks apply. `getVerboseLoggingLevel`, queried during
manager construction, is permission-free as specified by Android's WifiManager.

Power supports permission-checked, token-owned wake locks, reference counting
through the public PowerManager client, state callbacks, attribution checks
and death cleanup. It retains the virtual device's interactive/power-save
profile; wake locks do not change host suspend policy.

Build and run the focused real-framework clients (`java` and `javac` from an
installed JDK must be on `PATH`):

```bash
python3 - <<'PY'
from pathlib import Path
import sys
sys.path.insert(0, 'Scripts/Android')
from android_managed import build_launcher
build_launcher(Path('/tmp/bifrost-service-probe'), (Path('Tests/Host/ManagedServiceBatch.java'),))
PY
python3 Scripts/Android/test_android_framework.py --services --managed-graphics \
  --probe /tmp/bifrost-service-probe/classes.dex --main-class ManagedServiceBatch \
  --output /tmp/bifrost-service-jit
python3 Scripts/Android/test_android_framework.py --services --managed-graphics \
  --probe /tmp/bifrost-service-probe/classes.dex --main-class ManagedServiceBatch \
  --mode interp --timeout 90 --output /tmp/bifrost-service-interp
python3 Scripts/Android/test_android_framework.py --services \
  --probe /tmp/bifrost-service-probe/classes.dex --main-class ManagedLauncher \
  --probe-arg=--service-inventory --output /tmp/bifrost-service-inventory
```

The default transport is the shared broker. `--binder-mode local` isolates the
same native Binder contracts in one process; it does not validate cross-process
transport. The focused probe uses real NotificationManager, AudioManager,
AudioFocusRequest, AudioTrack queries, PowerManager and WifiManager. Token
death in this probe is simulated; transport death is tested separately by the
Binder suite. A bounded SpongeBob run also calls Unity's
`registerNotificationChannel` successfully through the production service.
Neither these tests nor presented frames establish video-ad playback: the
`libmedia_jni.so`, decoder and video-surface path remains unfinished.

### Common device services (2026-10-13)

Android 15's
[SystemServiceRegistry](https://android.googlesource.com/platform/frameworks/base/+/android-15.0.0_r1/core/java/android/app/SystemServiceRegistry.java)
builds many managers with `ServiceManager.getServiceOrThrow`, or looks the
Binder up in the manager constructor. Without the service,
`getSystemService` logs `ServiceNotFoundException` and returns null; Hungry
Shark Evolution then dereferenced a null `UiModeManager`, and session logs held
40 null `PermissionManager` dereferences from `PackageManager.checkPermission`.
Eleven services now let the real managers construct. Each declares its
contract enum; other methods throw. Semantics follow the linked Android 15
server classes, for one app, user 0 and the default device:

| Service | Model |
|---|---|
| `uimode` | Normal UI mode, night mode off and locked, contrast 0 (`config_defaultNightMode`, `config_lockDayNightMode`). Process, Activity and bind configurations carry `uiMode` 0x11. Per-app dark mode throws: it needs a configuration change. |
| `permissionmgr`, `permission_checker` | Delegate to the install-time grants (normal permissions granted, runtime permissions denied), retained framework-res/app permission definitions, no user rationale, no policy revocation, Android 15's 24 `platform.xml` split permissions, attribution registration. Proxy chains through other uids are hard-denied. |
| `batteryproperties`, `batterystats` | healthd's no-battery AC profile: absent battery, AC plugged, unknown status, level/capacity 100, unknown charge/current, not charging. `registerReceiver` returns the sticky `ACTION_BATTERY_CHANGED` intent and delivers it to a new receiver. A host laptop battery is not reflected. |
| `vibrator_manager` | No vibrators; `VIBRATE` is enforced and accepted vibrations are ignored. |
| `trust` and `window` keyguard calls | No credential and never locked; `DISABLE_KEYGUARD` gates keyguard locks, `exitKeyguardSecurely` succeeds. |
| `clipboard` | In-process clipboard with copied `ClipData`, one-hour auto-clear and listeners; reads need the play window's focus. Not shared with the host desktop. |
| `alarm` | One-shot `OnAlarmListener` alarms on a scheduler thread, at least 5 s ahead (`MIN_FUTURITY`), RTC converted to elapsed time; exact-alarm queries follow the target-SDK rules. PendingIntent alarms throw (no PendingIntent creation yet); alarms never restart a stopped process. |
| `account` | No accounts or authenticators (no Play services); secret reads and explicit additions are rejected. |
| `location` | Only the passive provider, location setting on; location data needs a runtime location grant and has no source. |

`notification` also accepts `cancelAll`, `cancel` and `getActiveNotifications`
for its own package: nothing is posted, so they find nothing. Clipboard and
alarm checks construct a focused clipboard and wait for a real listener alarm.
`ManagedServiceBatch` exercises these through the real managers; see
[the test record](../TESTS.md#common-device-services-2026-10-13).

### Software Gralloc5 (2026-10-13)

The native service manager now implements `isDeclared` and
`getDeclaredInstances` for the virtual allocator HAL. `GraphicsAllocator.cpp`
implements the frozen `android.hardware.graphics.allocator.IAllocator` V2
contract, including interface version/hash, mapper suffix, `allocate2` and
`isSupported`. It is owned by the Binder realm rather than a managed app: a
cached libui allocator Binder survives another app closing. The legacy mapper4
descriptor method returns `UNSUPPORTED`; malformed descriptions and unsupported
formats are not turned into successful dummy allocations.

`mapper.bifrost.so` implements the Android 15 stable-C mapper v5 ABI. The adapter
builder fetches checksum-pinned AOSP mapper/native-handle headers and the managed
runtime installs the library at `/vendor/lib64/hw`. The visible default linker
namespace includes that location for `android_load_sphal_library` fallback.
Allocations have one memfd containing a metadata prefix, linear pixels and an
optional reserved region. Grow/shrink seals prevent truncation while imported;
the mapper validates the transported layout and file size, duplicates the fd,
and creates an independent guest `MAP_SHARED` mapping for every import. The
shared transport is defined in `Include/Bifrost/AndroidGralloc.h`.

Supported one-layer formats are RGBA/RGBX/BGRA 8888, RGB 888, RGB 565, RGBA FP16,
RGBA 1010102, R8, BLOB (height 1), flexible YUV420 and YV12 (planar Y,V,U with
aligned chroma strides). IMPLEMENTATION_DEFINED selects RGBA 8888. Dimensions,
per-buffer bytes, batch count and reserved bytes are bounded. Protected buffers,
front-buffer rendering, cube/mipmap layouts and vendor options/usages are
rejected. This software allocation support does not imply native host-GPU import.
The removed HW_2D/EXTERNAL_DISP allocation hints (bits 10 and 13) remain
accepted, matching libui's explicit compatibility allowance. The mapper lock
entry point requires CPU usage flags only, as required by the stable-C API.

The mapper implements all 23 Android 15 standard metadata queries, transport
size, reserved regions, import/free, lock/unlock and metadata dumps. Dataspace,
blend and bounded HDR metadata are shared between imports using atomic
per-field sequence snapshots. A conflicting or interrupted metadata writer
returns `NO_RESOURCES` after bounded retries, avoiding an indefinite wait on
a dead process. CPU memory is coherent and unlock returns no release fence;
incoming acquire fences are waited on and closed by the mapper.
Invalid lock requests close their fence before returning an error, without
waiting for it to signal; valid waits do not hold the mapper registry mutex.

Source-checked contracts:
[IAllocator V2](https://android.googlesource.com/platform/hardware/interfaces/+/refs/tags/android-15.0.0_r1/graphics/allocator/aidl/aidl_api/android.hardware.graphics.allocator/2/android/hardware/graphics/allocator/IAllocator.aidl),
[stable-C IMapper](https://android.googlesource.com/platform/hardware/interfaces/+/refs/tags/android-15.0.0_r1/graphics/mapper/stable-c/include/android/hardware/graphics/mapper/IMapper.h),
[metadata encoding](https://android.googlesource.com/platform/hardware/interfaces/+/refs/tags/android-15.0.0_r1/graphics/mapper/stable-c/implutils/include/android/hardware/graphics/mapper/utils/IMapperMetadataTypes.h),
and [libui's Gralloc5 client](https://android.googlesource.com/platform/frameworks/native/+/refs/tags/android-15.0.0_r1/libs/ui/Gralloc5.cpp).

Observed in real-framework tests: all eleven explicit formats allocate,
describe, lock and round-trip through the NDK Unix-socket handle API. Duplicate
imports retain buffer identity and pixels after releasing the original import.
The two YUV layouts expose three planes. Protected, multi-layer and unknown
formats are rejected. A real `AImageReader` receives two buffers queued through
`ANativeWindow_lock`/`unlockAndPost`; `acquireLatestImage` selects the latest,
all 32x18 RGBA pixels match, and its timestamp is positive. JIT and interpreter
pass through the shared broker. Two independent framework VMs with UIDs 10002
and 10003 also pass, including allocating again through a cached Binder after
the peer exits; UID 10004 allocates after both previous clients have exited.
An independent Java `allocate2` Parcel request with legacy hints and packed
1x1 RGB888 pixels verifies a word-aligned 64-byte reserved region, sharing it
through duplicate imports; shared dataspace updates, malformed metadata rejection
and non-CPU lock rejection are also checked.
The existing raw Binder process fixture now independently checks shared-memfd
mmap writes in both directions across different UIDs in all four engine pairs.

Build and run the focused probe:

After updating the emulator, an already-running shared broker still executes
its old binary. Stop its app sessions before restarting that realm with
`asgard.py --state-root STATE realm stop` and `realm start`. The focused runner
starts a fresh temporary broker unless `--binder-broker` is supplied.

```bash
python3 Tools/Android/build-android-adapters.py
make android-managed-launcher
python3 Scripts/Android/test_android_framework.py --services --managed-graphics \
  --probe build/android-managed-launcher/classes.dex \
  --main-class 'ActivityProbe$GrallocProbe' --output /tmp/gralloc-jit
python3 Scripts/Android/test_android_framework.py --services --managed-graphics \
  --probe build/android-managed-launcher/classes.dex \
  --main-class 'ActivityProbe$GrallocProbe' --mode interp --timeout 90 \
  --output /tmp/gralloc-interp
python3 Scripts/Android/test_asgard.py
```

For concurrent clients use `--binder-broker SOCKET --app-id UID` on each
framework runner. `GrallocProbe --probe-arg 15` prints
`ANDROID_GRALLOC_CLIENT_READY`, waits 15 seconds, then repeats the native checks
with the same cached libui allocator. Start the peer after that marker and
require it to exit before `ANDROID_GRALLOC_PEER_EXIT_OK` in the survivor.

This completes the allocation/mapping milestone only. Managed `libandroid`
still needs dispatch between opaque Bifrost windows and real libgui windows,
including `ANativeWindow_toSurface`; the probe deliberately uses only native
window entry points that already reach the platform library. EGL native-buffer
import/external textures, `libmedia_jni`, MediaCodec callbacks, video/audio
decoding and in-game playback remain unfinished. Two full games running
concurrently have not been validated.

The normal-JIT untraced Trickcal check on this change (session
`fa1bca4868624ab98eee6d2341638179`) reached Unity and failed after about 69 s
with exit 127. Its saved `logd.log` contains Scudo's `corrupted chunk header`
on guest thread 11, reproducing the previously documented failure. The run
does not verify a video opening or any in-game playback.

### Android app credentials (2026-10-09)

`--android-runtime` uses UID/GID 10000 (Android's first application ID, user 0),
independent of the host account. The single-app profile reports that identity
through getuid/geteuid/getgid/getegid, getresuid/getresgid, supplementary groups,
the ELF auxiliary vector, `/proc/self/status` and native Binder sender credentials.
Managed ApplicationInfo and PackageManager replies take their UID from the real
guest Process API. Android apps have no effective/permitted/inheritable
capabilities. ID changes outside the fixed identity fail with EPERM; setfsuid
and setfsgid retain it and return the previous ID.

Previously the syscall path returned the host UID, typically 1000, while auxv
and procfs claimed root. Android interprets 1000 as the system UID. SpongeBob's
LevelPlay SDK hit WebView's explicit privileged-process rejection, and ContextImpl
selected system-process warnings. The app ID range and WebView check were verified
against Android 15 [Process](https://github.com/aosp-mirror/platform_frameworks_base/blob/android-15.0.0_r1/core/java/android/os/Process.java)
and [WebViewFactory](https://github.com/aosp-mirror/platform_frameworks_base/blob/android-15.0.0_r1/core/java/android/webkit/WebViewFactory.java).
This fixes the process identity. Schema 6 adds the same-image WebView provider;
its current execution limits are described below.

ART also enforces read-only dynamic DEX for ordinary app UIDs. The managed
runtime and framework harness copy their launcher/probe DEX with mode 0444;
managed runtime schema 3 rebuilds older writable copies while retaining app data.
The native Bionic gate checks IDs, auxv, procfs, groups, rejected ID/capability
changes and same-ID operations in JIT, interpreter and Tier-2, with fixed/random
ASLR and Binder services on/off. Package contracts pass 52 checks in each engine;
the signed service APK passes 38 lifecycle checks and exercises WebView's
ordinary unsupported-device gate without the privileged-process error.

This is a fixed identity in each independently launched app runtime, not a
multi-package UID allocator or filesystem ownership/DAC virtualization. Linux
guest identity behavior is unchanged, including host UID reporting for D-Bus.

### Provisioned framework

The same pinned API-35 image now supplies `app_process64`, framework boot jars,
and the mainline APEX native libraries. Framework provisioning schema 6
verifies 923 files and exports a linker namespace for every installed boot
APEX, including `com_android_tethering`. Each delegates to the same canonical
native-library scope so JNI loading does not instantiate a second ART VM.
Schema 4 adds `framework-res.apk`, `fonts.xml`, `font_fallback.xml` and
`/system/fonts`: `Resources.getSystem()` and text layout need them before an
Activity draws. Schema 5 adds the `system_ext` and `product` `build.prop`
files (see the CMC note below). Schema 6 also includes `/vendor/build.prop`
and the product WebView APK/native libraries. Vendor properties are loaded
between system_ext and product, following Android 15
[PropertyLoadBootDefaults](https://android.googlesource.com/platform/system/core/+/android-15.0.0_r1/init/property_service.cpp).
The pinned image specifies a 192 MiB app growth limit and 512 MiB maximum;
omitting these properties made
[AndroidRuntime](https://github.com/aosp-mirror/platform_frameworks_base/blob/android-15.0.0_r1/core/jni/AndroidRuntime.cpp)
use its 16 MiB default. The WebView probe observed `Runtime.maxMemory()` as
192 MiB after the change. This bootstrap layout does not reproduce production Android
namespace isolation. Older framework roots must be re-provisioned
in a fresh destination; preserve the old directory if it is still useful for
comparison.

### WebView startup checkpoint (2026-10-10)

The managed runtime registers the pinned image's genuine `com.android.webview`
124.0.6367.219 package, signatures, metadata and service declarations. Its APK
is read-only for ART. Package queries and feature enumeration expose that
registered provider; reads of visible system-component overrides are allowed,
while component changes remain restricted to the installed app.
`webviewupdate.waitForAndGetProvider` returns the real package with AOSP's
`LIBLOAD_FAILED_WAITING_FOR_RELRO` status: there is no zygote-created shared
RELRO, and [WebViewFactory](https://github.com/aosp-mirror/platform_frameworks_base/blob/android-15.0.0_r1/core/java/android/webkit/WebViewFactory.java)
accepts that status and loads the provider without it.

Application binding records both the native process name and the framework's
`AppBindData` before creating the Application, following
[ActivityThread](https://github.com/aosp-mirror/platform_frameworks_base/blob/android-15.0.0_r1/core/java/android/app/ActivityThread.java).
The launcher checks `Application.getProcessName()` against that binding.
Previously only `setArgV0` was called, leaving the Java name null and breaking
Chromium's data-directory lock.

For this unmanaged, single-user profile, application restrictions return a
fresh empty Bundle for the calling app, following
[UserManagerService](https://github.com/aosp-mirror/platform_frameworks_base/blob/android-15.0.0_r1/services/core/java/com/android/server/pm/UserManagerService.java).
Other apps/users and policy writes are denied. `FrameworkDisplay` owns window
context tokens for display 0: attachment returns its bounds/density configuration,
resize posts configuration changes to the framework client controller, and
release detaches the token. Unknown callers/displays cannot attach. This follows
the attachment contract in
[WindowManagerService](https://github.com/aosp-mirror/platform_frameworks_base/blob/android-15.0.0_r1/services/core/java/com/android/server/wm/WindowManagerService.java);
it does not implement arbitrary window creation or additional display areas.

The provider's native AAudio imports resolve through the managed adapter.
Usage and input-preset setters are validated at open and retained by the stream.
`AAudioStream_getTimestamp` follows the
[AAudio contract](https://android.googlesource.com/platform/frameworks/av/+/android-15.0.0_r1/media/libaaudio/include/aaudio/AAudio.h):
not-started streams return `INVALID_STATE`, invalid clocks are rejected, and
the SDL backend returns `UNAVAILABLE` for hardware presentation timestamps,
preserving both outputs. Mixer-consumption counters are not hardware timestamps.
The Bionic callback/timestamp regression passes in JIT, interpreter and Tier-2.

```bash
python3 Scripts/Android/test_android_framework.py --services \
  --probe build/android-managed-launcher/classes.dex --main-class ActivityProbe \
  --probe-arg=--webview --managed-graphics --timeout 60 \
  --output build/android-webview-results
```

This opt-in probe exercises provider class loading, cookies, user-agent startup,
and construction/destruction of a real WebView using an app `LoadedApk` context.
The system context's singleton
resources cannot serve as a WebView embedding app. The harness also inspects
logd for fatal native signals and Chromium's uncaught JNI exceptions;
successful process exit or `FRAMEWORK_OK` alone
does not pass this probe.

The launcher now starts fresh ART
VMs for remote and isolated Services, including Chromium's named renderer
instance, and publishes their real service Binders through the shared realm.
With normal JIT chaining, the bounded `--webview-renderer` probe completes a
local HTML navigation and evaluates its DOM expression to `"asgard:42"`
(`ACTIVITY_WEBVIEW_RENDERER_DOM_OK`). After the renderer's visual-state callback,
it draws the WebView into an Android bitmap and checks distinct CSS foreground
and background pixels (`ACTIVITY_WEBVIEW_RENDERER_PIXELS_OK`). The combined
probe passed in 23.9 seconds on October 11. This offscreen software drawing
probe has no attached ViewRoot and does not establish visible game/WebView GPU
compositing.

```bash
python3 Scripts/Android/test_android_framework.py --services \
  --probe build/android-managed-launcher/classes.dex --main-class ActivityProbe \
  --probe-arg=--webview-renderer --managed-graphics \
  --library build/android-adapters/libbifrost-activity.so --timeout 100 \
  --output build/android-renderer-pixels
```

The managed libandroid forwards ALooper and AChoreographer to the platform's
real libandroid (`re_android.so`), so Java MessageQueue and Chromium's
[MessagePumpAndroid](https://chromium.googlesource.com/chromium/src/+/124.0.6367.219/base/message_loop/message_pump_android.cc)
share the same thread-local libutils Looper, following
[AOSP's ALooper implementation](https://android.googlesource.com/platform/frameworks/base/+/android-15.0.0_r1/native/android/looper.cpp).
`ActivityProbe$NativeLooperProbe` passes main/worker Java-driven eventfd callbacks
in JIT and interpreter modes (`ANDROID_JAVA_NATIVE_LOOPER_OK`). Standalone native
adapters retain the host looper implementation.

An app library resolves imports only through libraries its classloader
namespace can access: the managed `libandroid.so`, not its private
`re_android.so` dependency (Bionic `find_libraries` skips inaccessible
libraries). The adapter therefore exports every other public `LIBANDROID`
function from AOSP's pinned `libandroid.map.txt` (android-15.0.0_r1) as a
generated forwarder resolved through `re_android.so`. Azur Lane's CRIWARE
`libcri_ware_unity.so` needed `AAsset_seek`. Window-handle functions
(`ANativeWindow_*`, `ASurfaceControl_createFromWindow`) keep their thunked or
wrapped behaviour; `ANativeWindow_toSurface` remains unexported.

Unix `sendmsg`/`recvmsg` SCM_RIGHTS now translate guest descriptors into referenced
host descriptors and register newly received descriptors in the guest table.
Receive copyback failures close transferred descriptors. `socket_msg_layout`
passes native Linux, JIT and interpreter checks for deliberately divergent FD
numbers, duplicate transfers, shared mmap contents, seals and CLOEXEC. The
`sendmmsg`/`recvmmsg` ancillary paths remain outside this fix. The renderer also
exposed the halfword atomic encoding/result bug documented in [JIT.md](../JIT.md).

The managed EGL adapter exports the Android 15 public EGL symbol inventory.
It filters host extensions to supported guest contracts, implements blob-cache
callback registration without handing Arm callbacks to a host driver, and
provides the monotonic NV system clock. Android platform/window entry points
use the existing owned host display/window connection. Native-buffer allocation,
native-fence FD export, pixmaps, EGLStreams and presentation timestamps remain
unsupported and their extensions are not advertised. Symbol coverage is not
EGL conformance. The host-GPU `ActivityProbe$EglProbe` passes registration,
duplicate/invalid-argument errors, proc-address identity, platform selection,
clock and terminate/reinitialize checks (`ANDROID_EGL_CONTRACT_OK`). Generated
EGL signatures also pass `make opgen-thunk-check egl-check`.

### Input methods (October 11, 2026)

`FrameworkInputMethod` publishes the process-local `input_method` service using
Android 15's [IInputMethodManager contract](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-15.0.0_r1/core/java/com/android/internal/view/IInputMethodManager.aidl).
It records clients' UID/PID, reported display and fallback connection, rejects
duplicate process/display registrations, and removes dead clients. As in AOSP's
[ClientController](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-15.0.0_r1/services/core/java/com/android/server/inputmethod/ClientController.java),
untrusted display IDs are accepted during registration and checked when input
starts. Display 0 is the only available display. Start-input validates the client, user and editor
package, records the window/editor, and returns the framework's `NO_IME`,
`NULL_EDITOR_INFO`, `NULL`, `INVALID_DISPLAY_ID` or `INVALID_PACKAGE_NAME` result as appropriate.
Asynchronous starts deliver `onStartInputResult` with the caller's sequence on a
service HandlerThread. IME tracking has bounded token lifetime/storage;
privileged mutation and test methods are denied. Unknown methods fail explicitly.

There is no installed/selected Android IME in this image. Inventories and
subtypes are empty, keyboard visibility stays hidden, show/hide return false
and notify their ResultReceiver with `RESULT_UNCHANGED_HIDDEN`, and IME height
is zero. This follows the no-selected-method path in
[InputMethodManagerService](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-15.0.0_r1/services/core/java/com/android/server/inputmethod/InputMethodManagerService.java).
It does not install an on-screen keyboard or implement IME binding/sessions,
WindowManager focus arbitration, ViewRoot input stages, or cross-process Java
framework services. Host key events still dispatch directly to the Activity;
full Android text composition is unimplemented.

Actual WebView construction also exposed `Settings.Config`'s null-name
`LIST_config` call. The immutable Settings provider now returns a fresh empty
serialized map under `value`, matching
[SettingsProvider](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-15.0.0_r1/packages/SettingsProvider/src/com/android/providers/settings/SettingsProvider.java),
so DeviceConfig consumers apply their defaults. It does not invent flag values.

The focused probe runs on real ART in both engines and checks service
publication, inventory copy isolation, invalid clients/displays/users/packages,
synchronous and asynchronous start results, ResultReceiver visibility and
privileged-operation rejection:

```sh
python3 Scripts/Android/test_android_framework.py --services \
  --probe build/android-managed-launcher/classes.dex \
  --main-class 'ActivityProbe$InputMethodProbe' --timeout 90 \
  --output build/android-input-method-probe/protocol-jit
# Repeat with --mode interp and a separate output directory.
```

Both modes passed, and the genuine pinned WebView constructed and destroyed
successfully in the JIT Activity probe. These observations do not establish
renderer-process startup, HTML rendering, or working text entry in SpongeBob.

The initial input-method build also ran SpongeBob with normal JIT and the
protocol-2 broker for the requested 400 seconds (session
`1f9e865ddeca4176a24109b2242d2b3b`). It reached the timeout (`124`), with no ART
abort, fatal JNI exception, or missing `IInputMethodManager` error in stdout or
logd. The host process opened `/dev/dri/renderD128` through Mesa EGL. Chromium
renderer-service connection failures, Chartboost's `window.getWindowInsets`,
and AppLovin's `audio.isMusicActive` failures remain. Loading beyond 40% and
gameplay were not visually verified. The subsequent duplicate-client and
deferred-display validation refinements passed both contract probes and real
WebView construction; the 400-second session preceded those refinements.
The JIT startup probe passes provider loading, cookie-manager creation, user-agent
retrieval, restriction isolation and window-context attachment/release in 15.1 s.
It does not test renderer execution, page loads or gameplay. One preceding probe
timed out in the independent framework-vsync check; the two subsequent startup
runs passed, so repeated vsync reliability remains unproven.

```bash
python3 Tools/Android/android_framework.py
mkdir -p build/framework-probe/classes
javac --release 8 -d build/framework-probe/classes Guest/Android/Probes/FrameworkProbe.java
java -cp Tools/android-tools/d8.jar com.android.tools.r8.D8 --min-api 26 \
  --output build/framework-probe build/framework-probe/classes/FrameworkProbe.class
timeout -s KILL 165 python3 Scripts/Android/test_android_framework.py --mode jit --timeout 150
timeout -s KILL 165 python3 Scripts/Android/test_android_framework.py --mode interp --timeout 150
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

Private file mappings now use host `MAP_PRIVATE` backings too. Previously
`mmap` copied their bytes into anonymous storage, so `MADV_DONTNEED` erased
file contents instead of refaulting them. Trickcal 1.6.1 exposed this while
loading Unity: a discard covered the initializer at `libunity.so+0x5804b8`,
and execution reached zero instructions instead of the installed library's
`adrp`. File backings now survive descriptor close, partial replacement and
ordinary remap, including private COW bytes on a move. The low/high cases
in `Tests/Unit/test_madv_remove.c` fail on the old implementation and match a
native Linux run in both Bifrost engines after the fix. Private-file
`MADV_REMOVE` now returns `EACCES`, also checked against the kernel.
File-backed `MREMAP_DONTUNMAP` remains unsupported; moving private files
currently copies their contents into the new file mapping rather than
moving host page tables. This regression does not cover file truncation
or interpreter accesses beyond EOF.

The GPU-enabled Trickcal rerun passed the former zero-code failure and reached
Unity's product initialization and `SwappyWrapperInit`. It then exited in
`JavaBBinder::onTransact` with `env null. Attach JVM?`, before a game screen
was verified. The launcher publishes Java implementations of native services
inside the app process; direct calls from native workers without a JVM
attachment can therefore reach Java Binder directly. A bounded guest-stack
capture confirmed the failing path: Swappy's native worker calls
`Choreographer::getForThread`, then `DisplayEventReceiver` and
`BpSurfaceComposer::createDisplayEventConnection`, reaching
`BBinder::transact` and the Java Binder fatal check on the same thread.
The activity JNI bridge now registers its `JavaVM` with the managed adapter
before these services are published. `AChoreographer_getInstance` attaches
an otherwise unattached native owner as a daemon and detaches it at pthread
exit; an attachment owned by ART or the app is left alone. The platform
Looper remains shared with Java MessageQueue. This follows Android's
[JNI thread lifetime guidance](https://developer.android.com/ndk/guides/jni-tips).
`ActivityProbe$NativeChoreographerProbe` starts two independent Bionic pthreads,
checks their initially detached state, and requires three increasing frame
timestamps on each owner before joining it. With the old adapter this path
reproduced the Java Binder fatal check; the repaired adapter passes in JIT.
Run with `--services --managed-graphics` and the current launcher DEX.

The next GPU-enabled run reached rendering and a user-confirmed splash/loading
screen, then crashed in the image's `libnativewindow.so` at
`ANativeWindow_setFrameRate+0x28`. The saved instruction bytes identified that
function, and X0 held Bifrost's opaque native-window handle. Swappy resolves
this export through `libandroid.so`; without an adapter export, lookup reached
the real platform function, which dereferenced the handle as a native struct.
Both frame-rate setters now use the window thunk registry, validate live
handles and rate/compatibility/strategy arguments, and accept the advisory
hint without switching the host display mode. This matches the documented
[NDK frame-rate hint contract](https://developer.android.com/ndk/reference/group/a-native-window)
and AOSP's non-SurfaceFlinger destination behavior. The existing surface test
passes 66 checks, including rendered pixels and these setters, in both engines.
The AVProVideo plugin also could not resolve `ANativeWindow_toSurface`; this
separate missing export remains open. Startup fixes and a visible splash do
not establish Trickcal gameplay or resolve SpongeBob's intermittent stall.

The Android profile leaves guest page 0 unmapped, as Linux does
(`mmap_min_addr`). ART's implicit null checks need that fault: its SIGSEGV
handler converts it into `NullPointerException`. Bifrost's Linux profiles
still map a readable zero page for legacy guests. With it mapped, nterp read
a zero method entry point from a null `ArtMethod*` and jumped to address 0.
`/proc/self/maps` lists every guest mapping in the kernel's format with its
real permissions; file-backed pages carry their offset, device, inode and
resolved guest path (`linker64` appears as `/apex/com.android.runtime/bin/linker64`).
It omits an empty `[heap]`, so lines never overlap. Bionic takes the first
line containing the initial SP as the main stack; an old 256 MiB `[interp]`
guess could win.
ART then computed a negative `madvise` length below the stack. `madvise` now
returns `EINVAL` for wrapping ranges like Linux `do_madvise`; it previously
zeroed every page above the start, including libc text. This caused the
layout-dependent early SIGILL recorded in [test evidence](../TESTS.md).

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

`Guest/Android/Managed/ActivityProbe.java` creates a real Activity through `onCreate`.
It compiles against the checksum-pinned API-35 `android.jar` from
`Tools/Android/fetch-android-package-tools.py`; hidden APIs use reflection.

```bash
python3 Tools/Android/fetch-android-package-tools.py
make android-managed-launcher
timeout -s KILL 310 python3 Scripts/Android/test_android_framework.py --services \
  --probe build/android-managed-launcher/classes.dex --main-class ActivityProbe \
  --mode jit --timeout 300 --output build/android-activity-results
```

Both engines print `FRAMEWORK_OK` on a schema-5 root with CMC: 8.4 s JIT
and 18.0 s interpreter on an idle host (2026-10-07; 22.6 s and 45.0 s before
guest memory became one host reservation, when ART's heaps above 4 GiB used a
locked sparse map). The probe
calls `ActivityThread.initializeMainlineModules()` before preparing the main
Looper, matching Android 15's `ActivityThread.main()`, then calls
`ActivityThread.systemMain()` and loads the preinstalled font map. Mainline
initialization installs per-process managers for telephony, media, Bluetooth
and other modules; it does not provide their system services. The regression
calls `TelephonyManager.getNetworkCountryIso()` and requires an empty country
on this device without a radio, rather than a null-manager exception. It
then runs `Instrumentation.newActivity` and `callActivityOnCreate`.
The mainline regression (2026-10-10) reproduced the SDK's null-manager
exception before the initialization call and passed with it under JIT.
The interpreter run failed earlier in `FrameworkVsyncProbe0`, so this
checkpoint does not establish interpreter coverage. A subsequent SpongeBob
run stopped with a host SIGSEGV at 1%; passing the probe does not establish
successful game login.

Android SDK startup exposed two further stack failures (2026-10-10).
Native JIT guest calls now have a bounded host frame depth; see `JIT.md`.
The next captured exit was a child of raw `clone` with a zero stack argument:
the emulator replaced its SP with zero. Like arm64 Linux
[`copy_thread`](https://github.com/torvalds/linux/blob/v6.12/arch/arm64/kernel/process.c#L350-L369),
`spawn_thread` now retains the inherited SP unless the supplied stack is
nonzero. `test_fork_threads` checks ten such clones and their stack reads,
alongside the existing fork-with-live-workers checks. This does not add
`CLONE_VFORK` suspension or full process isolation for shared-VM children.
The subsequent SpongeBob trace captured `clone(0x4111, 0, ...)` (vfork) on
the AdQuality SDK thread, followed by the child's `exit_group(-1)` ending
the whole emulator with status 255. Shared-VM clones without `CLONE_THREAD`
now return `ENOSYS` in both clone APIs instead of entering the thread
backend. This is an explicit compatibility limitation, not vfork support;
Linux suspends the caller until the child releases its address space and
gives the child its own thread group (see
[`kernel_clone`](https://github.com/torvalds/linux/blob/v6.12/kernel/fork.c)).
The regression exercises the captured flags and child `exit_group(7)`:
the old backend killed the test with status 7, while the corrected backend
rejects the unsupported operation and preserves the parent. QEMU checks the
supported legacy child-exit path and reports clone3 as unavailable.

The next real-game failure was AdMob initialization querying
`ActivityManager.getHistoricalProcessExitReasons`: the strict service stub
threw `UnsupportedOperationException`, and RuntimeInit killed the app with
SIGKILL (status 137). The service now returns an empty `ParceledListSlice`
for the caller's package/UID on user 0, matching
[AOSP's empty-result shape](https://github.com/aosp-mirror/platform_frameworks_base/blob/android-15.0.0_r1/services/core/java/com/android/server/am/ActivityManagerService.java#L9519-L9550).
Foreign package/user queries are denied. This launcher has no persistent
exit-history tracker; previous host runs are not reported as Android exit
records. The framework probe checks the public API with caller/default
filters and a denied foreign-package query (`ACTIVITY_EXIT_HISTORY_OK`).
Validation: the JIT framework probe passed with that marker, and eight
focused JIT/thread/shared-memory regressions passed. The combined-patch
SpongeBob run reached its 240-second timeout with repeated ART GC before
AdQuality initialization; the timeout sent SIGTERM (guest status 143,
runner status 124). It did not exercise the exit-history query in the game,
so it does not establish successful SDK initialization or gameplay.

CursorWindow shared memory uses libcutils' memfd path: `sys.use_memfd`
defaults to true (an image override is retained), and `F_ADD_SEALS` /
`F_GET_SEALS` forward to the host descriptor with host errors preserved.
libcutils still probes kernel support for `F_SEAL_FUTURE_WRITE`; unsupported
hosts do not receive fake success. This removes the dependency on a host
ashmem driver for this path. The framework probe creates a CursorWindow,
writes/reads a value, and requires `ACTIVITY_SHARED_MEMORY_OK`. The fcntl
regression checks seal readback and rejected resizing against native Linux
and QEMU. See AOSP
[`ashmem-dev.cpp`](https://android.googlesource.com/platform/system/core/+/android-15.0.0_r1/libcutils/ashmem-dev.cpp).
`BIFROST_PROCESS_TRACE=1` logs non-thread shared-VM clone inputs and guest
group exits (`[clone]`, `[guest-exit]`) to distinguish child-process lifecycle
errors from memory-access faults without stopping threads in a debugger or
changing the JIT memory-guard trace path. `BIFROST_FAULT_TRACE` includes these
process records too.

`FRAMEWORK_OK` requires that `onCreate` ran and the decor view exists,
including the inflated title `TextView`.
`Typeface.loadPreinstalledSystemFontMap()` stands in for
`handleBindApplication`, which loads fonts when lazy Typeface initialization is
enabled. Missing system services are in-process Java Binder stubs installed in
`ServiceManager.sCache`. Each answers only the calls Activity creation makes,
logs them as `LOCAL_SERVICE`, and throws on anything else. Display returns one
480×800 display; package reports no features. Settings reads return absent
values except for the installed app's scoped Android ID, described below.
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

### Managed framework service modules (2026-10-09)

The managed launcher separates the local Binder registration into
[`FrameworkServices.java`](../../Guest/Android/Managed/Framework/FrameworkServices.java), manifest-backed
package state into [`FrameworkPackages.java`](../../Guest/Android/Managed/Framework/FrameworkPackages.java),
and host-network state and callbacks into
[`FrameworkConnectivity.java`](../../Guest/Android/Managed/Framework/FrameworkConnectivity.java).
`ManagedLauncher` retains Activity lifecycle, windows, input and native wire services.
The explicit launcher build and freshness check hash every module; after changing
any of them, run `make android-managed-launcher` before launching an installed app.

The package service now supports receiver/provider component lookup, installed
package/application enumeration, package-to-UID and UID-to-package queries. This
profile exposes its one installed APK, including its parsed receivers and splits;
unknown packages and UIDs remain absent. Queries enforce the installed user and
`MATCH_SYSTEM_ONLY`. Component replies and package component arrays also honor
manifest/default/overridden enabled state and explicit direct-boot filters.
Component state updates reject nonexistent classes, invalid states and foreign
users; returning to DEFAULT restores the manifest state. Overrides last for this
launcher process; persistent package settings and package-change broadcasts are
not implemented.

`getPackageInfo` omits component arrays, signatures, permissions, instrumentation,
configuration data and GIDs unless their query flags request them. Application
and component metadata, shared-library information and provider URI patterns
follow their separate flags. No matching components yields null arrays, as AOSP's
`ArrayUtils.trimToSize` does. The installed record and query replies cross a
Parcel copy boundary, so modifying returned arrays, metadata or ApplicationInfo
cannot change later queries or the launcher's installed snapshot. The local Binder
proxy implements identity equality/hash codes without sending Object methods to a
service handler. Unknown service operations still throw explicitly.

Focused regression gate (requires the provisioned Android framework and JDK):

```bash
make android-managed-launcher
python3 Scripts/Android/test_android_package.py --emu ./bifrost-emu FrameworkPackageContractTests
```

This compiles `Tests/Host/FrameworkPackagesProbe.java` alongside the production
modules into a separate temporary DEX, then runs the real Android PackageManager
client in both engines. Results and logs are saved under
`build/android-framework-package-contract/`. The suite exercises query flags,
copy isolation including nested arrays, missing entries, receiver/provider and UID
lookups, installed lists, disabled/default component transitions, direct-boot
filters and user isolation. Service execution has a separate APK regression gate
below. Intent-filter resolution, multiple installed packages/users and a full
system_server remain outside this profile.

Observed verification: 51 package contract checks pass in each engine, and the
related launcher/network/settings/DNS test selection passes 11 tests. The managed
APK fixture completes three launches with persistent preferences and the expected
resume/surface/pause/stop/destroy order. SpongeBob's real launcher still reaches
`onCreate` in JIT mode with 23 services, 19 metadata entries and 13 provider
authorities checked. Full interpreter ActivityProbe runs still fail in the
previously documented `FrameworkVsyncProbe0` null-read/vsync path before app
creation; the focused package gate avoids that unrelated path. These results do
not establish full framework completion or gameplay.

Reference contracts were checked against Android 15's
[IPackageManager](https://github.com/aosp-mirror/platform_frameworks_base/blob/android-15.0.0_r1/core/java/android/content/pm/IPackageManager.aidl),
[PackageInfoUtils](https://github.com/aosp-mirror/platform_frameworks_base/blob/android-15.0.0_r1/services/core/java/com/android/server/pm/parsing/PackageInfoUtils.java),
[PackageManagerService](https://github.com/aosp-mirror/platform_frameworks_base/blob/android-15.0.0_r1/services/core/java/com/android/server/pm/PackageManagerService.java),
[PackageUserStateUtils](https://github.com/aosp-mirror/platform_frameworks_base/blob/android-15.0.0_r1/services/core/java/com/android/server/pm/pkg/PackageUserStateUtils.java),
and [ArrayUtils](https://github.com/aosp-mirror/platform_frameworks_base/blob/android-15.0.0_r1/core/java/com/android/internal/util/ArrayUtils.java).

### Managed Android Service execution (2026-10-09)

[`FrameworkServiceRuntime.java`](../../Guest/Android/Managed/Framework/FrameworkServiceRuntime.java)
implements started and bound Services for explicit components in the installed
APK's main process or a fresh remote ART VM. It manages service tokens, start IDs, intent-filter binding
records and client connections. Android's real `ActivityThread` creates the app's
Service class with its Application, class loader and attached Context, and runs
`onCreate`, `onStartCommand`, `onBind`, `onUnbind`, `onRebind` and `onDestroy` on
the main Looper through `IApplicationThread`. The framework's `ServiceDispatcher`
delivers the actual Binder returned by `onBind`, including null bindings and
binding-death notifications.

`startService`, `stopService`, `stopSelf`/`stopSelfResult`, `bindService`,
`unbindService` and `peekService` now execute this lifecycle. Repeated starts
reuse the instance and receive increasing start IDs; an old start ID cannot stop
a newer request. Binding identity uses `Intent.FilterComparison`, so extras do
not create a second binding, while distinct actions/data do. Intents are copied
at submission. A non-auto-create binding waits for a subsequent start without
creating the Service. Stopping a started Service preserves it while auto-create
clients remain bound; the final unbind destroys a bound-only Service. Cached
Binders and `onUnbind(true)`/`onRebind` follow Android's distinct paths, including
a rebind requested before the previous unbind callback completes. Existing
records remain stoppable after disabling their component with `DONT_KILL_APP`.
PackageManager supports explicit `resolveService`/`queryIntentServices` using
the same manifest-backed component lookup.

Finishing the Activity performs Context connection cleanup and keeps the main
Looper alive until its remaining started Services stop and finish destruction.
Closing the host window explicitly shuts down Services and waits for their
destruction callbacks before process exit. Application `onCreate` can start
Services because the runtime is attached before that callback.

`FrameworkProcessRuntime` bootstraps children through an authenticated host
supervisor and the shared Binder realm. Named isolated services receive UIDs
99000..99999 leased within that realm; ordinary remote Services retain the app
UID. Application metadata and the WebView provider snapshot cross Binder before
ActivityThread creates the service. External isolated binding validates the
manifest flags and uses the caller's package identity with the provider's code.
The supervisor bounds child count, reaps exits and kills its owned children on
shutdown. These VMs share a guest root and the host user's permissions; isolated
Android identity is not a host filesystem or SELinux sandbox.

The real APK service gate checks a different PID, the same ordinary app UID,
and a synchronous Binder round trip, in addition to local lifecycle behavior.
The isolated WebView handshake is still incomplete. This profile rejects
foreground-service requests (notification/deadline support is absent) and
installed-package intent-filter resolution. Unknown explicit components and
packages return absent results. It does not implement sticky
restart/redelivery after process death, cross-app Services, Android background
execution policy, or system_server process management.

### Managed JobScheduler (October 11, 2026)

`FrameworkJobs` provides namespaced queues, owned-UID and `BIND_JOB_SERVICE`
validation, real JobService/JobParameters callbacks, cancellation, work-item
delivery/completion, latency/deadline and backoff timing, periodic scheduling,
and AtomicFile persistence of JobInfo across launches. It permits at most 150
queued jobs and four concurrent bindings per launcher, and bounds service
connection/execution waits. Content observers route URI notifications to job
triggers with descendant matching and debounce/max-delay timing. Connectivity
and storage constraints use the current host-network model and guest data
filesystem. The virtual power profile is plugged in; idle means the launcher's
host window is closed.

The real APK gate passes completion, same-ID namespace isolation, two work
items, cancellation reason, content-trigger URI/authority delivery, rejection
of unrelated/skipped notifications, and persisted-job recovery on a second
launch. These results cover that fixture, not all Android scheduling policy.
Queues are launcher-local rather than a realm-wide system service. Persisted
work items, URI permission grants, predictive prefetch, user-initiated transfer
notifications and privileged system scheduling are not implemented; unsupported
operations fail explicitly. No Doze, quota, thermal, standby-bucket or global
cross-process scheduling equivalence is claimed.

Build and focused APK gate:

```bash
make android-managed-launcher
python3 Scripts/Android/test_android_package.py --emu ./bifrost-emu PackageTests.test_managed_service_execution
```

The signed split APK in `Tests/Host/ManagedServices.java` exercises the public
Context/PackageManager APIs and real Service callbacks, Binder transactions,
multiple clients, distinct binding filters, immediate unbind, cached/rebound and
null Binders, dormant non-auto-create bindings, disabled/missing components,
start-ID-aware self-stop, recreation and service survival after Activity
destruction. Its JIT log is saved at `build/android-service-results/jit.log`;
the gate rejects guest worker faults and fatal Java errors. The interpreter
delivered the initial 25 service assertions and post-Activity destruction
callbacks, but its full APK run failed the gate on font-worker null reads before
Application creation. Those same worker faults were reproduced with the prior
launcher DEX; the full interpreter service gate remains unverified.

Observed JIT verification: all 38 APK assertions pass, including stopping an
already running component after disabling it. The existing managed APK still
completes its three-launch persistence/lifecycle test, and all 51 package checks
pass in each engine. A separate 45-second SpongeBob run reaches resume and a
ready surface, presents 325 frames and logs no guest worker/fatal failure before
the harness deadline. Missing optional platform services remain logged; this
smoke run does not verify SpongeBob service callbacks or playable gameplay.

The subsequent requested 240-second run exits early at 142.073 seconds (status
127). It reports 1,615 frames, logs the real Firebase `AppMeasurementService`
starting, publishing a Binder and shutting down repeatedly, then aborts with
ART `SuspendAll timeout` on `Firebase Background Thread #3`. Results are saved
under `build/android-service-spongebob-240/`. Immediately before abort, host `ps`
reports 4,482,968 KiB RSS in the main emulator and 2,176,492 KiB RSS in its forked
child; shared pages were not measured before exit, so those values must not be
added as unique RAM usage. The longer run fails; neither the abort nor high RAM
has been localized to the service implementation.

Scheduling signatures and lifecycle/binding rules were checked against Android
15's [ActivityThread](https://github.com/aosp-mirror/platform_frameworks_base/blob/android-15.0.0_r1/core/java/android/app/ActivityThread.java),
[ActiveServices](https://github.com/aosp-mirror/platform_frameworks_base/blob/android-15.0.0_r1/services/core/java/com/android/server/am/ActiveServices.java),
[ContextImpl](https://github.com/aosp-mirror/platform_frameworks_base/blob/android-15.0.0_r1/core/java/android/app/ContextImpl.java)
and [IApplicationThread](https://github.com/aosp-mirror/platform_frameworks_base/blob/android-15.0.0_r1/core/java/android/app/IApplicationThread.aidl).
Passing the APK gate is observed emulator behavior for the covered cases; it
does not establish all of Android's service policy or application gameplay.

### APK call and stall tracing (2026-10-09)

`FrameworkTrace.java` adds opt-in tracing to the managed launcher. Normal app
launches leave it disabled. After `make android-managed-launcher`, capture an
installed app with:

```bash
python3 Scripts/Android/android_package.py --state-root android-state/choreo-sensor run com.tiltingpoint.spongebob --timeout 240 --trace-apk
```

Each session gets a separate directory under the installed package's
`managed/traces/apk-trace-*`. `calls.jsonl` contains paired call/return/throw
records for local framework Binder interfaces and ApplicationThread service
scheduling. Records include monotonic nanoseconds, guest PID/TID, thread name,
call ID, operation, bounded argument/result descriptions, duration and caller or
exception stacks. Object descriptions avoid arbitrary app `toString`, Bundle
unparceling and Binder invocation. Missing returns remain in `summary.json` as
unfinished calls, alongside exception counts, slow calls and the last thread
stacks. Arrays are described by type/length; arbitrary object contents are not
serialized.

For an Activity API implementation backlog, use `--trace-apk activity`, for
example on the next Azur Lane launch:

```bash
python3 Scripts/Android/android_package.py run com.YoStarEN.AzurLane --trace-apk activity
```

This mode catalogs the actual loaded `IActivityManager`, `IActivityTaskManager`
and `IActivityClientController` interfaces, including overloaded signatures.
It captures their local Binder calls, returns, throws and pending-call/heap
heartbeats, without ART method tracing, global thread-stack snapshots, library
map scans or automatically enabling native Binder/fault/frame tracing. Normal
launches leave tracing disabled. The full `calls` mode still captures other
local framework boundaries.

After the session exits or times out, `activity-api-report.json` and
`activity-api-report.txt` accompany the raw `calls.jsonl` and `summary.json`.
Reports list all cataloged signatures, group repeated calls by signature and
prioritize failures and unfinished calls. JSON includes first arguments/caller,
exception counts and the last exception stack. Invalid JSON records, truncation,
unmatched completions and absence of a clean trace stop are reported explicitly.
An available signature is a possible API, not evidence the app uses it. A
successful observed return establishes only that invocation; unclassified
dispatch handlers remain unclassified rather than being labeled implemented.
This is a catalog of three Binder interfaces, not every Java Activity method,
JNI lookup, filesystem request, network request or API in an unexecuted branch.

In `calls`, `methods` and `sample` modes, a daemon records pending calls, Java
heap usage and Java thread states/stacks every ten seconds, with separate
markers before/after stack collection. Mapped
library names are recorded when the guest maps expose them; this profile omits
most library labels, so ART's existing `nativeloader` output in `last-run.log`
remains the library-load evidence. These modes enable native Binder, fault and
frame tracing through their existing centralized flags. The
JSONL file stops at 64 MiB and emits a truncation marker. Completed JSONL writes
survive a process abort/kill, and supervision preserves the files on timeout.
The ordinary runtime/logd logs have their existing size behavior.

For a bounded Java/JNI method capture, use `--trace-apk methods --trace-after 45
--trace-duration 15`; `sample` instead of `methods` samples at 10 ms intervals.
ART writes `methods.trace` with a 16 MiB buffer. The host also writes
`methods-summary.json` with entry counts, overflow status and the last events
per ART trace thread. Trace thread IDs differ from guest OS TIDs; correlate by
thread name. An interrupted/unfinalized capture is reported as incomplete.
Method tracing covers Java and JNI method boundaries, not arbitrary internal
IL2CPP/native C calls, native arguments or every system call. Sampling can miss
short methods; a full buffer does not prove complete call coverage.

Both method tracing and global thread-stack collection alter scheduling. The
signed service APK passes all 38 lifecycle assertions with tracing enabled and
with tracing disabled; the trace verifies 140 paired framework calls, error
records, thread stacks and a finalized 48,419-record ART method capture without
buffer overflow. A SpongeBob sampling capture saved its framework calls and
thread stacks, but aborted on ART thread suspension before finalizing its
method file. This is diagnostic evidence, not proof that instrumentation or an
unsupported call caused the game's loading stall. Prefer ordinary `--trace-apk`
for the first stall capture, then narrow the method window as needed.

The method APIs and v2/v3 file reader were checked against Android 15
[Debug.java](https://github.com/aosp-mirror/platform_frameworks_base/blob/android-15.0.0_r1/core/java/android/os/Debug.java)
and [ART trace.cc](https://android.googlesource.com/platform/art/+/refs/tags/android-15.0.0_r1/runtime/trace.cc).
The focused APK gate is `PackageTests.test_managed_service_trace` in
`Scripts/Android/test_android_package.py`.

### Managed display enumeration and listeners (2026-10-09)

`FrameworkDisplay.java` owns the launcher's single virtual display and the local
`IDisplayManager` contract. It supports `getDisplayIds`, legacy `registerCallback`
and `registerCallbackWithEventMask`. The first callback for the guest process is
retained, and subsequent registrations update its event mask, matching AOSP's
per-PID callback record. A zero mask disables events; a null callback is rejected.
Display 0 is enabled, internal and nominally 60 Hz; it reports 160 dpi until play
mode sets the host window's density (see below). Unknown display IDs
return null. DisplayInfo replies cross a Parcel boundary, so client mutation
cannot corrupt the service's state.

Window creation and host-window resize publish current dimensions before
queuing a CHANGED event (event 2, mask bit 4). `DisplayManagerGlobal` then dispatches
to each app listener's selected Handler/Executor. Callbacks execute outside the
service lock. Repeating the same dimensions queues no event. Enumeration returns
only display 0 for either include-disabled setting; this profile has no disabled
or hot-pluggable displays and does not advertise a presentation display.

The concrete failure was Unity `nativeRender` calling
`DisplayManager.registerDisplayListener`, which threw on
`display.registerCallbackWithEventMask`. MediaRouter also failed on
`display.getDisplayIds`. These operations now return through the real Android
framework path. This fixes those display contracts; other missing window/audio
operations and the game's loading stall remain separate issues.

Focused gate:

```bash
make android-managed-launcher
python3 Scripts/Android/test_android_package.py --emu ./bifrost-emu FrameworkDisplayContractTests
```

`Tests/Host/FrameworkDisplayProbe.java` runs in both engines with real
DisplayManager/DisplayManagerGlobal clients. It checks enumeration, absent IDs,
presentation filtering, mode/state, copy isolation, null-listener rejection,
event-mask updates, resize dimensions, callback delivery on a worker Handler and
unregister behavior. Logs are under `build/android-framework-display-contract/`.
All 11 checks passed in each engine. A 240-second-bound SpongeBob run with call
tracing completed display enumeration and callback registration without display
exceptions, and delivered two display-change callbacks. It still aborted after
108.991 seconds with an ART `SuspendAll timeout` (670 reported frames); this does
not establish that the loading stall is fixed. The run and trace are under
`build/android-display-spongebob/`.
A follow-up normal managed launch without APK tracing reached its full
240-second deadline without a reported ART suspension abort. Its logs are in
`build/android-display-control/`. This separates the successful control run from
the failing instrumented runs; it does not prove a cause for those timeouts.
This does not test physical hotplug, HDR/brightness events or a changing refresh
rate. The implementation remains strict for unsupported display operations.

Signatures, event masks and per-process callback behavior were checked against
Android 15 [DisplayManagerService](https://github.com/aosp-mirror/platform_frameworks_base/blob/android-15.0.0_r1/services/core/java/com/android/server/display/DisplayManagerService.java)
and [DisplayManagerGlobal](https://github.com/aosp-mirror/platform_frameworks_base/blob/android-15.0.0_r1/core/java/android/hardware/display/DisplayManagerGlobal.java).

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
install descriptors. Scatter/gather FD arrays are now supported and close their
installed descriptors when the receive buffer is freed. Binder-device FD transfer
remains unsupported, as does the FdTable's pre-existing per-descriptor CLOEXEC
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
python3 Scripts/Android/test_android_framework.py --services \
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
rejected with EBUSY.

The Java `Surface` is an identity key for the host window, not a real libgui
Surface: its native object stays null, so `Surface.isValid()` is false. Android
delivers `surfaceDestroyed` while the surface is still valid, and Unity copies
the last frame there with
[PixelCopy](https://android.googlesource.com/platform/frameworks/base/+/android-15.0.0_r1/graphics/java/android/view/PixelCopy.java),
which rejects invalid sources. Window-close teardown therefore skips only an
`IllegalArgumentException` thrown by `PixelCopy` itself, logs
`APP_SURFACE_SNAPSHOT_SKIPPED`, and finishes stop/destroy with a zero exit.
The rest of that callback after the copy does not run; other callback failures
still fail the launch. PixelCopy and other `isValid()` users at runtime remain
unsupported.

Apps may end their own process during that teardown: SpongeBob logs
`Process: Sending signal. PID: ... SIG: 9` (Android's `Process.sendSignal`)
right after `APP_STOPPED`, as a phone would. The launcher prints
`APP_HOST_CLOSED` when the host window closes, and the session supervisor
reports a SIGKILL exit (137) after that marker as state `closed`, keeping the
real exit code. A SIGKILL without the marker remains `failed`.

### Framework windows and SurfaceViews (2026-10-13)

Every `WindowManager.addView` window goes through one path: the launcher
Activity's decor, stacked Activities, `Dialog`/`AlertDialog`, `PopupWindow`
and similar. After `onResume` the launcher adds its decor as a
`TYPE_BASE_APPLICATION` window like `handleResumeActivity`, so an Activity
built only from Views (buttons, text, WebView) launches; ViewRootImpl lays it
out and the window is drawn like any other. A base Activity window stacks
below the dialogs and panels sharing its token, whatever the add order, as
in WindowManagerService (a dialog shown from `onResume` is added first).
`IWindowManager.openSession` returns `FrameworkWindows`' `IWindowSession`:
- `addToDisplay*` registers the window, initializes its input channel pair
  (ViewRootImpl's receiver requires one), reports size-compat scale 1, display
  insets and, for sub-windows, the full-screen parent frame, and returns
  `ADD_FLAG_IN_TOUCH_MODE | ADD_FLAG_APP_VISIBLE`.
- `relayout`/`relayoutLegacy`/`relayoutAsync` compute frames like
  `WindowLayout.computeFrames` without insets (explicit or MATCH_PARENT size,
  else the measured size, then `Gravity.apply`) and report the window's
  current configuration, so no configuration change is invented.
- `remove` drops the host layer and returns focus.

Android composes these windows in SurfaceFlinger. Here the UI loop draws each
view tree in software when it is dirty and sends the pixels through private
op 9 to a host layer above every surface (op 10 removes it); see
[window plugins](ANDROID_WINDOW_PLUGINS.md). A fully transparent window gets
no host layer. Host layers stack in creation order, so when a window or dim
layer appears below existing ones, the layers above it are recreated in
window order. `FLAG_DIM_BEHIND` adds a translucent full-display layer below
its window. Input follows InputDispatcher: touches go to the topmost window
containing them or to a modal window, windows with `FLAG_WATCH_OUTSIDE_TOUCH`
receive `ACTION_OUTSIDE`, and keys go to the topmost focusable window (the
back key cancels dialogs). Exactly one window has focus; a window that never
had it gets no `onWindowFocusChanged(false)`, as under WindowManagerService.
The log records `APP_POPUP_ADDED`, `APP_POPUP_SHOWN <id> <frame>
visible=<non-transparent pixels> text=<visible text>` (once per window),
`APP_POPUP_HIDDEN <id> transparent` when a shown window becomes fully
transparent, and `APP_POPUP_REMOVED` for every window, Activity windows
included. Unity's startup placeholder view, for example, covers its
SurfaceView in the Activity window until Unity removes it. View click
sounds are accepted silently.

`SurfaceView.updateSurface()` gives up while its window has no
SurfaceFlinger surface, so `FrameworkSurfaces` applies its visibility
contract instead
([Android 15 SurfaceView](https://android.googlesource.com/platform/frameworks/base/+/android-15.0.0_r1/core/java/android/view/SurfaceView.java)
`mRequestedVisible` and `mHaveFrame`): a SurfaceView has a host surface while
it is attached, shown, laid out with a non-empty frame and its window is
visible and not stopped. After each traversal (a ViewTreeObserver pre-draw)
the window's SurfaceViews are compared with the tracked ones: a new one gets
`surfaceCreated` then `surfaceChanged`, a size or format change gets
`surfaceChanged`, and a hidden, detached or removed one gets
`surfaceDestroyed` and its host reference released. A created surface marks
the SurfaceView drawn (`performDrawFinished`), so the window punches its hole
with `Canvas.punchHole` in drawing order and Views drawn after it stay
visible above the surface. Host resizes relayout every window
(`ViewRootImpl.forceWmRelayout`). Window close stops all windows, delivering
`surfaceDestroyed` before `onStop`. The log records
`APP_SURFACE <class> <w>x<h> at <x>,<y> format=<f>` and
`APP_SURFACE_DESTROYED <class>`.

`releaseSurfaces` applies an empty `SurfaceControl.Transaction` on every such
update. libgui sends it through the legacy `SurfaceFlinger` service
(`android.ui.ISurfaceComposer`), which `ComposerService` waits for forever.
The session process therefore publishes `SurfaceFlinger` next to
`SurfaceFlingerAIDL`: it accepts `SET_TRANSACTION_STATE` with no layer or
display states and rejects every other transaction, since no SurfaceControl
layers exist.

The Activity context now receives the override configuration WMS supplies:
window bounds, app bounds and max bounds equal to the virtual display, updated
on host resize through `ResourcesManager.updateResourcesForActivity`. Without
them, ViewRootImpl measured new dialogs to 0×0 and
`WindowManager.getCurrentWindowMetrics()` reported an empty window.

`ViewConfiguration` caches `4 × maxBounds` area per density as the software
drawing-cache limit and never recomputes it (AOSP 15 constructor), because
a phone display never grows. After the host window grew from 2560 to 3226
pixels wide, SpongeBob's full-window Kidoz ad `HtmlFiveWebView` failed
"too large to fit into a software layer" on every frame. The level-end
screen stayed black while the main thread retried at full CPU. The
launcher now calls the hidden `ViewConfiguration.resetCacheForTesting()`
after `updateResourcesForActivity` on each resize, so the next lookup uses
the new bounds.

Limits: windows draw only through software `View.draw` (no hardware layers or
`TextureView`), SurfaceView surfaces are not positioned or clipped (see the
launcher limits below), window animations are not played, and system windows
(toasts, system alerts) are not composited. On
X11 without a 32-bit ARGB visual, translucent edges blend on black; a parent
that never redraws (no rendering SurfaceView) can keep stale dialog pixels
after removal.

### Native-density play windows (2026-10-12)

Play mode opens the host window at 1280x720 desktop units (720x1280 for
portrait Activities), then reads its native pixel size and density through
private op 11 (`windowMetrics`). On a 2x display the Android display is
2560x1440 at 320 dpi; density is `160 × pixels per desktop unit`. Before the
Application and Activity contexts exist, the launcher publishes these metrics to
`DisplayInfo` and applies them with `ResourcesManager.applyConfigurationToResources`,
as `ActivityThread.handleConfigurationChanged` does. The Activity and window-context
override configurations carry the density and the screen size in dp, rounded
like Android 15's `TaskFragment.computeConfigResourceOverrides`
(`(int) (px / density + 0.5f)`). Later host resizes, including a move to a
display with another scale, change the pixel size; the density stays at its
startup value, since a density change would need an Activity configuration
change, which is not implemented. Loadable window plugins keep 160 dpi.
`BIFROST_ANDROID_LOGICAL_PIXELS=1` is an A/B control that opens the window at
its desktop size and 160 dpi (compositor-upscaled), as before this change.
Zero `ANativeWindow_setBuffersGeometry` restores the window's own pixel size.

Max bounds (2026-10-13): `windowConfiguration` max bounds are the pixel size
of the host display holding the window (private op 12, `displaySize`: SDL3
display bounds times the window's pixel density, never smaller than the
window). Both the Activity override and the process configuration carry
them, as WMS's global configuration does
([DisplayContent.computeScreenConfiguration](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-15.0.0_r1/services/core/java/com/android/server/wm/DisplayContent.java)).
[ViewConfiguration](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-15.0.0_r1/core/java/android/view/ViewConfiguration.java)
caches `4 * maxBounds` as the software drawing-cache limit per density,
built from whichever context asks first. The launcher resets that cache
when the window grows. Before this change the process configuration had no
bounds, so an application-context rebuild set the limit to 0: SpongeBob's
Kidoz ad WebView logged "too large to fit into a software layer, needs
19043840 bytes, only 0 available" every two seconds. Window-sized bounds
would still be too small after maximizing, since the process configuration
is not reapplied on resize. That warning is gone, but the ad still does not
get past its loading screen (see [SpongeBob](../Apps/spongebob.md#kidoz-ad-stuck-on-its-loading-screen-2026-10-13)).
Moving the window to another monitor does not update max bounds.
Loadable window plugins do not report a display, so they keep window-sized bounds.

SpongeBob 5.12.0 on a 3440x1440 monitor at 200% scale opens at 2560x1384 px,
320 dpi (Unity's surface follows to 3440x1384 when maximized). The user
confirmed high-resolution rendering and correct pointer mapping at 2x.
The game's loading art is cropped
on 16:9 windows by its own layout, as on 16:9 phones.

### Notification enablement (2026-10-12)

A strict local `notification` service (`android.app.INotificationManager`)
implements only `areNotificationsEnabled(pkg)`, following Android 15's
[NotificationManagerService](https://android.googlesource.com/platform/frameworks/base/+/android-15.0.0_r1/services/core/java/com/android/server/notification/NotificationManagerService.java)
and [PermissionHelper](https://android.googlesource.com/platform/frameworks/base/+/android-15.0.0_r1/services/core/java/com/android/server/notification/PermissionHelper.java):
a foreign package gets `SecurityException`, and the answer is the package's
`POST_NOTIFICATIONS` grant. This profile grants no runtime permissions, so it
reports false, as on a device where the user has not allowed notifications.
Posting, channels and listeners still throw.

Without the service, `NotificationManager.areNotificationsEnabled()` threw
inside SpongeBob's `NotificationSettings` helper on every game state change.
Unity surfaced the exception in the game's state-change event, which then
skipped `MainHudHandler` re-enabling the main HUD after Settings closed: the
scene kept drawing without its buttons. The user confirmed the HUD now stays.

### App ops and missing content providers (2026-10-12)

`SystemServiceRegistry` builds `AppOpsManager` from
`ServiceManager.getServiceOrThrow("appops")`, so without that service
`getSystemService(APP_OPS_SERVICE)` returned null. Firebase measurement then
failed its upload task with a `NullPointerException`. `FrameworkAppOps`
registers a strict `com.android.internal.app.IAppOpsService`. A SpongeBob
run with a fully unimplemented stub showed only `checkPackage(uid, pkg)`
(from GMS UidVerifier), so that is the one method. It follows Android 15's
[AppOpsService](https://android.googlesource.com/platform/frameworks/base/+/refs/heads/android15-release/services/core/java/com/android/server/appop/AppOpsService.java)
`checkPackage`/`verifyAndGetBypass`/`resolveUid`. Root may name any existing
package. `root`, `shell`, `dumpstate`, `media`, `audioserver`, `cameraserver`
and `com.android.shell` resolve to fixed uids. Any other package must exist
and belong to the uid. Failures return `MODE_ERRORED`, which
`AppOpsManager.checkPackage` throws as `SecurityException`. Other methods
print `APPOPS_UNSUPPORTED <method>` and throw.

Firebase next asked `IActivityManager.getContentProvider` for Play
services' `com.google.android.gms.phenotype` and hit the unsupported path.
As in AOSP's `ContentProviderHelper`, an authority no installed or system
package declares now returns no holder (`APP_PROVIDER_MISSING <authority>`).
`ContentResolver` then returns null. Declared authorities other than
`settings` originally still threw; normal launches now install local manifest
providers as described below. Firebase now logs `App measurement initialized` and
only `Service invalid`, its warning for a device without Play services.
ActivityProbe prints `APP_APP_OPS_OK` after checking the app's own package
and rejecting a missing package, `root` and `com.android.shell` for the
app uid. This was run in JIT play mode only.

### Manifest provider startup and Azur Lane downloads (2026-10-13)

Azur Lane 9.3.7 fetched its version/hash lists and prepared a download queue,
but its YsDownload code called `Context.getSystemService` on a null static
context. Inspection of the APK's `YsDownloadProvider.onCreate` showed that this
context is initialized by its manifest ContentProvider. The launcher had never
installed manifest providers.

Normal installed-app launches now filter enabled providers to the current
process, order them by descending `initOrder`, and invoke the framework's real
`ActivityThread.installContentProviders` before `Application.onCreate`.
ActivityThread supplies context attachment, class loading, `onCreate` and local
authority caching; `FrameworkContent` validates and retains its published
holders, including semicolon-separated aliases. Startup ordering and publication
were checked against Android 15's
[ActivityThread](https://github.com/aosp-mirror/platform_frameworks_base/blob/android-15.0.0_r1/core/java/android/app/ActivityThread.java),
and priority sorting against
[ComputerEngine](https://github.com/aosp-mirror/platform_frameworks_base/blob/android-15.0.0_r1/services/core/java/com/android/server/pm/ComputerEngine.java).
Disabled providers and declarations for another process are skipped. Remote
provider acquisition, provider startup in service-only child processes and
multi-process provider policy are not implemented by this change.

Starting the previously omitted AndroidX WorkManager provider exposed its
force-stop check: `PendingIntent.getBroadcast(..., FLAG_NO_CREATE, ...)`.
`IActivityManager.getIntentSenderWithFeature` now validates the installed owner,
user, sender type and intents and returns null for an empty-registry
`FLAG_NO_CREATE` lookup. This follows the lookup contract in Android 15's
[PendingIntent](https://github.com/aosp-mirror/platform_frameworks_base/blob/android-15.0.0_r1/core/java/android/app/PendingIntent.java).
Sender creation remains explicitly unsupported; this does not implement alarms
or background PendingIntent delivery.

In the real untraced JIT session `84a17b02fbb54a1d83802245e543c2a3`, Azur Lane
passed that path and wrote 2,305,675,539 asset bytes across 12,470 files, up from
78,150 bytes in two files before the fix. This is filesystem evidence of
downloading, not a claim that the roughly 24.5 GB queue completed or gameplay
was reached. The session was left running during logger validation.

### Activity stack (2026-10-12)

Play mode starts further Activities of the installed package above the
launcher Activity (full-screen ads, sign-in and web pages).
`FrameworkActivities` implements `IActivityTaskManager.startActivity` and
`startActivityAsUser` (argument layout checked against the Android 15
framework.jar) and posts the launch to the main thread:
- the current top Activity pauses (`performPause`), then the new one is
  created like `ActivityThread.performLaunchActivity`: Activity context with
  the display override configuration, `Instrumentation.newActivity`, the
  ActivityThread as `mMainThread`, theme, `onCreate`/`onStart`/
  `onPostCreate`/`onResume`;
- `handleResumeActivity`'s `TYPE_BASE_APPLICATION` decor window is added
  through `WindowManager`, so `FrameworkWindows` draws it full-display,
  routes touches and keys to it and moves window focus to it;
- once the new Activity is resumed, every Activity it covers is stopped and
  its window made invisible, following `EnsureActivitiesVisibleHelper`: an
  Activity occludes those beneath unless its manifest theme sets
  `windowIsTranslucent` or `windowIsFloating` (`windowShowWallpaper` always
  occludes; `ActivityRecord`, android-15.0.0_r1). The stop is
  `handleStopActivity`: `performStop`, then `updateVisibility(false)` makes
  the decor `INVISIBLE`, so `FrameworkWindows` drops its layer and destroys
  its SurfaceViews' host surfaces;
- `Activity.finish()` records the result through `finishActivity`; the UI loop
  pauses the finished Activity, restarts any Activity it uncovered
  (`performRestart`), calls the caller's `dispatchActivityResult` for
  `startActivityForResult`, resumes the Activity below and makes uncovered
  windows visible again, then stops and destroys the finished one and
  removes its window.
`getCallingActivity`/`getCallingPackage` report the caller of an Activity
started for a result. The launcher Activity is registered before its
`onCreate`, so a splash Activity may start the game Activity there (Azur
Lane's `PrePermissionActivity`); the launch is posted and runs after the
launcher Activity has resumed. A host window close destroys stacked Activities before
the launcher Activity. The log records `APP_ACTIVITY_PAUSED`,
`APP_ACTIVITY_STARTING`, `APP_ACTIVITY_RESUMED`, `APP_ACTIVITY_STOPPED`,
`APP_ACTIVITY_RESTARTED` and `APP_ACTIVITY_DESTROYED`. Azur Lane's opaque
`PrePermissionActivity` window previously stayed above `MainActivity`'s
Unity SurfaceView (window layers sit above all surfaces), so the screen stayed
on its blue background until a resize.

The launcher Activity, too, now receives the ActivityThread as `mMainThread`.
Without it `Activity.startActivity` failed with a null
`getApplicationThread()`: SpongeBob's Kidoz rewarded ad
(`FullScreenAdManager`) reported `onAdFailedToShow` and the game stalled
waiting for the ad.

Only explicit components of the installed package resolve. Implicit Intents
and other packages (browser, Play Store) return `START_INTENT_NOT_RESOLVED`
and an unknown class `START_CLASS_NOT_FOUND` (Android 15 values -91 and -92),
so apps receive `ActivityNotFoundException` as on a device without a handler.
The exception is `PackageManager.ACTION_REQUEST_PERMISSIONS` addressed to the
permission controller, built by `Activity.requestPermissions`
([android15-release](https://android.googlesource.com/platform/frameworks/base/+/refs/heads/android15-release/core/java/android/app/Activity.java)).
It is answered as `GrantPermissionsActivity` would: the caller pauses, receives
`RESULT_OK` with `EXTRA_REQUEST_PERMISSIONS_NAMES`/`_RESULTS` through
`dispatchActivityResult` (so `onRequestPermissionsResult` runs), then resumes.
With no permission UI, each grant is the install-time `checkPermission`
result, so runtime permissions come back denied, as if the user declined.
`requestPermissions` marks its request pending only after
`startActivityForResult` returns, and Unity calls it off the main thread; an
Activity's own result is therefore delivered once that mark is set (bounded
to about a second). An earlier result would leave the mark set and cancel
every later request, as on a device it never could, since the result
arrives from another process.
Previously `getPermissionControllerPackageName` threw, so Trickcal's Unity
permission request failed and its callback never ran.
An Activity below a translucent or floating one is paused but not stopped
and keeps its window and surfaces. The stop happens at once rather than after
the new Activity is idle, and `onSaveInstanceState` is not called on it
(there is no process death to restore from). Stacked Activities draw only in
software, keep the display size regardless of their requested orientation,
and launch flags, task affinities and `onNewIntent` are not modelled.
A mouse's back/forward buttons send `KEYCODE_BACK`/`KEYCODE_FORWARD`, as
Android's InputReader synthesizes them. The configured back key
(`[android] back_key`, `BIFROST_ANDROID_BACK_KEY`; default `escape`, `none`
disables) is delivered as `KEYCODE_BACK` to NativeActivity and managed apps
alike; that host key then no longer reaches apps under its own code.
Back follows `ActivityClientController.onBackPressed` (android-15.0.0_r1):
a stacked Activity is asked to finish through its `IRequestFinishCallback`,
which posts `finishAfterTransition`. On the launcher root Android moves the
task to the back; with no home screen here the root stays in front, so only
the app's own key handling (Unity's view, for example) acts on it.
`getTaskForActivity` reports the single launcher task.
`PackageTests.test_managed_activity_stack` (`ManagedActivities`) checks the
ordering above, the unresolved implicit Intent, a provider intent-filter query
against the installed split cluster, a permission request returning a granted
normal and a denied runtime permission, the result, the caller, a
second round closed by a `KEYCODE_BACK` through the window's view tree
(`RESULT_CANCELED`), and a
display-sized window with the button text on the host layer.

Adding `--play` opens a host window, starts/resumes the Activity, adds its
decor window through WindowManager, reports SurfaceView holder callbacks and
pumps host input. The SurfaceView surfaces use the managed graphics adapters,
not SurfaceFlinger. The runner
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
`Tests/Unit/test_signal_semantics.c` fails against the old dispatcher in both engines
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

The session's managed launcher registers `media.audio_policy` and
`media.audio_flinger` stand-ins. They answer only the Android 15 transactions
native AudioSystem makes here (policy 8/44/49/51/52/53/80/82,
flinger 3/5/6/23/45/46; codes
checked against `IAudioPolicyService.aidl`/`IAudioFlingerService.aidl` at
`android-15.0.0_r1`): no HAL ports or patches, and the primary output's rate
and frames per buffer come from Bifrost's host Audio engine
(`APP_AUDIO_ROUTING_READY`). Playback itself never uses AudioFlinger: the
managed `libaaudio.so` adapter sends AAudio to the AudioThunk. Like real
libaaudio, the adapter runs each data callback stream on a Bionic pthread of
its own (`Guest/Android/Adapters/aaudio.c`), created at open and joined at close; the
host callback pump cannot make Bionic TLS. That thread asks the thunk when the
host ring has room for one more callback (`bifrost_aaudio_wait_callback`),
calls the app, and hands the buffer back. `test_android_bionic.sh` checks
this in all three engine modes (`Tests/Unit/android_aaudio_thread.c`).

The AudioThunk exports every AAudio function CRIWARE looks up. Builder
attributes (channel mask or count, content type, spatialization, capture
policy, privacy) are validated at open with AOSP libaaudio's rules and codes
(`AAudioStreamParameters::validate`, `android-15.0.0_r1`); the host engine
mixes at most 8 of the 12 channels AOSP accepts and returns `UNAVAILABLE` above
that. A channel count becomes an index mask, which `getChannelMask` hides, as
AOSP does. `requestFlush` works in OPEN/PAUSED/STOPPED/FLUSHED, discards the
queued host audio and reports FLUSHED at once; `release` waits for a running
callback, frees the host stream and leaves the handle in CLOSING until close.
Both return `INVALID_STATE` from the data callback. Package name and
attribution tag are accepted and ignored (no app-ops service).

#### Managed OpenSL ES (2026-10-13)

The image's real `libOpenSLES.so` (wilhelm) plays through AudioTrack, so
`createTrack` fails without AudioFlinger and player Realize returns
`SL_RESULT_CONTENT_UNSUPPORTED`; Azur Lane's CRIWARE then reports
`E2015062223: failed to realize OpenSLES player`. The managed
`libOpenSLES.so` adapter (`Guest/Android/Adapters/opensles.c`) replaces it.
It implements, against AOSP `frameworks/wilhelm` at `android-15.0.0_r1` and
its pinned headers:

- all 53 `SL_IID_*` exports of `libOpenSLES.map.txt` with their real GUIDs,
  matched by content as wilhelm does; `slCreateEngine` returns Android's one
  reference-counted engine per process;
- engine, output mix and audio player objects; the player takes a
  `BUFFERQUEUE`/`ANDROIDSIMPLEBUFFERQUEUE` source of PCM or `PCM_EX`
  (8-bit unsigned, 16/24/32-bit signed, 32-bit float, little-endian, 1–8
  channels, 8–192 kHz) and an output-mix sink. Creation validation and
  result codes follow `checkDataFormat`/`android_audioPlayer_checkSourceSink`;
- `Play` (states, position, marker/new-position/end events), both buffer-queue
  IIDs on one interface, `Volume` (level, mute, stereo position with
  wilhelm's gain and pan law), `EffectSend` direct level, and
  `AndroidConfiguration` stream type and performance mode before Realize.

Realize opens a float AAudio stream through the managed `libaaudio.so`, so
buffer-queue and play callbacks run on that adapter's Bionic callback thread,
pulling from the queue the way wilhelm's AudioTrack callback does. Mono plays
on a stereo stream so it can be panned. Destroy from a player's own callback is
deferred until the callback returns. Not implemented: decoding sources
(URI, fd, Android buffer queue), audio recording, LED/vibra/MIDI/3D/metadata
objects, environmental reverb and other effects, routing proxies, and
`BufferQueue::Clear`/Stop flushing audio already queued in the host stream
(up to about three callbacks). `test_android_bionic.sh` runs
`Tests/Unit/android_opensles_player.c` in all three engine modes.

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
python3 Tools/Android/fetch-android-runtime.py
bash Scripts/Android/test_android_bionic.sh
```

The script also rebuilds the Android adapters and runs the managed AAudio
callback-thread test against them (`SDL_AUDIODRIVER=dummy`, no sound device).

The fetcher pins the official AOSP `prebuilts/runtime` APEX at revision
`8aeb37cca394ce39c1311744c60960cbd466aa77` (the `android-15.0.0_r1` repository
revision), checks archive and extracted-file SHA-256 hashes, retains its
license notices, and installs only linker64, libc, libdl, libdl_android and
libm under `Tools/android-runtime/rootfs`. A repository tag is provenance;
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
./bifrost-emu --android-runtime --rootfs Tools/android-runtime/rootfs \
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
python3 Tools/Android/android_art.py
python3 Tools/Android/build-android-jni.py --javac /path/to/javac
BIFROST_NO_ASLR=1 python3 Scripts/Android/test_android_jni.py --timeout 60
python3 Scripts/Android/test_android_jni.py --mode qemu   # qemu-aarch64 reference run
```

Requirements: Python 3.11+, `debugfs`, `lpunpack`, `readelf`, Clang/LLD and a
JDK 17+. The image download is approximately 734 MiB; extraction needs several GiB
of scratch disk space. An existing verified Media NDK source archive is reused.
The DEX compiler is checksum-pinned D8 from AOSP `android-15.0.0_r1`.
Generated runtime files and fixtures stay under ignored `build/` directories.

The runner copies the verified runtime into a temporary root, adds writable
data and fixtures (the DEX read-only: the guest runs as an app UID, for which
ART rejects writable DEX files), enforces a timeout, and saves logs plus JSON
results in `build/android-jni-results/`. The launcher calls the real `JNI_CreateJavaVM`
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
Open gap (found 2026-10-12, not fixed): a faulting tagged access reports
`si_addr` with its tag (e.g. `1 << 56`). Linux 5.11+ clears the tag bits unless
the handler sets `SA_EXPOSE_TAGBITS`, and QEMU 11.1 reports the untagged address.

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
python3 Tools/Android/fetch-aapt2.py
python3 Scripts/Android/android_preflight.py /path/to/package.apkm --output report.json
python3 Scripts/Android/test_android_preflight.py
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
bash Scripts/Android/test_android_loader.sh
./Scripts/Testing/run_tests.sh --filter 'simd_addlv|crc32_instructions'
./Scripts/Testing/run_tests.sh --no-jit --filter 'simd_addlv|crc32_instructions'
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
variants pass after the mmap/procfs changes; see [test evidence](../TESTS.md).
The subsequent AAudio wake-up fix passes 20 callback stress runs, including
injected delayed waits in both engines, and four neighboring audio/thread tests
per engine. This resolves a reproducible lost-notification race without raising
the guest fixture's callback deadline. The older interpreter checkpoint's
267/268 result remains recorded in [test evidence](../TESTS.md).


### First SpongeBob rendering milestone (2026-10-07)

This supersedes the earlier managed-audio checkpoint's unknown null-object
failure. Unity's aligned stack frame was broken by ignored AND-to-SP writes;
logical immediates now honor SP/WSP in both engines. The next crash came from
GLES returning a host mapped-buffer pointer to guest code; GLES now uses the
existing guest-buffer map/unmap/flush policies. See [JIT.md](../JIT.md).

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
[JIT.md](../JIT.md#reciprocal-estimates-and-refinement-spongebob-2026-10-07)).
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
only finding a Java service-cache entry. See [test evidence](../TESTS.md#native-sensor-service-follow-up-2026-10-07).


## Managed APK launcher (2026-10-08)

`ManagedLauncher` contains the Activity bootstrap previously exercised through
`ActivityProbe --play`. Normal app launches use a real ActivityThread, LoadedApk,
Application and Instrumentation, the verified installed base/splits, and the
app's launcher Activity. Diagnostic registration tests and probe preferences
are disabled during ordinary launches. `ActivityProbe` remains a compatibility
entry point for the diagnostic checks.

```bash
make android-managed-launcher
python3 Scripts/Android/android_package.py --state-root android-state run PACKAGE
# Optional bounded run and title statistics, without a separate stats window:
python3 Scripts/Android/android_package.py --state-root android-state run PACKAGE \
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
Every launch recreates the package's CE/DE data directories
(`data/user/0/<package>`, `data/user_de/0/<package>`) if absent, as `installd`
does on Android; `ContextImpl` creates only their subdirectories and otherwise
fails with `ENOENT`, losing early shared preferences and databases.
A temporary short root alias avoids Linux's Unix-socket pathname limit without
making app data temporary. A per-package lock covers preparation and the entire
session. The supervisor forwards interruption, terminates owned descendants,
removes its log socket, and propagates guest failure; a requested timeout returns
124 and interruption returns 130. Normal runs have no time limit.

Managed sessions also supervise `/dev/socket/dnsproxyd` for Bionic's
`getaddrinfo` requests. The service uses the host resolver and translates Android
flags/error numbers and IPv4/IPv6 socket addresses using the
[AOSP Bionic protocol](https://android.googlesource.com/platform/bionic/+/refs/tags/android-15.0.0_r1/libc/dns/net/getaddrinfo.c)
and [DnsResolver serialization](https://android.googlesource.com/platform/packages/modules/DnsResolver/+/refs/tags/android-15.0.0_r1/DnsProxyListener.cpp).
Requests and concurrent clients are bounded; sockets close with the session.
This supplies the host's default network. Android per-network routing, legacy
hostent commands and raw `resnsend` queries are not implemented. No DNS server
address or application hostname is hardcoded.

The activity service answers `getHistoricalProcessStartReasons` with an empty
`ParceledListSlice<ApplicationStartInfo>` for the caller's package/UID; the public
`ActivityManager` API returns its empty list. This profile has no start-info
tracker. It does not invent launch reasons or
timestamps. Foreign-package/user queries are rejected; `USER_ALL` and
`USER_CURRENT` are invalid, matching Android 15's
[ActivityManagerService](https://android.googlesource.com/platform/frameworks/base/+/android-15.0.0_r1/services/core/java/com/android/server/am/ActivityManagerService.java).
The framework probe checks the public API and the Binder package/user access
boundaries (`ACTIVITY_START_HISTORY_OK`). Persistent process-start history
remains unsupported.

The managed connectivity service snapshots the host's IPv4/IPv6 default routes
at launch and exposes one virtual Wi-Fi network through active-network,
legacy connection-info and capability queries. Default callbacks and matching
network listeners receive Android's `AVAILABLE` message with capabilities and
link properties; unmatched requests with a timeout receive `UNAVAILABLE`.
Requests have independent identifiers and can be released separately. This
follows the framework's
[ConnectivityManager callback protocol](https://android.googlesource.com/platform/packages/modules/Connectivity/+/refs/heads/master/framework/src/android/net/ConnectivityManager.java).
The network is Wi-Fi (`TYPE_WIFI`, `TRANSPORT_WIFI`) whatever the host link is:
phone apps expect Wi-Fi or cellular, and SDKs reported the earlier virtual
Ethernet as `connectionType: none`. Its capabilities are the regular Wi-Fi filter
from Android 15's
[WifiInjector](https://android.googlesource.com/platform/packages/modules/Wifi/+/refs/tags/android-15.0.0_r1/service/java/com/android/server/wifi/WifiInjector.java)
(`INTERNET`, `NOT_METERED`, `NOT_ROAMING`, `NOT_CONGESTED`, `NOT_SUSPENDED`,
`NOT_VCN_MANAGED`, 1 Gbit/s link estimates) over the framework defaults, plus
`FOREGROUND`, which
[ConnectivityService](https://android.googlesource.com/platform/packages/modules/Connectivity/+/refs/tags/android-15.0.0_r1/service/src/com/android/server/ConnectivityService.java)
gives every non-background network. Wi-Fi is unmetered unless an access-point hint
or user override marks it metered (`WifiConfiguration.isMetered`); the host
supplies neither, so `isActiveNetworkMetered()` is false online and true with no
network.
`VALIDATED` follows Android's
[NetworkMonitor](https://android.googlesource.com/platform/packages/modules/NetworkStack/+/refs/tags/android-15.0.0_r1/src/com/android/server/connectivity/NetworkMonitor.java)
HTTPS probe: the launcher requests `https://www.google.com/generate_204` once,
alongside runtime preparation (3-second timeout), and only HTTP 204 validates;
redirects (captive portals) and failures leave the network unvalidated.
Partial connectivity, captive-portal login and later revalidation are not
modelled. Link properties carry the host default-route interface name, which
apps also see through netlink; addresses, routes and DNS servers are empty.
Explicit per-network routing is unsupported, and changes to the host network
during a session are not tracked. `BIFROST_ANDROID_HOST_NETWORK=0` or `1`
overrides the route snapshot and `BIFROST_ANDROID_NETWORK_VALIDATED=0` or `1`
skips the probe, for deterministic offline/online diagnostics.
`ConnectivityManager.getRestrictBackgroundStatus()` reports
`RESTRICT_BACKGROUND_STATUS_DISABLED`: the host bridge enforces no Android
Data Saver or per-UID background restriction. This policy is independent of
route availability and metering. The online/offline
framework checks exercise the public API. The status follows Android 15's
[NetworkPolicyManagerService](https://android.googlesource.com/platform/frameworks/base/+/android-15.0.0_r1/services/core/java/com/android/server/net/NetworkPolicyManagerService.java).

The APK's Unity connectivity helper queries `getActiveNetwork()` and
`getNetworkCapabilities()` before registering its default callback. Returning
null for every query previously made Unity report no reachable network even
when host sockets and DNS worked. SpongeBob then skipped device login and left
its `WaitForUserId` loading task unfinished. Exposing the route allows the login
path to run, but the observed game session still reached its PlayFab failure
path with no user ID; successful loading is not established. The real-framework
JIT probes cover both route modes, matching listeners, independent release and
an unmatched cellular request's timeout. An interpreter probe failed earlier
in `FrameworkVsyncProbe0` with an unmapped read, before connectivity checks;
interpreter coverage remains unverified.

With the network presented as validated Wi-Fi (above), SpongeBob's loader
passes 40% in some runs (observed: 4 of 10 runs of 200-360 seconds) and then downloads
its `CityLevelData*` files. Stuck runs stay at the "DOWNLOADING 0%" step near
40% with the emulator mostly idle (about 1-2 host cores), so the stall is not
CPU saturation. Its cause is not localized: a full ART method trace of a stuck
run shows no Java activity in the native loader, and stuck runs keep idle
sockets with unread bytes (`ss` Recv-Q) whose meaning is unproven. A Galaxy
S24 FE running 5.12.0 with cached data loads to the menu in about 15 seconds.

Previously, SpongeBob's TiltingPoint registration request failed with
`Cannot resolve destination host`. With the resolver, the actual game obtains
its SDK device ID; the user observed loading pass its former 73% plateau to 74%.
This does not establish completed loading or gameplay. `DnsProxyTests` covers
wire framing, Android flag/error translation, fragmented requests and cleanup,
and real Bionic IPv4/IPv6 lookups under both JIT and interpreter, without an
external network dependency:

```bash
python3 Scripts/Android/test_android_package.py DnsProxyTests LauncherArgumentTests
```

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

The settings provider implements `GET_secure`, `GET_system` and `GET_global`
through the real framework `Settings` client. `Settings.Secure.ANDROID_ID`
returns a persistent 16-character lowercase hexadecimal identifier scoped to
the APK's signing certificates, virtual device and user. Derivation follows
Android 15's
[SettingsProvider.generateSsaidLocked](https://android.googlesource.com/platform/frameworks/base/+/android-15.0.0_r1/packages/SettingsProvider/src/com/android/providers/settings/SettingsProvider.java):
HMAC-SHA256 of each certificate's big-endian four-byte length followed by its
DER bytes, using a random 32-byte per-user key, truncated to the first 64 bits.
Packages with the same signing certificates in the same state root share the ID;
the package name does not enter the derivation.

The device key lives in `<state-root>/.settings/user-0.key` (mode 0600), shared
across package runtimes. Concurrent first launches publish one complete key
atomically. Runtime preparation installs it at
`/data/system/users/0/bifrost-ssaid-key` and checks that copy before reuse.
Launcher/runtime upgrades preserve the host key and app data. A different state
root represents a different virtual device; deleting the key changes its
identity. Corrupt or redirected key files fail explicitly instead of silently
generating a new identity.

This is a bounded user-0 settings profile. Foreign users/callers are rejected;
unprovided keys return null so framework defaults still apply. Settings writes,
observers, query-based access and generation tracking are not implemented.
`APP_SETTINGS_OK` checks real framework reads, defaults, write denial and a
fixed derivation oracle; the signed fixture independently computes the expected
ID from its actual certificate and checks persistence through a forced runtime
replacement. `SettingsIdentityTests` checks concurrent initialization, device
and user separation, and corrupt/symlink rejection.

SpongeBob's previous `LoginWithAndroidDeviceID` failure reported
`Invalid input parameters`, with the detail that `AndroidDeviceId` or
`EncryptedRequest` must be defined. The old provider returned null for every
setting, including Android ID. PlayFab's
[Android device login contract](https://learn.microsoft.com/en-us/rest/api/playfab/client/authentication/login-with-android-device-id)
recommends `Settings.Secure.ANDROID_ID` for that request. Supplying the ID repairs
this missing framework value; it does not establish complete Android services
or gameplay compatibility.
The installed SpongeBob creation-only JIT probe reaches `APP_SETTINGS_OK` and
`FRAMEWORK_OK`, and the signed managed fixture passes all three launches. An
interpreter probe fails earlier, before these settings checks. Full-game
validation remains incomplete: subsequent diagnostic launches encountered a
host SIGSEGV during early startup, before the device-login callback was reached.

Activity metadata queries now use the installed manifest, preserving aliases,
orientation, enabled/direct-boot filters, and `GET_META_DATA` behavior, as service
and provider queries do. Permission checks grant only the installed app's
requested normal permissions, using protection levels parsed from the runtime's
`framework-res.apk` and the app's own permission definitions. Unknown names,
other UIDs/devices, runtime permissions and signature permissions are denied;
runtime grant dialogs and signature grant policy are not implemented. Package
permission flags agree with this bounded policy. See Android's
[permission types](https://developer.android.com/guide/topics/permissions/overview#types_of_permissions).
`getPermissionControllerPackageName` reports AOSP's
`com.android.permissioncontroller`, and `Activity.requestPermissions` receives
that controller's result (see the stacked-Activity section below).

`queryIntentContentProviders` matches provider intent filters read from the
installed APK cluster (base plus splits) by
[`ParsingPackageUtils.parseDefault`](https://android.googlesource.com/platform/frameworks/base/+/refs/heads/android15-release/core/java/com/android/internal/pm/pkg/parsing/ParsingPackageUtils.java),
AOSP's one-off parser outside PackageManagerService, with the same inputs as
`PackageManager.getPackageArchiveInfo`. The legacy `PackageParser` cannot be
used: `parseBaseApkCommon` dereferences `ActivityThread.getPermissionManager()`
without a null check, and this profile publishes no `permissionmgr`, so
Trickcal's AppsFlyer `queryIntentContentProviders` failed with a
`NullPointerException`. Split permissions are passed as an empty list; they
only extend requested permissions, not components.
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
queued preference writes, destroy and removal of the Activity window.
This is a bounded launcher, not a complete Android system_server or
WindowManager implementation. Views draw in software only; SurfaceView
surfaces are full-window host layers below every View window, so their
position, size within the window, clipping, `setZOrderOnTop` and
`setFixedSize` buffer sizes are not composited (surfaces report the View's
size). The UI loop still polls the host every 4 ms rather than waking from
host events. Every service and every Activity transition are not implemented.
Unsupported service calls remain explicit failures.

The managed window uses the launcher Activity label and icon from the APK's
real resources (with Android's application/default fallbacks). Android renders
the icon into ARGB pixels; the JNI bridge supplies standard UTF-8 and RGBA to
the host. SDL3 applies title/icon updates on its window thread. Compositor icon
support varies; window plugins without the optional identity operations retain
their own defaults. The native-only launcher still uses its existing identity.
Android EGL presentations now feed the shared [window statistics](../window-stats.md)
without passing SDL3 pointers through SDL2 APIs. The title is the default;
the independent stats window remains opt-in.

The signed managed fixture verifies three launches, persistent preferences and
Android ID across a runtime replacement,
resource label resolution, a real SurfaceView, Activity.finish and ordered
pause/surface destruction/stop/destroy/close. The View fixture
(`ManagedViews`) launches an Activity without a SurfaceView, checks that its
button reaches the host, and adds, hides, shows and removes a SurfaceView with
exactly two create/destroy pairs. These checks do not establish
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
