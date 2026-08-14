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
//   - SDL2 audio backend. The Audio class now supports three
//     backends: SDL2 (preferred, cross-platform, low latency via
//     callback), OSS /dev/dsp (legacy), headless (buffer + WAV dump).
//     The SDL2 backend uses a lock-free SPSC ring buffer (64 KiB,
//     power-of-2) — the guest's write() is the producer, SDL2's audio
//     callback is the consumer. No mutex on the hot path.
//   - Input event handling. New FrostInput class
//     (include/frost/input.hpp) captures keyboard/mouse events from
//     the SDL2 window and translates them to Linux input_event
//     records (24 bytes on AArch64). SDL2 scancodes → Linux KEY_*
//     codes; SDL2 mouse buttons → BTN_LEFT/RIGHT/MIDDLE; SDL2 mouse
//     motion → EV_REL REL_X/REL_Y; SDL2 mouse wheel → EV_REL REL_WHEEL.
//     Wired through Yggdrasil DevFS as /dev/input/event0, /dev/input/mice,
//     /dev/input/mouse0, /dev/input/js0 (all return the same stream).
//   - FrostGraphics smarter SDL2 init. Window is now
//     SDL_WINDOW_RESIZABLE; the fb texture auto-scales to the window
//     size via RenderCopy. New set_window_title() and set_window_size()
//     methods. has_window() diagnostic.
//   - Game controller support. FrostInput now opens all
//     connected SDL2 game controllers via SDL_GameControllerOpen and
//     translates their events to both EV_ABS/EV_KEY (for
//     /dev/input/eventX) and JS_EVENT (for /dev/input/js0). Hot-plug
//     (SDL_CONTROLLERDEVICEADDED/REMOVED) is handled. Full button +
//     axis mapping: A/B/X/Y, shoulders, triggers, sticks, D-pad,
//     Start/Back/Guide. New has_game_controller() and
//     game_controller_count() diagnostics.
//   - Dynamic linker bug fixes. Three bugs that broke ALL
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
//
// 1.5.0.alpha (2026-07-07): Major feature release — "the games release".
//   Skipped 1.4.6–1.4.x version numbers per project decision. New in
//   1.5.0.alpha (high-level — see CHANGELOG.md for the full list):
//   - Configuration system: TOML-like .bifrost.toml + --config PATH CLI
//     flag + Emulator::Config struct (jit, fb, audio, thunk, paths,
//     signal policy, performance knobs). Old env vars still work and
//     override the config file for backward compatibility.
//   - 30+ new Linux AArch64 syscalls: xattr family (get/set/list/remove),
//     fallocate, name_to_handle_at / open_by_handle_at, syncfs, renameat2,
//     copy_file_range, statx fields, fanotify_init/event_mark (stubs),
//     inotify_event_unpack, perf_event_open (stub), pidfd_open/send_signal,
//     landlock (stubs), seccomp (stub), madvise enhancements, getcpu,
//     sched_getaffinity/setscheduler robustness, capget/capset, personality,
//     membarrier, get_mempolicy/set_mempolicy (stubs), kcmp, sethostname.
//   - More ARM64 instruction coverage: CRC32CW/PMULL1B crypto (interp +
//     IR), FMINNM/FMAXNM vector variants, FRINTN/Z/P/M/A/I/X by-direction
//     rounding, FRSQRTE/FSQRTE reciprocal roots, vector SQRSHL/UQRSHL,
//     scalar SQABS/SQNEG, RBIT/REV16/REV32 vector, CLS/CLZ vector,
//     CNT (popcount) vector, MOVI/MVNI vector immediate.
//   - Audio thunking: new AudioThunk class forwards guest ALSA/OSS/
//     SDL2/PulseAudio calls to the host's audio stack. Lock-free SPSC
//     ring buffer; supports resampling on the host side via SDL2's
//     AudioCVT; aligns guest/host sample formats.
//   - Display thunking: new DisplayThunk handles VK / Wayland / X11 /
//     GBM / DMA-BUF symbol resolution. Wraps the existing GL/EGL/SDL2
//     GraphicThunk under a unified "graphics thunk" interface.
//   - Performance: PC-relative inline cache for indirect branches
//     (cache last target → 1-cycle re-dispatch on hit), block coalescing
//     (merge adjacent blocks ending in unconditional B), interpreter
//     decode-cache compression (DecodedInst packed to 16 bytes — halves
//     L1 cache pressure), JIT "tiny block" fast path that skips the
//     shared_mutex lock for interp_only blocks under 4 instructions.
//   - Stability: rt_sigreturn now restores pstate.NZCV cleanly even
//     under nested signals (the old code clobbered V-bit on the second
//     signal); futex_wake_single skips the shard mutex when there's
//     exactly one waiter (common case for pthread mutex unlock — saves
//     ~80ns per unlock on 8-vCPU guests); rt_sigprocmask now correctly
//     copies the host's sigset rather than aliasing guest pointers.
//   - Game-readiness: SDL2 game controller rumble support, /dev/input
//     event timestamps, /dev/fb0 pan/blank ioctls, vsync hint, double-
//     buffering fb_node so tearing no longer occurs when the guest
//     renders to /dev/fb0 while the host window redraws.
//   - All 1.4.5-alpha features retained. Test count grows from 92/92
//     to 95/95 (3 new tests for the new instruction/syscall coverage).
//
// 1.5.1-alpha (2026-08-01): SIMD/FP + GL state + dladdr + vDSO batch.
//   New in 1.5.1-alpha (high-level — see CHANGELOG.md for the full list):
//   - SIMD vector FP 2-source ops (FADD/FSUB/FMUL/FDIV/FMAX/FMIN/
//     FMAXNM/FMINNM/FABD/FMULX, .2s/.4s/.2d/.1d) implemented; single
//     precision is native JIT SSE, double runs via the interp handler +
//     JIT fallback. Fixed the FMULX vector encoding and added the
//     missing double-precision interpreter case labels.
//   - LD1/ST1 decoder fix: single-vs-multi structure classification now
//     uses bit[24] (0x0C multiple / 0x0D single); multi register count
//     comes from opcode bits[15:12], not bits[14:13].
//   - GL state tracker (GLStateTracker) mirrors guest GL state so
//     glIsEnabled / glGetIntegerv / glGetFloatv / glGetBooleanv return
//     what the guest set. test_gl_state went from SIGSEGV (57 fails) to
//     ALL PASS.
//   - FABS/FNEG single-precision JIT sign-mask fix; FABD (|a-b|)
//     implemented for single and double (musl fabsf lowers to fabd).
//   - dladdr() enabled through the real glibc dladdr@GLIBC_2.34 symbol.
//   - Guest vDSO: an embedded AArch64 vDSO ELF is loaded and
//     AT_SYSINFO_EHDR set (gettimeofday/clock_gettime/clock_getres/
//     __kernel_rt_sigreturn stubs trap to the syscall handler).
//   - Test count grows from 175 to 187 (fabs_sign, fabd, simd_vec_fp,
//     gl_state, test_dladdr, test_dladdr_glibc).
//
// 1.5.2-alpha (2026-08-13): interp-fallback elimination for FP/SIMD.
//   New in 1.5.2-alpha (high-level — see CHANGELOG.md for the full list):
//   - AdvSIMD modified-immediate MOVI/MVNI/ORR/BIC native (new MODIMM
//     simd_dp table row + SIMD_MOVI/SIMD_ORRIMM IR ops; ~500K interp
//     executions removed; fixed latent cmode=8 MOVI returning 0).
//   - Scalar 64-bit shift-by-immediate (SHL/USHR/SSHR Dd,Dn,#imm) native
//     in the FP_SCALAR translator (the game's `ushr dN,dM,#32` was the
//     top remaining fallback); fixed latent esize=8 SSHR SIGILL (PSRAQ
//     is AVX-512F only, now CALL_INTERP).
//   - Version bump: all version refs normalized to 1.5.2-alpha.
constexpr const char* VERSION  = "1.5.2-alpha";
constexpr const char* CODENAME = "bifrost-emu";
} // namespace arm64emu
