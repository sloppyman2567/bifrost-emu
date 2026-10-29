# Azur Lane

Package: `com.YoStarEN.AzurLane` (installed package version 9.3.7).
Status: **playable**, from the user's report on October 29, 2026. No gameplay
screenshots, session length or performance numbers were recorded with that
report. The October 14 screenshots below show loading and the title/login
screen on Bifrost 2.0.0-alpha.1.

## Screenshots (2026-10-14)

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
see the [implementation and measurement limits](../JIT.md#azur-lane-polynomial-instructions-2026-10-15).
The bounded runs and CPU profiles do not establish a matched before/after FPS
improvement.

- Re-check the UI stutters reported in the delayed login scene on October 14
  (input/service, rendering, ART and JIT costs); the playable report does not
  say whether they remain.
- Toasts no longer end apps (`notification.enqueueToast` is implemented since
  October 21); the Google sign-in failure path that exited the app was not
  re-run.
- Capture gameplay, audio, pause/resume and resize evidence; WebView pointer,
  focus and scroll interaction are still partial (see the roadmap).

See the [runtime checkpoint](../Android/ANDROID_RUNTIME.md#azur-lane-interaction-checkpoint-2026-10-14),
[generated-code audit](../JIT.md#azur-lane-generated-code-audit-2026-10-14)
and [next-release priorities](../roadmap.md#next-release-priorities).
