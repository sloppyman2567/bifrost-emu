// DisplayProxy WaylandMarshal: guest handles and host display forwarding.
#include "System/Graphics/Display/DisplayProxy/DisplayProxyInternal.hpp"
namespace arm64emu {
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

} // namespace arm64emu
