# Arknights

Global package: `com.YoStarEN.Arknights` (tested version 37.7.41, installed from its APKMirror `.apkm` bundle).
Status: **playable**, from the user's report on November 3, 2026, on Bifrost 2.0.0-beta.1: Google sign-in through microG
works, a battle ran at about 60 FPS with nothing obviously wrong, and a capture shows the opening story scene. The game
ran through the globally installed `bifrost-emu`.

## Installing

```sh
bifrost-emu com.YoStarEN.Arknights_37.7.41-2000194_..._apkmirror.com.apkm
```

The bundle carries a base APK, `arm64-v8a` and `armeabi-v7a` config splits and two Unity asset packs. The data asset
pack alone is 1,024.1 MiB and the APKs unpack to 2.27 GiB, so installing it needs commit `1d61c57a`: before it, Asgard
refused nested APKs over 1 GiB and APK sets over 2 GiB uncompressed.

## What is established

- User-reported: install, microG sign-in and a battle at about 60 FPS, with no problems noticed (2026-11-03).
- Not recorded: frame-time logs, audio, long sessions, pause/resume, and how far past the first battle the game works.
  The 60 FPS figure is the user's observation, not a measurement.

## Screenshots (2026-11-03)

User-supplied capture from Bifrost 2.0.0-beta.1. The opening story scene, with the player's chosen name in Amiya's line
("Dr. BifrostEmu"); the black bars at the sides are the window outside the game's 16:9 area:

![Arknights opening story scene on Bifrost](../Images/Arknights-2026-11-03/story.png)
