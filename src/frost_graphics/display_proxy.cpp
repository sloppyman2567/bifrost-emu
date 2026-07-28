// frost_graphics/display_proxy.cpp — DisplayProxy implementation.
//
// v1.5.0.alpha: NEW. Owns a single host SDL2 window and translates
// minimal X11 / Wayland calls from the guest into SDL2 render calls.
//
// Guest handles (Display*, Window, wl_display*, wl_surface*) are guest-
// memory addresses. The thunk trampoline path translates them to host
// pointers via guest_to_host_ptr(); we store a small handle header at
// each guest address so we can recover our proxy state on entry.
#include "frost/display_proxy.hpp"
#include "core/memory.h"
#include <SDL2/SDL.h>
#include <cstdio>
#include <cstring>
namespace arm64emu {
struct HandleHdr {
    uint32_t type;
    uint32_t index;
};
DisplayProxy::DisplayProxy() = default;
DisplayProxy::~DisplayProxy() { shutdown(); }
void DisplayProxy::shutdown() {
#if defined(BIFROST_USE_SDL2)
    SDL_Window* w = (SDL_Window*)window_;
    SDL_Renderer* r = (SDL_Renderer*)renderer_;
    SDL_Texture* t = (SDL_Texture*)texture_;
    if (t) SDL_DestroyTexture(t);
    if (r) SDL_DestroyRenderer(r);
    if (w) SDL_DestroyWindow(w);
#endif
    handles_.clear();
    next_guest_addr_ = 0;
    window_ = renderer_ = texture_ = nullptr;
}
bool DisplayProxy::init(uint32_t width, uint32_t height, Memory* mem) {
    mem_ = mem;
    width_ = width ? width : 640;
    height_ = height ? height : 480;
    return init_sdl2_();
}
bool DisplayProxy::init_sdl2_() {
#if defined(BIFROST_USE_SDL2)
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[display-proxy] SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    window_ = SDL_CreateWindow("bifrost-emu proxy",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        (int)width_, (int)height_,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!window_) {
        fprintf(stderr, "[display-proxy] SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return false;
    }
    renderer_ = SDL_CreateRenderer((SDL_Window*)window_, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer_) {
        renderer_ = SDL_CreateRenderer((SDL_Window*)window_, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!renderer_) {
        fprintf(stderr, "[display-proxy] SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow((SDL_Window*)window_); window_ = nullptr;
        SDL_Quit();
        return false;
    }
    texture_ = SDL_CreateTexture((SDL_Renderer*)renderer_, SDL_PIXELFORMAT_RGBA8888,
        SDL_TEXTUREACCESS_STREAMING, (int)width_, (int)height_);
    if (!texture_) {
        fprintf(stderr, "[display-proxy] SDL_CreateTexture failed: %s\n", SDL_GetError());
    }
    return true;
#else
    (void)width_; (void)height_;
    fprintf(stderr, "[display-proxy] SDL2 not available\n");
    return false;
#endif
}
void DisplayProxy::present() {
#if defined(BIFROST_USE_SDL2)
    if (!window_ || !renderer_) return;
    SDL_SetRenderDrawColor((SDL_Renderer*)renderer_, 0, 0, 0, 255);
    SDL_RenderClear((SDL_Renderer*)renderer_);
    if (texture_) {
        SDL_RenderCopy((SDL_Renderer*)renderer_, (SDL_Texture*)texture_, nullptr, nullptr);
    }
    SDL_RenderPresent((SDL_Renderer*)renderer_);
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) {
        }
    }
#else
    (void)window_; (void)renderer_; (void)texture_;
#endif
}
uint64_t DisplayProxy::alloc_handle(uint32_t type) {
    if (!mem_ || handles_.size() >= MAX_HANDLES) return 0;
    uint64_t guest_addr = HANDLE_BASE + next_guest_addr_;
    next_guest_addr_ += HANDLE_STEP;
    HandleHdr hdr;
    hdr.type = type;
    hdr.index = (uint32_t)handles_.size();
    mem_->write(guest_addr, &hdr, sizeof(hdr));
    handles_.push_back({guest_addr, type});
    return guest_addr;
}
void DisplayProxy::free_handle(uint64_t guest_addr) {
    if (!mem_ || guest_addr == 0) return;
    for (size_t i = 0; i < handles_.size(); ++i) {
        if (handles_[i].guest_addr == guest_addr) {
            handles_.erase(handles_.begin() + (ptrdiff_t)i);
            break;
        }
    }
}
void* DisplayProxy::handle_to_host(uint64_t guest_addr) const {
    if (!mem_ || guest_addr == 0) return nullptr;
    uint8_t* hp = mem_->guest_to_host_ptr(guest_addr);
    if (hp) return hp;
    return nullptr;
}
// ── X11 proxy ────────────────────────────────────────────────────────
uint64_t DisplayProxy::XOpenDisplay(const char* name) {
    (void)name;
    if (!ready()) return 0;
    return alloc_handle(1);
}
int DisplayProxy::XCloseDisplay(uint64_t display_guest) {
    if (display_guest == 0) return 0;
    free_handle(display_guest);
    return 1;
}
uint64_t DisplayProxy::XCreateWindow(uint64_t display_guest, uint64_t parent, int x, int y, unsigned w, unsigned h, unsigned bw, int depth, unsigned long visual, uint64_t visual_ptr, unsigned long valuemask, const char* attributes) {
    (void)display_guest; (void)parent; (void)x; (void)y;
    (void)w; (void)h; (void)bw; (void)depth; (void)visual;
    (void)visual_ptr; (void)valuemask; (void)attributes;
    if (!ready()) return 0;
    return alloc_handle(2);
}
uint64_t DisplayProxy::XCreateSimpleWindow(uint64_t display_guest, uint64_t parent, int x, int y, int w, int h, int bw, unsigned long border, unsigned long background) {
    (void)display_guest; (void)parent; (void)x; (void)y;
    (void)w; (void)h; (void)bw; (void)border; (void)background;
    if (!ready()) return 0;
    return alloc_handle(2);
}
int DisplayProxy::XMapWindow(uint64_t display_guest, uint64_t window_guest) {
    (void)display_guest; (void)window_guest;
    if (!ready()) return 0;
    return 1;
}
int DisplayProxy::XUnmapWindow(uint64_t display_guest, uint64_t window_guest) {
    (void)display_guest; (void)window_guest;
    return 1;
}
void DisplayProxy::XFillRectangle(uint64_t display_guest, uint64_t window_guest, unsigned long gc, int x, int y, unsigned w, unsigned h) {
    (void)display_guest; (void)window_guest; (void)gc;
#if defined(BIFROST_USE_SDL2)
    if (!renderer_) return;
    int iw = (int)w, ih = (int)h;
    if (x < 0) { iw += x; x = 0; }
    if (y < 0) { ih += y; y = 0; }
    if (x + iw > (int)width_) iw = (int)width_ - x;
    if (y + ih > (int)height_) ih = (int)height_ - y;
    if (iw <= 0 || ih <= 0) return;
    SDL_SetRenderDrawColor((SDL_Renderer*)renderer_, 255, 0, 0, 255);
    SDL_Rect r = {x, y, iw, ih};
    SDL_RenderFillRect((SDL_Renderer*)renderer_, &r);
#endif
}
int DisplayProxy::XFlush(uint64_t display_guest) {
    (void)display_guest;
    present();
    return 0;
}
// ── Wayland proxy (minimal) ─────────────────────────────────────────
uint64_t DisplayProxy::wl_display_connect(const char* name) {
    (void)name;
    if (!ready()) return 0;
    return alloc_handle(3);
}
void DisplayProxy::wl_display_disconnect(uint64_t display_guest) {
    free_handle(display_guest);
}
uint64_t DisplayProxy::wl_surface_create(uint64_t display_guest, const char* interface, uint32_t version) {
    (void)display_guest; (void)interface; (void)version;
    if (!ready()) return 0;
    return alloc_handle(4);
}
void DisplayProxy::wl_surface_commit(uint64_t surface_guest) {
    (void)surface_guest;
    present();
}
void DisplayProxy::wl_surface_destroy(uint64_t surface_guest) {
    free_handle(surface_guest);
}
// ── Additional X11 proxy methods (v1.5.0.alpha) ────────────────────────
// These provide a SDL2-based software fallback for common X11 functions
// used by desktop applications. They are called from DisplayThunk::dispatch
// when the THUNK_PROXY flag is set and the host library is unavailable.
int DisplayProxy::XDestroyWindow(uint64_t display_guest, uint64_t window_guest) {
    (void)display_guest;
    free_handle(window_guest);
    return 1;
}
int DisplayProxy::XSync(uint64_t display_guest, int discard) {
    (void)display_guest; (void)discard;
    present();
    return 0;
}
void DisplayProxy::XDrawRectangle(uint64_t display_guest, uint64_t window_guest, unsigned long gc, int x, int y, unsigned w, unsigned h) {
    (void)display_guest; (void)window_guest; (void)gc;
#if defined(BIFROST_USE_SDL2)
    if (!renderer_) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + (int)w > (int)width_) w = (int)width_ - x;
    if (y + (int)h > (int)height_) h = (int)height_ - y;
    if (w <= 0 || h <= 0) return;
    SDL_SetRenderDrawColor((SDL_Renderer*)renderer_, 255, 255, 255, 255);
    SDL_Rect r = {x, y, (int)w, (int)h};
    SDL_RenderDrawRect((SDL_Renderer*)renderer_, &r);
#endif
}
void DisplayProxy::XDrawLine(uint64_t display_guest, uint64_t window_guest, unsigned long gc, int x1, int y1, int x2, int y2) {
    (void)display_guest; (void)window_guest; (void)gc;
#if defined(BIFROST_USE_SDL2)
    if (!renderer_) return;
    SDL_SetRenderDrawColor((SDL_Renderer*)renderer_, 255, 255, 255, 255);
    SDL_RenderDrawLine((SDL_Renderer*)renderer_, x1, y1, x2, y2);
#endif
}
void DisplayProxy::XDrawPoint(uint64_t display_guest, uint64_t window_guest, unsigned long gc, int x, int y) {
    (void)display_guest; (void)window_guest; (void)gc;
#if defined(BIFROST_USE_SDL2)
    if (!renderer_) return;
    SDL_SetRenderDrawColor((SDL_Renderer*)renderer_, 255, 255, 255, 255);
    SDL_RenderDrawPoint((SDL_Renderer*)renderer_, x, y);
#endif
}
unsigned long DisplayProxy::XSetForeground(uint64_t display_guest, unsigned long gc, unsigned long color) {
    (void)display_guest; (void)gc; (void)color;
    return 1;
}
unsigned long DisplayProxy::XSetBackground(uint64_t display_guest, unsigned long gc, unsigned long color) {
    (void)display_guest; (void)gc; (void)color;
    return 1;
}
unsigned long DisplayProxy::XCreateGC(uint64_t display_guest, unsigned long drawable, unsigned long valuemask, const char* values, int screen, uint64_t visual) {
    (void)display_guest; (void)drawable; (void)valuemask; (void)values; (void)screen; (void)visual;
    if (!ready()) return 0;
    return alloc_handle(5);
}
int DisplayProxy::XFreeGC(uint64_t display_guest, unsigned long gc) {
    (void)display_guest;
    free_handle(gc);
    return 1;
}
int DisplayProxy::XStoreName(uint64_t display_guest, uint64_t window_guest, const char* name) {
    (void)display_guest; (void)window_guest;
#if defined(BIFROST_USE_SDL2)
    if (window_ && name) {
        SDL_SetWindowTitle((SDL_Window*)window_, name);
    }
#endif
    return 1;
}
int DisplayProxy::XGetWindowAttributes(uint64_t display_guest, uint64_t window_guest, void* attrs) {
    (void)display_guest; (void)window_guest;
    if (!attrs) return 0;
    // Fill a minimal XWindowAttributes struct (width, height, etc.)
    // The struct layout matches the X11 XWindowAttributes:
    // struct { int x, y; int width, height; int border_width; int depth; ... }
    struct XWindowAttributes {
        int x, y;
        int width, height;
        int border_width;
        int depth;
        void* visual;
        void* display;
        int screen;
        int class_;
        int event_mask;
        int own_pressing;
        int min_pixel;
        int max_pixel;
    };
    auto* a = static_cast<XWindowAttributes*>(attrs);
    a->x = 0; a->y = 0;
    a->width = (int)width_;
    a->height = (int)height_;
    a->border_width = 0;
    a->depth = 32;
    a->visual = nullptr;
    a->display = nullptr;
    a->screen = 0;
    a->class_ = 0;
    a->event_mask = 0;
    a->own_pressing = 0;
    a->min_pixel = 0;
    a->max_pixel = 0;
    return 1;
}
int DisplayProxy::XSelectInput(uint64_t display_guest, uint64_t window_guest, long event_mask) {
    (void)display_guest; (void)window_guest; (void)event_mask;
    return 1;
}
unsigned long DisplayProxy::XInternAtom(uint64_t display_guest, const char* name, int only_if_exists) {
    (void)display_guest; (void)only_if_exists;
    // Return a fake atom ID based on the name hash.
    if (!name) return 0;
    unsigned long hash = 0;
    for (const char* p = name; *p; p++) {
        hash = hash * 31 + (unsigned char)(*p);
    }
    return hash ? hash : 1;
}
int DisplayProxy::XSetWMProtocols(uint64_t display_guest, uint64_t window_guest, const char* protocols, int count) {
    (void)display_guest; (void)window_guest; (void)protocols; (void)count;
    return 1;
}
unsigned long DisplayProxy::XGetAtomName(uint64_t display_guest, unsigned long atom) {
    (void)display_guest; (void)atom;
    // Return a static string for the atom name.
    static char atom_name[32];
    snprintf(atom_name, sizeof(atom_name), "ATOM_%lu", atom);
    return (unsigned long)(uintptr_t)atom_name;
}
int DisplayProxy::XPending(uint64_t display_guest) {
    (void)display_guest;
    return 0;
}
int DisplayProxy::XNextEvent(uint64_t display_guest, void* event) {
    (void)display_guest;
    if (!event) return 0;
    // Fill a minimal XEvent struct (type = 0 = no event).
    memset(event, 0, 192);  // XEvent is 192 bytes
    return 0;
}
int DisplayProxy::XCheckMaskEvent(uint64_t display_guest, long event_mask, void* event) {
    (void)display_guest; (void)event_mask;
    if (!event) return 0;
    memset(event, 0, 192);
    return 0;
}
int DisplayProxy::XEventsQueued(uint64_t display_guest, int mode) {
    (void)display_guest; (void)mode;
    return 0;
}
int DisplayProxy::XDisplayWidth(uint64_t display_guest, int screen) {
    (void)display_guest; (void)screen;
    return (int)width_;
}
int DisplayProxy::XDisplayHeight(uint64_t display_guest, int screen) {
    (void)display_guest; (void)screen;
    return (int)height_;
}
int DisplayProxy::XDisplayWidthMM(uint64_t display_guest, int screen) {
    (void)display_guest; (void)screen;
    return (int)(width_ * 25.4 / 96);  // ~96 DPI
}
int DisplayProxy::XDisplayHeightMM(uint64_t display_guest, int screen) {
    (void)display_guest; (void)screen;
    return (int)(height_ * 25.4 / 96);
}
uint64_t DisplayProxy::DefaultRootWindow(uint64_t display_guest) {
    (void)display_guest;
    return 0x50000001ULL;  // fake root window ID
}
unsigned long DisplayProxy::BlackPixel(uint64_t display_guest, int screen) {
    (void)display_guest; (void)screen;
    return 0;
}
unsigned long DisplayProxy::WhitePixel(uint64_t display_guest, int screen) {
    (void)display_guest; (void)screen;
    return 0xFFFFFF;
}
int DisplayProxy::XSetWindowBackground(uint64_t display_guest, uint64_t window_guest, unsigned long color) {
    (void)display_guest; (void)window_guest; (void)color;
    return 1;
}
unsigned long DisplayProxy::XCreateColormap(uint64_t display_guest, uint64_t window_guest, uint64_t visual, int alloc) {
    (void)display_guest; (void)window_guest; (void)visual; (void)alloc;
    if (!ready()) return 0;
    return alloc_handle(6);
}
int DisplayProxy::XFreeColormap(uint64_t display_guest, unsigned long colormap) {
    (void)display_guest;
    free_handle(colormap);
    return 1;
}
int DisplayProxy::XAllocColor(uint64_t display_guest, unsigned long colormap, void* color) {
    (void)display_guest; (void)colormap;
    if (!color) return 0;
    // Fill a minimal XColor struct:
    // struct { unsigned long pixel; unsigned short red, green, blue, flags, pad; }
    struct XColor {
        unsigned long pixel;
        unsigned short red, green, blue;
        unsigned char flags;
        unsigned char pad;
    };
    auto* c = static_cast<XColor*>(color);
    c->pixel = 0;
    c->red = 0; c->green = 0; c->blue = 0;
    c->flags = 0;
    c->pad = 0;
    return 1;
}
unsigned long DisplayProxy::XSetLineAttributes(uint64_t display_guest, unsigned long gc, unsigned line_width, unsigned line_style, int cap_style, int join_style) {
    (void)display_guest; (void)gc; (void)line_width; (void)line_style; (void)cap_style; (void)join_style;
    return 1;
}
void DisplayProxy::XDrawString(uint64_t display_guest, uint64_t window_guest, unsigned long gc, int x, int y, const char* str, int length) {
    (void)display_guest; (void)window_guest; (void)gc;
#if defined(BIFROST_USE_SDL2)
    if (!renderer_ || !str || length <= 0) return;
    SDL_SetRenderDrawColor((SDL_Renderer*)renderer_, 255, 255, 255, 255);
    SDL_RenderDrawLine((SDL_Renderer*)renderer_, x, y, x + length * 6, y);
#endif
}
void DisplayProxy::XDrawImageString(uint64_t display_guest, uint64_t window_guest, unsigned long gc, int x, int y, const char* str, int length) {
    (void)display_guest; (void)window_guest; (void)gc;
#if defined(BIFROST_USE_SDL2)
    if (!renderer_ || !str || length <= 0) return;
    SDL_SetRenderDrawColor((SDL_Renderer*)renderer_, 255, 255, 255, 255);
    SDL_RenderDrawLine((SDL_Renderer*)renderer_, x, y, x + length * 6, y);
#endif
}
int DisplayProxy::XQueryPointer(uint64_t display_guest, uint64_t window_guest, void* root, void* child, void* x, void* y, void* win_x, void* win_y, void* mask) {
    (void)display_guest; (void)window_guest;
    int mx = 0, my = 0;
#if defined(BIFROST_USE_SDL2)
    if (window_) {
        SDL_GetMouseState(&mx, &my);
    }
#endif
    if (root) *(int*)root = 0;
    if (child) *(int*)child = 0;
    if (x) *(int*)x = mx;
    if (y) *(int*)y = my;
    if (win_x) *(int*)win_x = mx;
    if (win_y) *(int*)win_y = my;
    if (mask) *(unsigned int*)mask = 0;
    return 1;
}
int DisplayProxy::XWarpPointer(uint64_t display_guest, uint64_t src_window, uint64_t dest_window, int src_x, int src_y, int src_width, int src_height, int dest_x, int dest_y) {
    (void)display_guest; (void)src_window; (void)dest_window;
    (void)src_x; (void)src_y; (void)src_width; (void)src_height;
#if defined(BIFROST_USE_SDL2)
    if (window_) {
        SDL_WarpMouseInWindow((SDL_Window*)window_, dest_x, dest_y);
    }
#endif
    return 0;
}
int DisplayProxy::XBell(uint64_t display_guest, int percent) {
    (void)display_guest; (void)percent;
    return 1;
}
int DisplayProxy::XScreenCount(uint64_t display_guest) {
    (void)display_guest;
    return 1;
}
int DisplayProxy::XSetInputFocus(uint64_t display_guest, uint64_t focus, int revert_to, uint64_t time) {
    (void)display_guest; (void)focus; (void)revert_to; (void)time;
    return 0;
}
int DisplayProxy::XGetInputFocus(uint64_t display_guest, void* focus, void* revert_to) {
    (void)display_guest;
    if (focus) *(uint64_t*)focus = 0;
    if (revert_to) *(int*)revert_to = 0;
    return 0;
}
int DisplayProxy::XChangeProperty(uint64_t display_guest, uint64_t window_guest, unsigned long prop, unsigned long type, int format, int mode, const char* data, long nelements) {
    (void)display_guest; (void)window_guest; (void)prop; (void)type; (void)format; (void)mode; (void)data; (void)nelements;
    return 0;
}
int DisplayProxy::XDeleteProperty(uint64_t display_guest, uint64_t window_guest, unsigned long prop) {
    (void)display_guest; (void)window_guest; (void)prop;
    return 0;
}
int DisplayProxy::XCopyArea(uint64_t display_guest, uint64_t src_drawable, uint64_t dest_drawable, unsigned long gc, int src_x, int src_y, int dest_x, int dest_y, int width, int height) {
    (void)display_guest; (void)src_drawable; (void)dest_drawable; (void)gc;
    (void)src_x; (void)src_y; (void)dest_x; (void)dest_y;
    (void)width; (void)height;
    return 1;
}
unsigned long DisplayProxy::XCreatePixmap(uint64_t display_guest, unsigned long drawable, int width, int height, int depth) {
    (void)display_guest; (void)drawable; (void)width; (void)height; (void)depth;
    if (!ready()) return 0;
    return alloc_handle(7);
}
int DisplayProxy::XFreePixmap(uint64_t display_guest, unsigned long pixmap) {
    (void)display_guest;
    free_handle(pixmap);
    return 1;
}
int DisplayProxy::XSetWindowBackgroundPixmap(uint64_t display_guest, uint64_t window_guest, unsigned long pixmap) {
    (void)display_guest; (void)window_guest; (void)pixmap;
    return 1;
}
int DisplayProxy::XSetClipMask(uint64_t display_guest, unsigned long gc, unsigned long bitmap) {
    (void)display_guest; (void)gc; (void)bitmap;
    return 1;
}
int DisplayProxy::XSetClipOrigin(uint64_t display_guest, unsigned long gc, int x, int y) {
    (void)display_guest; (void)gc; (void)x; (void)y;
    return 1;
}
int DisplayProxy::XCopyGC(uint64_t display_guest, unsigned long src_gc, unsigned long valuemask, unsigned long dest_gc) {
    (void)display_guest; (void)src_gc; (void)valuemask; (void)dest_gc;
    return 1;
}
int DisplayProxy::XChangeGC(uint64_t display_guest, unsigned long gc, unsigned long valuemask, const char* values) {
    (void)display_guest; (void)gc; (void)valuemask; (void)values;
    return 1;
}
int DisplayProxy::XSetFunction(uint64_t display_guest, unsigned long gc, int function) {
    (void)display_guest; (void)gc; (void)function;
    return 1;
}
int DisplayProxy::XSetDashes(uint64_t display_guest, unsigned long gc, int dash_offset, const char* dashes) {
    (void)display_guest; (void)gc; (void)dash_offset; (void)dashes;
    return 1;
}
int DisplayProxy::XFreeColors(uint64_t display_guest, unsigned long colormap, const char* pixels, int num_pixels, unsigned long planes) {
    (void)display_guest; (void)colormap; (void)pixels; (void)num_pixels; (void)planes;
    return 0;
}
uint64_t DisplayProxy::wl_egl_window_create(uint64_t surface_guest, int width, int height) {
    (void)surface_guest; (void)width; (void)height;
    if (!ready()) return 0;
    return alloc_handle(8);
}
int DisplayProxy::wl_egl_window_destroy(uint64_t window_guest) {
    free_handle(window_guest);
    return 0;
}
int DisplayProxy::wl_egl_window_get_attached_size(uint64_t window_guest, void* width, void* height) {
    (void)window_guest;
    if (width) *(int*)width = (int)width_;
    if (height) *(int*)height = (int)height_;
    return 0;
}
int DisplayProxy::wl_egl_window_resize(uint64_t window_guest, int x, int y, int width, int height) {
    (void)window_guest; (void)x; (void)y;
#if defined(BIFROST_USE_SDL2)
    if (window_) {
        SDL_SetWindowSize((SDL_Window*)window_, width, height);
    }
#endif
    return 0;
}
} // namespace arm64emu
