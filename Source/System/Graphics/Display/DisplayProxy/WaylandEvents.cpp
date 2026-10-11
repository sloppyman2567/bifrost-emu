// DisplayProxy WaylandEvents: guest handles and host display forwarding.
#include "System/Graphics/Display/DisplayProxy/DisplayProxyInternal.hpp"
namespace arm64emu {
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

} // namespace arm64emu
