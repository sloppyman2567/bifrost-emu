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

Modern GL is covered: `glMapBuffer`/`glMapBufferRange` and
persistent-coherent mappings use a guest-window bounce (PCWFC
writeback before buffer-consuming calls), and GL3.3+/4.x rows cover
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
BIFROST_NO_THUNK_GRAPHICS=1      # Disable GL/EGL/SDL2 thunking
BIFROST_NO_THUNK_AUDIO=1         # Disable ALSA/PulseAudio thunking
BIFROST_NO_THUNK_DISPLAY=1       # Disable Vulkan/Wayland thunking
```

Graphical programs log their first successful frame presentation to stderr
(`--no-render-log` suppresses it). Presentation statistics in the window
title, a separate window or an overlay are described in
[window stats](window-stats.md). Android windows render at the host
display's native pixel density; see [window plugins](Android/ANDROID_WINDOW_PLUGINS.md).

## Host input

With SDL2 enabled, `FrostInput` translates host keyboard, mouse, and game
controller events into Linux-style records. `/dev/input/event0` exposes
`input_event` records; `/dev/input/js0` exposes legacy `js_event` records.
Mouse movement uses relative axes, while absolute axes are reserved for
game controller sticks and triggers. Controller connection queues
`JS_EVENT_INIT` state, and focus loss synthesizes key releases to avoid
stuck keys. In headless builds input queues remain empty; `/dev/input/mice`
ImPS/2 reads are currently unsupported.
