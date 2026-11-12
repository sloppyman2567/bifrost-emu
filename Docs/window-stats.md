# Graphics window stats

Statistics are **opt-in**: a game window shows only the game's own title until you ask for them.
`--window-stats` (alias `--window_stats`) turns them on in the default layout. `--window-stats-mode MODE`
(or `--window-stats-mode=MODE`) picks a layout and turns them on:

| Mode | Name | Window title | In-window overlay |
|---|---|---|---|
| 1 | `minimal` | statistics | no |
| 2 | `normal` (default) | statistics | yes |
| 3 | `overlay` | the game's own | yes |

In the environment, `BIFROST_WINDOW_STATS=1` turns the statistics on and `BIFROST_WINDOW_STATS_MODE`
(`1`, `2`, `3`, `minimal`, `normal`, `overlay`) picks the layout; a mode alone does not turn them on there.
Android apps keep both choices per app (`window_stats` and `window_stats_mode`, see
[per-app settings](Android/ANDROID_APK.md#per-app-settings)). The emulator name and version come from
`Include/Bifrost/Version.hpp`.

The display includes the presentation backend, FPS, window size in physical
pixels and total presented frames. On a scaled (HiDPI) display this is the
drawable size, not the smaller logical size the desktop reports. The original game title is retained.
Experimental hardware fields show the full host CPU and current GL renderer names
without CPU/GPU prefixes, emulator session RAM / total system RAM, and emulator
resident VRAM / total dedicated GPU VRAM when available. Both memory fields use
decimal MB (1,000,000 bytes), for example `RAM 400/33567 MB | ... | VRAM 300/8573 MB`; 6144 MB is therefore 5.7 GiB, not 6 GiB.
The driver-reported device-local heap capacity can be smaller than the physical
memory fitted to the card. System RAM comes from Linux MemTotal, so it may be lower
than installed capacity.

Session RAM is the memory the whole emulator session holds, not only the windowed
process. An Android app counts its Binder realm (the broker, its services, every app
and helper using the same broker socket); anything else counts itself. Descendants of
those processes are included, and the launcher script is not. Each process contributes
its proportional share from `/proc/PID/smaps_rollup` (PSS), so pages they share count
once, and it covers emulator and guest allocations but not the guest's reserved
address space. Two apps in one realm therefore both show the realm's total, and
swapped-out pages are not counted. Where `smaps_rollup` cannot be read (an old kernel, a
process that is not dumpable, restricted `/proc`), that process counts its `VmRSS`, which
counts shared pages in full. A background thread samples every 5 seconds, because the
read walks page tables (about 37 ms in total for a 2.7 GB game on the development
machine) and holds a lock the guest's `mmap` calls wait for. Until its first sample,
and if the thread cannot start, the value is this process's `VmRSS`. CPU share remains
this process only.

VRAM usage is sampled once per second from the emulator's Linux DRM fdinfo
resident VRAM regions. Duplicate descriptors for the same device/client are
counted once; the deprecated `drm-memory-vram` field is accepted as a fallback
without adding it to its resident alias. Numbered VRAM regions are supported.
This measures driver-accounted resident buffer storage for the process, including
shared buffers, rather than global GPU usage or requested allocations. A buffer
shared across distinct clients may be included in each client's accounting.
EGL's actual DRM render node is matched to the usage counters and to a Vulkan
physical device before summing its device-local heaps; a Mesa GLX renderer query
is a capacity fallback. When GLX does not expose a device identity, usage is
reported only if the process has a single identifiable DRM device. Unsupported
usage counters display `unavailable/total MB`; shared integrated-GPU memory or
unknown capacity displays `unavailable/shared`. These values are discovered at
runtime, without assumptions about CPU/GPU vendor or installed capacity. GPU
names and capacity currently require a presenting GL context; Vulkan-only paths
retain the presentation counters but may show hardware as unavailable.

Each display has its own enable and disable switch, which overrides what the mode implies:

- `--window-stats-title` / `--no-window-stats-title`
- `--window-stats-window` / `--no-window-stats-window`
- `--window-stats-overlay` / `--no-window-stats-overlay`

`--no-window-stats` disables all three, explicitly, wherever it appears: a display that was disabled explicitly stays
off when a later `--window-stats` or `--window-stats-mode` turns the rest on, so enable displays individually after it
(`--no-window-stats --window-stats-overlay`). The separate stats window is never part of a mode; enable it on its own.

```bash
./bifrost-emu --window-stats --rootfs "$PWD/rootfs" rootfs/usr/games/neverball
./bifrost-emu --window-stats-mode overlay app.apk
./bifrost-emu --window-stats-window --window-stats-overlay --no-window-stats-title game.elf
```

Environment equivalents are `BIFROST_WINDOW_STATS`,
`BIFROST_WINDOW_STATS_TITLE`, `BIFROST_WINDOW_STATS_WINDOW`, and
`BIFROST_WINDOW_STATS_OVERLAY`. Set a switch to `1` to enable or `0` to disable.
These are cached in `DebugFlags.hpp`; blanket `BIFROST_TRACE` does not enable UI.

FPS measures completed host presentation calls using a monotonic clock and
one-second samples. It starts at zero until a sample is available. No frame
for two seconds means idle, with zero FPS; a resumed presentation starts a fresh
sample. This measures presentation, not guest instruction throughput or whether
the pixels themselves changed. Menus and repeated frames still count. Title
refreshes are limited to four per second and run when the guest presents or
pumps events. A blocked guest cannot refresh its title; the separate UI continues
to show elapsed time since the last frame and detects idle independently.

The optional UI runs in a separate, re-executed process with its own SDL event
queue and software renderer. It does not drain game events or change guest GPU
state. Closing a stats window leaves the game running. Game-window destruction
closes that window's stats UI. No font files or SDL_ttf are required.

## In-window overlay

On an **Android window** the overlay is a small translucent panel in the window's top-left corner, drawn inside
the window on both Wayland and X11. It uses the window backend's popup layer (the one framework dialogs use,
reserved id `0xFFFF0001`): software-rendered ARGB pixels in a separate layer above every surface that never takes
host input. Nothing is drawn through the guest's GL or Vulkan state, and nothing is added to its framebuffer.
It refreshes at most four times a second and scales its 5x7 built-in font with the window height (2x up to
about 1440p, 3x at 4K). The panel is about 85 percent opaque with a thin outline and anti-aliased rounded corners.
It shows, from top to bottom:

- `BIFROST-EMU` with the version at the right edge, then the app's own title;
- the frame rate as a double-size figure, then `FPS`, the frame time and, at the right edge, the presentation
  backend (a large `IDLE` after two seconds without a frame);
- aligned `CPU`, `GPU`, `VRAM` and `RAM` rows, each with a percentage, a meter and, at the right edge, a detail
  (a temperature, or used/total memory); meters and numbers turn amber from 80 percent and red from 95,
  temperatures at 80 and 90 degrees Celsius;
- a dim footer: `Emulator`, this process's CPU share, session RAM, and the shortened GPU name (`llvmpipe`
  identifies a software renderer). A guest that presents through Vulkan has no GL renderer string, so the name
  comes from the host Vulkan loader's device (`AMD Radeon RX 9060 XT (RADV GFX1200)`), not the kernel driver
  (`amdgpu`), which names no card.

Text keeps its case: the font has lower case with descenders. Accented Latin letters print without the accent
(`Pokémon` as `Pokemon`), dashes and curly quotes as their ASCII forms, and other characters as `?`. A title or
GPU name longer than 31 characters ends in an ellipsis, and a title with nothing the font can show (all CJK, say)
is left out. Values that cannot be read show `--` with an empty meter. A window plugin without popup layers logs once and keeps
the title statistics.

Readings come from generic Linux interfaces, sampled once a second, with no vendor tools or libraries:

- CPU usage: `/proc/stat`; the process share: `/proc/self/stat`, divided by the online CPU count.
- CPU temperature: the hwmon package sensor by driver (`k10temp`, `zenpower`, `coretemp`, `cpu_thermal`,
  `soc_thermal`, `acpitz`), else a thermal zone (`x86_pkg_temp`, `cpu-thermal`, `soc-thermal`).
- The GPU: the sysfs device of the DRM node this process has open (its `/proc/self/fdinfo` clients; the one with
  the most resident memory), so no GL context is needed. Its hwmon temperature uses the `edge` label, else the
  first input.
- GPU usage: amdgpu's `gpu_busy_percent` (the whole GPU). Without that file, the kernel's DRM client usage
  statistics (`drm-engine-*` times in fdinfo, which Intel and other drivers print): this process's busiest engine
  over the elapsed time and engine count. That is this process's share, not the whole GPU.
- GPU name: the presenting window's GL renderer string, else the host Vulkan loader's device name (one instance
  is created the first time it is needed, and a discrete device is preferred over an integrated one).
- VRAM: amdgpu's `mem_info_vram_*` (the whole GPU), else the per-process DRM accounting described above.
- System RAM: `MemTotal` minus `MemAvailable`.

Driver support, honestly: on AMD (amdgpu, an RX 7600) everything was checked against `sensors`, sysfs and
`/proc/meminfo`, and the engine-time path against `gpu_busy_percent` (a running game read about 20 percent by engine
time while the GPU reported 23 to 24). Other drivers are covered by fixture trees and the documented formats only:
Intel (i915, xe) and others should give usage through fdinfo, nouveau, radeon and xe a hwmon temperature; an
integrated GPU usually has no temperature of its own. NVIDIA's proprietary driver exposes neither DRM statistics nor
hwmon, so it shows `--` (reading it needs NVML, which is not implemented). Software renderers have no GPU device.

**Other windows** (SDL, GLFW, Vulkan and framebuffer windows of Linux guests) do not get the embedded panel. For
them `--window-stats-overlay` is a desktop HUD in a separate process that shows the same panel, positioned over
the game client area and **not drawn into the guest framebuffer**. An X11 window has no alpha channel, so the
HUD draws the panel over a dark base and trims its corners with the window's XShape bounding region. It requires an X11 game window and XShape input regions to remain
click-through. SDL overlays hide when the game loses focus. Native Wayland positioning is not supported: the
emulator reports that limitation and the independent stats window remains available. For X11 testing on a Wayland
desktop, `SDL_VIDEODRIVER=x11` selects XWayland for an SDL game. Fullscreen compositor stacking and game capture
behavior are compositor-specific; framebuffer dumps do not include the HUD. The separate stats window also shows
CPU and GPU load and temperature.

Presentation hooks cover SDL OpenGL, SDL renderers/surfaces, GLFW OpenGL, the
SDL-backed EGL window, framebuffer/display-proxy windows, and Vulkan swapchains
associated with `SDL_Vulkan_CreateSurface`, plus Android EGL surfaces.
Android counts successful swaps on the rendering thread and refreshes native
window metadata/title on the SDL3 window thread. Destruction resets the frame
clock. SDL3 handles never enter the SDL2 stats APIs. Android window plugins
without metadata operations cannot supply title/placement information. Vulkan counts only successful or
suboptimal presentations, including per-swapchain results. Native guest graphics
paths that bypass these host thunks are not counted.

A first presentation logs `[render] bifrost-emu VERSION: rendering started
(BACKEND)` once per window lifetime by default. `--no-render-log` or
`BIFROST_RENDER_LOG=0` disables it; `--render-log` enables it again.

Validation:

```bash
EMU=build/release-sdl1-gl1/bifrost-emu ./Scripts/Testing/test_window_stats.sh
```

The regression checks the presentation clock, default first-frame logging,
opt-in titles and the layout modes, title renaming, idle detection, native-window
recreation, hardware field formatting and explicit disable switches, plus
`Tests/Unit/window_stats_overlay.cpp` (sensor parsing against fixture `/proc` and
`/sys` trees, overlay text and rendering) and, in
`make test-android-window-backend`, the overlay popup an Android window backend receives.
The SDL probes open real windows unless `SDL_VIDEODRIVER=offscreen` is set. DRM fixtures cover duplicate
clients, multiple devices/regions, legacy aliases, invalid units and overflow,
and zero versus unsupported usage. Optional UI placement and click-through
require a real X11 desktop.


Hardware discovery follows [EGL_EXT_device_query](https://registry.khronos.org/EGL/extensions/EXT/EGL_EXT_device_query.txt)
and Vulkan's [physical-device DRM properties](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceDrmPropertiesEXT.html).
VRAM accounting follows Linux's [DRM client usage statistics](https://kernel.org/doc/html/next/gpu/drm-usage-stats.html),
including device/client deduplication, resident regions, and byte/KiB/MiB input
units. Display units are always decimal MB, matching system RAM.
