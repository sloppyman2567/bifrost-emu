# Shared runtime and host integration

See [the source map](../../Docs/ARCHITECTURE.md) for subsystem entry points.
`Core/Emulator/` owns startup/lifetime, `Memory/` guest mappings, `Loader/`
ELF/dynamic linking, `Syscalls/` Linux ABI translation, `VFS/` guest paths and
devices, `Android/` Android host integration, and `Graphics/`/`Audio/` host APIs.

- Preserve the guest/host boundary: guest handles and addresses must round-trip
  through guest memory; host pointers are not valid guest pointers. Keep existing
  bounce/writeback and mapping lifetime contracts.
- Keep file mapping identity, discard/remap semantics, TLS layout, userfaultfd
  state, signal recovery and Binder arena ownership as specified in the scoped guides.
  A source split must preserve file-local callback state and initialization order.
- Private `*Internal.hpp` headers stay within their subsystem. Public embedding
  interfaces are under `Include/Bifrost/` and `Source/System/API/`.
- Android guest shims/services live in `Guest/Android/` (see the Android guide
  for its subfolders); adapter provisioning
  reads the audio registry in `Audio/AudioThunk/AudioThunk.cpp`. When moving registry
  definitions, update `Tools/Android/build-android-adapters.py` too.
- Validate both engines and the affected host/guest boundary. Android AAudio and
  OpenSL ES need the managed Bionic tests, not just host audio checks.
  Display tests need GPU access; smoke frames do not establish game playability.

## Scoped contracts

- [Memory](Memory/AGENTS.md): mappings, discard/remap, userfaultfd and null pages.
- [Loader](Loader/AGENTS.md): ELF identity, library selection and thread TLS.
- [Graphics](Graphics/AGENTS.md): trampolines, guest handles and mapped buffers.
- [Audio](Audio/AGENTS.md): callback ownership and Bionic adapter wiring.
- [Android](Android/AGENTS.md): Binder, loopers, surfaces and service contracts.
- [Syscalls](Syscalls/AGENTS.md): process exit, ABI argument translation and signals.
- [VFS](VFS/AGENTS.md): host confinement and synthetic device/proc/sys views.

`Emulator::group_exit` stops every guest CPU, wakes blocking operations and
preserves the requested exit status. Fatal signals go through
`Emulator::signal_exit`. SIGKILL is not cooperative: it flushes stdio and
quick-exits with 137 (terminal restore via `at_quick_exit`). Crash handling must use the shared reporter
and terminate coherently; a worker failure must not leave the main CPU running.
