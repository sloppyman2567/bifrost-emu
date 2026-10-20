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
- App processes register their code and profile paths with ART
  (`registerProfiles`) and run from `dexopt`'s install-time odex. Keep the
  odex accepted: `PCL[]` context, the image's boot classpath and ISA props.
  `MANAGED_FIXTURE_ODEX`/`_PROFILE_SAVER` in `test_android_package.py` check it.
- Settings defaults are generated from pinned AOSP inputs with
  `Tools/Android/generate-settings-defaults.py`; regenerate rather than edit
  `FrameworkSettingsDefaults.java`. Preserve System overrides through runtime
  replacement and keep write grants, validators, cache generations and
  observer notifications consistent. PendingIntent identity excludes extras;
  broadcasts must use the image's ActivityThread receiver signatures and
  protected-action declarations. Keep unsupported cross-process and
  foreground-service behavior explicit.
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
- Input events keep `System.nanoTime()` (CLOCK_MONOTONIC) nanoseconds, as
  the NDK `AKeyEvent`/`AMotionEvent` time getters return them; the managed
  launcher converts to framework milliseconds. Window plugins report
  monotonic milliseconds; backends rebase their own clocks onto it.
- Window backends report one pointer per event under an opaque `pointer_id`
  key. `AndroidSurfaceManager` alone owns gesture state and assigns Android
  pointer ids and `POINTER_DOWN/UP` actions. The 55-word op-3 record is
  shared by `HostCalls/Android.cpp`, `activity_window.c` and
  `ManagedLauncher`; change all three together.
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
  ActivityThread's lifecycle order. Hardware-accelerated Activity windows
  draw through their ViewRootImpl's own ThreadedRenderer into a host layer
  (`FrameworkWindows.drawHardware`); each window's SurfaceViews are restacked
  directly below it (host op 19). A host surface is owned only by its
  preparation (released with `bifrost_surface_release`) and its host EGL
  surfaces. Never mirror app `ANativeWindow` references to it, because HWUI
  releases its windows through libnativewindow. Dialogs, menus and dim layers stay
  software popup layers above every surface. Only a window's
  `ViewRootImpl.mThread` may touch its views: windows owned by other threads
  (Unity's `UnityMain` dialogs) draw, take input and get SurfaceHolder
  callbacks on that Looper (`drawForeign`, `onOwner`, `updateSurfaces`);
  window layers stay on `main`. `FrameworkSurfaces` guards its entries and
  their host calls with one lock, never held across app callbacks. Deliver
  `surfaceDestroyed` while the Surface is valid (window detach, `stop`). An Activity covered by an
  opaque one must still stop and hide its window
  (`EnsureActivitiesVisibleHelper`); only translucent/floating themes leave it
  visible. Only the session process publishes device
  services (`sensorservice`, `SurfaceFlingerAIDL`, `SurfaceFlinger`).
- Managed connectivity presents the host route as Wi-Fi. VALIDATED requires
  the launcher's HTTPS probe returning 204 or its explicit diagnostic override.
- Keep Android's unmapped null page, real shared/private file mappings and
  userfaultfd CMC GC behavior; see the memory guide. Managed AAudio requires
  Bionic thread callbacks; see the audio guide.
- Run framework contracts and Bionic probes in both engines after boundary
  changes. Installed-app creation, surface startup and rendering checkpoints
  cover different behavior; report the checkpoint actually reached.
