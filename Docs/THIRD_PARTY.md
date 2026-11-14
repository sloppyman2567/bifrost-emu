# Tests, third-party code and distribution

Bifrost's original emulator code and original regression tests use the root
[Unlicense](../LICENSE). Tests can be published with the project: being freely
reusable does not prevent a project from including its own test suite.
The Unlicense describes reuse of the authors' own work; it does not replace
licenses attached to other people's code, binaries or assets. The
[license directory](../LICENSES/README.md) contains a copy of the project
Unlicense, the canonical Apache 2.0 text and Android source attribution.

## Reused Android source and runtime licenses

Asgard's original integration code uses the project Unlicense. Its generated
`Guest/Android/Managed/Framework/FrameworkSettingsDefaults.java` derives defaults,
initialization expressions, public setting names and validation rules from six
Android 15 AOSP sources. Their Apache 2.0 terms and copyright attribution are
recorded in [Apache-2.0.txt](../LICENSES/Apache-2.0.txt) and
[NOTICE-Android.txt](../LICENSES/NOTICE-Android.txt); the generated header records
the source tag, input paths and SHA-256 values.

Downloaded Bionic, ART, framework, codec and packaging bundles preserve their
own NOTICE files and per-component licenses. Android includes material under
other licenses, so this Apache text is not a blanket license for every runtime
library, header, tool or external game. Retain the upstream notices when
redistributing any bundle.

## Regression sources and generated fixtures

Commit original `.c`, `.cpp` and `.s` test sources and the scripts that build
and run them. Guest `.elf` files and generated probe `.so` files are ignored
and rebuilt with `make setup-tests`, `make cross`, or their standalone test
script. The host C API probe builds its executable guest library through
`make test-capi`.

Self-contained instruction, memory, thread and ABI probes need no commercial
game data. Prefer small expected-result tests over copied game routines;
code copied from an upstream project must retain that project's terms.

Some separately licensed real-world fixtures are deliberately committed or
fetched to exercise behavior beyond the synthetic tests:

- **Toybox:** `Tests/Apps/toybox` identifies itself as 0.8.10. Its upstream
  Zero-Clause BSD license is preserved in `Tests/Apps/toybox.LICENSE`, extracted
  from the [0.8.10 source archive](https://landley.net/toybox/downloads/toybox-0.8.10.tar.gz).
  The committed binary's SHA-256 is
  `ef98721241d9949fc4b2472bbef76260c9669e8e5d0d7ac99de50fff26cc1aa9`.
  The existing fetch script also targets 0.8.10; this identity check is not
  a reproducible-build verification.
- **zstd:** `Tests/Apps/zstd-aarch64` has a BSD license in
  `Tests/Apps/zstd.LICENSE`; [its fixture notes](../Tests/Apps/ZSTD-README.md)
  record the source revision, build options and checksum.
- **Minecraft Weekend:** the vendored game has an MIT license in
  `Tests/Apps/minecraft_weekend/LICENSE`. Its dependency headers retain their
  individual notices; the game's license is not a blanket license for every
  dependency or separately obtained asset.
- **Voxelspace and libfixmath:** MIT notices are preserved in
  `Tests/Apps/voxelspace/LICENSE` and
  `Tests/Apps/voxelspace/libs/libfixmath/LICENSE`.
- **BC7 tables and SPIR-V in the ASTC tests:** `Source/System/Graphics/TextureCodec/Bc7Tables.hpp` holds the partition
  patterns and anchor indices of the BPTC format, extracted by script from the Khronos Data Format Specification's BPTC
  chapter and cross-checked against that chapter's anchor tables (the ASTC decoder and BC7 encoder are original code,
  written from the specification text). The specification's own license terms were not checked when it was used.
  `Tests/Apps/test_vulkan_astc_spv.h` is a small compute shader compiled with glslc; its GLSL source is in the header.
- **Vulkan headers:** upstream license information remains under
  `ThirdParty/VulkanHeaders/LICENSE.md` and `ThirdParty/VulkanHeaders/LICENSES/`.
- **OpenGL registry:** `ThirdParty/GLRegistry/gl.xml` is Apache-2.0 (header in the
  file), from [KhronosGroup/OpenGL-Registry](https://github.com/KhronosGroup/OpenGL-Registry)
  `xml/gl.xml` at commit `6af574a14089ccfee87efe230ebcdd8742859813`
  (2026-10-06, SHA-256 `b9ca2cfa5c676e901c20d34af3407f1687cde0f1336a5ff7a8974d04c7494ad3`).
  `Tools/CodeGen/glcoverage.py` derives thunk rows and the hidden ES
  extension list from it.
- **Android runtime and packaging tools:** the ignored Bionic runtime is
  checksum-pinned to AOSP `prebuilts/runtime` and retains `NOTICE.html.gz`;
  each installed app root receives a copy. The packaging fetchers download
  AAPT2, apksigner and the API-35 manifest fixture library from official AOSP
  prebuilts. `Tools/Android/fetch-android-package-tools.py` pins apksigner to SHA-256
  `9469c60e5e40fc5c44a2f2338509cb6600cdf065e9b50f9fa3ca6c5be5bae6a9`
  and android.jar to
  `ee568219aa3754977207a8fd849fdbf3a299658dd26dd0907d39ca830587f8aa`
  at `android-15.0.0_r1` in
  [AOSP prebuilts/sdk](https://android.googlesource.com/platform/prebuilts/sdk/+/android-15.0.0_r1/).
  The same fetcher pins D8 to SHA-256
  `875fab053a6d1bce99751d7a529fa7f80e408550275a2919d76fa4cea56d5819`.
  The separate ART experiment extracts a verified API-35 ARM64 revision-2 SDK
  image using `Tools/Android/android_art.py`, retains its `NOTICE.txt` and APEX notices,
  and records all runtime file hashes. It reuses the same pinned image as the
  Media NDK provisioner; see [runtime provenance](Android/ANDROID_RUNTIME_REFERENCE.md#experimental-same-image-art-bootstrap).
  These downloads retain their upstream terms and are not redistributed by
  the source commit. Generated regression APKs use a temporary test key and
  original fixture code; commercial APKs and installed app state stay local.
- **microG:** `Tools/Android/android_microg.py` downloads the Apache-2.0
  [microG GmsCore](https://github.com/microg/GmsCore) `v0.3.17.252432`
  release APKs (`com.google.android.gms`, `com.android.vending`) into
  `build/microg/`, pinned by size and SHA-256. The hashes match microG's
  signed F-Droid index (repository certificate SHA-256
  `9BD06727E62796C0130EB6DAB39B73157451582CBD138E86C468ACC395D14165`).
  Each managed app root receives a copy. The Play services availability
  test downloads Google's `play-services-basement` 18.12.0 AAR (Android SDK
  terms) from Google Maven into `build/play-services-basement/`, pinned by
  SHA-256 `30e9df7012c15ef31a4b288b3d77eb4550b195a0581675cad121b0b4bbc47b76`;
  it is only linked into a generated test APK.

This lists the main fixture bundles, not a complete dependency/license audit.
Preserve per-file notices and inspect the exact upstream revision and linked
libraries when producing a binary distribution.

## External applications and game data

Toolchains, rootfs libraries, BusyBox and other downloaded utilities retain
their upstream terms. Downloading them through a setup script does not make
them Unlicense software. The ignored `rootfs/`, toolchain directories and
`Tests/Apps/realworld/` are local setup state; they are not part of a Git push.

Neverball, vkQuake, Teeworlds and dhewm3 are external compatibility targets.
Game-engine source licenses and game-data licenses must be checked separately.
Keep commercial game packs, personal saves and APKs outside the published
repository. Users provide game data or obtain it from an authorized source;
the emulator's license does not grant redistribution rights to that data.

Publish source releases from tracked Git content rather than archiving the
entire working directory. Include the root license and all tracked upstream
notices. A binary release that bundles additional dependencies needs those
dependencies' distribution requirements reviewed separately.
