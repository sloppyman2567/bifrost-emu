# Signed APK installation and native launch

The emulator executable raises its own host `RLIMIT_NOFILE` soft limit to
the inherited hard limit before startup, including its Binder-broker mode.
Desktop sessions commonly supply 1024 soft / 1048576 hard; large managed
apps therefore no longer need a preceding `ulimit -n 1048576`. This uses
the permitted ceiling rather than assuming a particular distribution's
limit. It leaves the hard limit and parent desktop/shell unchanged. A
lower hard limit still constrains open files; a failed adjustment prints
one warning and startup continues under the inherited limit. This is a
host-executable startup policy, not complete guest resource-limit emulation
or a change to resource limits of library-embedding callers.

Current tooling scope: October 14, 2026. Bifrost installs signed APK sets and executes a
`android.app.NativeActivity` through the real guest Bionic linker, and managed
SurfaceView Activities through the ART/framework
[managed launcher](ANDROID_RUNTIME.md#managed-apk-launcher-2026-10-08).
The regression APK uses split assets, pthread/TLS, persistent storage, EGL
pixel readback and an actual Binder service call. Managed Activities still
require guest ART and Android framework startup.

## Setup and use

On Debian and Ubuntu the `bifrost-emu-asgard` package does all of the provisioning below: install it and run `asgard-setup`
(see the README). It prints a numbered heading for each of its nine steps, and every download shows its progress:
on a terminal one line updates in place (`Android 15 system image  [=====>   ]  33%  8.0 MiB/24.0 MiB  6.4 MiB/s  ETA 0:02`),
and piped into a log it prints a line per ten percent. The rest of this section is the route from a source
checkout.

Provision Python 3.12 or newer, Clang/LLD, `debugfs` from e2fsprogs and a JDK containing
`java` and `keytool`. Building the managed launcher also needs `javac`, found through `JAVAC`, `PATH`,
`JAVA_HOME`, then the newest JDK under `/usr/lib/jvm`, `/usr/lib64/jvm` or `/usr/java`. The signing and manifest tools are checksum-pinned AOSP
downloads; no full Android SDK installation is required. `make deps` (`Scripts/Setup/install-deps.sh`) installs all of
these with apt, dnf, zypper or pacman, and asks for `sudo` only if something is missing. It keeps a JDK you already have. Python
comes from the distribution and `make deps` does not check its version: Debian 12's 3.11, for one, is too old. Not every
distribution has been shown to work; see the [support matrix](../SUPPORT.md).

From a fresh clone, one target does all of the provisioning and the install, with about 1.5 GB of pinned downloads:

```bash
make provision-asgard PREFIX=$HOME/.local       # for your user, no root needed
asgard --help                                   # with ~/.local/bin on PATH

make android-provision                          # or system-wide: provision as your user...
sudo make install-asgard                        # ...then install under $(PREFIX), default /usr/local
```

`PREFIX` and `DESTDIR` work as for `make install`. The default `PREFIX`, `/usr/local`, needs root: `make
provision-asgard` checks first that it can write to the prefix, so it fails before downloading anything rather than
at the end. `sudo make provision-asgard` also works, but then root downloads everything and owns the checkout's build
trees, which is why the two steps above are better. Run the target again at any time: each step keeps what it has
already verified, and the install copies only what differs. It does not build `bifrost-emu` itself (`make`, `make install`).
It does not install host packages either: `make deps` does (see above), including PyQt6, which only the desktop launcher
(`asgard launcher`) needs. The install says so when PyQt6 is missing. Running apps from the command line works without it.
`make bootstrap` runs `make deps`, builds and installs the emulator and then runs this target, under `~/.local` unless you give a `PREFIX`.
It has not yet been confirmed end to end.

A new prefix means a second install, because the installers do not look for other copies. With both `/usr/local` and `~/.local`,
whichever `bin` comes first on `PATH` runs, and `bifrost-emu` uses the Asgard beside it. App state is separate from both, so they
share it. To update an install that already exists, use its own prefix: for `/usr/local`, `make android-provision` and then
`sudo make install-asgard`.

It runs these steps, which can also be run by hand:

```bash
make -j6 USE_SDL2=1 USE_SDL3=1 USE_THUNK_GL=1
python3 Tools/Android/fetch-android-runtime.py
python3 Tools/Android/fetch-aapt2.py
python3 Tools/Android/fetch-android-package-tools.py
make android-runtime-adapters android-window-plugin

python3 Scripts/Android/android_package.py install /path/to/package.apkm
python3 Scripts/Android/android_package.py run org.example.package

# Diagnose native loading even when the package has a managed launcher.
# Each library gets a fresh process; constructors run, JNI_OnLoad does not.
python3 Scripts/Android/android_package.py probe org.example.package
python3 Scripts/Android/android_package.py probe org.example.package --library libunity.so

# Use the loadable SDL3 window backend instead of the built-in backend.
python3 Scripts/Android/android_package.py run org.example.package \
    --android-window-plugin build/release-sdl1-gl1/plugins/android-window-sdl.so
```

`bifrost-emu PATH.apk` does the same without naming Asgard: a file ending in `.apk`,
`.apkm`, `.apks` or `.xapk` replaces the process with `python3 asgard.py --emu
<this binary> run PATH [args]` (so exit status and signals pass through), taking
`-d`/`--allow-downgrade` and `--android-window-plugin PATH` along. It uses the first
`Scripts/Android/asgard.py` it finds in: `$BIFROST_ASGARD`; next to the executable
(a source checkout); `<executable dir>/../share/bifrost/asgard`; `$XDG_DATA_HOME/bifrost/asgard`
(default `~/.local/share/bifrost/asgard`); `/usr/local/share/bifrost/asgard`;
`/usr/share/bifrost/asgard`. If none exists it lists them and names the installer
(status 127; 126 for a file that is not a ZIP). `BIFROST_ROOT` is removed
from the environment first, because it would confine every Android process.

### Installing Asgard

`Scripts/Setup/install-asgard.py` installs a self-contained Asgard so the emulator works without
the source tree. `make provision-asgard` runs it for you. To run it yourself, provision the checkout first (the commands above), then:

```bash
./Scripts/Setup/install-asgard.py              # for your user: ~/.local/share/bifrost/asgard, ~/.local/bin/asgard
sudo make install install-asgard               # or system-wide, under $(PREFIX) (default /usr/local)
./Scripts/Setup/install-asgard.py --dry-run    # after updating the checkout: what would change
./Scripts/Setup/install-asgard.py              # copy only the files that differ
./Scripts/Setup/install-asgard.py --uninstall  # or: sudo make uninstall-asgard
```

Before it copies anything, it checks the checkout. The Android trees must exist, and `aapt2`, `apksigner.jar`,
`adapters.json`, the launcher's `classes.dex` and the manifests that end a complete ART and framework build
(`bifrost-art.json`, `bifrost-framework.json`) must be present and not empty. If not, it names each one and the command that
creates it, and installs nothing. These checks look for the files; they do not compare a tree with its manifest. The ART and
framework tools refuse a tree without a manifest, so the message says to remove it first.

The two microG APKs pinned in `Tools/Android/android_microg.py` are checked by name and size. An install downloads any that
are missing or the wrong size into `build/microg` and checks the SHA-256 of both, because an installed Asgard may be unable
to write into its own tree when an app first needs them. A same-size file with the wrong hash stops the install. `--dry-run` never downloads
and writes nothing, not even to the checkout: it lists the APKs a real run would fetch and counts them in what it would copy.
The target directory is checked before any download too (`--check-prefix` does only that).

It copies what Asgard reads at run time, about 1 GiB at the same relative paths: the scripts and
`Tools/Android` modules, `Tools/android-tools` and `Tools/android-runtime`, `Guest/Android`
(the managed launcher's sources and the codec fingerprint inputs), the framework and ART root
images, the guest adapters, the managed launcher dex and the microG archives. Tests and
build-only sources are not installed. Re-running updates in place: a file is copied when its size,
modification time, mode or link target differs (`--verify` also compares contents, which finds a
same-size edit), each replaced atomically so a running session keeps the file it opened, and files
the checkout no longer has are removed. The `.asgard-install` marker records `complete: false` while
a run is in progress, so an interrupted run is reported and finished by the next one. Installed
files are readable by everyone and writable only by their owner (`a+rX`, `go-w`), whatever mode the
checkout gave them, so a `sudo` install also works for other users. Updating an install that root
owns needs root. First installs use `cp --reflink=auto`, so they are near-instant
on filesystems that can share blocks and a plain copy elsewhere.

An installed Asgard cannot write into its own tree, so app state (installed apps, saves, settings)
lives in `~/.local/share/bifrost/android` (`$XDG_DATA_HOME/bifrost/android`), not in the checkout's
`android-state`. Set `BIFROST_ANDROID_STATE` to use another directory, such as an existing
`android-state`, for both layouts; `--state-root` still overrides it per command. Running a package
file (`asgard.py run FILE`, `bifrost-emu FILE`) installs it into that state on first use. The host
needs Python 3.12 or newer and a JDK with `java` and `keytool`, as for the checkout.
`asgard.py run PATH.apk` (also `.apkm`, `.apks`, `.xapk`) installs the file and
then runs its package, as `wine app.exe` does. `install` records the SHA-256 of
the file it installed (`source_sha256` in `package.json`); `run FILE` hashes the
file and, when an installed app with an existing root recorded the same hash,
skips the installer and runs that package (it prints `Already installed ...`).
The run still verifies every installed file, so a modified install is caught.
Skipping needs a hash: an app installed before this field existed is installed
once more, which records it. A changed or different file, or an app installed
from another file with the same package id, takes the full install path, which
verifies signatures and the version, as `asgard.py install FILE` always does.
Upgrades and identical-version reinstalls retain app data and saved settings.
An older version requires an explicit downgrade option:

```bash
python3 Scripts/Android/asgard.py install /path/to/older.apkm --allow-downgrade
python3 Scripts/Android/asgard.py run /path/to/older.apk -d
```

`-d` is an alias for `--allow-downgrade`, using the spelling from
[ADB's install options](https://android.googlesource.com/platform/packages/modules/adb/+/refs/heads/main/docs/user/adb.1.md).
Asgard permits this for signed release packages too; the installed signer must
still match and all split/signature/integrity checks still run. Saves, accounts
and launcher settings are retained. Asgard does not convert databases or saves
to an older app's schema; use `clear-data` only if you intend to discard them.

Installation names the missing provisioning step (`fetch-android-runtime.py`,
`fetch-aapt2.py`, `fetch-android-package-tools.py` or
`make android-runtime-adapters`) instead of failing on a missing file.

Installation accepts a single APK or a ZIP/APKM/APKS containing one consistent
base/split set. It does not select among bundletool device variants. Use
`--state-root PATH` before `install`, `run` or `probe` to choose a different state directory.
`run --no-jit` uses the interpreter; `--frames N` bounds redraw cycles for tests,
and zero runs until the window closes. Bifrost flags precede its guest ELF.
`probe --no-jit` also uses the interpreter. Repeat `--library` to select a subset;
the default checks all installed ARM64 libraries. Repeat `--preload BASENAME` to
load packaged prerequisites in the given order before each target, with Bionic's
normal `RTLD_LOCAL` scope. No application-specific ordering is inferred.
`--timeout` bounds each process
to 1..300 seconds (45 by default), including its descendants. Diagnostics print
a bounded output tail, reject fatal markers even with exit zero, and return
nonzero if any library fails. A flushed `LOAD_OK` marker records successful
`dlopen` separately from process exit: `loaded, process failed` still fails the
command. This executes native constructors and cleanup;
it is a loading diagnostic, not an APK launch or a side-effect-free inspection.

Adapter changes require a fresh `--state-root`; the installer deliberately
rejects replacing different adapters inside an existing app root. Keep the
original app state when evaluating a new runtime.

## Uninstall and reset

```bash
python3 Scripts/Android/asgard.py uninstall org.example.package    # code, runtime copies and all data
python3 Scripts/Android/asgard.py clear-data org.example.package   # saves and settings only
```

Both ask for confirmation on a terminal; scripts must pass `--yes`. They fail
while the app runs (stop it first), for provisioned system apps such as microG,
and for packages that are not installed. A failed uninstall deletion reports the
hidden `.removing-*` directory left in the state root; the app is already gone
from the library.

- `uninstall` renames the app directory out of the library, deletes it (the
  installed code is read-only, so directories are made writable first) and frees
  the app's UID. Google accounts in `<state>/.google` are shared and stay.
- `clear-data` empties `data/user/0/PKG`, `data/user_de/0/PKG`, both ART
  profile directories and `storage/emulated/0/Android/{data,media}/PKG` in the
  install root and the managed root, then recreates the empty directories a
  first start expects. It also deletes the app's Android Keystore keys
  (`data/misc/keystore/bifrost-<uid>.json`). It keeps the installed code, OBBs, other shared storage,
  per-root device settings and accounts, and the `data-backup-*` folders.
  Symlinks inside the data are removed, never followed, and a symlinked parent
  directory aborts the command.

The library window offers both as "Clear data…" and "Uninstall…" in an app's
settings, disabled while the app runs.

## Per-app settings

```bash
python3 Scripts/Android/asgard.py config org.example.package                 # list every setting
python3 Scripts/Android/asgard.py config org.example.package network offline # set one
python3 Scripts/Android/asgard.py config org.example.package --reset         # all back to defaults
```

Settings are saved in the app's `launcher-settings.json` and apply to every
start of that app, from the CLI (`start`, `run`) and the library window alike.
Changes take effect at the next start. A setting you never chose is not stored,
so it keeps following the emulator's default.

An app's settings in the launcher are its compatibility profile. General
contains window stats, Back key, network mode, the current bundled Android API,
a button to open the app environment folder, and "Clear data…" and "Uninstall…".
Compatibility contains an
Android emulator flag selector (Runtime default / Off / On) and an editable
property list. Off stores `property.ro.kernel.qemu=0`; arbitrary saved values
are preserved as a Custom choice. Native-only apps show the device controls
disabled because that launch path does not consume them.

Add/remove properties there or use the CLI below; both edit the same profile.
Duplicate names, invalid property names and invalid values keep the dialog open
with an error. **Restore defaults** stages removal of all saved choices;
Save applies it and Cancel discards it. Changes never clear app data. The
bundled runtime is identified rather than offering an unsupported Android
version selector. Runtime version switching, permission policy editing and
backup/restore are not part of this profile editor.

While an app starts, its session records the current preparation step: installed
file verification, media setup, Android runtime preparation, app-code optimization
or process launch. The session sidebar shows that step and updates elapsed time
even when no new log output arrives. Before guest output exists, Open full log
opens the preparation log; it switches to the guest log once that has output.
ART optimization can take time after a runtime rebuild; Starting does not itself
mean the game process has already launched.

| Setting | Values | Effect |
|---|---|---|
| `window_stats` | `on`, `off` | `--window-stats` / `--no-window-stats` (off by default: the window shows only the app's title) |
| `window_stats_mode` | `minimal`, `normal`, `overlay` (or `1`, `2`, `3`) | `--window-stats-mode=` while `window_stats` is on: title only, title and in-window overlay (default), or overlay only; see [window stats](../window-stats.md) |
| `back_key` | `escape`, `backspace`, `delete`, `f1`, `f2`, `f12`, `none` | host key delivered as Back (`BIFROST_ANDROID_BACK_KEY`) |
| `enter_key` | `enter`, `dpad_center`, `button_a` | what Enter sends outside a text field, for games with their own cursor (`BIFROST_ANDROID_ENTER_KEY`); `button_a` reports a gamepad source |
| `jit_disk_cache` | `default`, `on`, `off`, `verify` | experimental reuse of JIT translations across launches (`BIFROST_JIT_DISK_CACHE`); `default` follows `[jit] disk_cache` |
| `jit_disk_cache_mb` | `default`, `128`, `256`, `512`, `1024`, `2048`, `4096` | on-disk budget of that cache for this app (`BIFROST_JIT_DISK_CACHE_MB`); `default` follows `[jit] disk_cache_mb`, normally 512 |
| `network` | `auto`, `offline` | `offline` sets `BIFROST_ANDROID_OFFLINE=1`: the app's sockets cannot reach anything beyond loopback (`ENETUNREACH`), names do not resolve, and Android reports no network |

Managed apps also accept `property.NAME` settings. These override Android
properties in this app's generated runtime, without changing the shared image:

```bash
python3 Scripts/Android/asgard.py config com.cygames.umamusume property.ro.kernel.qemu 0
python3 Scripts/Android/asgard.py config com.cygames.umamusume property.ro.kernel.qemu
python3 Scripts/Android/asgard.py config com.cygames.umamusume property.ro.kernel.qemu --reset
```

Values must be trimmed text without control characters, at most 91 UTF-8 bytes;
an empty string is allowed. `config` lists saved overrides and `config --json`
includes them. The launcher library preserves them when saving its other settings.
They survive app upgrades, downgrades and managed runtime reprovisioning.
Changing or resetting an override rebuilds the generated runtime at the next
start, preserving app data; reset restores the verified image's property values.
The generated property file is checked with the runtime inventory, so edit the
saved configuration rather than `managed/rootfs/debug_ramdisk/adb_debug.prop`.
This supports the managed Android runtime; native-only Activity launch does not
consume these overrides.

Precedence, highest first: an explicit command-line option or an environment
variable you exported (for example `--window-stats`, `BIFROST_ANDROID_OFFLINE=0`),
then the saved setting, then the emulator default. Unknown or malformed values in
the file are ignored. Media decoding is not a per-app setting: its routing belongs
to the shared realm broker.

## Compatibility status

```bash
python3 Scripts/Android/asgard.py compat                         # installed and recorded apps
python3 Scripts/Android/asgard.py compat com.example.package     # status, evidence and known issues
python3 Scripts/Android/asgard.py compat game.apk                # the same, for the package an .apk declares
```

`Scripts/Android/compatibility.json` records what has actually been observed
per app, in four tiers: `playable` (a person played it and confirmed
gameplay), `title` (reached its title, login or main menu on screen),
`runs` (a bounded session reached its window and surface without a fatal
error; nothing more is claimed) and `blocked` (it starts, then the app itself
ends the session). An installed app with no entry is `untested`. Every entry
carries its evidence, the date and, where known, the version, and many link to
an app page under `Docs/Apps/`. The library's app settings show the same text.
This is documentation of past observations, not a guarantee for other
versions, devices or Google-account states; update an entry only from a new
observation, and keep the claim no stronger than its evidence. A test checks
the file's structure and page links.

## Desktop library

`python3 Scripts/Android/asgard.py launcher` opens the optional PyQt6 app
library using the same install and session supervisor as the CLI.

Each card shows a rounded icon tile, the label, version and install date, and one line about
the latest session ("Played Today 13:08", "Running for 25 min", or in red what went wrong, such as "Crashed in native
code"), with Play/Stop, per-app settings and last-session buttons. Colors come from the desktop
palette and the icons are drawn by the launcher, so the look does not depend on an icon
theme. A padded launcher bitmap is cropped to its visible pixels so every tile fills; an app
without an icon gets a tile in one of ten fixed colors with its initial. Cards stretch
between 208 and 280 px to fill the row, and the column count follows the viewport width.
A running or starting app carries a status chip and its Play button becomes a red Stop.

Double-click, Enter or Space plays (or stops) the focused card, arrow keys move between
cards, and the context menu or Menu key offers Play/Stop, app settings, last session,
clear data and uninstall (the last two are disabled while the app runs). Search matches the
label or package ID, and Sort offers Name or Recently played. Dropping `.apk`, `.apkm`,
`.apks` or `.xapk` files anywhere on the window installs them one at a time (the Install
button accepts several files too), and a failure in one does not stop the rest. The window
size and sort order are remembered (Qt settings, `Asgard/Library`). The library refreshes every 1.5 s
while the window is focused or an app is running, every 10 s otherwise, and not at all while
it is minimized; it refreshes at once when the window is activated. Package IDs, library
paths, interpreter launch and Java call tracing are grouped under Developer.

Install records now retain the base APK's label/version and a bounded launcher
bitmap; older records recover labels and versions from their saved base report.
Icon selection prefers a dense daytime bitmap from the declared launcher
resource, then that resource's bitmap fallback. An adaptive icon without one keeps its two layers (a colour, or the densest
bitmap across the base APK and its splits; `adaptive_icon` in `package.json`) and the card composes them as Android does:
each layer is 108 dp and the inner 72 dp show. A layer that is a vector leaves the placeholder. The library runs
`asgard.py refresh-icons` once for apps installed before this, without a reinstall.

A red line means a run really went wrong, and clicking it opens the drawer. `explain_ending` decides from the session and its
log: a known crash signature names the crash, a session lost by the supervisor is "Ended unexpectedly", and an outside
SIGKILL is "Ended from outside". A session's time limit and an app killing its own process with no crash in the log (Android
apps often quit this way) are not failures, so the card just says when it was played. With no known cause it says "Last run
failed". Ad and Play services errors never count as the cause.
An app that exits with status 0 is quiet too, except when its log shows Google Play's licence check failing
(`LicenseClient: Error while checking license`, from the pairip app protection some games ship with, which asks
the Play Store app to confirm the install and quits when it cannot). The card then says "Closed itself: Google Play
licence check", the terminal prints the same, and Flying Gorilla 5.2.13 is an example. Bifrost has no Play Store and does not
fake the answer, so such an app cannot run here; the line only says why it stopped.

### Menu entries (2026-11-04)

Each installed app gets a launcher in the desktop's own menu: `asgard-PACKAGE.desktop` in `$XDG_DATA_HOME/applications`
(default `~/.local/share/applications`), with the app's icon. Its `Exec` line runs `start PACKAGE` through the Asgard that wrote it.
An app's own PNG is used as it is; a JPEG or WebP bitmap, or an adaptive icon, is saved as `desktop-icon.png` beside the app
(composed the way the library does, 256 px, and only when PyQt6 is present; without it the entry has no icon). Apps with no icon, or
whose layer files are missing, get the desktop's generic one. The entry is written after an install, removed after an uninstall, and
refreshed by `refresh-icons`. `asgard desktop` writes entries for every installed app and `asgard desktop --remove` removes
them. Entries are made only for the default state root, so a test's or a scratch state root never reaches the menu, and
`ASGARD_DESKTOP_ENTRIES=0` turns them off. A hand-written file, or one made for another state root, is never removed.

The app's window is named after its entry: Asgard sets `SDL_APP_ID=asgard-PACKAGE` for the game process (unless you set it),
which SDL3 uses as the Wayland application id and the X11 class, so the taskbar button gets the entry's icon. Checked on KDE
Plasma (Wayland): the window of 2048 reported `desktopFileName=asgard-com.tpcstld.twozerogame`. Not checked on GNOME, on X11, or with
a native-activity app (its launch path sets the variable too, untried). `refresh-icons`, which the library also runs once per state
root, restores an adaptive icon whose layer files are missing from the app's directory (The Battle Cats had its record without the
file).

Per-app settings are described under [Per-app settings](#per-app-settings).
The last-session drawer shows saved outcome/timing and
recognized error signatures from at most the final 16 MiB of its log; those
counts are a bounded log summary, not a diagnosis or gameplay result.

## Google account

Experimental: some apps and Google services may not work with it. The library window's "Google account…" button opens the device's Google
accounts: the signed-in list, "Sign in with Google…" (microG's own window with
Google's embedded sign-in page; the password goes to Google, not to Asgard),
"microG settings…" and "Remove". The same actions exist on the command line:
`asgard.py google sign-in|settings`, `google accounts [--json]` and
`google remove ACCOUNT`. One sign-in serves every app: accounts are kept under
`<state>/.google` and imported into an app's root at each launch; microG's
settings stay per app. A completed login needs real credentials and has not
been observed here. See the
[runtime guide](ANDROID_RUNTIME.md#google-accounts-and-sign-in-2026-10-20) for
the account service and its limits. When an app itself starts a Google
screen (a Google Sign-In button, `AccountManager.addAccount`), it opens in the
same kind of separate window and returns to the app; the library window then
says which app asked, and the dialog's hint names it.

## Optional real Media NDK bundle

The Media NDK can run as guest ARM64 code through Bionic. Provision the actual
Android 15/API-35 implementation from the official AOSP emulator system image;
the NDK's `.so` link stubs are not runtime libraries:

```bash
python3 Tools/Android/android_media.py
python3 Scripts/Android/android_package.py --state-root android-state/real-media \
    install /path/to/package.apkm --media-root build/android-media
```

Provisioning requires `debugfs` and `readelf`; it unpacks the dynamic
partitions itself (`Tools/Android/android_super.py`), so AOSP's `lpunpack`, which
Ubuntu does not package, is not needed. The first download is approximately 734 MiB; extraction needs several
GiB of temporary disk space. It reads GPT/dynamic partitions and APEX payloads
without mounting filesystems. The image and complete extracted file set are
SHA-256 pinned. The source image cache and existing bundles are verified before
reuse; a modified bundle is rejected. Upstream notices are retained.

The bundle contains the genuine `libmediandk.so`, its 154-library `DT_NEEDED`
dependency closure (including itself), and ICU data. These libraries load in
an isolated Bionic `media` namespace under `/system/lib64/media`, sharing only
the existing Bionic C/math/dynamic-link libraries. The app namespace can access
`libmediandk.so` through a declared linker namespace link. Its existing
`libandroid`/EGL/GLES/logging adapters continue to serve application calls.
Installation verifies the copied libraries before publishing the root, records
their hashes in the package manifest, and rechecks the files and complete
namespace configuration on launch/probe/reinstall. Enabling, disabling or
changing the media bundle requires a fresh app state; reinstall with the same
`--media-root` to preserve an existing media-enabled installation.

Real `AMediaFormat` allocation, typed fields, copied strings/buffers, format
copy/clear/description and deletion are exercised in the signed APK gate.
No Media NDK replacement functions are supplied. The managed framework's later
[host media checkpoint](ANDROID_RUNTIME.md#host-media-decoding-2026-10-14)
adds real Codec2 services owned by the Binder realm, host decoding and genuine
ImageReader/native-window output. Temporary Java MediaCodec probes decoded
exact 64x64 AVC/HEVC/VP8/VP9 frames with checked timestamps and EOS; separate
NDK AMediaCodec/native-window probes checked forced-GPU output. These checks
are separate from the signed APK gate's MediaFormat assertions. Provisioning
the Media NDK bundle alone does not create those codec services.

The realm-owned software Gralloc5 allocator and guest mapper are also checked
through AHardwareBuffer and AImageReader CPU pixels (see
[allocation validation](ANDROID_RUNTIME.md#software-gralloc5-2026-10-13)).
`media.player` remains absent; unavailable service calls may block. `media.extractor`
is the image's real daemon, and Oddmar's splash video opens through it. Full stream reconfiguration, HDR,
native EGL rendering into an aliased Surface and sustained in-game video playback remain unverified. This is a bounded media
implementation, not arbitrary codec/player/DRM compatibility.

## Package and runtime ownership

The installer snapshots the bounded input, checks archive paths/member types,
verifies every APK with [official apksigner](https://developer.android.com/tools/apksigner),
and requires matching signer certificates, package/version identities and split
dependencies. It rejects duplicate/conflicting native providers and attempts
to replace runtime or adapter libraries. Input is capped at 2 GiB and the
aggregate expanded APK size at 4 GiB, so Play asset packs that unpack past
the input limit still install. Updates require the same signer identity and cannot
downgrade; certificate-rotation lineages are not implemented. With APK
Signature Scheme v3.1 key rotation, apksigner labels each signer with an SDK
range; the installer records the signer whose range covers API 35, the one
Android uses (Roblox signs API 33+ with a rotated key). The launcher is AAPT2's
`launchable-activity`, or else the manifest's first enabled MAIN/LAUNCHER
activity or `activity-alias` (AAPT2 omits launchers that only an alias
declares); an alias resolves to its `targetActivity`.

Each package has an independent rootfs under ignored `android-state/`:

- `/data/app/<package>-<digest>` retains signed APKs, extracted ARM64 libraries
  and merged assets. Code is published by rename and made read-only.
- `/data/user/0/<package>/files` and `cache` retain application data.
- `/storage/emulated/0/Android/data/<package>/files` provides external app storage.
- The root contains copied, verified Bionic libraries and guest adapter DSOs.

Managed apps also get what installd and ART Service provide on a device.
`/data/misc/profiles/{cur/0,ref}/<package>` exist and survive runtime
replacement. The launcher registers the app's code paths with ART
(`LoadedApk.registerAppInfoToArt`, which `systemMain()` otherwise skips), so
ART's ProfileSaver and androidx ProfileInstaller write `primary.prof`. Before each
launch, `dexopt` in `Scripts/Android/android_managed.py` runs the image's
`profman` and `dex2oat64` under Bifrost, as
[artd](https://android.googlesource.com/platform/art/+/refs/heads/android15-release/artd/artd.cc)
does. It merges the current profile into the reference profile and writes
`<code>/oat/arm64/base.{odex,vdex,art}`: `speed-profile` with an app image once
a reference profile exists, otherwise `verify` (the image's install filter
without a profile). `managed/dexopt.json` records the compilation inputs:
verified framework/ART image identity, APK contents and location, boot classpath,
class-loader context, ART properties, compiler filter and reference profile.
Launcher, microG, media routing and unrelated device-property changes (including
`ro.kernel.qemu`) do not invalidate compiled code. Framework image changes remain
conservative invalidations. This follows ART's dependency requirements, especially
the boot classpath; see [ART configuration](https://source.android.com/docs/core/runtime/configure).

Runtime replacement carries matching compiled artifacts forward, verifies their
hashes, and keeps generated `oat/` files outside the immutable package inventory.
Missing or modified artifacts trigger compilation. Older cache records migrate
without compiling only when their original full-runtime key still matches.
Unchanged failures are not retried until a compilation input changes. A failed compile leaves
the app on ART's JIT. Split APK dex is not compiled. SpongeBob (68 MB of dex)
compiles in about 15 s with `verify` and 25 s with a synthetic 5% profile.

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

Before app loading, the guest launcher gives Bionic the packaged library
directory through the pinned `libdl_android` platform entrypoint
`android_update_LD_LIBRARY_PATH`. This makes sibling `DT_NEEDED` libraries
searchable within the isolated process. It is an internal platform API tied to
this runtime, not a public NDK compatibility promise. Bionic's descriptor-path
checks use translated guest `/proc/self/fd` identities and guest-root paths;
the host descriptor with the same number must never decide namespace access.

Guest `libandroid`, `liblog`, `libEGL` and `libGLESv2` adapters resolve explicit
exports into Bifrost's existing thunks. Assembly wrappers preserve AAPCS64
arguments and cache trampoline addresses with acquire/release operations.
Graphics remain behind the [window plugin interface](ANDROID_WINDOW_PLUGINS.md).
The build automatically fetches AOSP Bionic, NativeActivity, window, rectangle,
JNI-type, logging and Media NDK headers at pinned Android-15 repository revisions.
Public ABI declarations come from those headers. Downloaded archives live under
ignored `build/android-dependencies`; bounded extraction accepts regular files
and directories only. Gitiles adds changing archive timestamps, so AOSP pins
cover sorted regular-file names, lengths and contents with SHA-256. Static
upstream zlib archives use their published archive SHA-256. Every cache reuse
is reverified; adapter metadata records source URLs and checksum kinds.

Guest `liblog` formats print/vprint/buffer-print arguments using real Bionic
`vsnprintf`, preserving the ARM64 `va_list` ABI, then sends a fixed string to
the existing stderr bridge. Successful writes return one, as specified by
[Android's logging API](https://developer.android.com/ndk/reference/group/logging).
Formatting is capped at 1023 bytes; buffers share one sink. This does not supply
logd, property-based filtering or the full logging API. Log assertions terminate
through Bionic `abort` rather than reporting success.

`libz.so` is genuine guest ARM64 [zlib 1.3.2](https://zlib.net/), compiled from
the pinned upstream release against the real Bionic library and AOSP headers.
Compression/decompression executes in the guest; no host zlib pointer or
callback ABI crosses this boundary. Its license accompanies the installed DSO.
Private syscalls `0x100D` (adapter resolution) and `0x100E` (event pump/input
queue) require the Android runtime profile. They are not Android kernel APIs.

## Compatibility boundaries

The native-only path accepts the exact `android.app.NativeActivity` class.
On that path, `JavaVM`, `JNIEnv` and the Java Activity object are null; apps must
avoid JNI. Managed launcher classes instead use the separate ART/framework path.
The native-only launcher invokes startup, window/input, redraw and teardown callbacks.
That path does not implement full Activity lifecycle transitions, resource-table
resolution, Java class loading, DEX execution or Android property services.

`ALooper` is per guest thread, as AOSP's `Looper::getForThread` is:
`ALooper_forThread` returns NULL until the thread calls `ALooper_prepare`,
`pollOnce`/`pollAll` use the calling thread's looper (`ALOOPER_POLL_ERROR`
without one), `addFd` rejects a NULL callback unless the looper allows
non-callback sources, and a callback returning 0 unregisters its fd.
`AChoreographer_getInstance` returns the thread's choreographer, or NULL
without a looper. `postFrameCallback`/`64`/`Delayed` callbacks are one-shot and
run inside the owning thread's poll, at the first 60 Hz `CLOCK_MONOTONIC` tick
after they fall due. They receive that tick as the frame time. Vsync callbacks
with frame-timeline data, refresh-rate callbacks and real display timing are
not implemented. The sensor manager reports a device with no sensors: an
empty list, NULL default sensors, and event queues that never have events.
Enabling a sensor fails with `BAD_VALUE`, and direct channels are not
exported.

Asset file open/read/seek/length/buffer/close work on extracted files, capped
at 64 MiB per asset. Directory enumeration, asset file descriptors and OBB
mounting are not implemented. Graphics and platform exports cover the existing
thunk subset; the adapter route does not establish arbitrary NDK or audio
compatibility. Missing adapter exports fail explicitly.

`--android-services` enables the bounded native service-manager subset described in
[ANDROID_BINDER.md](ANDROID_BINDER.md). That flag alone does not start the managed
framework. The managed launcher separately supplies Activity/Package manager
contracts and limited SurfaceFlinger endpoints; it is not a full Android system.
See [framework service scope](ANDROID_RUNTIME.md#managed-framework-service-modules-2026-10-09).

Historical native-loading results: the user-provided SpongeBob
5.11.5 set installs successfully: nine signed APKs
and 21 ARM64 libraries. The normal APK runner now launches
`com.clevertap.unity.CleverTapOverrideActivity` through ManagedLauncher.
Its SurfaceView path provides a window and input; this remains a bounded
framework profile rather than a complete Android system. A separate
[installed-app framework probe](../archive/ANDROID_RUNTIME-history.md#installed-app-creation-checkpoint-2026-10-07)
now reaches the real launcher's `onCreate`, including IL2CPP JNI startup, in
both engines. It exits after creation and does not draw or accept input.
Installation is not game-play compatibility evidence. The earlier native probe
completed loading and process
cleanup for 19/21 libraries under JIT, interpreter and opt-in Tier 2, including
`libmain`, `libil2cpp`, Firebase Analytics/App/AppCheck/Crashlytics and the native
Crashlytics libraries. Unity reaches a missing `libmediandk.so` dependency.
Firebase Messaging finishes `dlopen` but aborts during process exit when probed
directly. Its cause is now traced: App and Messaging both export
`firebase::messaging::ListenerImpl::g_mutex`. Loading Messaging and its App
dependency together makes both constructors register the same mutex address
with `__cxa_atexit`, using distinct DSO handles. The App destructor
`firebase::Mutex::~Mutex` at offset `0x1936a0` destroys it twice; the observed
Bionic state changes from `0x4000` to the destroyed marker `0xffff` before the
second call, matching the [pinned Bionic mutex implementation](https://android.googlesource.com/platform/bionic/+/361ba86734fb2821a6adcfdf775db8abd04e0de0/libc/bionic/pthread_mutex.cpp).
The registrations belong to App (`__dso_handle` at `0x3ddb88`) and
Messaging (`0x139f8`); the shared object resolves to Messaging at `0x140d8`.
These offsets refer to this packaged version, not a general Firebase ABI.

The current SpongeBob 5.12.0 gameplay and remaining WebView/Google Play gaps
are recorded in [its application page](../Apps/spongebob.md). The earlier
19/21-library result above is a native-load diagnostic, not the current managed
game's compatibility status.

Explicitly loading App first with `RTLD_LOCAL` gives each DSO its own mutex
address and completes Messaging cleanup under JIT, interpreter and opt-in
Tier 2. QEMU reproduces both the double destruction and the clean App-first
exit. This isolates a load-order problem in the standalone probe; it does not
establish the managed game's initialization order or successful JNI startup.
The probe preserves normal Bionic checks and exit handlers. JNI entrypoints are
located, never invoked. Media services, ART and framework startup remain required.
With the explicit App preload, the full 21-library JIT probe completes 20/21
loads and clean exits in the earlier adapter-only checkpoint; Unity remains
blocked on `libmediandk.so` there. With the optional real-media bundle, that
dependency resolves. The guest EGL adapter now exports every EGL function in
Android's `libEGL.map.txt` that has a host equivalent, which covers all of
Unity's EGL and GLES imports. `libandroid.so` now also exports
`ALooper_forThread`, `AChoreographer_*` frame callbacks and the `ASensor*`
API, so every Unity import resolves. With vector `FADDP` implemented (a
Unity constructor runs `faddp v17.2s, v17.2s, v18.2s`), `libunity.so` loads
and exits cleanly under the JIT and the interpreter, and the full probe with
the Firebase App preload loads 21/21 libraries with 21/21 clean exits (JIT,
fresh `android-state/choreo-sensor`). JNI entrypoints are still not invoked.
The local real-media installation uses `android-state/real-media`.

The earlier header checkpoint uses `android-state/bionic-headers`. The updated
launcher installation uses a fresh `android-state/bionic-mutex-final`:

```bash
python3 Scripts/Android/android_package.py --state-root android-state/bionic-mutex-final \
    probe com.tiltingpoint.spongebob --library libFirebaseCppMessaging.so \
    --preload libFirebaseCppApp-13_8_0.so
```

## Verification

```bash
make test-android-package
./Scripts/Testing/run_tests.sh --strict --filter android_service_manager
./Scripts/Testing/run_tests.sh --strict --no-jit --filter android_service_manager
```

The APK gate requires a native Wayland desktop and GPU and provisions the
pinned real-media bundle above. It builds and signs a
base/split NativeActivity fixture with actual AAPT2/API-35 manifest resources
and apksigner. All six JIT/interpreter/Tier-2 and built-in/plugin combinations
pass lifecycle, pthread/TLS, split-asset, save persistence, EGL pixel readback
and service-handle invocation checks. The signed fixture also verifies sibling
dependency loading, mixed integer/FP/string/stack variadic logs, a known CRC,
an independently generated compressed stream and zlib round trips. Its two
libraries pass the standalone loading probe. Ordered preloads and a real guest
exit-handler failure also pass: the latter preserves `LOAD_OK` while failing
the command with guest exit 23. The six host test cases also cover path
and symlink rejection, input growth bounds, signature tampering, signer mismatch,
version mismatch, downgrade prevention, reordered reinstall, modified installed
code, and native-launcher boundaries. An additional signed managed fixture now tests
ART Activity startup, resource labels, persistent preferences and ordered
Activity.finish teardown over two launches; see the managed launcher guide.
The real Media NDK additionally passes 256 repeated metadata ownership/type
cycles per fixture process in all six backend/execution-mode combinations.
These checks exercise metadata, not Android media services or decoding.
Modified installed media libraries and namespace configuration are rejected
before the emulator can execute.

The complete Linux regression suite passes 273/273 JIT runs and 266/266
interpreter runs with seven JIT-only skips. The real Bionic bootstrap separately
passes all six fixed/random ASLR and execution-mode configurations.
For the preceding Bionic/header refinement, the full Linux runs initially
rejected two stale fixtures;
after rebuilding, both cases passed in each mode. Aggregate coverage remains
273 passing JIT runs and 266 passing interpreter runs with seven JIT-only skips.

### HOME and returning to a running app

A managed app's MAIN/HOME Intent, or supported `am start` HOME command, opens
Asgard for its state directory and backgrounds the Activity task. Select
**Return to app** to resume that instance; the CLI equivalent is
`asgard resume PACKAGE`. The pinned Android shell and native `am` client are
provisioned with managed roots. This requires PyQt6 for Asgard and a window
backend supporting task visibility; it does not keep alive an app that exits
after HOME. See the runtime guide for supported command options and the
remaining Java `Runtime.exec`/vfork limitation.
