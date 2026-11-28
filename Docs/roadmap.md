# Roadmap

Current version: **2.0.0-rc.1**, a release candidate (November 25, 2026).

This is what's planned next. Nothing here is a promise or a release date. What already changed is in the [changelog](CHANGELOG.md), and older plans
with all their details are in the [archive](archive/roadmap-2026-11-25.md).

## Goals for 2.0

- Run Android games on the real Android 15 runtime, installed and started from the desktop library.
- Run Linux games at a good frame rate. They start from the command line.
- Make mouse, keyboard, touch and gamepads all work properly.
- Don't break programs that already work.

### What's missing

rc.1 is close, but it isn't production grade:

- Android is still experimental. Some media and system services are missing, and Google services come from microG.
- It's mostly been tested on one AMD desktop. CI only covers Ubuntu 24.04 without a display, and the Steam Deck only in Desktop mode.
- Long sessions need more testing.
- The new JIT work in rc.1 (translating on several threads, and Carry) hasn't been measured in a game yet.

## Next release priorities

In this order.

### 1. Less overhead in translated code

Translated code does a lot of extra work around the game's own instructions: loading and storing guest registers, copying vectors, saving flags and
checking memory addresses. In Zenless Zone Zero and Umamusume, loading and storing guest state alone was about a quarter of the translated code's work
([numbers](archive/JIT-history.md#game-profiles-zenless-zone-zero-and-umamusume-2026-11-23)).

- **Flags:** stop saving them before every memory access (about 6.5% of CoreMark's translated code).
- **Loops:** stop reloading values that don't change and storing ones that get overwritten. Keep two more registers across loops, and align loop bodies.
- **Between blocks:** merge blocks that only have one way in, and make jumps and returns cheaper.
- **Vectors:** use three-operand AVX-128 instructions to avoid copies, and stop using 256-bit instructions for 128-bit shifts.
- **Memory checks:** measure how much they cost in a game, then do register pairs.
- **Interpreter fallbacks:** `ldxp`/`stxp`, some narrowing shifts, and whatever else shows up while Android starts.
- **Carry regions:** keep registers loaded across loops made of several blocks ([design](JIT.md#planned-codegen-retaining-guest-state-across-regions)).

It's done when CoreMark, the game-style tests and a real game all run fewer host instructions, with no new bugs and no slower Android start.

### 2. Smoother games

- **Lag inside ads.** Ads load without much stutter, but games lag once the ad itself is running, such as a playable ad. This hasn't been profiled yet, so the
  first step is a frame-time log of one.
- **Frame times.** Nothing records them today, and an average FPS hides hitches. Add a log that can be switched on.
- **Translating new code.** Several threads can do it at once now, but no game's loading time has been measured with it. Next is Lean first, Carry when hot: new
  code gets the quick translation (4.8 µs a block instead of 9.0 µs in Android's start-up), and only code that runs a lot gets the better one.
- **Keeping translations.** Turn the disk cache on by default once a game shows a gain, and keep hot code when the code cache fills up.
- **Textures.** A faster ASTC to BC7 encoder, and a cache that survives a restart.
- **Vulkan frames.** Every frame is copied through the CPU. Share buffers with the GPU instead (dma-buf).
- **Shaders.** Not measured yet.

It's done when load times and frame times (median, p95 and p99) of a few games are measured and better.

### 3. A glibc 2.44 rootfs

The rootfs has glibc 2.42, from Arm's GNU toolchain 15.2. The next version should have 2.44, which came out in July 2026.

- **Where to get it.** Arm had no newer toolchain on November 25, 2026. Ubuntu 26.10 has 2.44 for arm64, and so does Debian experimental; building it ourselves is
  the other option. Either way the download has to be pinned and checked like the others.
- **The built-in linker.** It fills in fields of glibc's internal `_rtld_global_ro`, and where they are depends on the glibc version. It has to learn 2.44's layout,
  unless `--guest-ld` is the default by then.
- **System calls.** glibc 2.43 added `mseal` and `openat2`. Bifrost's `mseal` reports success without sealing anything, and its `openat2` ignores the `resolve` field.
- **Configuration.** 2.44 reads `/etc/tunables.conf` through `ldconfig`, so check that Bifrost's own `ld.so.cache` still works.
- **Existing installs.** Updating must replace the old libraries cleanly. The `games` profile's Debian packages were built for an older glibc and should keep working.

It's done when the dynamic linking, threads and TLS tests and the Linux games pass on 2.44 with both linkers, on a fresh install and on an update.

### 4. Setup and the app library

- Try the whole setup on a fresh machine, including Debian 13.
- Fix the black screen in Game Mode on the Steam Deck, and add a shortcut that starts a game directly.
- Start the right build of the emulator, and don't let a headless test build replace it.
- Test install, play, settings, stop and start again, with longer sessions and several apps at once.

### 5. Ads, WebView and windows

- Clicks, scrolling and text input inside WebView pages, and links that return from the browser to the app.
- SpongeBob's ad that gets stuck, and rewarded ads with the SDKs' own test ads.
- YouTube keeps its landscape layout when the window gets narrow.
- Check that a minimised game really pauses on KWin and other Wayland desktops.
- Check rotation in Umamusume, and opening maximised in Geometry Dash Lite and Family Island.
- WebView processes use 350 to 450 MiB each and sometimes don't stop when they should (Battle Cats).

### 6. More games

- Azur Lane's login screen is slow to react.
- Zenless Zone Zero's models are very low resolution.
- Firefox opens but loads no pages. It hasn't been tried since the Binder fix of November 1.
- Try more engines, with sound, saves, resizing and pausing.

## Everything else

### Android

- Settings screens, notifications on the desktop, and network discovery.
- Alarms and jobs that survive a restart, and a proper permission model.
- More of Bionic and Android's linker namespaces.
- Audio effects, video playback to a Surface, and longer media sessions.
- Lower memory use.

### Input

- Rumble, back paddles, button remapping, and scrolling with the right stick in cursor mode.
- Test with a plain Xbox-style pad and in Game Mode.
- Gamepads in Linux games, keyboard layouts and held keys.

### WebView text input and external pages

- Text selection, paste and the on-screen candidate window in web pages.
- Sign-in pages that hand control back to the app.

### Linux games: linker, rootfs and performance

- Make `--guest-ld` the default. Neverball needs `libudev.so.1` in the `games` profile, and vkQuake crashes at start.
- Say which library function is missing instead of crashing later (Doom 3 without OpenAL).
- Doom 3's frame pacing and save games.
- Starting a child process can wait up to 3 seconds while other threads map memory (`test_fork_lock_contention` fails because of this).
- A program hangs at exit when one of its threads is in `pause()`.

### Testing

- Run the full tests on an AVX-512 machine, and more games on RDNA 4.
- Fedora, openSUSE and Alpine have had little or no testing ([support](SUPPORT.md)).

## Games that don't work

23 games have been tried and 20 are playable; see [What has been run](COMPATIBILITY.md).

- **Roblox** logs in and joins a game, then the server disconnects it after about three minutes with error 304 ("missing or corrupted files"). The cause isn't known. It
  probably isn't Google attestation: Roblox doesn't use it by default, and the games tried in Sober work fine. Sober runs the Android build of Roblox too, without
  translating between CPU types, and provides what it asks for itself.
- **TrickCal** is stopped by its own anti-cheat.
- **Umamusume (Japan)** stops at a device integrity check. The Global version works.
- **Angry Birds Space** and other apps with only 32-bit or x86 code can't start.

## Not planned

- Linux games in the desktop library.
- Exporting or importing saves from the library.
- Getting around anti-cheat or device integrity checks.
- Every APK, or every Android version.
- 32-bit ARM or x86 programs.
