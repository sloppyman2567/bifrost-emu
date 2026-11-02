# bifrost-emu

```
  ____ _____ ______ _____   ____   _____ _______
 |  _ \_   _|  ____|  __ \ / __ \ / ____|__   __|
 | |_) || | | |__  | |__) | |  | | (___    | |
 |  _ < | | |  __| |  _  /| |  | |\___ \   | |
 | |_) || |_| |    | | \ \| |__| |____) |  | |
 |____/_____|_|    |_|  \_\\____/|_____/   |_|

  bifrost-emu  v2.0.0-alpha.3
  x86_64 ◄─────────────────► ARM64
```

**A bridge between worlds.** Run ARM64 (AArch64) Linux programs, games and
Android apps on an x86_64 Linux PC, translated on the fly by a JIT compiler.
Existing binaries need no cross-compiler and no QEMU.

[![CI](https://github.com/sloppyman2567/bifrost-emu/actions/workflows/ci.yml/badge.svg)](https://github.com/sloppyman2567/bifrost-emu/actions/workflows/ci.yml)
[![License: Unlicense](https://img.shields.io/badge/license-Unlicense-blue.svg)](http://unlicense.org/)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/)
[![Platform: Linux x86_64](https://img.shields.io/badge/platform-Linux%20x86__64-lightgrey.svg)](Docs/SUPPORT.md)
[![Version: 2.0.0-alpha.3](https://img.shields.io/badge/version-2.0.0--alpha.3-orange.svg)](Docs/CHANGELOG.md)

---

## Highlights

- **FrostJIT** — ARM64 → typed IR → scalar SSA → x86_64, with block chaining,
  an indirect-branch cache and an optional Tier-2 superblock compiler.
  A switch-based reference interpreter backs every instruction.
- **Checked against references** — instruction semantics (NaNs, rounding,
  signed zero, Q=0 lanes, faults) are compared with QEMU and the Arm pseudocode;
  `make verify` replays JIT blocks against the interpreter.
- **Android apps without a phone** — *Asgard* runs real Bionic, ART and the
  Android 15 framework with a shared Binder broker, installs signed APK sets
  (`bifrost-emu game.apk` installs and runs one) and plays a Unity/IL2CPP game.
- **Your real GPU, audio and input** — GL, EGL, GLES, Vulkan, SDL2/SDL3, ALSA
  and PulseAudio calls are forwarded to host libraries; Android windows render
  at the monitor's native pixel density.
- **A Linux personality** — 200+ syscalls, threads, signals, futexes,
  userfaultfd, cooperative `ptrace`, and a virtual `/proc`, `/dev` and `/sys`
  (*Yggdrasil* VFS). Dynamic programs find an [installed rootfs](Docs/ROOTFS.md)
  without flags.
- **Embeddable** — a C API (`libbifrost`) and an Android native-bridge
  adapter; see [embedding](Docs/EMBEDDING.md).

## What runs

Statuses come from the linked pages and `Scripts/Android/compatibility.json`;
"user-confirmed" and "user-reported" mean the user played it on Bifrost, not
that every feature has an automated test or a recorded capture.

| Program | Kind | Status |
|---|---|---|
| [SpongeBob – Krusty Cook-Off 5.12.0](Docs/Apps/spongebob.md) | Android, Unity/IL2CPP | User-confirmed cooking levels, reaching level 16 at about 60 FPS (2026-11-01), outfits and menus at native resolution. Child-age path; the adult path waits on Google Play services. |
| [Azur Lane 9.3.7](Docs/Apps/azur-lane.md) | Android, Unity/IL2CPP | User-reported playable online through a finished battle, with audio and pause/resume (2026-11-01). Home screen captured at 60 FPS. |
| The Battle Cats 15.6.1 | Android | User-reported playable (2026-10-29). |
| [Umamusume: Pretty Derby](Docs/Apps/umamusume.md) | Android, Unity | Global 1.35.2: user-reported playable, title, home and Career story captured at 30 FPS (2026-11-01). JP 2.32.0: blocked by the game's device integrity check when it connects. |
| Subway Surfers 3.70.3 | Android, Unity/IL2CPP | User-reported playable, ads work (2026-10-29). |
| Geometry Dash Lite 2.2.148 | Android, Cocos2d-x | User-reported playable (2026-10-29); ads only somewhat stable. |
| YouTube 21.42.247 | Android | Home screen, search and video playback seen (2026-10-28); not reported as fully working. |
| Oddmar | Android, Unity/IL2CPP | Gets past its opening video and on to the next one (2026-11-01); gameplay is not confirmed. |
| Hungry Shark Evolution, Hungry Shark World | Android | Run sessions to their windows; gameplay is not confirmed. Hungry Shark World's Play Games sign-in does not complete. |
| Uptodown | Android | Draws its home list, then asks for all-files access, which Bifrost cannot grant. |
| Roblox, TrickCal | Android | Blocked: Roblox is disconnected after joining (needs real Google attestation); TrickCal's anti-cheat ends it. |
| [Neverball 1.6.0](Docs/Apps/neverball.md) | Linux, SDL2/GL | User-confirmed stable gameplay, replays, menus and fonts. |
| [Minecraft Weekend](Docs/Apps/minecraft-weekend.md) | Linux, SDL2/GL voxel game | User-confirmed stable gameplay; chunk rendering can lag. |
| [vkQuake 1.33.1](Docs/Apps/vkquake.md) | Linux, Vulkan | User-confirmed gameplay and menus; texture corruption remains. |
| [Doom 3 (dhewm3)](Docs/Apps/doom3.md) | Linux, SDL2/GL | User-reported extended gameplay with audio and animation; mostly sustained 60 FPS in the demo, even in heavy scenes (2026-11-01, not instrumented); the full game is not covered by that report. |
| Teeworlds | Linux, SDL2/GL | Reaches its rendering menu with sound initialized. |
| BusyBox, ToyBox | Linux CLI | Covered by the regression suite. |
| CoreMark | Linux benchmark | CRC-validated runs through `Scripts/Bench/run_benchmarks.py`. |

## Screenshots

User-supplied captures, not benchmarks. The SpongeBob shot is from November 1, 2026 (Bifrost 2.0.0-alpha.3) and the
Azur Lane shot from October 14 (2.0.0-alpha.1); the Linux games are from October 2 to 4. More are in the
[SpongeBob gallery](Docs/Apps/spongebob.md#screenshots-2026-11-01),
[Azur Lane gallery](Docs/Apps/azur-lane.md#screenshots-2026-10-14) and
[Umamusume gallery](Docs/Apps/umamusume.md#screenshots-2026-11-01).

<table>
<tr>
<td><img src="Docs/Images/SpongeBob-2026-11-01/gameplay.png" alt="SpongeBob cooking gameplay" width="320"><br><sub>SpongeBob: Krusty Cook-Off (Android)</sub></td>
<td><img src="Docs/Images/AzurLane-2026-10-14/login.png" alt="Azur Lane title and login screen" width="320"><br><sub>Azur Lane title/login (Android)</sub></td>
<td><img src="Docs/Images/Neverball-2026-10-04/gameplay.png" alt="Neverball gameplay" width="320"><br><sub>Neverball (Linux, SDL2/GL)</sub></td>
</tr>
<tr>
<td><img src="Docs/Images/MinecraftWeekend-2026-10-04/daytime.png" alt="Minecraft Weekend" width="320"><br><sub>Minecraft Weekend (Linux)</sub></td>
<td><img src="Docs/Images/Doom3-2026-10-04/hangar-npc.png" alt="Doom 3 (dhewm3)" width="320"><br><sub>Doom 3 / dhewm3 (Linux)</sub></td>
<td><img src="Docs/Images/VkQuake-2026-10-04/gameplay.png" alt="vkQuake gameplay" width="320"><br><sub>vkQuake (Linux, Vulkan)</sub></td>
</tr>
</table>

## Progress

**2.0.0-alpha.3** (2026-11-01) is the current pre-release. The goals for it and
what follows are in the [roadmap](Docs/roadmap.md), which has a
progress table and the known blockers; everything that changed is in the
[changelog](Docs/CHANGELOG.md). Since 2.0.0-alpha.2:

- **Install and run:** `bifrost-emu app.apk` installs and runs an Android
  package, a Debian/Ubuntu package and `asgard-setup` install Bifrost and Asgard,
  and an installable rootfs lets dynamic programs run without flags.
- **Linux personality:** guest `ptrace` and pseudo terminals, `/proc/<pid>/status`,
  and fork/exec fixes (fork children no longer hang or copy the game).
- **JIT:** 18-19% fewer generated bytes per guest instruction in the measured
  programs (guarded memory accesses), less multithreaded overhead, native pixel/media NEON forms, and an experimental,
  off-by-default persistent translation cache.
- **Android and graphics:** more service contracts (display, audio, Binder),
  fewer platform errors every game logged, and graphics-thunk fixes for pointer
  and string-array arguments.
- **Window stats:** an in-window overlay (`--window-stats`) for Android apps.
- **Open:** measured frame times, the first-ad-load wait, WebView interaction,
  and sustained play. A passing build or a launch is not gameplay.

Check an installed app's recorded status yourself with
`python3 Scripts/Android/asgard.py compat`.

## Quick start

From a fresh clone, one command does everything: it installs the host packages (apt, dnf, zypper or pacman; it asks for
`sudo` only if some are missing), builds the emulator, then downloads and installs Asgard, all under `~/.local`:

```bash
git clone https://github.com/sloppyman2567/bifrost-emu && cd bifrost-emu
make bootstrap                         # about 1.5 GB of pinned downloads; PREFIX=DIR to install elsewhere
```

`make bootstrap` and `make deps` need `make`, and bootstrap also needs Python. On a minimal system without them, run
`Scripts/Setup/install-deps.sh` first: it needs only bash and installs both.

This is new and not yet confirmed end to end, and `make deps` has only been shown to work on some distributions: see the
[support matrix](Docs/SUPPORT.md). If Bifrost is already installed elsewhere, such as `/usr/local`, this makes a second copy of
about 1.1 GB, which takes over if `~/.local/bin` comes first on your `PATH`. To update an existing install instead, see
[the APK guide](Docs/Android/ANDROID_APK.md).

To build only the emulator:

```bash
make deps                              # once: the host packages
make -j$(nproc)                        # build (SDL2/GL thunks auto-detected)
./bifrost-emu hello.elf                # run a static ARM64 binary
./bifrost-emu -v Tests/Apps/fib.elf    # print execution stats on exit
./bifrost-emu Tests/Apps/toybox seq 1 10   # arguments pass through
```

On an immutable Fedora such as Bazzite, do it inside a [Distrobox](https://distrobox.it) container, and run the emulator
there too, since it links the container's libraries:

```bash
distrobox create --name bifrost --image registry.fedoraproject.org/fedora-toolbox:44 && distrobox enter bifrost   # Bazzite only
make deps
make -j$(nproc)
./bifrost-emu --setup-rootfs                  # once: the libraries Linux programs need
./bifrost-emu --setup-rootfs --games          # for Linux games; --rootfs-default games then uses them by default
```

Checked in a Fedora 44 chroot on one machine, without Distrobox, a display or a GPU: see the [support matrix](Docs/SUPPORT.md).

On Debian and Ubuntu (25.10 or newer for Android windows) the quickest install is the package:

```bash
sudo apt install build-essential debhelper pkg-config libavcodec-dev libavutil-dev libswscale-dev \
     libegl-dev libffi-dev libgl-dev libsdl3-dev libsdl2-dev libwayland-dev libx11-dev
make deb                                  # writes ../bifrost-emu_2.0.0~alpha.3_amd64.deb
sudo apt install ../bifrost-emu_*.deb
bifrost-emu --setup-rootfs                # once: downloads the toolchains and builds the guest rootfs
```

Android apps need Asgard, which `make deb` also packages as `bifrost-emu-asgard`:

```bash
sudo apt install ../bifrost-emu-asgard_*.deb      # also pulls in clang, lld and a headless JDK
asgard-setup                                      # once: ~1.5 GB of pinned downloads, then installs ~1.1 GB under ~/.local
bifrost-emu /path/to/app.apkm                     # installs and runs it
```

Dynamically linked programs need a guest root filesystem for their libraries.
Install a standard one, then run programs directly ([details](Docs/ROOTFS.md)):

```bash
./bifrost-emu --setup-rootfs           # for your user, ~/.local/share/bifrost/rootfs (~235 MB download)
sudo make install install-rootfs       # or system-wide, under $(PREFIX)/share/bifrost
bifrost-emu my_dynamic_app.elf         # finds the rootfs automatically
```

`--setup-rootfs` runs the checkout's `Scripts/Setup/install-rootfs.sh` and passes its options on: `--system`,
`--prefix DIR`, `--full` (more real-world libraries), `--profile NAME`, `--games` and `--uninstall`.

Linux games need more libraries than the base rootfs; Android apps use neither, because Asgard brings its own. `--setup-rootfs --games` downloads OpenAL, SDL2_ttf, Vorbis, libpng,
libjpeg, libcurl and their dependencies into a `games` profile; `--rootfs-profile games` selects it and `--rootfs-list`
shows what is installed. Without any flag, the first rootfs found next to the executable, in the user's data directory
or system-wide supplies the libraries, like `qemu-aarch64 -L`, and the guest still sees host paths; `--rootfs PATH`
(or `BIFROST_ROOT`) instead makes PATH the guest's root. The full search order is in the [rootfs guide](Docs/ROOTFS.md).
`./Scripts/Setup/setup-rootfs-all.sh` builds the development `rootfs/` that the test suite uses.

Android apps install from a signed APK, APKM or APKS bundle:

```bash
./bifrost-emu /path/to/game.apkm      # installs if needed, then runs (.apk/.apkm/.apks/.xapk)
python3 Scripts/Android/android_package.py run org.example.game   # an installed package id
```

This needs Asgard. From a fresh clone, one command downloads and builds what it needs (about 1.5 GB of pinned
downloads) and installs it so it works from any directory:

```bash
make provision-asgard PREFIX=$HOME/.local      # for your user; add ~/.local/bin to PATH for the asgard command
make android-provision && sudo make install-asgard   # or system-wide: /usr/local needs sudo
```

The desktop launcher (`asgard launcher`) also needs PyQt6, which `make deps` and `make bootstrap` install;
`bifrost-emu app.apk` works without it. Run the command again after an update: only the files that changed are copied. `./Scripts/Setup/install-asgard.py` is the
install step alone, for a checkout that is already provisioned; the [APK guide](Docs/Android/ANDROID_APK.md) has the details.

`--guest-ld` (experimental, not the default) runs dynamic programs under the rootfs's own `ld.so`, as a kernel exec
would, instead of the built-in linker. It needs the `ld.so.cache` that the rootfs setup writes. Thunked libraries (GL,
SDL2, Vulkan, audio) are served to it as in-memory stubs, and it works with an automatically found rootfs as well as
with `--rootfs`. It passes the same test suites as the built-in linker; dhewm3 (120 s) and neverball ran under it, but
other games and `make verify` have not been run with it.

Useful switches: `-d` traces every instruction, `--no-jit` forces the
interpreter, `--window-stats` shows FPS and load statistics in the title and, for Android
apps, an in-window overlay (see [window stats](Docs/window-stats.md); `--window-stats-mode`
picks the layout), `BIFROST_TIER2=1` enables experimental Tier-2 region compilation, and the
experimental `--jit-disk-cache` keeps translations in `~/.cache/bifrost/jit` so later launches start
faster (the first launch compiles more slowly; see [the JIT guide](Docs/JIT.md#persistent-translation-cache-2026-10-26-experimental-off-by-default)).

## Running programs

### Static binaries

Static AArch64 ELF binaries work out of the box. This covers most
cross-compiled Go, Rust, C and C++ programs, static BusyBox and ToyBox, and
game engines that ship statically linked.

### Dynamically linked binaries

Both **glibc** (2.36 through 2.40+, including DT_RELR) and **musl** guests are
supported, with shared libraries, GOT/PLT relocation, TLS, ifuncs, versioned
symbols and constructors. glibc's NPTL threads, mutexes, condition variables
and `__thread` TLS work end to end across multiple thread waves. The rootfs
setup can also be done step by step:

```bash
./Tools/Fetch/fetch-glibc-toolchain.sh     # glibc toolchain (130 MB)
./Tools/Fetch/fetch-musl-toolchain.sh      # musl toolchain (104 MB)
./Scripts/Setup/setup-rootfs.sh            # rootfs with glibc + musl libraries
./Scripts/Setup/fetch-realworld-libs.sh    # libselinux, libpcre2, libssl, ...
./Scripts/Setup/fetch-curl-deps.sh         # curl's 24 dependency libraries
```

The rootfs provides both dynamic loaders and libcs, `/etc` basics, timezone
data, and Android-style `/system`, `/data` and `/sdcard` scaffolding.

### Android apps (Asgard)

**Asgard** is Bifrost's Android compatibility layer. It boots a real guest
`linker64`, Bionic and ART, and supplies Android 15 framework services from a
managed launcher: Activities, windows and dialogs, input, audio, display,
package queries, JobScheduler and more. An app supervisor and shared Binder
broker connect an app's processes (game, WebView, services).

```bash
python3 Tools/Android/fetch-android-runtime.py         # one-time runtime setup
python3 Tools/Android/fetch-aapt2.py
python3 Tools/Android/fetch-android-package-tools.py
make android-runtime-adapters android-managed-launcher
python3 Scripts/Android/android_package.py install /path/to/package.apkm
python3 Scripts/Android/android_package.py run org.example.package
python3 Scripts/Android/asgard.py run /path/to/package.apkm   # installs, then runs (or run an installed package id)
python3 Scripts/Android/asgard.py realm status        # inspect the shared realm
python3 Scripts/Android/asgard.py config org.example.package network offline   # per-app settings, applied at every start
python3 Scripts/Android/asgard.py clear-data org.example.package   # reset saves; `uninstall` removes the app
python3 Scripts/Android/asgard.py launcher             # the desktop app library: install, play, settings, last session
python3 Scripts/Android/asgard.py google sign-in       # experimental: microG's Google sign-in window (accounts shared by every app)
```

Native `NativeActivity` libraries can also run directly with
`./bifrost-emu --android libgame.so`. Android support is experimental: Google
Play services come from microG (sign-in works up to Google's own page; see the
runtime guide), media decoding and many system services are partial or missing. See
the [Android runtime guide](Docs/Android/ANDROID_RUNTIME.md),
[APK tooling](Docs/Android/ANDROID_APK.md),
[Binder](Docs/Android/ANDROID_BINDER.md) and
[window plugins](Docs/Android/ANDROID_WINDOW_PLUGINS.md).

## Architecture

```mermaid
flowchart LR
    ELF["ARM64 ELF / APK"] --> Loader["Loader + dynamic linker"]
    Loader --> Decoder
    Decoder --> IR["IR translator<br/>scalar SSA + passes"]
    IR --> JIT["FrostJIT<br/>x86_64 codegen"]
    Decoder --> Interp["Reference interpreter"]
    JIT <-->|"fallback, verify"| Interp
    JIT --> Sys["Syscalls + Yggdrasil VFS"]
    Interp --> Sys
    JIT --> Thunks["GL / Vulkan / SDL / audio thunks"]
    Sys --> Host[("x86_64 Linux host")]
    Thunks --> Host
```

| Component | Location | Role |
|---|---|---|
| ELF loader | `Source/System/Loader/ElfLoader.cpp` | Static and PIE binaries, RELA relocations |
| Dynamic linker | `Source/System/Loader/DynamicLinker/` | DT_NEEDED, GOT/PLT, TLS, ifuncs, versioned symbols |
| Decoder | `Source/Interpreter/Decoder/` | Shared AArch64 decoder for both engines |
| IR translator | `Source/FrostJIT/IR/` | Typed IR, scalar SSA, optimization and validation |
| FrostJIT | `Source/FrostJIT/` | x86_64 code generation, register allocation, dispatch, Tier-2 |
| Interpreter | `Source/Interpreter/` | Reference semantics and JIT fallback |
| Syscalls | `Source/System/Syscalls/` | 200+ Linux AArch64 syscalls by family |
| Yggdrasil VFS | `Source/System/VFS/` | Virtual `/proc`, `/dev`, `/sys`, framebuffer, audio, input |
| Threads/signals | `Source/System/Core/` | `clone`, futexes, signal delivery, shared JIT |
| Thunks | `Source/System/Graphics/`, `Source/System/Audio/` | Host GL/EGL/Vulkan/SDL/audio forwarding |
| Asgard | `Source/System/Android/`, `Guest/Android/` | Binder, surfaces, loopers, managed framework services |

The [source layout](Docs/ARCHITECTURE.md) and [JIT guide](Docs/JIT.md) go
deeper. Host forwarding and input are described in
[host integration](Docs/HOST_INTEGRATION.md).

## Building

| Requirement | Needed for |
|---|---|
| g++ 9+ (C++17), make, Linux x86_64 | Everything (required) |
| SDL2 development headers (`./Tools/Fetch/fetch-sdl2-headers.sh`) | Framebuffer, guest SDL2 and GL thunks |
| SDL3 ≥ 3.2 headers and runtime | Android host windows |
| FFmpeg libavcodec/libavutil/libswscale development files | Android host media decoding (`USE_MEDIA=1`, default) |
| AArch64 cross toolchain (`./Tools/Fetch/fetch-musl-toolchain.sh`) | Building guest test programs |

`make deps` installs all of these except the cross toolchain, using apt, dnf, zypper or pacman
([what has been checked](Docs/SUPPORT.md)).

| Command | Builds |
|---|---|
| `make bootstrap` | Host packages, the emulator and Asgard, installed under `~/.local` or `PREFIX` |
| `make` | Default; auto-detects SDL2 and enables host GL/EGL thunking |
| `make USE_SDL2=1 USE_SDL3=1 USE_THUNK_GL=1` | Android SDL3 windows plus guest SDL2/GL thunks |
| `make USE_SDL2=0 USE_SDL3=0` | Headless build |
| `make USE_MEDIA=0` | No host media decoders; Codec2 advertises no host codecs |
| `make debug` | Sanitizer build at `build/debug/bifrost-emu-dbg` |
| `make lib` | `libbifrost.a` for [embedding](Docs/EMBEDDING.md) |

Each configuration gets its own `build/release-sdl*-gl*` directory; the default
release binary is also copied to `./bifrost-emu`. `make clean` removes them all.
Guest test programs build with `make setup-tests`, or one at a time with
`make cross SRC=Tests/Apps/my_test.c OUT=Tests/Apps/my_test.elf`.

## Testing

```bash
make check            # standard suite (benchmarks included)
make check-quick      # without benchmarks
make check-nojit      # quick suite under the interpreter
make verify           # replay JIT blocks against the interpreter
make ci               # local correctness gate (generated files, APIs, both engines)
./Scripts/Testing/run_tests.sh --strict --filter "sig|brk|pipe"   # by name
```

On 2026-11-01, with every fixture provisioned, the strict suite passed
**377/377** under the JIT and **362/362** under the interpreter (15 JIT-only
tests are not run there), and `make verify` passed 100 differential fixtures plus
ten threaded oracle runs. Missing fixtures fail strict mode; environment skips
(exit 77) stay visible. Details, history and fixture licensing are in
[test evidence](Docs/TESTS.md) and [third-party notes](Docs/THIRD_PARTY.md).

## Configuration

Settings come from a TOML file, environment variables and flags:

```bash
./bifrost-emu --config bifrost.toml my_app.elf   # use a config file
./bifrost-emu --print-config                     # show effective settings

BIFROST_ROOT=/path/to/rootfs    # guest root filesystem
BIFROST_TIER2=1                 # experimental Tier-2 region compilation
BIFROST_JIT_VERIFY=1            # JIT vs interpreter divergence check
BIFROST_SYSCALL_TRACE=1         # trace syscalls
BIFROST_DYNLINK_TRACE=1         # trace the dynamic linker
BIFROST_THUNK_TRACE=1           # trace host GL/audio/display calls
```

[`bifrost.toml.sample`](bifrost.toml.sample) lists every option;
`Source/System/Core/DebugFlags.hpp` lists the diagnostic switches.

## Performance

CoreMark (October 27, 2026), measured on a Ryzen 7 5800X3D: 160,000 iterations,
one warmup and three interleaved measured runs per binary, every CRC validated
(`crcfinal` 0xa14c, the same value QEMU reports) and no CoreMark validity error.
The iteration count was raised from 110,000, which the current build finishes in
8.4 s, under CoreMark's ten-second minimum. See [the method and
limits](Docs/JIT.md#coremark-checkpoint-2026-10-27).

| Build | Median iterations/s |
|---|---|
| bifrost-emu 2.0.0-alpha.3 (JIT, shipped SDL/GL build) | 13,156 |
| same code, headless build | 12,981 |
| bifrost-emu 2.0.0-alpha.2 (JIT, headless build) | 12,332 |
| QEMU 11.1.1 user mode (TCG) | 7,710 |

On this integer/control-flow benchmark the shipped build is about 71% faster than
QEMU, and the headless build of the same code is about 5% faster than the
alpha.2 release built the same way. Neither predicts game FPS. QEMU's runs
spread 9% (7,182 to 7,817) on this shared desktop, so differences of a few
percent are within noise. Measurements from the `bench_*` programs
(same host, median of three runs; each program's own timer, except `bench_mips`,
which is process wall time; JIT compilation is included, so the millisecond-scale
JIT times are coarse):

| Benchmark | Interpreter | JIT | Speedup |
|---|---|---|---|
| fib(35) | 6.98 s | 0.074 s | ~94x |
| memcpy 256 MiB | 194 MiB/s | ~9,570 MiB/s | ~49x |
| qsort 100K ints | 4.27 s | 0.079 s | ~54x |
| matrix 256×256 | 12.1 MFLOPS | ~1,119 MFLOPS | ~92x |
| bench_mips (800M instructions) | ~51 MIPS | ~4,280 MIPS | ~84x |

Repeatable runs with checksums and host metadata: `Scripts/Bench/run_benchmarks.py`.
The JIT uses AVX-512, AVX2, SSE4.2, AES-NI/PCLMULQDQ/SHA-NI, BMI1/BMI2 and
FMA3 where the host has them, with fallbacks. Game profiling notes live
on each app page, for example [Doom 3](Docs/Apps/doom3.md).

## Embedding

```c
#include "Bifrost.h"

bifrost_emu_t* emu = bifrost_create();
bifrost_load_elf(emu, "hello.elf", argc, argv);
int exit_code = bifrost_run(emu);   // JIT on by default
bifrost_destroy(emu);
```

The C API also offers register and memory access, stepping with breakpoints,
guest function calls, symbol lookup, syscall hooks and guest `dlopen`. The
[embedding guide](Docs/EMBEDDING.md) covers it and the Android native-bridge
adapter.

## Limitations

- **User mode only** — no kernel or full-system emulation (use QEMU system mode).
- **AArch64 guests only** — no 32-bit ARM, so an Android APK that ships only
  `armeabi-v7a` or `x86` libraries cannot start.
- **x86_64 Linux hosts only** — other distributions are expected to work but
  only Ubuntu 24.04 is CI-built; see the [support matrix](Docs/SUPPORT.md).
- **Android is experimental** — Google Play services and complete system_server
  remain absent. Small Codec2 decoding/Surface probes pass; full in-game video
  and rewarded-ad playback are unverified.
- **Untrusted binaries** — read the [security model](Docs/SECURITY.md) first.

## Documentation

The current version is **2.0.0-alpha.3**, the third pre-release toward the
[v2.0 roadmap](Docs/roadmap.md); its goals are not complete yet.

| Topic | Document |
|---|---|
| App status | [SpongeBob](Docs/Apps/spongebob.md), [Azur Lane](Docs/Apps/azur-lane.md), [Umamusume](Docs/Apps/umamusume.md), [Neverball](Docs/Apps/neverball.md), [Minecraft Weekend](Docs/Apps/minecraft-weekend.md), [vkQuake](Docs/Apps/vkquake.md), [Doom 3](Docs/Apps/doom3.md), [zstd](Docs/Apps/zstd.md) |
| JIT internals | [JIT guide](Docs/JIT.md) |
| Source layout | [Architecture](Docs/ARCHITECTURE.md) |
| Android | [Runtime](Docs/Android/ANDROID_RUNTIME.md), [APKs](Docs/Android/ANDROID_APK.md), [Binder](Docs/Android/ANDROID_BINDER.md), [window plugins](Docs/Android/ANDROID_WINDOW_PLUGINS.md) |
| Host graphics, audio and input | [Host integration](Docs/HOST_INTEGRATION.md), [window stats](Docs/window-stats.md) |
| Embedding | [C API and native bridge](Docs/EMBEDDING.md) |
| Support and security | [Support matrix](Docs/SUPPORT.md), [security model](Docs/SECURITY.md) |
| Tests and history | [Test guide](Docs/TESTS.md), [changelog](Docs/CHANGELOG.md) (releases before 2.0.0-alpha.3 are in `Docs/archive/`), and the dated [test](Docs/archive/TESTS-history.md), [JIT](Docs/archive/JIT-history.md) and [runtime](Docs/archive/ANDROID_RUNTIME-history.md) history, [third-party notes](Docs/THIRD_PARTY.md); per-app investigation logs are in `Docs/archive/apps/` |
| Licensing | [License scope and texts](LICENSES/README.md) |
| Configuration | [`bifrost.toml.sample`](bifrost.toml.sample) |

## License

Bifrost's original code, original Asgard integration and original regression
tests use the [Unlicense](LICENSE). Reused AOSP material keeps its upstream
license; [LICENSES/](LICENSES/README.md) includes Apache 2.0 and Android source
attribution alongside the project license. Third-party fixtures, vendored
dependencies and game assets keep their own terms. See
[third-party and test distribution notes](Docs/THIRD_PARTY.md).

## Contributing

1. Fork the repository and follow the existing code style.
2. Build guest fixtures with `make setup-tests`, then run the relevant JIT and
   interpreter checks (`make check-all` for provisioned coverage).
3. For JIT changes, read the [JIT guide](Docs/JIT.md) and run `make verify`;
   report existing verifier failures separately from new ones.
4. Preserve upstream notices for reused code and include validation evidence
   with your pull request. Original contributions, including tests, use the
   Unlicense; reused code retains its upstream license and attribution.

Bug reports help most with the AArch64 binary (or a minimal reproducer), the
exact command line, and the tail of `./bifrost-emu -v -d ... 2>&1`.
