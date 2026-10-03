# vkQuake

On 2026-10-02 the user confirmed working gameplay with the repaired default
JIT build and supplied four screenshots showing the world, HUD, main menu,
help, and load menu. This is a Vulkan game bring-up milestone, with remaining
camera and rendering issues; it is not a claim of complete compatibility.

## User screenshot checkpoint

- Gameplay and menus work according to the user.
- In a later checkpoint the user reported no sudden crashes during continued
  play. Session duration was not measured; this is observed interactive
  stability, not a completed soak test. The user also noted that additional
  unknown issues may remain.
- The later console screenshot shows pause/unpause, saving to `./id1/s0.sav`
  followed by `done.`, and repeated loading from that save. These paths were
  exercised, but saved-state fidelity has not been independently checked.
- Mouse camera sensitivity is much too high.
- The user reports a camera issue in the automatically played gameplay too.
  That sequence is demo playback (a prerecorded game). Its view angles are
  read from the demo in `Quake/cl_demo.c`, so ordinary mouse sensitivity
  alone is not established as the cause of both symptoms.
- The supplied JIT screenshots show strong blue/multicolor texture artifacts
  across walls, floors, and parts of the HUD. Texture corruption therefore
  affects the default JIT run too, not only the interpreter as previously
  recorded. The screenshots do not establish its cause.
- Interpreter mode reaches the demo, but clean rendering remains unverified.

Saved evidence: [gameplay](images/vkquake-2026-10-02/gameplay.png),
[main menu](images/vkquake-2026-10-02/menu.png),
[help](images/vkquake-2026-10-02/help.png), and
[load menu](images/vkquake-2026-10-02/load.png).
Later checkpoint: [console with save/load activity](images/vkquake-2026-10-02/console-save-load.png).

All saved screenshots are cropped to game content, removing the surrounding
desktop/video and window title bar. Game pixels are retained without resizing
or retouching.

![JIT gameplay with remaining texture corruption](images/vkquake-2026-10-02/gameplay.png)

Run the locally staged AArch64 binary and shareware data from the repository:

```sh
cd rootfs/vkquake
BIFROST_ROOT="$(realpath ..)" ../../bifrost-emu ./vkquake -basedir .
```

Add `--no-jit` before `./vkquake` to run the interpreter.

## Pointer corruption fixed

SDL renderer workers use the interpreter even during a default JIT run.
In `Sky_DrawSky`, `fcsel s31,s25,s24,gt` at guest PC `0x4329b4` matched an
obsolete, overly broad FP-to-integer conversion handler. It wrote the GPR
zero-register slot instead of FP register 31. Subsequent pointer copies using
XZR then added one, corrupting Vulkan handles and eventually faulting.

Removed the duplicate handler: the existing scalar conversion handler already
distinguishes legal conversions from FCSEL. FP moves and conversions now also
discard writes to GPR register 31 while retaining valid FP register 31 writes.
Thread exception reports include guest PC, SP, and LR.

`ctest/jit_fcsel_high_regs.c` checks high source registers, FP destination 31,
both widths and branch outcomes, aliasing, GPR preservation, and discarded XZR
writes. All 61 checks pass under QEMU, JIT, and interpreter. The saved pre-fix
binary fails the first check and then crashes. The focused FP suite passes
12/12 in each emulator mode.

This reproduction supersedes the earlier stale-guest-state diagnosis in the
2026-09-03 session history. Working JIT gameplay does not establish long-run
stability or resolve the camera and texture issues recorded above.
