# Built-in Android window plugins

`AndroidWindowBackend` is an injectable host-side C++ interface. The default
`sdl-egl` implementation wraps the existing DisplayProxy window and resolves
its X11 native window or Wayland EGL wrapper. AndroidSurfaceManager owns the
backend and retains guest handles, reference bookkeeping, activity callbacks
and input delivery. The backend receives no guest Memory or CPU capability.

Install a backend with `install_window_backend()` before attaching a host
window or issuing any guest surface handle. Live replacement is rejected:
changing a backend would invalidate existing EGL surfaces. The host window is
borrowed; its owner detaches it before destruction. The SDL implementation
releases its Wayland EGL wrapper at detach, before SDL destroys the wl_surface.
Window and display are resolved together so Wayland EGL uses one connection.
Attach, resize and EGL binding operations belong to the display thread.

This checkpoint extracts window handling. It does not yet provide a general
Binder service plugin registry, framework Surface/BufferQueue parcel handling,
multiple Android surfaces, external shared-object loading or a stable C ABI.
Presentation remains in the existing EGL thunk (`eglSwapBuffers`); plugins
provide the native binding that it uses. SDL input remains in the activity
adapter. Android runtime mode still needs guest platform shim DSOs before it
can use this bridge through the real Bionic linker.

## Reproduction

```bash
make -j6
make test-android-window-backend
make cross SRC=ctest_real/test_android_surface.c OUT=ctest_real/test_android_surface.elf
./scripts/run_tests.sh --filter 'android_surface|android_activity|sdl_gl_triangle'
./scripts/run_tests.sh --no-jit --filter 'android_surface|android_activity|sdl_gl_triangle'
bash scripts/run_android_surface_demo.sh
SDL_VIDEODRIVER=wayland ./bifrost-emu ctest_real/test_android_surface.elf
SDL_VIDEODRIVER=wayland ./bifrost-emu --no-jit ctest_real/test_android_surface.elf
```

The host test installs a second backend, checks delegation and rejects backend
replacement after attachment or handle issuance. The ARM64 surface fixture
creates a real EGL window, reads back its clear color, presents it, checks
geometry and destroys its EGL objects. Demo mode displays changing colors for
ten seconds. This is a musl-static ARM64 adapter fixture, not an Android APK
or a test of the real Bionic linker and Android framework.

Native Wayland verification: forcing `SDL_VIDEODRIVER=wayland` passes all 25
surface checks in both JIT and interpreter mode, including EGL window creation,
GPU pixel readback, presentation, dimensions and teardown. SDL cannot select its
X11 backend in these runs. This does not validate the downloaded Mesa demos.

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
