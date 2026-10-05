# Signed APK installation and native launch

Checkpoint: October 5, 2026. Bifrost installs signed APK sets and executes a
native-only `android.app.NativeActivity` through the real guest Bionic linker.
The regression APK uses split assets, pthread/TLS, persistent storage, EGL
pixel readback and an actual Binder service call. Managed Activities still
require guest ART and Android framework startup.

## Setup and use

Provision Python 3, Clang/LLD, `debugfs` from e2fsprogs and a JDK containing
`java` and `keytool`. The signing and manifest tools are checksum-pinned AOSP
downloads; no full Android SDK installation is required.

```bash
make -j6 USE_SDL2=1 USE_THUNK_GL=1
python3 tools/fetch-android-runtime.py
python3 tools/fetch-aapt2.py
python3 tools/fetch-android-package-tools.py
make android-runtime-adapters android-window-plugin

python3 scripts/android_package.py install /path/to/package.apkm
python3 scripts/android_package.py run org.example.package

# Use the existing loadable window backend instead of the built-in backend.
python3 scripts/android_package.py run org.example.package \
    --android-window-plugin build/release-sdl1-gl1/plugins/android-window-sdl.so
```

Installation accepts a single APK or a ZIP/APKM/APKS containing one consistent
base/split set. It does not select among bundletool device variants. Use
`--state-root PATH` before `install` or `run` to choose a different state directory.
`run --no-jit` uses the interpreter; `--frames N` bounds redraw cycles for tests,
and zero runs until the window closes. Bifrost flags precede its guest ELF.

## Package and runtime ownership

The installer snapshots the bounded input, checks archive paths/member types,
verifies every APK with [official apksigner](https://developer.android.com/tools/apksigner),
and requires matching signer certificates, package/version identities and split
dependencies. It rejects duplicate/conflicting native providers and attempts
to replace runtime or adapter libraries. Input and aggregate expanded APK size
are capped at 2 GiB. Updates require the same signer identity and cannot
downgrade; certificate-rotation lineages are not implemented.

Each package has an independent rootfs under ignored `android-state/`:

- `/data/app/<package>-<digest>` retains signed APKs, extracted ARM64 libraries
  and merged assets. Code is published by rename and made read-only.
- `/data/user/0/<package>/files` and `cache` retain application data.
- `/storage/emulated/0/Android/data/<package>/files` provides external app storage.
- The root contains copied, verified Bionic libraries and guest adapter DSOs.

An install lock and atomic metadata publication keep failed validation from
replacing the active package. Reinstalling a reordered set is idempotent and
preserves saves. Launch verifies stored package/runtime/adapter hashes before
execution. This layout uses Bifrost's VFS root; it is not a validated host
security sandbox or a complete Android permission/UID model. Concurrent app
instances share their package's data directories.

The guest `bifrost-native` launcher calls `dlopen` through Bionic and supplies a
guest-owned NDK LP64 Activity and callback table. Guest linker64 owns relocation,
constructors, dependency loading and TLS. The API-35 linker configuration uses
an isolated app namespace; APK code resides in its permitted `/data/app` tree.
The pinned runtime's provenance and 4 KiB pages/16 KiB ELF alignment are covered
in [ANDROID_RUNTIME.md](ANDROID_RUNTIME.md). This does not establish a complete
API-35 platform implementation.

Guest `libandroid`, `liblog`, `libEGL` and `libGLESv2` adapters resolve explicit
exports into Bifrost's existing thunks. Assembly wrappers preserve AAPCS64
arguments and cache trampoline addresses with acquire/release operations.
Graphics remain behind the [window plugin interface](ANDROID_WINDOW_PLUGINS.md).
Private syscalls `0x100D` (adapter resolution) and `0x100E` (event pump/input
queue) require the Android runtime profile. They are not Android kernel APIs.

## Compatibility boundaries

Only the exact `android.app.NativeActivity` launcher class is executable here.
`JavaVM`, `JNIEnv` and the Java Activity object are null; apps must avoid JNI.
The launcher invokes startup, window/input, redraw and teardown callbacks.
It does not implement full Activity lifecycle transitions, resource-table
resolution, Java class loading, DEX execution or Android property services.

Asset file open/read/seek/length/buffer/close work on extracted files, capped
at 64 MiB per asset. Directory enumeration, asset file descriptors and OBB
mounting are not implemented. Graphics and platform exports cover the existing
thunk subset; the adapter route does not establish arbitrary NDK or audio
compatibility. Missing adapter exports fail explicitly.

`--android-services` enables the bounded service-manager subset described in
[ANDROID_BINDER.md](ANDROID_BINDER.md). No SurfaceFlinger, ActivityManager,
PackageManager or other framework service is supplied by this checkpoint.

The user-provided SpongeBob 5.11.5 set installs successfully: nine signed APKs
and 21 ARM64 libraries. Launch rejects
`com.clevertap.unity.CleverTapOverrideActivity` before executing the emulator
because it requires ART/framework startup. Installation is not game-play
compatibility evidence.

## Verification

```bash
make test-android-package
./scripts/run_tests.sh --strict --filter android_service_manager
./scripts/run_tests.sh --strict --no-jit --filter android_service_manager
```

The APK gate requires a native Wayland desktop and GPU. It builds and signs a
base/split NativeActivity fixture with actual AAPT2/API-35 manifest resources
and apksigner. All six JIT/interpreter/Tier-2 and built-in/plugin combinations
pass lifecycle, pthread/TLS, split-asset, save persistence, EGL pixel readback
and service-handle invocation checks. The six host test cases also cover path
and symlink rejection, input growth bounds, signature tampering, signer mismatch,
version mismatch, downgrade prevention, reordered reinstall, modified installed
code, and managed-launcher rejection. The fixture does not test ART or JNI.

The complete Linux regression suite passes 273/273 JIT runs and 266/266
interpreter runs with seven JIT-only skips. The real Bionic bootstrap separately
passes all six fixed/random ASLR and execution-mode configurations.
