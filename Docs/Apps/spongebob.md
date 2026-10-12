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

### SpongeBob character drawn incompletely after equipping (2026-10-12)

Reproduction (user, reliable): open the outfit screen. SpongeBob draws
correctly until any item is equipped; afterwards his square sponge body, base
and nose are missing and yellow sponge pieces float beside his head. The
outfit thumbnails (Default Outfit, Santa, Spongicus) stay complete. The
equipped character is also missing from the main screen's top-left avatar
portrait and from the "Lv. 1 SpongeBob" platform on the new-item screen.

- Not caused by native-density rendering: it reproduces with
  `BIFROST_ANDROID_LOGICAL_PIXELS=1` (1280x692, 160 dpi).
- No GL, framebuffer or Unity render warnings are logged.
- Hypothesis (unverified): equipping changes the Spine skin, which Spine-Unity
  typically repacks into a runtime atlas copied on the GPU (`CopyTexture`,
  `glCopyImageSubData` or framebuffer blits); the missing parts would be the
  repacked regions. The GLES thunks implement `glCopyImageSubData` (15
  arguments) and `glBlitFramebuffer`; their AAPCS64 stack-argument reads look
  correct.

Next step: a `BIFROST_THUNK_TRACE=1` capture of the GL calls made while
equipping one item, then compare copy/blit arguments and texture formats with
a reference. The user asked not to run with every JIT optimization disabled:
that configuration is too slow to play.

### No mail/news (2026-10-12, uninvestigated)

The phone shows mail in the news envelope; Bifrost shows none. Likely a
failed news fetch or service gap; no log evidence collected yet.
