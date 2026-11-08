# Host graphics, audio and input

Guest programs use the host's real GPU, audio and input devices. Graphics,
display and audio calls are forwarded through a **thunk** layer; input comes
from SDL and is exposed as Linux device nodes.

## Graphics, audio and display thunking

bifrost-emu forwards guest GL/EGL/SDL2/ALSA/Vulkan calls to the host's
native libraries via a **thunk** layer. This is **enabled by default** —
no env var needed. Build with `make USE_SDL2=1 USE_THUNK_GL=1` so host
headers/libs are linked.

Marshalling supports AAPCS64 stack args (e.g. `glTexImage2D` data),
FP args in `v0..` (`glClearColor`, `glVertex3f`), host→guest string
returns (`glGetString`, `SDL_GetError`), nested `glShaderSource`
pointers, and bounce buffers for pointers whose range is not one accessible
host range (`SDL_PollEvent`). Trampolines end with `ret` after `svc`.
dlopen of libGL/libSDL2 uses the thunk when the on-disk `.so` is not
AArch64 (so host x86_64 libs are never executed as guest code).

A host without a sound device behaves like a silent one: audio streams drain
in real time, so a guest neither hangs on `/dev/dsp` nor runs unpaced.

Modern GL is covered: `glMapBuffer`/`glMapBufferRange` map the host
buffer and give the guest a guest-window bounce of the same range, copied
into the host mapping on flush, on unmap and, for persistent-coherent
mappings, before buffer-consuming calls (PCWFC); this works for immutable
`glBufferStorage` buffers. GL3.3+/4.x rows cover
uniform blocks, instancing, compute, transform feedback, sampler and
query objects, DSA (`glCreateBuffers`/`glCreateVertexArrays`),
`glBufferStorage`, and `glTexImage3D`. GLFW callback setters
(`glfwSetKeyCallback`, `glfwSetCursorPosCallback`, …) store guest
AArch64 callbacks and deliver them after each `glfwPollEvents`.

Demo:

```bash
make USE_SDL2=1 USE_THUNK_GL=1
make cross SRC=Tests/Apps/test_sdl_gl_triangle.c OUT=Tests/Apps/test_sdl_gl_triangle.elf
DISPLAY=:0 ./bifrost-emu Tests/Apps/test_sdl_gl_triangle.elf
```

Thunk switches (all thunking is on by default):

```bash
BIFROST_THUNK_TRACE=1            # Trace graphic/audio/display thunk calls
BIFROST_AUDIO_TRACE=1            # Trace only audio thunk calls: x0-x3 and the result
BIFROST_AUDIO_DUMP=1             # Keep the guest's PCM for the WAV that --audio-dump PATH writes at exit
BIFROST_AUDIO_DUMP=/tmp/a.pcm    # Write the guest's raw PCM to that absolute path as it arrives (survives a signal-ended managed app)
BIFROST_TEXT_SCRIPT='down:H,text:h;up:H;edit:ni;text:x'  # Android window tests: push key/text events
BIFROST_NO_THUNK_GRAPHICS=1      # Disable GL/EGL/SDL2 thunking
BIFROST_NO_THUNK_AUDIO=1         # Disable ALSA/PulseAudio thunking
BIFROST_NO_THUNK_DISPLAY=1       # Disable Vulkan/Wayland thunking
BIFROST_VK_NO_PLACED=1           # Keep vkMapMemory guest bounces instead of placed maps (A/B)
BIFROST_VK_NO_ASTC=1             # Do not offer ASTC LDR textures to Vulkan guests on hosts without ASTC (A/B)
BIFROST_ASTC_THREADS=4           # Threads transcoding one ASTC upload to BC7 (default: half the cores, at most 8); helpers run at nice 19
BIFROST_ASTC_CACHE_MB=256        # Finished BC7 transcodes kept for re-uploaded textures (0 = off, default 256)
BIFROST_ASTC_STATS=1             # Log ASTC upload volume, time and cache hits every 64 uploads
```

Graphical programs log their first successful frame presentation to stderr
(`--no-render-log` suppresses it). Presentation statistics in the window
title, a separate window or an overlay are described in
[window stats](window-stats.md). Android windows render at the host
display's native pixel density; see [window plugins](Android/ANDROID_WINDOW_PLUGINS.md).

## Diagnostic switches

These are for developers and bug reports; names and output can change without notice. Almost all are read by presence:
**any value, including `0`, turns the switch on**, so `unset` one to turn it off. Where a value is shown it is parsed.
The user-facing switches are listed above and in the [README](../README.md).

### Logging, crash reports and process control

| Switch | What it does |
|---|---|
| `BIFROST_VERBOSE` | Verbose logging (also `[log] verbose` in the config). |
| `BIFROST_CRASH_DUMP` | A detailed crash report: registers, 16 stack words and a frame-pointer call chain, after the one-line `[crash]` report that is always printed. |
| `BIFROST_TRACE_CRASH` | The registers at a `BRK #1000` (the emulator's own trap). |
| `BIFROST_DBG_GUARD` | Backtraces on decode errors. |
| `BIFROST_CRASH_BT` | A guest backtrace when a signal is delivered. |
| `BIFROST_ALLBT / BIFROST_ALLBT2 / BIFROST_BTRAW` | Dump every thread's stack; the second walks frames differently; the third prints raw (unsymbolized) frames. |
| `BIFROST_TRAP_CORRUPT` | Raise a host SIGTRAP at the first corrupt Vulkan command-buffer handle (an odd, byte-shifted value), with the guest pc and lr still in place. |
| `BIFROST_ANDROID_TIMEOUT_SECS=N` | End an Android app's process after N seconds (tests). |
| `BIFROST_GUEST_ENV='A=1;B=2'` | Extra environment variables for the guest, split on `;`. |
| `BIFROST_NO_FAKE_TTY` | Do not answer `TCGETS` as a terminal when the host descriptor is not one (by default `isatty()` returns true for guest stdio). |

### Tracing and watches

| Switch | What it does |
|---|---|
| `BIFROST_EXEC_TRACE` | Thread exec tracing. |
| `BIFROST_SYSCALL_TRACE / BIFROST_SYSCALL_TRACE_ALL` | Log syscalls; the second also logs the ones the tracer has no name for, with their first three arguments. |
| `BIFROST_STEP_TRACE` | Log each interpreter fallback (`CALL_INTERP`) the JIT takes. |
| `BIFROST_FUTEX_TRACE / BIFROST_FUTEX_BT` | Futex wait and wake lines; backtraces on futex waits. |
| `BIFROST_PPOLL_TRACE / BIFROST_PPOLL_PEEK` | `ppoll` entry and revents; message peeks before and after. |
| `BIFROST_SIGNAL_TRACE` | Trace in the time and sleep syscalls (sleeps and signals). |
| `BIFROST_TRACE_MADVISE / BIFROST_TRACE_RUSAGE` | `madvise` lines; log a failing `getrusage`. |
| `BIFROST_ALLOC_TRACE / BIFROST_MAPFIX_TRACE / BIFROST_MEMGUARD` | Print every `mmap_alloc` (address, size); trace mappings that replace others (mimalloc commits and decommits arenas this way); check for allocation-accounting drift that would explain a zeroed live heap. |
| `BIFROST_MEMSTATS=N` | A reporter thread printing memory statistics every N seconds (5 when N is 0). |
| `BIFROST_TRACE_PC / BIFROST_TRACE_PC_MAX=N` | Trace executed guest pcs, at most N instructions (default 10000). |
| `BIFROST_WATCH=addr[:size]` | Log every store to that address (hex); `BIFROST_WATCH_TRAP` raises a host SIGTRAP on a hit; `BIFROST_WATCH_VAL=v` reports only stores of that value; `BIFROST_WATCH_BADEXT` flags 2-byte stores whose value exceeds 4096 in magnitude. |
| `BIFROST_WRITE_TRACE=path / BIFROST_WRITE_TRACE_RANGE=lo:hi` | A complete guest store log (address, size, thread, guest pc, value), optionally limited to a hex address range. |
| `BIFROST_INPUT_TRACE / BIFROST_INPUT_WATCH` | A bounded log of SDL mouse and input-mode events; comma-separated guest `u32` addresses to watch. |
| `BIFROST_FRAME_TRACE` | A per-present frame counter. |
| `BIFROST_IFUNC_TRACE / BIFROST_SCRATCH_TRACE / BIFROST_LD2_DBG` | Log an `ifunc` resolver that threw during load; print the callback scratch-stack ranges per CPU; second-run dynamic-linker diagnostics. |
| `BIFROST_NAN_TRACE / BIFROST_SIMD_TRACE / BIFROST_SIMD_COLLECT` | NaN writes in floating-point helpers; a log of unhandled SIMD instructions; enumerate-and-nop mode for them. |
| `BIFROST_XTRACE` `BIFROST_XFULL` `BIFROST_XBT` `BIFROST_XPROBE` `BIFROST_XRECV_TRACE` `BIFROST_XDELAY` `BIFROST_XSEQLOG` `BIFROST_XCAP` `BIFROST_XWRCHECK` | The X11 forwarding diagnostics, in that order: wire and descriptor lifecycle, full request payloads, opcode backtraces, request probes, receive traces, a delayed-delivery simulation, a wire sequence capture file, a capture output root, and write-corruption checks. |

### JIT and the optimizer

| Switch | What it does |
|---|---|
| `BIFROST_JIT_THRESHOLD=N` | Interpret a block for N instructions before compiling it (0, the default, compiles from the start; `[jit] threshold` in the config). |
| `BIFROST_JIT_TRACE / BIFROST_JIT_DUMP / BIFROST_DUMP_PC=0x...` | Trace JIT dispatch; dump each block's IR after the optimizer; restrict that dump to one block pc. |
| `BIFROST_PERF_MAP` | Write `/tmp/perf-<pid>.map` with guest block names for `perf`. |
| `BIFROST_BLOCK_PROF / BIFROST_CLASS_PROF / BIFROST_CLASS_PROF_PERIOD=N` | Print the JIT's per-block profile; count interpreter instruction classes, reported every N instructions (default 20,000,000). |
| `BIFROST_JIT_DISPATCH_STATS / BIFROST_REGALLOC_STATS / BIFROST_FALLBACK_PROFILE=path` | TLS dispatch-cache telemetry; per-block spill and reload density; a complete per-thread inventory of interpreter fallbacks (`%p` in the path becomes the host pid). |
| `BIFROST_TIER2_TRACE / BIFROST_TIER2_TRACE_VERBOSE / BIFROST_T2_REGIONDUMP` | Tier-2 candidates and timing decisions; discovery and compiler details; a hexdump of each published region for offline `objdump`. |
| `BIFROST_JIT_SUPERBLOCK_LIMIT=N` | Blocks per superblock, clamped to 2..8. |
| `BIFROST_JIT_VERIFY_EVERY=N / BIFROST_JIT_VERIFY_MEM / BIFROST_JIT_VERIFY_MEMFULL` | JIT differential verification: check every Nth block (default 1); also compare memory effects; snapshot and compare a whole memory range (range `BIFROST_MEMFULL_START` to `BIFROST_MEMFULL_END`, hex, default 0x10000000 to 0x14000000). |
| `BIFROST_VERIFY_TRACE / BIFROST_VERIFY_UNTRACKED / BIFROST_INTERP_BL_CALL / BIFROST_TRACE_SIMDST16` | Print block entry state during verification; also verify blocks with untracked stores (their re-runs can apply stores twice, so trust only divergences that repeat); run `BL` callees interpreted so the full-memory verifier sees them; log unresolvable 16-byte SIMD stores. |
| `BIFROST_JIT_SLOW_STORES / BIFROST_GUARD_ALLOCS / BIFROST_MEM_TRACE / BIFROST_DBG_PC / BIFROST_DEBUG_INLINE` | Send JIT stores through the slow path so `BIFROST_WATCH` sees them with a pc (very slow); the first two also select the old checked memory path that `BIFROST_JIT_SOFTWARE_GUARD` does, for comparison; trace slow-path memory accesses; print the processor state at entry of chosen blocks; log leaf-inlining scans. |
| `BIFROST_NO_OPT / BIFROST_NO_DSE` | Skip the IR optimizer; skip dead-store elimination. |
| `BIFROST_NO_BL_CALL / BIFROST_NO_BLR_CALL` | Turn off the native call within a block for `BL` or `BLR`. |
| `BIFROST_NO_CALLREGION / BIFROST_NO_DESTSRC / BIFROST_NO_RDCE / BIFROST_NO_LICM / BIFROST_NO_KEEPRES / BIFROST_NO_EXITCHAIN` | Tier-2: regions end at calls; skip the destination-as-source rewrite; skip region dead-code elimination; skip loop-invariant hoisting; skip steering hot values to stay resident across a loop; skip chained region exits. |
| `BIFROST_ENABLE_LEAF_INLINE / BIFROST_HOT_INTERP / BIFROST_FP_NATIVE_GATE=mask / BIFROST_REGALLOC_CHECK` | Enable leaf inlining (translations shaped by it are not recorded in the persistent cache); enable the hot-interpreter path (measured 8 to 10% slower in a game, so off); a bit mask of floating-point forms the JIT runs natively (default all); run the register-allocator checker. |
| `BIFROST_JIT_DISK_CACHE_COMPACT_DELAY=s / BIFROST_JIT_DISK_CACHE_KB=n` | Seconds to wait before compacting the persistent cache (default 15); a cache budget in KiB, for tests (the normal knob is `BIFROST_JIT_DISK_CACHE_MB`). |

### Graphics, audio and Android

| Switch | What it does |
|---|---|
| `BIFROST_GRAPHICS_VERBOSE / BIFROST_AUDIO_VERBOSE` | Print why SDL video or audio failed to start. |
| `BIFROST_NO_DISPLAY_SUBST / BIFROST_NO_ELIDE / BIFROST_GUI_DBG / BIFROST_FB_DUMP` | Do not substitute the display in `eglGetDisplay`; do not elide redundant GL calls; log vertex-array pointer arguments; dump frames at `SDL_GL_SwapWindow`. |
| `BIFROST_AUDIO_STATS / BIFROST_AUDIO_PUMP=0 / BIFROST_AUDIO_NO_CB / BIFROST_AUDIO_FAIL_OPEN` | A roughly 2-second audio heartbeat (callbacks and per-stream backlog); use the inline fallback instead of the audio pump; do not use the guest callback for SDL devices; make SDL audio open fail so the guest takes its own no-audio path. |
| `BIFROST_ANDROID_LEGACY_CB / BIFROST_ANDROID_TAP` | Use the legacy surface callbacks; inject synthetic taps so the input path is exercised without a person (tests). |
| `BIFROST_ANDROID_NETWORK_INTERFACE` | The host interface the guest sees as its network; Asgard sets it (default `wlan0`). |

### Set by the emulator or used only by tests

| Switch | What it does |
|---|---|
| `BIFROST_NO_WEX` | Set by the emulator itself in shared-JIT mode. |
| `BIFROST_TEST_NO_BINDER_ATTACH / BIFROST_FIXTURE_EXIT_FAILURE` | Android framework test controls: skip preloading `libbifrost-binder-attach.so`; make a fixture exit with 23. |
| `BIFROST_TOOL_SEARCH=0 / BIFROST_HOST_BIN` | The setup scripts' host-tool search (`BIFROST_TOOL_SEARCH=0` turns the built-in directory list off) and the directory the tests of those scripts point it at. |

## Host input

With SDL2 enabled, `FrostInput` translates host keyboard, mouse, and game
controller events into Linux-style records. `/dev/input/event0` exposes
`input_event` records; `/dev/input/js0` exposes legacy `js_event` records.
Mouse movement uses relative axes, while absolute axes are reserved for
game controller sticks and triggers. Controller connection queues
`JS_EVENT_INIT` state, and focus loss synthesizes key releases to avoid
stuck keys. In headless builds input queues remain empty; `/dev/input/mice`
ImPS/2 reads are currently unsupported.
