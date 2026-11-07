# Android host integration

Read the [runtime](../../../Docs/Android/ANDROID_RUNTIME.md) and
[Binder](../../../Docs/Android/ANDROID_BINDER.md) guides for service/ABI contracts.
Guest-side code lives in `Guest/Android/`: native adapters in `Adapters/`,
launchers in `Launchers/`, the managed launcher and Java services in `Managed/`
(services in `Managed/Framework/`). JNI natives bind by class name
(`Java_ManagedLauncher_*`), so keep native methods declared in `ManagedLauncher`.

- Binder receive arenas use a free low-window hole after ART's high reservations.
  Preserve process/realm identity, fd ownership and registry transaction flags;
  host pointers never stand in for cross-process guest pointers.
- Loopers are per guest thread, keyed by CPU tid. Choreographer callbacks run
  through the owner's poll and monotonic frame ticks. Managed Java/native
  consumers must share the image's actual Looper where required.
- Framework service stubs are strict: add the exact observed method contract,
  not a blanket default. Preserve package query filtering/copy isolation and
  per-Activity manifest/requested orientation.
- Device services model one fixed device (see the runtime guide's common
  device services): their answers, the install-time permission grants and the
  configurations must agree. Process, Activity and bind configurations take
  `uiMode` from `FrameworkUiMode.CONFIGURATION_UI_MODE`. New `Framework*.java`
  modules go in `LAUNCHER_SOURCES` (`Scripts/Android/android_managed.py`).
- microG (`Tools/Android/android_microg.py`, pinned) is a privileged system
  package set in every managed root: GmsCore UID 19998, Play Store companion
  UID 19999, reserved in `android_sessions.app_id`. `FrameworkPackages` applies
  microG's `fake-signature` rule to both `signatures` and `signingInfo`; keep
  it limited to packages declaring the meta-data and permission. Their
  archives load lazily (`installSystemLazily`): app startup must not pay
  the GmsCore parse. Children get the parent's already parsed snapshots
  (`FrameworkProcessRuntime.SHARED_SYSTEM_PACKAGES`) and the session app as
  a peer package. microG runs in managed children: providers in other
  processes go through `FrameworkContent.acquireRemote` (only the session
  starts processes; children forward), and children install their process's
  providers before `Application.onCreate`. Keep provider publication to
  names and Binders: holders are rebuilt from the session's manifest records.
  `test_managed_play_services_available` runs Google's own basement check,
  a microG service bind, a GServices query and the chimera lookup. microG's
  `AdRequestService` stub is never bound (`FrameworkServiceRuntime`), so
  Google Mobile Ads uses its local request path; see the runtime guide.
- Accounts: `FrameworkAccounts` is Android 15's AccountManagerService for one
  user, one instance per guest process over `FrameworkAccountStore`'s shared
  JSON file (`/data/system/users/0/bifrost-accounts.json`; lock, atomic replace,
  kept across root replacement). Identity is `Process.myUid()`, never
  `Binder.getCallingUid()`: these services are in-process and an authenticator
  serves transactions under the session's uid. Authenticator binds go through
  `FrameworkServiceRuntime.SYSTEM_BIND`; keep it set only around that
  `bindService` call. The session app's uid holds `ACCOUNT_MANAGER` for
  authenticator-side checks. Keep unsupported operations throwing
  (`UnsupportedOperationException`) or answering `ERROR_CODE_UNSUPPORTED_OPERATION`,
  never succeeding silently. `Scripts/Android/android_google.py` runs microG's
  sign-in UI as a session of the provisioned system app (`metadata.system`,
  UID 19998, no dexopt, `runApp` uses the lazily parsed system `PackageInfo`) and
  shares accounts across roots through `<state>/.google` (import before each
  managed launch, publish after; microG settings stay per root). Run
  `test_managed_accounts_service` and `GoogleAccountStateTests`. Activities of
  `com.google.android.gms` that an app starts never load into the app:
  `FrameworkRemoteActivities` hands the marshalled Intent to the supervisor
  (`ProcessHost` operation `activity`, `android_google.ActivityRelay`), which
  runs it as that system app's session in its own window and returns the
  result. The session's root reports the app as its caller and finishes on
  Back; the app's `AccountAuthenticatorResponse` never crosses (a stand-in is
  relayed). A manifest receiver declared for another process runs there through
  `FrameworkProcessRuntime.receiver` (oneway, the process stays cached); keep it
  unordered and keep the component on the delivered Intent. Keep the request path validation and the always-written
  `result.json` (the guest polls for it). Host code reads and copies
  guest-writable files only through `android_google.open_regular` (no symlinks,
  regular files, bounded size) and writes into an app's request directory by
  replacing, never through, what is there: an app is untrusted. Run `test_managed_google_activity_window`.
- The window-stats overlay (`pump_host_events`) is the window backend popup `0xFFFF0001` (guest popup ids count up
  from 1), drawn through the locked `show_popup`: translucent, never an input target, at most four updates a second,
  and asked once of a backend that returns `-ENOTSUP`. Test with the recording backend in
  `Tests/Unit/android_window_backend_host.cpp`.
- `media_session` (`FrameworkMediaSessions`) has no sessions and accepts only
  `dispatchMediaKeyEvent`; WebView forwards unhandled keys to it.
- App processes register their code and profile paths with ART
  (`registerProfiles`) and run from `dexopt`'s install-time odex. Keep the
  odex accepted: `PCL[]` context, the image's boot classpath and ISA props.
  `MANAGED_FIXTURE_ODEX`/`_PROFILE_SAVER` in `test_android_package.py` check it.
- Manifest parses of immutable archives go through `FrameworkParseCache`
  (`/data/local/tmp/package-cache` in the root): `getPackageArchiveInfo`
  results, filters and the platform's protected broadcasts. Add a new
  archive parse there; never cache an archive that can change under the same
  path, size and modification time. Run
  `test_managed_parse_cache_skips_manifest_parsing`.
- `FrameworkStallWatch` reads the main thread's stack from a daemon thread
  (no process-wide suspension) and stays bounded; keep any new diagnostic of
  that kind cheap and rate-limited rather than spamming the log.
- Web links an app starts open in the host browser only through
  `FrameworkActivities.browserLink` and `android_processes.open_url`: http(s)
  only, within 5 s of host input, one per second, never a shell. Anything else
  stays `START_INTENT_NOT_RESOLVED`; keep apps' fallbacks working. Another app's
  package in the Intent is never mapped to the browser.
- Settings defaults are generated from pinned AOSP inputs with
  `Tools/Android/generate-settings-defaults.py`; regenerate rather than edit
  `FrameworkSettingsDefaults.java`. Preserve System overrides through runtime
  replacement and keep write grants, validators, cache generations and
  observer notifications consistent. PendingIntent identity excludes extras;
  broadcasts must use the image's ActivityThread receiver signatures and
  protected-action declarations. Keep unsupported cross-process and
  cross-process behavior explicit: another process's IntentSender is
  `START_CANCELED` (callers handle `SendIntentException`), never a silent
  success or an unsupported-call crash.
- Foreground services are `FrameworkServiceRuntime` state (`setForeground`, with child processes over
  `SERVICE_FOREGROUND*`); their notification lives in `FrameworkNotifications`' in-memory store, which only the
  session's foreground services share across processes. Keep `ActiveServices` order (permission, type, policy, then
  state), and `stopForeground` on a non-foreground service a no-op. `ManagedServices` `foreground` checks both
  processes; the runtime guide lists what is not modelled.
- Battery state is the host's: `FrameworkBattery` reads `/sys/class/power_supply` (served read-only by the VFS) like
  healthd and broadcasts changes like BatteryService; a host without a battery is a full one on AC. `usagestats` and
  `netstats` (`FrameworkUsageStats`, `FrameworkNetworkStats`) keep no history or traffic: empty results, the services'
  permission gates, never invented counters. `batteryFromSupplies` and `usageAndNetworkStats` in `ManagedServiceBatch`
  cover them.
- A video SurfaceView layer is clipped to its view's visible rectangle (`FrameworkSurfaces.visibleClip`): convert and
  present only that part, show a transparent pixel when none shows, and keep host limits on the visible region, never on
  the view's full size. `test_managed_view_window_and_surface_lifecycle` checks the clip.
- Video frames are converted by the host (`Surface/Yuv420.cpp`, op 25), never per pixel in Java, and handled off the main
  Looper; a new frame path keeps `APP_SURFACE_FRAMES` honest. `Yuv420.cpp` is the one YUV converter (SurfaceView layers
  and the EGL buffer images of `Graphics/`): its AVX2 kernel is runtime-gated, must stay bit-identical to the scalar
  `pixel()` and never load past a plane's `size`. `make test-android-yuv420` covers all three (guard-page planes).
- Gralloc5 uses the realm-owned native allocator in `Services/GraphicsAllocator.cpp`
  and `Guest/Android/Adapters/gralloc_mapper.c`. Never publish it from an app:
  libui caches its Binder, which must survive other apps exiting. Preserve the
  sealed-size memfd ABI, independent imports, bounded metadata snapshots and fd
  ownership. Run `ActivityProbe$GrallocProbe` in both engines and with distinct
  UIDs sharing a broker; this does not establish EGL image import or decoding.
- Media decoding (`Media/`) uses the real Codec2 stack. The Binder broker
  owns the codec services, never an app. Treat Goldfish `user_priv` as
  opaque and never dereference it. Match the provisioned binaries'
  contract: VPX needs immediate output and packed YV12 at the decoded size.
  Change `HostDecoder`/`GoldfishMedia` together with
  `make test-android-goldfish-media`. Keep `--media-capabilities` limited to
  what it decodes. Do not report codec, Surface or playback readiness from
  64x64 probes.
- `media.player` and `media.resource_manager` are the image's real `mediaserver`, run by the broker as UID 1013 (`CodecServices`).
- `media.extractor` is the image's real `mediaextractor`, run by the broker as UID 1040 (`CodecServices`), its files
  pinned in `MEDIA_EXTRACTOR` (`android_framework.py`) and added in place. It aborts without its seccomp policy, which is
  installed; `prctl(PR_SET_SECCOMP)` accepts and ignores the filter, so it never reaches the host. It is not `required`:
  without it only video fails, and realm status reports it.
- Java-hosted native services run on the caller's thread, unlike a device's Binder pool, so a native thread reaching one
  would abort in `JavaBBinder::onTransact`. `libbifrost-binder-attach.so`, preloaded into app processes and hidden from
  their environment, interposes `BBinder::transact` to attach such a thread. It does not cover
  `JavaBBinder::getInterfaceDescriptor` on an unattached thread. `ActivityProbe$NativeChoreographerProbe` covers it.
- Input events keep `System.nanoTime()` (CLOCK_MONOTONIC) nanoseconds, as
  the NDK `AKeyEvent`/`AMotionEvent` time getters return them; the managed
  launcher converts to framework milliseconds. Window plugins report
  monotonic milliseconds; backends rebase their own clocks onto it.
- Window backends report one pointer per event under an opaque `pointer_id`
  key. `AndroidSurfaceManager` alone owns gesture state and assigns Android
  pointer ids and `POINTER_DOWN/UP` actions. The 55-word op-3 record is
  shared by `HostCalls/Android.cpp`, `activity_window.c`, `ManagedLauncher`
  and `FrameworkGamepads` (joystick axes in words 20..27, device records);
  change them together.
- Gamepads: backends report Android terms (`poll_gamepad`, an append-only plugin field; Xbox 360 key codes, the d-pad
  as hat axes); `AndroidSurfaceManager` owns device ids and controller numbers; `FrameworkGamepads` owns the
  `InputDevice`s, InputManager's listener, `Generic.kcm` fallbacks and the synthetic d-pad. Keep the ABI enums, the
  SDL mapping, the op-3 words and `FrameworkGamepads` together. Regenerate `FrameworkKeyLayout.java` (both tables)
  with `generate-key-layout.py`. Nothing in `ManagedLauncher`'s static initialization may need the main Looper. Run
  `make test-android-sdl-backend`, `make test-android-window-plugin` and `MANAGED_GAMEPADS_OK`
  (`ManagedServiceBatch`).
- The pad cursor (`Window/PadCursor.hpp`, SDL-free) takes the left stick, triggers, A and X while it is on, and Start+Select
  switch it. Whatever it takes it must give back: a chord sends the game the release of the Start or Select it already saw,
  and turning it on releases A and X and centres the stick and triggers for the game. Its events are a mouse's
  (`kMouseFinger`, `kMouseSecondFinger`) in app pixels. Run `make test-android-pad-cursor` and `make test-android-sdl-backend`.
- Wheel, trackpad pinch and middle-drag input become synthetic two-finger gestures in `Window/WheelGestures`
  (SDL-free; the SDL3 backend queues its events ahead of newer ones). Fingers never stay down: they lift on idle,
  button press, focus loss and close. Test with `make test-android-wheel-gestures` and
  `make test-android-sdl-backend` (the fake SDL3 in `Tests/Host/android_sdl3_fixture.c`).
- Host text input: an editor with a live `InputConnection` is bound to a host
  `IInputMethodSession` (`FrameworkInputMethod`; no input channel, so keys keep
  reaching views). The launcher starts the host's text input from its poll loop
  (op 20, on the window thread only) and delivers text events (op-3 record: kind
  in word 4, length in 13, bytes from 14) with `commitText`/`setComposingText`
  under the connection's current session id. Backends report text as an ordered
  `BIFROST_WINDOW_TEXT` event plus `poll_text` (append-only plugin fields); the
  SDL backend drops a key press that SDL followed with text, so nothing types
  twice. Keep the virtual keyboard's `Virtual.kcm` (type `FULL`) from
  `Tools/Android/generate-key-layout.py`; regenerate `FrameworkKeyLayout.java`,
  never edit it, or text fields stop honoring Backspace. Run
  `ActivityProbe$InputMethodProbe` and `test_managed_text_input`.
- Pointer icons: host input bypasses `ViewRootImpl`'s input stages, so after each mouse hover the launcher asks the
  Activity's view tree (`View.onResolvePointerIcon`, as `ViewRootImpl.updatePointerIcon` does) and `FrameworkCursors`
  sends the nearest `BIFROST_WINDOW_CURSOR_*` shape (op 24) only when it changes. Hover is the trigger because a
  touchscreen never hovers; a button-held drag keeps the last shape. `set_cursor` is an append-only optional plugin
  field (`-ENOTSUP` without it); the SDL3 backend maps shapes to SDL system cursors, frees them before SDL quits and
  shows a hidden pointer again on close. A failure stops the feature and logs `APP_CURSOR_UNAVAILABLE` once; it must
  never break input delivery. An app's own bitmap cursor is not carried. Keep `FrameworkCursors`, the C enum and
  the SDL mapping together; `MANAGED_CURSORS_OK` (`ManagedServiceBatch`), `make test-android-sdl-backend` and
  `make test-android-window-plugin` cover it.
- Shared libraries: the jars come from the pinned image (`SHARED_LIBRARIES` digests in `android_framework.py`, added in
  place to an existing runtime like the shell) and must stay read-only in every managed root, or ART refuses them. An
  app's `<uses-library>` is resolved in `FrameworkSharedLibraries` into `sharedLibraryInfos`; setting `sharedLibraryFiles`
  alone does nothing for a jar in Android 15. When a class "cannot be found" though it is in the APK, load it in the live
  class loader before theorizing: its real missing dependency is in the exception (`Class.forName(name, true, loader)`).
- Framework stub gaps: rank the `SERVICE_CALL_UNSUPPORTED <service> <call>` lines (once per process; sessions before
  2026-10-25 show an `UnsupportedOperationException` instead) across every app's `managed/sessions/*/stdout*.log`
  and implement the most common small call from its AOSP source, with a check in `ManagedServiceBatch` (both engines).
  A service declared by a `Contract` enum lists its methods in `methods()`, so grepping for a quoted name misses
  it. A call that needs a model, not a stub, is `ISurfaceComposer.createConnection` (the SurfaceControl layers); do not
  answer it with an empty success.
- A new host window is fitted once at `open` (`fit_new_window`: the compositor clamps it to the work area and changes
  its shape; the clamp is the work area limit). A fixed-size app's rotation takes the panel's shape within that limit
  and keeps its pixel dimensions whatever the user does to the window; never derive them from the window. A
  fixed-size app's window is never aspect-locked (a locked window cannot be tiled to a half or a corner): the fit
  centres the panel in any shape; only a resizable app that requests an orientation is locked. Apps
  declaring no orientation open landscape. `make test-android-sdl-backend` covers it.
- Play windows open in desktop units; the Android display is the window's
  native pixel buffer and density (op 11); windowConfiguration max bounds are
  the pixel size of the host display holding it (op 12), never smaller than
  the window, in both the process and Activity configurations. Layers created by Bifrost on Wayland
  must scale their pixel buffers to the parent's desktop coordinates
  (`SurfaceScale`, generated `wp_viewporter` glue; regenerate, don't hand-edit).
- Every window, the launcher Activity's included (dialogs, popups, stacked
  Activities from `FrameworkActivities`), goes through WindowManager and
  `FrameworkWindows`; SurfaceViews in any window get host surfaces from
  `FrameworkSurfaces` following SurfaceView's visibility contract;
  `FrameworkSurfaces`, not SurfaceView's own `updateSurface`, owns their
  surface lifecycle (see the runtime guide). Keep
  ActivityThread's lifecycle order. The task ends with its last Activity: a
  root that finishes after starting another (a splash screen) is destroyed
  and the next becomes root; launches posted by `startActivity` count as
  present. Hardware-accelerated Activity windows
  draw through their ViewRootImpl's own ThreadedRenderer into a host layer
  (`FrameworkWindows.drawHardware`); each window's SurfaceViews are restacked
  directly below it (host op 19). A host surface is owned only by its
  preparation (released with `bifrost_surface_release`) and its host EGL
  surfaces. Never mirror app `ANativeWindow` references to it, because HWUI
  releases its windows through libnativewindow. A window that has a renderer (dialogs and popup menus
  included) draws on the GPU, never in software: a software canvas refuses hardware bitmaps. A GPU window's dim is its own layer created just below it (`Popup.dimLayer`), never the software
  dim layer, which stacks above every GPU layer. Unaccelerated windows and toasts stay software popup layers above every
  surface. Only a window's
  `ViewRootImpl.mThread` may touch its views: windows owned by other threads
  (Unity's `UnityMain` dialogs) draw, take input and get SurfaceHolder
  callbacks on that Looper (`drawForeign`, `onOwner`, `updateSurfaces`);
  window layers stay on `main`. `FrameworkSurfaces` guards its entries and
  their host calls with one lock, never held across app callbacks. Deliver
  `surfaceDestroyed` while the Surface is valid (window detach, `stop`). An Activity covered by an
  opaque one must still stop and hide its window
  (`EnsureActivitiesVisibleHelper`); only translucent/floating themes leave it
  visible. Session processes publish the device
  services (`sensorservice`, `SurfaceFlingerAIDL`, `SurfaceFlinger`), their
  service children do not. Sessions in one realm all publish them, so the
  registry keeps the registration an owner replaced and restores it when that
  owner goes away (`BinderDomain::shadowed`).
- Managed connectivity presents the host route as Wi-Fi. VALIDATED requires
  the launcher's HTTPS probe returning 204 or its explicit diagnostic override.
- Keep Android's unmapped null page, real shared/private file mappings and
  userfaultfd CMC GC behavior; see the memory guide. Managed AAudio requires
  Bionic thread callbacks; see the audio guide.
- Run framework contracts and Bionic probes in both engines after boundary
  changes. Installed-app creation, surface startup and rendering checkpoints
  cover different behavior; report the checkpoint actually reached.

- `Package/PackageLauncher` is the only place `bifrost-emu app.apk` is
  recognized: it execs Asgard (it never installs or runs a package itself) and
  must drop `BIFROST_ROOT`. `Scripts/Testing/test_apk_launch.sh` checks it with
  a stub Asgard; keep its argument order and its search list (`asgard_candidates`)
  in step with `asgard.py` and `Scripts/Setup/install-asgard.py`.
- `Scripts/Setup/install-asgard.py` copies exactly what Asgard reads under its root
  (its `FILES` list); when a script or module starts reading a new root-relative
  path at run time, add it there. An installed tree is not writable at run time:
  nothing may write below `ROOT`, and state defaults come from
  `Scripts/Android/android_paths.py`. `test_asgard_install.py` covers the installer.

- `ProcessHost` starts every service child with `--asgard-service-parent <its pid>` from its single long-lived thread (the parent-death signal follows the spawning thread); keep both, or a killed supervisor leaves orphaned service processes. `asgard.broker_clients` treats `--service-child` processes as clients and other parent-bound helpers as not.
- Managed HOME activates Asgard through ProcessHost, then changes task visibility on the guest main thread. Preserve Activity instances and pause/stop/restart/resume order; old window plugins must return ENOTSUP for missing visibility callbacks.
