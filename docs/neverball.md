# Neverball

Neverball's title and menu text render correctly with fonts enabled after the
2026-10-01 fixes. The user also confirmed that the game works during interactive
play. Automated validation below covers font rendering and its underlying
instruction and SDL ABI regressions; it does not exercise every level.

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
