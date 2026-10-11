#pragma once
#include "System/Graphics/Display/WindowStats.hpp"
#include "System/Graphics/Display/DisplayProxy.hpp"
#include "System/Android/Surface/Surface.hpp"
#include "System/Memory/Memory.hpp"
#include "Generated/WaylandTable.hpp"
#if defined(BIFROST_USE_SDL2)
#include <SDL2/SDL.h>
#endif
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
// Public libwayland wire ABI. Only these descriptors are used with dlsym;
// no host Wayland SDK or link-time library is required by the proxy.
struct wl_message {
    const char* name;
    const char* signature;
    const wl_interface** types;
};
struct wl_interface {
    const char* name;
    int version;
    int method_count;
    const wl_message* methods;
    int event_count;
    const wl_message* events;
};
static_assert(sizeof(wl_message) == 24 && sizeof(wl_interface) == 40,
              "Wayland descriptor ABI requires a 64-bit host");

namespace arm64emu {
struct HandleHdr {
    uint32_t type;
    uint32_t index;
};
// Handle type tags.
static constexpr uint32_t H_DISPLAY     = 1;
static constexpr uint32_t H_WINDOW      = 2;
static constexpr uint32_t H_GC          = 5;
static constexpr uint32_t H_COLORMAP    = 6;
static constexpr uint32_t H_PIXMAP      = 7;
static constexpr uint32_t H_EGL_WINDOW  = 8;
static constexpr uint32_t H_SHMSEG      = 9;
static constexpr uint32_t H_GLX_CONTEXT = 10;
static constexpr uint32_t H_GLX_WINDOW  = 11;
static constexpr uint32_t H_GLX_PBUFFER = 12;
static constexpr uint32_t H_RANDR_CRTC  = 13;
static constexpr uint32_t H_RANDR_OUTPUT= 14;
static constexpr uint32_t H_RANDR_MODE  = 15;
static constexpr uint32_t H_XKB         = 16;
static constexpr uint32_t H_SHM_IMAGE   = 17;
} // namespace arm64emu
