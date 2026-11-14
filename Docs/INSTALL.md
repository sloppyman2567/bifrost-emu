# Install and update

The plain steps. The [README](../README.md) has the full picture and the
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

## Update

Pull the new code first (`git pull` in your clone), then pick the line that matches how you installed.

| You installed with | Update with |
|---|---|
| `make bootstrap` (under `~/.local`) | `make bootstrap` |
| `sudo make install` and `sudo make install-asgard` (under `/usr/local`) | `make`, then `sudo make install`, then `sudo make install-asgard` |
| The Debian or Ubuntu packages | `make deb`, `sudo apt install ../bifrost-emu*.deb`, then `asgard-setup` |

Run `make` as yourself, never with `sudo`. If you forget, `sudo make install` stops and tells you to run `make` first.

Updating only one of the emulator and Asgard is the usual mistake. Asgard now notices and prints a warning when it is started with a
`bifrost-emu` it was not installed for. The fix is the update line above, in full.

`sudo make install-asgard` rebuilds whatever the update changed (the app launcher, the adapters, the Android framework files),
so there is nothing extra to run. If an update makes the Android framework image unusable, it is deleted and rebuilt by itself,
which downloads the Android image again (734 MiB).

## Remove

```bash
make uninstall-asgard             # the Android part
make uninstall                    # the emulator
make uninstall-rootfs             # the Linux library root, if you installed one
```

Add `PREFIX=$HOME/.local` to each if you installed with `make bootstrap`. Your apps and their data stay in
`~/.local/share/bifrost` until you delete that folder.

## If something goes wrong

| You see | Do this |
|---|---|
| `sudo make install` says to run `make` first | Run `make` as yourself, then `sudo make install` again. |
| Asgard warns about a different `bifrost-emu` build | Update both: `make && sudo make install && sudo make install-asgard`. |
| An app will not start and says the managed launcher does not match its sources | `sudo make install-asgard`, or `make android-provision` in your clone. |
| A tool is missing | `make deps` installs the host packages. |
| A download stopped | Run the same command again. |
| Something else | See the [support matrix](SUPPORT.md), then open an [issue](https://github.com/sloppyman2567/bifrost-emu/issues). |
