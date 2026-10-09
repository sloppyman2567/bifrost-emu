# Graphics window stats

Statistics appear in the game window **title** by default.
`--window-stats` (alias `--window_stats`) explicitly enables title statistics.
The separate stats window and overlay remain opt-in. The emulator name and
version come from `bifrost/version.hpp`.
The display includes the presentation backend, FPS, logical window dimensions
and total presented frames. The original game title is retained.
Experimental hardware fields show the host CPU and current GL renderer, emulator
resident RAM / total system RAM, and total dedicated GPU VRAM when available.
RAM is displayed in decimal MB; VRAM uses binary MiB and GiB to match GPU
capacity conventions. The value is the driver-reported device-local heap
capacity, which can be smaller than the physical memory fitted to the card. Resident RAM includes the emulator and guest
resident allocations; it is not the guest's reserved virtual address space.
System RAM comes from Linux MemTotal, so it may be lower than installed capacity.
VRAM is capacity, not per-app GPU usage. EGL's actual DRM render node is matched
to a Vulkan physical device before summing its device-local heaps; a Mesa GLX
renderer query is a fallback. Shared integrated-GPU memory and unsupported
queries display unavailable/shared. These values are discovered at runtime,
without assumptions about CPU/GPU vendor or installed capacity. GPU names and
VRAM currently require a presenting GL context; Vulkan-only paths retain the
presentation counters but may show hardware as unavailable.

Each display has its own enable and disable switch:

- `--window-stats-title` / `--no-window-stats-title`
- `--window-stats-window` / `--no-window-stats-window`
- `--window-stats-overlay` / `--no-window-stats-overlay`

`--no-window-stats` disables all three. Options apply in command-line order;
`--window-stats` enables the title default unless the title is explicitly
configured. The optional displays can be enabled without the title.

```bash
./bifrost-emu --window-stats --rootfs "$PWD/rootfs" rootfs/usr/games/neverball
./bifrost-emu --window-stats-window --window-stats-overlay --no-window-stats-title game.elf
```

Environment equivalents are `BIFROST_WINDOW_STATS`,
`BIFROST_WINDOW_STATS_TITLE`, `BIFROST_WINDOW_STATS_WINDOW`, and
`BIFROST_WINDOW_STATS_OVERLAY`. Set a switch to `1` to enable or `0` to disable.
These are cached in `debug_flags.h`; blanket `BIFROST_TRACE` does not enable UI.

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

The overlay is a desktop HUD positioned over the game client area, **not drawn
into the guest framebuffer**. It requires an X11 game window and XShape input
regions to remain click-through. SDL overlays hide when the game loses focus.
Native Wayland positioning/overlay is not supported: the emulator reports that
limitation and the independent stats window remains available. For X11 testing
on a Wayland desktop, `SDL_VIDEODRIVER=x11` selects XWayland for an SDL game.
Fullscreen compositor stacking and game capture behavior are compositor-specific;
framebuffer dumps do not include the HUD.

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
EMU=build/release-sdl1-gl1/bifrost-emu ./scripts/test_window_stats.sh
```

The regression checks the presentation clock, default first-frame logging,
default titles, title renaming, idle detection, native-window recreation, hardware
field formatting and explicit disable
switches. Optional UI placement and click-through require a real X11 desktop.


Hardware discovery follows [EGL_EXT_device_query](https://registry.khronos.org/EGL/extensions/EXT/EGL_EXT_device_query.txt)
and Vulkan's [physical-device DRM properties](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceDrmPropertiesEXT.html).
The host-driver check reported a device-local heap of 8,573,157,376 bytes:
8176 MiB (7.98 GiB). The previous 8573 MB label represented the same bytes in
decimal units; it was not an allocation or usage measurement. These exact
numbers are verification evidence, not constants used by the implementation.
