#include "frost/window_stats.hpp"
// frost_graphics/display_proxy.cpp — DisplayProxy implementation.
//
// 1.5.4-alpha: NEW. Owns a single host SDL2 window and translates
// minimal X11 / Wayland calls from the guest into SDL2 render calls.
//
// Guest handles (Display*, Window, wl_display*, wl_surface*) are guest-
// memory addresses. The thunk trampoline path translates them to host
// pointers via guest_to_host_ptr(); we store a small handle header at
// each guest address so we can recover our proxy state on entry.
#include "frost/display_proxy.hpp"
#include "core/memory.h"
#include "opgen_wl.hpp"
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
DisplayProxy::DisplayProxy() = default;
DisplayProxy::~DisplayProxy() { shutdown(); }
void DisplayProxy::shutdown() {
#if defined(BIFROST_USE_SDL2)
    SDL_Window* w = (SDL_Window*)window_;
    SDL_Renderer* r = (SDL_Renderer*)renderer_;
    SDL_Texture* t = (SDL_Texture*)texture_;
    if (t) SDL_DestroyTexture(t);
    if (r) SDL_DestroyRenderer(r);
    if (w) { window_stats::forget(w); SDL_DestroyWindow(w); }
#endif
    handles_.clear();
    next_guest_addr_ = 0;
    handle_page_end_ = 0;
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
    // Prevent SDL from installing its own signal handlers, which would
    // interfere with the emulator's signal delivery (sigint, sigsuspend, etc.).
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
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
        window_stats::forget(window_); SDL_DestroyWindow((SDL_Window*)window_); window_ = nullptr;
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
    if (window_ && renderer_) {
        SDL_SetRenderDrawColor((SDL_Renderer*)renderer_, 0, 0, 0, 255);
        SDL_RenderClear((SDL_Renderer*)renderer_);
        if (texture_) {
            SDL_RenderCopy((SDL_Renderer*)renderer_, (SDL_Texture*)texture_, nullptr, nullptr);
        }
        SDL_RenderPresent((SDL_Renderer*)renderer_);
        window_stats::sdl_present(window_,"Display proxy");
    }
    // Always pump SDL events (even without a window) so the host window
    // never looks frozen and X11 input keeps flowing. x_pump_ owns the
    // drain now — present() no longer drops input on the floor.
    x_pump_();
#else
    (void)window_; (void)renderer_; (void)texture_;
#endif
}

bool DisplayProxy::ensure_sdl() {
    if (ready()) return true;
    return init_sdl2_();
}

void DisplayProxy::wl_flush_all_() {
    if (!wlfn_flush_) return;
    using Fn = int (*)(void*);
    auto fn = reinterpret_cast<Fn>(wlfn_flush_);
    for (auto& kv : wl_objs_) {
        if (kv.second.iface == "wl_display" && kv.second.host)
            fn(kv.second.host);
    }
}

// ── X11 host event pump (SDL2 -> XEvent queue) ─────────────────────────
// Minimal AArch64 Xlib layout: type@0 i32, serial@8 u64, send_event@16 i32,
// display@24 ptr, window@32 u64, root@40 u64, subwindow@48 u64, time@56 u64,
// x@64 i32, y@68 i32, x_root@72 i32, y_root@76 i32, state@80 u32,
// detail@84 u32 (keycode/button), same_screen@88 i32. ClientMessage (33)
// reuses the first 48 bytes then format@48 + data.l[5]@56.
void DisplayProxy::x_pump_() {
#if defined(BIFROST_USE_SDL2)
    auto cur_win = [&]() -> uint64_t {
        if (x_last_window_) return x_last_window_;
        for (auto& h : handles_) {
            if (h.type == H_WINDOW) return h.guest_addr;
        }
        return 0;
    };
    auto cur_disp = [&]() -> uint64_t {
        if (x_last_display_) return x_last_display_;
        for (auto& h : handles_) {
            if (h.type == H_DISPLAY) return h.guest_addr;
        }
        return 0;
    };
    auto push_key = [&](int type, uint32_t keycode, uint32_t state) {
        XQEv e{};
        std::memset(e.b, 0, sizeof(e.b));
        std::memcpy(e.b + 0, &type, 4);
        uint64_t w = cur_win(), d = cur_disp();
        std::memcpy(e.b + 24, &d, 8);
        std::memcpy(e.b + 32, &w, 8);
        uint32_t t = SDL_GetTicks();
        std::memcpy(e.b + 56, &t, 4);
        std::memcpy(e.b + 80, &state, 4);
        std::memcpy(e.b + 84, &keycode, 4);
        int ss = 1;
        std::memcpy(e.b + 88, &ss, 4);
        if (x_queue_.size() < 256) x_queue_.push_back(e);
    };
    auto push_button = [&](int type, uint32_t button, int x, int y,
                           uint32_t state) {
        XQEv e{};
        std::memset(e.b, 0, sizeof(e.b));
        std::memcpy(e.b + 0, &type, 4);
        uint64_t w = cur_win(), d = cur_disp();
        std::memcpy(e.b + 24, &d, 8);
        std::memcpy(e.b + 32, &w, 8);
        uint32_t t = SDL_GetTicks();
        std::memcpy(e.b + 56, &t, 4);
        std::memcpy(e.b + 64, &x, 4);
        std::memcpy(e.b + 68, &y, 4);
        std::memcpy(e.b + 72, &x, 4);
        std::memcpy(e.b + 76, &y, 4);
        std::memcpy(e.b + 80, &state, 4);
        std::memcpy(e.b + 84, &button, 4);
        int ss = 1;
        std::memcpy(e.b + 88, &ss, 4);
        if (x_queue_.size() < 256) x_queue_.push_back(e);
    };
    auto push_motion = [&](int x, int y, uint32_t state) {
        XQEv e{};
        int type = 6;  // MotionNotify
        std::memset(e.b, 0, sizeof(e.b));
        std::memcpy(e.b + 0, &type, 4);
        uint64_t w = cur_win(), d = cur_disp();
        std::memcpy(e.b + 24, &d, 8);
        std::memcpy(e.b + 32, &w, 8);
        uint32_t t = SDL_GetTicks();
        std::memcpy(e.b + 56, &t, 4);
        std::memcpy(e.b + 64, &x, 4);
        std::memcpy(e.b + 68, &y, 4);
        std::memcpy(e.b + 72, &x, 4);
        std::memcpy(e.b + 76, &y, 4);
        std::memcpy(e.b + 80, &state, 4);
        int ss = 1;
        std::memcpy(e.b + 88, &ss, 4);
        if (x_queue_.size() < 256) x_queue_.push_back(e);
    };
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) {
            quit_requested_ = true;
        } else if (ev.type == SDL_WINDOWEVENT &&
                   ev.window.event == SDL_WINDOWEVENT_CLOSE) {
            quit_requested_ = true;
            XQEv e{};
            int type = 33;  // ClientMessage (close request)
            std::memset(e.b, 0, sizeof(e.b));
            std::memcpy(e.b + 0, &type, 4);
            uint64_t w = cur_win(), d = cur_disp();
            std::memcpy(e.b + 24, &d, 8);
            std::memcpy(e.b + 32, &w, 8);
            int fmt = 32;
            std::memcpy(e.b + 48, &fmt, 4);
            if (x_queue_.size() < 256) x_queue_.push_back(e);
        } else if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
            if (ev.key.repeat) continue;
            uint32_t keycode = (uint32_t)ev.key.keysym.scancode + 8;
            uint16_t mod = ev.key.keysym.mod;
            uint32_t state = 0;
            if (mod & KMOD_SHIFT) state |= 1;
            if (mod & KMOD_CAPS) state |= 2;
            if (mod & KMOD_CTRL) state |= 4;
            if (mod & KMOD_ALT) state |= 8;
            push_key(ev.type == SDL_KEYDOWN ? 2 : 3, keycode, state);
        } else if (ev.type == SDL_MOUSEBUTTONDOWN ||
                   ev.type == SDL_MOUSEBUTTONUP) {
            uint32_t b = 1;
            if (ev.button.button == 2) b = 2;
            else if (ev.button.button == 3) b = 3;
            push_button(ev.type == SDL_MOUSEBUTTONDOWN ? 4 : 5, b,
                        ev.button.x, ev.button.y, 0);
        } else if (ev.type == SDL_MOUSEMOTION) {
            uint32_t state = 0;
            if (ev.motion.state & SDL_BUTTON_LMASK) state |= 256;
            if (ev.motion.state & SDL_BUTTON_MMASK) state |= 512;
            if (ev.motion.state & SDL_BUTTON_RMASK) state |= 1024;
            push_motion(ev.motion.x, ev.motion.y, state);
        } else if (ev.type == SDL_MOUSEWHEEL) {
            int mx = 0, my = 0;
            SDL_GetMouseState(&mx, &my);
            if (ev.wheel.y > 0)
                push_button(4, 4, mx, my, 0);
            else if (ev.wheel.y < 0)
                push_button(4, 5, mx, my, 0);
        }
    }
#endif
}
uint64_t DisplayProxy::alloc_handle(uint32_t type) {
    if (!mem_ || handles_.size() >= MAX_HANDLES) return 0;
    // Reserve real guest storage instead of writing into a fixed, unmapped
    // address (which also overlapped the sigreturn trampoline). Pack handles
    // into pages and keep their addresses distinct after free_handle().
    if (next_guest_addr_ == handle_page_end_) {
        uint64_t page = mem_->mmap_alloc(Memory::PAGE_SIZE, 0, false,
            Memory::GUEST_PROT_READ | Memory::GUEST_PROT_WRITE);
        if (!page) return 0;
        next_guest_addr_ = page;
        handle_page_end_ = page + Memory::PAGE_SIZE;
    }
    uint64_t guest_addr = next_guest_addr_;
    HandleHdr hdr;
    hdr.type = type;
    hdr.index = (uint32_t)handles_.size();
    mem_->write(guest_addr, &hdr, sizeof(hdr));
    handles_.push_back({guest_addr, type});
    next_guest_addr_ += HANDLE_STEP;
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
    if (!ready() && !ensure_sdl()) return 0;
    uint64_t d = alloc_handle(1);
    if (d) x_last_display_ = d;
    return d;
}
int DisplayProxy::XCloseDisplay(uint64_t display_guest) {
    if (display_guest == 0) return 0;
    free_handle(display_guest);
    return 1;
}
uint64_t DisplayProxy::XCreateWindow(uint64_t display_guest, uint64_t parent, int x, int y, unsigned w, unsigned h, unsigned bw, int depth, unsigned long visual, uint64_t visual_ptr, unsigned long valuemask, void* attributes) {
    (void)display_guest; (void)parent; (void)x; (void)y;
    (void)w; (void)h; (void)bw; (void)depth; (void)visual;
    (void)visual_ptr; (void)valuemask; (void)attributes;
    if (!ready() && !ensure_sdl()) return 0;
    uint64_t win = alloc_handle(2);
    if (win) x_last_window_ = win;
    return win;
}
uint64_t DisplayProxy::XCreateSimpleWindow(uint64_t display_guest, uint64_t parent, int x, int y, int w, int h, int bw, unsigned long border, unsigned long background) {
    (void)display_guest; (void)parent; (void)x; (void)y;
    (void)w; (void)h; (void)bw; (void)border; (void)background;
    if (!ready() && !ensure_sdl()) return 0;
    uint64_t win = alloc_handle(2);
    if (win) x_last_window_ = win;
    return win;
}
int DisplayProxy::XMapWindow(uint64_t display_guest, uint64_t window_guest) {
    (void)display_guest;
    if (window_guest) x_last_window_ = window_guest;
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
bool DisplayProxy::wl_bridge_init_() {
    if (wl_client_) return wlfn_connect_ != nullptr;
    wl_client_ = dlopen("libwayland-client.so.0", RTLD_LAZY);
    if (!wl_client_) wl_client_ = dlopen("libwayland-client.so", RTLD_LAZY);
    if (!wl_client_) return false;
    wlfn_connect_ = dlsym(wl_client_, "wl_display_connect");
    wlfn_disconnect_ = dlsym(wl_client_, "wl_display_disconnect");
    wlfn_flush_ = dlsym(wl_client_, "wl_display_flush");
    wlfn_get_fd_ = dlsym(wl_client_, "wl_display_get_fd");
    wlfn_dispatch_ = dlsym(wl_client_, "wl_display_dispatch");
    wlfn_dispatch_pending_ = dlsym(wl_client_, "wl_display_dispatch_pending");
    wlfn_roundtrip_ = dlsym(wl_client_, "wl_display_roundtrip");
    wlfn_read_events_ = dlsym(wl_client_, "wl_display_read_events");
    wlfn_prepare_read_ = dlsym(wl_client_, "wl_display_prepare_read");
    wlfn_cancel_read_ = dlsym(wl_client_, "wl_display_cancel_read");
    wlfn_proxy_destroy_ = dlsym(wl_client_, "wl_proxy_destroy");
    return wlfn_connect_ != nullptr;
}
void* DisplayProxy::wl_host(uint64_t guest_addr) const {
    if (guest_addr == 0) return nullptr;
    auto it = wl_objs_.find(guest_addr);
    return it != wl_objs_.end() ? it->second.host : nullptr;
}
std::string DisplayProxy::wl_iface(uint64_t guest_addr) const {
    auto it = wl_objs_.find(guest_addr);
    return it != wl_objs_.end() ? it->second.iface : std::string();
}
uint64_t DisplayProxy::wl_guest_for_host(const void* host) const {
    if (!host) return 0;
    for (const auto& kv : wl_objs_) {
        if (kv.second.host == host) return kv.first;
    }
    return 0;
}
uint64_t DisplayProxy::wl_display_connect(const char* name) {
    uint64_t guest = alloc_handle(3);
    if (!guest) return 0;
    // Prefer a real host compositor connection first — this needs no SDL
    // window. Only fall back to the SDL stub (which needs a window) when
    // headless. Old code required ready() up front, spawning a phantom SDL
    // window for every bridged wayland guest that was never pumped.
    if (wl_bridge_init_()) {
        using ConnectFn = void* (*)(const char*);
        void* host = reinterpret_cast<ConnectFn>(wlfn_connect_)(name);
        if (host) {
            wl_objs_[guest] = WlObj{host, "wl_display", 1};
            return guest;
        }
    }
    if (!ensure_sdl()) {
        free_handle(guest);
        return 0;
    }
    return guest;
}
void DisplayProxy::wl_display_disconnect(uint64_t display_guest) {
    auto it = wl_objs_.find(display_guest);
    if (it != wl_objs_.end()) {
        if (wlfn_disconnect_) {
            using DiscFn = void (*)(void*);
            reinterpret_cast<DiscFn>(wlfn_disconnect_)(it->second.host);
        }
        wl_objs_.erase(it);
    }
    // Drop the cached guest-fd mapping. The guest fd itself stays open for
    // the guest to close (same ownership as the keymap publisher path).
    wl_fd_guest_.erase(display_guest);
    free_handle(display_guest);
}
int DisplayProxy::wl_display_get_fd(uint64_t display_guest) {
    // Return a cached published guest fd when we have one: the guest polls
    // this number, which resolves through FdTable to the dup'd host socket
    // (same open file description → same POLLIN). Handing out the raw host
    // number collides with unrelated guest fds and stalls event loops.
    auto itc = wl_fd_guest_.find(display_guest);
    if (itc != wl_fd_guest_.end()) return itc->second;
    void* host = wl_host(display_guest);
    if (!host || !wlfn_get_fd_) return -1;
    using Fn = int (*)(void*);
    int hfd = reinterpret_cast<Fn>(wlfn_get_fd_)(host);
    if (hfd < 0) return -1;
    if (fd_publisher_) {
        int gfd = fd_publisher_(hfd);
        if (gfd >= 0) {
            wl_fd_guest_[display_guest] = gfd;
            return gfd;
        }
    }
    return hfd;  // no publisher wired: legacy raw fd
}
int DisplayProxy::wl_display_flush(uint64_t display_guest) {
    void* host = wl_host(display_guest);
    if (!host || !wlfn_flush_) return 0;
    using Fn = int (*)(void*);
    return reinterpret_cast<Fn>(wlfn_flush_)(host);
}
int DisplayProxy::wl_display_dispatch(uint64_t display_guest) {
    void* host = wl_host(display_guest);
    if (!host || !wlfn_dispatch_) return 0;
    using Fn = int (*)(void*);
    return reinterpret_cast<Fn>(wlfn_dispatch_)(host);
}
int DisplayProxy::wl_display_dispatch_pending(uint64_t display_guest) {
    void* host = wl_host(display_guest);
    if (!host || !wlfn_dispatch_pending_) return 0;
    using Fn = int (*)(void*);
    return reinterpret_cast<Fn>(wlfn_dispatch_pending_)(host);
}
int DisplayProxy::wl_display_roundtrip(uint64_t display_guest) {
    void* host = wl_host(display_guest);
    if (!host || !wlfn_roundtrip_) return 0;
    using Fn = int (*)(void*);
    return reinterpret_cast<Fn>(wlfn_roundtrip_)(host);
}
int DisplayProxy::wl_display_read_events(uint64_t display_guest) {
    void* host = wl_host(display_guest);
    if (!host || !wlfn_read_events_) return 0;
    using Fn = int (*)(void*);
    return reinterpret_cast<Fn>(wlfn_read_events_)(host);
}
int DisplayProxy::wl_display_prepare_read(uint64_t display_guest) {
    void* host = wl_host(display_guest);
    if (!host || !wlfn_prepare_read_) return 0;
    using Fn = int (*)(void*);
    return reinterpret_cast<Fn>(wlfn_prepare_read_)(host);
}
int DisplayProxy::wl_display_cancel_read(uint64_t display_guest) {
    void* host = wl_host(display_guest);
    if (!host || !wlfn_cancel_read_) return 0;
    using Fn = int (*)(void*);
    return reinterpret_cast<Fn>(wlfn_cancel_read_)(host);
}
void DisplayProxy::wl_proxy_destroy(uint64_t proxy_guest) {
    auto it = wl_objs_.find(proxy_guest);
    if (it != wl_objs_.end()) {
        if (wlfn_proxy_destroy_) {
            using Fn = void (*)(void*);
            reinterpret_cast<Fn>(wlfn_proxy_destroy_)(it->second.host);
        }
        wl_objs_.erase(it);
    }
    free_handle(proxy_guest);
}

// ── xdg_shell wire interfaces (hand-built, v1 view) ───────────────
// xdg-shell ships no host .so, so libwayland has no xdg_wm_base_interface
// symbols to dlsym. We synthesize the wl_interfaces from the vendored
// xdg-shell.xml instead (opcodes/signatures match the scanner output).
// Only the ops we marshal need exact types; the rest carry correct
// counts + signatures with null types. Bound at version 1 everywhere
// (every used op/event is v1-stable), so listener arrays stay exact.
namespace {
// forward decls for cross-referencing types arrays.
extern const wl_interface kXdgWmBaseIf;
extern const wl_interface kXdgSurfaceIf;
extern const wl_interface kXdgToplevelIf;
extern const wl_interface kXdgPositionerIf;
extern const wl_interface kXdgPopupIf;

// types arrays (one per message with object/new_id args we marshal).
// the wl_surface slot is patched to the host interface on first use.
const wl_interface* kWmBaseGetSurfaceTypes[] = {&kXdgSurfaceIf, nullptr};
const wl_interface* kSurfaceGetToplevelTypes[] = {&kXdgToplevelIf};
const wl_interface* kSurfaceGetPopupTypes[] = {nullptr, nullptr, nullptr};

const wl_message kWmBaseRequests[] = {
    {"destroy", "", nullptr},
    {"create_positioner", "n", nullptr},  // types patched at first use
    {"get_xdg_surface", "no", kWmBaseGetSurfaceTypes},
    {"pong", "u", nullptr},
};
const wl_message kWmBaseEvents[] = {
    {"ping", "u", nullptr},
};
const wl_interface kXdgWmBaseIf = {"xdg_wm_base", 1, 4, kWmBaseRequests,
                                   1, kWmBaseEvents};

const wl_message kSurfaceRequests[] = {
    {"destroy", "", nullptr},
    {"get_toplevel", "n", kSurfaceGetToplevelTypes},
    {"get_popup", "n?oo", kSurfaceGetPopupTypes},
    {"set_window_geometry", "iiii", nullptr},
    {"ack_configure", "u", nullptr},
};
const wl_message kSurfaceEvents[] = {
    {"configure", "u", nullptr},
};
const wl_interface kXdgSurfaceIf = {"xdg_surface", 1, 5, kSurfaceRequests,
                                    1, kSurfaceEvents};

const wl_message kToplevelRequests[] = {
    {"destroy", "", nullptr},
    {"set_parent", "?o", nullptr},
    {"set_title", "s", nullptr},
    {"set_app_id", "s", nullptr},
    {"show_window_menu", "ouii", nullptr},
    {"move", "ou", nullptr},
    {"resize", "oui", nullptr},
    {"set_max_size", "ii", nullptr},
    {"set_min_size", "ii", nullptr},
    {"set_maximized", "", nullptr},
    {"unset_maximized", "", nullptr},
    {"set_fullscreen", "?o", nullptr},
    {"unset_fullscreen", "", nullptr},
    {"set_minimized", "", nullptr},
};
const wl_message kToplevelEvents[] = {
    {"configure", "iia", nullptr},
    {"close", "", nullptr},
};
const wl_interface kXdgToplevelIf = {"xdg_toplevel", 1, 14, kToplevelRequests,
                                     2, kToplevelEvents};

const wl_message kPositionerRequests[] = {
    {"destroy", "", nullptr},
};
const wl_interface kXdgPositionerIf = {"xdg_positioner", 1, 1,
                                       kPositionerRequests, 0, nullptr};
const wl_message kPopupRequests[] = {
    {"destroy", "", nullptr},
};
const wl_interface kXdgPopupIf = {"xdg_popup", 1, 1, kPopupRequests,
                                  0, nullptr};
}  // namespace

const wl_interface* DisplayProxy::wl_xdg_iface(const std::string& name) {
    const wl_interface* out = nullptr;
    if (name == "xdg_wm_base") out = &kXdgWmBaseIf;
    else if (name == "xdg_surface") out = &kXdgSurfaceIf;
    else if (name == "xdg_toplevel") out = &kXdgToplevelIf;
    else if (name == "xdg_positioner") out = &kXdgPositionerIf;
    else if (name == "xdg_popup") out = &kXdgPopupIf;
    else return nullptr;
    // patch the wl_surface types slot once the bridge is up.
    if (!kWmBaseGetSurfaceTypes[1] && wl_bridge_init_()) {
        void* s = dlsym(wl_client_, "wl_surface_interface");
        if (s) kWmBaseGetSurfaceTypes[1] =
            static_cast<const wl_interface*>(s);
    }
    return out;
}

void DisplayProxy::wl_xdg_pong_host(void* host_base, uint32_t serial) {
    if (!host_base || !wl_bridge_init_()) return;
    void* fn = dlsym(wl_client_, "wl_proxy_marshal_array");
    if (!fn) return;
    struct WlArg { uint64_t v = 0; };
    WlArg args[1];
    args[0].v = serial;
    using Fn = void (*)(void*, uint32_t, void*);
    reinterpret_cast<Fn>(fn)(host_base, 3 /* pong */, args);
}

// ── Wayland generic marshal (opgen_wl signature-driven) ────────────
// libwayland wire types: int32/uint32/wl_fixed (4 bytes), pointers and
// fds (8-byte slots in wl_argument). Guest varargs arrive in regs[0..4]
// (= x3..x7) then the guest stack at sp, 8 bytes each.
namespace {
// Host wl_argument layout (see wayland-util.h): 32-bit values in the
// low half, pointers native. We only ever fill 8-byte slots.
struct WlArg { uint64_t v = 0; };
}  // namespace

uint64_t DisplayProxy::wl_display_get_registry(uint64_t display_guest) {
    // NOTE: wl_display_get_registry is inline in the real headers (a
    // marshal_constructor for wl_display op 1) — there is no host symbol
    // to dlsym. Route through the generic marshaller (no varargs).
    return wl_marshal(display_guest, 1, 0, false, 0, nullptr, 0, 0);
}

bool DisplayProxy::wl_pop_event(WlEvent& out) {
    if (wl_pending_.empty()) return false;
    out = wl_pending_.front();
    wl_pending_.erase(wl_pending_.begin());
    return true;
}

void DisplayProxy::wl_push_event(const WlEvent& ev) {
    // cap the queue so a motion-heavy aaa frame cannot grow it without
    // bound when the guest stops dispatching. drop oldest, count it.
    static constexpr size_t kMaxPending = 1024;
    if (wl_pending_.size() >= kMaxPending) {
        wl_pending_.erase(wl_pending_.begin());
        wl_dropped_++;
        if (wl_dropped_ == 1 || wl_dropped_ % 1024 == 0)
            fprintf(stderr,
                    "[display-thunk] wl event queue full, dropped %llu\n",
                    static_cast<unsigned long long>(wl_dropped_));
    }
    wl_pending_.push_back(ev);
}

uint64_t DisplayProxy::wl_class_name(uint64_t proxy_guest) {
    void* host = wl_host(proxy_guest);
    if (!host || !wl_bridge_init_()) return 0;
    void* fn = dlsym(wl_client_, "wl_proxy_get_class");
    if (!fn) return 0;
    using Fn = const char* (*)(void*);
    const char* name = reinterpret_cast<Fn>(fn)(host);
    return wl_str_bounce(name);
}

uint64_t DisplayProxy::wl_str_bounce(const char* s) {
    if (!s || !mem_) return 0;
    if (!wl_str_bounce_) {
        wl_str_bounce_ = mem_->mmap_alloc(4096);
        if (!wl_str_bounce_) return 0;
    }
    // first half of a split page; cap so wl_str_bounce2's half survives.
    size_t n = strlen(s) + 1;
    if (n > 2048) n = 2048;
    mem_->write(wl_str_bounce_, s, n);
    return wl_str_bounce_;
}

uint64_t DisplayProxy::wl_str_bounce2(const char* s) {
    if (!s || !mem_) return 0;
    // second half of a doubled string page; first half stays valid.
    if (!wl_str_bounce_) {
        wl_str_bounce_ = mem_->mmap_alloc(4096);
        if (!wl_str_bounce_) return 0;
    }
    size_t n = strlen(s) + 1;
    if (n > 2048) n = 2048;
    mem_->write(wl_str_bounce_ + 2048, s, n);
    return wl_str_bounce_ + 2048;
}

uint64_t DisplayProxy::wl_arr_bounce(const uint8_t* data, size_t n) {
    if (!mem_) return 0;
    if (n > 4096) n = 4096;
    size_t total = 24 + n;
    if (!wl_arr_bounce_ || wl_arr_size_ < total) {
        if (!wl_arr_bounce_) {
            wl_arr_bounce_ = mem_->mmap_alloc(8192);
            if (!wl_arr_bounce_) return 0;
        }
        wl_arr_size_ = 8192;
    }
    // struct wl_array guest layout: size, alloc, data_ptr.
    uint64_t guest_data = wl_arr_bounce_ + 24;
    uint64_t words[3] = {n, n, guest_data};
    mem_->write(wl_arr_bounce_, words, sizeof(words));
    if (n && data) mem_->write(guest_data, data, n);
    return wl_arr_bounce_;
}

uint64_t DisplayProxy::wl_marshal(uint64_t proxy_guest, uint32_t opcode,
                                  uint64_t ifstruct, bool is_versioned,
                                  uint32_t version,
                                  const uint64_t* regs, int nregs,
                                  uint64_t sp) {
    void* host = wl_host(proxy_guest);
    std::string ifname = wl_iface(proxy_guest);
    if (!host || ifname.empty() || !wl_bridge_init_() || !mem_) return 0;
    const arm64emu::wl::Message* msg =
        arm64emu::wl::find(false, ifname.c_str(), opcode);
    if (!msg) {
        fprintf(stderr, "[display-thunk] wl_marshal: unknown %s op %u\n",
                ifname.c_str(), opcode);
        return 0;
    }
    // New-object interface: from the opgen_wl `created` field — except
    // bind (wl_registry op 0), whose new_id carries no interface in the
    // XML by design (it arrives as the C-level wl_interface* instead).
    std::string new_iface = msg->created;
    bool is_bind = !strcmp(ifname.c_str(), "wl_registry") && opcode == 0;
    if (is_bind && new_iface.empty()) new_iface = "<bind>";
    // Decode varargs per signature. 'h' (fd) needs fd translation
    // (phase 2) — bail loud instead of sending garbage.
    WlArg args[16];
    int nargs = 0, r = 0;
    auto next_vararg = [&](uint64_t& out) -> bool {
        if (r < nregs) {
            out = regs[r++];
            return true;
        }
        uint64_t slot = 0;
        try {
            mem_->read(sp + (uint64_t)(r - 5) * 8, &slot, 8);
        } catch (...) { return false; }
        r++;
        out = slot;
        return true;
    };
    // When the request creates an object, the host constructor call
    // allocates the id itself — no guest arg is consumed for 'n'.
    // (Plain marshal of a pre-existing id still passes it through.)
    bool skip_new_id = !new_iface.empty();
    for (const char* p = msg->sig; *p && nargs < 16; p++) {
        char c = *p >= 'A' && *p <= 'Z' ? *p - 'A' + 'a' : *p;
        if (c == 'n' && skip_new_id) {
            // placeholder: libwayland's create_outgoing_proxy writes the
            // fresh proxy into args[i] here before marshal reads the rest.
            // without the slot every later arg shifts by one ('h' then
            // dups the size word as an fd -> EBADF).
            args[nargs++].v = 0;
            continue;
        }
        uint64_t v = 0;
        if (!next_vararg(v)) return 0;
        if (c == 'h') {
            // fd-passing: translate the guest fd to the host fd. The
            // host lib sends it over the compositor socket (SCM_RIGHTS).
            if (!fd_resolver_) {
                fprintf(stderr, "[display-thunk] wl_marshal: fd arg in %s op %u, no resolver\n",
                        ifname.c_str(), opcode);
                return 0;
            }
            int host_fd = fd_resolver_(static_cast<int>(v & 0xFFFFFFFFULL));
            if (host_fd < 0) {
                fprintf(stderr, "[display-thunk] wl_marshal: bad guest fd in %s op %u\n",
                        ifname.c_str(), opcode);
                return 0;
            }
            v = static_cast<uint64_t>(host_fd);
        }
        if (c == 's') {
            // Strings are copied into the message by the host lib, so a
            // direct guest alias is safe for the duration of the call.
            uint8_t* hp = v ? mem_->guest_to_host_ptr(v) : nullptr;
            v = reinterpret_cast<uint64_t>(hp);
        } else if (c == 'o') {
            // Object args travel as mapped host pointers (null stays null).
            auto it = wl_objs_.find(v);
            v = (v && it != wl_objs_.end())
                    ? reinterpret_cast<uint64_t>(it->second.host)
                    : 0;
        } else if (c == 'a') {
            // wl_array {size, alloc, data}: rebuild with a host-visible
            // data pointer; the host lib copies `size` bytes out.
            struct Arr { uint64_t size, alloc, data; };
            Arr src{0, 0, 0}, dst{0, 0, 0};
            if (v) {
                try { mem_->read(v, &src, sizeof(src)); }
                catch (...) { return 0; }
                uint8_t* hp = mem_->guest_to_host_ptr(src.data);
                if (!hp) return 0;
                dst.size = src.size;
                dst.alloc = src.size;
                dst.data = reinterpret_cast<uint64_t>(hp);
            }
            // Host copies synchronously; thread-local outlives the call.
            static thread_local Arr live;
            live = dst;
            v = reinterpret_cast<uint64_t>(&live);
        } else {
            v &= 0xFFFFFFFFULL;  // i/u/f travel 32-bit
        }
        args[nargs++].v = v;
    }
    if (new_iface.empty()) {
        // No object created: plain fire-and-forget request.
        void* fn = dlsym(wl_client_, "wl_proxy_marshal_array");
        if (!fn) return 0;
        using Fn = void (*)(void*, uint32_t, void*);
        reinterpret_cast<Fn>(fn)(host, opcode, args);
        // Latency-critical replies must reach the compositor even when
        // the guest only calls dispatch_pending (which never flushes):
        // surface commit, xdg ack_configure, xdg pong.
        bool urgent = (ifname == "wl_surface" && opcode == 6) ||
                      (ifname == "xdg_surface" && opcode == 4) ||
                      (ifname == "xdg_wm_base" && opcode == 3);
        if (urgent) wl_flush_all_();
        return 0;
    }
    // bind (wl_registry op 0) takes a dedicated path below: current
    // host libwayland validates bind as (name:uint, interface:string,
    // version:uint, id) while the vendored XML still describes the old
    // (uint, new_id) shape, so the generic walker cannot express it.
    if (is_bind) {
        if (!ifstruct) return 0;
        uint64_t namep = 0;
        try { mem_->read(ifstruct, &namep, 8); }
        catch (...) { return 0; }
        char iname[128] = {0};
        if (namep) {
            try {
                for (size_t i = 0; i < sizeof(iname) - 1; i++) {
                    uint8_t ch = 0;
                    mem_->read(namep + i, &ch, 1);
                    iname[i] = static_cast<char>(ch);
                    if (!ch) break;
                }
            } catch (...) { return 0; }
        }
        if (!iname[0]) return 0;
        // bind varargs are ALWAYS [name, version] starting at regs[0]
        // (x3, or x4 in the versioned shape — the caller already offset).
        // Version: explicit (versioned shape) else regs[1].
        if (1 >= nregs) return 0;
        uint64_t bv = regs[0];
        uint32_t ver = version;
        if (!is_versioned) ver = static_cast<uint32_t>(regs[1]);
        // Interface name string for the wire (host copies it out).
        static thread_local char wire_iface[128];
        strncpy(wire_iface, iname, sizeof(wire_iface) - 1);
        wire_iface[sizeof(wire_iface) - 1] = 0;
        WlArg bargs[3];
        bargs[0].v = bv & 0xFFFFFFFFULL;
        bargs[1].v = reinterpret_cast<uint64_t>(wire_iface);
        bargs[2].v = ver;
        std::string sym = std::string(iname) + "_interface";
        void* host_iface = dlsym(wl_client_, sym.c_str());
        if (!host_iface) {
            // xdg-shell ships no host .so: use the hand-built table.
            const wl_interface* xi = wl_xdg_iface(iname);
            if (xi) host_iface = const_cast<wl_interface*>(xi);
        }
        void* cfn = is_versioned
            ? dlsym(wl_client_, "wl_proxy_marshal_array_constructor_versioned")
            : dlsym(wl_client_, "wl_proxy_marshal_array_constructor");
        if (!cfn || !host_iface) return 0;
        void* obj = nullptr;
        if (is_versioned) {
            using Fn = void* (*)(void*, uint32_t, void*, const void*, uint32_t);
            obj = reinterpret_cast<Fn>(cfn)(host, opcode, bargs,
                                            host_iface, ver);
        } else {
            using Fn = void* (*)(void*, uint32_t, void*, const void*);
            obj = reinterpret_cast<Fn>(cfn)(host, opcode, bargs, host_iface);
        }
        if (!obj) return 0;
        uint64_t guest = alloc_handle(3);
        if (!guest) return 0;
        wl_objs_[guest] = WlObj{obj, iname, ver};
        return guest;
    }
    // Object-creating request. The new object's interface is `created`
    // (bind returned early above with its own path).
    std::string real_iface = new_iface;
    // Constructor version: explicit `version` (versioned shape); else
    // bind takes it from the next vararg (name, version), while other
    // creations inherit the parent object's bound version (falling back
    // to the interface struct's when the parent has none recorded).
    uint32_t ver = version;
    if (!is_versioned) {
        auto pit = wl_objs_.find(proxy_guest);
        ver = (pit != wl_objs_.end() && pit->second.version)
                  ? pit->second.version
                  : 0;
        if (!ver && ifstruct) {
            uint32_t iv = 0;
            try { mem_->read(ifstruct + 8, &iv, 4); }
            catch (...) { return 0; }
            ver = iv;
        }
        if (!ver) {
            fprintf(stderr, "[display-thunk] wl_marshal: no version for %s op %u\n",
                    ifname.c_str(), opcode);
            return 0;
        }
    }
    void* cfn = is_versioned
        ? dlsym(wl_client_, "wl_proxy_marshal_array_constructor_versioned")
        : dlsym(wl_client_, "wl_proxy_marshal_array_constructor");
    if (!cfn) return 0;
    std::string sym = real_iface + "_interface";
    void* host_iface = dlsym(wl_client_, sym.c_str());
    if (!host_iface) {
        // xdg-shell ships no host .so: use the hand-built table.
        const wl_interface* xi = wl_xdg_iface(real_iface);
        if (xi) {
            host_iface = const_cast<wl_interface*>(xi);
        } else {
            fprintf(stderr, "[display-thunk] wl_marshal: no host %s\n",
                    sym.c_str());
            return 0;
        }
    }
    void* obj = nullptr;
    if (is_versioned) {
        using Fn = void* (*)(void*, uint32_t, void*, const void*, uint32_t);
        obj = reinterpret_cast<Fn>(cfn)(host, opcode, args,
                                        host_iface, ver);
    } else {
        using Fn = void* (*)(void*, uint32_t, void*, const void*);
        obj = reinterpret_cast<Fn>(cfn)(host, opcode, args, host_iface);
    }
    if (!obj) return 0;
    uint64_t guest = alloc_handle(3);
    if (!guest) return 0;
    wl_objs_[guest] = WlObj{obj, real_iface, ver};
    return guest;
}

// Host registry listener: the two wl_registry events, queued for guest
// delivery after dispatch/roundtrip returns (mirrors the GLFW_POLL
// deliver-after-poll pattern — never re-enter the guest mid-dispatch).
namespace {
DisplayProxy* g_wl_sink = nullptr;
}  // namespace

void DisplayProxy::wl_note_proxy(int kind, uint64_t guest_proxy) {
    if (!g_wl_sink) return;
    if (kind == 0)
        g_wl_sink->wl_ptr_proxy_ = guest_proxy;
    else if (kind == 1)
        g_wl_sink->wl_kb_proxy_ = guest_proxy;
    else if (kind == 2)
        g_wl_sink->wl_cb_proxy_ = guest_proxy;
    else if (kind == 3)
        g_wl_sink->wl_touch_proxy_ = guest_proxy;
    else if (kind == 4)
        g_wl_sink->wl_output_proxy_ = guest_proxy;
    else if (kind == 5)
        g_wl_sink->wl_seat_proxy_ = guest_proxy;
    else if (kind == 6)
        g_wl_sink->wl_data_offer_proxy_ = guest_proxy;
    else if (kind == 7)
        g_wl_sink->wl_xdg_base_proxy_ = guest_proxy;
    else if (kind == 8)
        g_wl_sink->wl_xdg_surf_proxy_ = guest_proxy;
    else if (kind == 9)
        g_wl_sink->wl_xdg_top_proxy_ = guest_proxy;
}

void DisplayProxy::wl_note_event(int kind, uint32_t ev, uint64_t a,
                                 uint64_t b, uint64_t c, uint64_t d,
                                 void* obj, bool has_obj) {
    if (!g_wl_sink) return;
    DisplayProxy::WlEvent e;
    e.proxy = kind == 0   ? g_wl_sink->wl_ptr_proxy_
              : kind == 1 ? g_wl_sink->wl_kb_proxy_
              : kind == 2 ? g_wl_sink->wl_cb_proxy_
              : kind == 3 ? g_wl_sink->wl_touch_proxy_
              : kind == 4 ? g_wl_sink->wl_output_proxy_
              : kind == 5 ? g_wl_sink->wl_seat_proxy_
              : kind == 6 ? g_wl_sink->wl_data_offer_proxy_
              : kind == 7 ? g_wl_sink->wl_xdg_base_proxy_
              : kind == 8 ? g_wl_sink->wl_xdg_surf_proxy_
                          : g_wl_sink->wl_xdg_top_proxy_;
    e.ev = ev;
    e.ints[0] = a;
    e.ints[1] = b;
    e.ints[2] = c;
    e.ints[3] = d;
    e.n_ints = 4;
    e.obj = obj;
    e.has_obj = has_obj;
    g_wl_sink->wl_push_event(e);
}

void DisplayProxy::wl_note_full(int kind, uint32_t ev, const uint64_t* ints,
                                uint8_t n_ints, const char* s, const char* s2,
                                void* obj, bool has_obj, void* obj2,
                                bool has_obj2, const uint8_t* arr, size_t arr_n,
                                int host_fd) {
    if (!g_wl_sink) return;
    DisplayProxy::WlEvent e;
    e.proxy = kind == 0   ? g_wl_sink->wl_ptr_proxy_
              : kind == 1 ? g_wl_sink->wl_kb_proxy_
              : kind == 2 ? g_wl_sink->wl_cb_proxy_
              : kind == 3 ? g_wl_sink->wl_touch_proxy_
              : kind == 4 ? g_wl_sink->wl_output_proxy_
              : kind == 5 ? g_wl_sink->wl_seat_proxy_
              : kind == 6 ? g_wl_sink->wl_data_offer_proxy_
              : kind == 7 ? g_wl_sink->wl_xdg_base_proxy_
              : kind == 8 ? g_wl_sink->wl_xdg_surf_proxy_
                          : g_wl_sink->wl_xdg_top_proxy_;
    e.ev = ev;
    if (ints && n_ints) {
        if (n_ints > 8) n_ints = 8;
        for (uint8_t i = 0; i < n_ints; i++) e.ints[i] = ints[i];
        e.n_ints = n_ints;
    }
    if (s) {
        e.s = s;
        e.n_strs = 1;
    }
    if (s2) {
        e.s2 = s2;
        e.n_strs = e.n_strs ? 2 : 1;
        if (!s) e.s = s2;
    }
    e.obj = obj;
    e.has_obj = has_obj;
    e.obj2 = obj2;
    e.has_obj2 = has_obj2;
    if (arr && arr_n) {
        if (arr_n > 4096) arr_n = 4096;
        e.arr.assign(arr, arr + arr_n);
        e.has_arr = true;
    }
    e.host_fd = host_fd;
    g_wl_sink->wl_push_event(e);
}

namespace {
// Static host trampolines funnel through the public emitters above
// (they cannot touch private state directly).
// convention: host libwayland calls fn(data, obj, ...args) — the object
// MUST be a param, otherwise every arg shifts and fds/pointers corrupt.
void wl_reg_global(void* data, void* reg, uint32_t name,
                   const char* iface, uint32_t version) {
    (void)data;
    if (!g_wl_sink) return;
    DisplayProxy::WlEvent ev;
    ev.proxy = g_wl_sink->wl_guest_for_host(reg);
    ev.ev = 0;
    ev.ints[0] = name;
    ev.ints[1] = version;
    ev.n_ints = 2;
    ev.s = iface ? iface : "";
    ev.n_strs = 1;
    g_wl_sink->wl_push_event(ev);
}
void wl_reg_remove(void* data, void* reg, uint32_t name) {
    (void)data;
    if (!g_wl_sink) return;
    DisplayProxy::WlEvent ev;
    ev.proxy = g_wl_sink->wl_guest_for_host(reg);
    ev.ev = 1;
    ev.ints[0] = name;
    ev.n_ints = 1;
    g_wl_sink->wl_push_event(ev);
}
void wl_ptr_enter(void*, void*, void* surf, uint32_t s, int32_t x, int32_t y) {
    DisplayProxy::wl_note_event(0, 0, s, (uint32_t)x, (uint32_t)y, 0, surf, surf != nullptr);
}
void wl_ptr_leave(void*, void*, void* surf, uint32_t s) {
    DisplayProxy::wl_note_event(0, 1, s, 0, 0, 0, surf, surf != nullptr);
}
void wl_ptr_motion(void*, void*, uint32_t t, int32_t x, int32_t y) {
    DisplayProxy::wl_note_event(0, 2, t, (uint32_t)x, (uint32_t)y, 0, nullptr, false);
}
void wl_ptr_button(void*, void*, uint32_t s, uint32_t t, uint32_t b, uint32_t st) {
    DisplayProxy::wl_note_event(0, 3, s, t, b, st, nullptr, false);
}
void wl_ptr_axis(void*, void*, uint32_t t, uint32_t ax, int32_t v) {
    DisplayProxy::wl_note_event(0, 4, t, ax, (uint32_t)v, 0, nullptr, false);
}
void wl_ptr_frame(void*, void*) {
    DisplayProxy::wl_note_event(0, 5, 0, 0, 0, 0, nullptr, false);
}
void wl_ptr_axis_src(void*, void*, uint32_t a) {
    DisplayProxy::wl_note_event(0, 6, a, 0, 0, 0, nullptr, false);
}
void wl_ptr_axis_stop(void*, void*, uint32_t t, uint32_t a) {
    DisplayProxy::wl_note_event(0, 7, t, a, 0, 0, nullptr, false);
}
void wl_ptr_axis_disc(void*, void*, uint32_t a, int32_t d) {
    DisplayProxy::wl_note_event(0, 8, a, (uint32_t)d, 0, 0, nullptr, false);
}
void wl_ptr_axis120(void*, void*, uint32_t a, int32_t d) {
    DisplayProxy::wl_note_event(0, 9, a, (uint32_t)d, 0, 0, nullptr, false);
}
void wl_ptr_axis_rel(void*, void*, uint32_t a, uint32_t d) {
    DisplayProxy::wl_note_event(0, 10, a, d, 0, 0, nullptr, false);
}
void wl_kb_key(void*, void*, uint32_t s, uint32_t t, uint32_t k, uint32_t st) {
    DisplayProxy::wl_note_event(1, 3, s, t, k, st, nullptr, false);
}
void wl_kb_mods(void*, void*, uint32_t s, uint32_t dep, uint32_t lat,
                uint32_t lock, uint32_t grp) {
    uint64_t ints[5] = {s, dep, lat, lock, grp};
    DisplayProxy::wl_note_full(1, 4, ints, 5, nullptr, nullptr, nullptr,
                               false, nullptr, false, nullptr, 0, -1);
}
void wl_kb_repeat(void*, void*, int32_t r, int32_t d) {
    DisplayProxy::wl_note_event(1, 5, (uint32_t)r, (uint32_t)d, 0, 0, nullptr, false);
}
void wl_cb_done(void*, void*, uint32_t t) {
    DisplayProxy::wl_note_event(2, 0, t, 0, 0, 0, nullptr, false);
}
// host wl_array layout for event payloads (size_t is 8 bytes: a u32
// struct reads the alloc word as the data pointer and segfaults).
struct WlHostArray { size_t size; size_t alloc; const void* data; };
void wl_kb_keymap(void*, void*, uint32_t fmt, int fd, uint32_t size) {
    uint64_t ints[2] = {fmt, size};
    DisplayProxy::wl_note_full(1, 0, ints, 2, nullptr, nullptr, nullptr,
                               false, nullptr, false, nullptr, 0, fd);
}
void wl_kb_enter(void*, void*, uint32_t serial, void* surf, void* keys) {
    const uint8_t* bytes = nullptr;
    size_t n = 0;
    if (keys) {
        auto* arr = static_cast<WlHostArray*>(keys);
        if (arr->size && arr->size <= 4096 && arr->data) {
            bytes = static_cast<const uint8_t*>(arr->data);
            n = arr->size;
        }
    }
    uint64_t ints[1] = {serial};
    DisplayProxy::wl_note_full(1, 1, ints, 1, nullptr, nullptr, surf,
                               surf != nullptr, nullptr, false, bytes, n, -1);
}
void wl_kb_leave(void*, void*, uint32_t serial, void* surf) {
    uint64_t ints[1] = {serial};
    DisplayProxy::wl_note_full(1, 2, ints, 1, nullptr, nullptr, surf,
                               surf != nullptr, nullptr, false, nullptr, 0,
                               -1);
}
// touch: down/up/motion/frame/cancel/shape/orientation.
void wl_touch_down(void*, void*, uint32_t s, uint32_t t, void* surf, int32_t id,
                   int32_t x, int32_t y) {
    uint64_t ints[5] = {s, t, (uint64_t)(uint32_t)id, (uint64_t)(uint32_t)x,
                        (uint64_t)(uint32_t)y};
    DisplayProxy::wl_note_full(3, 0, ints, 5, nullptr, nullptr, surf,
                               surf != nullptr, nullptr, false, nullptr, 0,
                               -1);
}
void wl_touch_up(void*, void*, uint32_t s, uint32_t t, int32_t id) {
    uint64_t ints[3] = {s, t, (uint64_t)(uint32_t)id};
    DisplayProxy::wl_note_full(3, 1, ints, 3, nullptr, nullptr, nullptr,
                               false, nullptr, false, nullptr, 0, -1);
}
void wl_touch_motion(void*, void*, uint32_t t, int32_t id, int32_t x, int32_t y) {
    uint64_t ints[4] = {t, (uint64_t)(uint32_t)id, (uint64_t)(uint32_t)x,
                        (uint64_t)(uint32_t)y};
    DisplayProxy::wl_note_full(3, 2, ints, 4, nullptr, nullptr, nullptr,
                               false, nullptr, false, nullptr, 0, -1);
}
void wl_touch_frame(void*, void*) {
    DisplayProxy::wl_note_full(3, 3, nullptr, 0, nullptr, nullptr, nullptr,
                               false, nullptr, false, nullptr, 0, -1);
}
void wl_touch_cancel(void*, void*) {
    DisplayProxy::wl_note_full(3, 4, nullptr, 0, nullptr, nullptr, nullptr,
                               false, nullptr, false, nullptr, 0, -1);
}
void wl_touch_shape(void*, void*, int32_t id, int32_t major, int32_t minor) {
    uint64_t ints[3] = {(uint64_t)(uint32_t)id, (uint64_t)(uint32_t)major,
                        (uint64_t)(uint32_t)minor};
    DisplayProxy::wl_note_full(3, 5, ints, 3, nullptr, nullptr, nullptr,
                               false, nullptr, false, nullptr, 0, -1);
}
void wl_touch_orient(void*, void*, int32_t id, int32_t orient) {
    uint64_t ints[2] = {(uint64_t)(uint32_t)id, (uint64_t)(uint32_t)orient};
    DisplayProxy::wl_note_full(3, 6, ints, 2, nullptr, nullptr, nullptr,
                               false, nullptr, false, nullptr, 0, -1);
}
// output: geometry/mode/done/scale/name/description.
void wl_out_geometry(void*, void*, int32_t x, int32_t y, int32_t pw, int32_t ph,
                     int32_t sub, const char* make, const char* model,
                     int32_t tr) {
    uint64_t ints[6] = {(uint64_t)(uint32_t)x, (uint64_t)(uint32_t)y,
                        (uint64_t)(uint32_t)pw, (uint64_t)(uint32_t)ph,
                        (uint64_t)(uint32_t)sub, (uint64_t)(uint32_t)tr};
    DisplayProxy::wl_note_full(4, 0, ints, 6, make, model, nullptr, false,
                               nullptr, false, nullptr, 0, -1);
}
void wl_out_mode(void*, void*, uint32_t f, int32_t w, int32_t h, int32_t r) {
    uint64_t ints[4] = {f, (uint64_t)(uint32_t)w, (uint64_t)(uint32_t)h,
                        (uint64_t)(uint32_t)r};
    DisplayProxy::wl_note_full(4, 1, ints, 4, nullptr, nullptr, nullptr,
                               false, nullptr, false, nullptr, 0, -1);
}
void wl_out_done(void*, void*) {
    DisplayProxy::wl_note_full(4, 2, nullptr, 0, nullptr, nullptr, nullptr,
                               false, nullptr, false, nullptr, 0, -1);
}
void wl_out_scale(void*, void*, int32_t f) {
    uint64_t ints[1] = {(uint64_t)(uint32_t)f};
    DisplayProxy::wl_note_full(4, 3, ints, 1, nullptr, nullptr, nullptr,
                               false, nullptr, false, nullptr, 0, -1);
}
void wl_out_name(void*, void*, const char* name) {
    DisplayProxy::wl_note_full(4, 4, nullptr, 0, name, nullptr, nullptr,
                               false, nullptr, false, nullptr, 0, -1);
}
void wl_out_desc(void*, void*, const char* desc) {
    DisplayProxy::wl_note_full(4, 5, nullptr, 0, desc, nullptr, nullptr,
                               false, nullptr, false, nullptr, 0, -1);
}
// seat: capabilities/name.
void wl_seat_caps(void*, void*, uint32_t caps) {
    uint64_t ints[1] = {caps};
    DisplayProxy::wl_note_full(5, 0, ints, 1, nullptr, nullptr, nullptr,
                               false, nullptr, false, nullptr, 0, -1);
}
void wl_seat_name(void*, void*, const char* name) {
    DisplayProxy::wl_note_full(5, 1, nullptr, 0, name, nullptr, nullptr,
                               false, nullptr, false, nullptr, 0, -1);
}
// data_offer: offer/source_actions/action.
void wl_offer_offer(void*, void*, const char* mime) {
    DisplayProxy::wl_note_full(6, 0, nullptr, 0, mime, nullptr, nullptr,
                               false, nullptr, false, nullptr, 0, -1);
}
void wl_offer_actions(void*, void*, uint32_t acts) {
    uint64_t ints[1] = {acts};
    DisplayProxy::wl_note_full(6, 1, ints, 1, nullptr, nullptr, nullptr,
                               false, nullptr, false, nullptr, 0, -1);
}
void wl_offer_action(void*, void*, uint32_t act) {
    uint64_t ints[1] = {act};
    DisplayProxy::wl_note_full(6, 2, ints, 1, nullptr, nullptr, nullptr,
                               false, nullptr, false, nullptr, 0, -1);
}
// xdg_shell: ping (auto-ponged), surface configure, toplevel
// configure (w/h + states array) / close.
void wl_xdg_ping(void*, void* base, uint32_t serial) {
    // auto-pong first: the compositor kills clients that ignore ping.
    if (g_wl_sink) g_wl_sink->wl_xdg_pong_host(base, serial);
    uint64_t ints[1] = {serial};
    DisplayProxy::wl_note_full(7, 0, ints, 1, nullptr, nullptr, nullptr,
                               false, nullptr, false, nullptr, 0, -1);
}
void wl_xdg_configure(void*, void*, uint32_t serial) {
    uint64_t ints[1] = {serial};
    DisplayProxy::wl_note_full(8, 0, ints, 1, nullptr, nullptr, nullptr,
                               false, nullptr, false, nullptr, 0, -1);
}
void wl_xdg_top_configure(void*, void*, int32_t w, int32_t h, void* states) {
    const uint8_t* bytes = nullptr;
    size_t n = 0;
    if (states) {
        auto* arr = static_cast<WlHostArray*>(states);
        if (arr->size && arr->size <= 4096 && arr->data) {
            bytes = static_cast<const uint8_t*>(arr->data);
            n = arr->size;
        }
    }
    uint64_t ints[2] = {(uint64_t)(uint32_t)w, (uint64_t)(uint32_t)h};
    DisplayProxy::wl_note_full(9, 0, ints, 2, nullptr, nullptr, nullptr,
                               false, nullptr, false, bytes, n, -1);
}
void wl_xdg_top_close(void*, void*) {
    DisplayProxy::wl_note_full(9, 1, nullptr, 0, nullptr, nullptr, nullptr,
                               false, nullptr, false, nullptr, 0, -1);
}
// v4+ toplevel events (only arrive when bound higher; kept real so the
// array never overruns and games get bounds/caps).
void wl_xdg_top_bounds(void*, void*, int32_t w, int32_t h) {
    uint64_t ints[2] = {(uint64_t)(uint32_t)w, (uint64_t)(uint32_t)h};
    DisplayProxy::wl_note_full(9, 2, ints, 2, nullptr, nullptr, nullptr,
                               false, nullptr, false, nullptr, 0, -1);
}
void wl_xdg_top_caps(void*, void*, void* caps) {
    const uint8_t* bytes = nullptr;
    size_t n = 0;
    if (caps) {
        auto* arr = static_cast<WlHostArray*>(caps);
        if (arr->size && arr->size <= 4096 && arr->data) {
            bytes = static_cast<const uint8_t*>(arr->data);
            n = arr->size;
        }
    }
    DisplayProxy::wl_note_full(9, 3, nullptr, 0, nullptr, nullptr, nullptr,
                               false, nullptr, false, bytes, n, -1);
}
struct WlRegListener { void* fns[2]; };
}  // namespace

bool DisplayProxy::wl_registry_listen(void* host_registry) {
    if (!host_registry || !wl_bridge_init_()) return false;
    void* fn = dlsym(wl_client_, "wl_proxy_add_listener");
    if (!fn) return false;
    static WlRegListener listener = {{reinterpret_cast<void*>(wl_reg_global),
                                      reinterpret_cast<void*>(wl_reg_remove)}};
    g_wl_sink = this;
    using Fn = int (*)(void*, void**, void*);
    return reinterpret_cast<Fn>(fn)(host_registry, listener.fns, nullptr) == 0;
}

namespace {
struct WlPtrListener { void* fns[11]; };
struct WlCbListener { void* fns[1]; };
struct WlTouchListener { void* fns[7]; };
struct WlOutputListener { void* fns[6]; };
struct WlSeatListener { void* fns[2]; };
struct WlOfferListener { void* fns[3]; };
struct WlXdgBaseListener { void* fns[1]; };
struct WlXdgSurfListener { void* fns[1]; };
struct WlXdgTopListener { void* fns[4]; };
}  // namespace

bool DisplayProxy::wl_listen_for(void* host_obj, uint64_t guest_proxy,
                                 const std::string& iface) {
    if (!host_obj || !wl_bridge_init_()) return false;
    void* fn = dlsym(wl_client_, "wl_proxy_add_listener");
    if (!fn) return false;
    using AddFn = int (*)(void*, void**, void*);
    auto add = reinterpret_cast<AddFn>(fn);
    g_wl_sink = this;
    if (iface == "wl_pointer") {
        static WlPtrListener l = {{
            reinterpret_cast<void*>(wl_ptr_enter),
            reinterpret_cast<void*>(wl_ptr_leave),
            reinterpret_cast<void*>(wl_ptr_motion),
            reinterpret_cast<void*>(wl_ptr_button),
            reinterpret_cast<void*>(wl_ptr_axis),
            reinterpret_cast<void*>(wl_ptr_frame),
            reinterpret_cast<void*>(wl_ptr_axis_src),
            reinterpret_cast<void*>(wl_ptr_axis_stop),
            reinterpret_cast<void*>(wl_ptr_axis_disc),
            reinterpret_cast<void*>(wl_ptr_axis120),
            reinterpret_cast<void*>(wl_ptr_axis_rel),
        }};
        wl_ptr_proxy_ = guest_proxy;
        return add(host_obj, l.fns, nullptr) == 0;
    }
    if (iface == "wl_keyboard") {
        static void* fns[6] = {reinterpret_cast<void*>(wl_kb_keymap),
                               reinterpret_cast<void*>(wl_kb_enter),
                               reinterpret_cast<void*>(wl_kb_leave),
                               reinterpret_cast<void*>(wl_kb_key),
                               reinterpret_cast<void*>(wl_kb_mods),
                               reinterpret_cast<void*>(wl_kb_repeat)};
        wl_kb_proxy_ = guest_proxy;
        return add(host_obj, fns, nullptr) == 0;
    }
    if (iface == "wl_touch") {
        static WlTouchListener l = {{
            reinterpret_cast<void*>(wl_touch_down),
            reinterpret_cast<void*>(wl_touch_up),
            reinterpret_cast<void*>(wl_touch_motion),
            reinterpret_cast<void*>(wl_touch_frame),
            reinterpret_cast<void*>(wl_touch_cancel),
            reinterpret_cast<void*>(wl_touch_shape),
            reinterpret_cast<void*>(wl_touch_orient),
        }};
        wl_touch_proxy_ = guest_proxy;
        return add(host_obj, l.fns, nullptr) == 0;
    }
    if (iface == "wl_output") {
        static WlOutputListener l = {{
            reinterpret_cast<void*>(wl_out_geometry),
            reinterpret_cast<void*>(wl_out_mode),
            reinterpret_cast<void*>(wl_out_done),
            reinterpret_cast<void*>(wl_out_scale),
            reinterpret_cast<void*>(wl_out_name),
            reinterpret_cast<void*>(wl_out_desc),
        }};
        wl_output_proxy_ = guest_proxy;
        return add(host_obj, l.fns, nullptr) == 0;
    }
    if (iface == "wl_seat") {
        static WlSeatListener l = {{
            reinterpret_cast<void*>(wl_seat_caps),
            reinterpret_cast<void*>(wl_seat_name),
        }};
        wl_seat_proxy_ = guest_proxy;
        return add(host_obj, l.fns, nullptr) == 0;
    }
    if (iface == "wl_data_offer") {
        static WlOfferListener l = {{
            reinterpret_cast<void*>(wl_offer_offer),
            reinterpret_cast<void*>(wl_offer_actions),
            reinterpret_cast<void*>(wl_offer_action),
        }};
        wl_data_offer_proxy_ = guest_proxy;
        return add(host_obj, l.fns, nullptr) == 0;
    }
    if (iface == "wl_callback") {
        static WlCbListener l = {{reinterpret_cast<void*>(wl_cb_done)}};
        wl_cb_proxy_ = guest_proxy;
        return add(host_obj, l.fns, nullptr) == 0;
    }
    if (iface == "xdg_wm_base") {
        static WlXdgBaseListener l = {{reinterpret_cast<void*>(wl_xdg_ping)}};
        wl_xdg_base_proxy_ = guest_proxy;
        return add(host_obj, l.fns, nullptr) == 0;
    }
    if (iface == "xdg_surface") {
        static WlXdgSurfListener l = {{reinterpret_cast<void*>(wl_xdg_configure)}};
        wl_xdg_surf_proxy_ = guest_proxy;
        return add(host_obj, l.fns, nullptr) == 0;
    }
    if (iface == "xdg_toplevel") {
        static WlXdgTopListener l = {{
            reinterpret_cast<void*>(wl_xdg_top_configure),
            reinterpret_cast<void*>(wl_xdg_top_close),
            reinterpret_cast<void*>(wl_xdg_top_bounds),
            reinterpret_cast<void*>(wl_xdg_top_caps),
        }};
        wl_xdg_top_proxy_ = guest_proxy;
        return add(host_obj, l.fns, nullptr) == 0;
    }
    return false;
}

uint64_t DisplayProxy::wl_surface_create(uint64_t display_guest, const char* interface, uint32_t version) {
    (void)display_guest; (void)interface; (void)version;
    if (!ready()) return 0;
    return alloc_handle(4);
}
void DisplayProxy::wl_surface_commit(uint64_t surface_guest) {
    // Real surfaces commit to the host compositor (drives frame
    // callbacks and xdg configure/ack mapping); stub handles keep the
    // SDL present path. wl_surface_commit is inline in the protocol
    // headers (no host symbol), so marshal opcode 6 directly instead
    // of dlsyming it — the old dlsym silently missed and every bridged
    // commit was dropped (xdg surfaces never configured).
    void* host = wl_host(surface_guest);
    if (host && wl_bridge_init_() && mem_) {
        void* fn = dlsym(wl_client_, "wl_proxy_marshal_array");
        if (fn) {
            using Fn = void (*)(void*, uint32_t, void*);
            reinterpret_cast<Fn>(fn)(host, 6 /* commit */, nullptr);
            // Commit without flush leaves the frame queued in libwayland
            // until the next dispatch (which flushes). A guest that only
            // calls dispatch_pending would starve the compositor, so push
            // it out now.
            wl_flush_all_();
            return;
        }
    }
    present();
}
void DisplayProxy::wl_surface_destroy(uint64_t surface_guest) {
    free_handle(surface_guest);
}
// ── Additional X11 proxy methods (1.5.4-alpha) ────────────────────────
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
    int iw = (int)w, ih = (int)h;
    if (x < 0) { iw += x; x = 0; }
    if (y < 0) { ih += y; y = 0; }
    if (x + iw > (int)width_) iw = (int)width_ - x;
    if (y + ih > (int)height_) ih = (int)height_ - y;
    if (iw <= 0 || ih <= 0) return;
    SDL_SetRenderDrawColor((SDL_Renderer*)renderer_, 255, 255, 255, 255);
    SDL_Rect r = {x, y, iw, ih};
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
unsigned long DisplayProxy::XCreateGC(uint64_t display_guest, unsigned long drawable, unsigned long valuemask, void* values, int screen, uint64_t visual) {
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
    (void)display_guest;
    if (window_guest) x_select_mask_[window_guest] = event_mask;
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
int DisplayProxy::XSetWMProtocols(uint64_t display_guest, uint64_t window_guest, void* protocols, int count) {
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
    // pump first so an app that never calls XFlush still sees input and
    // the host window never looks frozen.
    x_pump_();
    return (int)x_queue_.size();
}
int DisplayProxy::XNextEvent(uint64_t display_guest, void* event) {
    (void)display_guest;
    if (!event) return 0;
    x_pump_();
    if (x_queue_.empty()) {
        memset(event, 0, 192);
        return 0;
    }
    memcpy(event, x_queue_.front().b, 192);
    x_queue_.erase(x_queue_.begin());
    return 0;
}
int DisplayProxy::XCheckMaskEvent(uint64_t display_guest, long event_mask, void* event) {
    (void)display_guest;
    if (!event) return 0;
    x_pump_();
    // xlib mask bits: keypress=1, keyrelease=2, buttonpress=4,
    // buttonrelease=8, motion=64. map queued type to its bit.
    for (size_t i = 0; i < x_queue_.size(); i++) {
        int type = 0;
        memcpy(&type, x_queue_[i].b, 4);
        long bit = 0;
        if (type == 2) bit = 1;
        else if (type == 3) bit = 2;
        else if (type == 4) bit = 4;
        else if (type == 5) bit = 8;
        else if (type == 6) bit = 64;
        else if (type == 33) bit = event_mask;  // close: always match
        if (bit && (event_mask & bit)) {
            memcpy(event, x_queue_[i].b, 192);
            x_queue_.erase(x_queue_.begin() + (ptrdiff_t)i);
            return 1;
        }
    }
    return 0;
}
int DisplayProxy::XEventsQueued(uint64_t display_guest, int mode) {
    (void)display_guest; (void)mode;
    x_pump_();
    return (int)x_queue_.size();
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
int DisplayProxy::XChangeProperty(uint64_t display_guest, uint64_t window_guest, unsigned long prop, unsigned long type, int format, int mode, const void* data, long nelements) {
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
int DisplayProxy::XChangeGC(uint64_t display_guest, unsigned long gc, unsigned long valuemask, const void* values) {
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
int DisplayProxy::XFreeColors(uint64_t display_guest, unsigned long colormap, const unsigned long* pixels, int num_pixels, unsigned long planes) {
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

// ── XShm (X Shared Memory extension) ─────────────────────────────────────
// Stubbed for now — the real implementation would use shmget/shmat to create
// shared memory segments and pass the SHM IDs to the host X server. For now
// we return 0 (not available) so callers fall back to XPutImage.
int DisplayProxy::XShmQueryExtension(uint64_t display_guest) {
    (void)display_guest;
    return 0;  // not available
}
int DisplayProxy::XShmGetEventBase(uint64_t display_guest) {
    (void)display_guest;
    return 0;
}
uint64_t DisplayProxy::XShmCreateImage(uint64_t display_guest, uint64_t visual, unsigned int depth, int format, void* data, void* shminfo, unsigned int width, unsigned int height) {
    (void)display_guest; (void)visual; (void)depth; (void)format;
    (void)data; (void)shminfo; (void)width; (void)height;
    if (!ready()) return 0;
    return alloc_handle(H_SHM_IMAGE);
}
int DisplayProxy::XShmAttach(uint64_t display_guest, uint64_t shmseg_guest) {
    (void)display_guest; (void)shmseg_guest;
    return 0;
}
int DisplayProxy::XShmDetach(uint64_t display_guest, uint64_t shmseg_guest) {
    (void)display_guest; (void)shmseg_guest;
    free_handle(shmseg_guest);
    return 1;
}
int DisplayProxy::XShmPutImage(uint64_t display_guest, uint64_t drawable, uint64_t gc, uint64_t image, int src_x, int src_y, int dst_x, int dst_y, unsigned int src_width, unsigned int src_height, bool send_event) {
    (void)display_guest; (void)drawable; (void)gc; (void)image;
    (void)src_x; (void)src_y; (void)dst_x; (void)dst_y;
    (void)src_width; (void)src_height; (void)send_event;
    return 0;
}
int DisplayProxy::XShmGetImage(uint64_t display_guest, uint64_t drawable, uint64_t image, int x, int y, unsigned int width, unsigned int height, unsigned long plane_mask) {
    (void)display_guest; (void)drawable; (void)image;
    (void)x; (void)y; (void)width; (void)height; (void)plane_mask;
    return 0;
}

// ── GLX ────────────────────────────────────────────────────────────────────
// GLX functions are registered but return stubs when host GLX is unavailable.
// When host GLX is available, the host function pointer is passed through.
uint64_t DisplayProxy::glXChooseVisual(uint64_t display_guest, int screen, const int* attrib_list) {
    (void)display_guest; (void)screen; (void)attrib_list;
    if (!ready()) return 0;
    return alloc_handle(H_GLX_WINDOW);
}
uint64_t DisplayProxy::glXCreateContext(uint64_t display_guest, uint64_t visual, uint64_t share_list, int direct) {
    (void)display_guest; (void)visual; (void)share_list; (void)direct;
    if (!ready()) return 0;
    return alloc_handle(H_GLX_CONTEXT);
}
int DisplayProxy::glXDestroyContext(uint64_t display_guest, uint64_t context) {
    (void)display_guest;
    free_handle(context);
    return 1;
}
int DisplayProxy::glXMakeCurrent(uint64_t display_guest, uint64_t drawable, uint64_t context) {
    (void)display_guest; (void)drawable; (void)context;
    return 1;
}
void DisplayProxy::glXSwapBuffers(uint64_t display_guest, uint64_t drawable) {
    (void)display_guest; (void)drawable;
    present();
}
const char* DisplayProxy::glXGetClientString(uint64_t display_guest, int name) {
    (void)display_guest; (void)name;
    static const char* ver = "1.4";
    return ver;
}
const char* DisplayProxy::glXQueryExtensionsString(uint64_t display_guest, int screen) {
    (void)display_guest; (void)screen;
    static const char* exts = "GLX_EXT_import_context GLX_EXT_texture_from_pixmap";
    return exts;
}
const char* DisplayProxy::glXQueryServerString(uint64_t display_guest, int screen, int name) {
    (void)display_guest; (void)screen; (void)name;
    return "bifrost-emu";
}
uint64_t DisplayProxy::glXGetFBConfigs(uint64_t display_guest, int screen, int* nelements) {
    (void)display_guest; (void)screen;
    if (nelements) *nelements = 0;
    return 0;
}
int DisplayProxy::glXGetFBConfigAttrib(uint64_t display_guest, uint64_t fbconfig, int attribute, int* value) {
    (void)display_guest; (void)fbconfig; (void)attribute;
    if (value) *value = 0;
    return 0;
}
uint64_t DisplayProxy::glXCreateWindow(uint64_t display_guest, uint64_t config, uint64_t window, const int* attrib_list) {
    (void)display_guest; (void)config; (void)window; (void)attrib_list;
    if (!ready()) return 0;
    return alloc_handle(H_GLX_WINDOW);
}
int DisplayProxy::glXDestroyWindow(uint64_t display_guest, uint64_t window) {
    (void)display_guest;
    free_handle(window);
    return 1;
}
uint64_t DisplayProxy::glXCreatePbuffer(uint64_t display_guest, uint64_t config, const int* attrib_list) {
    (void)display_guest; (void)config; (void)attrib_list;
    if (!ready()) return 0;
    return alloc_handle(H_GLX_PBUFFER);
}
int DisplayProxy::glXDestroyPbuffer(uint64_t display_guest, uint64_t pbuffer) {
    (void)display_guest;
    free_handle(pbuffer);
    return 1;
}

// ── XRandR ─────────────────────────────────────────────────────────────────
// Minimal RandR stub: returns a single screen config.
uint64_t DisplayProxy::XRRGetScreenResources(uint64_t display_guest, uint64_t window) {
    (void)display_guest; (void)window;
    if (!ready()) return 0;
    return alloc_handle(H_RANDR_MODE);
}
uint64_t DisplayProxy::XRRGetScreenResourcesCurrent(uint64_t display_guest, uint64_t window) {
    (void)display_guest; (void)window;
    if (!ready()) return 0;
    return alloc_handle(H_RANDR_MODE);
}
void DisplayProxy::XRRFreeScreenResources(uint64_t resources_guest) {
    free_handle(resources_guest);
}
uint64_t DisplayProxy::XRRGetCrtcInfo(uint64_t display_guest, uint64_t resources, uint64_t crtc) {
    (void)display_guest; (void)resources; (void)crtc;
    if (!ready()) return 0;
    return alloc_handle(H_RANDR_CRTC);
}
void DisplayProxy::XRRFreeCrtcInfo(uint64_t crtc_info_guest) {
    free_handle(crtc_info_guest);
}
uint64_t DisplayProxy::XRRGetOutputInfo(uint64_t display_guest, uint64_t resources, uint64_t output) {
    (void)display_guest; (void)resources; (void)output;
    if (!ready()) return 0;
    return alloc_handle(H_RANDR_OUTPUT);
}
void DisplayProxy::XRRFreeOutputInfo(uint64_t output_info_guest) {
    free_handle(output_info_guest);
}
int DisplayProxy::XRRSetCrtcConfig(uint64_t display_guest, uint64_t resources, uint64_t crtc, uint64_t timestamp, int x, int y, uint64_t mode, unsigned int rotation, uint64_t outputs_guest, int noutputs) {
    (void)display_guest; (void)resources; (void)crtc; (void)timestamp;
    (void)x; (void)y; (void)mode; (void)rotation; (void)outputs_guest; (void)noutputs;
    return 1;
}
int DisplayProxy::XRRGetScreenSizeRange(uint64_t display_guest, int screen, int* min_width, int* min_height, int* max_width, int* max_height) {
    (void)display_guest; (void)screen;
    if (min_width)  *min_width  = 1;
    if (min_height) *min_height = 1;
    if (max_width)  *max_width  = (int)width_;
    if (max_height) *max_height = (int)height_;
    return 1;
}

// ── Xkb (X Keyboard extension) ─────────────────────────────────────────────
// Stubbed: returns success for state queries so games don't abort.
int DisplayProxy::XkbOpenDevice(uint64_t display_guest, int device_id) {
    (void)display_guest; (void)device_id;
    if (!ready()) return 0;
    return alloc_handle(H_XKB);
}
uint64_t DisplayProxy::XkbGetMap(uint64_t display_guest, uint64_t device_spec, unsigned int which) {
    (void)display_guest; (void)device_spec; (void)which;
    if (!ready()) return 0;
    return alloc_handle(H_XKB);
}
int DisplayProxy::XkbGetState(uint64_t display_guest, uint64_t device_spec, void* state_return) {
    (void)display_guest; (void)device_spec;
    if (!state_return) return 0;
    // Fill a minimal XkbState: mods=0, group=0.
    struct XkbState { uint32_t mods; uint32_t group; };
    auto* s = static_cast<XkbState*>(state_return);
    s->mods = 0;
    s->group = 0;
    return 1;
}
int DisplayProxy::XkbSetState(uint64_t display_guest, uint64_t device_spec, unsigned int map_part, void* state) {
    (void)display_guest; (void)device_spec; (void)map_part; (void)state;
    return 1;
}
int DisplayProxy::XkbSetAutoRepeatRate(uint64_t display_guest, uint64_t device_spec, unsigned int delay, unsigned int interval) {
    (void)display_guest; (void)device_spec; (void)delay; (void)interval;
    return 1;
}
int DisplayProxy::XkbGetAutoRepeatRate(uint64_t display_guest, uint64_t device_spec, unsigned int* delay_return, unsigned int* interval_return) {
    (void)display_guest; (void)device_spec;
    if (delay_return)  *delay_return  = 500;
    if (interval_return) *interval_return = 33;
    return 1;
}
void DisplayProxy::XkbFreeKeyboard(uint64_t xkb_guest) {
    free_handle(xkb_guest);
}
} // namespace arm64emu
