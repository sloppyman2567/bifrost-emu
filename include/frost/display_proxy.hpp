// frost_graphics/display_proxy.hpp — host-side display proxy for guest
// X11/Wayland apps.
//
// 1.5.4-alpha: NEW. Displays owns ONE host SDL2 window and exposes
// guest-callable proxy entry points for a minimal X11 and Wayland
// surface-creation + draw loop.
//
// Handles (Display*, Window, wl_display*, wl_surface*) are guest
// addresses backed by the emulator's Memory. The thunk trampoline path
// translates them automatically via guest_to_host_ptr().
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
// Host wire structs for the hand-built xdg interfaces (xdg-shell has no
// host .so; we synthesize wl_interfaces from the vendored XML at runtime).
struct wl_interface;
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
    // Set when SDL_QUIT / SDL_WINDOWEVENT_CLOSE is drained in present().
    bool quit_requested() const { return quit_requested_; }
    void* host_window() const { return window_; }
    Memory* memory() const { return mem_; }
    void set_memory(Memory* mem) { mem_ = mem; }
    uint64_t alloc_handle(uint32_t type);
    void free_handle(uint64_t guest_addr);
    void* handle_to_host(uint64_t guest_addr) const;
    // ── X11 core ─────────────────────────────────────────────────────
    uint64_t XOpenDisplay(const char* name);
    int XCloseDisplay(uint64_t display_guest);
    uint64_t XCreateSimpleWindow(uint64_t display_guest, uint64_t parent, int x, int y, int w, int h, int bw, unsigned long border, unsigned long background);
    uint64_t XCreateWindow(uint64_t display_guest, uint64_t parent, int x, int y, unsigned w, unsigned h, unsigned bw, int depth, unsigned long visual, uint64_t visual_ptr, unsigned long valuemask, void* attributes);
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
    unsigned long XCreateGC(uint64_t display_guest, unsigned long drawable, unsigned long valuemask, void* values, int screen, uint64_t visual);
    int XFreeGC(uint64_t display_guest, unsigned long gc);
    int XStoreName(uint64_t display_guest, uint64_t window_guest, const char* name);
    int XGetWindowAttributes(uint64_t display_guest, uint64_t window_guest, void* attrs);
    int XSelectInput(uint64_t display_guest, uint64_t window_guest, long event_mask);
    unsigned long XInternAtom(uint64_t display_guest, const char* name, int only_if_exists);
    int XSetWMProtocols(uint64_t display_guest, uint64_t window_guest, void* protocols, int count);
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
    int XChangeProperty(uint64_t display_guest, uint64_t window_guest, unsigned long prop, unsigned long type, int format, int mode, const void* data, long nelements);
    int XDeleteProperty(uint64_t display_guest, uint64_t window_guest, unsigned long prop);
    int XCopyArea(uint64_t display_guest, uint64_t src_drawable, uint64_t dest_drawable, unsigned long gc, int src_x, int src_y, int dest_x, int dest_y, int width, int height);
    unsigned long XCreatePixmap(uint64_t display_guest, unsigned long drawable, int width, int height, int depth);
    int XFreePixmap(uint64_t display_guest, unsigned long pixmap);
    int XSetWindowBackgroundPixmap(uint64_t display_guest, uint64_t window_guest, unsigned long pixmap);
    int XSetClipMask(uint64_t display_guest, unsigned long gc, unsigned long bitmap);
    int XSetClipOrigin(uint64_t display_guest, unsigned long gc, int x, int y);
    int XCopyGC(uint64_t display_guest, unsigned long src_gc, unsigned long valuemask, unsigned long dest_gc);
    int XChangeGC(uint64_t display_guest, unsigned long gc, unsigned long valuemask, const void* values);
    int XSetFunction(uint64_t display_guest, unsigned long gc, int function);
    int XSetDashes(uint64_t display_guest, unsigned long gc, int dash_offset, const char* dashes);
    int XFreeColors(uint64_t display_guest, unsigned long colormap, const unsigned long* pixels, int num_pixels, unsigned long planes);
    // ── XShm (X Shared Memory extension) ────────────────────────────
    int XShmQueryExtension(uint64_t display_guest);
    int XShmGetEventBase(uint64_t display_guest);
    uint64_t XShmCreateImage(uint64_t display_guest, uint64_t visual, unsigned int depth, int format, void* data, void* shminfo, unsigned int width, unsigned int height);
    int XShmAttach(uint64_t display_guest, uint64_t shmseg_guest);
    int XShmDetach(uint64_t display_guest, uint64_t shmseg_guest);
    int XShmPutImage(uint64_t display_guest, uint64_t drawable, uint64_t gc, uint64_t image, int src_x, int src_y, int dst_x, int dst_y, unsigned int src_width, unsigned int src_height, bool send_event);
    int XShmGetImage(uint64_t display_guest, uint64_t drawable, uint64_t image, int x, int y, unsigned int width, unsigned int height, unsigned long plane_mask);
    // ── GLX ──────────────────────────────────────────────────────────
    uint64_t glXChooseVisual(uint64_t display_guest, int screen, const int* attrib_list);
    uint64_t glXCreateContext(uint64_t display_guest, uint64_t visual, uint64_t share_list, int direct);
    int glXDestroyContext(uint64_t display_guest, uint64_t context);
    int glXMakeCurrent(uint64_t display_guest, uint64_t drawable, uint64_t context);
    void glXSwapBuffers(uint64_t display_guest, uint64_t drawable);
    const char* glXGetClientString(uint64_t display_guest, int name);
    const char* glXQueryExtensionsString(uint64_t display_guest, int screen);
    const char* glXQueryServerString(uint64_t display_guest, int screen, int name);
    uint64_t glXGetFBConfigs(uint64_t display_guest, int screen, int* nelements);
    int glXGetFBConfigAttrib(uint64_t display_guest, uint64_t fbconfig, int attribute, int* value);
    uint64_t glXCreateWindow(uint64_t display_guest, uint64_t config, uint64_t window, const int* attrib_list);
    int glXDestroyWindow(uint64_t display_guest, uint64_t window);
    uint64_t glXCreatePbuffer(uint64_t display_guest, uint64_t config, const int* attrib_list);
    int glXDestroyPbuffer(uint64_t display_guest, uint64_t pbuffer);
    // ── XRandR ───────────────────────────────────────────────────────
    uint64_t XRRGetScreenResources(uint64_t display_guest, uint64_t window);
    uint64_t XRRGetScreenResourcesCurrent(uint64_t display_guest, uint64_t window);
    void XRRFreeScreenResources(uint64_t resources_guest);
    uint64_t XRRGetCrtcInfo(uint64_t display_guest, uint64_t resources, uint64_t crtc);
    void XRRFreeCrtcInfo(uint64_t crtc_info_guest);
    uint64_t XRRGetOutputInfo(uint64_t display_guest, uint64_t resources, uint64_t output);
    void XRRFreeOutputInfo(uint64_t output_info_guest);
    int XRRSetCrtcConfig(uint64_t display_guest, uint64_t resources, uint64_t crtc, uint64_t timestamp, int x, int y, uint64_t mode, unsigned int rotation, uint64_t outputs_guest, int noutputs);
    int XRRGetScreenSizeRange(uint64_t display_guest, int screen, int* min_width, int* min_height, int* max_width, int* max_height);
    // ── Xkb (X Keyboard extension) ──────────────────────────────────
    int XkbOpenDevice(uint64_t display_guest, int device_id);
    uint64_t XkbGetMap(uint64_t display_guest, uint64_t device_spec, unsigned int which);
    int XkbGetState(uint64_t display_guest, uint64_t device_spec, void* state_return);
    int XkbSetState(uint64_t display_guest, uint64_t device_spec, unsigned int map_part, void* state);
    int XkbSetAutoRepeatRate(uint64_t display_guest, uint64_t device_spec, unsigned int delay, unsigned int interval);
    int XkbGetAutoRepeatRate(uint64_t display_guest, uint64_t device_spec, unsigned int* delay_return, unsigned int* interval_return);
    void XkbFreeKeyboard(uint64_t xkb_guest);
    // ── Wayland ──────────────────────────────────────────────────────
    uint64_t wl_egl_window_create(uint64_t surface_guest, int width, int height);
    int wl_egl_window_destroy(uint64_t window_guest);
    int wl_egl_window_get_attached_size(uint64_t window_guest, void* width, void* height);
    int wl_egl_window_resize(uint64_t window_guest, int x, int y, int width, int height);
    uint64_t wl_display_connect(const char* name);
    void wl_display_disconnect(uint64_t display_guest);
    // ── Wayland host bridge (real compositor forwarding) ─────────
    // Lifecycle/event-loop calls forward to host libwayland when the
    // display handle carries a host object (see wl_display_connect);
    // stub handles keep the old return-0 behavior. No-ops stay safe
    // headless: without a compositor every handle is a stub.
    int wl_display_get_fd(uint64_t display_guest);
    int wl_display_flush(uint64_t display_guest);
    int wl_display_dispatch(uint64_t display_guest);
    int wl_display_dispatch_pending(uint64_t display_guest);
    int wl_display_roundtrip(uint64_t display_guest);
    int wl_display_read_events(uint64_t display_guest);
    int wl_display_prepare_read(uint64_t display_guest);
    int wl_display_cancel_read(uint64_t display_guest);
    void wl_proxy_destroy(uint64_t proxy_guest);
    // Host object for a guest handle, or nullptr for stub handles.
    void* wl_host(uint64_t guest_addr) const;
    // Guest-fd -> host-fd translation for wl 'h' (fd-passing) args,
    // wired by the emulator (mirrors AndroidSurfaceManager). Without
    // it, fd-carrying requests bail loud instead of sending garbage.
    using FdResolver = std::function<int(int guest_fd)>;
    void set_fd_resolver(FdResolver r) { fd_resolver_ = std::move(r); }
    // Guest handle owning a host object (for event attribution), or 0.
    uint64_t wl_guest_for_host(const void* host) const;
    // Interface name bound to a mapped object ("" when unknown/stub).
    std::string wl_iface(uint64_t guest_addr) const;
    uint64_t wl_display_get_registry(uint64_t display_guest);
    // Generic request marshaller driven by the opgen_wl signature table.
    // `regs` points at the first vararg (x3, or x4 for the versioned
    // shape); remaining varargs spill to the guest stack at sp.
    // `ifstruct` is the guest wl_interface* for constructor calls
    // (name at +0 selects the created type for bind).
    // Returns the guest handle of the created object for constructors,
    // 0 otherwise (and 0 with a loud-miss note when the pair is unknown
    // or needs fd-passing, which stays unsupported).
    uint64_t wl_marshal(uint64_t proxy_guest, uint32_t opcode,
                        uint64_t ifstruct, bool is_versioned,
                        uint32_t version, const uint64_t* regs, int nregs,
                        uint64_t sp);
    // Pending inbound events queued by the host listener trampolines,
    // drained by DisplayThunk after dispatch/roundtrip.
    // widened for full sig decode: up to 8 int/fixed words, 2 strings,
    // 2 objects, one array blob, one host fd. keeps every event the
    // table describes without silent truncation.
    struct WlEvent {
        uint64_t proxy = 0;
        uint32_t ev = 0;
        uint64_t ints[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        uint8_t n_ints = 0;
        std::string s;
        std::string s2;
        uint8_t n_strs = 0;
        // Non-proxy object args in sig order (e.g. enter's surface+offer).
        void* obj = nullptr;
        bool has_obj = false;
        void* obj2 = nullptr;
        bool has_obj2 = false;
        // Array arg payload (wl_array content copy), if any.
        std::vector<uint8_t> arr;
        bool has_arr = false;
        // Host fd from the compositor (keymap, data send), -1 when none.
        // published to a guest fd at delivery time via fd_publisher_.
        int host_fd = -1;
    };
    bool wl_pop_event(WlEvent& out);
    void wl_push_event(const WlEvent& ev);
    uint64_t wl_dropped() const { return wl_dropped_; }
    // Host fd -> guest fd publisher (wired by the emulator; dup + wrap
    // in HostNode + fds_.allocate). returns guest fd or -1.
    using FdPublisher = std::function<int(int host_fd)>;
    void set_fd_publisher(FdPublisher p) { fd_publisher_ = std::move(p); }
    // Guest-visible scratch for event strings (sticky bounce).
    uint64_t wl_str_bounce(const char* s);
    // Second sticky string slot so two-string events survive delivery.
    uint64_t wl_str_bounce2(const char* s);
    // Guest-visible scratch for event arrays (sticky bounce).
    // lays out struct wl_array { size, alloc, data } + payload.
    uint64_t wl_arr_bounce(const uint8_t* data, size_t n);
    // Class name of a mapped object as a guest string address (0 when
    // unmapped). Bounced, valid until the next string goes through.
    uint64_t wl_class_name(uint64_t proxy_guest);
    // Host registry listener support (installed once per host registry).
    bool wl_registry_listen(void* host_registry);
    // Hand-built xdg_shell wire interfaces (v1 view) for marshal +
    // listeners. nullptr for non-xdg names.
    const wl_interface* wl_xdg_iface(const std::string& name);
    // Reply to an xdg_wm_base ping on its host object (auto-pong keeps
    // the compositor from killing slow clients).
    void wl_xdg_pong_host(void* host_base, uint32_t serial);
    // Install the static host listener for pointer/keyboard/touch/
    // output/seat/callback objects. Records the guest proxy for event
    // attribution (single object per kind in practice).
    bool wl_listen_for(void* host_obj, uint64_t guest_proxy,
                       const std::string& iface);
    // Full event push for new trampolines (touch/output/seat/data/xdg).
    // kind: 0=pointer 1=keyboard 2=callback 3=touch 4=output 5=seat
    // 6=data_offer 7=xdg_wm_base 8=xdg_surface 9=xdg_toplevel.
    static void wl_note_full(int kind, uint32_t ev, const uint64_t* ints,
                             uint8_t n_ints, const char* s, const char* s2,
                             void* obj, bool has_obj, void* obj2,
                             bool has_obj2, const uint8_t* arr, size_t arr_n,
                             int host_fd);
    // Static host trampolines funnel through here (they cannot touch
    // private state directly): record the feeding proxy (kind 0/1/2 =
    // pointer/keyboard/callback) and push an attributed event.
    static void wl_note_proxy(int kind, uint64_t guest_proxy);
    static void wl_note_event(int kind, uint32_t ev, uint64_t a,
                              uint64_t b, uint64_t c, uint64_t d,
                              void* obj, bool has_obj);
    uint64_t wl_surface_create(uint64_t display_guest, const char* interface, uint32_t version);
    void wl_surface_commit(uint64_t surface_guest);
    void wl_surface_destroy(uint64_t surface_guest);
    // Ensure the SDL2 window exists (stub fallback path). Returns ready().
    bool ensure_sdl();
private:
    bool init_sdl2_();
    // Push queued wayland requests to the compositor on every known
    // display connection. Used after commit/ack/pong marshals so a guest
    // that only calls dispatch_pending can never starve the compositor
    // (the classic "not responding" cause).
    void wl_flush_all_();
    // Drain host SDL2 events into the X11 event queue (plus quit flag).
    // Called from present() and every XPending/XNextEvent-family entry so
    // an X11 guest that never calls XFlush still pumps the host window.
    void x_pump_();
    struct XQEv { uint8_t b[192]; };
    std::vector<XQEv> x_queue_;
    // Last display/window seen (event attribution) + per-window select mask.
    uint64_t x_last_display_ = 0;
    uint64_t x_last_window_ = 0;
    std::unordered_map<uint64_t, long> x_select_mask_;
    // Display guest handle -> published guest fd for wl_display_get_fd.
    // Published once via fd_publisher_ (dup + HostNode + allocate) so the
    // guest polls a real guest fd instead of a colliding raw host number.
    std::unordered_map<uint64_t, int> wl_fd_guest_;
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
    uint64_t handle_page_end_ = 0;
    bool quit_requested_ = false;
    static constexpr uint64_t HANDLE_STEP = 64;
    static constexpr size_t MAX_HANDLES = 512;
    // Handle type tags.
    static constexpr uint32_t H_DISPLAY    = 1;
    static constexpr uint32_t H_WINDOW     = 2;
    static constexpr uint32_t H_GC         = 5;
    static constexpr uint32_t H_COLORMAP   = 6;
    static constexpr uint32_t H_PIXMAP     = 7;
    static constexpr uint32_t H_EGL_WINDOW = 8;
    static constexpr uint32_t H_SHMSEG     = 9;
    static constexpr uint32_t H_GLX_CONTEXT= 10;
    static constexpr uint32_t H_GLX_WINDOW = 11;
    static constexpr uint32_t H_GLX_PBUFFER= 12;
    static constexpr uint32_t H_RANDR_CRTC = 13;
    static constexpr uint32_t H_RANDR_OUTPUT= 14;
    static constexpr uint32_t H_RANDR_MODE = 15;
    static constexpr uint32_t H_XKB         = 16;
    static constexpr uint32_t H_SHM_IMAGE  = 17;
    // ── Wayland host bridge state ──────────────────────────────────
    void* wl_client_ = nullptr;  // dlopen'd libwayland-client (lazy)
    // Resolved host entry points (null until bridge init succeeds).
    void* wlfn_connect_ = nullptr;
    void* wlfn_disconnect_ = nullptr;
    void* wlfn_flush_ = nullptr;
    void* wlfn_get_fd_ = nullptr;
    void* wlfn_dispatch_ = nullptr;
    void* wlfn_dispatch_pending_ = nullptr;
    void* wlfn_roundtrip_ = nullptr;
    void* wlfn_read_events_ = nullptr;
    void* wlfn_prepare_read_ = nullptr;
    void* wlfn_cancel_read_ = nullptr;
    void* wlfn_proxy_destroy_ = nullptr;
    // Guest handle -> {host wl_display*/wl_proxy*, interface name}.
    // Absent = stub handle (old behavior: return 0). Populated by
    // wl_display_connect / marshal constructors, dropped by
    // disconnect/destroy.
    struct WlObj {
        void* host = nullptr;
        std::string iface;
        uint32_t version = 0;  // bound/created version (for creations)
    };
    std::unordered_map<uint64_t, WlObj> wl_objs_;
    // Inbound event queue (host listener trampolines push, thunk drains).
    std::vector<WlEvent> wl_pending_;
    uint64_t wl_dropped_ = 0;
    FdResolver fd_resolver_;
    FdPublisher fd_publisher_;
    // Guest bounce for event arrays (sticky, like strings).
    uint64_t wl_arr_bounce_ = 0;
    size_t wl_arr_size_ = 0;
    // Sticky guest bounce for event strings.
    uint64_t wl_str_bounce_ = 0;
    void* wl_registry_host_listener_ = nullptr;  // installed once
    // Guest proxy currently feeding each listener (single object per
    // kind in practice; last install wins).
    uint64_t wl_ptr_proxy_ = 0;
    uint64_t wl_kb_proxy_ = 0;
    uint64_t wl_cb_proxy_ = 0;
    uint64_t wl_touch_proxy_ = 0;
    uint64_t wl_output_proxy_ = 0;
    uint64_t wl_seat_proxy_ = 0;
    uint64_t wl_data_offer_proxy_ = 0;
    uint64_t wl_xdg_base_proxy_ = 0;
    uint64_t wl_xdg_surf_proxy_ = 0;
    uint64_t wl_xdg_top_proxy_ = 0;
    bool wl_bridge_init_();
};
} // namespace arm64emu
