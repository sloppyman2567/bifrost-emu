# Android window plugins

`AndroidWindowBackend` is the internal host-side C++ interface. The default
`sdl-egl` implementation wraps the existing DisplayProxy window and resolves
its X11 native window or Wayland EGL wrapper. AndroidSurfaceManager owns the
backend and retains guest handles, reference bookkeeping, activity callbacks
and input delivery. The backend receives no guest Memory or CPU capability.

Install a backend with `install_window_backend()` before attaching a host
window or issuing any guest surface handle. Live replacement is rejected:
changing a backend would invalidate existing EGL surfaces. The host window is
borrowed; its owner detaches it before destruction. The SDL implementation
releases its Wayland EGL wrapper at detach, before SDL destroys the wl_surface.
Its helper-library reference is owned by that backend and released after wrapper
destruction, including when a shared-object plugin unloads. Failed bindings
return an empty window/display pair and can recover on a later attempt.
Window and display are resolved together so Wayland EGL uses one connection.
Attach, resize and EGL binding operations belong to the display thread.

Attach and resize return explicit errors. A failed attach preserves the current
borrowed window; a failed resize preserves the reported dimensions and format.
Negative dimensions or format are rejected. Width and height must both be zero
or both positive; zeros restore the adapter's base 800×600/RGBA8888 settings,
following the [NDK geometry contract](https://developer.android.com/ndk/reference/group/a-native-window#anativewindow_setbuffersgeometry).
This single-window adapter also resizes the host window; it does not yet separate
physical window dimensions from Android buffer dimensions or negotiate buffer
formats. Detach is infallible and idempotent. Independent adapter contexts can be
constructed with an owned backend; their destruction detaches before destroying
it. The emulator's existing thunk/activity wiring still uses its singleton
context, so this does not establish multiple simultaneous Android runtimes.

## Loadable host modules

The public C header [android_window_plugin.h](../include/bifrost/android_window_plugin.h)
defines host ABI version 1. A plugin exports
`bifrost_android_window_plugin_get_api(uint32_t host_abi)` and returns an immutable
descriptor with its version, size, name and create/destroy/attach/detach/resize/
binding callbacks. The loader checks the entire required prefix before creating
an instance. Status callbacks return zero or a negative errno; invalid positive
or out-of-range status values become `-EPROTO`. Binding errors discard the whole
window/display pair. Plugins must preserve state on failed attach/resize, avoid
reentry and never throw across the C boundary.

Load a module explicitly before `--android` or the guest ELF:

```bash
make android-window-plugin
./bifrost-emu --android-window-plugin ./build/release-sdl1-gl1/plugins/android-window-sdl.so \
  ctest_real/test_android_surface.elf
```

The optional SDL/EGL module is built from `plugins/android-window-sdl/plugin.cpp`
and the same backend implementation as the built-in default. It exports only
the C entry point and does not link libbifrost. Leaving the option unset uses
the built-in backend. `--android` consumes subsequent arguments as guest arguments,
so place the plugin option before it. Host help/config-only requests do not load
it, including config-only requests on the `--android` launch path. Guest arguments
named `--print-config` or `--config` are not parsed as host options.

Module paths are host filesystem paths containing `/`, with no guest soname
search or rootfs remapping. The loader uses `RTLD_NOW | RTLD_LOCAL` so missing
dependencies fail during loading and plugin symbols are not made globally
available; see [dlopen semantics](https://man7.org/linux/man-pages/man3/dlopen.3.html).
These are trusted native host modules, not sandboxed guest code. No guest
Memory/CPU capability or C++ object crosses the public interface. Destroy handles
partially created instances after creation failure. The module remains loaded
until successful instances have detached and all plugin destruction callbacks
have returned.

This checkpoint extracts window handling. It does not yet provide a general
Binder service plugin registry, framework Surface/BufferQueue parcel handling,
multiple Android surfaces or an ABI for complete Android platform plugins.
Version 1 covers only host window binding and geometry.
Presentation remains in the existing EGL thunk (`eglSwapBuffers`); plugins
provide the native binding that it uses. SDL input remains in the activity
adapter. Android runtime mode still needs guest platform shim DSOs before it
can use this bridge through the real Bionic linker.

## Reproduction

```bash
make -j6
make test-android-window-backend
make test-android-window-plugin
make test-android-window-plugin-cli # requires the cross-built ctest/hello.elf
make test-android-sdl-backend # SDL headers; fake SDL boundary, real dlopen lifetime
make android-window-plugin
make cross SRC=ctest_real/test_android_surface.c OUT=ctest_real/test_android_surface.elf
./scripts/run_tests.sh --filter 'android_surface|android_activity|sdl_gl_triangle'
./scripts/run_tests.sh --no-jit --filter 'android_surface|android_activity|sdl_gl_triangle'
bash scripts/run_android_surface_demo.sh
SDL_VIDEODRIVER=wayland ./bifrost-emu ctest_real/test_android_surface.elf
SDL_VIDEODRIVER=wayland ./bifrost-emu --no-jit ctest_real/test_android_surface.elf
SDL_VIDEODRIVER=wayland ./bifrost-emu \
  --android-window-plugin ./build/release-sdl1-gl1/plugins/android-window-sdl.so \
  ctest_real/test_android_surface.elf
```

Host tests check delegation, failure rollback, independent context teardown and
replacement rejection after attachment or handle issuance. A separately compiled
C shared object verifies descriptor rejection, partial-create cleanup, poisoned
binding rejection and detach/destroy-before-unload ordering. Both host targets
also run without SDL and are included in CI, `make ci` and `make check-all`.
The SDL helper-lifetime oracle stubs SDL through its actual public declarations
and loads an independently compiled helper DSO. It checks concurrent backend
references, wrapper-before-helper destruction and recovery after WM query or
wrapper allocation failure. This runs in CI's SDL build job without a compositor.
The ARM64 surface fixture creates a real EGL window, reads back its clear color, presents it, checks
geometry and destroys its EGL objects. Demo mode displays changing colors for
ten seconds. This is a musl-static ARM64 adapter fixture, not an Android APK
or a test of the real Bionic linker and Android framework.

Native Wayland verification: forcing `SDL_VIDEODRIVER=wayland` passes all 33
surface checks in both JIT and interpreter mode, including EGL window creation,
GPU pixel readback, presentation, geometry error/reset semantics and teardown.
Both the built-in backend and the loadable SDL/EGL module pass these checks.
SDL cannot select its X11 backend in these runs. This does not validate the downloaded Mesa demos.

The plugin refinement passes both host contract targets in SDL and headless
builds, plus all twelve CLI startup/help/error cases in each build. Headless SDK
independence passes. Focused Android/Binder/syscall, SDL triangle, socket and
XWayland regressions pass 10/10 in both engines. The separate pinned Bionic
bootstrap still passes all six fixed/random ASLR and execution configurations.
The prior full-suite checkpoint below predates these plugin changes.

## Independent downloaded programs

`bash tools/fetch-graphics-smoke.sh` downloads Debian's
`mesa-utils-bin_9.0.0-2+b3_arm64.deb` from the official Debian mirror, verifies
SHA-256 `0027480ef4dca9b70d004aad29005c0a89872196c39ccbeb4ae4946b9064b24e`, and
extracts its ARM64 EGL/GLES/X11 examples under ignored `tools/graphics-smoke`.
The package includes its copyright/license information. It uses glibc rather
than Bionic. Example invocation:

```bash
BIFROST_ROOT="$PWD/rootfs" ./bifrost-emu \
  tools/graphics-smoke/mesa/usr/bin/es2tri.aarch64-linux-gnu
```

Initial runs of `es2tri` and `glxgears` both exited with an X connection error.
That investigation exposed BFM mask corruption and unsafe cross-block NZCV
elision in the JIT; both are fixed with regressions. The real guest libX11
fixture `ctest_real/xwayland_wire.c` opens an XWayland window, draws through the
socket protocol, completes 100 round-trips and checks server pixel readback.
It passes JIT, interpreter and QEMU against the same guest libraries.

The Mesa demos still do not render: `es2tri` now fails at `eglGetDisplay`, while
`glxgears` reaches a null-PC crash. Those remaining graphics integration failures
are separate from the repaired X11 protocol path. The Android surface fixture
remains the native Wayland rendering acceptance check.

Build the wire test with the ARM64 glibc toolchain and guest libX11 (host X11
development headers supply the architecture-independent public declarations):

```bash
tools/aarch64-linux-gnu-cross/bin/aarch64-none-linux-gnu-gcc -O2 \
  -idirafter /usr/include ctest_real/xwayland_wire.c -Lrootfs/usr/lib \
  -Wl,-rpath-link,rootfs/usr/lib -l:libX11.so.6 -o ctest_real/xwayland_wire.elf
./scripts/run_tests.sh --filter xwayland_wire
```

The socket investigation also corrected `recvmsg`'s LP64 control-length/flags
offsets and bounded copyback to bytes received. `socket_msg_layout` checks
truncation flags and untouched tails after a short scatter receive. Descriptor
translation for ancillary `SCM_RIGHTS` messages remains outside this repair.

At the XWayland repair checkpoint, the complete suites pass 272/272 in JIT
mode and 265/265 in interpreter mode (seven JIT-only tests skipped). BFM and
flag-passthrough oracles also pass with Tier-2 enabled; BFM passes differential
verification. The native Wayland Android surface test still passes 25/25.
