// DisplayProxy Wayland: guest handles and host display forwarding.
#include "System/Graphics/Display/DisplayProxy/DisplayProxyInternal.hpp"
namespace arm64emu {
void DisplayProxy::wl_flush_all_() {
    if (!wlfn_flush_) return;
    using Fn = int (*)(void*);
    auto fn = reinterpret_cast<Fn>(wlfn_flush_);
    for (auto& kv : wl_objs_) {
        if (kv.second.iface == "wl_display" && kv.second.host)
            fn(kv.second.host);
    }
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
