# Tests, third-party code and distribution

Bifrost's original emulator code and original regression tests use the root
[Unlicense](../LICENSE). Tests can be published with the project: being freely
reusable does not prevent a project from including its own test suite.
The Unlicense describes reuse of the authors' own work; it does not replace
licenses attached to other people's code, binaries or assets.

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

- **Toybox:** `ctest_real/toybox` identifies itself as 0.8.10. Its upstream
  Zero-Clause BSD license is preserved in `ctest_real/toybox.LICENSE`, extracted
  from the [0.8.10 source archive](https://landley.net/toybox/downloads/toybox-0.8.10.tar.gz).
  The committed binary's SHA-256 is
  `ef98721241d9949fc4b2472bbef76260c9669e8e5d0d7ac99de50fff26cc1aa9`.
  The existing fetch script also targets 0.8.10; this identity check is not
  a reproducible-build verification.
- **zstd:** `ctest_real/zstd-aarch64` has a BSD license in
  `ctest_real/zstd.LICENSE`; [its fixture notes](../ctest_real/ZSTD-README.md)
  record the source revision, build options and checksum.
- **Minecraft Weekend:** the vendored game has an MIT license in
  `ctest_real/minecraft_weekend/LICENSE`. Its dependency headers retain their
  individual notices; the game's license is not a blanket license for every
  dependency or separately obtained asset.
- **Voxelspace and libfixmath:** MIT notices are preserved in
  `ctest_real/voxelspace/LICENSE` and
  `ctest_real/voxelspace/libs/libfixmath/LICENSE`.
- **Vulkan headers:** upstream license information remains under
  `tools/vulkan-headers/LICENSE.md` and `tools/vulkan-headers/LICENSES/`.

This lists the main fixture bundles, not a complete dependency/license audit.
Preserve per-file notices and inspect the exact upstream revision and linked
libraries when producing a binary distribution.

## External applications and game data

Toolchains, rootfs libraries, BusyBox and other downloaded utilities retain
their upstream terms. Downloading them through a setup script does not make
them Unlicense software. The ignored `rootfs/`, toolchain directories and
`ctest_real/realworld/` are local setup state; they are not part of a Git push.

Neverball, vkQuake, Teeworlds and dhewm3 are external compatibility targets.
Game-engine source licenses and game-data licenses must be checked separately.
Keep commercial game packs, personal saves and APKs outside the published
repository. Users provide game data or obtain it from an authorized source;
the emulator's license does not grant redistribution rights to that data.

Publish source releases from tracked Git content rather than archiving the
entire working directory. Include the root license and all tracked upstream
notices. A binary release that bundles additional dependencies needs those
dependencies' distribution requirements reviewed separately.
