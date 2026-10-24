# Guest rootfs

A guest rootfs holds the AArch64 libraries (glibc and musl) that dynamically
linked programs load. bifrost-emu finds one without flags, like
`qemu-aarch64 -L`. It contains no `/proc` or `/dev` content; those are
synthesized at run time.

## Where bifrost-emu looks

1. `--rootfs PATH`, `BIFROST_ROOT` or `[paths] rootfs`: PATH is the library root
   **and** the guest's `/`. Absolute guest paths resolve inside it and escape
   attempts are blocked (see the VFS guide).
2. Otherwise the first of these that contains `lib/ld-linux-aarch64.so.1`,
   `lib64/ld-linux-aarch64.so.1` or `lib/ld-musl-aarch64.so.1`:
   `<executable dir>/rootfs`, `<executable dir>/../share/bifrost/rootfs`,
   `$XDG_DATA_HOME/bifrost/rootfs` (default `~/.local/share/bifrost/rootfs`),
   `/usr/local/share/bifrost/rootfs`, `/usr/share/bifrost/rootfs`.
   This one supplies libraries only; the guest still sees host paths, except
   that its library directories resolve in the rootfs (below).

**Profiles.** A named rootfs is installed beside the default one, in `<data>/bifrost/rootfs.d/NAME`
(`$XDG_DATA_HOME`, `<prefix>/share/bifrost`, `/usr/local/share/bifrost`, `/usr/share/bifrost`, or `<executable dir>/rootfs.d/NAME`).
`--rootfs-profile NAME`, `BIFROST_ROOTFS_PROFILE=NAME` or `[paths] profile = "NAME"` selects it; `bifrost-emu --rootfs-list`
shows the installed ones (`*` marks the one in use). A profile supplies libraries only, like the default rootfs, and does
not confine guest paths; `--rootfs` / `BIFROST_ROOT` still sets the sandboxed root and wins over a profile. A profile that is
not installed is an error naming where it was looked for, never a fall back to another rootfs.

**Library overlay.** With a discovered rootfs (no `--rootfs`), a read-only access by
the guest to an absolute path under `/lib`, `/lib64`, `/usr/lib`, `/usr/lib64` or
`/etc/ld.so.cache` (open, stat, access, readlink) is answered by the rootfs and by
nothing else: a missing file is `ENOENT`, never the host's x86-64 file at that path.
Resolution is confined inside the rootfs, so its absolute symlinks stay in it. This is
what lets a guest `ld.so` (`--guest-ld`) find its libraries. Every other path, and every
write, goes to the host as before; with `--rootfs` the whole sandbox applies instead.

**Host graphics libraries.** A rootfs may hold real AArch64 Mesa (`libEGL.so.1`, libGL,
libwayland-*, ...). The dynamic linker still binds those sonames to the host GL/display
thunks, so the graphical tests behave the same with or without an adjacent `./rootfs`;
only a copy the program brings itself (its RUNPATH/RPATH, `LD_LIBRARY_PATH`) is loaded
as guest code. Under `--guest-ld` a real rootfs file is used, as with the native linker.

`<prefix>/bin/bifrost-emu` therefore finds `<prefix>/share/bifrost/rootfs`
whatever the prefix. With none found, a program whose libc is missing stops with
a message listing these places.

## Installing one

```sh
./bifrost-emu --setup-rootfs                 # same as install-rootfs.sh below, with its options
./Scripts/Setup/install-rootfs.sh            # for your user (~/.local/share/bifrost/rootfs)
./Scripts/Setup/install-rootfs.sh --full     # also the real-world and curl libraries
./Scripts/Setup/install-rootfs.sh --profile NAME   # a named rootfs: <data>/rootfs.d/NAME
./Scripts/Setup/install-rootfs.sh --games    # profile "games": plus OpenAL, SDL2_ttf, Vorbis, libpng, libjpeg, libcurl, ...
sudo make install install-rootfs             # system-wide, in $(PREFIX)/share/bifrost/rootfs
./Scripts/Setup/install-rootfs.sh --uninstall   # or: sudo make uninstall-rootfs
```

`bifrost-emu --setup-rootfs [OPTIONS]` finds `Scripts/Setup/install-rootfs.sh` beside the executable (a checkout
or `build/<config>/`), runs it with the options and exits with its status; an executable installed without
the checkout exits 127 and says so. `install-rootfs.sh` downloads the glibc and musl toolchains into `Tools/` (about
235 MB) when they are missing, builds the rootfs with `setup-rootfs.sh` in a
staging directory beside the destination and renames it into place, so a failed
run keeps the previous install. The `make` targets never download: run the
script once as your user before `sudo make install-rootfs`. `DESTDIR` and
`PREFIX` work as for `make install`. The installed base rootfs is about 70 MB.

### Game libraries

`--games` (profile `games` unless `--profile` says otherwise) runs `Scripts/Setup/fetch-game-libs.py`, which downloads
pinned Debian `trixie` arm64 packages for the libraries games link against and their dependencies (about 50 packages),
unpacks only their shared libraries into `usr/lib`, rebuilds `ld.so.cache` and records the packages in
`etc/bifrost-game-libs.txt`. Checked: Packages.xz against the `Release` file's SHA-256 and every `.deb` against the index
(SHA-256 and size), all over HTTPS; the `Release` signature is not verified. Never installed: the C runtime and the
libraries bifrost-emu serves from the host (ALSA, PulseAudio, PipeWire, SDL2, Mesa/GL, X11, Wayland, ...), because a real copy
would shadow the host-backed one. The install fails when a package needs a newer glibc than the rootfs has. No game data
is fetched. `bifrost-emu --setup-rootfs --games` runs it; then `bifrost-emu --rootfs-profile games --guest-ld ./game`.

## Contents

`lib/` and `lib64/` (glibc and musl loaders, libc, libm, libgcc_s, NSS modules),
`usr/lib/` (libstdc++, gconv), `etc/` (passwd, group, hosts, nsswitch,
`ld.so.cache` for `--guest-ld`, the installer's timezone), `bin/sh` when
`Tests/Apps/toybox` exists, empty `tmp`/`proc`/`dev`/`sys` mountpoints, and
Android-style `/system`, `/vendor`, `/data`, `/sdcard` scaffolding.

The checkout's own `rootfs/` is a development tree (games, Android state) and is
not what gets installed. Android runtime provisioning has its own contract; see
the [Android guide](Android/ANDROID_RUNTIME.md).
