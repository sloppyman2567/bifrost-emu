# SpongeBob - Krusty Cook-Off

Installed version: 5.12.0 (`com.tiltingpoint.spongebob`, pulled from the user's
phone). Runtime contracts and the bug history behind each checkpoint are in
[the Android runtime guide](../Android/ANDROID_RUNTIME.md).

As of 2026-10-12 the user plays cooking levels, completes them, opens Settings
and Help Center, and the main HUD stays after closing Settings. Play renders
at the host display's native density (2560x1384 or 3440x1384 px, 320 dpi on a
200%-scaled 3440x1440 monitor). The child-age path is required: the adult path
still stalls at 73% on Firebase/Google Play services.

## Open issues

### SpongeBob character drawn incompletely (2026-10-12, uninvestigated)

The Spine-animated SpongeBob character on the avatar platform draws partially:

- On the level-failed "You received a new item!" screen the platform under
  "Lv. 1 SpongeBob" is empty; only the small head icon draws.
- On the outfit screen his eyes, nose, shirt, pants, arms and legs draw, but
  the square sponge body is missing and several yellow sponge pieces float to
  the left of his head.

No GL, framebuffer or Unity render warnings were logged. Region attachments
appear intact while deformable (mesh) attachments do not, which points at the
CPU-side vertex/skinning math run by the JIT rather than at resolution, but
this is a hypothesis: no A/B run has been made. Not yet known whether it
predates native-density rendering (aac7c5b). Next steps, in order: compare
against a pre-aac7c5b binary or 1280x720 rendering, then bisect single JIT
switches (`BIFROST_NO_SSA`, `BIFROST_NO_CHAIN`, `BIFROST_JIT_BLOCK_LIMIT`)
one at a time. The user asked not to run with every optimization disabled:
that configuration is too slow to play.
