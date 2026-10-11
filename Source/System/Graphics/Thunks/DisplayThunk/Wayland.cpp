// Wayland listener callbacks: queue-then-deliver to guest listeners.
#include "System/Graphics/Thunks/DisplayThunk/DisplayThunkInternal.hpp"
// Compile-time guard: the generated message table is parsed content,
// not dead text. wl_surface.attach (opcode 1, object+2 ints) is the
// canary — if wlgen ever emits garbage, this fails the build.
static_assert(arm64emu::wl::kMessageCount > 0, "wlgen emitted no messages");
// A missing row dereferences nullptr, which is not a constant expression.
static_assert(arm64emu::wl::find(false, "wl_surface", 1u)->opcode == 1u,
              "wl_surface.attach missing from wlgen table");
namespace arm64emu {
void DisplayThunk::set_wl_cb_runner(WlCbRunner runner) {
    if (!impl_) return;
    impl_->wl_cb_runner_ = std::move(runner);
}

// Drain queued Wayland events into guest listeners (queue-then-deliver:
// host trampolines push during dispatch, we run guest code only after
// the host call returns — same reentrancy rule as GLFW_POLL).
// generic sig decode: i/u/f raw 32-bit, s strings bounced, o objects
// mapped, a arrays bounced as wl_array, h fds published to guest fds,
// n new-id slots arrive as 0. supports 2 strings / 2 objects per event
// (output geometry, data enter) which the old 4-slot path truncated.
void DisplayThunk::deliver_wl_events_(CPU& cpu) {
    if (!impl_ || !impl_->wl_cb_runner_ || !impl_->proxy_) return;
    DisplayProxy::WlEvent ev;
    while (impl_->proxy_->wl_pop_event(ev)) {
        auto lit = impl_->wl_listeners_.find(ev.proxy);
        if (lit == impl_->wl_listeners_.end()) {
            if (ev.host_fd >= 0) ::close(ev.host_fd);
            continue;
        }
        std::string iface = impl_->proxy_->wl_iface(ev.proxy);
        const arm64emu::wl::Message* msg =
            arm64emu::wl::find(true, iface.c_str(), ev.ev);
        // copy callback + data before running guest code: the guest
        // may add/remove listeners inside the callback, invalidating
        // the iterator for the next loop round.
        uint64_t data = lit->second.data;
        uint64_t fn = 0;
        size_t nfns = lit->second.fns.size();
        if (ev.ev < nfns) fn = lit->second.fns[ev.ev];
        if (!fn) {
            if (ev.host_fd >= 0) ::close(ev.host_fd);
            continue;
        }
        // iargs = {data, proxy} + decoded event args in sig order.
        int64_t iargs[12];
        size_t n = 0;
        iargs[n++] = static_cast<int64_t>(data);
        iargs[n++] = static_cast<int64_t>(ev.proxy);
        if (!msg) {
            // unknown iface (xdg and friends have no table row yet):
            // best-effort raw delivery so a game with a host listener
            // still gets something instead of silence. one-shot note.
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true))
                fprintf(stderr,
                        "[thunk] wl event %s:%u has no signature, "
                        "raw fallback\n",
                        iface.c_str(), ev.ev);
            for (size_t i = 0; i < ev.n_ints && n < 12; i++)
                iargs[n++] = static_cast<int64_t>(ev.ints[i]);
            if (ev.n_strs && n < 12)
                iargs[n++] = static_cast<int64_t>(
                    impl_->proxy_->wl_str_bounce(ev.s.c_str()));
            if (ev.has_obj && n < 12) {
                uint64_t g =
                    impl_->proxy_->wl_guest_for_host(ev.obj);
                iargs[n++] = static_cast<int64_t>(g ? g : ev.proxy);
            }
            if (ev.has_arr && n < 12)
                iargs[n++] = static_cast<int64_t>(
                    impl_->proxy_->wl_arr_bounce(ev.arr.data(),
                                                 ev.arr.size()));
            if (ev.host_fd >= 0) {
                int gfd = -1;
                if (impl_->wl_fd_publisher_)
                    gfd = impl_->wl_fd_publisher_(ev.host_fd);
                ::close(ev.host_fd);
                if (n < 12) iargs[n++] = gfd;
            }
            impl_->wl_cb_runner_(cpu, fn, iargs, n);
            continue;
        }
        size_t ii = 0, si = 0, oi = 0;
        bool fd_used = false;
        for (const char* p = msg->sig; *p && n < 12; p++) {
            char c = *p >= 'A' && *p <= 'Z' ? *p - 'A' + 'a' : *p;
            if (c == 's') {
                const std::string& str = (si == 0) ? ev.s : ev.s2;
                si++;
                iargs[n++] = static_cast<int64_t>(
                    impl_->proxy_->wl_str_bounce(str.c_str()));
                // second string needs its own page half.
                if (si == 2 && n > 0)
                    iargs[n - 1] = static_cast<int64_t>(
                        impl_->proxy_->wl_str_bounce2(str.c_str()));
            } else if (c == 'o') {
                void* obj = nullptr;
                bool has = false;
                if (oi == 0) {
                    obj = ev.obj;
                    has = ev.has_obj;
                } else {
                    obj = ev.obj2;
                    has = ev.has_obj2;
                }
                oi++;
                uint64_t g = has ? impl_->proxy_->wl_guest_for_host(obj)
                                 : ev.proxy;
                iargs[n++] = static_cast<int64_t>(g ? g : ev.proxy);
            } else if (c == 'a') {
                uint64_t g = 0;
                if (ev.has_arr)
                    g = impl_->proxy_->wl_arr_bounce(ev.arr.data(),
                                                     ev.arr.size());
                iargs[n++] = static_cast<int64_t>(g);
            } else if (c == 'h') {
                int gfd = -1;
                if (ev.host_fd >= 0 && !fd_used) {
                    if (impl_->wl_fd_publisher_)
                        gfd = impl_->wl_fd_publisher_(ev.host_fd);
                    else if (dbg().thunk_trace)
                        fprintf(stderr,
                                "[thunk] wl event %s:%u fd %d dropped "
                                "(no publisher)\n",
                                iface.c_str(), ev.ev, ev.host_fd);
                    // host lib keeps its copy; close ours only when the
                    // publisher duped it (publisher owns the dup).
                    ::close(ev.host_fd);
                    fd_used = true;
                }
                iargs[n++] = static_cast<int64_t>(gfd);
            } else if (c == 'n') {
                iargs[n++] = 0;
            } else if (ii < 8) {
                iargs[n++] = static_cast<int64_t>(ev.ints[ii++]);
            } else {
                iargs[n++] = 0;
            }
        }
        if (dbg().thunk_trace) {
            fprintf(stderr, "[thunk] wl event %s:%u -> 0x%llx\n",
                    iface.c_str(), ev.ev,
                    static_cast<unsigned long long>(fn));
        }
        impl_->wl_cb_runner_(cpu, fn, iargs, n);
    }
}
}  // namespace arm64emu
