# Install and update

The plain steps. The [README](../README.md) is the short version and the
[Android guide](Android/ANDROID_APK.md) has every option. What has been checked on which distribution is in the
[support matrix](SUPPORT.md).

You need an x86_64 Linux PC with SSE4.1 and FMA3 (the JIT will not start without them; `--no-jit` runs on any x86-64 CPU),
about 1.5 GB to download and about 1.1 GB of disk for the Android part.

## Install

```bash
git clone https://github.com/sloppyman2567/bifrost-emu && cd bifrost-emu
make bootstrap
```

That installs the packages it needs (it asks for `sudo` only if some are missing), builds the emulator, downloads the Android
runtime and installs everything under `~/.local`. Add `~/.local/bin` to your `PATH`. If a download stops, run the command again:
it resumes.

Then:

```bash
bifrost-emu game.apk              # installs and runs an Android app
asgard launcher                   # a library of the apps you installed
bifrost-emu ./program-arm64       # runs an ARM64 Linux program
bifrost-emu --setup-rootfs        # once, if a Linux program needs shared libraries
```

The [rootfs guide](ROOTFS.md) has the options of `--setup-rootfs` (`--games`, `--full`, `--system`, `--uninstall`).

## Update

Pull the new code (`git pull` in your clone), then run the line that matches how you installed. Run `make` as yourself, never
with `sudo`.

| You installed with | Update with |
|---|---|
| `make bootstrap` (under `~/.local`) | `make bootstrap` |
| `sudo make install` and `sudo make install-asgard` (under `/usr/local`) | `make`, then `sudo make install`, then `sudo make install-asgard` |
| The Debian or Ubuntu packages | `make deb`, `sudo apt install ../bifrost-emu*.deb`, then `asgard-setup` |

Update the emulator and Asgard together: Asgard prints a warning when it runs with a `bifrost-emu` it was not installed for.
`sudo make install-asgard` rebuilds whatever the update changed. If an update leaves the Android framework image unusable, it is
deleted and rebuilt, which downloads the Android image again (734 MiB).

## Remove

```bash
make uninstall-asgard             # the Android part
make uninstall                    # the emulator
make uninstall-rootfs             # the Linux library root, if you installed one
```

Add `PREFIX=$HOME/.local` to each if you installed with `make bootstrap` (without it they look under `/usr/local` and report
nothing to remove). The Linux game libraries are a separate root: `./Scripts/Setup/install-rootfs.sh --uninstall --profile games`.
Your apps and their data stay in `~/.local/share/bifrost` until you delete that folder.

## Other ways to install

### Debian and Ubuntu packages

Ubuntu 25.10 or newer is needed for Android windows.

```bash
sudo apt install build-essential debhelper pkg-config libavcodec-dev libavutil-dev libswscale-dev \
     libegl-dev libffi-dev libgl-dev libsdl3-dev libsdl2-dev libwayland-dev libx11-dev
make deb                                  # writes ../bifrost-emu_2.0.0~beta.2_amd64.deb
sudo apt install ../bifrost-emu_*.deb     # the emulator
bifrost-emu --setup-rootfs                # once, for Linux programs: downloads the toolchains and builds the guest rootfs
```

Android apps need Asgard, which `make deb` also packages as `bifrost-emu-asgard`:

```bash
sudo apt install ../bifrost-emu-asgard_*.deb      # also pulls in clang, lld and a headless JDK
asgard-setup                                      # once: ~1.5 GB of pinned downloads, then installs ~1.1 GB under ~/.local
```

### For all users, under /usr/local

```bash
make deps                                 # once: the host packages
make -j$(nproc)                           # build as yourself, never with sudo
sudo make install install-rootfs          # the emulator, and the Linux library root if you want one
make provision-asgard PREFIX=$HOME/.local # Android: about 1.5 GB of pinned downloads
sudo make install-asgard                  # then the Android part
```

### Only the emulator, without Android

```bash
make deps                                 # once: the host packages
make -j$(nproc)                           # build (SDL2/GL thunks are detected)
./bifrost-emu hello.elf                   # run a static ARM64 binary
./bifrost-emu -v Tests/Apps/fib.elf       # print execution stats on exit
```

### An immutable Fedora such as Bazzite

Build and run inside a [Distrobox](https://distrobox.it) container, since the emulator links the container's libraries:

```bash
distrobox create --name bifrost --image registry.fedoraproject.org/fedora-toolbox:44 && distrobox enter bifrost   # Bazzite only
make deps
make -j$(nproc)
./bifrost-emu --setup-rootfs              # once: the libraries Linux programs need
./bifrost-emu --setup-rootfs --games      # for Linux games; --rootfs-default games then uses them by default
```

This was checked in a Fedora 44 chroot on one machine, without Distrobox, a display or a GPU: see the [support matrix](SUPPORT.md).

## Good to know

- `make bootstrap` and `make deps` need `make`, and bootstrap also needs Python. On a minimal system without them, run
  `Scripts/Setup/install-deps.sh` first: it needs only bash and installs both.
- Nothing needs root for what is already installed. A tool that is present but not on your `PATH` is found anyway (the places are
  listed in the [APK guide](Android/ANDROID_APK.md); `BIFROST_TOOL_DIRS=DIR` adds a directory).
- Where the distribution has no SDL3 (Debian 12 and 13, Ubuntu 24.04), bootstrap builds SDL 3.4.18 from the pinned release into
  `Tools/sdl3` and installs it beside the emulator.
- If Bifrost is already installed elsewhere, such as `/usr/local`, `make bootstrap` makes a second copy of about 1.1 GB, which
  takes over if `~/.local/bin` comes first on your `PATH`.
- `asgard launcher` needs PyQt6, which `make deps` and `make bootstrap` install; `bifrost-emu app.apk` works without it.
- `make bootstrap` is new and not yet confirmed end to end, and `make deps` has only been shown to work on some distributions: see
  the [support matrix](SUPPORT.md).

## If something goes wrong

| You see | Do this |
|---|---|
| `sudo make install` says to run `make` first | Run `make` as yourself, then `sudo make install` again. |
| Asgard warns about a different `bifrost-emu` build | Update both, as in the table above. |
| An app will not start and says the managed launcher does not match its sources | `sudo make install-asgard`, or `make android-provision` in your clone. |
| A tool is missing | `make deps` installs the host packages. |
| Something else | See the [support matrix](SUPPORT.md), then open an [issue](https://github.com/sloppyman2567/bifrost-emu/issues). |
