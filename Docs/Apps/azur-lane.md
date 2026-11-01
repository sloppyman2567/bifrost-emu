# Azur Lane

Package: `com.YoStarEN.AzurLane` (installed package version 9.3.7).
Status: **playable**, online, from the user's report on November 1, 2026: home screen, story, campaign map, formation
and a finished battle, with working audio and pause/resume.

## Measurements (2026-11-01, 2.0.0-alpha.3)

![Azur Lane home screen on Bifrost, at 60 FPS with the window statistics showing](../Images/AzurLane-2026-11-01/home.png)

- **Frame rate:** 60 FPS at 16.7 ms on the home screen above, and about 59 FPS on average over a five-minute session
  (from the window title's frame counter). This is the game's screens around a battle, not the fight itself.
- **Memory:** the screenshot's `RAM 3349/33567 MB` and `3.3 GB` are the whole session by PSS, with the game's own
  window statistics. A separate `smaps_rollup` snapshot of another session, 15 minutes in, read 3.21 GiB: the game
  2,603 MiB, Google services 455 MiB and the broker and services 224 MiB. JIT code was 386 MiB of the game's share, and
  it used 562 MiB of VRAM. Memory across screens varied by about 4%; a long session has not been measured.

## Screenshots (2026-10-14)

From Bifrost 2.0.0-alpha.1.

Loading after the update check:

![Azur Lane loading screen on Bifrost](../Images/AzurLane-2026-10-14/loading.png)

Title/login screen:

![Azur Lane title and login screen on Bifrost](../Images/AzurLane-2026-10-14/login.png)

These still images do not measure frame pacing or long-session stability.

## Remaining work

The October 15 trace exposed `display.getWifiDisplayStatus` throwing during
Unity's `MediaRouter` construction. The no-adapter Wi-Fi display contract and
connected local audio/router services now let that constructor complete;
the follow-up trace contains successful calls and no framework-service throws.
Byte PMULL and CRC instruction fallbacks were also removed from FrostJIT;
see the [implementation and measurement limits](../archive/JIT-history.md#azur-lane-polynomial-instructions-2026-10-15).
The bounded runs and CPU profiles do not establish a matched before/after FPS
improvement.

- Re-check the UI stutters reported in the delayed login scene on October 14
  (input/service, rendering, ART and JIT costs); the playable report does not
  say whether they remain.
- Toasts no longer end apps (`notification.enqueueToast` is implemented since
  October 21); the Google sign-in failure path that exited the app was not
  re-run.
- Capture the fight itself and resize evidence; WebView pointer, focus and scroll
  interaction are still partial (see the roadmap).

See the [runtime checkpoint](../archive/ANDROID_RUNTIME-history.md#azur-lane-interaction-checkpoint-2026-10-14),
[generated-code audit](../archive/JIT-history.md#azur-lane-generated-code-audit-2026-10-14)
and [next-release priorities](../roadmap.md#next-release-priorities).
