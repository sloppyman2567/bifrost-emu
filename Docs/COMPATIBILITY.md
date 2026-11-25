# What has been run

**20 games recorded (2026-11-14): 17 playable** (13 Android, 4 Linux), **1 starts** without confirmed gameplay
(Teeworlds) and **2 are blocked** (Roblox, TrickCal). Umamusume counts as playable through its
Global client; its JP client is blocked. Apps such as YouTube and Firefox are not counted.

This page keeps the notes for every program: versions, dates, frame rates and known problems. The machine-readable status
of the Android apps is in [`Scripts/Android/compatibility.json`](../Scripts/Android/compatibility.json), and the pages in
[`Apps/`](Apps/) have more detail for some of them.

"User-confirmed" and "user-reported" mean the user played it on Bifrost, not that every feature has an automated test or a
recorded capture. A passing build or a launch is not gameplay.

| Program | Kind | Status |
|---|---|---|
| [SpongeBob – Krusty Cook-Off 5.12.0](Apps/spongebob.md) | Android, Unity/IL2CPP | User-confirmed cooking levels, reaching level 16 at about 60 FPS (2026-11-01), outfits and menus at native resolution. Both age choices reach the lobby (adult age user-confirmed 2026-11-25, without a level played). |
| [Azur Lane 9.3.7](Apps/azur-lane.md) | Android, Unity/IL2CPP | User-reported playable online through a finished battle, with audio and pause/resume (2026-11-01). Home screen captured at 60 FPS. Google sign-in through microG works (user-reported 2026-11-23). |
| [Arknights 37.7.41](Apps/arknights.md) | Android, Unity/IL2CPP | User-reported playable: Google sign-in through microG (also reported working 2026-11-23) and a battle at about 60 FPS (2026-11-03). |
| The Battle Cats 15.6.1 | Android | User-reported playable (2026-10-29). |
| [Umamusume: Pretty Derby](Apps/umamusume.md) | Android, Unity | Global 1.35.2: user-reported playable, title, home and Career story captured at 30 FPS, the game's own frame cap (2026-11-01). JP 2.32.0: blocked by the game's device integrity check when it connects. |
| Subway Surfers 3.70.3 | Android, Unity/IL2CPP | User-reported playable, ads work (2026-10-29). On a Steam Deck in Desktop mode (2026-11-08): 20 to 40 FPS, mostly 30 to 40, stable with stutters and a hot CPU, about 30 W. |
| Geometry Dash Lite 2.2.148 | Android, Cocos2d-x | User-reported playable (2026-10-29); ads only somewhat stable. |
| Zenless Zone Zero 3.3.0 | Android, Unity/IL2CPP, Vulkan | User-confirmed gameplay in the city (2026-11-06) and story scenes (2026-11-08), with very low-resolution textures and heavy lag (roughly 4 to 30 FPS on the overlay on 2026-11-06, 29 to 30 FPS in story scenes on 2026-11-08; the long loading screen is 0 to 2 FPS). Its ASTC textures are converted to BC7 on the CPU, which costs CPU while they load. Maximizing the window freezes its progress bar. Google sign-in through microG works (user-reported 2026-11-23). |
| Doki Doki Literature Club 1.3.11.1 | Android, Ren'Py (SDL2, GLES) | User-confirmed play into the story (2026-11-06). The audio popped at every buffer boundary until the AAudio write path was fixed (user-confirmed clean 2026-11-08), and resizing the window blacks out images while text still draws (Ren'Py deletes its textures on an Android resize; the app opens with Window set to "Keep the app's shape", which avoids it, user-confirmed 2026-11-08). |
| Hungry Shark Evolution 14.5.0, Hungry Shark World 8.2.6 | Android | User-reported stable gameplay (2026-11-06). Hungry Shark World's Play Games sign-in does not complete. |
| YouTube 21.42.247 | Android | Home screen, search and video playback seen (2026-10-28); not reported as fully working. |
| [Oddmar](Apps/oddmar.md) | Android, Unity/IL2CPP | User-confirmed gameplay in the first level on 2.0.0-beta.1 (2026-11-08), with no stutter noticed; later levels are not confirmed. |
| Family Island 2026370.1.94765 | Android, Defold | User-reported stable play on 2.0.0-beta.2 (2026-11-14): home island, the Torch Island map and the Beaver Grove screen captured at about 27 to 47 FPS on the overlay while other programs loaded the machine; one session, no longer run recorded. |
| Uptodown | Android | Draws its home list, then asks for all-files access, which Bifrost cannot grant. |
| Roblox, TrickCal | Android | Blocked: Roblox is disconnected after joining (needs real Google attestation); TrickCal's anti-cheat ends it. |
| [Neverball 1.6.0](Apps/neverball.md) | Linux, SDL2/GL | User-confirmed stable gameplay, replays, menus and fonts. |
| [Minecraft Weekend](Apps/minecraft-weekend.md) | Linux, SDL2/GL voxel game | User-confirmed stable gameplay; chunk rendering can lag. |
| [vkQuake 1.33.1](Apps/vkquake.md) | Linux, Vulkan | User-confirmed gameplay and menus. No texture corruption seen on LibreQuake data (2026-11-04); Quake's own data not retested. |
| [Doom 3 (dhewm3)](Apps/doom3.md) | Linux, SDL2/GL | User-reported extended gameplay with audio and animation; mostly sustained 60 FPS in the demo, even in heavy scenes (2026-11-01, not instrumented); the full game is not covered by that report. |
| Teeworlds | Linux, SDL2/GL | Reaches its rendering menu with sound initialized. |
| BusyBox, ToyBox | Linux CLI | Covered by the regression suite. |
| CoreMark | Linux benchmark | CRC-validated runs through `Scripts/Bench/run_benchmarks.py`. |
