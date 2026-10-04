# Neverball

Latest user gameplay confirmation: October 3, 2026. No new issue was reported.
The dated repair/test results below are historical checkpoints; current
cross-project suite counts are maintained in [TESTS.md](TESTS.md).

As of 2026-10-02, the user reports Neverball 1.6.0 is effectively fully stable
in interactive play on Bifrost. Gameplay, replay, fonts and menus, configuration
persistence, and level-preview rendering are working after the font, SDL,
JIT memory-guard, and JPEG decoding fixes below.

Earlier testing confirmed monitor and resolution selection, ball-model changes,
and player-name editing. The October 2 confirmation followed the JPEG repair and
relaunch. This is a user-reported stability milestone; automated checks cover
the underlying regressions and selected images, rather than every level or a
long-duration gameplay soak. The user also confirmed mouse control works in
fullscreen gameplay and after returning to windowed mode on 2026-10-02.

The user reconfirmed working gameplay on 2026-10-03 after relaunching with
the repaired interpreter decode cache. No new Neverball issue was reported.

## Running

With the AArch64 game and its dependencies installed in `rootfs`:

```sh
make
BIFROST_ROOT="$PWD/rootfs" ./bifrost-emu rootfs/usr/games/neverball
```

A desktop display must be available. The font symlink under
`rootfs/usr/share/games/neverball/ttf` must resolve to a readable font in the
rootfs. This setup uses `DejaVuSans-Bold.ttf`. A missing target prevents a valid
font-enabled reproduction.

## Font corruption fixes

Two independent bugs affected the text:

- **JIT forwarding across CCMP/CCMN.** The IR optimizer treated the
  conditional compare's placeholder `dest=0` as a definition of X0. In
  FreeType's gray rasterizer, a following CSEL could then read stale X0 after
  dead-store elimination removed the earlier architectural store. CCMP and
  CCMN change flags only, so they now preserve the forwarded GPR value.
- **Copied SDL pixel formats.** Neverball copies the rendered surface's
  `SDL_PixelFormat` to its stack and changes the channel masks before calling
  `SDL_ConvertSurface`. The bridge formerly accepted only format pointers
  attached to known surfaces and returned NULL for this copy. Neverball's
  fallback retained SDL_ttf's padded rows, which its tightly packed texture
  conversion misread. The bridge now reads the guest format by value and
  translates its nested palette and color pointers before calling host SDL.

The shared SDL surface bridge publishes guest-addressed surface, format,
palette, and pixel storage while retaining private host objects. Surface API
rows use the generated `SDL_SURFACE` dispatch policy.

## Validation

```sh
make cross SRC=ctest/jit_ccmp_forward.c OUT=ctest/jit_ccmp_forward.elf
BIFROST_IR_VALIDATE=1 BIFROST_REGALLOC_CHECK=1 \
  BIFROST_JIT_VERIFY=1 BIFROST_JIT_VERIFY_MEM=1 \
  ./bifrost-emu ctest/jit_ccmp_forward.elf
./bifrost-emu --no-jit ctest/jit_ccmp_forward.elf
./scripts/test_sdl_surface_format.sh
make opgen-thunk-check
```

The conditional-compare regression fails on the saved pre-fix binary and passes
with JIT, interpreter, Tier 2 disabled, and an early Tier 2 promotion threshold.
The surface regression checks a stack-copied format with changed masks,
repacking a padded input pitch, nested copied palettes, and rejected invalid
palette counts. Its pre-fix control fails at the copied-format conversion.

A standalone SDL_ttf probe rendered `Neverball` at 24 pixels: both execution
modes produced a 129 × 28 surface, hash `a2d72ab02fef2a97`, and 1,292 nonzero
alpha pixels after the JIT fix. A captured game framebuffer verified readable
`Neverball`, `Play`, `Replay`, `Help`, `Options`, and `Exit` after both fixes.

The broader working-tree suite passed 231 of 232 checks; `test_mem_guard` timed
out during that run and passed on an isolated retry alongside the new CCMP
regression. Existing SDL, GL, and Vulkan thunk compatibility checks passed in
both JIT and interpreter modes. Those results include other uncommitted changes
and should not be read as validation of this commit in isolation.

The exact staged snapshot was also exported and built separately. Both new
regressions passed in JIT and interpreter modes against that build, with IR
validation, register-allocation checks, and JIT verification enabled for the
conditional-compare regression. Its generated thunk table passed the freshness
check. The build retained two existing warnings in display/input code.

## Interactive follow-up: 2026-10-01

User testing confirmed gameplay and replay, monitor selection, resolution
changes, ball-model changes, player-name editing, and successful configuration
writes that persist across launches. The earlier report of unavailable
resolution choices was superseded by the later confirmation that resolution
can be changed. Screenshots show readable options and the selected `blinky`
model preview. These are user-reported interactive results, not additional
automated coverage.

## Fullscreen dimensions fix: 2026-10-01

The root cause was an incorrect SDL2 ABI override for
`SDL_GetDesktopDisplayMode` and `SDL_GetCurrentDisplayMode`. SDL2 takes an
output `SDL_DisplayMode*` and returns 0 on success; the thunk instead treated
the return as a pointer and ignored the guest output. Neverball's fullscreen
creation reads the desktop mode to set and save its dimensions, so stale
values could poison the configuration and collapse the next window.
The override and its unused guest-pointer cache were removed; the normal
SDL2 thunk path fills the caller's buffer and returns the status.
See the [SDL2 API contract](https://wiki.libsdl.org/SDL2/SDL_GetDesktopDisplayMode).

The available Neverball source recreates its window when changing fullscreen,
and its SIZE_CHANGED handler only logs the event. The earlier dropped-event
explanation was a hypothesis; it does not explain this configuration write.
The focused regression fails on the pre-fix build and passes after the repair,
including resize-event delivery on the real desktop and SDL dummy driver,
with JIT and interpreter. It covers mode output, invalid indices, three
recreation cycles, fullscreen transitions, and preserved window dimensions.

The rebuilt game was also launched with the existing configuration. Its log
reported a 1920×1080 window and then a 3440×1440 fullscreen window, with a
matching SIZE_CHANGED event, followed by a usable 3440×1440 windowed window
after leaving fullscreen. Four focused fullscreen/SDL/GL checks passed in
each execution mode.

The user’s other agent already restored `neverballrc` to 1920×1080. This fix
preserves that configuration; it does not rewrite previously poisoned files.

Issues recorded at that stage (2026-10-01):

- Mouse input was reported broken in fullscreen. On 2026-10-02 the user
  explicitly confirmed normal mouse control in fullscreen gameplay and after
  switching back to windowed mode.
- Help-image corruption was subsequently traced to JPEG NEON decoding and
  repaired on 2026-10-02; see the validation below.

User-reported gameplay, replay, monitor/resolution selection, model and player
name changes, and configuration persistence remain recorded above.

## Crash after the first fullscreen repair

The live game subsequently crashed in JIT code while entering another menu.
The saved core showed a native load using guest address `0x51ce` instead of
`0x51a60f0`: RCX contained page index `0x51a6` after the permission guard,
and the following memory emitter reused it as an address with offset 40.
The permission guard now restores its scratch-register entry values on both
its hot continuation and cold interpreter exit. The user confirmed the
rebuilt game works, and logs show repeated resolution changes and window
recreation without repeating that crash.

`ctest/jit_memory_guard_address.c` covers a stack/pointer-load/conditional-exit
sequence shaped like the failing block. It passes under JIT, interpreter,
and JIT verification with memory checks; its reduced sequence also passes on
the old build, so it is coverage rather than an independent reproduction of
the original crash.

The broader run passed 239/241, with two failures: the visible-window
fullscreen regression hit a compositor-dependent size assertion (it now uses
hidden windows), and `test_mem_guard` failed intermittently. Repeated runs
also reproduced the latter failure on the saved pre-fix emulator (1/5), so
that existing high-address memory issue remains separate. Isolated memory
and signal checks passed, but this is not a clean full-suite result.

## JPEG preview and Help-image corruption fix: 2026-10-02

The hard-set preview (`shot-hard/grid.jpg`) was intact on disk, but guest
libjpeg's NEON IDCT produced blocky, incorrectly colored pixels. A direct
`ctest_real/jpegdec_t.elf` decode reproduced this in both execution modes;
`BIFROST_GUEST_ENV=JSIMD_FORCENONE=1` made both match QEMU.

The interpreter's saturating narrowing shifts treated the narrow destination
width as the source width, omitted sign extension, and used Q to change the
input lane count. RSHRN also entered this handler and incorrectly saturated.
The handler now consumes all 128 source bits, narrows to eight output bytes,
rounds in a wider temporary, applies the appropriate signed/unsigned clamp,
and sets sticky FPSR.QC on saturation. RSHRN rounds and truncates without
saturation. Upper-half forms preserve the destination's lower half.

The native JIT lowering also had incorrect widths, subop decoding, rounding
and saturation, and did not report QC. These instructions now use the corrected
interpreter handler; the translator and block-splitting predictor agree on
that fallback. Restoring native lowering requires equivalent regression
coverage, including rounding overflow and QC. No performance claim is made.

Validation: `ctest/jit_simd_sat_narrow.c` passes 672 checks under QEMU, JIT
(with IR validation and register-allocation checks), and interpreter. The
saved pre-fix interpreter fails 632 of them. The focused SIMD suite passes
19/19 in each Bifrost mode. Direct JPEG decodes of `shot-hard/grid.jpg`,
`gui/help1.jpg`, `gui/help2.jpg`, and `shot-easy/slalom.jpg` match QEMU's raw
RGB output byte-for-byte in both modes. This verifies the decode path;
it does not constitute interactive coverage of every menu or level.


## Stability follow-up: 2026-10-02

After the JPEG repair, `test_mem_guard` passed 100 consecutive runs under JIT
and 100 under interpreter, with a 20-second timeout per run and no failures.
This exercises the current single-threaded memory-range/high-address mapping
regression; it does not validate concurrent MAP_FIXED/JIT invalidation.

The quick suite then passed 238/238 under JIT and 236/236 under interpreter,
with zero failures. Interpreter mode intentionally skipped `tier2_smov` and
`zstd_compression` (the latter is a host harness that already checks both
engines in the JIT run). The initial JIT run rejected four stale fixtures:
`test_sigreturn_context`, `test_lse_inline`, `sha256_crypto`, and `sha256_full`.
After rebuilding them, their focused checks and a complete JIT quick rerun
passed. These results cover the current working tree, including other existing
changes, rather than the JPEG patch alone.

The user explicitly confirmed normal mouse control during fullscreen gameplay
and after returning to windowed mode. This closes the earlier fullscreen mouse
report for the tested local setup; it is interactive user evidence.
