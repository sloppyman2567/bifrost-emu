// bifrost/version.hpp — version constants for bifrost-emu.
//
// Public header: safe to include from any consumer (C++ or C-binding).
#pragma once

namespace arm64emu {

// ── Version ────────────────────────────────────────────────────────────
// 1.4.5-alpha (2026-07-04): First feature release after 1.4.0 stable.
//   - Native SIMD vector shift codegen (SHL/USHR/SSHR) via SSE2
//     psllw/pslld/psllq, psrlw/psrld/psrlq, psraw/psrad. Previously
//     these fell back to CALL_INTERP (~20% overhead on SIMD-heavy
//     workloads). 8-bit element shifts still fall back (no psllb in
//     SSE2). 64-bit SSHR falls back (needs AVX-512 psraq).
//   - Fixed missing SSHR-by-immediate handler in the interpreter
//     (vector SSHR was silently NOP'd; now properly arithmetic-shifts
//     per lane).
//   - New IR ops: SIMD_SHL, SIMD_USHR, SIMD_SSHR.
//   - New test: ctest/jit_neon_advanced.elf (SHL/USHR/SSHR 16/32/64-bit).
//     All 11 checks pass under JIT and interpreter (was 9/11 before
//     the SSHR fix).
//   - VFS subsystem renamed to Yggdrasil (the world-tree from Norse
//     cosmology, matching the bifrost + FrostJIT theme). Class hierarchy
//     renamed: VFS→Yggdrasil, VNode→Node, HostVNode→HostNode, etc.
//     File layout split from src/vfs/ (6 files) into src/yggdrasil/
//     (12 files, one per concern).
//   - New DirNode class: ls /proc, ls /dev, ls /proc/self now work
//     (previously returned nothing — no Node existed for the
//     directories themselves).
//   - Lazy regeneration for /proc/self/maps and /proc/self/status:
//     content is re-rendered on each SEEK_SET 0 so re-reads reflect
//     live state.
//   - /dev/random vs /dev/urandom distinction: distinct entropy pools
//     via getrandom(GRND_RANDOM) vs getrandom(0).
//   - Ioctl dispatch moved from the syscall layer (130-line if-else
//     chain) into the Node hierarchy — FbNode owns framebuffer ioctls,
//     HostNode/StdioNode own terminal ioctls.
//   - O_NONBLOCK on virtual fds now works (via host-fd passthrough).
//   - lseek on memfd-backed virtual files works (was ESPIPE in some
//     paths); triggers lazy regeneration on SEEK_SET 0.
//   - GraphicsBackend renamed to FrostGraphics (matching the
//     FrostJIT/Yggdrasil theme). Header moved to include/frost/graphics.hpp;
//     implementation moved to src/frost_graphics/. GraphicsBackend is
//     kept as a using alias for backward compat.
//   - EXPERIMENTAL: graphic API thunking. New GraphicThunk class
//     (include/frost/thunk.hpp) forwards guest GL/EGL/SDL2 dlsym calls
//     to the host's equivalent libraries. Enabled via
//     BIFROST_THUNK_GRAPHICS=1 env var. Proof-of-concept — only a
//     subset of entry points are thunked.
//     Turn 37: REDESIGNED. The thunk now allocates a guest trampoline
//     page; each registered symbol gets a 16-byte AArch64 trampoline
//     (movz x9,#sym_id; movz x8,#__NR_thunk; svc #0; nop). The thunk
//     is wired into the dynamic linker via a set_thunk_resolver()
//     callback — when find_library() fails for libGL*/libEGL*/libSDL2*/
//     libGLESv2*, a synthetic LoadedObject is registered whose symbols
//     resolve to trampoline addresses. The previous design returned
//     raw host function pointers (broken: guests can't call x86-64
//     pointers as AArch64 code). New syscall __NR_bifrost_thunk=0x1000
//     dispatches to GraphicThunk::dispatch() which reads x0..x7, calls
//     the host function, and writes the result to x0.
//   - SDL2 audio backend (Turn 38). The Audio class now supports three
//     backends: SDL2 (preferred, cross-platform, low latency via
//     callback), OSS /dev/dsp (legacy), headless (buffer + WAV dump).
//     The SDL2 backend uses a lock-free SPSC ring buffer (64 KiB,
//     power-of-2) — the guest's write() is the producer, SDL2's audio
//     callback is the consumer. No mutex on the hot path.
//   - Input event handling (Turn 38). New FrostInput class
//     (include/frost/input.hpp) captures keyboard/mouse events from
//     the SDL2 window and translates them to Linux input_event
//     records (24 bytes on AArch64). SDL2 scancodes → Linux KEY_*
//     codes; SDL2 mouse buttons → BTN_LEFT/RIGHT/MIDDLE; SDL2 mouse
//     motion → EV_REL REL_X/REL_Y; SDL2 mouse wheel → EV_REL REL_WHEEL.
//     Wired through Yggdrasil DevFS as /dev/input/event0, /dev/input/mice,
//     /dev/input/mouse0, /dev/input/js0 (all return the same stream).
//   - FrostGraphics smarter SDL2 init (Turn 38). Window is now
//     SDL_WINDOW_RESIZABLE; the fb texture auto-scales to the window
//     size via RenderCopy. New set_window_title() and set_window_size()
//     methods. has_window() diagnostic.
//   - Game controller support (Turn 39). FrostInput now opens all
//     connected SDL2 game controllers via SDL_GameControllerOpen and
//     translates their events to both EV_ABS/EV_KEY (for
//     /dev/input/eventX) and JS_EVENT (for /dev/input/js0). Hot-plug
//     (SDL_CONTROLLERDEVICEADDED/REMOVED) is handled. Full button +
//     axis mapping: A/B/X/Y, shoulders, triggers, sticks, D-pad,
//     Start/Back/Guide. New has_game_controller() and
//     game_controller_count() diagnostics.
//   - Dynamic linker bug fixes (Turn 39). Three bugs that broke ALL
//     dynamically-linked binaries (glibc AND musl):
//     1. PT_DYNAMIC parsing read p_offset (file offset) into dyn_vaddr
//        instead of p_vaddr (virtual address). Fixed.
//     2. DT_JMPREL (PLT relocations) was completely ignored — only
//        DT_RELA was processed. JUMP_SLOT relocations for libc
//        functions (printf, malloc, __libc_start_main) were never
//        applied, so GOT entries stayed 0 → PLT stubs jumped to 0 →
//        decode error at pc=0x0. Fixed: DT_JMPREL now processed
//        separately with eager binding.
//     3. d_val for DT_RELA/DT_JMPREL in shared libraries is a vaddr
//        RELATIVE to the library's load base. The old code used d_val
//        directly, which worked for the main binary (base=0) but read
//        from wrong addresses for shared libs (e.g., libc's DT_JMPREL
//        at 0x2a880 was read from low memory instead of
//        0x500002a880). Fixed: obj.base_addr added to d_val.
//     After these fixes, dynamically-linked binaries progress much
//     further (symbols resolve, PLT works). Glibc binaries still hang
//     in libc init (needs vDSO/signal frame work — future enhancement).
//     Musl dynamic binaries also progress further (was decode error
//     at 0x0, now reaches libc init).
//   - FrostJIT split: frostjit.cpp was 4520 LOC — too big to navigate.
//     Split into 7 files: jit_interp.cpp (trampoline), jit_helpers.cpp
//     (emit helpers), jit_codegen_fp.cpp (FP/SIMD codegen),
//     jit_flags.cpp (flag materialization), jit_translate.cpp
//     (translate_block), jit_dispatch.cpp (run_block), and frostjit.cpp
//     (integer codegen, now 1798 LOC). No behavior change.
//   - Version consistency sweep: all version refs across the tree now
//     say 1.4.5-alpha (previously many said 1.4.0; test_capi was
//     checking for "1.4.0" and failing).
//   - All 1.4.0 features retained: 72/72 tests pass under JIT+interp+FWD.
constexpr const char* VERSION  = "1.4.5-alpha";
constexpr const char* CODENAME = "bifrost-emu";

} // namespace arm64emu
