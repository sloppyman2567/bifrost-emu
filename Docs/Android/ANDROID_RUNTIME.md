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

When a broker cannot start, the error says how it ended (killed by a signal, or status 127 for a dynamic-loader failure) and
quotes the last lines of its `broker.log`; the same holds for the private broker of a launched app.

A realm with no app attached ends by itself: after `ASGARD_REALM_IDLE_SECONDS` (default 600; `0` keeps the realm until
`realm stop`) the broker stops its codec services and removes its socket, so an idle desktop holds no emulated runtimes.
The next launch starts a fresh realm, which pays the services' start and a new copy of their root, so a launch within the
grace period is the quick one. Only an app's Binder channels count as attached: control requests (`realm status`, a launch's
probe) restart the clock but never hold the realm open, and the broker's own codec services are ignored (they are its
children). The retired broker leaves its service root on disk until the next realm's sweep removes it. An `asgard.py` with an
emulator built before this option starts the realm without it and says so. The broker logs `ASGARD_BROKER_IDLE_EXIT` to
`<state>/.asgard/broker.log` when it ends.

`--debug CHANNELS` (before the command, like `WINEDEBUG`) turns on trace output:
`asgard.py --debug binder,futex run org.example.app`. Channels: `audio`, `binder`,
`dynlink`, `fault`, `frame`, `futex`, `input`, `media`, `mmap`, `poll`,
`process`, `thunk`, `x11` and `all` (the emulator's blanket `BIFROST_TRACE`).
Each sets the matching trace switches in `Source/System/Core/DebugFlags.hpp`;
only logging switches are mapped. Sessions started by the command inherit
them. A realm broker keeps the channels it was started with (`realm stop`, then
start it with `--debug`) so `binder` is seen only that way. An unknown channel
is rejected before anything runs.

The details of each part (windows, input, audio and media, accounts, framework services, launching) are in the
[runtime reference](ANDROID_RUNTIME_REFERENCE.md), which keeps the dated implementation and failure checkpoints. Later
checkpoints supersede earlier limits only for the contracts they cover; an old failure report is not automatically a
current compatibility claim. See [current support](../SUPPORT.md#android-scope-and-v20-plans),
[host media](ANDROID_RUNTIME_REFERENCE.md#host-media-decoding-2026-10-14) and the
[roadmap](../roadmap.md) for the latest scope and remaining work.


## How it fits together

- **One app, one root.** Asgard installs a signed APK or a split set and gives each app its own root under the state
  directory. The app runs through `bifrost-emu` on the real Android 15 pieces: Bionic, ART and the framework.
- **One realm per state root.** A shared Binder broker (the realm) owns graphics-buffer allocation and codec services, so an
  app's processes find each other and the services outlive one app. `asgard realm start|status|stop|restart` manages it, and
  an idle realm ends by itself.
- **A Java framework made of small modules.** Windows, packages, audio, accounts, jobs, notifications and the rest are
  service modules in the managed launcher. Native adapters connect Bionic's EGL, audio and surface libraries to the host.
- **The desktop is the display.** Windows, input, audio and video decoding go to the host through SDL3 or a window plugin
  ([plugins](ANDROID_WINDOW_PLUGINS.md)).
- **Google services come from microG,** installed as a privileged system package set in every root.

The parts below are described in the [runtime reference](ANDROID_RUNTIME_REFERENCE.md); the install and run commands are in the
[APK guide](ANDROID_APK.md).

## Where the details are

Each entry is a section of the [runtime reference](ANDROID_RUNTIME_REFERENCE.md), unchanged from where it was written.

**Windows and display**

- [GPU-rendered Activity windows](ANDROID_RUNTIME_REFERENCE.md#gpu-rendered-activity-windows-2026-10-17)
- [Windows owned by other threads](ANDROID_RUNTIME_REFERENCE.md#windows-owned-by-other-threads-2026-10-17)
- [Framework windows and SurfaceViews](ANDROID_RUNTIME_REFERENCE.md#framework-windows-and-surfaceviews-2026-10-13)
- [SurfaceView layers are clipped to what shows](ANDROID_RUNTIME_REFERENCE.md#surfaceview-layers-are-clipped-to-what-shows-2026-10-28)
- [Taken window surfaces and input queues](ANDROID_RUNTIME_REFERENCE.md#taken-window-surfaces-and-input-queues-2026-11-11)
- [Window visibility](ANDROID_RUNTIME_REFERENCE.md#window-visibility-2026-11-07)
- [Fullscreen and remembered size](ANDROID_RUNTIME_REFERENCE.md#fullscreen-and-remembered-size-2026-11-07)
- [Window shape](ANDROID_RUNTIME_REFERENCE.md#window-shape-2026-10-28)
- [The Window setting and resizing](ANDROID_RUNTIME_REFERENCE.md#the-window-setting-and-resizing-2026-11-07)
- [Activity relaunch](ANDROID_RUNTIME_REFERENCE.md#activity-relaunch-2026-11-07)
- [Native-density play windows](ANDROID_RUNTIME_REFERENCE.md#native-density-play-windows-2026-10-12)
- [Default bitmap density](ANDROID_RUNTIME_REFERENCE.md#default-bitmap-density-2026-10-28)
- [Colour scheme](ANDROID_RUNTIME_REFERENCE.md#colour-scheme-2026-11-08)
- [Managed display enumeration and listeners](ANDROID_RUNTIME_REFERENCE.md#managed-display-enumeration-and-listeners-2026-10-09)
- [Framework display events](ANDROID_RUNTIME_REFERENCE.md#framework-display-events-2026-10-07)
- [View dump on demand](ANDROID_RUNTIME_REFERENCE.md#view-dump-on-demand-2026-10-28)

**Input**

- [Input reaches windows through their input channels](ANDROID_RUNTIME_REFERENCE.md#input-reaches-windows-through-their-input-channels-2026-11-12)
- [Host text input](ANDROID_RUNTIME_REFERENCE.md#host-text-input-2026-10-20)
- [Pointer icons](ANDROID_RUNTIME_REFERENCE.md#pointer-icons-2026-10-28)
- [Gamepads](ANDROID_RUNTIME_REFERENCE.md#gamepads-2026-11-06)
- [Touch cursor](ANDROID_RUNTIME_REFERENCE.md#touch-cursor-2026-11-07)
- [Input methods](ANDROID_RUNTIME_REFERENCE.md#input-methods-october-11-2026)

**Audio, video and graphics buffers**

- [AudioTrack playback](ANDROID_RUNTIME_REFERENCE.md#audiotrack-playback-2026-10-21)
- [Managed OpenSL ES](ANDROID_RUNTIME_REFERENCE.md#managed-opensl-es-2026-10-13)
- [MediaPlayer and SoundPool](ANDROID_RUNTIME_REFERENCE.md#mediaplayer-and-soundpool-2026-11-04)
- [RenderScript and audio effects](ANDROID_RUNTIME_REFERENCE.md#renderscript-and-audio-effects-2026-11-04)
- [Host media decoding](ANDROID_RUNTIME_REFERENCE.md#host-media-decoding-2026-10-14)
- [YUV conversion kernel](ANDROID_RUNTIME_REFERENCE.md#yuv-conversion-kernel-2026-10-31)
- [Software Gralloc5](ANDROID_RUNTIME_REFERENCE.md#software-gralloc5-2026-10-13)
- [Saved pictures](ANDROID_RUNTIME_REFERENCE.md#saved-pictures-mediastore-images-2026-11-11)
- [Guest Vulkan HAL module](ANDROID_RUNTIME_REFERENCE.md#guest-vulkan-hal-module-2026-11-06)

**Google accounts, sign-in and credentials**

- [microG Play services packages](ANDROID_RUNTIME_REFERENCE.md#microg-play-services-packages-2026-10-16)
- [microG processes, remote providers and service resolution](ANDROID_RUNTIME_REFERENCE.md#microg-processes-remote-providers-and-service-resolution-2026-10-17)
- [Google accounts and sign-in](ANDROID_RUNTIME_REFERENCE.md#google-accounts-and-sign-in-2026-10-20)
- [Play Games sign-in answers 404](ANDROID_RUNTIME_REFERENCE.md#play-games-sign-in-answers-404-2026-10-21)
- [Android Keystore](ANDROID_RUNTIME_REFERENCE.md#android-keystore-2026-11-02)
- [Android app credentials](ANDROID_RUNTIME_REFERENCE.md#android-app-credentials-2026-10-09)
- [Web links and Play Store links](ANDROID_RUNTIME_REFERENCE.md#web-links-and-play-store-links-2026-10-20)

**Framework services**

- [Managed framework service modules](ANDROID_RUNTIME_REFERENCE.md#managed-framework-service-modules-2026-10-09)
- [Managed Android Service execution](ANDROID_RUNTIME_REFERENCE.md#managed-android-service-execution-2026-10-09)
- [Managed JobScheduler](ANDROID_RUNTIME_REFERENCE.md#managed-jobscheduler-october-11-2026)
- [Common device services](ANDROID_RUNTIME_REFERENCE.md#common-device-services-2026-10-13)
- [User and idle-policy queries](ANDROID_RUNTIME_REFERENCE.md#user-and-idle-policy-queries-2026-10-14)
- [Settings, PendingIntents and broadcasts](ANDROID_RUNTIME_REFERENCE.md#settings-pendingintents-and-broadcasts-2026-10-13)
- [Notification enablement](ANDROID_RUNTIME_REFERENCE.md#notification-enablement-2026-10-12)
- [Foreground services and notification posting](ANDROID_RUNTIME_REFERENCE.md#foreground-services-and-notification-posting-2026-10-28)
- [Battery](ANDROID_RUNTIME_REFERENCE.md#battery-2026-10-28)
- [App ops and missing content providers](ANDROID_RUNTIME_REFERENCE.md#app-ops-and-missing-content-providers-2026-10-12)
- [App audio, notification, Wi-Fi and power services](ANDROID_RUNTIME_REFERENCE.md#app-audio-notification-wi-fi-and-power-services-2026-10-13)
- [Per-process compatibility defaults](ANDROID_RUNTIME_REFERENCE.md#per-process-compatibility-defaults-2026-10-23)
- [Shared libraries and the device configuration](ANDROID_RUNTIME_REFERENCE.md#shared-libraries-and-the-device-configuration-2026-10-28)
- [Time zone](ANDROID_RUNTIME_REFERENCE.md#time-zone-2026-11-04)
- [The realm's service root](ANDROID_RUNTIME_REFERENCE.md#the-realms-service-root-2026-11-04)

**Launching apps and processes**

- [Managed APK launcher](ANDROID_RUNTIME_REFERENCE.md#managed-apk-launcher-2026-10-08)
- [Shared guest processes](ANDROID_RUNTIME_REFERENCE.md#shared-guest-processes-october-11-2026)
- [Session Zygote](ANDROID_RUNTIME_REFERENCE.md#session-zygote)
- [Activity stack](ANDROID_RUNTIME_REFERENCE.md#activity-stack-2026-10-12)
- [Startup: parsed-package cache](ANDROID_RUNTIME_REFERENCE.md#startup-parsed-package-cache-2026-10-20)
- [Main-thread stall reports](ANDROID_RUNTIME_REFERENCE.md#main-thread-stall-reports-2026-10-20)
- [APK call and stall tracing](ANDROID_RUNTIME_REFERENCE.md#apk-call-and-stall-tracing-2026-10-09)
- [Bionic compatibility paths](ANDROID_RUNTIME_REFERENCE.md#bionic-compatibility-paths-2026-10-21)
- [Android HOME and Asgard](ANDROID_RUNTIME_REFERENCE.md#android-home-and-asgard-2026-10-22)

**Runtime bootstrap and provisioning**

- [Android native runtime bootstrap](ANDROID_RUNTIME_REFERENCE.md#android-native-runtime-bootstrap)
- [Android 15 framework probe](ANDROID_RUNTIME_REFERENCE.md#android-15-framework-probe)
- [Provisioned framework](ANDROID_RUNTIME_REFERENCE.md#provisioned-framework)
- [Experimental same-image ART bootstrap](ANDROID_RUNTIME_REFERENCE.md#experimental-same-image-art-bootstrap)
- [Pointer tags and page sizes](ANDROID_RUNTIME_REFERENCE.md#pointer-tags-and-page-sizes)

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
