# Guest rootfs

A guest rootfs holds the AArch64 libraries (glibc and musl) that dynamically
linked programs load. bifrost-emu finds one without flags, like
`qemu-aarch64 -L`. It contains no `/proc` or `/dev` content; those are
synthesized at run time.

## Where bifrost-emu looks

1. `--rootfs PATH`, `BIFROST_ROOT` or `[paths] rootfs`: PATH is the library root
   **and** the guest's `/`. Absolute guest paths resolve inside it and escape
   attempts are blocked (see the VFS guide). It wins over everything below.
2. Otherwise the first of these that contains `lib/ld-linux-aarch64.so.1`,
   `lib64/ld-linux-aarch64.so.1` or `lib/ld-musl-aarch64.so.1`:
   `<executable dir>/rootfs`, `<executable dir>/../share/bifrost/rootfs`,
   `$XDG_DATA_HOME/bifrost/rootfs` (default `~/.local/share/bifrost/rootfs`),
   `/usr/local/share/bifrost/rootfs`, `/usr/share/bifrost/rootfs`.
   With a profile selected (below), the same five places are searched for its
   `rootfs.d/NAME` instead.
   This one supplies libraries only; the guest still sees host paths, except
   that its library directories resolve in the rootfs (library overlay, below).

`<prefix>/bin/bifrost-emu` therefore finds `<prefix>/share/bifrost/rootfs`
whatever the prefix. With none found, a program whose libc is missing stops with
a message listing these places.

### Profiles

A named rootfs is installed beside the default one, in `rootfs.d/NAME` under the same data directories
(`<data>/bifrost/rootfs.d/NAME`, `<executable dir>/rootfs.d/NAME`). Select it with `--rootfs-profile NAME`,
`BIFROST_ROOTFS_PROFILE=NAME` or `[paths] profile = "NAME"`; `bifrost-emu --rootfs-list` shows the installed ones
(`*` marks the one in use). `bifrost-emu --rootfs-default NAME` saves one in your config so you need no flag (`default` goes back),
changing only that line and warning if another config file or the environment overrides it. A profile supplies libraries only and does not confine guest paths, like the default
rootfs; `--rootfs` / `BIFROST_ROOT` still sets the sandboxed root and wins over a profile. A profile that is not
installed is an error naming the places searched, not a fall back to another rootfs. Names are letters, digits,
`.`, `_` and `-`, up to 64, not starting with `.`.

### Library overlay

With a discovered rootfs or profile (no `--rootfs`), a read-only access by
the guest to an absolute path under `/lib`, `/lib64`, `/usr/lib`, `/usr/lib64` or
`/etc/ld.so.cache` (open, stat, access, readlink) is answered by the rootfs and by
nothing else: a missing file is `ENOENT`, never the host's x86-64 file at that path.
Resolution is confined inside the rootfs, so its absolute symlinks stay in it. This is
what lets a guest `ld.so` (`--guest-ld`) find its libraries. Every other path, and every
write, goes to the host as before; with `--rootfs` the whole sandbox applies instead.

### Host-backed libraries

Under the built-in linker, a rootfs may hold real AArch64 Mesa (`libEGL.so.1`, `libGL`, `libGLESv2`,
`libwayland-*`, `libgbm`, `libvulkan`), but those sonames still bind to the host GL/display thunks, so graphical
programs behave the same with or without a rootfs that has them. A copy the program brings itself (its
RUNPATH/RPATH, `LD_LIBRARY_PATH`) is loaded as guest code, and so is `libX11` (the X11 wire-protocol test needs the
real one). Audio libraries load their real copy when the rootfs has one. Under `--guest-ld` a real rootfs file is
always used, as with the native linker. A rootfs without the real library falls back to the thunk; the thunk's OpenAL
is a small stub, which is why Linux games need the `games` profile below.

## Installing one

```sh
./bifrost-emu --setup-rootfs                 # same as install-rootfs.sh below, with its options
./Scripts/Setup/install-rootfs.sh            # for your user (~/.local/share/bifrost/rootfs)
./Scripts/Setup/install-rootfs.sh --full     # also the real-world and curl libraries
./Scripts/Setup/install-rootfs.sh --profile NAME   # a named rootfs: <data>/rootfs.d/NAME
./Scripts/Setup/install-rootfs.sh --games    # profile "games": plus OpenAL, SDL2_ttf/image/mixer, Vorbis, libsndfile, libpng, libjpeg, libcurl, ...
./Scripts/Setup/install-rootfs.sh --games --rebuild   # rebuild the profile from scratch instead of updating it
sudo make install install-rootfs             # system-wide, in $(PREFIX)/share/bifrost/rootfs
./Scripts/Setup/install-rootfs.sh --uninstall   # or: sudo make uninstall-rootfs
```

`bifrost-emu --setup-rootfs [OPTIONS]` runs `Scripts/Setup/install-rootfs.sh` with the same options and exits with
its status. It finds the script beside the executable (a checkout or `build/<config>/`) or in an installed package's
`<prefix>/share/bifrost/setup`, which it first copies to `$XDG_CACHE_HOME/bifrost/setup` (default `~/.cache/bifrost/setup`)
because the scripts download and build next to themselves; downloaded toolchains stay there between runs. An executable
with neither exits 127 and says so.

`install-rootfs.sh` downloads the glibc and musl toolchains into `Tools/` (about
235 MB) when they are missing, builds the rootfs with `setup-rootfs.sh` in a
staging directory beside the destination and renames it into place, so a failed
run keeps the previous install. The `make` targets never download: run the
script once as your user before `sudo make install-rootfs`. `DESTDIR` and
`PREFIX` work as for `make install`. The installed base rootfs is about 70 MB.

### Game libraries

The rootfs and its profiles are for Linux programs; Android apps use neither, because Asgard brings its own.
`--games` (profile `games` unless `--profile` says otherwise) runs `Scripts/Setup/fetch-game-libs.py`, which downloads
pinned Debian `trixie` arm64 packages for the libraries Linux games link against and their dependencies (about 70 packages: OpenAL,
SDL2_ttf/image/mixer, Vorbis/Opus/FLAC/mpg123/libsndfile, libpng/jpeg/tiff/webp, FreeType, libcurl with its TLS stack, SQLite, libxml2),
unpacks only their shared libraries into `usr/lib`, rebuilds `ld.so.cache` and records the packages in
`etc/bifrost-game-libs.txt`. Checked: Packages.xz against the `Release` file's SHA-256 and every `.deb` against the index
(SHA-256 and size), all over HTTPS; the `Release` signature is not verified. Never installed: the C runtime and the
libraries bifrost-emu serves from the host (ALSA, PulseAudio, PipeWire, SDL2, Mesa/GL, X11, Wayland, ...), because a real copy
would shadow the host-backed one. The install fails when a package needs a newer glibc than the rootfs has. No game data
is fetched. When the destination already is a rootfs, `--games` first runs `fetch-game-libs.py --check`, which compares
`etc/bifrost-game-libs.txt` with the suite's current index: if nothing is new or changed it stops, otherwise it copies the
installed rootfs to a staging directory, downloads only the new or changed packages, rebuilds `ld.so.cache` and renames it
into place (no toolchains needed; packages dropped from the list leave their old files behind until `--rebuild`).
A program that fails with `libcurl.so.4: cannot open shared object file` was run without the profile: add
`--rootfs-profile games`. `bifrost-emu --setup-rootfs --games` runs it; then `bifrost-emu --rootfs-profile games ./game`
(add `--guest-ld` to run under the rootfs's own `ld.so`). dhewm3 ran 120 s under each linker this way.

## Contents

`lib/` and `lib64/` (glibc and musl loaders, libc, libm, libgcc_s, NSS modules),
`usr/lib/` (libstdc++, gconv), `etc/` (passwd, group, hosts, nsswitch,
`ld.so.cache` for `--guest-ld`, the installer's timezone), `bin/sh` when
`Tests/Apps/toybox` exists, empty `tmp`/`proc`/`dev`/`sys` mountpoints, and
Android-style `/system`, `/vendor`, `/data`, `/sdcard` scaffolding.

The checkout's own `rootfs/` is a development tree (games, Android state) and is
not what gets installed. Android runtime provisioning has its own contract; see
the [Android guide](Android/ANDROID_RUNTIME.md).
