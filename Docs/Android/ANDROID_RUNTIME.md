# Asgard Android compatibility layer

Asgard is the Android layer of Bifrost: guest Bionic/ART, framework adapters,
APK installation, app sessions, and shared Binder communication. One-off checkpoints and per-game investigations are in
[the runtime history](../archive/ANDROID_RUNTIME-history.md), listed under [Recorded changes](#recorded-changes). Original
integration code uses the project Unlicense; reused AOSP material keeps its
upstream terms. See [license scope and Android attribution](../../LICENSES/README.md).
Existing `android_package.py` commands remain supported. The Asgard entry point also
starts one shared Binder broker per state root:

```sh
python3 Scripts/Android/asgard.py --state-root android-state realm start
python3 Scripts/Android/asgard.py --state-root android-state start com.example.app
python3 Scripts/Android/asgard.py --state-root android-state list
python3 Scripts/Android/asgard.py --state-root android-state realm status
# Stopping the realm disconnects its clients; --force also ends the app processes still joined to it.
python3 Scripts/Android/asgard.py --state-root android-state realm stop [--force]
python3 Scripts/Android/asgard.py --state-root android-state realm restart [--force]
```

A broker that an upgrade left running (its program was replaced by a different build) is restarted by the next
`start` or `run` when no app is joined to it, and refused with a message naming `realm stop --force` when one is:
the protocol number can match while the Binder behavior no longer does, and a new app then fails on its first
service call.

`--debug CHANNELS` (before the command, like `WINEDEBUG`) turns on trace output:
`asgard.py --debug binder,futex run org.example.app`. Channels: `audio`, `binder`,
`dynlink`, `fault`, `frame`, `futex`, `input`, `media`, `mmap`, `poll`,
`process`, `thunk`, `x11` and `all` (the emulator's blanket `BIFROST_TRACE`).
Each sets the matching trace switches in `Source/System/Core/DebugFlags.hpp`;
only logging switches are mapped. Sessions started by the command inherit
them. A realm broker keeps the channels it was started with (`realm stop`, then
start it with `--debug`) so `binder` is seen only that way. An unknown channel
is rejected before anything runs.

This guide retains dated implementation and failure checkpoints. Later
checkpoints supersede earlier limits only for the contracts they cover;
an old failure report is not automatically a current compatibility claim.
See [current support](../SUPPORT.md#android-scope-and-v20-plans),
[host media](#host-media-decoding-2026-10-14) and the
[roadmap](../roadmap.md) for the latest scope and remaining work.

### Per-process compatibility defaults (2026-10-23)

`FrameworkCompatibility` initializes ART's target SDK and disabled compatibility
changes, installs `AppCompatCallbacks`, then runs libcore's application
specialization hook before Application creation. Main apps and service children
use their own `ApplicationInfo`, including provisioned system packages; a child
does not inherit the session game's target SDK. Defaults come from the verified
runtime's partition and APEX `etc/compatconfig/*.xml` files, with
`enableSinceTargetSdk`, `enableAfterTargetSdk` and disabled-by-default rules.
Unknown changes retain Android's enabled default. This implements the metadata
available in the provisioned image (currently the ART/libcore inventory), not
system_server's per-package developer overrides or the complete platform change
catalogue. An empty metadata inventory or conflicting entries fails initialization
explicitly.

This fixes microG API-29 processes incorrectly receiving
`ENFORCE_READ_ONLY_JAVA_DCL` (218865702), enabled only for API 34+ targets.
The launcher previously left Android's default compatibility delegate installed,
which enabled every queried change. Cached file permissions are untouched and
API 34+ processes still reject writable dynamic DEX. The contract follows
[ActivityThread](https://github.com/aosp-mirror/platform_frameworks_base/blob/android-15.0.0_r1/core/java/android/app/ActivityThread.java),
[RuntimeInit](https://github.com/aosp-mirror/platform_frameworks_base/blob/android-15.0.0_r1/core/java/com/android/internal/os/RuntimeInit.java),
[CompatChange](https://github.com/aosp-mirror/platform_frameworks_base/blob/android-15.0.0_r1/services/core/java/com/android/server/compat/CompatChange.java)
and the image's compatibility metadata. This does not supply hardware
attestation or a Google integrity verdict.

### Windows owned by other threads (2026-10-17)

Hungry Shark World exited with status 1 when a round ended. Unity's
`UnityMain` thread, which runs its own Looper, showed a dialog, and the
launcher's UI loop drew it from `main`. The dialog's `UnityPlayerActivity`
declares `android:hardwareAccelerated="false"`, so the window is software.
A button's `RippleDrawable` got new bounds during `View.draw` and invalidated
it. Software windows take `ViewGroup.invalidateChild`'s parent walk, which
ends in `ViewRootImpl.invalidateChildInParent` → `checkThread`, so the draw
threw `CalledFromWrongThreadException` (Expected: UnityMain Calling: main).
Hardware-accelerated windows take `onDescendantInvalidated`, which has no
thread check. A dialog inherits its parent Activity's flag
(`Window.adjustLayoutParamsForSubWindow`). All checked against
[Android 15 ViewRootImpl](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-15.0.0_r1/core/java/android/view/ViewRootImpl.java),
`View`, `ViewGroup` and `Window`.

Android runs each window's traversals, drawing and input on the Looper of
the thread that created its ViewRootImpl (`mThread`). `FrameworkWindows` now
does the same for windows that thread owns:
- Their software draw (`drawForeign`) is posted to `ViewRootImpl.mHandler`.
  The main-thread compositor shows the pixels once that draw finishes; a
  volatile `drawing` flag hands the bitmap over between the threads.
- Touches, keys and `forceWmRelayout` are posted to that thread too.
  `windowFocusChanged` already posts to the owner itself.
- `remove()` marks the window removed under the window lock, so no host
  layer appears for it afterwards.

SurfaceViews in those windows get host surfaces too. SurfaceHolder
callbacks run on the window's own thread, where SurfaceView delivers them:
- `FrameworkWindows.updateSurfaces` posts each window's `FrameworkSurfaces.update`
  to its owner, one at a time per window. The ViewTreeObserver listeners are
  added on that thread.
- `FrameworkSurfaces` takes one lock for its entries and for every host call
  on an entry's window (`geometry`, `present`, `release`). The app's callbacks
  never run under that lock. Frames are still consumed on the main Looper, at
  the position from the last update, and a released window is never presented.
- Window removal: `ViewRootImpl.dispatchDetachedFromWindow` reports the window
  detaching before its views detach, and `SurfaceView.onDetachedFromWindow`
  destroys its Surface (`releaseSurfaces`). `FrameworkWindows` therefore
  destroys the window's surfaces from `OnWindowAttachListener.onWindowDetached`,
  while `surfaceDestroyed` still sees a valid Surface. This applies to
  main-thread windows too.
- Window close: `FrameworkSurfaces.stop` follows
  `WindowManagerGlobal.setStoppedState`. Windows owned by other threads stop on
  their Looper with `Handler.runWithScissors(…, 0)`, through the handler
  recorded when the surface was created. If that thread has quit its Looper,
  its host surfaces are released without callbacks.

`FrameworkActivities.removeWindow` now ends like `handleDestroyActivity`.
After removing the decor, `WindowManagerGlobal.closeAll` closes the windows
the Activity leaked (on its decor's token, then on its own token). This logs
`WindowLeaked`, and each window dies on its owning thread. A destroyed stacked
Activity's open dialogs used to stay registered.

Main-thread windows keep the inline path. `test_managed_dialog_and_popup_windows`
adds an unaccelerated `$Engine` Activity whose `HandlerThread` does three things:
1. Shows an AlertDialog, resizes its button and clicks it. With the launcher
   before this change, the fixture exits with the same exception.
2. Shows a SurfaceView dialog and dismisses it.
3. Shows a SurfaceView dialog on the root Activity, which is still open when
   the app closes.

Every SurfaceHolder callback must run on `FixtureEngine`, with a valid
Surface in `surfaceDestroyed`. On the first windows-thread commit (no
foreign surfaces), the SurfaceView never gets a surface and the launch times
out.

Limits:
- An owner thread that never runs its Looper never draws its window, as on
  Android. Window close then waits for it, as `setStoppedState` does.
- Pixels presented to a SurfaceView in such a window are untested: the
  fixture checks only the surface lifecycle and the host surface.
- Real touch delivery to such a window is covered by code review only: the
  fixture clicks with `performClick` on the owner thread.

### microG Play services packages (2026-10-16)

Every managed app root carries microG's `com.google.android.gms` (GmsCore)
and `com.android.vending` (Play Store companion) as privileged system apps:
`/system/priv-app/GmsCore/GmsCore.apk` and `/system/priv-app/Phonesky/Phonesky.apk`,
with arm64 libraries extracted beside them (`extractNativeLibs="true"`).
`Tools/Android/android_microg.py` downloads and pins them; see
[third-party provenance](../THIRD_PARTY.md). They use UIDs 19998 and 19999,
which `android_sessions.app_id` never assigns to an installed app.

`ManagedLauncher.installMicroG` registers both as lazily loaded system
packages. An archive is parsed on the first lookup that needs it (about
1.8 s for GmsCore and 0.2 s for the companion in the JIT, far longer in the
interpreter); UID-to-name queries do not parse. Service child processes
receive the parent's already parsed snapshots in the BOOTSTRAP reply. `FrameworkPackages` reports the certificate in each
package's `fake-signature` meta-data, as microG's
[framework patch](https://github.com/microg/patches) does: the package must
target an API above 22, declare the meta-data and hold
`android.permission.FAKE_PACKAGE_SIGNATURE`, which the provisioned system
packages are granted. The spoofed certificate replaces `signatures` for
`GET_SIGNATURES` and `signingInfo` for `GET_SIGNING_CERTIFICATES`; the patch
itself only replaces `signatures`. `getPackagesForUid` and `getNameForUid`
now also resolve system packages.

Google's own `play-services-basement` 18.12.0 requires both packages with
Google certificates. Without microG it logs "requires the Google Play Store,
but it is missing" and returns `SERVICE_INVALID` (9), the status SpongeBob's
Firebase gate reported. With microG it returns `SUCCESS` with apk version
252432034.

### GPU-rendered Activity windows (2026-10-17)

Full-window WebView ads skipped about 3,500 frames a minute: each dirty frame,
the launcher's main thread drew the whole 2560x1384 view tree in software
(Chromium's software draw included), copied every pixel and scanned it for
alpha. Two changes address this:

- Software popups redraw only ViewRootImpl's accumulated `mDirty` region
  (ViewRootImpl never clears it without a valid Surface) and stop at the
  first visible pixel instead of counting all of them.
- Activity windows whose ViewRootImpl created a `ThreadedRenderer`
  (`FLAG_HARDWARE_ACCELERATED`) get an ImageReader-backed Surface bound to
  their own host layer. The compositor calls `ThreadedRenderer.initialize`,
  `setup` and `draw(view, attachInfo, viewRoot)` — ViewRootImpl's own
  hardware draw — when the tree is dirty. ViewRootImpl's own Surface stays
  invalid, so its traversal never initializes or destroys the renderer, and
  `destroySurface` only clears the renderer's SurfaceControl. Host layers
  restack (op 19, Wayland `place_below`/X11 `Below`) so each window's
  SurfaceViews sit directly below it, windows in stacking order.
- Both draw paths set `AttachInfo.mDrawingTime` (uptime milliseconds) before
  drawing, as `ViewRootImpl.draw` does. Legacy view animations read it through
  `drawChild`; left at zero, a fragment's enter fade stayed at its first frame
  (alpha 0), so a Navigation screen after the first (microG's settings) drew
  its toolbar but none of its content, and Back left the first screen blank.

HWUI needed three runtime fixes, each checked against Android 15 sources:
libhwui lists libGLESv1_CM before libGLESv2, so `glGetString` reached the
real GLES1 wrapper with no guest hooks (a null read at 0xdd0); a
libGLESv1_CM adapter now thunks the 58 GLES 1.0 commands shared with the
GLES table (from the Khronos registry). The other ES 1.x names are exported by
`Guest/Android/Adapters/gles1_oes.c`, because the real library's slot is empty and
Android binds all of an app's imports when its library loads (DevilutionX needed
`glBlendEquationOES` and then `glColor4f`). The framebuffer-object and blend
extensions (`GL_OES_framebuffer_object`, `GL_OES_blend_subtract`,
`GL_OES_blend_equation_separate`, `GL_OES_blend_func_separate`) forward to the ES 2
core functions, whose arguments and token values are identical (42 enums compared
in `gl.xml`). `GL_OES_draw_texture` and the 85 fixed-function commands of ES 1.x
(`glColor4f`, `glMatrixMode`, `glVertexPointer`, ...; stubs generated from the
registry at build time) have no ES 2 equivalent: they are exported so the library
loads, and the first call says so on stderr. Bifrost provides no ES 1 context, so an
app that draws through them gets nothing. With the adapter DevilutionX loads and
reaches its data-file prompt; its native game was not started (it needs the
original `DIABDAT.MPQ`), so no game has run these calls. `ManagedGeometry` does, through
`GLES11Ext` and `GLES10` against its ES 3 context: a framebuffer built and drawn into
through the OES names, a renderbuffer's size, the blend state they set, and
`glDrawTexfOES`/`glColor4f` returning with the message. A NativeActivity (non-managed) app
gets no `libGLESv1_CM.so` at all. HWUI's `DeviceInfo` aborts without
`ISurfaceComposer.getPhysicalDisplayIds`, `getStaticDisplayInfo`,
`getDynamicDisplayInfoFromId` and `getCompositionPreference`, now answered
from `FrameworkDisplay` (display id 1, one 60 Hz mode, sRGB). EglManager calls
the C++-linkage `eglBeginFrame` every frame; the EGL adapter now exports it.
`egl_set_cache_filename` and `egl_get_init_count` still resolve to the real
libEGL.

Limits: a window without a ThreadedRenderer (unaccelerated, or owned by another thread) and a toast remain software
layers above all surfaces, so such a dialog under a later Activity would cover it. Dialogs and popup menus whose
ViewRootImpl has a renderer draw on the GPU like Activity windows (2026-10-28: a software canvas throws "Software
rendering doesn't support hardware bitmaps" for the bitmaps an image loader hands out, which ended YouTube when it opened
a popup menu). A GPU window with `FLAG_DIM_BEHIND` keeps its dim in a display-sized layer created just before its own
layer, so the dim stacks directly below it (the software dim layer always stacks above every GPU layer and dimmed the
dialog itself); `APP_WINDOW_GPU` carries `dim=alpha` for such a window. Whether WebView
takes its GPU (draw functor) path in the game is not yet observed.
AHardwareBuffer import for TextureView/video is described under the EGL adapter.

Native fences: `eglDupNativeFenceFDANDROID` returns the driver's `sync_file` as a guest descriptor, and importing one through
`eglCreateSync[KHR]` gives the driver a host duplicate and consumes the guest fd (`AndroidNativeFences`, per
[EGL_ANDROID_native_fence_sync](https://registry.khronos.org/EGL/extensions/ANDROID/EGL_ANDROID_native_fence_sync.txt)). WebView calls
this without checking extensions, and its fences reach the buffer queue (`SYNC_IOC_MERGE`, `SYNC_IOC_FILE_INFO`, handled by `HostNode`).
The adapter does not advertise the extension. Buffers do not depend on these fences for correctness here, because a native-buffer image
is a host copy of the gralloc memory. With a driver lacking the extension the calls still fail and WebView logs it; that case is untested.

### microG processes, remote providers and service resolution (2026-10-17)

SpongeBob's `GoogleApiHandler` crashed on `activity.getContentProvider`:
`play-services-basement` asks microG's `com.google.android.gms.chimera`
provider (`serviceIntentCall`) for the Intent of a Play services action, and
the launcher had no remote provider path. microG now runs in its own managed
processes, under its own UIDs:

- `getContentProvider` follows Android 15's
  [ContentProviderHelper](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-15.0.0_r1/services/core/java/com/android/server/am/ContentProviderHelper.java):
  isolated callers are rejected, the authority resolves over every known
  package with `STOCK_PM_FLAGS | GET_URI_PERMISSION_PATTERNS`, then
  `checkContentProviderPermission` (owner app id, or exported with an absent
  or held read/write/path permission; no URI grants). `canRunHere` (same uid,
  multiprocess or same process) returns a local holder; anything else starts
  the provider's process and waits up to `CONTENT_PROVIDER_READY_TIMEOUT_MILLIS`
  for publication, returning null when the host dies or the wait times out.
  One connection per provider carries this client's stable/unstable counts
  (`refContentProvider`, `removeContentProvider`, `unstableProviderDied`).
  A child process whose providers have clients stays running until the session exits.
- A package's providers follow `ComponentResolver.addProvidersLocked`, applied where
  `FrameworkParseCache` reads a package: the first declaration of an authority owns it, a
  later one loses that name and, with none left, has a null authority and is not started.
  A class declared again is ignored (`FrameworkPackages.registeredProviders`). Firefox's
  merged manifest repeats `InitializationProvider`, keeping the initializers on the first.
- `updateServiceGroup` checks the connection exists and records nothing: groups only order
  isolated processes for a low-memory killer this runtime does not have.
- Every bound process installs its app's network security config first, as
  `handleBindApplication` does (`bindApplicationMetadata`). `systemMain()` leaves the `android`
  package's config as the process default; an app that differs from it on cleartext and has a
  `domain-config` would otherwise fail in `makeApplicationInner` with "Found multiple conflicting
  per-domain rules", and other apps would run without their own config applied to TLS.
- A service child is bound like `handleBindApplication`: it installs its
  process's providers, publishes them to the session, then runs
  `Application.onCreate`. Publication sends only provider class names and
  Binders; the session builds holders from its own manifest records (the
  child's `ProviderInfo` copies carried GmsCore's whole `ApplicationInfo`,
  1.4 MB, which the Binder driver rejected). Children cannot start processes,
  so their provider acquisitions and connection counts go through the session,
  which re-checks the permission for the child's uid.
- Implicit service Intents resolve through manifest intent filters
  (`ComputerEngine.queryIntentServicesInternal`: package restriction, priority
  order), as `ActiveServices.retrieveServiceLocked` does. Provider and service
  filters come from one archive parse per package.
- `queryIntentReceivers` (`PackageManager.queryBroadcastReceivers`) resolves an explicit component, or every
  enabled `<receiver>` whose filters match, restricted to the Intent's package, with the activity query's
  matching, `MATCH_DEFAULT_ONLY` and ordering (modelled on it, not read from `ComputerEngine`). Receiver
  filters share the one parse the broadcast path uses; only the installed package and the packages already
  loaded as system or peer packages are searched.
- A child hosting another package receives the session app as a peer
  package (`FrameworkPackages.installPeer`): microG checks its callers by uid,
  name, requested permissions and grants.
- System packages hold requested normal permissions and the signature
  permissions declared by a same-signer package (both microG APKs share
  certificate SHA-256 9bd06727…14165). Firebase's `c2dm.permission.SEND`
  check on GmsCore and the companion's read of microG's settings provider
  need this. Privileged allowlists and runtime grants are not modelled.

Limits: microG's cold start takes about 11 s in the JIT, so the first
`AdvertisingIdClient` request (10 s timeout) can still fail. Each child
bootstrap carries the parsed GmsCore snapshot (about 650 KB replies). microG
broadcasts and its `:ui` Activities, started from an app, are untested. Account sign-in is covered in
[Google accounts](#google-accounts-and-sign-in-2026-10-20).

### Google accounts and sign-in (2026-10-20)

Experimental: some apps and Google services may not work with it.

`account` is no longer a stub. `FrameworkAccounts` follows Android 15's
[AccountManagerService](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-15.0.0_r1/services/core/java/com/android/server/accounts/AccountManagerService.java)
for one user: authenticators are the services declaring
`android.accounts.AccountAuthenticator` (XML meta-data read through the owning
package), accounts are visible to callers by signature, `GET_ACCOUNTS`-style
permission and stored visibility (`resolveAccountVisibility`), and add,
remove, confirm, update, `getAuthToken` and the feature queries run a Session
that binds the authenticator and returns its `Bundle`. A `KEY_INTENT` result
must name an Activity of the authenticator's package (or one with its
signature). Several services run in each guest process instead of behind
Binder, so the caller is `Process.myUid()` even while the process serves an
authenticator transaction (an authenticator reading its own account data).

- **Store.** Accounts, tokens, user data, visibility and grants live in
  `/data/system/users/0/bifrost-accounts.json` (`FrameworkAccountStore`),
  shared by the session and microG's service processes: updates take a file
  lock and replace the file atomically; a process notices another's change
  within about 1.5 s and sends its own receivers `LOGIN_ACCOUNTS_CHANGED`.
  Replacing a runtime root keeps the file.
- **Authenticator binds** run with the system's identity
  (`FrameworkServiceRuntime.SYSTEM_BIND`: no export or permission check on the
  target service, which microG declares unexported) and the session app's uid
  holds `ACCOUNT_MANAGER` for the authenticator's own check
  (`FrameworkPackages.checkPermission`).
- **Sign-in window.** `android_package.py google sign-in` (also `settings`,
  `accounts`, `remove`; `asgard.py` joins the realm) starts the provisioned
  `com.google.android.gms` system app as an ordinary background session:
  `Scripts/Android/android_google.py` writes a synthetic `package.json`
  (`system: true`, launcher `org.microg.gms.auth.login.LoginActivity`) and
  `runApp` takes the lazily parsed system `PackageInfo` (UID 19998) instead of
  parsing an installed APK. There is no dexopt for it. microG's own UI then
  shows Google's embedded setup page in a WebView and stores the account
  through `AccountManager` as microG's authenticator.
- **One sign-in for every app.** `<state>/.google/` keeps a copy of the
  accounts file and microG's registration of the device (`checkin.xml`,
  `gservices.db`, account caches). `launch_managed` imports it into a root
  before each session and publishes the root's files after the session if they
  changed, so signing in (or out, `google remove`) in one session reaches the
  others at their next start. microG's settings file stays per root.
- Asgard's library window has a "Google account…" dialog: the account list,
  "Sign in with Google…", "microG settings…" and "Remove". The system app is
  not listed as a game.

Observed: the microG dialog and Google's real "Sign in" page rendered in the
window (screenshots), and a seeded `com.google` account was answered by
microG's authenticator from a separate process (`hasFeatures`).

Observed on 2026-10-21 (Hungry Shark World, a real Google account the user
signed in): the window returned `RESULT_OK` with `changed=true`; microG's Games
service then bound for the game, obtained a `games_lite` OAuth token from Google,
and Google answered `GET https://www.googleapis.com/games/v1/players/me` with
HTTP 404. microG replied `SIGN_IN_REQUIRED` and the game's Play Games plugin
reported "Authentication failed … 4 out of 4 underlying tasks failed". The 404
body was not logged, so the reason is not established (a Google account with no
Play Games profile is one candidate; microG has no profile creation). This is
Google's answer to a request the emulator delivered, not an emulator fault, and
it shows nothing yet about Play Integrity. After the window, `changed=true`
restarts the app's Play services processes (`FrameworkRemoteActivities`), which
kills the provider the game's clients were bound to: the ad-ID client logged
`DeadObjectException` once before it reconnected to the new process.

**Google Activities started by an app.** microG's `:ui` Activities
(`AuthSignInActivity`, `AccountPickerActivity`, `AskPermissionActivity`,
`LoginActivity`, …) run in their own window of the system app, through
`FrameworkRemoteActivities`:

- `FrameworkActivities.start` routes an explicit component of
  `com.google.android.gms`, or an implicit Intent whose action starts with
  `com.google.android.gms.` and resolves there (the parsed manifest filters;
  implicit resolution parses the whole package), instead of loading the class
  into the app. Other packages' Activities are still unresolved.
- The guest marshals the Intent and the app's `PackageInfo` into
  `<cache>/google-activity/<id>/` and asks the session supervisor
  (`ProcessHost`, operation `activity`) to run it. `android_google.ActivityRelay`
  stages the request in the system app's root
  (`data/system/users/0/bifrost-activity`), starts the `com.google.android.gms`
  session with that Activity as its root (`runApp`; the app is installed there as
  a peer package so label and signature lookups work, and the root reports the
  calling Activity to `getCallingActivity`, which `AuthSignInActivity` checks), and
  waits. When the window closes the session writes the Activity's result code
  and data; the relay returns them in `result.json` after importing the shared
  accounts into the app's root. The app delivers the result to `onActivityResult`
  and restarts its Play services processes when the accounts changed, because they
  cache the device registration and accounts.
  Activity completion is acknowledged through the authenticated process host's
  `activity_finished` operation after the result files are closed and the Activity
  is destroyed. The host publishes accounts and the result immediately, exactly
  once; process exit remains a fallback for crashes or older launchers. A remote
  UI session shuts down its remaining services after returning the result.
  Waiting for service idleness before forwarding the result left Global Uma
  paused behind a black microG window after cancellation, even though microG had
  saved `RESULT_CANCELED` and destroyed its Activity. The host regression covers
  cancellation and success while the service session remains running, plus
  duplicate completion after the app consumes the files. The 13 targeted Google
  account, HOME, browser-link and session-result host checks pass, and the managed
  launcher dex builds. The verification game launch reaches microG's LoginActivity;
  result delivery and game advancement in that run are not yet verified.
- The caller's `AccountAuthenticatorResponse` (add-account and token screens) is
  a Binder of the app and cannot cross: the session's Intent carries a stand-in
  whose `onResult`/`onError` is recorded and relayed to the real one afterwards;
  an Activity that ends without answering cancels it (`ERROR_CODE_CANCELED`).
  An Intent carrying other Binders or file descriptors cannot be marshalled and fails
  with `START_CANCELED`.
- One Google window at a time (the app lock of the system app): a request while
  the Asgard dialog's window, or another request, is open is answered cancelled
  with an error. A request costs a cold session start (about 15 s here before
  the window shows). Closing the window, or the app ending, cancels it.
  Asgard's library window shows which game asked while the window is open.

**Manifest broadcasts to another process.** A manifest receiver declared for
another process of the app (microG's `LoginActivity` sends its GCM group update
to `com.google.android.gms:persistent`) is scheduled on that process, which the
session starts: `FrameworkBroadcasts` passes `scheduleReceiver` to
`FrameworkProcessRuntime.receiver`, as a oneway call whose result is not awaited
(an ordered broadcast moves on at once). The Intent carries the receiver's
component, as `BroadcastQueue` sets it and `ActivityThread.handleReceiver`
reads it. A process that served a receiver stays cached; before this,
stopping a child with its last Service lost a receiver scheduled in that window.
Since 2026-10-24 a cached child stops once nothing has used it for 60 s (the
background broadcast timeout of Android 15's `BroadcastConstants`): no service
record, no provider reference, no receiver delivered in that time.
`FrameworkProcessRuntime.idleLater` re-checks after a receiver, after the last
service stops in a process kept for a provider or receiver, and when a
provider's last reference is released, and logs `ASGARD_PROCESS_IDLE_STOP`.
ActivityManager may kill such empty cached processes at any time; here each is
a whole emulated runtime with its own JIT. The rule has not yet been seen to
fire in a game: in Battle Cats the WebView service stayed alive while the game
ran, for a reason not yet traced. A child process still cannot send to another process.
Before this fix the broadcast threw `UnsupportedOperationException`, which
ended the first real Google login after Google had accepted it ("problem
connecting to Google services"; the account was stored anyway).

Observed: `test_managed_google_activity_window` starts `AuthSignInActivity`
explicitly and by action, and `AskPermissionActivity` with an authenticator
response, from a fixture app; each opens the system app's window and returns
(`RESULT_OK` with `errorCode=10` for a request without a configuration;
`RESULT_CANCELED` and a cancelled response for the permission screen). Not
observed: a real sign-in, `LoginActivity`'s own result and response relay, or
a Google Sign-In button in a real game.

Not modelled: other users, shared and restricted accounts, device-policy
limits, `startAddAccountSession` and its relatives, the credential-grant and
account-chooser Activities (a non-custom-token authenticator without a grant
answers `ERROR_CODE_UNSUPPORTED_OPERATION`, several matches for
`getAccountByTypeAndFeatures` the same), sign-in notifications and
account-access requests. microG's settings and every account-related file are
copied on the host, so two sessions signing in at once keep the last one to end.

#### Play Games sign-in answers 404 (2026-10-21)

Hungry Shark World and Umamusume complete Google's sign-in window, then fail the game's own
Play Games sign-in: microG's `performGamesSignIn` (GmsCore v0.3.17.252432,
`org.microg.gms.games.extensions.kt`) requests `https://www.googleapis.com/games/v1/players/me`,
Google answers **404**, and `GamesConnectService` reports `SIGN_IN_REQUIRED`. In microG a 404
means the account has no Play Games profile. It would create one (`registerForGames`: it asks
`gameswhitelisted.googleapis.com` for a gamer tag and saves a profile that is not visible or
discoverable) only when its setting `game_allow_create_player` is on, and that setting is off.
This is read from the log and microG's source; the 404 body was not captured, so "no
profile" is the reading microG's own code gives, not a proven fact about the account.

Creating the profile changes the user's real Google account, so Bifrost never turns it on
unasked. `asgard.py google games-profile on|off|status` records the choice once, behind a
notice that says what it does (`--yes` skips the prompt), under `<state>/.google`;
`android_google.apply_games_profile` writes it into each app root's
`com.google.android.gms_preferences.xml` before the session starts. Until a choice is made roots
keep their own preference. The file is guest-writable, so it is read without following
symlinks and a DOCTYPE is refused. Setting microG's switch through its own window only
reaches the Google root, because preferences stay per root. Not observed live: the user decides
whether to turn it on, then a sign-in retry shows whether the profile is created and the 404 ends.

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

### Bionic compatibility paths (2026-10-21)

Bootstrap, ART and framework provisioners create relative links from
`/system/lib64/{libc,libm,libdl}.so` to their canonical counterparts in
`/apex/com.android.runtime/lib64/bionic/`. This follows the
[Android compatibility contract](https://developer.android.com/about/versions/10/behavior-changes-all#bionic-libraries).
The libraries remain single canonical files. Existing ART/framework roots are
verified before upgrading their inventories; conflicting files, unexpected links
and redirected directories are rejected. A retry can complete an interrupted
link-only upgrade. The framework manifest digest then causes managed app roots
to refresh on their next start while retaining app data.

In the global Umamusume trace, its private loader omitted libc from its search
list without these paths. The `__errno` lookup returned zero and populated a null
call slot. A failed probe of `/system/lib/libhoudini.so` then attempted to store
ENOENT through `__errno()` and jumped to PC zero. Restoring only the three links
made libc discoverable, resolved `__errno`, populated the slot and removed that
crash. An uninstrumented run survived the 40-second observation window; later
startup and gameplay remain unverified. This trace establishes a runtime layout
defect; it does not establish deliberate protection rejection.

The focused `test_android_package.BionicCompatibilityTests` covers inventory
upgrade, idempotence, interrupted upgrade recovery and refusal of modified roots
or conflicting paths. The real `android_bionic_bootstrap.c` regression opens all
three legacy paths and checks that libc's resolved `__errno` uses the existing
thread's errno storage; it passed in JIT, interpreter and Tier-2 modes. These
checks do not cover game rendering, sign-in or server access.

### Android HOME and Asgard (2026-10-22)

Managed app sessions now route an implicit `ACTION_MAIN` + `CATEGORY_HOME`
Intent to Asgard. The authenticated session supervisor activates the launcher
for the same state directory (one launcher per state), then queues task
backgrounding on the app's main thread. Activities pause and stop without
being destroyed; the host window hides. Asgard's **Return to app** action, or
`asgard resume PACKAGE`, restores the window and restarts/resumes the existing
Activity stack. Services remain alive. This does not prevent an app from
terminating itself after requesting HOME. Root Back handling remains separate.

Managed roots contain the verified SDK image's `/system/bin/sh` and a native
`/system/bin/am` client. The client uses the same peer-UID/token authenticated
supervisor channel as managed services. It supports `start`/`start-activity`
with MAIN/HOME (optional DEFAULT), `--user 0`/`current`, and numeric `-f` flags;
unresolved intents, other users and unsupported options fail explicitly.
Cached framework inventories acquire the pinned shell after verification;
changing its bytes or mode fails verification. No host shell executes guest
commands. The desktop launcher requires PyQt6.

Tracing that shell found two prerequisite ABI bugs. Raw `TCGETS` copied libc's
60-byte `termios` over Bionic's 36-byte stack object, triggering the SDK shell's
stack protector. Terminal ioctls now marshal the 36-byte
[Linux kernel layout](https://github.com/torvalds/linux/blob/v6.12/include/uapi/asm-generic/termbits.h).
Android `execve` also jumped directly into the new executable without running
its interpreter. It now discards old device-mapping guards, loads Bionic's
confined PT_INTERP, preserves ELF map
identity, resets TLS, rebuilds vDSO/auxv and enters linker64. AT_EXECFN names the
executable path independently of argv[0], following
[Linux's ELF loader](https://github.com/torvalds/linux/blob/v6.12/fs/binfmt_elf.c).
This does not implement the still-unsupported shared-VM/vfork process contract:
Java `Runtime.exec` currently encounters ENOSYS on that path. Native fork/exec
and direct HOME Intents are the validated paths.

The real Global client was rerun with default JIT settings, a windowless plugin
and an offscreen Asgard launcher. The supervisor acknowledged HOME with status
0, and the shell/am child called `exit_group(0)`. The main game process then
reached `exit_group(127)` during Application startup, before an Activity could
poll the queued background request. Further tracing identifies that exit as
Bionic `abort()`'s fallback, not a game-selected exit status. `abort()` raises
SIGABRT twice through `rt_tgsigqueueinfo` (240); Bifrost's explicit ENOSYS stub
makes both attempts fail, after which Bionic calls `_exit(127)`. This matches
[Bionic's abort implementation](https://android.googlesource.com/platform/bionic/+/refs/heads/main/libc/bionic/abort.cpp)
and the pinned image's disassembly. A minimal program using the actual Bionic
DSO, with SIGABRT set to SIG_DFL to exclude debuggerd, terminates with SIGABRT
under QEMU but exits 127 under both Bifrost engines. The real Global client
also reaches the same HOME/abort path in the reference interpreter.

The captured constructor is `lib__4e06__.so + 0x1d54`. Its decoded code calls
a cleanup dispatcher, requests HOME, restores a saved register/stack context
with `abort` as the return target, and then reaches the fallback above. The
24-byte records passed to cleanup describe mapped modules: identifiers such as
`0xe2` are not established failure codes. The first captured initialization
failure was module 2: its final data-directory subcheck looked up `ANDROID_DATA`
in `/proc/self/environ`. Procfs returned only a hardcoded 19-byte
`PATH=/bin:/usr/bin` block, hiding the configured Android environment. The
preceding `/proc/self/exe` ELF-header check passed.

Procfs now reads the guest's original environment string block, with NUL
separators and numeric-PID aliases, independently of libc's later `setenv`
array. This follows [Linux's initial-environment contract](https://man7.org/linux/man-pages/man5/proc_pid_environ.5.html),
including direct edits to that original memory. The focused fixture passes
on native Linux and in Bifrost JIT, interpreter, Tier-2 and the headless build.
QEMU agrees on initial contents and `setenv`, but does not mirror direct edits
to the guest's original environment or argv through its procfs view. A new
windowless Global trace confirms module 2 now passes; a later module `0x60`
initializer returns false and startup still aborts.

The next narrow trace localizes module `0x60` to its emulator-property check
at module offset `0x3d90`. It queries `ro.hardware`, compares against
`goldfish` and `ranchu`, then queries `ro.kernel.qemu` (call at `+0x3efc`).
The latter returns `"1"`; comparison against `"0"` at `+0x3f9c` fails, reports
an event and returns false to the initializer. The provisioned image's
`/vendor/build.prop` contains `ro.kernel.qemu=1` (line 80), so the property
service is accurately reporting the image. These strings, comparison values,
and failure branch are observed in the captured code and block trace, rather
than inferred from the final exit code. This establishes an emulator-check
rejection in the native loader; it does not establish the vendor branding of
that code or prove that other startup checks would pass.

A subsequent user-requested, app-local experiment adds `ro.kernel.qemu=0` to
Global's managed root `debug_ramdisk/adb_debug.prop`, which the property loader
already reads after the vendor file. The shared image and production property
loader are unchanged. A default-JIT windowless run still exits 127. Narrow
tracing confirms the property now supplies `"0"`, the comparison succeeds, and
the entire module `0x60` initializer returns 1 (observed at offsets `+0x9c00`
and `+0x9c9c`). Execution subsequently reaches another decoded loader, whose
initial setup returns 0 at its `+0x3174`, before the abort fallback. That later
failure was subsequently localized as described below. The first broad block trace with the
override timed out from logging overhead; the narrowed run reached the abort.
The override lives only in that generated app root and can be lost when the
root is rebuilt; its original file was backed up outside the repository.
This experiment demonstrates progress past the emulator-property check,
not successful Application/Activity startup or gameplay.

A watchpoint on the first recorded error traced that later failure back to
module `0x60`, even though its initializer returned 1: it defers an error and
copies it to the loader's shared record afterward. The failing check compares
`/proc/filesystems`' directory-entry inode with `newfstatat` at module
`+0x78ec` / `+0x7900`. The expected value `0xbe15bdbd357f6e4f` exactly matches
Bifrost's directory-entry path hash; path stat instead returned the host proc
inode `0xf00000f1`. Independent open-file stats exposed yet another identity,
the backing memfd's inode. A focused probe agrees between native Linux and
QEMU on this file and reproduces the mismatch in Bifrost.

Synthetic procfs nodes now present the directory-entry identity through path
stat, descriptor stat, relative directory-fd queries, AT_EMPTY_PATH and statx.
Real executable identity through `/proc/self/exe` stays on its existing path.
The expanded procfs regression passes on native Linux and in all three
Bifrost engine modes and the headless build. The relevant contract is
[stat/fstat/fstatat referring to the same file](https://man7.org/linux/man-pages/man2/stat.2.html).
This is a general filesystem fix, with no game-specific checks or return values.

The real windowless Global run now passes the formerly failing loader setup,
then reaches a forked self-debugging stage. Lightweight syscall logging (no
GDB attachment) observes a child repeatedly issuing syscall 117 with request
16 (`PTRACE_ATTACH`) targeting the main app PID, then `exit_group(45)`. A main
process watchdog thread subsequently issues `exit_group(0)`. Bifrost's
`Scheduling.cpp` currently returns EPERM for every ptrace request; Linux
[attach semantics](https://man7.org/linux/man-pages/man2/ptrace.2.html) include
tracer ownership, stops and wait notifications, not just a success return.
Other fork descendants also repeatedly fail to open a related guest PID's
`/proc/<pid>/status` with ENOENT. These are the next observed compatibility
gaps; the trace does not prove that implementing them would complete startup.
The earlier abort/127 has been passed, but exit 0 is not Application startup
or playability. A GDB run timed out after fork, so its incomplete exit capture
was superseded by the lightweight trace.

Fixing syscall 240 would correct SIGABRT delivery, but is not evidence that
the game would start. HOME routing succeeds; gameplay remains unconfirmed.

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

A call no stand-in answers is named in the session log, once per process and call, as
`SERVICE_CALL_UNSUPPORTED <service> <call>`: for a Java service the method that threw
`UnsupportedOperationException` (`FrameworkServices.unsupported`, also used by
`ManagedLauncher.unsupported`), for a native Binder stand-in the descriptor and transaction number
`FrameworkServices.unknownTransaction` saw `Binder.onTransact` refuse (Binder's own interface, dump and shell codes
are not reported). The app still gets the exception or the unknown-transaction status; the line only makes a missing
method visible without opt-in APK tracing. The set holds at most 256 entries. `ManagedServiceBatch.nativeServices`
checks one of each kind and that repeats and Binder's own codes stay silent.

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
did not implement AudioFlinger track creation or AudioTrack PCM playback at that
point (it does now, see [AudioTrack playback](#audiotrack-playback-2026-10-21));
AAudio and OpenSL ES play through the host audio adapter.

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

Run without `--managed-graphics` and with `--probe-arg=no-audio-adapter` the same batch checks the
case where the process has Android's own `libaaudio.so` instead of the Bifrost adapter: the AudioTrack
must fail to initialize, with `AUDIOTRACK_REFUSED -19 createTrack re-entered ...` in the log.

The default transport is the shared broker. `--binder-mode local` isolates the
same native Binder contracts in one process; it does not validate cross-process
transport. The focused probe uses real NotificationManager, AudioManager,
AudioFocusRequest, AudioTrack queries, PowerManager and WifiManager. Token
death in this probe is simulated; transport death is tested separately by the
Binder suite. A bounded SpongeBob run also calls Unity's
`registerNotificationChannel` successfully through the production service.
Neither these tests nor presented frames establish video-ad playback: JNI
provisioning is covered below; decoders and the video-surface path remain
unfinished.

### The realm's service root (2026-11-04)

Each realm's media services run from a private 750 MB copy of the framework. It lives in `~/.cache/bifrost/media-roots/`, not beside
the socket: only the socket needs a short `/tmp` path, and `/tmp` is often RAM. `realm stop` and a private broker's close remove it,
and each start sweeps roots whose broker no longer answers, for one that was killed. If the cache cannot be written, it falls back
beside the socket. Roots left in `/tmp/asgard-<uid>/` by older versions are not swept.

### RenderScript and audio effects (2026-11-04)

`android.renderscript` runs on its CPU driver: `librs_jni`, `libRS_internal`, `libRSDriver`, `libRSCpuRef`, `libRSCacheDir` and
their libraries are pinned from the image, and the launcher sets the cache directory as `ActivityThread.setupGraphicsSupport`
does. The `Managed` fixture runs `ScriptIntrinsicBlur` and checks its pixels. Intrinsics are precompiled; a `ScriptC` kernel needs
`bcc`, which is not provisioned, so script kernels are expected to fail.

`android.media.audiofx` loads (`libaudioeffect_jni`), but nothing processes effects. `queryNumberEffects` reports 0, so
`AudioEffect.queryEffects` is null, as the JNI makes it on any device without effects, and `createEffect` stays unanswered, so `new Equalizer` throws `RuntimeException` ("Cannot
initialize effect engine"), which apps guard against. Implementing effects would need DSP in front of the host's audio.

### Time zone (2026-11-04)

Bionic's `tzset` and libcore's default zone both read `persist.sys.timezone`, and an image leaves it to the device.
`load_android_properties` fills it on each start from the host (`Properties/HostTimeZone.cpp`), so a host that changes zone is
followed without a reinstall, and a saved `property.persist.sys.timezone` wins. It takes the first of: a `TZ` naming a zone (with
`:` or as a zoneinfo path; a POSIX rule is skipped, and an empty `TZ` means UTC, as to libc), the `/etc/localtime` link, including
through NixOS's store, `/etc/timezone`, `/etc/sysconfig/clock` (`ZONE=` or `TIMEZONE=`), and for a copied `/etc/localtime`
(Alpine, containers) the identical file under `/usr/share/zoneinfo`. With none of these, Android's GMT stands. `TZ` itself is not
forwarded: no device sets it, and Bionic reads `:Asia/Tokyo` as UTC.

The property area's serial must not be 0: `CachedProperty` starts there and only looks a name up once it differs, so at 0 every
cached property in Bionic read as empty. `make test-host-timezone` covers each layout and the serial. A zone missing from the
image's tzdata reads as GMT; this is not checked at start.

### MediaPlayer and SoundPool (2026-11-04)

The broker runs the image's real `/system/bin/mediaserver` as UID 1013 in the slot that ran `bifrost-media-resource`
(`CodecServices`), so it provides `media.player` as well as `media.resource_manager`. `MediaMetadataRetriever` and
`MediaPlayer` reach the real `MediaPlayerService`, NuPlayer and the Codec2 stack. Its binary and four libraries are pinned in
`MEDIA_EXTRACTOR` (`android_framework.py`) and added in place, as is `libsoundpool.so`, which `android.media.SoundPool` loads by
name. Checked: FNAF Mobile Port (libGDX, Catrobat) runs with picture and sound, by eye. `make test-android-goldfish-media` and
the service batch pass. Not established: other apps' `MediaPlayer` playback, or that an Arknights hang in Unity start-up,
seen once on 2026-11-04 (1 of 34 sessions, the first after this change) and not seen again, is unrelated.

### Host media decoding (2026-10-14)

Apps use Android's real MediaCodec/Codec2 stack. Compressed packets are
decoded on the host; there are no Java or JNI codec substitutes.
`media.player` comes from the image's real `mediaserver` (see below); `media.extractor` is the image's real daemon.

- **Provisioning.** Framework schema 8 (`Tools/Android/android_codecs.py`)
  extracts the SDK image's real media.swcodec APEX, Goldfish Codec2 vendor
  modules, ResourceManagerService and their native closures. The codec
  daemon has its own linker section; the app namespace excludes those
  private libraries. `Guest/Android/Launchers/codec2_store.{c,S}` registers
  the genuine Goldfish store as `IComponentStore/default`; the stock
  software store stays `/software`.
- **Realm ownership.** The Binder broker owns the services
  (`Source/System/Android/Media/CodecServices.cpp`), independently of any
  app: `media.resource_manager` as UID 1013, `media.extractor` as UID 1040 and
  both component stores as UID 1046, from a cloned root with their own logs.
  Realm readiness does not wait for the extractor, whose failure costs only video. A service counts as
  ready only when the registered endpoint belongs to its spawned PID. A
  child that does not register within 30 s is killed. Restarts back off
  (250 ms × count) and give up after four consecutive failures. Broker
  `STOP` reaps every child before acknowledging. Children get
  `PR_SET_PDEATHSIG(SIGKILL)` and are spawned through `/proc/self/exe`.
  `Scripts/Android/asgard.py` (`ensure_media`) attaches a verified root;
  realm status reports it under `media`. A realm broker started from an
  older emulator rejects the media status op. Realm status then reports
  it as `incompatible`, and launches stop before any app starts. Stop its
  apps, then run `realm stop` and `realm start`.
- **Capability report.** `bifrost-emu --media-capabilities` decodes a 64x64
  black probe for VP8/VP9/AVC/HEVC on the native CPU and forced GPU paths
  (VA-API, CUDA or Vulkan when FFmpeg offers them). It checks every pixel,
  PTS 12345 and exactly one frame plus EOS. Scope is `host-decoder` only,
  not service or Surface readiness. The routing mask (VP8=1, VP9=2, AVC=4,
  HEVC=8) enables only probed codecs in `media_codecs.xml`.
  `BIFROST_MEDIA_BACKEND=auto|cpu|gpu` selects the decoder (`gpu` forbids a
  CPU fallback). `BIFROST_MEDIA_TRACE=1` logs transport inputs and images.
  Build with `USE_MEDIA=1` (default; needs FFmpeg's libavcodec/libavutil/
  libswscale). `USE_MEDIA=0`, or missing libraries, reports
  `FFmpeg support not built` and no codecs.
- **Goldfish transport.** `/dev/goldfish_address_space` is media-only
  (`GoldfishMedia.cpp`): a sealed memfd arena, snapshotted parameters and a
  bounds check on every offset. Decoder contexts are keyed by
  (codec family, guest ID) because the VP8 and VP9 modules both start at
  ID 1. Host color-buffer IDs are rejected; frames go to real gralloc
  buffers. `user_priv` is an opaque guest work address, returned verbatim
  and never dereferenced.
- **VPX output contract.** The pinned `C2GoldfishVpxDec` matches
  [android-15.0.0_r36](https://android.googlesource.com/device/generic/goldfish-opengl/+/refs/tags/android-15.0.0_r36/system/codecs/c2/decoders/vpxdec/C2GoldfishVpxDec.cpp),
  checked against the disassembly of the provisioned
  `libcodec2_goldfish_vp8dec.so`, not r1. It reads each shown frame right
  after its decode call and then releases that work. So VP8/VP9 use FFmpeg
  slice threading only; frame threading delays output one frame, and the
  late frame then carries a released work's `user_priv`. Like the
  emulator's
  [`MediaVpxDecoderGeneric::getImage`](https://android.googlesource.com/platform/external/qemu/+/refs/heads/emu-31-stable-release/android/android-emu/android/emulation/MediaVpxDecoderGeneric.cpp),
  the host writes a packed frame at the decoded size and ignores the
  guest's stale output size. The plane order is YV12 (Y, V, U), because
  that is what this binary copies into its YV12 block.
- **Surface output.** `FrameworkSurfaces` presents decoded frames through a
  genuine YUV `ImageReader` BufferQueue and `Surface.transferFrom`. The
  native adapter returns real `ANativeWindow`s. `bifrost_surface_prepare`
  binds a managed window to a separate host surface below the framework
  windows; the GraphicThunk EGL path resolves that alias to its host
  backing. SDR YUV is converted to ARGB on the Java UI thread (matrix and
  range from the dataspace, crop, stride). HDR is reported unsupported.
  Binding seeds the host surface with the BufferQueue's creation dimensions;
  subsequent SurfaceView geometry controls its size. For an aliased native
  window, `ANativeWindow_getWidth/Height` report that host surface size,
  matching EGL after a resize instead of the ImageReader's fixed creation
  size. The user confirmed clean game-window resizing on 2026-10-14;
  automated backend checks cover dimension seeding, validation and geometry
  updates. Full recreation and callback sequences remain unverified.
  `FrameworkSurfaces` owns each SurfaceView's lifecycle: it detaches
  SurfaceView's pre-draw listener and keeps `mHaveFrame` false. Otherwise
  [`SurfaceView.updateSurface`](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-15.0.0_r1/core/java/android/view/SurfaceView.java)
  treats the host-drawn window (no valid `ViewRootImpl` surface) as lost.
  Once the SurfaceView's own Surface is valid, that path calls
  `surfaceDestroyed` and destroys the installed Surface. Unity games then
  stopped before their first frame ("Timeout (2000 ms) while trying
  detaching primary window").
  On Wayland each managed surface is a `wl_subsurface` of the SDL window,
  [the protocol's model](https://gitlab.freedesktop.org/wayland/wayland/-/raw/main/protocol/wayland.xml)
  for video inside a decorated window. A subsurface is shown only while its
  parent is mapped, and an xdg toplevel maps only once a buffer is committed
  to its own surface. SDL attaches none, and EGL renders into the
  subsurface, so `create_surface` gives the SDL window an opaque black shm
  backdrop at its pixel size (`WaylandBackdrop`, refreshed on resize).
  Verified with `WAYLAND_DEBUG=client`: the toplevel received the backdrop
  buffer and `wl_surface.enter`; Unity's frames reached its subsurface; the
  window was visible on KDE Plasma. X11 needs no backdrop because it maps
  child windows without content.

Verified (temporary realm probes, not regression gates):
- Java MediaCodec under UIDs 10000/10001 sharing one realm: AVC, HEVC, VP8
  and VP9 each decoded five 64x64 frames with ffmpeg-reference MD5s, PTS
  0–800000 µs and EOS. Service PIDs were unchanged after the apps exited.
- `releaseOutputBuffer(render=true)` into a real ImageReader Surface, and
  NDK `AMediaCodec` to `ANativeWindow` with forced GPU, each produced exact
  frames and presented pixels to an X11 host layer.
- An X11 readback checked pixel placement, geometry and stacking.

Not yet established:
- Full SurfaceView callback and recreation sequences, and automated game resize coverage.
- Wayland readback.
- Native EGL rendering into an aliased Surface.
- Flush/reconfigure, B-frames, adaptive or HDR profiles, and resolutions
  beyond the 64x64 fixtures.
- Gameplay or video-ad playback.

On this host VA-API does not decode VP8, so forced GPU mode routes VP8 to
the software store.

### Common device services (2026-10-13)

Android 15's
[SystemServiceRegistry](https://android.googlesource.com/platform/frameworks/base/+/android-15.0.0_r1/core/java/android/app/SystemServiceRegistry.java)
builds many managers with `ServiceManager.getServiceOrThrow`, or looks the
Binder up in the manager constructor. Without the service,
`getSystemService` logs `ServiceNotFoundException` and returns null; Hungry
Shark Evolution then dereferenced a null `UiModeManager`, and session logs held
40 null `PermissionManager` dereferences from `PackageManager.checkPermission`.
These services let the real managers construct. Each declares its
contract enum; other methods throw. Semantics follow the linked Android 15
server classes, for one app, user 0 and the default device:

| Service | Model |
|---|---|
| `uimode` | Normal UI mode, night mode off and locked, contrast 0 (`config_defaultNightMode`, `config_lockDayNightMode`). Process, Activity and bind configurations carry `uiMode` 0x11. Per-app dark mode throws: it needs a configuration change. |
| `permissionmgr`, `permission_checker` | Delegate to the install-time grants (normal permissions granted, runtime permissions denied), retained framework-res/app permission definitions, no user rationale, no policy revocation, Android 15's 24 `platform.xml` split permissions, attribution registration. Proxy chains through other uids are hard-denied. |
| `batteryproperties`, `batterystats` | The host's own battery, read from `/sys/class/power_supply` through the VFS (see Battery below). A host without a battery is a full one on AC mains: present, 100%, `FULL`, `GOOD`, plugged AC, 4200 mV, 25.0 C, Li-ion. `registerReceiver` returns the sticky `ACTION_BATTERY_CHANGED` intent. `takeUidSnapshot(s)` gives the uid's health stat layout with no measurements. |
| `media_metrics` | `MediaMetricsManagerService`: playback, recording, transcoding, editing and bundle session ids are random 96-bit URL-safe Base64 strings; reports and `releaseSessionId` are accepted and dropped (statsd does not run here). The native `media.metrics` Binder is separate (`FrameworkNativeServices`). |
| `locale` | `LocaleManagerService` for one app: an app reads its own app-specific locales (empty), `getSystemLocales` is the system configuration's, another package needs `READ_APP_SPECIFIC_LOCALES`/`CHANGE_CONFIGURATION`, an unknown package is `IllegalArgumentException`, an override `LocaleConfig` is kept per package. Setting a different app locale would change the app's configuration and is `UnsupportedOperationException`, not a silent success. |
| `legacy_permission` | `LegacyPermissionManagerService`: `checkDeviceIdentifierAccess` and `checkPhoneNumberAccess` follow the service (privileged phone state, the identifier/phone op modes, `READ_PHONE_NUMBERS`, `READ_SMS`; a caller may only ask about its own uid and pid), so an app is denied device identifiers; the default-permission grants are system or root only. |
| `auth` | `AuthService` with no biometric sensors and no screen lock: `canAuthenticate` is `BIOMETRIC_ERROR_NO_HARDWARE` for biometrics alone and `NONE_ENROLLED` when the device credential is allowed (`PreAuthInfo.getInternalStatus`), an invalid authenticator combination is `SecurityException`, `authenticate` reports the same through the receiver with an increasing request id (no prompt), labels for the credential come from the framework strings, the system-only calls need `USE_BIOMETRIC_INTERNAL` and then end as unsupported. Without `USE_BIOMETRIC` the app gets the service's `SecurityException`; the permitted result was not run in the batch. |
| `usagestats` | `UsageStatsService.BinderService` for one app: every known package is in the ACTIVE standby bucket (`isAppInactive` false, no minimum bucket), queries without `PACKAGE_USAGE_STATS` return null/empty as the service does, history is empty, `reportUsageStart/Stop` keep the service's per-activity token rules, and calls that change system tracking (`setAppInactive`, `setAppStandbyBucket(s)`, observers, `reportUserInteraction`, chooser selections) need the system permission an app never holds (`SecurityException`; `UnsupportedOperationException` for a holder). |
| `netstats` | `NetworkStatsService` and its sessions for one app: argument and permission checks as the service has them (package must match the uid, wifi network keys need fine location, another uid's tagged history is refused, network-stack calls are `SecurityException`), usage callbacks register with the 2 MiB minimum threshold and never fire. There is no per-UID traffic accounting: summaries, histories and snapshots are empty, `getMobileIfaces` is empty, and the kernel counters (`getUidStats`, `getIfaceStats`, `getTotalStats`) answer `TrafficStats.UNSUPPORTED`. |
| `vibrator_manager` | No vibrators; `VIBRATE` is enforced and accepted vibrations are ignored. |
| `trust` and `window` keyguard calls | No credential and never locked; `DISABLE_KEYGUARD` gates keyguard locks, `exitKeyguardSecurely` succeeds. |
| `clipboard` | In-process clipboard with copied `ClipData`, one-hour auto-clear and listeners; reads need the play window's focus. Not shared with the host desktop. |
| `alarm` | Listener and PendingIntent alarms on a scheduler thread, at least 5 s ahead (`MIN_FUTURITY`), RTC converted to elapsed time. PendingIntent repeats clamp to at least 60 s; `remove(operation)` cancels them. Exact PendingIntent alarms enforce the target-SDK permission policy; exact listener alarms remain exempt. Alarms never restart a stopped process; AlarmClockInfo scheduling is unsupported. |
| `account` | Android 15's `AccountManagerService` for one user over a shared JSON store; authenticators are bound and run as the system. See [Google accounts](#google-accounts-and-sign-in-2026-10-20). |
| `media_session` | The sessions an app creates for itself (`FrameworkMediaSessions`, modelled on `MediaSessionRecord`, android-15.0.0_r1): `createSession` returns an `ISession` and its `ISessionController`; the session keeps the metadata, playback state (a playing position moves on with the clock), queue, queue title, extras, rating type, volume (local follows the system music stream, remote is the app's) and flags the app publishes, and tells the registered controller callbacks in order on one thread; a controller's transport commands, `sendCommand` and media buttons reach the app's `ISessionCallback`; `destroySession` tells the controllers and a later `registerCallback` is told at once. Limits: 100 sessions per uid (`SESSION_CREATION_LIMIT_PER_UID`, `Created too many sessions`), the global-priority flag is refused, a media button receiver that starts an activity is ignored. Not modelled: other packages' sessions, session listeners, media key routing (`dispatchMediaKeyEvent` still reaches nobody), local volume commands from a controller, the content-URI grant check on metadata art, `getSessions`. |
| `location` | Only the passive provider, location setting on; location data needs a runtime location grant and has no source. |
| `appwidget` | `FEATURE_APP_WIDGETS`, as on phones (`AppWidgetManager.getInstance` is null without the service). No home screen binds widgets: `getAppWidgetIds` checks that the provider is the caller's own package and returns none. Provider queries (`getInstalledProvidersForProfile`) are unsupported (`FrameworkAppWidgets`). `isRequestPinAppWidgetSupported` is false: no launcher confirms pin requests. `notifyProviderInheritance` (what the framework calls when an app's process starts, as `AppWidgetServiceImpl` in android-15.0.0_r1): a null array or entry ends the call, each package must be the caller's own, and the provider lookup finds none, so nothing is recorded. |
| `role` | `RoleService` for a device that defines no roles (`FrameworkRoles`): no permission controller is provisioned to grant one, so `isRoleAvailable` and `isRoleHeld` are false, with its caller-package, cross-user and empty-name checks. `RoleManager` is null without the service. Other methods are unsupported. |
| `shortcut` | `ShortcutService` with no launcher (`FrameworkShortcuts`): nothing is stored, so reads are empty and removals validate their arguments and find nothing; `isRequestPinItemSupported` is false. `ShortcutManager` is null without the service. Publishing a shortcut is unsupported. |
| `notification` zen mode | `getZenMode` is 0 (Do Not Disturb off, `Settings.Global.ZEN_MODE_OFF`), so `NotificationManager.getCurrentInterruptionFilter` is `INTERRUPTION_FILTER_ALL`; it takes no package and needs no permission. Policy and rule changes stay unsupported. |
| `activity` running services | `getServices` (`ActivityManager.getRunningServices`) reports the app's own created services, started or only bound, with pid, process, `FLAG_STARTED`, client count and activity times, up to `maxNum`: what an app without `GET_TASKS` sees (`ActiveServices.getRunningServiceInfoLocked`). `FLAG_FOREGROUND` and `foreground` follow `setServiceForeground` (see Foreground services). Only the main process is answered; a call from a child process is not routed. |
| camera | No camera HAL or cameraserver: `config.disable_cameraservice=true` (a default the image can override), so libcamera2ndk and `CameraManager` skip `media.camera` and list no cameras instead of polling for it forever. No `android.hardware.camera*` feature is declared. |
| `servicediscovery` | `NsdManager` (`FrameworkNsd`) on a network with no mDNS responder: `connect` returns the connector, `discoverServices`/`stopDiscovery` and `stopResolution` report started/stopped, no service is ever found, and `resolveService`, `registerService` and `unregisterService` fail with `FAILURE_INTERNAL_ERROR`. Uptodown dereferenced the null manager in `onActivityCreated`. Checked in `ManagedServiceBatch` (JIT only); the NSD classes are relocated by mainline, so types come from reflection. |
| `telephony.registry` | No modem (`getActiveModemCount()` 0): `listenWithEventList` records or removes listeners and never notifies. It keeps TelephonyRegistry's package check, `checkListenerPermission` (location, phone-state, precise, emergency and privileged event sets) and 50-listener limit (`FrameworkTelephony`). |

`notification` accepts `enqueueTextToast`, `enqueueToast` (both return true
for the caller's own package; a foreign package is a `SecurityException`) and
`cancelToast`, per Android 15's `INotificationManager.aidl`. `FrameworkToasts`
queues them as `NotificationManagerService` does: one at a time, 2 s
(`LENGTH_SHORT`) or 3.5 s (`LENGTH_LONG`), an equal token updates in place,
`cancelToast` takes the shown one down. A custom toast gets
`ITransientNotification.show(windowToken)` and `hide()` and renders itself. A
text toast has no SystemUI, so the process adds a `TYPE_TOAST` window of the
framework's `transient_notification` layout (bottom centre, not focusable or
touchable) and calls `onToastShown`/`onToastHidden`; if the layout cannot
inflate it logs `APP_TOAST_FAILED` and the app continues. Not modelled:
background-toast blocking, per-package limits, toast animations are only set
if the style exists, and the 400 ms animation allowance. Checked in the
`ManagedDialog` fixture (a `type=2005` window shown, then removed; JIT only).
`ManagedServiceBatch` only checks that `Toast.show` and `cancel` return.

Any `android.settings.*` intent without a component is declined with
`START_ABORTED` (`APP_ACTIVITY_SETTINGS_UNAVAILABLE`), not only for Play
services: a device always has a Settings app, so apps such as Uptodown's
all-files-access request do not catch `ActivityNotFoundException`. No Settings
screen exists, so the app is not granted anything. Not run live (it needs a
click on the app's dialog).

`notification` also accepts `cancelAll`, `cancel` and `getActiveNotifications`
for its own package: nothing is posted, so they find nothing. Clipboard and
alarm checks construct a focused clipboard and wait for a real listener alarm.
`ManagedServiceBatch` exercises these through the real managers; see
[the test record](../archive/TESTS-history.md#common-device-services-2026-10-13).

### Android Keystore (2026-11-02)

Zygote installs the `AndroidKeyStore` JCA provider
(`ZygoteInit.warmUpJcaProviders` → `AndroidKeyStoreProvider.install()`); apps
here start through `RuntimeInit` without Zygote, so `KeyStore.getInstance("AndroidKeyStore")`
failed with `AndroidKeyStore not found`. PolyTrack's Capacitor SQLite plugin
encrypts by default: its `load()` builds an androidx `MasterKey`, failed, and
left the plugin null, so every database call the game made was rejected.

`FrameworkKeystore` now installs the provider in every managed process and
registers `android.system.keystore2.IKeystoreService/default` (Android 15's
AIDL version 4) with a software KeyMint behind it, so the framework's own
client classes (`AndroidKeyStoreSpi`, `KeyStore2`, `KeyStoreSecurityLevel`)
run unchanged:

- Keys: AES-128/192/256 (ECB, CBC, CTR, GCM; no padding or PKCS#7) and HMAC
  (MD5, SHA-1, SHA-2; 64–1024-bit keys), generated or imported as raw
  material. Ciphers and MACs run in Conscrypt (`AndroidOpenSSL`).
- Generation and `begin` checks, and their error codes, follow the reference
  KeyMint TA
  ([system/keymint android15-release](https://android.googlesource.com/platform/system/keymint/+/refs/heads/android15-release/common/src/tag.rs),
  `check_*_gen_params`, `check_begin_params`, `crypto/aes.rs`): purpose,
  block mode, padding and digest must be authorized; GCM tags are 96–128 bits
  and not below `MIN_MAC_LENGTH`; caller nonces need `CALLER_NONCE`; GCM
  nonces are 12 bytes. A bad GCM tag or HMAC is `VERIFICATION_FAILED`.
  Characteristics are split as `tag/info.rs` does: KeyMint-enforced at
  `SecurityLevel.SOFTWARE`, keystore-enforced (validity dates, creation time,
  `USAGE_COUNT_LIMIT`) at `KEYSTORE`, application id/data hidden and required
  to match at `begin`.
- Keystore2 rules: the app domain only (`SELINUX`/`BLOB` are
  `PERMISSION_DENIED`, grants are `KEY_NOT_FOUND`), a new key replaces one
  under the same alias, operations address keys by `KEY_ID`, aliases list in
  order, validity dates are enforced, a `USAGE_COUNT_LIMIT` key is deleted
  after its last use, update input is limited to 32 KiB, and an operation
  ends after any error. Only `TRUSTED_ENVIRONMENT` is served; StrongBox is
  `HARDWARE_TYPE_UNAVAILABLE`.
- Storage: one JSON file per uid, `/data/misc/keystore/bifrost-<uid>.json`
  (where keystore2 keeps its database), written atomically under a
  cross-process lock, because each guest process runs its own instance.
  The managed root keeps the directory across runtime replacements, and
  `clear-data` removes the app's file, as `pm clear` deletes its keys.

Limits: key material sits unencrypted in the app's runtime root, and
`KeyInfo.isInsideSecureHardware()` is false. RSA, EC and 3DES keys,
attestation, grants, `updateSubcomponent`, wrapped-key import and keys bound
to user authentication (no lock-screen credential or biometrics exist, so
`begin` is `KEY_USER_NOT_AUTHENTICATED`) are not implemented. An unsupported
algorithm is `UNSUPPORTED_ALGORITHM`, and other unsupported calls are
`SYSTEM_ERROR`. Both are logged once as `SERVICE_CALL_UNSUPPORTED keystore2 ...`.
`OS_VERSION`/patch-level characteristics are not added.

`ManagedServiceBatch` checks the real client: AES-GCM generation and a round
trip with associated data, a tampered tag (`AEADBadTagException`), `KeyInfo`,
an encrypt-only key refusing decryption (`InvalidKeyException`), imported
AES-CBC and HMAC-SHA256 keys against Conscrypt's results, aliases and size
read back by a fresh provider instance, and deletion (`MANAGED_KEYSTORE_OK`,
JIT and interpreter). PolyTrack 0.6.3 then created its master key and
encrypted preferences, opened its database and reached its menu, where a
track started (seen on screen; longer play and saved progress not checked).
Its second start, and a start after a forced runtime replacement, reused the
stored key.

### User and idle-policy queries (2026-10-14)

`FrameworkUsers` replaces the launcher's partial `user` handler. The existing
device profile has one full system user (0), running, unlocked, visible and
administrative, with serial number 0. It now supports profile enumeration,
profile type, user state/flags, serial lookup and restriction queries through
the real UserManager. Profile objects, id arrays and restriction Bundles are
fresh replies. Unprivileged profile replies redact name/icon; user
administration permissions are independent of the user's admin flag.
First-user restrictions come from this image's
`config_defaultFirstUserRestrictions` resource. Application restrictions remain
empty because no administrator provisions them.

Permission checks follow Android 15's
[UserManagerService](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-15.0.0_r1/services/core/java/com/android/server/pm/UserManagerService.java):
self/profile-group queries, foreign-user interaction, full user info and
profile enumeration have separate gates. Missing-user `hasUserRestriction`
returns false before permission checking, as in AOSP. Application-restriction
reads enforce package UID/user ownership. Mutations, user creation/removal,
user switching and restriction listeners remain unsupported.

The new `FrameworkDeviceIdle` registers `deviceidle`, allowing
`PowerManager.isIgnoringBatteryOptimizations` to use its genuine endpoint.
Doze remains disabled in this profile, with no provisioned system/user/temp
power exemptions. Query methods return fresh empty lists or false membership;
they do not infer an exemption from Doze being disabled. Permanent and
temporary changes enforce `DEVICE_POWER` and
`CHANGE_DEVICE_IDLE_TEMP_WHITELIST`, respectively, before declining privileged
administration. Coverage inventory excludes those unsupported mutations.
This follows
[DeviceIdleController](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-15.0.0_r1/apex/jobscheduler/service/java/com/android/server/DeviceIdleController.java).

`FrameworkPower` also supplies `getPowerSaveState` and low-power-standby
exemption/reason/feature queries. Unsupported battery saver returns AOSP's
default `PowerSaveState.Builder` reply, including brightness factor 0.5;
unsupported/disabled standby permits the queries. These follow
[PowerManagerService](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-15.0.0_r1/services/core/java/com/android/server/power/PowerManagerService.java)
and
[LowPowerStandbyController](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-15.0.0_r1/services/core/java/com/android/server/power/LowPowerStandbyController.java).
They do not alter host suspend policy or implement Doze, exemption scheduling,
foreground-service policy or notification delivery. These are local Java service-cache
contracts; see [runtime checks](../archive/TESTS-history.md#user-and-idle-policy-queries-2026-10-14).

### Settings, PendingIntents and broadcasts (2026-10-13)

`FrameworkSettings` serves `GET_system`, `GET_secure`, `GET_global`,
`GET_config` and the corresponding `LIST_*` call protocols. Its fresh user-0
phone defaults come from Android 15's
[DatabaseHelper](https://github.com/aosp-mirror/platform_frameworks_base/blob/android-15.0.0_r1/packages/SettingsProvider/src/com/android/providers/settings/DatabaseHelper.java),
the SettingsProvider resources, framework resources from this runtime, and
the fresh-device effects of
[UpgradeController](https://github.com/aosp-mirror/platform_frameworks_base/blob/android-15.0.0_r1/packages/SettingsProvider/src/com/android/providers/settings/SettingsProvider.java).
`Tools/Android/generate-settings-defaults.py` caches the pinned inputs outside
the checkout and emits `FrameworkSettingsDefaults.java` with source hashes;
`--check` verifies regeneration. The destination table follows the loader,
including constants historically aliased to another namespace. Framework
configuration resources are resolved from `Resources.getSystem()` at runtime.
`accelerometer_rotation` is present and defaults to `0`; Google's
`limit_ad_tracking` remains absent.

This is a fresh phone profile, not an existing-device database upgrader. Wear
defaults (v203/v217), VR listeners (v125), upgrade-only SSAID migration (v136),
gesture navigation insets (v191), refresh-rate rewrites (v222), and deletes of
keys absent on a fresh device are omitted. It has no DeviceConfig overrides,
query/cursor API or privileged settings administration.

Foreign callers/users are rejected; current-user aliases resolve to user 0.
`PUT_secure`, `PUT_global` and `PUT_config` require the unavailable privileged
grant. `PUT_system` requires requested `WRITE_SETTINGS` and target SDK below
23: newer apps have no user-granted special access in this profile.
The install-time permission model and `Settings.System.canWrite` agree with
that policy. Writes are limited to AOSP `PUBLIC_SETTINGS` and checked against
its validators. Values, including nullable overrides, are atomically saved in
the app runtime's `/data/system/users/0/bifrost-system-settings.xml`, preserved
through runtime replacement. This is per-app state, not device-wide mutable
settings shared between different applications. Read-only `MemoryIntArray`
duplicates carry generation data; writes invalidate framework caches and
notify matching ContentObservers.

Framework permission definitions are parsed once from the immutable platform
APK and cached separately. Each app install still recomputes app definitions,
requested permissions and grants, so the cache does not retain revoked or
previous-app grants.

`FrameworkPendingIntents` follows Android 15's
[PendingIntentController](https://github.com/aosp-mirror/platform_frameworks_base/blob/android-15.0.0_r1/services/core/java/com/android/server/am/PendingIntentController.java)
and [PendingIntentRecord](https://github.com/aosp-mirror/platform_frameworks_base/blob/android-15.0.0_r1/services/core/java/com/android/server/am/PendingIntentRecord.java):
identity includes sender type, package/feature, Activity token/result target,
request code, final Intent filter, resolved type, retained flags and user.
Extras do not change identity. `NO_CREATE`, `CANCEL_CURRENT`, `UPDATE_CURRENT`,
`ONE_SHOT`, immutable/mutable sends, cancel listeners, metadata and tags are
supported. Update replaces extras even for immutable senders; mutable send
uses `Intent.fillIn`. The local IIntentSender is attached to a real Binder,
so Parcel round trips retain identity. Android 15 exposes creator package/UID
through `getInfoForIntentSender` rather than older separate AIDL queries.

Activity sends use `FrameworkActivities`; Activity-result sends post to the
target Activity; service sends use `FrameworkServiceRuntime`. Foreground
service sends retain that runtime's explicit unsupported error. Dynamic
receiver registration retains filters, permissions and export flags;
`broadcastIntentWithFeature` and PendingIntent broadcasts match them and
the installed manifest's receivers. Android 15's `scheduleRegisteredReceiver`
and `scheduleReceiver` deliver through ActivityThread. Ordered delivery waits
for `finishReceiver`, propagates results, and honors aborts. Protected actions
come from the platform manifest; apps cannot send them. Broadcast options,
app-op policies, sticky app broadcasts, receivers in other processes and
cross-app delivery are unsupported. There is no broadcast timeout/ANR policy.
Sender records and alarms live in the current process; they do not survive
process exit or implement system-server background/wakeup policies.
Notifications are posted and listed in memory but never shown, so supporting
PendingIntent tokens does not make notification taps available.

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
window entry points that already reach the platform library. MediaCodec
callbacks, video/audio decoding and in-game playback remained unfinished at
this point (EGL native-buffer import came later, under the EGL adapter). Two full games running
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

### Host text input (2026-10-20)

An editor with a live `InputConnection` (an `EditorInfo` that is not
`TYPE_NULL` and a non-null `IRemoteInputConnection`) is bound to a host session:
`startInputOrWindowGainedFocus` returns `SUCCESS_WITH_IME_SESSION` with an
`IInputMethodSession` and no input channel, so the client's `dispatchInputEvent`
still answers `DISPATCH_NOT_HANDLED` and key events keep reaching the views. The
session records the connection and its command session id, which
`RemoteInputConnectionImpl` advances each time the app invalidates its text
(`invalidateInput`); a command carrying a stale id is dropped by the app, as for
an installed IME. `finishInput`, a non-editor start or a dying client ends it.
Any other start keeps the `NO_IME` path of the [input methods](#input-methods-october-11-2026)
below.

While a session is active the launcher starts the host's text input (private
syscall 0x100E op 20; the poll loop applies it on the window thread). The host
reports committed text and composition as text events in order with key events
(op 3 returns the payload in the same record), and the launcher delivers them
with `commitText` and `setComposingText` (cursor after the text). A key press
that produced text is not also delivered as a key event, or each character would
type twice; Backspace, Delete, Enter, arrows and shortcuts stay key events. The
desktop's own input method (layouts, dead keys, CJK composition) supplies the
text, so non-ASCII input needs no key mapping. See the
[window plugin interface](ANDROID_WINDOW_PLUGINS.md#text-input-2026-10-20).
The composition is anchored at the bottom of the window until apps report a
caret rectangle (`updateCursorAnchorInfo` is accepted and ignored). There is
still no on-screen keyboard, and `showSoftInput` reports the keyboard hidden.

The virtual keyboard (id -1, which `KeyCharacterMap.load` falls back to for every
other device id) now has Android 15's `Virtual.kcm`: type `FULL`, US QWERTY with
shift, caps lock and alt behaviors. It used to be an empty map (type unknown),
so `TextKeyListener` chose its `NullKeyListener` and physical Backspace, Delete
and arrows did nothing in an `EditText`, and `KeyEvent.getUnicodeChar()` was
always 0. `Tools/Android/generate-key-layout.py` parses the pinned file with the
rules of libinput's `KeyCharacterMap` parser into `FrameworkKeyLayout.java`
(`--check` verifies it; regenerate rather than edit), and `FrameworkKeyboard`
writes the native parcel the framework reads. `BIFROST_TEXT_SCRIPT` pushes
scripted key and text events into the window's SDL queue for tests.

### Shared libraries and the device configuration (2026-10-28)

Shared libraries (`<uses-library>`): the image's `/system/etc/permissions/*.xml` declare each as `<library name file>`.
Provisioning adds the five that name a jar in `/system/framework` (`org.apache.http.legacy`, `android.test.base`,
`android.test.mock`, `android.test.runner`, `javax.obex`) with pinned digests, read-only (`protect_shared_libraries`, run
on every reused managed root; ART rejects writable dex). `FrameworkSharedLibraries` reads the declarations (a library
whose file is absent is ignored; `min-device-sdk` and `on-bootclasspath-*` are not read) and `FrameworkPackages.install`
resolves the manifest's required then optional libraries from a cached parse, adding `org.apache.http.legacy` for an
app targeting API below 28 unless it is declared. The result is recorded as `SharedLibraryInfo` objects (built-in, path set,
version undefined, declared by `android`) in `ApplicationInfo.sharedLibraryInfos`, and as paths in `sharedLibraryFiles`:
`LoadedApk.makePaths` keeps only `.apk` entries of the latter, so a jar library reaches the app only as a library class
loader built from the former. A required library the image lacks is logged (`APP_SHARED_LIBRARY_MISSING`) and the app
still starts. Not done: system packages (GmsCore, microG) get no libraries resolved, and system native libraries
(`uses-native-library`) are not provided.

The global configuration (`FrameworkDisplay.globalConfiguration`) now carries what WMS derives from the input devices: a
finger touchscreen, no physical keyboard and no navigation device. `getDeviceConfigurationInfo` reads it as
`ActivityTaskManagerService` does (input features follow the keyboard and navigation), with `ro.opengles.version` from the
property store (3.0, 196608).

### Pointer icons (2026-10-28)

On Android, `ViewRootImpl` resolves the pointer icon for mouse events
(`ViewPostImeInputStage.maybeUpdatePointerIcon`, then `updatePointerIcon`: `mView.onResolvePointerIcon(event,
pointerIndex)`, the default arrow when that returns null, reported only when the icon changes; read from
`android-15.0.0_r1`). Host input here bypasses those stages, so after each `ACTION_HOVER_MOVE` that no popup window
took, the launcher resolves the icon against the Activity's decor view itself (`FrameworkCursors.hover`, on a copy
of the event with `SOURCE_MOUSE`, the source `ViewRootImpl` requires) and shows the nearest host system cursor (syscall 0x100E op 24,
[plugin `set_cursor`](ANDROID_WINDOW_PLUGINS.md#pointer-shapes-2026-10-28)). `TYPE_NULL` hides the pointer; hand,
text, wait, crosshair, cell, grab and scroll, no-drop and the resize arrows map to their SDL counterparts; the
rest (context menu, help, alias, copy, zoom) and an unmapped type show the arrow.

Limits: only hover updates the shape, because a touchscreen never hovers while the mouse can (a mouse drag keeps the
shape from before the button went down); an app's own bitmap cursor (`TYPE_CUSTOM`) shows the arrow; the shape is not
reset when the pointer leaves the window or another Activity opens. The first failure (a host without the callback, a
refused shape, a framework error) turns the feature off and logs `APP_CURSOR_UNAVAILABLE` once. `ManagedServiceBatch`
checks the mapping and the send-on-change behaviour through a real `View` and `PointerIcon`s
(`MANAGED_CURSORS_OK`); the SDL3 backend is checked against a fake SDL3 that logs each cursor call. Not checked: a
game that sets a pointer icon, or a live window.

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
It does not install an on-screen keyboard, or implement WindowManager focus
arbitration, ViewRoot input stages, or cross-process Java framework services.
Host key events dispatch directly to the Activity; the desktop's text reaches
an editor through the [host session](#host-text-input-2026-10-20).

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

Later callers hit the same abort from other native threads (Oddmar when its splash video ended; Firefox reports the
same message, not re-tested). Every managed app process now preloads `libbifrost-binder-attach.so`, which interposes
`BBinder::transact` and attaches an unattached thread under the same rules. The same probe transacts on a Java Binder
through the NDK's `AIBinder` from two fresh pthreads. Without the preload it reproduces the abort (exit 127).

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

Service children route outgoing start, stop, bind, unbind, peek and stop-self
requests over the existing authenticated Binder controller to the session parent.
Only the parent retains the host process-supervisor capability. The controller
checks the child's key, Binder PID and UID; service resolution then checks that
child's package, user, export status and permissions, including cached service
records. Connection ownership and service-token ownership are checked separately.
Isolated callers cannot start or bind other services, but can stop their own
service. A dead child releases its outgoing bindings. This permits nested service
clients such as Phonesky calling microG's DroidGuard without giving the child
direct access to the host supervisor. The focused headless regression is described
in [the test guide](../archive/TESTS-history.md#child-service-routing-2026-10-23).

The real APK service gate checks a different PID, the same ordinary app UID,
and a synchronous Binder round trip, in addition to local lifecycle behavior.
The isolated WebView handshake is still incomplete. This profile does not model
the foreground-service start deadline (see Foreground services) or
installed-package intent-filter resolution. Unknown explicit components and
packages return absent results. It does not implement sticky
restart/redelivery after process death, arbitrary cross-app Services beyond the
provisioned package set, Android background
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
rate. The implementation remains strict for unsupported display operations; each one an
app reaches is named once in the log (`SERVICE_CALL_UNSUPPORTED display <call>`).

Eleven further `IDisplayManager` queries are answered (2026-10-25), each as `DisplayManagerService`
at `android-15.0.0_r1` does on a phone with one built-in panel (source fetched and read for each):
`getStableDisplaySize` is the default display's natural size, recorded from the first size play mode
gives it and unchanged by later resizes or rotation; `getRefreshRateSwitchingType` is
`SWITCHING_TYPE_WITHIN_GROUPS` (the `DisplayModeDirector` default, which makes
`getMatchContentFrameRateUserPreference` seamless-only); `getUserPreferredDisplayMode`,
`getSystemPreferredDisplayMode` and `getDisplayDecorationSupport` are null;
`getUserDisabledHdrTypes` and `getSupportedHdrOutputTypes` are empty,
`areUserDisabledHdrTypesAllowed` is true, and both HDR conversion getters return
`HDR_CONVERSION_UNSUPPORTED` (the `DeviceConfig` flag for HDR output control is unset here);
`isMinimalPostProcessingRequested(0)` is false and an unknown display is a `NullPointerException`,
as in AOSP. Brightness, virtual-display, mirroring and mode-request calls need privileged permissions or
a compositor and stay unsupported. The display still reports one fixed 60 Hz mode with
`appVsyncOffsetNanos` and `presentationDeadlineNanos` of 0 (the `DisplayInfo` defaults): there is no
compositor timing to report, and the refresh rate does not follow the host monitor.
`FrameworkDisplayProbe` checks all of this (70 checks in each engine); the stable size after a
real host window's first size has not been observed in a live game.

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

Android composes these windows in SurfaceFlinger. Here each view tree is
drawn in software when it is dirty, on the UI loop or on the Looper of the
thread that owns the window (see
[windows owned by other threads](#windows-owned-by-other-threads-2026-10-17)),
and the UI loop sends the pixels through private
op 9 to a host layer above every surface (op 10 removes it); see
[window plugins](ANDROID_WINDOW_PLUGINS.md). A fully transparent window gets
no host layer. Host layers stack in creation order, so when a window or dim
layer appears below existing ones, the layers above it are recreated in
window order. `FLAG_DIM_BEHIND` adds a translucent full-display layer below
its window. Input follows InputDispatcher: touches go to the topmost window
containing them or to a modal window (a multi-pointer gesture stays with the
window its first pointer chose; touches are not split across windows), windows with `FLAG_WATCH_OUTSIDE_TOUCH`
receive `ACTION_OUTSIDE`, and keys go to the topmost focusable window (the
back key cancels dialogs). Exactly one window has focus, recomputed when a window is
added or removed, when a relayout changes its visibility or flags and when an Activity
stops or is visible again; a window that never had it gets no
`onWindowFocusChanged(false)`, as under WindowManagerService. A stopped Activity's
dialogs and popups (sub-windows follow their parent) are not visible, focusable or
touchable, and have no host layer, as `WindowState.canReceiveKeys` and
`isVisibleRequestedOrAdding` require a visible Activity; a window added to a stopped
Activity is added without `ADD_FLAG_APP_VISIBLE`, and its views are told
(`dispatchAppVisibility`) when the Activity restarts. Windows stack by type layer
(`WindowManagerPolicy.getWindowLayerFromTypeLw`: application windows, then toasts, overlays
and input methods), an Activity's windows together in the order of its first window with
its base window lowest, and sub-windows within their parent's subtree by sub-layer, not
in add order; types the policy does not know take its default layer.
The log records `APP_POPUP_ADDED <id> type= title= stack=<window ids, bottom to top>`, `APP_POPUP_SHOWN <id> <frame>
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
`surfaceDestroyed` before `onStop`; a removed window delivers it when the
window detaches, before SurfaceView releases its Surface. Both happen on the
window's own thread. The log records
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

### SurfaceView layers are clipped to what shows (2026-10-28)

YouTube's Shorts pager scrolled vertical videos through a landscape window: each video's SurfaceView became a host
layer at its full size, so videos bled over the UI and each other (a Roblox clip and a marble-machine clip drawn
across the page), and a video scaled to the window's width (a 9:16 clip is about 4400 px tall at 2460 px wide) was
refused by the 4096 px limit of the geometry call, which ended the process with `IllegalStateException: Surface
geometry: -22`. `FrameworkSurfaces` now takes each SurfaceView's visible rectangle (`getGlobalVisibleRect`: clipped by
every ancestor and the window, as SurfaceView's own window crop is) and, for a video layer (frames through the
ImageReader), converts and presents only that part at its offset; a view scrolled out shows a transparent pixel.
Conversion cost follows what shows, and the 4096 px limit applies to the visible region, which a window bounds. The
geometry call accepts up to 16384 like the other host surface limits. `APP_SURFACE` carries `clip=l,t,r,b` when the
view is clipped (`test_managed_view_window_and_surface_lifecycle`). Not done: a SurfaceView that renders with a native
window (GL, as a game does) is positioned whole and not cropped, since that needs a viewport source crop of its EGL
buffer; and a paused video that is scrolled keeps its last frame stretched until the next frame arrives. 
A frame's YUV to ARGB conversion used to run per pixel in Java on the main Looper: a 3.3 Mpixel video layer took
763 ms a frame (1.3 fps, the main thread 104% busy), so on a wide window YouTube's UI starved, search worked but its
results stayed gray, and a Shorts feed hit the stall watcher. `APP_SURFACE_FRAMES` (one line per 10 s while frames
flow: frames, fps, acquire and present milliseconds per frame, pixels, busy share) came from that profile. The
conversion is now host code (`Source/System/Android/Surface/Yuv420.cpp`, syscall 0x100E op 25): the managed side passes the
image's three direct planes with strides, crop, matrix and range, and the host reads them in place and converts only the
visible region, with fixed-point BT.601/709/2020 weights in limited or full range and nearest-sample scaling.
Measured on this machine (scalar, one thread): 15.7 ms for 2408x1355 unscaled, 17.3 ms for 1920x1080 scaled to 2408x1355,
18.2 ms for the visible 2460x1384 of a 1080x1920 Short scaled to a 2460x4373 layer. Frames are handled on their own
`SurfaceFrames` thread, off the main Looper. `make test-android-yuv420` checks the converter against a floating-point
reference (planar and semi-planar chroma, padded rows, crop scaling, regions, matrices, ranges, refusals); the end-to-end
CPU video path has no fixture and was checked on YouTube only.

### YUV conversion kernel (2026-10-31)

A TextureView's YUV frames converted through a second, scalar copy of this maths in `AndroidBufferImages::convert`, on
HWUI's render thread at every `glEGLImageTargetTexture2DOES` (the bind has to leave the frame's pixels in the texture).
Both callers now use `yuv420_to_argb` (`YuvOrder::Rgba` for the GL upload). Its AVX2 kernel (selected at run time with
`__builtin_cpu_supports("avx2")`, which includes the OS's YMM state; other hosts run the scalar definition) converts 16
pixels at a time for rows that are unscaled across with luma at stride 1 and chroma planar or semi-planar (stride 1 or
2); odd start columns and the tail go through the scalar pixel function, and no wide load goes past a plane's `size`.
Scaled rows gather their samples into a staging row (scalar) and then use the same kernel. The kernel is bit-identical
to the scalar code, which stays as the definition. Measured on a Ryzen 7 5800X3D, one thread, random planes (a
micro-benchmark, not a game): 1920x1080 planar 9.1 to 0.66 ms, NV12-style chroma 10.0 to 0.68 ms, 3840x2160 39.9 to
2.6 ms (4.7 to 0.32 ns a pixel); 1920x1080 scaled to 2408x1355 13.8 to 4.6 ms, where the scalar gather is most of what
is left. The TextureView path's limited-range output moved by at most 2 levels (its old code folded the range scaling
into the coefficients; full range is unchanged). The conversion stays on the render thread: at about 0.7 ms for 1080p
there is nothing to hide, and the bind must return with the frame uploaded.

### Default bitmap density (2026-10-28)

`APP_VIEW_DUMP` showed YouTube's like, dislike, comment and bookmark icons as software `ARGB_8888` bitmaps with
`density=160` next to `HARDWARE` ones with `density=320`; its image drawable scales a bitmap by its density, so the
density-160 ones drew at half size on the 320 dpi window. `Bitmap`'s default density is `DisplayMetrics.DENSITY_DEVICE`
(the `ro.sf.lcd_density` property, unset here, so 160) until `ActivityThread.updateDefaultDensity` calls
`Bitmap.setDefaultDensity(densityDpi)`. The launcher now makes that call with the window's density when it publishes
the metrics (`ManagedHardware` checks a new bitmap's density equals the display's; it was 160 against 320 before). The
density is not updated if the window moves to a display with another scale.

### View dump on demand (2026-10-28)

Icons that draw at inconsistent sizes need the views' own numbers. While the file `data/local/tmp/bifrost-dump-views`
exists in the app's managed root (`<state>/<package>/managed/rootfs/`), the play loop (checked twice a second) writes
one `APP_VIEW_DUMP` line per visible `ImageView` of the Activities' windows (up to 400): view class, id name, size,
place in the window, scale, the drawable's class, intrinsic size and bounds, the scale type, and the density
(`Configuration.densityDpi` and `DisplayMetrics.densityDpi`) and screen width/smallest width in dp its Resources see,
then `APP_VIEW_DUMP_END <count>`, and removes the file. Create the file with `touch` while the app runs.

### Window shape (2026-10-28)

Roblox opened as a near-square 1440x1384 window and stayed that way. Its splash Activity declares no orientation, so
the launcher asked for a 720x1280 portrait window; the compositor (KWin) clamps a new window to the work area, so the
height became 692 desktop units and the width stayed 720. Roblox is not resizable, so `fixWindow` froze that shape,
Roblox classified the device as a tablet and turned to landscape, and a rotation swapped the clamped pixels. On a
3440x1440 monitor the engine's surface then did not match the window, and a maximized window showed it cut off with
black at the side. Observed with a probe of the real backend on this KWin/Wayland desktop (not on other compositors):

- `SdlAndroidWindowBackend::open` asks again, after the compositor's first configure, for the largest window of the
  requested shape that fits (`fit_new_window`), and remembers the clamped dimension as the work area limit. A later
  `SetWindowSize` by the app is honored beyond that limit (a 1280x720 request on a 692 high work area gave exactly
  1280x720), so only the open is fitted.
- An Activity opens portrait only when its manifest asks for portrait; otherwise the window is landscape.
- `set_orientation` for a fixed-size app turns the panel to its other shape (1280x720 or 720x1280 units, shrunk to the
  work area limit, so landscape is 1230x692 and portrait 389x692 here), and `set_window_orientation` reports the size
  the backend ended with instead of assuming a swap. The panel's pixel dimensions do not follow the window: a
  maximized 21:9 window shows the 16:9 panel pillarboxed by the fit (`g_fit`), never cut off.

`make test-android-sdl-backend` covers the clamp and the rotations with a fake SDL3 whose
`BIFROST_TEST_SDL_WORK_AREA` clamps new windows. Roblox's menu surface now matches its window (2460x1384); a game
in landscape was not run.

### Native-density play windows (2026-10-12)

Play mode opens the host window at 1280x720 desktop units (720x1280 only for an
Activity that asks for portrait; one with no orientation, such as Roblox's splash
or YouTube, opens landscape), then reads its native pixel size and density through
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
Posting is described under Foreground services and notifications below; listeners still throw.

Without the service, `NotificationManager.areNotificationsEnabled()` threw
inside SpongeBob's `NotificationSettings` helper on every game state change.
Unity surfaced the exception in the game's state-change event, which then
skipped `MainHudHandler` re-enabling the main HUD after Settings closed: the
scene kept drawing without its buttons. The user confirmed the HUD now stays.

### Foreground services and notification posting (2026-10-28)

YouTube's playback service calls `Service.stopForeground` when playback pauses. `activity.setServiceForeground` was
unsupported, the `UnsupportedOperationException` went up through the touch dispatch and the app exited with status 1.
`FrameworkServiceRuntime` now keeps the foreground state of each service record and follows
[ActiveServices.setServiceForegroundInnerLocked](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-15.0.0_r1/services/core/java/com/android/server/am/ActiveServices.java)
(read from android-15.0.0_r1):

- `startForegroundService` is accepted and marks the record as owing `startForeground` (`fgRequired`).
- `startForeground(id, notification, type)`: `id != 0` needs a notification (`IllegalArgumentException` otherwise),
  `FOREGROUND_SERVICE` for an app targeting Android 9+, a type that is a subset of the manifest's
  (`FOREGROUND_SERVICE_TYPE_MANIFEST` means the manifest type; a short service combined with other types is a regular
  one; a short service must be started). The framework's own `ForegroundServiceTypePolicy` (default policy,
  while-in-use permissions allowed because the app is the foreground app) then checks each type's permissions
  (`FOREGROUND_SERVICE_MEDIA_PLAYBACK`, ...) and raises `MissingForegroundServiceTypeException`,
  `InvalidForegroundServiceTypeException` or `SecurityException` as `validateForegroundServiceType` does.
- `stopForeground(flags)` on a service that is not foreground does nothing (the YouTube case). On one that is,
  `STOP_FOREGROUND_REMOVE` cancels the notification; otherwise, for an app targeting Android 5+, it stays (until the
  service stops) without its foreground flag, and `STOP_FOREGROUND_DETACH` forgets it. `getForegroundServiceType`
  returns the current type (0 after stop). A service that stops, or whose child process dies, releases its notification
  unless another foreground service of the app shares its id.
- Calls from a service in a child process travel over the service socket (`SERVICE_FOREGROUND`,
  `SERVICE_FOREGROUND_TYPE`) to the session's runtime, which refuses another process's token as `stopSelf` does.

The foreground service's notification goes to `FrameworkNotifications`, which now also implements
`enqueueNotificationWithTag`, `cancelNotificationWithTag`, `cancelAllNotifications` and `getAppActiveNotifications` over an
in-memory store of this process's `StatusBarNotification`s, following
[NotificationManagerService.enqueueNotificationInternal](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-15.0.0_r1/services/core/java/com/android/server/notification/NotificationManagerService.java):
a post to a channel the app never created (an app targeting Android 8+ has no implicit one), to a channel with
`IMPORTANCE_NONE`, or by an app without `POST_NOTIFICATIONS` that is not a foreground service, is dropped without an
error; only a foreground service's own notification keeps `FLAG_FOREGROUND_SERVICE` (and loses `FLAG_AUTO_CANCEL`); an
app cannot cancel a notification that carries the flag. This profile grants no runtime permissions, so an app's own
`notify()` is blocked, as on a device where the user has not allowed notifications.

Not modelled (checked against the source only): the 10 s deadline after which an app that did not call `startForeground`
after `startForegroundService` is crashed, background-start restrictions, the `OP_START_FOREGROUND` app op, the short
service's 3 minute timeout and `onTimeout`, foreground service time limits, the deferred display of a foreground
service's notification, delivery to the host (nothing is shown, so no taps), and `getRunningServices` from a child
process. The notification store is per process: a notification an app posts from a child process is listed only there,
while a foreground service's notification is always the session's. `ManagedServices` (`foreground`: a service in the app
process and one in `:remote`, with the stray `stopForeground`, a type missing from the manifest, detach and remove) and
`ManagedServiceBatch` (a `getForegroundService` `PendingIntent`) check it; both observed, not verified on a device.

### Battery (2026-10-28)

`FrameworkBattery` is BatteryService and healthd for the machine the emulator runs on. The VFS serves
`/sys/class/power_supply` from the host (`SysFS.cpp`: names and attributes validated, regular files only, read-only
(`EACCES` on write), 4 KiB cap, re-read at every open; an empty directory on a host with no supplies). The state
follows [BatteryMonitor](https://android.googlesource.com/platform/system/core/+/refs/tags/android-15.0.0_r1/healthd/BatteryMonitor_v1.cpp)
(android-15.0.0_r1): the first system battery (a `scope` of `Device`, a mouse or headset, is not the device's) gives
presence, capacity (or charge/energy now over full), status, health, voltage, temperature, technology, cycle count,
charge counter and current; every online charger of type `UPS`/`Mains`/`USB_*` (AC), `USB`/`USB_PD_DRP` (USB),
`Wireless` or `Dock` sets its plug bit. `BatteryManager` properties come from the same files (charge counter, current
now/average, remaining energy from `energy_now`, capacity, status; an attribute the battery lacks is not found). A host
with no battery is a full one on AC mains.

The state is re-read every 30 s while the session runs. A change broadcasts as
[BatteryService.processValuesLocked](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-15.0.0_r1/services/core/java/com/android/server/BatteryService.java)
does: `ACTION_BATTERY_CHANGED` with a new sequence number, `ACTION_POWER_CONNECTED`/`DISCONNECTED` when the plug
changes, `ACTION_BATTERY_LOW` at 15% unplugged and `ACTION_BATTERY_OKAY` at 20% or when plugged. Broadcasts reach this
process's registered receivers only. Not modelled: critical-level and over-temperature shutdown, charging policy, time
estimates (`computeBatteryTimeRemaining` is -1), health dates and state of health, and uevent-driven updates (the poll
is the only trigger). Checked: `ManagedServiceBatch` (`batteryFromSupplies` runs the model against a fake tree: battery
plus mains plus a peripheral, unplug at 14%, replug at 25%, and an empty tree), and `test_sysfs_cpu` for the VFS; this
desktop host has no battery, so the VFS attribute reads and the write refusal are not exercised on real supplies.

### App ops and missing content providers (2026-10-12)

`SystemServiceRegistry` builds `AppOpsManager` from
`ServiceManager.getServiceOrThrow("appops")`, so without that service
`getSystemService(APP_OPS_SERVICE)` returned null. Firebase measurement then
failed its upload task with a `NullPointerException`. `FrameworkAppOps`
registers `com.android.internal.app.IAppOpsService`; it first answered only
`checkPackage(uid, pkg)` (GMS UidVerifier) and was extended method by method
until the full interface below. `checkPackage` follows Android 15's
[AppOpsService](https://android.googlesource.com/platform/frameworks/base/+/refs/heads/android15-release/services/core/java/com/android/server/appop/AppOpsService.java)
`checkPackage`/`verifyAndGetBypass`/`resolveUid`. Root may name any existing
package. `root`, `shell`, `dumpstate`, `media`, `audioserver`, `cameraserver`
and `com.android.shell` resolve to fixed uids. Any other package must exist
and belong to the uid. Failures return `MODE_ERRORED`, which
`AppOpsManager.checkPackage` throws as `SecurityException`.

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
started for a result. `registerScreenCaptureObserver`/
`unregisterScreenCaptureObserver` (Android 14's
`Activity.registerScreenCaptureCallback`) enforce `DETECT_SCREEN_CAPTURE`
and keep the observers on the Activity's record, as
`ActivityTaskManagerService` does; an unknown token is ignored. Nothing here
detects a screenshot, so no observer is called. Roblox registers one in its
main Activity's `onPostResume`. The launcher Activity is registered before its
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

Only explicit components of the installed package resolve, plus Play services'
Activities, which open in a window of their own (see
[Google accounts and sign-in](#google-accounts-and-sign-in-2026-10-20)).
A web link, and a Play Store link, opens in the host's default browser when the
user pressed or touched the window in the last 5 seconds (see
[web links](#web-links-and-play-store-links-2026-10-20)).
Other implicit Intents and other packages (browser, Play Store) return `START_INTENT_NOT_RESOLVED`
and an unknown class `START_CLASS_NOT_FOUND` (Android 15 values -91 and -92),
so apps receive `ActivityNotFoundException` as on a device without a handler.
`startActivityIntentSender` (`startIntentSenderForResult`) sends a
PendingIntent of this process like `PendingIntentRecord.sendInner`: the
mutable flag override, and a single activity started for the caller's
`resultTo`/`requestCode`. A sender another process created is a Binder proxy.
The creator's sender Binder answers one transaction outside `IIntentSender`
(`FIRST_CALL_TRANSACTION + 1000`) with the Intent, resolved type, flags and
owner of an Activity sender aimed at Play services (`com.google.android.gms`
component or action) that is not cancelled; it describes no other sender.
`startActivityIntentSender` asks, then starts that Activity from the caller's
own process through the Play services window relay
([above](#google-accounts-and-sign-in-2026-10-20)), with the caller's
`resultTo`/`requestCode`, the sender's mutability and the fill-in Intent applied
as `PendingIntentRecord.sendInner` does. This is how Play services' sign-in UI
reaches an app (Play Games v2's `GamesResolutionActivity` in Azur Lane). Any other foreign sender, or a
cancelled one, still returns `START_CANCELED`, so the caller gets
`IntentSender.SendIntentException` and continues without it. Limits: a
one-shot foreign sender is not spent in its creator, only the last Intent of a
multi-Intent sender is described, and the Activity runs as the calling app, not
the creator (the relay checks the app as the calling Activity). Activities of
other processes cannot be shown, except Play services'.
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
software. Their topmost orientation request now controls the display in play
mode; launch flags, task affinities and `onNewIntent` are not modelled.
A mouse's back/forward buttons send `KEYCODE_BACK`/`KEYCODE_FORWARD`, as
Android's InputReader synthesizes them. The mouse wheel, a trackpad pinch and a
held middle button reach apps as two-finger pinch and pan gestures (see
[the window plugin input contract](ANDROID_WINDOW_PLUGINS.md)); these are what
a touch-driven game such as Melon Sandbox uses to zoom and move its view. They
are checked against a fake SDL3 and the unit test, not on a live desktop. The configured back key
(`[android] back_key`, `BIFROST_ANDROID_BACK_KEY`; default `escape`, `none`
disables) is delivered as `KEYCODE_BACK` to NativeActivity and managed apps
alike; that host key then no longer reaches apps under its own code. The
`enter_key` setting (`[android] enter_key`, `BIFROST_ANDROID_ENTER_KEY`; default
`enter`) sends Enter as `dpad_center` or `button_a` for games that draw their own
cursor. `button_a` reports a gamepad source; a text field keeps a plain Enter,
and a release follows its press. Whether a given game reacts is not checked.
Back follows `ActivityClientController.onBackPressed` (android-15.0.0_r1):
a stacked Activity is asked to finish through its `IRequestFinishCallback`,
which posts `finishAfterTransition`. On the launcher root Android moves the
task to the back; with no home screen here the root stays in front, so only
the app's own key handling (Unity's view, for example) acts on it.
`getTaskForActivity` reports the single launcher task.
`getTasks` (`ActivityManager.getRunningTasks`) follows `RunningTasks`: an app sees only its own task, here the launcher task while
it has a non-finishing Activity, with the base intent's extras stripped unless `keepIntentExtra`, and nothing for `maxNum <= 0` or
another display. `moveActivityTaskToBack` takes the route of a HOME start. Answering `getProcessMemoryInfo` fills only the
caller's own pid; `getProcessesInErrorState` is null, since no crash or ANR dialog exists here. Audio routing is the host's, so
`getDevicesForAttributesUnprotected` names the one built-in speaker for any attributes.
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

#### AudioTrack playback (2026-10-21)

Java `AudioTrack` (FMOD, ExoPlayer-style players), SoundPool-style users and
MediaPlayer reach native `AudioTrack`, whose `createTrack` failed with
`AudioFlinger could not create track, status: -74`: Umamusume's FMOD logged
about 60 of these and had no sound. `media.audio_flinger` now implements
`IAudioFlingerService.createTrack` and `newAudioUniqueId`/`acquire`/`releaseAudioSessionId`
for linear PCM streaming tracks. Wire formats and rules were read from
android-15.0.0_r1 (`IAudioFlingerService.aidl`, `IAudioTrack.aidl`,
`CreateTrackRequest/Response.aidl`, `SharedFileRegion.aidl`, `AudioTrackShared.h/.cpp`,
`AudioFlinger` `Tracks.cpp`/`Threads.cpp`) and checked against a real client:

- **Request/response.** Every nested parcelable or union field is written behind a
  non-null marker, a parcelable then has a size word; the channel mask is a union
  (`1 3 3`: marker, `layoutMask`, `LAYOUT_STEREO`). Refusals are
  `EX_SERVICE_SPECIFIC` with a `status_t`. Frame count is the larger of the request and
  three output quanta (AOSP's `calculateMinFrameCount` plus slack for the emulator);
  the notification period is half of it. Fast, offload, direct, static-buffer and
  non-PCM tracks are refused.
- **Shared region.** One memfd holds the 232-byte `audio_track_cblk_t` and a ring of
  `roundup(frameCount)` frames (the client indexes modulo the next power of two).
  `IAudioTrack.getCblk` returns it as a `SharedFileRegion` with `writeable` set; without
  it the client maps the region read-only and faults on its first write.
- **Server** (`Guest/Android/Adapters/audio_track.c`). A port of `ServerProxy` and
  `AudioTrackServerProxy`: obtain/release, flush and stop positions, start threshold
  (the whole buffer by default, since Android 12), underrun tally and the futex wake
  of the client. One AAudio float data-callback stream per track on the managed
  libaaudio adapter pulls frames, converts U8/16/24/32-bit, Q8.24 and float, applies
  the track's left/right gain and fills missing frames with silence. Stop drains up to
  the client's stop position; pause keeps the buffer; flush discards it.
- **Futex.** The client blocks in `FUTEX_WAIT` on the control block and the server
  wakes it through another mapping of the same pages. The emulator's futex table keyed
  words by guest address and never saw that wake; shared file mappings now use host
  futexes (see the syscall guide).
- **Metrics.** Native `mediametrics::LogItem::record()` waited 5 s for `media.metrics`
  twice per track; a stub service accepts and drops submissions.

Checked: `ManagedServiceBatch.audioTracks` plays a stereo tone (left a quarter of full
scale, right a half) through a real `AudioTrack` and asserts that the server consumed every
frame, that the per-channel peaks match, and that stop/start/pause/flush work, in JIT
and interpreter mode. Umamusume then logged no `AudioTrack` errors. Whether it is
audible and in sync was not measured. Not implemented: static (shared-buffer) tracks, so
SoundPool-style short sounds; kernel timestamps (`getTimestamp` reports would-block and
`getPlaybackHeadPosition` follows the frames consumed); sample-rate and playback-rate
changes after creation; volume shapers and effects; a track is freed when its Binder is
garbage collected, not at the client's release.

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
`BufferQueue::Clear` flushing audio already queued in the host stream
(up to about three callbacks). Stop does not play that audio out as Android does:
it freezes in the paused stream and is dropped if the player restarts, so a
restart never replays it. `test_android_bionic.sh` runs
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
symlink targets and executable flags, plus three derived Bionic compatibility
links (532 inventory entries). Besides the DT_NEEDED closure it extracts
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

Requirements: Python 3.11+, `debugfs`, `readelf`, Clang/LLD and a
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

## Web links and Play Store links (2026-10-20)

Across the saved game sessions the Intents nothing handled were almost all
links: privacy policies and terms (`https://`), ad clicks, `market://details`
and `play.google.com` install links. `FrameworkActivities.start` now sends an
`ACTION_VIEW` with an http(s) URL (no component, no package, or `com.android.chrome`)
to the host: a `market://details?id=` link, or a Play URL addressed to
`com.android.vending`, opens the store's web page. The request goes through the
session supervisor (`open_url` operation) to `xdg-open`, which starts the user's
default browser.

- **Only after input.** The launcher records the last key, touch or text event
  (`FrameworkActivities.userInput`); a link opens only within 5 s of one, the way
  Android allows a foreground start in response to input. An ad or SDK that
  fires a link on its own gets the old result, `START_INTENT_NOT_RESOLVED`.
  So does a refusal by the host, which is also what an app sees on a device with no browser.
- **The host does not trust the guest.** `android_processes.open_url` accepts
  only a well-formed `http://` or `https://` URL of up to 2000 printable
  characters (never an option, `file:`, `javascript:` or `intent:` link), at most
  one per second, passes it as one argument without a shell, and needs `xdg-open`.
  The same link again within 30 s is not opened twice (an ad's redirect loop fired
  one Play link five times in a few seconds and opened five tabs).
- **Intents that name another app** (`com.facebook.katana`, ...) stay unresolved
  so apps fall back as they do without that app.
- **Ad install links are rewritten.** An ad's "Install" button starts
  `https://play.google.com/d?id=<package>&inline=true&enifd=…&gclid=…` addressed to
  `com.android.vending`. That `/d` form is a deep link only the Play Store app
  opens; in a browser Google answers 404 (seen in Hungry Shark World, 2026-10-21).
  `FrameworkLinks.webLink` (plain Java) sends the public page
  `https://play.google.com/store/apps/details?id=<package>` instead, dropping the
  ad-tracking parameters (so the store does not see the click's `referrer`); it
  needs a valid package id in `id`, and leaves every other link, and a `/d` link
  without one, unchanged. `market://details?id=` uses the same page builder.

Observed live (Hungry Shark World, 358 s session): the game's links (a
conservation site, its Twitter page) and Play Store ad links reached the host as
`APP_ACTIVITY_BROWSER`; a burst inside one second logged `BROWSER_REFUSED` and fell
back to the unresolved result. Checked: `BrowserLinkTests` (the host rules, with a
fake launcher). Not automated: the guest side and a real browser opening; the existing
`MANAGED_ACTIVITIES_UNRESOLVED_OK` check still covers the no-input case.

## Main-thread stall reports (2026-10-20)

A Hungry Shark World profile showed a 6 s `Choreographer: Skipped 360 frames`
with the main thread at 100% CPU (about 500 samples/s against 50-200 normally),
no compile work and no log lines: guest code running on the UI thread. `perf`
cannot name guest code, and ART's SIGQUIT dump takes about 4.4 s and suspends the
process (the one dump taken during a pinned stretch was cut off by the session
timeout). `FrameworkStallWatch` is a daemon thread that sees whether the launcher's
UI loop has beaten in the last 250 ms steps; after 1.5 s of silence it prints
`APP_MAIN_STALL <ms>ms at <top 14 Java frames>` once a second, then
`APP_MAIN_STALL_END`, at most 60 reports per session. Reading one thread's stack
does not suspend the others, so it can stay on. Read a stall after the fact with
`grep APP_MAIN_STALL` in the session log. It shows Java frames only: a stall in
native code ends in the JNI method that called it.

First real session (Hungry Shark World, 358 s, with ads and links): 12 reports,
each 1.5 to 3.7 s. By cause:

- **WebView's first start, 1.7 + 2.7 s and a 3.7 s total** (`AwBrowserProcess` ->
  `BrowserStartupControllerImpl` -> native `startBrowserProcessesSync`): the UI thread
  waits while Chromium's browser process initializes, the first time an ad opens a
  WebView. About 3.7 s of main-thread block in one go; a hidden WebView created
  during the game's loading screen would move it (not tried).
- **Cold code in the app and its SDKs**: androidx DataStore initialization (1.7 s),
  and 1.5 s each in `Uri.decode` through ICU's charset converter and in
  `org.json` building and parsing a very large JSON value (the ad SDK). Guest code
  running slowly: JIT throughput and first-time translation.
- **Our own compositor, twice**: `Bitmap.getPixels` in `FrameworkWindows.drawSoftware`
  (1.5 s, copying a software window's pixels to the host layer) and
  `HardwareRenderer.syncAndDrawFrame` in `drawHardware` (1.6 s, an ad's frame).
  The first is conversion work that could move to the host.
- Reports that end in `nativePollOnce` are the sample landing just after the stall:
  the loop runs again within a few milliseconds (`APP_MAIN_STALL_END`).

## Startup: parsed-package cache (2026-10-20)

Session logs of real launches (SpongeBob, Azur Lane) put about 7 of the 11 to
12 seconds before the first window into manifest parsing, in this order on the
main path: the WebView provider (3.1 to 3.3 s), the app's `base.apk` (0.9 to 1.6
s), GmsCore when something asks for it (1.85 s), `framework-res.apk` (0.4 s) and
Phonesky (0.2 to 0.35 s); a second parse for the intent filters follows at the
first implicit-Intent or broadcast query. Every one of them reads an immutable
archive, and every service child repeats the platform one.

`FrameworkParseCache` keeps the result under `/data/local/tmp/package-cache` in
the app's root: the `PackageInfo` for `getPackageArchiveInfo` calls
(`installWebView`, `runApp`, `parseMicroG`, the platform permissions) and what
`FrameworkPackages` derives from a full parse (filters of activities, services,
providers and receivers, and the platform's protected broadcasts). An entry is
keyed by the archive's path, size and modification time (all files of a split
directory), the request flags, the platform resources' identity and the build
fingerprint; entries for an older version of the same archive are deleted, and
an unreadable entry is parsed again and rewritten. Writes are atomic renames.

Measured with a trivial fixture app (`ManagedSplash`), three launches in one
root: first launch (empty cache) window added at 8.2 s with the WebView parse at
3.2 s and `framework-res` at 0.6 s, as before the change; second and third
launches 4.85 and 4.93 s with no manifest parse in the log. The remaining
warm time is runtime start (about 2 s), font configuration (about 1 s) and
garbage collection. Not changed: the stalls inside apps after the first window
(Unity, Dynamite's class loading, Firebase), which dominate a game's first 30
seconds in those logs. A first launch after install still pays the parses.

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
skips the probe, for deterministic offline/online diagnostics. These two only
change what `ConnectivityManager` reports; they do not stop sockets.

`BIFROST_ANDROID_OFFLINE=1` (or `[android] offline = true`) is the enforced
offline mode (the per-app `network offline` setting). In every emulator process
of the session, `connect`, `sendto`, `sendmsg` and `sendmmsg` to any IPv4 or
IPv6 address except loopback (127.0.0.0/8, `::1`, the unspecified address and
v4-mapped loopback) fail with `ENETUNREACH`, a refused `sendmmsg` entry stops
the batch after the earlier messages as Linux does, the guest DNS proxy
answers every name except `localhost` and loopback literals with
`EAI_NODATA`, and Android is told there is no network. Unix, netlink and other
non-IP sockets, `bind` and `listen` are untouched, so an app that listens on a
wildcard address can still be reached from outside; the policy filters egress
only. Checked with `Tests/Unit/net_offline.c` (JIT and interpreter) and the
DNS proxy tests; not checked with a full app session.
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
unprovided keys return null so framework defaults still apply. Fresh AOSP
defaults, permitted System writes, observers and generation tracking are
implemented as described above; query-based access remains unsupported.
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
other UIDs/devices, runtime permissions and signature permissions are denied
(apart from the pre-M `WRITE_SETTINGS` compatibility grant described above);
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
without passing SDL3 pointers through SDL2 APIs. The statistics are opt-in
(title, in-window overlay and the independent stats window; see window stats).

The signed managed fixture verifies three launches, persistent preferences and
Android ID across a runtime replacement,
resource label resolution, a real SurfaceView, Activity.finish and ordered
pause/surface destruction/stop/destroy/close. The View fixture
(`ManagedViews`) launches an Activity without a SurfaceView, checks that its
button reaches the host, and adds, hides, shows and removes a SurfaceView with
exactly two create/destroy pairs. These checks do not establish
compatibility with arbitrary managed games.

## Recorded changes

Checkpoints and investigations are in [the history](../archive/ANDROID_RUNTIME-history.md), unchanged:

- [Ad SDK framework gaps (2026-10-17)](../archive/ANDROID_RUNTIME-history.md#ad-sdk-framework-gaps-2026-10-17)
- [Global Umamusume after the null-call fix (2026-10-22)](../archive/ANDROID_RUNTIME-history.md#global-umamusume-after-the-null-call-fix-2026-10-22)
- [Guest ptrace continuation (2026-10-22)](../archive/ANDROID_RUNTIME-history.md#guest-ptrace-continuation-2026-10-22)
- [Real media JNI (2026-10-13)](../archive/ANDROID_RUNTIME-history.md#real-media-jni-2026-10-13)
- [Wi-Fi display and local media routing (2026-10-15)](../archive/ANDROID_RUNTIME-history.md#wi-fi-display-and-local-media-routing-2026-10-15)
- [Azur Lane interaction checkpoint (2026-10-14)](../archive/ANDROID_RUNTIME-history.md#azur-lane-interaction-checkpoint-2026-10-14)
- [WebView startup checkpoint (2026-10-10)](../archive/ANDROID_RUNTIME-history.md#webview-startup-checkpoint-2026-10-10)
- [Activity creation checkpoint](../archive/ANDROID_RUNTIME-history.md#activity-creation-checkpoint)
- [Installed-app creation checkpoint (2026-10-07)](../archive/ANDROID_RUNTIME-history.md#installed-app-creation-checkpoint-2026-10-07)
- [Complete IAppOpsService (2026-10-21)](../archive/ANDROID_RUNTIME-history.md#complete-iappopsservice-2026-10-21)
- [Manifest provider startup and Azur Lane downloads (2026-10-13)](../archive/ANDROID_RUNTIME-history.md#manifest-provider-startup-and-azur-lane-downloads-2026-10-13)
- [Signals at syscall entry (2026-10-07)](../archive/ANDROID_RUNTIME-history.md#signals-at-syscall-entry-2026-10-07)
- [Binder registration ordering checkpoint (2026-10-07)](../archive/ANDROID_RUNTIME-history.md#binder-registration-ordering-checkpoint-2026-10-07)
- [Managed audio checkpoint (2026-10-07)](../archive/ANDROID_RUNTIME-history.md#managed-audio-checkpoint-2026-10-07)
- [First SpongeBob rendering milestone (2026-10-07)](../archive/ANDROID_RUNTIME-history.md#first-spongebob-rendering-milestone-2026-10-07)
- [Native sensor-service profile (2026-10-07)](../archive/ANDROID_RUNTIME-history.md#native-sensor-service-profile-2026-10-07)
- [ART field lookup restart correction](../archive/ANDROID_RUNTIME-history.md#art-field-lookup-restart-correction)
