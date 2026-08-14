# Display Thunk & Windowing System — Review Notes

Review of the display thunk and windowing subsystem as of v1.5.0.alpha.
Files: `src/frost_graphics/display_thunk.cpp` (~1580 lines),
`src/frost_graphics/display_proxy.cpp` / `include/frost/display_proxy.hpp`,
`src/frost_graphics/thunk_common.hpp`, `include/frost/display_thunk.hpp`.

## Architecture

- `DisplayThunk` forwards guest Vulkan / Wayland / X11 / XShm / GLX / XRandR /
  Xkb / GBM / DMA-BUF calls to the host. It shares the `__NR_bifrost_thunk`
  syscall with `GraphicThunk`/`AudioThunk`; the dispatcher routes by
  `symbol_id` (per-thunk, `ID_BASE = 0x2000`, `ID_MASK = 0x3000`).
- Guest handles (`Display*`, `Window`, `wl_display*`, `wl_surface*`) are guest
  memory addresses backed by the emulator `Memory`. The handle table
  (`DisplayProxy::handles_`) is a `std::vector<HandleEntry>`; a small
  `HandleHdr { type, index }` is written at each guest address.
- `THUNK_PROXY` entries: if the host library is absent, the call is routed to
  `DisplayProxy` (one SDL2 window + renderer + streaming texture), which
  lazily initializes on the first X11/Wayland call.
- `SymbolEntry` is defined exactly once in `thunk_common.hpp` and shared by
  `GraphicThunk` and `DisplayThunk` (ODR fix). Audio thunks keep the separate
  `ThunkSymbolEntry`. Do NOT reintroduce a TU-local `SymbolEntry` anywhere.

## Fixed (this review)

### Third pass (trampoline encoding + proxy-first routing)

- **Trampoline encoding bug (the one that made the whole X11/Wayland path
  dead):** `DisplayThunk::register_function_` and `write_trampoline_` wrote
  `movz x0, #sym_id` — the hand-rolled `0xD2800000 | (sym_id << 5)` dropped
  the Rd field (bit 0-4 = 0 → x0), even though the comment said "movz x9".
  The syscall dispatcher (`misc.cpp:948`) reads the symbol id from `x9`, so
  every display call carried the id in the wrong register: XOpenDisplay
  (`sym_id=0x20e2`) arrived as a `[thunk] dispatch: glClear` with `a0=0x20e2`,
  and no `[display-thunk] dispatch` line ever appeared. GraphicThunk and
  AudioThunk use the shared `MOVZ_Xd_IMM16(9, …)` helper and were unaffected.
  Both writers now call `write_thunk_trampoline(...)` like AudioThunk does.
- **Proxy-first routing for THUNK_PROXY symbols:** the dispatch used to prefer
  the host library whenever `host_fn` was present. But a host `Display*` (e.g.
  `0x55a7…` from `XOpenDisplay`) is a HOST pointer: marking arg0 as a pointer
  sent it down the bounce path as guest memory → `XCreateSimpleWindow` crashed
  (SIGSEGV, exit 139). The DisplayProxy's handles are guest addresses and
  round-trip correctly. Now, for `THUNK_PROXY` symbols the proxy is always
  tried first (lazily initialized, `ready()` checked); the host library is only
  a fallback when no SDL proxy can be initialized (headless host).
- **Validated end-to-end:** `DISPLAY=:0 ./bifrost-emu ctest_real/x11_minimal.elf`
  now exits 0 (`XOpenDisplay OK`, `XCreateSimpleWindow OK win=…`, `ALL PASS`)
  instead of `FAIL (XOpenDisplay returned NULL)` / SIGSEGV.

### Second pass (X11 host-path masks + crash guards)

- `XCreateSimpleWindow` — was `REG_X11_PTR` (no `n_stack`); the 9th arg
  (background pixel) lives on the guest stack and was never read in the host
  path. Now `REG_X11_EX(..., 0x01, 1)`.
- `XCreateWindow` mask `0x501 → 0xA01` — was marking XID `visual`@8 and XID
  `valuemask`@10 as pointers and missing the real `Visual*`@9 and
  `XSetWindowAttributes*`@11. Wrong masks send small XID values down the 64 KiB
  bounce path (see crash guard below).
- Event functions: `XWindowEvent`/`XCheckWindowEvent` `0x0B → 0x09`,
  `XMaskEvent`/`XCheckMaskEvent` `0x03 → 0x05`,
  `XCheckTypedEvent` `0x03 → 0x09`,
  `XCheckTypedWindowEvent` `0x0B → 0x11` (Window XIDs and ints were marked as
  pointers; `XEvent*` outs were missed).
- `XTextExtents` `0x90 → 0x3F` (was missing the font struct, string, and the
  three `int*` outputs).
- `XInternAtoms` `0x1F → 0x13` (count / only_if_exists are ints).
- `XGetWindowProperty` — was mask `0xFE`, `n_stack=0` on a 12-arg function;
  now `REG_X11_EX(..., 0xF81, 4)` so the five output pointers on the stack are
  read and translated.
- `XShmGetImage` — `XImage*`@2 was untranslated; now `REG_XEXT_PTR(..., 0x04)`.
- `DisplayThunk::dispatch` — the bounced-pointer writeback now catches
  `Memory::write`'s `UnmappedMemory`. A wrong mask (or any non-window pointer)
  used to throw out of `dispatch` into the syscall handler, corrupting the
  emulator's control flow. This also caps the blast radius of the remaining
  "full 64 KiB writeback" design (see Skipped).
- `DisplayProxy::XDrawRectangle` — clamped via signed ints like `XFillRectangle`
  (unsigned `w += x` underflowed for negative x).

### First pass

- `proxy_dispatch_` read guest-stack slots for X11 functions whose parameters
  all live in x0–x7, passing garbage (masked because the proxy `(void)`s the
  affected params). Corrected to use the real register args and drop the
  stack reads:
  - `XCreateGC` — screen/visual aren't real Xlib args → pass 0, 0.
  - `XSetLineAttributes` — `join_style` is x5, not a stack slot.
  - `XChangeProperty` — `data` is x6, `nelements` is x7.
  - `XCopyGC` — `dest_gc` is x3.
  - `XChangeGC` — `values` is x3.
  - `XSetWMProtocols` — `count` is x3.
- Removed the duplicate `REG_WL(wl_display_dispatch_queue)` so the intended
  `0x02` pointer mask (arg 1 = `wl_event_queue*`) actually takes effect
  (idempotent registration previously let the plain `REG_WL` win).

## Skipped / deferred (real issues, not yet fixed)

### 1. Vulkan pointer-arg masks are systematically wrong (17 of 23)

The `REG_VK_PTR(name, mask)` registrations translate opaque handles
(`VkDevice`, `VkPhysicalDevice`, `VkQueue`) and integer args as guest
pointers, and miss the real output pointers. In the host-Vulkan path this
passes corrupted args to the driver. Not fixed here because the host-Vulkan
path has no guest test to verify against (the demo target is SDL2+GL) and
a wrong correction can silently break the one limping case.

Corrected masks (verified against the Vulkan 1.x signatures):

| Function | Current mask (wrong) | Correct mask |
|---|---|---|
| `vkCreateInstance` | `0x03` | `0x07` (bits 0,1,2) |
| `vkDestroyInstance` | `0x02` | `0x02` |
| `vkEnumeratePhysicalDevices` | `0x06` | `0x06` |
| `vkGetPhysicalDeviceProperties` | `0x02` | `0x02` |
| `vkCreateDevice` | `0x07` | `0x0E` (bits 1,2,3) |
| `vkDestroyDevice` | `0x02` | `0x02` |
| `vkGetDeviceQueue` | `0x06` | `0x08` (bit 3 = ppQueue) |
| `vkCreateSwapchainKHR` | `0x07` | `0x0D` (bits 0,2,3) |
| `vkGetSwapchainImagesKHR` | `0x06` | `0x08` (bit 3 = pImages) |
| `vkAcquireNextImageKHR` | `0x1E` | `0x20` (bit 5 = pImageIndex) |
| `vkQueuePresentKHR` | `0x02` | `0x02` |
| `vkAllocateCommandBuffers` | `0x06` | `0x06` |
| `vkQueueSubmit` | `0x06` | `0x04` (bit 2 = pSubmits) |
| `vkMapMemory` | `0x0E` | `0x20` (bit 5 = ppData) |
| `vkFlushMappedMemoryRanges` | `0x02` | `0x04` (bit 2 = pRanges) |
| `vkWaitForFences` | `0x06` | `0x04` (bit 2 = pFences) |
| `vkGetQueryPoolResults` | `0x0E` | `0x20` (bit 5 = pData) |
| `vkUpdateDescriptorSets` | `0x02` | `0x14` (bits 2,4) |
| `vkAllocateDescriptorSets` | `0x02` | `0x06` (bits 1,2) |
| `vkCreateGraphicsPipelines` | `0x0E` | `0x28` (bits 3,5) |
| `vkCreateImage` | `0x06` | `0x0D` (bits 0,2,3) |
| `vkBindBufferMemory` | `0x06` | `0x07` (bits 0,1,2) |
| `vkBindImageMemory` | `0x06` | `0x07` (bits 0,1,2) |

Caveat when fixing output pointers (`pInstance`, `ppQueue`, `pDevice`,
`pPipelines`, `pImageIndex`): the generic `translate_ptr` bounce path writes
the full 64 KiB buffer back to the guest, which would clobber adjacent guest
memory for an output struct that falls outside the 4 GiB direct window.
Output pointers are usually small heap objects inside the direct window
(identity translation, no bounce), so this only bites stack outputs.

### 2. Generic host dispatch caps at 9 args (`Fn9`) — FIXED

`DisplayThunk::dispatch` now casts the host call to `Fn10`/`Fn11`/`Fn12` when
`n_stack` is 2/3/4 so stack args 9–11 are actually passed to the host, and
`REG_X11_EX`/`REG_RANDR_EX` register `n_stack` for `XCreateWindow` (3),
`XQueryPointer` (1), `XWarpPointer` (1), and `XRRSetCrtcConfig` (2) so their
stack args are read and pointer-translated in the host path.

### 3. X11 registration masks have similar errors (host path only) — FIXED

Corrected masks: `XCreateGC 0x05 → 0x09` (args 0,3), `XChangeGC 0x05 → 0x09`
(args 0,3), `XAllocColor 0x03 → 0x05` (args 0,2), `XFreeColors 0x07 → 0x05`
(args 0,2), `XSetDashes 0x07 → 0x09` (args 0,3).

### 4. Dead / latent code (partly fixed)

- ~~The float-only and mixed-FP dispatch branches are unreachable for display
  symbols (no `REG_*` passes `n_float` or `THUNK_MIXED_FP`); the mixed-FP
  branch silently drops unsupported `(n_int, n_float)` combos~~ **FIXED**: the
  mixed-FP branch now warns under trace when an unsupported `(n_int, n_float)`
  combo is hit instead of silently dropping; the float-only path warns when
  `n_float > 4` (only the first 4 floats are forwarded).
- ~~`DisplayProxy::present()` drains SDL events but ignores `SDL_QUIT`~~
  **FIXED**: it now sets a `quit_requested_` flag on `SDL_QUIT` and
  `SDL_WINDOWEVENT_CLOSE`, exposed via `DisplayProxy::quit_requested()`
  (matches the `FrostGraphics` drain pattern). Guest-visible wiring
  (e.g. a `WM_DELETE_WINDOW` ClientMessage) is future work.
- `DisplayThunkImpl::vk_handle_map_` is declared but never used. Not removed
  (Vulkan-adjacent; out of scope of the no-Vulkan fix pass).
- `XGetAtomName` returns a pointer into a `static char[32]`; the proxy path
  re-caches it via `cache_host_string_` (fine); a second call overwrites the
  first string, which Xlib permits (only one live pointer expected).

## Verification

- `make` — clean build, no new warnings.
- `make check-all` — 190/190 pass, 0 fail.
- `./scripts/run_tests.sh --unit` — 41/41.
- `DISPLAY=:0 ./bifrost-emu ctest_real/test_sdl_gl_triangle.elf` — exit 0.
- `DISPLAY=:0 ./bifrost-emu ctest_real/test_gl_state.elf` — ALL PASS.

## Real-world run: rudolf-cart (SDL2 + legacy GL + GLU)

Cross-compiled `~/Projects/rudolf-cart` to a static musl AArch64 guest
(`main.c` unchanged; shims in `guest/`), runs playable under `DISPLAY=:0`
with music + SFX + screenshots.

### New thunk rows added to `tools/opgen/thunk_dp.txt`

| Symbol | ARGS | SIZE |
|---|---|---|
| `glFogf` | `if` | - |
| `glFogfv` | `ip` | - |
| `glFogi` | `ii` | - |
| `glFogCoordf` | `f` | - |
| `glColorMaterial` | `ii` | - |
| `glTexEnvi` | `iii` | - |
| `glLoadMatrixf` | `p` | - |
| `glMultMatrixf` | `p` | - |
| `glDeleteTextures` | `ip` | - |
| `glReadPixels` | `iiiiiiz` | `READPIXELS` |
| `SDL_GL_SetSwapInterval` | `i` | - |
| `SDL_OpenAudioDevice` | `pippi` | - |
| `SDL_QueueAudio` | `izi` | `QUEUEAUDIO` |
| `SDL_GetQueuedAudioSize` | `i` | - |
| `SDL_ClearQueuedAudio` | `i` | - |
| `SDL_PauseAudioDevice` | `ii` | - |
| `SDL_LockAudioDevice` | `i` | - |
| `SDL_UnlockAudioDevice` | `i` | - |
| `SDL_LoadWAV` | `pppp` | - |
| `SDL_FreeWAV` | `p` | - |

`make opgen-thunk` regenerates the header; `make opgen-thunk-check` is the CI
guard. Current table: **422 symbols**.

### New `SizeKind`s (bounce-buffer sizing)

The default 64 KiB bounce corrupts memory when a guest out-buffer or a large
upload exceeds it. Two new kinds in `thunkgen.py` + `thunk.cpp`:

- `READPIXELS` — `glReadPixels(x, y, w, h, fmt, type, pixels)` (arg 6): bounce =
  `w * h * channels * type_size` (channels/type from the GL enums; cap 64 MiB).
  A 1280×720 RGB read is 2.7 MB — was smashing the 64 KiB bounce and dumping
  core in autoshot. Requires the pixels arg to be `z` so the generator's
  size-needs-pointer rule passes.
- `QUEUEAUDIO` — `SDL_QueueAudio(dev, data, len)` (arg 1): bounce = `len` arg
  (cap 64 MiB). The 35 MB music WAV was memcpy'd 35 MB from a 64 KiB bounce.

### Guest-side shims (`~/Projects/rudolf-cart/guest/`)

- `SDL2/SDL.h`, `GL/gl.h`, `GL/glu.h` — stub structs mirroring the host x86-64
  SDL2 layout so offsets line up.
- `guest_gl.c` — GL forwarders via `dlopen`/`dlsym` through the `0x1002`/`0x1003`
  internal syscalls, plus guest-side `glOrtho` and a mini-GLU. GLU takes
  `GLdouble` args; the thunk pipeline has no `d` arg kind (`ARGS = ifpz` only),
  so `gluCylinder`/`gluDisk` tessellate and `gluPerspective`/`gluLookAt` build
  matrices on the guest.
- `guest_sdl.c` — SDL forwarders plus:
  - Guest-side `SDL_LoadWAV` (RIFF-PCM parser): host SDL returns an allocated
    pointer the guest can't dereference, so the WAV is parsed guest-side.
    `fmt`/`ch`/`bits` are **16-bit** fields — reading them with a 32-bit LE read
    bleeds the channel byte into `fmt` and rejects every file as non-PCM.
  - `SDL_OpenAudioDevice` — host SDL's audio thread must never execute guest
    code, so callback devices are opened with `callback=NULL` and a **guest
    audio-fill thread** runs the guest callback into a stack buffer, feeding the
    host device via `SDL_QueueAudio` (paced at real time via `SDL_GetTicks`;
    `SDL_ClearQueuedAudio` drops stale audio when the host drains slower than
    real time). Without the always-run callback the game's 24 coin voices
    never deactivated (callback stopped firing when the queue was full) and
    coins silently stopped after 24 pickups.
  - `SDL_FreeWAV` — no-op: the buffer is guest `malloc`'d (musl); forwarding to
    host `free()` aborts glibc with "double free or corruption".

### Fixed crashes along the way

1. `glReadPixels` 2.7 MB write into 64 KiB bounce → core dump (fixed by
   `READPIXELS`).
2. `SDL_QueueAudio` 35 MB memcpy from 64 KiB bounce → heap corruption (fixed by
   `QUEUEAUDIO`).
3. `SDL_FreeWAV` host `free()` on a musl buffer → "double free or corruption"
   at exit (fixed by no-op).
4. WAV parser 16-bit fields read as 32-bit → every WAV rejected (fixed).

### Fullscreen / resize bug (SDK constant drift)

Fullscreen was "broken" (content didn't scale, stayed stretched at 1280×720):
the guest `SDL2/SDL.h` stub defined `SDL_WINDOWEVENT_SIZE_CHANGED = 9`, but the
real SDL2 enum is `6` (9 is `SDL_WINDOWEVENT_RESTORED`). The game's event loop
compared `e.window.event` against 9, never matched the host's `SIZE_CHANGED`
(6), so `winW`/`winH` never updated → `glViewport`/`gluPerspective`/`glOrtho`
kept the 1280×720 projection inside a fullscreen window → distorted, blurry
render. Verified with a guest event-probe (`SDL_SetWindowSize` → poll): the
host sends `SIZE_CHANGED=6` with correct `data1`/`data2`. Fixed by spelling out
the whole `SDL_WindowEventID` enum (SHOWN=1 … CLOSE=14) in the stub header.
Takeaway: SDK enum constants in guest stubs must match the host's exact values —
a "looks close enough" value silently disables the feature it gates.

### Mouse input fixes (struct layout drift + missing AdvSIMD scalar SCVTF)

Mouse "didn't work" in two layers, both verified by guest-side debug logs:

1. **Guest `SDL_MouseMotionEvent` / `SDL_MouseButtonEvent` had extra padding**
   bytes. The host (sdl2-compat over SDL3) lays out motion as
   `type,ts,x,y,xrel,yrel` = x@20/y@24 and button as
   `type,ts,windowID,which,button,state,clicks,padding1,x,y` = x@20/y@24; the
   stub had x@24/y@28 with a spare 8-byte field before them, so the guest read
   `host.y` into `mouseX` and `host.xrel` into `mouseY` (clicks read zeros).
   Hover was broken; clicks fired at garbage coordinates. Fixed by removing the
   padding and reordering the button struct (`button,state,clicks,padding1,x,y`).
   Takeaway: struct layout of a forwarded event MUST be verified against the
   host ABI, not assumed from memory — a `printf` of raw coordinates
   (`bx by mx my`) exposes exactly which host field each guest field aliases.

2. **`[FP-NOP] scvtf s0, s0, #1` (0x5F3FE400) silently skipped.** The click
   handler's `inRect()` did `scvtf s0, w6, #1` (GPR-source, handled) so the
   debug print's hit-test looked correct (`ir=1`) while the actual
   `if (inRect(...)) beginRide(1)` used `scvtf s0, s0, #1` — AdvSIMD scalar
   fixed-point SCVTF with **FP-register source**, which hit the unknown-op
   `[FP-NOP]` fallback. The dest kept the raw integer bit pattern as a denormal
   float, so the rect test never matched and `beginRide` never ran. Fixed in
   `interp_fp.cpp`: group `(op & 0xDF80E400) == 0x5F00E400`, fbits = 64 −
   bits[21:16], bit 29 = U, bit 22 = S/D size, bits[12:11] = 00 SCVTF/UCVTF /
   11 FCVTZS/FCVTZU (with saturating FP→int). JIT already routed unknown scalar
   FP through CALL_INTERP, so only the interpreter needed the handler.
   Takeaway: the [FP-NOP] log is a code smell for a missing SIMD-scalar
   conversion; check the disassembly of the *actual* failing block (pc 0x40160c)
   rather than the debug equivalent.

### Verification

- `make check-all` 190/190 after the `SDL_ClearQueuedAudio` row + size kinds.
- `DISPLAY=:0 timeout 25 ./bifrost-emu ./rudolf-cart.elf` — runs full 25 s,
  no crash (killed by the timeout).
- `DISPLAY=:0 ./bifrost-emu ./rudolf-cart.elf autoshot` — exit 0, writes
  `/tmp/opencode/shot_ready.ppm` + `shot_play.ppm` (1280×720 RGB, real frame
  content).
- Fullscreen + window resize now scale like native (user confirmed working).
- Mouse hover + menu clicks now work end-to-end in `rudolf-cart` (user
  confirmed): the START button starts the ride, no more `[FP-NOP]` at
  pc=0x40160c.
- `autoshot` still exit 0 + writes both PPMs after the mouse/FP fixes.
