# Android runtime history

Dated checkpoints and investigations moved out of [the Android runtime guide](../Android/ANDROID_RUNTIME.md), unchanged and in their original order. They record what was true at the time; the guide and `Source/System/Android/AGENTS.md` hold the current contracts.

### Ad SDK framework gaps (2026-10-17)

Hungry Shark World's rewarded videos failed with "No advert is available":
the Google Mobile Ads request ended with error 2 although the network worked.
Its mediation adapters hit three gaps, each now closed against Android 15
sources:
- AppLovin's bid token calls `PackageManager.resolveActivity`. The strict
  `package` stub had no `resolveIntent`. `FrameworkPackages` now resolves like
  `ResolveIntentHelper.resolveIntentInternal`:
  - **Query:** an explicit component, or every enabled activity whose parsed
    filters match. `MATCH_DEFAULT_ONLY` keeps only `CATEGORY_DEFAULT` filters,
    as `ActivityIntentResolver` does.
  - **Choice** (`chooseBestActivity`): no match, the only match, or a first
    match with a higher priority or a different default.
  - **Tie:** an even tie, which needs the platform `ResolverActivity`, stays
    an explicit `UnsupportedOperationException`. Neither preferred
    activities nor the `android` package are modelled.
  - **Scope:** only the observed `resolveIntent` is wired;
    `queryIntentActivities` remains strict.
- Google's consent SDK commits `SharedPreferences` from a worker thread.
  `EditorImpl.notifyListeners` posts to `ActivityThread.sMainThreadHandler`,
  which `ActivityThread.main` sets after `attach`; `systemMain()` does not. The
  launcher now sets it from the `systemMain()` thread's handler.
- Fyber/Inneractive registers a `TelephonyCallback`. SystemServer starts
  `TelephonyRegistry` on every device, so `telephony.registry` is now
  published (see the device-services table above).

With those fixed, AppLovin still failed "Unable to check Network Interfaces":
`NetworkInterface.getNetworkInterfaces()` returned null. libcore keeps only
interfaces whose `if_nametoindex` is nonzero, and bionic answers that,
`if_indextoname` and app `getifaddrs` flags with `struct ifreq` ioctls
(`SIOCGIFINDEX`, `SIOCGIFNAME`, `SIOCGIFFLAGS`) on an `AF_INET` socket.
Host sockets returned `-ENOTTY` for every non-terminal ioctl, so every
interface was dropped. `HostNode` now marshals the fixed-layout `ifreq` get
requests through guest memory (`VFS/Nodes/InterfaceIoctls.hpp`). Requests
carrying pointers and every set request still return `-ENOTTY`.
`socket_msg_layout` checks loopback by name, index and flags in both engines
and under qemu-aarch64.

A `--trace-apk calls` session then showed the strict stubs the ad SDKs still
hit, and caught, while preparing a request. They now follow Android 15:
- **`package.queryIntentActivities`**: the Google Mobile Ads signal collector
  queries `MAIN` in its own package with `GET_SIGNATURES`, and `VIEW` with
  `MATCH_DEFAULT_ONLY`. This uses the same `queryActivities` as `resolveIntent`.
- **`package.requestPackageChecksums`**: Google Mobile Ads' app-integrity
  signal. `FrameworkPackageChecksums` follows `requestChecksumsInternal` and
  `ApkChecksums` on the pinned framework's APK classes:
  - **Optional types:** only the v2/v3 content digests are readily available.
  - **Required types:** computed, i.e. whole-file digests, the fs-verity 4K
    root and the 1 MiB partial roots.
  - **Delivery:** the listener is called once, from a background thread.
  - **Not modelled:** fs-verity hashes, IncFS and installer digests.
- **`package.getApplicationEnabledSetting`**: AppLovin's Play Store check.
  It returns `DEFAULT` for a visible package (application state never
  changes here) and throws `IllegalArgumentException` for an unknown one.
- **`activity_task.getAppTasks`**: Facebook Audience Network. It returns the
  launcher task (ID 1, as `getTaskForActivity`) when its base package is the
  caller. `IAppTask.getTaskInfo` fills the base and top activities, count,
  base intent, display 0, focus/visibility, top `ActivityInfo` and
  configuration. The task description stays default. The other `IAppTask`
  operations stay unsupported.

The `Managed` fixture checks all three on every start: resolution
(explicit, implicit in-package, `MATCH_DEFAULT_ONLY`, none), a worker-thread
commit whose listener runs on main, and telephony registration (a display-info
listener registers and unregisters; cell info without location throws),
that `NetworkInterface` lists a loopback interface, the package queries above
(checksums against an independent SHA-256 of the APK) and `getAppTasks`. With
the previous launcher the fixture fails at `package.resolveIntent` and never
reaches the later checks.

The rewarded videos still failed with error 2, exactly 60 s after each
request; host sockets showed the game never sent the request. GMA's
`RemoteAdRequestClientTask` binds Play services' `ads.service.START` and
calls `IAdRequestService` synchronously from its GmsClient thread while
holding a lock; microG's
[AdRequestService](https://github.com/microg/GmsCore/blob/master/play-services-ads/core/src/main/kotlin/org/microg/gms/ads/AdRequestService.kt)
answers through a synchronous listener callback. The Binder transport gave
that callback to an app pool thread, which blocked on the same lock, so
neither returned (see [Binder boundaries](../Android/ANDROID_BINDER.md#boundaries)).
With Linux's caller-thread targeting the answer now arrives in about 100 ms.
microG implements no ad requests: it answers every one with "Not supported"
(`CommonStatusCodes.INTERNAL_ERROR`), which GMA reports as error 8. GMA's
listener turns that answer into a final error; only a `zzdyp` failure,
which `onConnectionFailed` produces, makes `zzdwm` retry through the local
request path, as on devices without Play services. `FrameworkServiceRuntime`
therefore never binds `com.google.android.gms/org.microg.gms.ads.AdRequestService`:
`bindService` returns false and GMA requests the ad itself. Other microG
services bind normally.

In-app purchases are not offered, so the Play Store companion's
`com.android.vending/com.android.vending.billing.InAppBillingService` is never
bound either (2026-10-24). Binding it had started a whole Play Store runtime
(about 370 MiB PSS) for every game that links the Play Billing Library; a
failed bind is what a device without the Play Store reports. Battle Cats then
logs `BillingClient: Connection to Billing service is blocked.` and continues.
Other Play Store services still start its process on demand.

The local path then loaded an ad and precached its video with GMA's
ExoPlayer, which calls Android 15's `LoudnessCodecController`. `FrameworkAudio`
now serves the `IAudioService` loudness methods as
[LoudnessCodecHelper](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-15.0.0_r1/services/core/java/com/android/server/audio/LoudnessCodecHelper.java)
does for a fixed built-in speaker:
- **Range:** CTA-2075 "small" (phone/tablet), or
  `audio.loudness.builtin-speaker-spl-range-size`.
- **Parameters:** AAC target reference level 64; heavy compression for
  MPEG-4, effect type 3 for MPEG-D.
- **Updates:** `addLoudnessCodecInfo` sends its one initial update to the
  registered dispatchers on a worker (both calls are one-way). No playback
  configurations are reported, so no route-change updates follow.

ExoPlayer also watches the `Spatializer`. Without a spatializer effect,
[SpatializerHelper](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-15.0.0_r1/services/core/java/com/android/server/audio/SpatializerHelper.java)
stays `STATE_NOT_SUPPORTED`: level `NONE`, neither enabled nor available,
nothing spatialized, and its state callbacks never run.

The `Managed` fixture checks the controller with a configured AAC decoder
and the Spatializer; `ManagedPlayServices` checks that the ad request bind
fails. When this was written ExoPlayer could not play: it needs Java
`AudioTrack`, and AudioFlinger had no track creation (it has since, see
[AudioTrack playback](../Android/ANDROID_RUNTIME_REFERENCE.md#audiotrack-playback-2026-10-21); `generateAudioSessionId`
and ExoPlayer itself were not rechecked), so GMA played the rewarded video in
its WebView, whose audio uses AAudio.

The ad window then froze black a few seconds in: the emulator was dumping
core after a host SIGSEGV, a NULL read in glibc's `memcpy` on HWUI's
RenderThread or WebView's GPU thread. GraphicThunk tracked GL bindings in
one tracker for every context, and the app, HWUI and WebView render with
their own EGL contexts on different threads. Another context's unbind made a
pixel-unpack-buffer upload at offset 0 look like a NULL client pointer.
Trackers are now per EGL context (see the
[graphics guide](../../Source/System/Graphics/AGENTS.md)). Afterwards a
Mintegral rewarded video (mediated) played to the end, its store link
opened Mintegral's web store page (there is no Play Store app), and the game
resumed. That session then ended by real-time signal 63 (status 191), which
is not yet explained. A Google WebView ad has not been retested, and the
Play page's SVG logo drew only one of its four paths.

A later Google rewarded video used GMA's `MediaPlayer` video view, whose
`PlayerBase` registers with `IAudioService.trackPlayer`; the unsupported
call ended the app. `FrameworkAudio` now follows
[PlaybackActivityMonitor](https://android.googlesource.com/platform/frameworks/base/+/refs/heads/android15-release/services/core/java/com/android/server/audio/PlaybackActivityMonitor.java)
for player IDs: each player gets a unique piid, and `playerAttributes`,
`playerEvent`, `playerSessionId`, `playerHasOpPlayAudio` and
`releasePlayer` are accepted without reporting playback configurations.
The `Managed` fixture creates, configures and releases a `MediaPlayer`.
There is no `media.player` service, so the video itself is not expected to
play through `MediaPlayer`; how GMA handles that failure is untested.

### Global Umamusume after the null-call fix (2026-10-22)

The next stall was traced to Bionic's spawn path. It queried signal dispositions
in the child; `rt_sigaction(SIGKILL, NULL, &old, 8)` incorrectly returned EINVAL.
[Linux do_sigaction](https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c)
rejects SIGKILL/SIGSTOP only when a new action is supplied.
[Bionic's spawn implementation](https://android.googlesource.com/platform/bionic/+/741541d307f2b48c5c87fb7f38e6618d3175dbd0/libc/bionic/spawn.cpp)
exits 127 if querying a disposition fails. Read-only queries now succeed while
setting either disposition remains forbidden.

That child then hung in host `pthread_cond_destroy` during emulator teardown:
its copied private futex table retained parent waiters that no longer existed
in the child. Fork now snapshots these tables under their shard locks and
abandons copied host waiter objects in the child, preserving guest memory but
starting fresh private queues. The parent retains its original queues.
Further tracing caught SDL teardown using the inherited Wayland connection,
which could hang or invalidate the parent's objects. A host-fork child's
`exit_group` now terminates directly through host `_exit`, bypassing host
destructors. Actual host-fork ownership is tracked separately from the CPU's
host-PID identity flag used by fresh Binder-realm processes.

With these fixes, the child actually terminates and the parent reaps status
`0x7f00`, then explicitly calls `exit_group(127)` itself. The command passed to
the child is `/system/bin/sh -c -- 'am start --user 0 -f 0x10200000 -a
android.intent.action.MAIN -c android.intent.category.HOME'`; exec fails ENOENT
because this runtime has no `/system/bin/sh`. This establishes an app-requested
HOME/termination path, not the reason the app selected it. Protection rejection
is not confirmed, and successful startup/gameplay is not established.

### Guest ptrace continuation (2026-10-22)

The unconditional ptrace rejection is now replaced by guest task tracing.
One process-shared control domain follows an emulator's fork family; it never
attaches a host debugger or exposes host register state. ATTACH/TRACEME establish
ownership, the target stops at a committed CPU boundary, and wait4/waitid report
the stop. CONT/DETACH can suppress or inject a signal; SINGLESTEP executes one
guest instruction through the interpreter, then stops again. Default execution
continues to use the selected JIT. SEIZE/INTERRUPT, PEEK/POKE, GETSIGINFO,
GETREGSET/SETREGSET for AArch64 GPR, FPSIMD and TLS, and EXITKILL are implemented.
Stopped CPUs execute memory/register commands themselves. Forced memory access
uses `/proc/self/mem` only after validating the guest mapping and window bounds;
code writes invalidate translations. Related registered tasks expose their
guest identity and TracerPid through `/proc/<tid>/status`.

The contract follows Linux [ptrace](https://man7.org/linux/man-pages/man2/ptrace.2.html)
and the [AArch64 UAPI register layouts](https://github.com/torvalds/linux/blob/v6.12/arch/arm64/include/uapi/asm/ptrace.h).
The focused fixture compares process, stop/wait, signal, memory and exec behavior
with native Linux; its AArch64 register/step checks run in Bifrost, not on a
native ARM machine. This is a bounded compatibility implementation, not a full
Linux debugger: syscall tracing, fork/clone/exec event options, hardware debug
regsets, unrelated emulator processes, and general job-control group stops are
unsupported. Only TRACESYSGOOD and EXITKILL option bits are accepted. The domain
has 1,024 task records; exited unowned records without tracer references can be
reused. Existing overlapping synthetic worker TIDs after fork are rejected as
ambiguous. Guest credentials/dumpability are checked; host Yama/LSM policies are
not emulated. `BIFROST_PTRACE_TRACE=1` logs requests and results.

A default-JIT windowless Global run (session
`40ef5e4550dc44e28cf6dd1c1671a618`) now observes successful child ATTACH and CONT,
loads `lib__4e06__.so`, and reaches `APP_APPLICATION_CREATED`,
`APP_ACTIVITY_CREATED` and `APP_RESUMED`. The child also successfully requests
GETSIGINFO and continues SIGBUS/SIGSEGV delivery stops. Thus the former ptrace
stub was a concrete startup blocker. This does not identify a particular
commercial protection vendor or establish a server-side integrity verdict.
The app-local property experiment described above remains in effect.

The same run subsequently reports `IllegalStateException: surface preparation:
-22` in FrameworkSurfaces with the windowless fixture plugin, then Java VM exit
status 1. The managed run reaches its 60-second timeout (124), so clean process
shutdown is also unconfirmed. Visible rendering, title/login screens and
gameplay have not been tested by this run. The surface failure is separate from
the earlier loader/self-debugging failure.

Subsequent normal-window traces supersede the windowless rendering limitation:
session `02048892f06d436ab4460aa463fb2b1c` successfully swaps Unity frames at
about 30 FPS. Thirty-five sampled 3x3 back-buffer grids are identical. This
supports the reported static image, but does not prove every pixel is unchanged.
The Java/ART dump's Unity `pthread_cond_wait` at `libunity.so+0x6e7bf8` is not
evidence of a stalled clock: two direct snapshots in session
`fcc8027100af45f48f1e97c6dd4e342d` observe its counter advancing from 290 to 475
over 3.3 seconds; the first wait requests counter 292. The initial suspicion of
a stuck timing counter was therefore rejected. The application can continue
presenting the same scene while game initialization remains incomplete.

The latest microG child log reports `GamesConnectService: signIn success`, then
a GamesService connection and `Not yet implemented: registerEventClient`.
Earlier, before the user completed the account window, automatic account
selection reported no account. Thus returning from that window alone did not
prove authentication, while the later run does report initial Games sign-in
success. This still does not establish delivery of every subsequent game
callback, server-side authorization, or successful game login. The event-client
stub is an observed gap, not a proven cause of the static scene. Lightweight
framework tracing has no unfinished local framework calls at its final
heartbeat; these traces do not cover every direct Games Binder callback.
The separate ART method-tracing run timed out before rendering and is not used
to diagnose the scene. No rendering or authentication fix is claimed here.

The real app also exposed a stale
Binder receive-arena protection guard after exec; clearing device mappings
before image replacement fixes the shell's atexit-array EACCES without changing
the parent's broker arena (fork-child RPC is rejected by owner PID).

Validation: SDL and headless builds, 16 focused Python tests, the offscreen
managed HOME/resume regression, plugin ABI checks, and the Bionic bootstrap
matrix (fixed/random addresses, JIT/interpreter/Tier-2, services off/on), plus
AAudio/OpenSL callback regressions in all three engine modes passed. The shell
regression uses the real pinned SDK shell. Terminal layout and ELF auxv/entry
contracts above were checked against Linux source; game exit behavior and
lifecycle ordering are observations from these runs.

Window plugin v2 appends optional `set_task_visible`; old descriptors still
load, but task visibility returns ENOTSUP. The built-in backend and packaged
SDL plugin implement hiding/showing the existing window. Headless tests use
a windowless plugin and Qt's offscreen platform; they establish routing and
lifecycle, not compositor focus behavior or gameplay.

`test_signal_semantics` checks both uncatchable signal queries and rejection of
setters. `test_fork_threads` adds three workers blocked on a private futex,
checks that the child's wake finds zero inherited waiters, and verifies child
exit status and continued parent operation. Both pass under QEMU, JIT,
interpreter and Tier-2; the focused existing process/signal/futex/exit selection
passes 10/10 in JIT and 10/10 in the interpreter. SDL and headless builds pass;
the two fixtures also pass on the headless build. These are bounded regressions,
not a full-suite or game-playability result.

### Real media JNI (2026-10-13)

The framework provisioner installs the pinned SDK image's real
`/system/lib64/libmedia_jni.so` and its native dependency closure. Android's
boot classes load it dynamically, so following only `app_process64`'s
DT_NEEDED dependencies previously omitted it. Azur Lane's Unity MediaCodec
initialization exposed this as an `UnsatisfiedLinkError`. No JNI methods or
success-returning media substitutes are supplied by Bifrost. Android 15's
[JNI sources and build inputs](https://android.googlesource.com/platform/frameworks/base/+/android-15.0.0_r1/media/jni/Android.bp)
define the implementation; the installed binary comes from the same
checksum-verified SDK archive as the framework.

Framework manifest schema 7 requires a fresh provisioned directory. Preserve
the old root before running `Tools/Android/android_framework.py`; do not edit
its inventory or graft unverified libraries into an existing runtime. Managed
app roots replace themselves from the new verified inventory while retaining
app data and System settings. Fresh extraction adds eight native files and
leaves every schema-6 runtime file unchanged.

`ActivityProbe$MediaJniProbe` initializes MediaCodec, MediaCodecList,
MediaExtractor, MediaMetadataRetriever, MediaPlayer, MediaMuxer and MediaCrypto,
then writes three AAC silence samples through the real native MP4 muxer.
It passed in JIT (3.0 s) and interpreter (10.4 s). Independently, host FFmpeg
parsed each output as mono 8 kHz AAC with three frames and decoded exactly
6,144 bytes of zero PCM. These checks establish JNI loading/registration and
native muxing, not guest decoding or playback.

Azur Lane's follow-up GPU-enabled session
`c4507e78efc749aea94c80a0acd91b84` loaded the JNI library successfully,
continued to a logged Account Recovery popup and reached its 150 s limit
(exit 124). Neither stdout nor logd contained fatal-exception, native-fault
or missing-JNI markers. This removes the observed missing-library crash;
the screen was not visually inspected and gameplay/video playback remains
unverified.

### Wi-Fi display and local media routing (2026-10-15)

The display service models Android 15's **absent Wi-Fi display adapter**:
`getWifiDisplayStatus` returns a fresh `WifiDisplayStatus` with feature
unavailable, no scan or connection and no devices. Disconnect is unprivileged;
scan/connect/rename/forget/pause/resume enforce `CONFIGURE_WIFI_DISPLAY`.
Address validation precedes permission checks where AOSP does; scan requests
require the process's display callback, are idempotent and are retired on its
Binder death. No Miracast discovery, connection or remote display is advertised.

Unity's real `MediaRouter` constructor also needs audio route observation and
the legacy `media_router` service. Audio reports one fixed local speaker route
and no Bluetooth A2DP, with fresh snapshots and death-tracked observers.
Legacy router clients have package/user validation, Binder death/unregister
cleanup, retained discovery/selection state and AOSP's trusted-client filtering.
There are no remote route providers; group routing and Router2 operations
still throw explicitly.
This is local media routing, not wireless casting or arbitrary output-device
management.

Contracts were compared with Android 15's
[DisplayManagerService](https://android.googlesource.com/platform/frameworks/base/+/android-15.0.0_r1/services/core/java/com/android/server/display/DisplayManagerService.java),
[MediaRouter](https://android.googlesource.com/platform/frameworks/base/+/android-15.0.0_r1/media/java/android/media/MediaRouter.java)
and [MediaRouterService](https://android.googlesource.com/platform/frameworks/base/+/android-15.0.0_r1/services/core/java/com/android/server/media/MediaRouterService.java).
The real-framework display probe passed 47 checks in each engine, including
the complete MediaRouter constructor, permission denial/grant, snapshot
isolation and client registration/unregistration. An installed Azur Lane 9.3.7
trace returned successfully from Wi-Fi status, audio-route observation and
router registration/selection with no traced service throws. These checks do
not establish remote Binder death delivery, casting, login or gameplay.

### Azur Lane interaction checkpoint (2026-10-14)

The installed `com.YoStarEN.AzurLane` 9.3.7 was run with the current SDL/GL
build and normal JIT settings. The app process held an actual amdgpu
`renderD128` context with increasing GPU engine counters, and the installed
launcher DEX matched the current build. Completed logs reached Yostar's login
Activity and emitted repeated Choreographer skipped-frame notifications.
Desktop visual tools were unavailable during that diagnostic run. Later
user-supplied October 14 [screenshots](../Apps/azur-lane.md#screenshots-2026-10-14)
visibly capture loading and the title/login screen. Neither checkpoint
establishes gameplay, working text input or measured sustained FPS.

One run ended after `APP_HOST_CLOSED` with the app sending itself SIGKILL during
teardown; another reached its configured time limit. A follow-up session
(`9638860a0eb24002b3a98c1829713969`) exited with status 1: Google sign-in failure
invoked `Toast.show`, which reached unsupported `notification.enqueueToast`.
It ended before the scheduled late-scene profile. This is a concrete service
failure, not an established cause of the prior stutter.

The [startup codegen audit](JIT-history.md#azur-lane-generated-code-audit-2026-10-14)
precedes the regular login-scene delays. Remaining work is real toast lifecycle,
input/focus/scroll reproduction on an attached WebView, desktop IME composition,
implicit browser intent resolution and supported callback delivery. Browser
launch does not supply Google Play Services authentication. Keep those failures
separate from JIT, software window drawing and codec compatibility.

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
wrapped behaviour. `ANativeWindow_lock`, `unlockAndPost` and `toSurface` reach the image's library for a
framework Surface; a host window handle has no CPU-visible buffers, so locking returns `-EINVAL` and
`toSurface` returns NULL. Firefox's `libxul.so` imports all three.
`ASurfaceControl_createFromWindow` had no wrapper at all, so YouTube's `libgoogle3.so` failed to load (Bionic
binds imports at `dlopen`, whether or not the function is ever called). It is now a wrapper in
`surface.c`: a Bifrost host window handle has no SurfaceFlinger layer behind it and returns NULL (creation
failed, which the NDK allows); a framework Surface goes to the image's library, which also returns NULL while
`ISurfaceComposer.createConnection` is not served. `test_android_adapter_exports.py` checks the built
libraries.

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
unsupported and their extensions are not advertised.

Native-buffer import is supported (`EGL_ANDROID_image_native_buffer`,
`EGL_ANDROID_get_native_client_buffer`). HWUI samples a TextureView's
producer buffers this way
([AutoBackendTextureRelease](https://android.googlesource.com/platform/frameworks/base/+/refs/heads/android15-release/libs/hwui/AutoBackendTextureRelease.cpp)
through Skia's `MakeGLBackendTexture`), as does libgui's `GLConsumer`.
Before, the adapter rejected `EGL_NATIVE_BUFFER_ANDROID`, Skia returned an
invalid texture and HWUI aborted. That happened to a Fyber video ad in
Hungry Shark World (`Invalid GrBackendTexture. Width==1920, height==1080`).

The guest adapter checks the `ANativeWindowBuffer` and holds its reference
until the image is destroyed. `GraphicThunk/AndroidBufferImages.cpp` maps the
buffer's gralloc memfd read-only on the host. Each GL context that binds the
image gets a host texture with a host `EGL_GL_TEXTURE_2D_KHR` image on it.
Every `glEGLImageTargetTexture2DOES` refreshes that texture from the buffer
with `glTexSubImage2D`, saving and restoring the unpack and texture state it
touches. HWUI's update proc and `GLConsumer` rebind once per new frame.
Guest handles are tagged values, never host pointers.

Formats are RGBA/RGBX/BGRA 8888, RGB888, RGB565 and YUV 4:2:0 (gralloc's
planar Y, V, U). YUV converts on the CPU with the buffer's dataspace matrix
and range through the converter `FrameworkSurfaces` uses (`Surface/Yuv420.cpp`, with
an AVX2 kernel where the host has it): BT.709, BT.2020, otherwise BT.601;
limited range unless the dataspace says full. A buffer smaller than its own
geometry is refused and shows opaque black, never read past. FP16, 1010102, R8 and protected
buffers are refused (`EGL_BAD_PARAMETER`, reason under `BIFROST_THUNK_TRACE`).
`glEGLImageTargetRenderbufferStorageOES` uses the same host image. The image is a copy refreshed at
bind time, not a live alias: a consumer that samples a rebound buffer
without rebinding it sees the previous upload.

Writes into the image reach the buffer for one case: HWUI's hardware-bitmap upload (`EGLUploader`: bind the
buffer's image to a texture, `glTexSubImage2D` the bitmap's pixels, destroy the image). Before this the pixels only
went to the host texture, so every `Bitmap.Config.HARDWARE` bitmap (what Glide and other image loaders produce) read
back as zeros and YouTube's thumbnails and avatars were empty boxes, with nothing in the log. The thunk now also
copies a `glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels)` into the buffer's memory
(`AndroidBufferImages::capture_upload`) when the bound texture is the app's view of an RGBA8 image, honouring the unpack
alignment, row length and skips; the call still goes to the host too. The buffer is mapped read/write when its memfd
allows. Not covered: other formats (F16, 565), a pixel unpack buffer, other ways a GPU writes into such a buffer
(rendering into a framebuffer attached to the image, `glCopyTexSubImage2D`), and `ManagedHardware` checks only a solid
`ARGB_8888` bitmap copied to `HARDWARE` and back. Symbol coverage is not
EGL conformance. The host-GPU `ActivityProbe$EglProbe` passes registration,
duplicate/invalid-argument errors, proc-address identity, platform selection,
clock and terminate/reinitialize checks (`ANDROID_EGL_CONTRACT_OK`). Generated
EGL signatures also pass `make opgen-thunk-check egl-check`.

Display initialization is reference-counted, as in Android 15's
[egl_display_t](https://android.googlesource.com/platform/frameworks/native/+/refs/heads/android15-release/opengl/libs/EGL/egl_display.cpp):
`eglTerminate` terminates the host display only when it releases the last
`eglInitialize`. A process's GL renderer, HWUI and WebView share one display,
and HWUI's
[EglManager::destroy](https://android.googlesource.com/platform/frameworks/base/+/refs/heads/android15-release/libs/hwui/renderthread/EglManager.cpp)
terminates it when its last hardware window goes. In Hungry Shark World that
was the ad's window: the host display died under Unity (software-drawn
Activity, own GL renderer), which then failed every surface call with
`EGL_NOT_INITIALIZED`. `EglProbe` checks that a second user's terminate
leaves the display initialized.

A host surface lives while it has owners: the framework's preparation
(`bifrost_surface_prepare`, released by `bifrost_surface_release` when a
SurfaceView's surface or a window's HWUI layer goes) and each host EGL surface
created on it. App `ANativeWindow` references are not mirrored to the host.
They also change through libnativewindow and `sp<Surface>`, which the libandroid
adapter never sees. HWUI's
[setSurface](https://android.googlesource.com/platform/frameworks/base/+/refs/heads/android15-release/libs/hwui/jni/android_graphics_HardwareRenderer.cpp)
looks up `ANativeWindow_fromSurface` in libandroid but calls
`ANativeWindow_release@LIBNATIVEWINDOW`. When the adapter counted the first and
not the second, every hardware-drawn window's host layer leaked one reference
and stayed on screen after the window closed. In Hungry Shark World the
rewarded ad's end card covered Unity, which kept rendering at 60 FPS
underneath. `EglProbe` replays HWUI's two calls on a prepared surface and
checks that releasing the preparation removes the host backing
(`ANDROID_SURFACE_RELEASE_OK`).

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

Managed play mode also applies the top Activity's requested orientation to the
SDL window through private window operation 23 (0 releases the constraint,
1 selects portrait, 2 landscape). `BEHIND` inherits the next Activity's request.
The launcher publishes display rotation, bounds, resources and configuration
changes through the existing resize path, including reverse rotations without
a size change. A fixed-size app's rotated window has its virtual panel's shape (the size the window
opened at, in desktop units), limited by the work area the open-time clamp revealed (see Window shape
below); the panel keeps those pixel dimensions whatever size the user gives the window, and the fit
letterboxes it uniformly.
Sensor/user axis requests retain the current reverse direction; physical sensor
rotation and Activity recreation are not implemented by this path.

Uma Global's cover-art stall was traced to its live
`Screen.WaitDeviceOrientation` coroutine: it repeatedly observed landscape
while waiting for portrait, blocking `GameSystem.InitializeGame`. Before this
change, requested orientation was stored but never applied to the display.
The approved live verification (session `63f0656adf534ee39ba21f7168d85a5a`)
returned from microG cancellation, applied portrait request 1, resumed Unity
rendering, and the user confirmed that the game advanced beyond the cover art.
This establishes progress past this startup gate, not full gameplay or Google
sign-in support.

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
For a black frame, `[frame] swap N` lines (the first three swaps, then every
120th) sample a 3x3 grid of the default back buffer before the host swap and
list the swapping thread's GL/EGL calls since its previous swap. A non-zero
`glGetError` result appears as `ERROR(code after call)`, naming the call before
it; an error from an emulator-internal call shows up after the guest call that
triggered it. Mesa's own reason is printed with `MESA_DEBUG=1`. All bindings
and pack state the sampler touches are restored.
`[android-window]` records SDL window events before filtering and a two-second
event-pump heartbeat. Event, pump and EGL timestamps use the host monotonic
clock (SDL event times are rebased onto it). These diagnostics observe restore/expose events;
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
real exit code in the session record; the command itself then exits 0, as for any
normal close. A SIGKILL without the marker remains `failed` and exits 137.

### Complete IAppOpsService (2026-10-21)

Battle Cats (Tapjoy `checkOpNoThrow(OP_GET_USAGE_STATS)`), gms, SpongeBob and
Hungry Shark (`OP_SYSTEM_ALERT_WINDOW`) threw `UnsupportedOperationException:
appops.checkOperation 43/24` because the service implemented three methods.
`FrameworkAppOps` now implements every method of android-15.0.0_r1's
`IAppOpsService.aidl` (59: the `FrameworkServices.Call` enum drives dispatch
and the inventory, and `ManagedServiceBatch.appOps` compares it with the
framework interface at run time). No method throws
`UnsupportedOperationException`. It follows `AppOpsService`, `AttributedOp`,
`AppOpsRestrictionsImpl` and `AudioRestrictionManager` of that tag:

- Checks: `verifyIncomingOp` (`IllegalArgumentException` for a bad code;
  restricted-read ops need `MANAGE_APPOPS`, `GET_APP_OPS_STATS` or
  `MANAGE_APP_OPS_MODES`). Without an override the answer is
  `opToDefaultMode(opToSwitch(op))`, as stored (a `MODE_FOREGROUND` default
  stays 4 until the op has a record), and `MODE_FOREGROUND` evaluates to
  allowed afterwards because the process is the foreground app. A package that
  does not exist or fails verification gives the default; a name that
  resolves to nothing gives `MODE_IGNORED`. Uid overrides win over package
  modes; user restrictions answer `MODE_IGNORED`.
- `note`/`start`/`finish` and their proxy and device variants record
  accesses, rejections, durations and proxy info per attribution tag
  (`getOpsForPackage`, `getPackagesForOps`, `getHistoricalOps` serve them as
  `PackageOps`/`OpEntry`/`HistoricalOps`); invalid package or failed
  verification is `SyncNotedAppOp(MODE_ERRORED)`. Starts are counted per
  client binder and a dead client's start is released.
- Watchers: mode, active (op goes inactive to active and back), started,
  noted and async-noted callbacks are delivered by reflection from one service
  thread (oneway, never under the lock) with the AOSP 15 signatures. Without
  `WATCH_APPOPS` a watcher only hears its own uid; restricted-read ops are
  hidden from watchers without `MANAGE_APPOPS`.
- Privileged methods (`setMode`, `setUidMode`, `resetAllModes`, user and audio
  restrictions, history management, `getUidOps`) check the permission for the
  calling uid. The service runs in the app's process, so AOSP's
  `callingPid == myPid` bypass is deliberately absent: an app gets
  `SecurityException` exactly as on a device. The package-private
  `setModeUnchecked`/`setUidModeUnchecked`/`setUserRestrictionUnchecked` are
  the in-process permission-policy path.
- Not modelled: persistence between launches, package visibility filtering,
  uid-state tracking, suspended-package restrictions, role grants on reset,
  DPM deferral and virtual devices (any device id but the default is invalid,
  as with an empty `VirtualDeviceManager`). Runtime-message sampling answers
  "sample nothing" (`collectRuntimeAppOpAccessMessage` is `null`, as for a
  non-system caller). A reserved op whose switch op is `OP_NONE` is its own
  switch (AOSP would index its table with -1).

Hidden APIs (`opToDefaultMode`, `opToSwitch`, `resolvePackageName`, the
`SyncNotedAppOp`/`PackageOps`/`OpEntry` constructors, `makeKey`) are called by
reflection because the build links the public `android.jar` at `--release 8`.

Audit of the other services (2026-10-21, `ManagedLauncher --service-inventory`
and the `UnsupportedOperationException` lines of the Hungry Shark, Roblox,
SpongeBob, Battle Cats, Azur Lane and gms session logs). Of the 36 registered
Java services only `appops` (59 of 59), `permission_checker`, `batteryproperties`
and `vibrator_manager` cover every interface method. Methods still unsupported,
as `implemented/unsupported` counts: wifi 10/199, audio 38/227, notification
17/139, batterystats 4/95, user 26/78, display 15/44, power 18/49,
telephony.registry 1/53, location 13/49, appwidget 2/30, media_session 1/29,
media_router 9/30, permissionmgr 12/20, uimode 12/15, trust 5/11, deviceidle
13/9, clipboard 8/4, account 40/6, alarm 9/1; jobscheduler, connectivity,
input_method, mount, input, game, package, content, window, thermalservice,
activity, activity_task and the Bifrost-internal services are unclassified
(lambda dispatch, no per-method contract). Most names in the older session logs
(`audio.isMusicActive`, `notification.cancelAllNotifications`,
`package.resolveIntent`, `audio.isBluetoothScoOn`, ...) are already handled
by later builds. Still failing in recent logs: `appwidget.getInstalledProvidersForProfile`
(Roblox, 22 calls; now answered: own profile, empty list, `null` for other
profiles, `SecurityException` for a foreign package, with provider enumeration
not modelled), `notification.enqueueToast` (Azur Lane's `Toast.show` after a
cancelled Facebook login; now accepted, see below), and `Cannot create
AudioTrack`, which is an audio-output limit, not a service contract.

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
provider acquisition and provider startup in child processes were added
later; see [microG processes](../Android/ANDROID_RUNTIME_REFERENCE.md#microg-processes-remote-providers-and-service-resolution-2026-10-17).

Starting the previously omitted AndroidX WorkManager provider exposed its
force-stop check: `PendingIntent.getBroadcast(..., FLAG_NO_CREATE, ...)`.
`IActivityManager.getIntentSenderWithFeature` now validates the installed owner,
user, sender type and intents and returns null for an empty-registry
`FLAG_NO_CREATE` lookup. This follows the lookup contract in Android 15's
[PendingIntent](https://github.com/aosp-mirror/platform_frameworks_base/blob/android-15.0.0_r1/core/java/android/app/PendingIntent.java).
Sender creation and in-process delivery are now supported as described in
the Settings, PendingIntents and broadcasts section above. Process restart
and cross-app background PendingIntent delivery remain unavailable.

In the real untraced JIT session `84a17b02fbb54a1d83802245e543c2a3`, Azur Lane
passed that path and wrote 2,305,675,539 asset bytes across 12,470 files, up from
78,150 bytes in two files before the fix. This is filesystem evidence of
downloading, not a claim that the roughly 24.5 GB queue completed or gameplay
was reached. The session was left running during logger validation.

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

(2026-10-25: policy transaction 3, `getDeviceConnectionState`, answers `UNAVAILABLE` because the profile has no
ports, and the session also publishes `audio` to the native ServiceManager
(`FrameworkNativeServices.AudioService`): native `AudioTrack` looks it up with `checkService` to send the one-way
`playerEvent` with its port id and logged "binding to audio service failed" without it. It answers
`trackPlayer`/`trackRecorder` with an id and accepts the one-way player and recorder reports without keeping them; the
codes are read from the framework's `IAudioService.Stub`. `ManagedServiceBatch.nativeServices` transacts with
both and with a malformed policy request.)

The session's managed launcher registers `media.audio_policy` and
`media.audio_flinger` stand-ins. They answer only the Android 15 transactions
native AudioSystem makes here (policy 3/8/44/49/51/52/53/80/82,
flinger 3/5/6/23/45/46; codes
checked against `IAudioPolicyService.aidl`/`IAudioFlingerService.aidl` at
`android-15.0.0_r1`): no HAL ports or patches, and the primary output's rate
and frames per buffer come from Bifrost's host Audio engine
(`APP_AUDIO_ROUTING_READY`). AAudio playback never uses AudioFlinger: the
managed `libaaudio.so` adapter sends AAudio to the AudioThunk. (Native
`AudioTrack` clients do, below.) Like real
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

A requested buffer capacity grows the stream's host ring to at least that many
frames (`Audio::stream_open`'s `min_frames`), as AudioTrack sizes its buffer to
the request; above 262144 frames `openStream` answers `UNAVAILABLE`. FMOD
(Roblox) reopens its probe stream with twice the default capacity and fails its
output init if that is refused. The hardware getters report the host device
the engine opened, `getContentType` the builder's value (default `MUSIC`), and
the MMAP policy is recorded but streams never use MMAP. Every name of Android
15's `libaaudio.map.txt` resolves in the managed adapter
(`android_aaudio_thread.c` checks the table with `dlsym`).
`BIFROST_AUDIO_TRACE=1` prints each audio thunk call with its arguments.

### First SpongeBob rendering milestone (2026-10-07)

This supersedes the earlier managed-audio checkpoint's unknown null-object
failure. Unity's aligned stack frame was broken by ignored AND-to-SP writes;
logical immediates now honor SP/WSP in both engines. The next crash came from
GLES returning a host mapped-buffer pointer to guest code; GLES now uses the
existing guest-buffer map/unmap/flush policies. See [JIT.md](../JIT.md).

ActivityProbe now implements the newly observed calls explicitly:

- `reportActivityFullyDrawn` records the first completion for a known Activity
  token; unknown tokens are ignored, as in AOSP's ActivityRecord lookup.
- `overridePendingTransition` (synchronous) completes without recording
  anything. In AOSP's
  [ActivityClientController](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-15.0.0_r1/services/core/java/com/android/server/wm/ActivityClientController.java)
  it only selects custom animations for the next window transition, and
  Bifrost does not animate window transitions. The Yostar SDK calls it from
  `BackgroundAct.finish()` when Azur Lane closes its web view; the missing
  stub previously ended the app with `System.exit(1)`.
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
[JIT.md](JIT-history.md#reciprocal-estimates-and-refinement-spongebob-2026-10-07)).
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
only finding a Java service-cache entry. See [test evidence](TESTS-history.md#native-sensor-service-follow-up-2026-10-07).

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
