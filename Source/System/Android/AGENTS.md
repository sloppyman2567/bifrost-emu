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
- Gralloc5 uses the realm-owned native allocator in `Services/GraphicsAllocator.cpp`
  and `Guest/Android/Adapters/gralloc_mapper.c`. Never publish it from an app:
  libui caches its Binder, which must survive other apps exiting. Preserve the
  sealed-size memfd ABI, independent imports, bounded metadata snapshots and fd
  ownership. Run `ActivityProbe$GrallocProbe` in both engines and with distinct
  UIDs sharing a broker; this does not establish EGL image import or decoding.
- Play windows open in desktop units; the Android display is the window's
  native pixel buffer and density (op 11); windowConfiguration max bounds are
  the pixel size of the host display holding it (op 12), never smaller than
  the window, in both the process and Activity configurations. Layers created by Bifrost on Wayland
  must scale their pixel buffers to the parent's desktop coordinates
  (`SurfaceScale`, generated `wp_viewporter` glue; regenerate, don't hand-edit).
- Every window, the launcher Activity's included (dialogs, popups, stacked
  Activities from `FrameworkActivities`), goes through WindowManager and
  `FrameworkWindows`; SurfaceViews in any window get host surfaces from
  `FrameworkSurfaces` following SurfaceView's visibility contract. Keep
  ActivityThread's lifecycle order. Only the session process publishes device
  services (`sensorservice`, `SurfaceFlingerAIDL`, `SurfaceFlinger`).
- Managed connectivity presents the host route as Wi-Fi. VALIDATED requires
  the launcher's HTTPS probe returning 204 or its explicit diagnostic override.
- Keep Android's unmapped null page, real shared/private file mappings and
  userfaultfd CMC GC behavior; see the memory guide. Managed AAudio requires
  Bionic thread callbacks; see the audio guide.
- Run framework contracts and Bionic probes in both engines after boundary
  changes. Installed-app creation, surface startup and rendering checkpoints
  cover different behavior; report the checkpoint actually reached.
