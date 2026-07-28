// frost_graphics/display_proxy.hpp — host-side display proxy for guest
// X11/Wayland apps.
//
// v1.5.0.alpha: NEW. Displays owns ONE host SDL2 window and exposes
// guest-callable proxy entry points for a minimal X11 and Wayland
// surface-creation + draw loop.
//
// Handles (Display*, Window, wl_display*, wl_surface*) are guest
// addresses backed by the emulator's Memory. The thunk trampoline path
// translates them automatically via guest_to_host_ptr().
#pragma once
#include <cstdint>
#include <vector>
namespace arm64emu {
class Memory;
class DisplayProxy {
public:
    DisplayProxy();
    ~DisplayProxy();
    DisplayProxy(const DisplayProxy&) = delete;
    DisplayProxy& operator=(const DisplayProxy&) = delete;
    bool init(uint32_t width, uint32_t height, Memory* mem);
    void shutdown();
    void present();
    bool ready() const { return window_ != nullptr; }
    void* host_window() const { return window_; }
    Memory* memory() const { return mem_; }
    void set_memory(Memory* mem) { mem_ = mem; }
    uint64_t alloc_handle(uint32_t type);
    void free_handle(uint64_t guest_addr);
    void* handle_to_host(uint64_t guest_addr) const;
    uint64_t XOpenDisplay(const char* name);
    int XCloseDisplay(uint64_t display_guest);
    uint64_t XCreateSimpleWindow(uint64_t display_guest, uint64_t parent, int x, int y, int w, int h, int bw, unsigned long border, unsigned long background);
    uint64_t XCreateWindow(uint64_t display_guest, uint64_t parent, int x, int y, unsigned w, unsigned h, unsigned bw, int depth, unsigned long visual, uint64_t visual_ptr, unsigned long valuemask, const char* attributes);
    int XDestroyWindow(uint64_t display_guest, uint64_t window_guest);
    int XMapWindow(uint64_t display_guest, uint64_t window_guest);
    int XUnmapWindow(uint64_t display_guest, uint64_t window_guest);
    void XFillRectangle(uint64_t display_guest, uint64_t window_guest, unsigned long gc, int x, int y, unsigned w, unsigned h);
    void XDrawRectangle(uint64_t display_guest, uint64_t window_guest, unsigned long gc, int x, int y, unsigned w, unsigned h);
    void XDrawLine(uint64_t display_guest, uint64_t window_guest, unsigned long gc, int x1, int y1, int x2, int y2);
    void XDrawPoint(uint64_t display_guest, uint64_t window_guest, unsigned long gc, int x, int y);
    int XFlush(uint64_t display_guest);
    int XSync(uint64_t display_guest, int discard);
    unsigned long XSetForeground(uint64_t display_guest, unsigned long gc, unsigned long color);
    unsigned long XSetBackground(uint64_t display_guest, unsigned long gc, unsigned long color);
    unsigned long XCreateGC(uint64_t display_guest, unsigned long drawable, unsigned long valuemask, const char* values, int screen, uint64_t visual);
    int XFreeGC(uint64_t display_guest, unsigned long gc);
    int XStoreName(uint64_t display_guest, uint64_t window_guest, const char* name);
    int XGetWindowAttributes(uint64_t display_guest, uint64_t window_guest, void* attrs);
    int XSelectInput(uint64_t display_guest, uint64_t window_guest, long event_mask);
    unsigned long XInternAtom(uint64_t display_guest, const char* name, int only_if_exists);
    int XSetWMProtocols(uint64_t display_guest, uint64_t window_guest, const char* protocols, int count);
    unsigned long XGetAtomName(uint64_t display_guest, unsigned long atom);
    int XPending(uint64_t display_guest);
    int XNextEvent(uint64_t display_guest, void* event);
    int XCheckMaskEvent(uint64_t display_guest, long event_mask, void* event);
    int XEventsQueued(uint64_t display_guest, int mode);
    int XDisplayWidth(uint64_t display_guest, int screen);
    int XDisplayHeight(uint64_t display_guest, int screen);
    int XDisplayWidthMM(uint64_t display_guest, int screen);
    int XDisplayHeightMM(uint64_t display_guest, int screen);
    uint64_t DefaultRootWindow(uint64_t display_guest);
    unsigned long BlackPixel(uint64_t display_guest, int screen);
    unsigned long WhitePixel(uint64_t display_guest, int screen);
    int XSetWindowBackground(uint64_t display_guest, uint64_t window_guest, unsigned long color);
    unsigned long XCreateColormap(uint64_t display_guest, uint64_t window_guest, uint64_t visual, int alloc);
    int XFreeColormap(uint64_t display_guest, unsigned long colormap);
    int XAllocColor(uint64_t display_guest, unsigned long colormap, void* color);
    unsigned long XSetLineAttributes(uint64_t display_guest, unsigned long gc, unsigned line_width, unsigned line_style, int cap_style, int join_style);
    void XDrawString(uint64_t display_guest, uint64_t window_guest, unsigned long gc, int x, int y, const char* str, int length);
    void XDrawImageString(uint64_t display_guest, uint64_t window_guest, unsigned long gc, int x, int y, const char* str, int length);
    int XQueryPointer(uint64_t display_guest, uint64_t window_guest, void* root, void* child, void* x, void* y, void* win_x, void* win_y, void* mask);
    int XWarpPointer(uint64_t display_guest, uint64_t src_window, uint64_t dest_window, int src_x, int src_y, int src_width, int src_height, int dest_x, int dest_y);
    int XBell(uint64_t display_guest, int percent);
    int XScreenCount(uint64_t display_guest);
    int XSetInputFocus(uint64_t display_guest, uint64_t focus, int revert_to, uint64_t time);
    int XGetInputFocus(uint64_t display_guest, void* focus, void* revert_to);
    int XChangeProperty(uint64_t display_guest, uint64_t window_guest, unsigned long prop, unsigned long type, int format, int mode, const char* data, long nelements);
    int XDeleteProperty(uint64_t display_guest, uint64_t window_guest, unsigned long prop);
    int XCopyArea(uint64_t display_guest, uint64_t src_drawable, uint64_t dest_drawable, unsigned long gc, int src_x, int src_y, int dest_x, int dest_y, int width, int height);
    unsigned long XCreatePixmap(uint64_t display_guest, unsigned long drawable, int width, int height, int depth);
    int XFreePixmap(uint64_t display_guest, unsigned long pixmap);
    int XSetWindowBackgroundPixmap(uint64_t display_guest, uint64_t window_guest, unsigned long pixmap);
    int XSetClipMask(uint64_t display_guest, unsigned long gc, unsigned long bitmap);
    int XSetClipOrigin(uint64_t display_guest, unsigned long gc, int x, int y);
    int XCopyGC(uint64_t display_guest, unsigned long src_gc, unsigned long valuemask, unsigned long dest_gc);
    int XChangeGC(uint64_t display_guest, unsigned long gc, unsigned long valuemask, const char* values);
    int XSetFunction(uint64_t display_guest, unsigned long gc, int function);
    int XSetDashes(uint64_t display_guest, unsigned long gc, int dash_offset, const char* dashes);
    int XFreeColors(uint64_t display_guest, unsigned long colormap, const char* pixels, int num_pixels, unsigned long planes);
    uint64_t wl_egl_window_create(uint64_t surface_guest, int width, int height);
    int wl_egl_window_destroy(uint64_t window_guest);
    int wl_egl_window_get_attached_size(uint64_t window_guest, void* width, void* height);
    int wl_egl_window_resize(uint64_t window_guest, int x, int y, int width, int height);
    uint64_t wl_display_connect(const char* name);
    void wl_display_disconnect(uint64_t display_guest);
    uint64_t wl_surface_create(uint64_t display_guest, const char* interface, uint32_t version);
    void wl_surface_commit(uint64_t surface_guest);
    void wl_surface_destroy(uint64_t surface_guest);
private:
    bool init_sdl2_();
    struct HandleEntry {
        uint64_t guest_addr;
        uint32_t type;
    };
    void* window_ = nullptr;
    void* renderer_ = nullptr;
    void* texture_ = nullptr;
    uint32_t width_ = 640;
    uint32_t height_ = 480;
    Memory* mem_ = nullptr;
    std::vector<HandleEntry> handles_;
    uint64_t next_guest_addr_ = 0;
    static constexpr uint64_t HANDLE_BASE = 0x7000000000ULL;
    static constexpr uint64_t HANDLE_STEP = 64;
    static constexpr size_t MAX_HANDLES = 256;
};
} // namespace arm64emu
