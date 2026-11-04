# vkQuake

On 2026-10-02 the user confirmed working gameplay with the repaired default
JIT build and supplied four screenshots showing the world, HUD, main menu,
help, and load menu. This is a Vulkan game bring-up milestone, with remaining
camera and rendering issues at that checkpoint; it is not a claim of
complete compatibility. The latest input report is recorded below.

The detailed investigation history (dated fixes, profiles, regressions) is in [the vkQuake history](../archive/apps/vkquake-history.md).

## Thread TLS and stack repairs (2026-11-04)

A cross-built 1.33.1 with LibreQuake data aborted in mimalloc (`heap->tld->thread_id == tid`) on its texture workers, then corrupted
memory. The workers had inherited main's heap through the TLS copy, and their `alloca` of whole textures overran a 256 KiB SDL stack.
Both were emulator bugs ([changelog](../CHANGELOG.md)). With the fixes, five 15 s runs did not crash, against four of six before, and the
user reached a map and saw no texture corruption. That is LibreQuake data: whether the original corruption is gone on Quake's own data is
untested, and no link between these bugs and that corruption has been shown.

To reproduce: no arm64 Linux release exists and Debian's 1.37 package needs glibc 2.43 and SDL3, so cross-compile tag 1.33.1 with
`Tools/aarch64-linux-gnu-cross` and Debian arm64 `-dev` packages in a sysroot. Remove the RPATH meson leaves in the uninstalled binary
(`patchelf --remove-rpath`): with it, the guest loads the sysroot's real SDL2 instead of the host-backed one and jumps to address 0.

## Latest input follow-up (2026-10-04)

The user subsequently reported that the mouse issue was resolved. A connection
with the Doom 3 fixes is suspected by the user but has not been established.
Treat the excessive-sensitivity notes below as historical repro evidence;
repeatable axis/sensitivity/focus checks remain pending. Texture corruption
is still unresolved.

## Gameplay checkpoint (2026-10-04)

User-supplied gameplay shows the world, weapon and HUD at 1720×692. Remaining texture artifacts are visible; this does not establish complete rendering correctness. The FPS in the title is an instantaneous presentation sample, not a sustained benchmark.

![Gameplay checkpoint (2026-10-04)](../Images/VkQuake-2026-10-04/gameplay.png)

![vkQuake options screen](../Images/VkQuake-2026-10-04/options.png)
