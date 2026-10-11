# Android host integration

Read the [runtime](../../../Docs/Android/ANDROID_RUNTIME.md) and
[Binder](../../../Docs/Android/ANDROID_BINDER.md) guides for service/ABI contracts.
Guest Java services and native adapters live in `Guest/Android/`.

- Binder receive arenas use a free low-window hole after ART's high reservations.
  Preserve process/realm identity, fd ownership and registry transaction flags;
  host pointers never stand in for cross-process guest pointers.
- Loopers are per guest thread, keyed by CPU tid. Choreographer callbacks run
  through the owner's poll and monotonic frame ticks. Managed Java/native
  consumers must share the image's actual Looper where required.
- Framework service stubs are strict: add the exact observed method contract,
  not a blanket default. Preserve package query filtering/copy isolation and
  per-Activity manifest/requested orientation.
- Managed connectivity presents the host route as Wi-Fi. VALIDATED requires
  the launcher's HTTPS probe returning 204 or its explicit diagnostic override.
- Keep Android's unmapped null page, real shared/private file mappings and
  userfaultfd CMC GC behavior; see the memory guide. Managed AAudio requires
  Bionic thread callbacks; see the audio guide.
- Run framework contracts and Bionic probes in both engines after boundary
  changes. Installed-app creation, surface startup and rendering checkpoints
  cover different behavior; report the checkpoint actually reached.
