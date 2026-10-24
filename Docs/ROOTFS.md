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

**Library overlay.** With a discovered rootfs (no `--rootfs`), a read-only access by
the guest to an absolute path under `/lib`, `/lib64`, `/usr/lib`, `/usr/lib64` or
`/etc/ld.so.cache` (open, stat, access, readlink) is answered by the rootfs and by
nothing else: a missing file is `ENOENT`, never the host's x86-64 file at that path.
Resolution is confined inside the rootfs, so its absolute symlinks stay in it. This is
what lets a guest `ld.so` (`--guest-ld`) find its libraries. Every other path, and every
write, goes to the host as before; with `--rootfs` the whole sandbox applies instead.

`<prefix>/bin/bifrost-emu` therefore finds `<prefix>/share/bifrost/rootfs`
whatever the prefix. With none found, a program whose libc is missing stops with
a message listing these places.

## Installing one

```sh
./Scripts/Setup/install-rootfs.sh            # for your user (~/.local/share/bifrost/rootfs)
./Scripts/Setup/install-rootfs.sh --full     # also the real-world and curl libraries
sudo make install install-rootfs             # system-wide, in $(PREFIX)/share/bifrost/rootfs
./Scripts/Setup/install-rootfs.sh --uninstall   # or: sudo make uninstall-rootfs
```

`install-rootfs.sh` downloads the glibc and musl toolchains into `Tools/` (about
235 MB) when they are missing, builds the rootfs with `setup-rootfs.sh` in a
staging directory beside the destination and renames it into place, so a failed
run keeps the previous install. The `make` targets never download: run the
script once as your user before `sudo make install-rootfs`. `DESTDIR` and
`PREFIX` work as for `make install`. The installed base rootfs is about 70 MB.

## Contents

`lib/` and `lib64/` (glibc and musl loaders, libc, libm, libgcc_s, NSS modules),
`usr/lib/` (libstdc++, gconv), `etc/` (passwd, group, hosts, nsswitch,
`ld.so.cache` for `--guest-ld`, the installer's timezone), `bin/sh` when
`Tests/Apps/toybox` exists, empty `tmp`/`proc`/`dev`/`sys` mountpoints, and
Android-style `/system`, `/vendor`, `/data`, `/sdcard` scaffolding.

The checkout's own `rootfs/` is a development tree (games, Android state) and is
not what gets installed. Android runtime provisioning has its own contract; see
the [Android guide](Android/ANDROID_RUNTIME.md).
