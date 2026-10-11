# Android window plugins

## Current interface (2026-10-05)

`AndroidWindowBackend` owns its host window, native EGL binding and input source.
The default `sdl3-egl` backend uses SDL3. Guest SDL2 thunks, SDL2 surface conversion,
Linux DisplayProxy and framebuffer/audio paths retain their existing SDL2 ABI.
AndroidSurfaceManager retains guest handles, reference bookkeeping, activity
callbacks and input delivery; the backend receives no guest Memory or CPU.
There is no borrowed `SDL_Window*` in the plugin interface.

Install before opening a window or issuing any guest surface handle. Live backend
replacement is rejected because it would invalidate existing EGL objects. Calls
belong to the display thread. The emulator still wires one singleton Android
runtime and one window; multiple concurrent runtimes and SDL event demultiplexing
between independent contexts are not implemented.

The SDL3 backend opens a window without an extra SDL renderer. On Wayland it owns
its `wl_egl_window` and keeps the helper DSO loaded until wrapper destruction;
on X11 it returns the window ID and its actual `Display*`. The native display
and window belong to the same connection. Failed binding resolution returns
an empty pair and can recover later. Close releases the wrapper before destroying
the host window, balances its SDL video-subsystem reference and is idempotent.
It uses `SDL_QuitSubSystem`, never process-global `SDL_Quit`.

SDL3 functions are resolved explicitly from `libSDL3.so.0` with `dlopen`/`dlsym`.
This avoids mixing SDL2 and SDL3 functions with overlapping symbol names; the
[upstream SDL2 compatibility implementation](https://github.com/libsdl-org/sdl2-compat/blob/main/src/sdl2_compat.c)
uses the same explicit SDL3 lookup approach. No SDL3 symbols are linked globally.
The [SDL3 window-properties API](https://wiki.libsdl.org/SDL3/SDL_GetWindowProperties)
supplies the native handles. SDL3 development headers >=3.2 are optional at
build time (`pkg-config sdl3`); a build without them, or a missing runtime DSO,
returns `-ENOSYS` when opening the default Android window. Other guest SDL2
paths continue to work. `USE_SDL3` defaults to `USE_SDL2` and can be selected
independently. `make USE_SDL2=0 USE_SDL3=0 USE_THUNK_GL=0` disables both.

Resize operations commit reported dimensions only after backend success,
including host resize notifications. Failed notifications are logged and preserve
the manager's previous geometry; the backend must likewise preserve its buffer
geometry. SDL3 resize failure is checked before modifying its EGL wrapper. Pixel geometry
is converted to SDL logical window dimensions on scaled displays.
Negative geometry is rejected; both dimensions must be zero or both positive.
Zeros restore the adapter's base 800×600 settings. The adapter supports only
RGBA8888: other positive formats now return `-ENOTSUP` instead of reporting
an unsupported format as successful. Separate Android buffer queues, format
negotiation and independent physical/buffer sizing are still absent.

## Loadable host modules

[AndroidWindowPlugin.h](../../Include/Bifrost/AndroidWindowPlugin.h) defines
**host ABI version 2**. Plugins export
`bifrost_android_window_plugin_get_api(uint32_t host_abi)` and return an immutable
version/size/name descriptor with create, destroy, open, close, resize, binding
and poll callbacks. Open owns or internally borrows a window; ownership never
crosses as an SDL object. Binding returns only native EGL handles. V1 binaries
are incompatible and rejected explicitly; rebuild plugins for V2. The old
SDL-pointer callbacks are not silently reinterpreted.

Status callbacks return zero or negative errno. Poll is nonblocking and returns
1 for an event, 0 for an empty queue, or negative errno. Events carry Android
keycodes/actions/meta bits, monotonic milliseconds and surface-pixel pointer
coordinates, without SDL enums or structs. V2 supports a single pointer per
event, not Android multitouch. The SDL3 adapter filters by its own window ID,
converts mouse coordinates to pixels and preserves modifier state. Event work
is bounded per pump. Invalid statuses, types, geometry, actions and nonfinite
pointer values become `-EPROTO`; error/empty outputs discard poisoned data.

The loader validates the descriptor before instance creation. Plugins must
preserve geometry on failed resize, leave a failed open closed, avoid reentry
and never throw across the C boundary. Partial create failures are destroyed.
Close/destroy finish before module unload; workers must stop/join during destroy.
Host module paths contain `/`, with no guest soname search or rootfs remapping.
Modules load with `RTLD_NOW | RTLD_LOCAL`. These are trusted native modules,
not a security sandbox.

```bash
make android-window-plugin
./bifrost-emu --android-window-plugin ./build/release-sdl1-gl1/plugins/android-window-sdl.so \
  Tests/Apps/test_android_surface.elf
```

The optional module shares the built-in SDL3 implementation, exports only the
C entry point and links neither libbifrost nor SDL2/SDL3. Place the option before
`--android` or the ELF. Help/config-only host requests do not load the module.
Presentation remains in the EGL thunk (`eglSwapBuffers`); input comes from the
backend's normalized event stream. This is a window interface, not a general
Binder/media/ART plugin ABI or framework Surface/BufferQueue implementation.
See [APK launch and limits](ANDROID_APK.md).

## Validation and reproduction

Completed checks:

- Host backend and C plugin contracts in SDL-enabled and headless builds,
  including open failure, geometry rollback, unsupported formats, legacy ABI
  rejection, invalid event rejection, partial-create cleanup and
  close/destroy-before-unload. Twelve CLI cases pass in each build.
- SDL3/helper DSO lifetime oracle, including failed creation, failed resize,
  binding recovery, modifier/pixel conversion and 256 teardown cycles. Its
  synthetic display has 2× pixel density and exercises logical/pixel sizing.
- Focused Android/SDL/EGL/Wayland regressions: 21/21 JIT and 21/21 interpreter
  with desktop/GPU access, after rebuilding the changed ARM64 surface fixture.
- Final signed APK suite: six unittest cases in 67.182 seconds. Real Bionic and
  Media NDK plus built-in/plugin × JIT/interpreter/Tier-2 native Wayland graphics
  pass with SDL3's `SDL_VIDEO_DRIVER=wayland` set explicitly.

The initial sandbox run blocked the cross compiler and reported the stale surface
fixture; restricted GPU access also produced Mesa errors in the framebuffer test.
The subsequent provisioned focused reruns passed. Concurrent Doom 3/Neverball/
Minecraft Weekend smoke attempts used software rendering inside that sandbox;
they do not establish hardware performance or a gameplay stability checkpoint.
After testing was authorized again, a separate bounded Neverball run initialized
OpenGL on the Radeon GPU with no logged fatal error. It was forcibly stopped
at the smoke-check deadline; this is startup/rendering evidence, not a clean
shutdown or sustained gameplay result.
Testing was stopped at the user's request and resumed only after authorization.
Final X11 Android rendering and a full-suite rerun are not claimed for this V2
checkpoint. Earlier full-suite numbers below describe older commits.

These commands open windows; run them only when desktop testing is wanted:

```bash
make -j6
make test-android-window-backend test-android-window-plugin-cli
make test-android-sdl-backend
make cross SRC=Tests/Apps/test_android_surface.c OUT=Tests/Apps/test_android_surface.elf
SDL_VIDEO_DRIVER=wayland ./bifrost-emu Tests/Apps/test_android_surface.elf
SDL_VIDEO_DRIVER=wayland ./bifrost-emu --android-window-plugin \
  ./build/release-sdl1-gl1/plugins/android-window-sdl.so Tests/Apps/test_android_surface.elf
```

## Independent downloaded programs

`bash Tools/Fetch/fetch-graphics-smoke.sh` downloads Debian's
`mesa-utils-bin_9.0.0-2+b3_arm64.deb` from the official Debian mirror, verifies
SHA-256 `0027480ef4dca9b70d004aad29005c0a89872196c39ccbeb4ae4946b9064b24e`, and
extracts its ARM64 EGL/GLES/X11 examples under ignored `Tools/graphics-smoke`.
The package includes its copyright/license information. It uses glibc rather
than Bionic. Example invocation:

```bash
BIFROST_ROOT="$PWD/rootfs" ./bifrost-emu \
  Tools/graphics-smoke/mesa/usr/bin/es2tri.aarch64-linux-gnu
```

Initial runs of `es2tri` and `glxgears` both exited with an X connection error.
That investigation exposed BFM mask corruption and unsafe cross-block NZCV
elision in the JIT; both are fixed with regressions. The real guest libX11
fixture `Tests/Apps/xwayland_wire.c` opens an XWayland window, draws through the
socket protocol, completes 100 round-trips and checks server pixel readback.
It passes JIT, interpreter and QEMU against the same guest libraries.

The Mesa demos still do not render: `es2tri` now fails at `eglGetDisplay`, while
`glxgears` reaches a null-PC crash. Those remaining graphics integration failures
are separate from the repaired X11 protocol path. The Android surface fixture
remains the native Wayland rendering acceptance check.

Build the wire test with the ARM64 glibc toolchain and guest libX11 (host X11
development headers supply the architecture-independent public declarations):

```bash
Tools/aarch64-linux-gnu-cross/bin/aarch64-none-linux-gnu-gcc -O2 \
  -idirafter /usr/include Tests/Apps/xwayland_wire.c -Lrootfs/usr/lib \
  -Wl,-rpath-link,rootfs/usr/lib -l:libX11.so.6 -o Tests/Apps/xwayland_wire.elf
./Scripts/Testing/run_tests.sh --filter xwayland_wire
```

The socket investigation also corrected `recvmsg`'s LP64 control-length/flags
offsets and bounded copyback to bytes received. `socket_msg_layout` checks
truncation flags and untouched tails after a short scatter receive. Descriptor
translation for ancillary `SCM_RIGHTS` messages remains outside this repair.

At the XWayland repair checkpoint, the complete suites pass 272/272 in JIT
mode and 265/265 in interpreter mode (seven JIT-only tests skipped). BFM and
flag-passthrough oracles also pass with Tier-2 enabled; BFM passes differential
verification. The native Wayland Android surface test still passes 25/25.
