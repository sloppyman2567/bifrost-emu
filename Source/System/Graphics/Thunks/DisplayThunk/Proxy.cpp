// DisplayProxy path for X11/Wayland symbols (SDL2 software fallback).
#include "System/Graphics/Thunks/DisplayThunk/DisplayThunkInternal.hpp"
namespace arm64emu {
bool proxy_symbol_needs_sdl(const std::string& sym_name) {
    static const char* kNoSdlSyms[] = {
        "wl_display_connect", "wl_display_disconnect", "wl_display_get_fd",
        "wl_display_flush", "wl_display_dispatch",
        "wl_display_dispatch_pending", "wl_display_roundtrip",
        "wl_display_read_events", "wl_display_prepare_read",
        "wl_display_cancel_read", "wl_proxy_destroy",
        "wl_display_get_registry", "wl_proxy_get_class",
        "wl_proxy_marshal", "wl_proxy_marshal_constructor",
        "wl_proxy_marshal_constructor_versioned", "wl_proxy_add_listener",
    };
    for (const char* sym : kNoSdlSyms) {
        if (sym_name == sym) return false;
    }
    return true;
}

// Called when a symbol has the THUNK_PROXY flag. The DisplayProxy provides
// a SDL2-based software fallback for X11/Wayland functions when the host
// libraries are unavailable or have no display.
uint64_t DisplayThunk::proxy_dispatch_(CPU& cpu, const std::string& sym_name) {
    if (!impl_->proxy_) {
        cpu.regs[0] = 0;
        return 0;
    }
    // Lazy-init: only create the SDL2 window for symbols that need it
    // (X11 + software stubs). Wayland bridge symbols talk straight to
    // the host compositor and must not spawn a phantom SDL window that
    // is never pumped (compositor flags it not-responding, and clicks
    // into the wrong window do nothing).
    const bool need_sdl = proxy_symbol_needs_sdl(sym_name);
    if (need_sdl) {
        // Same mu-guarded lazy init as dispatch()'s THUNK_PROXY block —
        // proxy_dispatch_ runs on guest threads too and raced init the
        // same way.
        bool proxy_ok = false;
        {
            std::lock_guard<std::mutex> g(impl_->mu);
            if (impl_->proxy_) {
                if (!impl_->proxy_->ready()) {
                    impl_->proxy_->init(640, 480, impl_->mem);
                }
                proxy_ok = impl_->proxy_->ready();
            }
        }
        if (!proxy_ok) {
            cpu.regs[0] = 0;
            return 0;
        }
    } else if (!impl_->proxy_) {
        cpu.regs[0] = 0;
        return 0;
    }
    DisplayProxy* proxy = impl_->proxy_.get();
    Memory* mem = impl_->mem;
    bool trace = dbg().thunk_trace;

    // Helper: translate a guest pointer arg to a host pointer.
    auto g2h = [&](uint64_t guest_addr) -> void* {
        if (!guest_addr || !mem) return nullptr;
        uint8_t* hp = mem->guest_to_host_ptr(guest_addr);
        return hp ? reinterpret_cast<void*>(hp) : nullptr;
    };
    auto g2h_str = [&](uint64_t guest_addr) -> const char* {
        return reinterpret_cast<const char*>(g2h(guest_addr));
    };

    if (sym_name == "XOpenDisplay") {
        const char* name = g2h_str(cpu.regs[0]);
        cpu.regs[0] = proxy->XOpenDisplay(name);
        return 0;
    }
    if (sym_name == "XCloseDisplay") {
        cpu.regs[0] = proxy->XCloseDisplay(cpu.regs[0]);
        return 0;
    }
    if (sym_name == "XCreateSimpleWindow") {
        // 9 args: Display*, Window, int, int, unsigned, unsigned, unsigned,
        // unsigned long, unsigned long. Arg 8 (background) is on the stack.
        uint64_t bg = 0;
        if (mem && cpu.sp) {
            mem->read(cpu.sp, &bg, sizeof(uint64_t));
        }
        cpu.regs[0] = proxy->XCreateSimpleWindow(
            cpu.regs[0], cpu.regs[1],
            static_cast<int>(cpu.regs[2]), static_cast<int>(cpu.regs[3]),
            static_cast<int>(cpu.regs[4]), static_cast<int>(cpu.regs[5]),
            static_cast<int>(cpu.regs[6]),
            static_cast<unsigned long>(cpu.regs[7]),
            static_cast<unsigned long>(bg));
        return 0;
    }
    if (sym_name == "XCreateWindow") {
        // 12 args: Display*, Window, int, int, unsigned, unsigned, unsigned,
        // int, unsigned long, Visual*, unsigned long, XSetWindowAttributes*.
        // Args 8-11 are on the stack.
        uint64_t stack_args[4] = {0};
        if (mem && cpu.sp) {
            mem->read(cpu.sp, stack_args, sizeof(stack_args));
        }
        cpu.regs[0] = proxy->XCreateWindow(
            cpu.regs[0], cpu.regs[1],
            static_cast<int>(cpu.regs[2]), static_cast<int>(cpu.regs[3]),
            static_cast<unsigned>(cpu.regs[4]), static_cast<unsigned>(cpu.regs[5]),
            static_cast<unsigned>(cpu.regs[6]),
            static_cast<int>(cpu.regs[7]),
            static_cast<unsigned long>(stack_args[0]),
            static_cast<uint64_t>(stack_args[1]),
            static_cast<unsigned long>(stack_args[2]),
            g2h(stack_args[3]));
        return 0;
    }
    if (sym_name == "XDestroyWindow") {
        cpu.regs[0] = proxy->XDestroyWindow(cpu.regs[0], cpu.regs[1]);
        return 0;
    }
    if (sym_name == "XMapWindow") {
        cpu.regs[0] = proxy->XMapWindow(cpu.regs[0], cpu.regs[1]);
        return 0;
    }
    if (sym_name == "XUnmapWindow") {
        cpu.regs[0] = proxy->XUnmapWindow(cpu.regs[0], cpu.regs[1]);
        return 0;
    }
    if (sym_name == "XFlush") {
        cpu.regs[0] = proxy->XFlush(cpu.regs[0]);
        return 0;
    }
    if (sym_name == "XSync") {
        cpu.regs[0] = proxy->XSync(cpu.regs[0], static_cast<int>(cpu.regs[1]));
        return 0;
    }
    if (sym_name == "XFillRectangle") {
        proxy->XFillRectangle(
            cpu.regs[0], cpu.regs[1],
            static_cast<unsigned long>(cpu.regs[2]),
            static_cast<int>(cpu.regs[3]), static_cast<int>(cpu.regs[4]),
            static_cast<unsigned>(cpu.regs[5]), static_cast<unsigned>(cpu.regs[6]));
        cpu.regs[0] = 1;
        return 0;
    }
    if (sym_name == "XDrawRectangle") {
        proxy->XDrawRectangle(
            cpu.regs[0], cpu.regs[1],
            static_cast<unsigned long>(cpu.regs[2]),
            static_cast<int>(cpu.regs[3]), static_cast<int>(cpu.regs[4]),
            static_cast<unsigned>(cpu.regs[5]), static_cast<unsigned>(cpu.regs[6]));
        cpu.regs[0] = 1;
        return 0;
    }
    if (sym_name == "XDrawLine") {
        proxy->XDrawLine(
            cpu.regs[0], cpu.regs[1],
            static_cast<unsigned long>(cpu.regs[2]),
            static_cast<int>(cpu.regs[3]), static_cast<int>(cpu.regs[4]),
            static_cast<int>(cpu.regs[5]), static_cast<int>(cpu.regs[6]));
        cpu.regs[0] = 1;
        return 0;
    }
    if (sym_name == "XDrawPoint") {
        proxy->XDrawPoint(
            cpu.regs[0], cpu.regs[1],
            static_cast<unsigned long>(cpu.regs[2]),
            static_cast<int>(cpu.regs[3]), static_cast<int>(cpu.regs[4]));
        cpu.regs[0] = 1;
        return 0;
    }
    if (sym_name == "XSetForeground") {
        cpu.regs[0] = proxy->XSetForeground(
            cpu.regs[0], static_cast<unsigned long>(cpu.regs[1]),
            static_cast<unsigned long>(cpu.regs[2]));
        return 0;
    }
    if (sym_name == "XSetBackground") {
        cpu.regs[0] = proxy->XSetBackground(
            cpu.regs[0], static_cast<unsigned long>(cpu.regs[1]),
            static_cast<unsigned long>(cpu.regs[2]));
        return 0;
    }
    if (sym_name == "XCreateGC") {
        // 4 args: Display*, Drawable, unsigned long, XGCValues* — all in
        // x0..x3 (the extra screen/visual proxy params are unused).
        cpu.regs[0] = proxy->XCreateGC(
            cpu.regs[0], static_cast<unsigned long>(cpu.regs[1]),
            static_cast<unsigned long>(cpu.regs[2]),
            g2h(cpu.regs[3]), 0, 0);
        return 0;
    }
    if (sym_name == "XFreeGC") {
        cpu.regs[0] = proxy->XFreeGC(cpu.regs[0], cpu.regs[1]);
        return 0;
    }
    if (sym_name == "XStoreName") {
        const char* name = g2h_str(cpu.regs[2]);
        cpu.regs[0] = proxy->XStoreName(cpu.regs[0], cpu.regs[1], name);
        return 0;
    }
    if (sym_name == "XGetWindowAttributes") {
        cpu.regs[0] = proxy->XGetWindowAttributes(
            cpu.regs[0], cpu.regs[1], g2h(cpu.regs[2]));
        return 0;
    }
    if (sym_name == "XSelectInput") {
        cpu.regs[0] = proxy->XSelectInput(
            cpu.regs[0], cpu.regs[1], static_cast<long>(cpu.regs[2]));
        return 0;
    }
    if (sym_name == "XInternAtom") {
        const char* name = g2h_str(cpu.regs[1]);
        cpu.regs[0] = proxy->XInternAtom(
            cpu.regs[0], name, static_cast<int>(cpu.regs[2]));
        return 0;
    }
    if (sym_name == "XSetWMProtocols") {
        // 4 args: Display*, Window, Atom*, int — all in x0..x3.
        cpu.regs[0] = proxy->XSetWMProtocols(
            cpu.regs[0], cpu.regs[1], g2h(cpu.regs[2]),
            static_cast<int>(cpu.regs[3]));
        return 0;
    }
    if (sym_name == "XGetAtomName") {
        unsigned long host_ptr = proxy->XGetAtomName(cpu.regs[0], static_cast<unsigned long>(cpu.regs[1]));
        const char* host_str = reinterpret_cast<const char*>(host_ptr);
        cpu.regs[0] = host_str ? impl_->cache_host_string_(host_str) : 0;
        return 0;
    }
    if (sym_name == "XPending") {
        cpu.regs[0] = proxy->XPending(cpu.regs[0]);
        return 0;
    }
    if (sym_name == "XNextEvent") {
        cpu.regs[0] = proxy->XNextEvent(cpu.regs[0], g2h(cpu.regs[1]));
        return 0;
    }
    if (sym_name == "XCheckMaskEvent") {
        cpu.regs[0] = proxy->XCheckMaskEvent(
            cpu.regs[0], static_cast<long>(cpu.regs[1]), g2h(cpu.regs[2]));
        return 0;
    }
    if (sym_name == "XEventsQueued") {
        cpu.regs[0] = proxy->XEventsQueued(
            cpu.regs[0], static_cast<int>(cpu.regs[1]));
        return 0;
    }
    if (sym_name == "XDisplayWidth") {
        cpu.regs[0] = proxy->XDisplayWidth(cpu.regs[0], static_cast<int>(cpu.regs[1]));
        return 0;
    }
    if (sym_name == "XDisplayHeight") {
        cpu.regs[0] = proxy->XDisplayHeight(cpu.regs[0], static_cast<int>(cpu.regs[1]));
        return 0;
    }
    if (sym_name == "XDisplayWidthMM") {
        cpu.regs[0] = proxy->XDisplayWidthMM(cpu.regs[0], static_cast<int>(cpu.regs[1]));
        return 0;
    }
    if (sym_name == "XDisplayHeightMM") {
        cpu.regs[0] = proxy->XDisplayHeightMM(cpu.regs[0], static_cast<int>(cpu.regs[1]));
        return 0;
    }
    if (sym_name == "DefaultRootWindow") {
        cpu.regs[0] = proxy->DefaultRootWindow(cpu.regs[0]);
        return 0;
    }
    if (sym_name == "BlackPixel") {
        cpu.regs[0] = proxy->BlackPixel(cpu.regs[0], static_cast<int>(cpu.regs[1]));
        return 0;
    }
    if (sym_name == "WhitePixel") {
        cpu.regs[0] = proxy->WhitePixel(cpu.regs[0], static_cast<int>(cpu.regs[1]));
        return 0;
    }
    if (sym_name == "XSetWindowBackground") {
        cpu.regs[0] = proxy->XSetWindowBackground(
            cpu.regs[0], cpu.regs[1], static_cast<unsigned long>(cpu.regs[2]));
        return 0;
    }
    if (sym_name == "XCreateColormap") {
        cpu.regs[0] = proxy->XCreateColormap(
            cpu.regs[0], cpu.regs[1], static_cast<uint64_t>(cpu.regs[2]),
            static_cast<int>(cpu.regs[3]));
        return 0;
    }
    if (sym_name == "XFreeColormap") {
        cpu.regs[0] = proxy->XFreeColormap(cpu.regs[0], cpu.regs[1]);
        return 0;
    }
    if (sym_name == "XAllocColor") {
        cpu.regs[0] = proxy->XAllocColor(
            cpu.regs[0], static_cast<unsigned long>(cpu.regs[1]), g2h(cpu.regs[2]));
        return 0;
    }
    if (sym_name == "XSetLineAttributes") {
        // 6 args: Display*, GC, unsigned, unsigned, int, int — x0..x5.
        cpu.regs[0] = proxy->XSetLineAttributes(
            cpu.regs[0], static_cast<unsigned long>(cpu.regs[1]),
            static_cast<unsigned>(cpu.regs[2]), static_cast<unsigned>(cpu.regs[3]),
            static_cast<int>(cpu.regs[4]), static_cast<int>(cpu.regs[5]));
        return 0;
    }
    if (sym_name == "XDrawString") {
        const char* str = g2h_str(cpu.regs[5]);
        proxy->XDrawString(
            cpu.regs[0], cpu.regs[1], static_cast<unsigned long>(cpu.regs[2]),
            static_cast<int>(cpu.regs[3]), static_cast<int>(cpu.regs[4]),
            str, static_cast<int>(cpu.regs[6]));
        cpu.regs[0] = 1;
        return 0;
    }
    if (sym_name == "XQueryPointer") {
        uint64_t mask_ptr = 0;
        if (mem && cpu.sp) {
            mem->read(cpu.sp, &mask_ptr, sizeof(uint64_t));
        }
        cpu.regs[0] = proxy->XQueryPointer(
            cpu.regs[0], cpu.regs[1], g2h(cpu.regs[2]), g2h(cpu.regs[3]),
            g2h(cpu.regs[4]), g2h(cpu.regs[5]), g2h(cpu.regs[6]), g2h(cpu.regs[7]),
            g2h(mask_ptr));
        return 0;
    }
    if (sym_name == "XWarpPointer") {
        uint64_t dest_y = 0;
        if (mem && cpu.sp) {
            mem->read(cpu.sp, &dest_y, sizeof(uint64_t));
        }
        cpu.regs[0] = proxy->XWarpPointer(
            cpu.regs[0], cpu.regs[1], cpu.regs[2],
            static_cast<int>(cpu.regs[3]), static_cast<int>(cpu.regs[4]),
            static_cast<int>(cpu.regs[5]), static_cast<int>(cpu.regs[6]),
            static_cast<int>(cpu.regs[7]), static_cast<int>(dest_y));
        return 0;
    }
    if (sym_name == "XBell") {
        cpu.regs[0] = proxy->XBell(cpu.regs[0], static_cast<int>(cpu.regs[1]));
        return 0;
    }
    if (sym_name == "XScreenCount") {
        cpu.regs[0] = proxy->XScreenCount(cpu.regs[0]);
        return 0;
    }
    if (sym_name == "XSetInputFocus") {
        cpu.regs[0] = proxy->XSetInputFocus(
            cpu.regs[0], static_cast<uint64_t>(cpu.regs[1]),
            static_cast<int>(cpu.regs[2]), static_cast<uint64_t>(cpu.regs[3]));
        return 0;
    }
    if (sym_name == "XGetInputFocus") {
        cpu.regs[0] = proxy->XGetInputFocus(
            cpu.regs[0], g2h(cpu.regs[1]), g2h(cpu.regs[2]));
        return 0;
    }
    if (sym_name == "XChangeProperty") {
        // 8 args: Display*, Window, Atom, Atom, int, int, const unsigned char*,
        // long — all in x0..x7 (data = x6, nelements = x7).
        cpu.regs[0] = proxy->XChangeProperty(
            cpu.regs[0], cpu.regs[1], static_cast<unsigned long>(cpu.regs[2]),
            static_cast<unsigned long>(cpu.regs[3]),
            static_cast<int>(cpu.regs[4]), static_cast<int>(cpu.regs[5]),
            g2h(cpu.regs[6]), static_cast<long>(cpu.regs[7]));
        return 0;
    }
    if (sym_name == "XDeleteProperty") {
        cpu.regs[0] = proxy->XDeleteProperty(
            cpu.regs[0], cpu.regs[1], static_cast<unsigned long>(cpu.regs[2]));
        return 0;
    }
    if (sym_name == "XCopyArea") {
        uint64_t stack_args[2] = {0};
        if (mem && cpu.sp) {
            mem->read(cpu.sp, stack_args, sizeof(stack_args));
        }
        cpu.regs[0] = proxy->XCopyArea(
            cpu.regs[0], cpu.regs[1], cpu.regs[2], static_cast<unsigned long>(cpu.regs[3]),
            static_cast<int>(cpu.regs[4]), static_cast<int>(cpu.regs[5]),
            static_cast<int>(cpu.regs[6]), static_cast<int>(cpu.regs[7]),
            static_cast<int>(stack_args[0]), static_cast<int>(stack_args[1]));
        return 0;
    }
    if (sym_name == "XCreatePixmap") {
        cpu.regs[0] = proxy->XCreatePixmap(
            cpu.regs[0], cpu.regs[1],
            static_cast<int>(cpu.regs[2]), static_cast<int>(cpu.regs[3]),
            static_cast<int>(cpu.regs[4]));
        return 0;
    }
    if (sym_name == "XFreePixmap") {
        cpu.regs[0] = proxy->XFreePixmap(cpu.regs[0], cpu.regs[1]);
        return 0;
    }
    if (sym_name == "XSetWindowBackgroundPixmap") {
        cpu.regs[0] = proxy->XSetWindowBackgroundPixmap(
            cpu.regs[0], cpu.regs[1], cpu.regs[2]);
        return 0;
    }
    if (sym_name == "XSetClipMask") {
        cpu.regs[0] = proxy->XSetClipMask(
            cpu.regs[0], static_cast<unsigned long>(cpu.regs[1]), cpu.regs[2]);
        return 0;
    }
    if (sym_name == "XSetClipOrigin") {
        cpu.regs[0] = proxy->XSetClipOrigin(
            cpu.regs[0], static_cast<unsigned long>(cpu.regs[1]),
            static_cast<int>(cpu.regs[2]), static_cast<int>(cpu.regs[3]));
        return 0;
    }
    if (sym_name == "XCopyGC") {
        // 4 args: Display*, GC, unsigned long, GC — all in x0..x3.
        cpu.regs[0] = proxy->XCopyGC(
            cpu.regs[0], static_cast<unsigned long>(cpu.regs[1]),
            static_cast<unsigned long>(cpu.regs[2]), cpu.regs[3]);
        return 0;
    }
    if (sym_name == "XChangeGC") {
        // 4 args: Display*, GC, unsigned long, XGCValues* — all in x0..x3.
        cpu.regs[0] = proxy->XChangeGC(
            cpu.regs[0], static_cast<unsigned long>(cpu.regs[1]),
            static_cast<unsigned long>(cpu.regs[2]), g2h(cpu.regs[3]));
        return 0;
    }
    if (sym_name == "XSetFunction") {
        cpu.regs[0] = proxy->XSetFunction(
            cpu.regs[0], static_cast<unsigned long>(cpu.regs[1]),
            static_cast<int>(cpu.regs[2]));
        return 0;
    }
    if (sym_name == "XSetDashes") {
        const char* dashes = g2h_str(cpu.regs[3]);
        cpu.regs[0] = proxy->XSetDashes(
            cpu.regs[0], static_cast<unsigned long>(cpu.regs[1]),
            static_cast<int>(cpu.regs[2]), dashes);
        return 0;
    }
    if (sym_name == "XFreeColors") {
        cpu.regs[0] = proxy->XFreeColors(
            cpu.regs[0], static_cast<unsigned long>(cpu.regs[1]),
            reinterpret_cast<const unsigned long*>(g2h(cpu.regs[2])), static_cast<int>(cpu.regs[3]),
            static_cast<unsigned long>(cpu.regs[4]));
        return 0;
    }
    if (sym_name == "XDrawImageString") {
        const char* str = g2h_str(cpu.regs[5]);
        proxy->XDrawImageString(
            cpu.regs[0], cpu.regs[1], static_cast<unsigned long>(cpu.regs[2]),
            static_cast<int>(cpu.regs[3]), static_cast<int>(cpu.regs[4]),
            str, static_cast<int>(cpu.regs[6]));
        cpu.regs[0] = 1;
        return 0;
    }
    // Wayland proxy methods
    if (sym_name == "wl_display_connect") {
        const char* name = g2h_str(cpu.regs[0]);
        cpu.regs[0] = proxy->wl_display_connect(name);
        return 0;
    }
    if (sym_name == "wl_display_disconnect") {
        proxy->wl_display_disconnect(cpu.regs[0]);
        cpu.regs[0] = 0;
        return 0;
    }
    if (sym_name == "wl_surface_create") {
        const char* interface = g2h_str(cpu.regs[1]);
        cpu.regs[0] = proxy->wl_surface_create(
            cpu.regs[0], interface, static_cast<uint32_t>(cpu.regs[2]));
        return 0;
    }
    if (sym_name == "wl_surface_commit") {
        proxy->wl_surface_commit(cpu.regs[0]);
        cpu.regs[0] = 0;
        return 0;
    }
    if (sym_name == "wl_surface_destroy") {
        proxy->wl_surface_destroy(cpu.regs[0]);
        cpu.regs[0] = 0;
        return 0;
    }
    if (sym_name == "wl_egl_window_create") {
        cpu.regs[0] = proxy->wl_egl_window_create(
            cpu.regs[0], static_cast<int>(cpu.regs[1]),
            static_cast<int>(cpu.regs[2]));
        return 0;
    }
    if (sym_name == "wl_egl_window_destroy") {
        cpu.regs[0] = proxy->wl_egl_window_destroy(cpu.regs[0]);
        return 0;
    }
    if (sym_name == "wl_egl_window_get_attached_size") {
        cpu.regs[0] = proxy->wl_egl_window_get_attached_size(
            cpu.regs[0], g2h(cpu.regs[1]), g2h(cpu.regs[2]));
        return 0;
    }
    if (sym_name == "wl_egl_window_resize") {
        cpu.regs[0] = proxy->wl_egl_window_resize(
            cpu.regs[0], static_cast<int>(cpu.regs[1]),
            static_cast<int>(cpu.regs[2]), static_cast<int>(cpu.regs[3]),
            static_cast<int>(cpu.regs[4]));
        return 0;
    }
    // ── XShm proxy ──────────────────────────────────────────────────
    if (sym_name == "XShmQueryExtension") {
        cpu.regs[0] = proxy->XShmQueryExtension(cpu.regs[0]);
        return 0;
    }
    if (sym_name == "XShmGetEventBase") {
        cpu.regs[0] = proxy->XShmGetEventBase(cpu.regs[0]);
        return 0;
    }
    if (sym_name == "XShmCreateImage") {
        cpu.regs[0] = proxy->XShmCreateImage(
            cpu.regs[0], cpu.regs[1], static_cast<unsigned>(cpu.regs[2]),
            static_cast<int>(cpu.regs[3]), g2h(cpu.regs[4]), g2h(cpu.regs[5]),
            static_cast<unsigned>(cpu.regs[6]), static_cast<unsigned>(cpu.regs[7]));
        return 0;
    }
    if (sym_name == "XShmAttach") {
        cpu.regs[0] = proxy->XShmAttach(cpu.regs[0], cpu.regs[1]);
        return 0;
    }
    if (sym_name == "XShmDetach") {
        cpu.regs[0] = proxy->XShmDetach(cpu.regs[0], cpu.regs[1]);
        return 0;
    }
    if (sym_name == "XShmPutImage") {
        // 11 args: args 8-10 (src_width, src_height, send_event) on stack.
        uint64_t stack_args[3] = {0};
        if (impl_->mem && cpu.sp) {
            impl_->mem->read(cpu.sp, stack_args, sizeof(stack_args));
        }
        cpu.regs[0] = proxy->XShmPutImage(
            cpu.regs[0], cpu.regs[1], cpu.regs[2], cpu.regs[3],
            static_cast<int>(cpu.regs[4]), static_cast<int>(cpu.regs[5]),
            static_cast<int>(cpu.regs[6]), static_cast<int>(cpu.regs[7]),
            static_cast<unsigned>(stack_args[0]),
            static_cast<unsigned>(stack_args[1]),
            static_cast<bool>(stack_args[2]));
        return 0;
    }
    if (sym_name == "XShmGetImage") {
        cpu.regs[0] = proxy->XShmGetImage(
            cpu.regs[0], cpu.regs[1], cpu.regs[2],
            static_cast<int>(cpu.regs[3]), static_cast<int>(cpu.regs[4]),
            static_cast<unsigned>(cpu.regs[5]), static_cast<unsigned>(cpu.regs[6]),
            static_cast<unsigned long>(cpu.regs[7]));
        return 0;
    }
    // ── GLX proxy ───────────────────────────────────────────────────
    if (sym_name == "glXChooseVisual") {
        cpu.regs[0] = proxy->glXChooseVisual(
            cpu.regs[0], static_cast<int>(cpu.regs[1]),
            reinterpret_cast<const int*>(g2h(cpu.regs[2])));
        return 0;
    }
    if (sym_name == "glXCreateContext") {
        cpu.regs[0] = proxy->glXCreateContext(
            cpu.regs[0], cpu.regs[1], cpu.regs[2], static_cast<int>(cpu.regs[3]));
        return 0;
    }
    if (sym_name == "glXDestroyContext") {
        cpu.regs[0] = proxy->glXDestroyContext(cpu.regs[0], cpu.regs[1]);
        return 0;
    }
    if (sym_name == "glXMakeCurrent") {
        cpu.regs[0] = proxy->glXMakeCurrent(
            cpu.regs[0], cpu.regs[1], cpu.regs[2]);
        return 0;
    }
    if (sym_name == "glXSwapBuffers") {
        proxy->glXSwapBuffers(cpu.regs[0], cpu.regs[1]);
        cpu.regs[0] = 0;
        return 0;
    }
    if (sym_name == "glXGetClientString") {
        const char* s = proxy->glXGetClientString(cpu.regs[0], static_cast<int>(cpu.regs[1]));
        cpu.regs[0] = s ? impl_->cache_host_string_(s) : 0;
        return 0;
    }
    if (sym_name == "glXQueryExtensionsString") {
        const char* s = proxy->glXQueryExtensionsString(cpu.regs[0], static_cast<int>(cpu.regs[1]));
        cpu.regs[0] = s ? impl_->cache_host_string_(s) : 0;
        return 0;
    }
    if (sym_name == "glXQueryServerString") {
        const char* s = proxy->glXQueryServerString(cpu.regs[0], static_cast<int>(cpu.regs[1]), static_cast<int>(cpu.regs[2]));
        cpu.regs[0] = s ? impl_->cache_host_string_(s) : 0;
        return 0;
    }
    if (sym_name == "glXGetFBConfigs") {
        int nelements = 0;
        uint64_t fbconfigs = proxy->glXGetFBConfigs(cpu.regs[0], static_cast<int>(cpu.regs[1]), &nelements);
        if (cpu.regs[2] && impl_->mem) {
            impl_->mem->write(cpu.regs[2], &nelements, sizeof(nelements));
        }
        cpu.regs[0] = fbconfigs;
        return 0;
    }
    if (sym_name == "glXGetFBConfigAttrib") {
        int value = 0;
        cpu.regs[0] = proxy->glXGetFBConfigAttrib(
            cpu.regs[0], cpu.regs[1], static_cast<int>(cpu.regs[2]),
            cpu.regs[3] ? &value : nullptr);
        if (cpu.regs[3] && impl_->mem) {
            impl_->mem->write(cpu.regs[3], &value, sizeof(value));
        }
        return 0;
    }
    if (sym_name == "glXCreateWindow") {
        cpu.regs[0] = proxy->glXCreateWindow(
            cpu.regs[0], cpu.regs[1], cpu.regs[2],
            reinterpret_cast<const int*>(g2h(cpu.regs[3])));
        return 0;
    }
    if (sym_name == "glXDestroyWindow") {
        cpu.regs[0] = proxy->glXDestroyWindow(cpu.regs[0], cpu.regs[1]);
        return 0;
    }
    if (sym_name == "glXCreatePbuffer") {
        cpu.regs[0] = proxy->glXCreatePbuffer(
            cpu.regs[0], cpu.regs[1],
            reinterpret_cast<const int*>(g2h(cpu.regs[2])));
        return 0;
    }
    if (sym_name == "glXDestroyPbuffer") {
        cpu.regs[0] = proxy->glXDestroyPbuffer(cpu.regs[0], cpu.regs[1]);
        return 0;
    }
    // ── XRandR proxy ────────────────────────────────────────────────
    if (sym_name == "XRRGetScreenResources") {
        cpu.regs[0] = proxy->XRRGetScreenResources(cpu.regs[0], cpu.regs[1]);
        return 0;
    }
    if (sym_name == "XRRGetScreenResourcesCurrent") {
        cpu.regs[0] = proxy->XRRGetScreenResourcesCurrent(cpu.regs[0], cpu.regs[1]);
        return 0;
    }
    if (sym_name == "XRRFreeScreenResources") {
        proxy->XRRFreeScreenResources(cpu.regs[0]);
        cpu.regs[0] = 0;
        return 0;
    }
    if (sym_name == "XRRGetCrtcInfo") {
        cpu.regs[0] = proxy->XRRGetCrtcInfo(cpu.regs[0], cpu.regs[1], cpu.regs[2]);
        return 0;
    }
    if (sym_name == "XRRFreeCrtcInfo") {
        proxy->XRRFreeCrtcInfo(cpu.regs[0]);
        cpu.regs[0] = 0;
        return 0;
    }
    if (sym_name == "XRRGetOutputInfo") {
        cpu.regs[0] = proxy->XRRGetOutputInfo(cpu.regs[0], cpu.regs[1], cpu.regs[2]);
        return 0;
    }
    if (sym_name == "XRRFreeOutputInfo") {
        proxy->XRRFreeOutputInfo(cpu.regs[0]);
        cpu.regs[0] = 0;
        return 0;
    }
    if (sym_name == "XRRSetCrtcConfig") {
        // 10 args: x0..x7 + 2 stack args (outputs, noutputs).
        uint64_t outputs = 0;
        int32_t noutputs = 0;
        if (impl_->mem && cpu.sp) {
            impl_->mem->read(cpu.sp, &outputs, sizeof(uint64_t));
            impl_->mem->read(cpu.sp + 8, &noutputs, sizeof(int32_t));
        }
        cpu.regs[0] = proxy->XRRSetCrtcConfig(
            cpu.regs[0], cpu.regs[1], cpu.regs[2], cpu.regs[3],
            static_cast<int>(cpu.regs[4]), static_cast<int>(cpu.regs[5]),
            cpu.regs[6], static_cast<unsigned>(cpu.regs[7]),
            outputs, noutputs);
        return 0;
    }
    if (sym_name == "XRRGetScreenSizeRange") {
        int min_w = 0, min_h = 0, max_w = 0, max_h = 0;
        cpu.regs[0] = proxy->XRRGetScreenSizeRange(
            cpu.regs[0], static_cast<int>(cpu.regs[1]),
            cpu.regs[2] ? &min_w : nullptr,
            cpu.regs[3] ? &min_h : nullptr,
            cpu.regs[4] ? &max_w : nullptr,
            cpu.regs[5] ? &max_h : nullptr);
        if (cpu.regs[2] && impl_->mem) impl_->mem->write(cpu.regs[2], &min_w, sizeof(min_w));
        if (cpu.regs[3] && impl_->mem) impl_->mem->write(cpu.regs[3], &min_h, sizeof(min_h));
        if (cpu.regs[4] && impl_->mem) impl_->mem->write(cpu.regs[4], &max_w, sizeof(max_w));
        if (cpu.regs[5] && impl_->mem) impl_->mem->write(cpu.regs[5], &max_h, sizeof(max_h));
        return 0;
    }
    // ── Xkb proxy ───────────────────────────────────────────────────
    if (sym_name == "XkbOpenDevice") {
        cpu.regs[0] = proxy->XkbOpenDevice(cpu.regs[0], static_cast<int>(cpu.regs[1]));
        return 0;
    }
    if (sym_name == "XkbGetMap") {
        cpu.regs[0] = proxy->XkbGetMap(cpu.regs[0], cpu.regs[1], static_cast<unsigned>(cpu.regs[2]));
        return 0;
    }
    if (sym_name == "XkbGetState") {
        cpu.regs[0] = proxy->XkbGetState(cpu.regs[0], cpu.regs[1], g2h(cpu.regs[2]));
        return 0;
    }
    if (sym_name == "XkbSetState") {
        cpu.regs[0] = proxy->XkbSetState(cpu.regs[0], cpu.regs[1], static_cast<unsigned>(cpu.regs[2]), g2h(cpu.regs[3]));
        return 0;
    }
    if (sym_name == "XkbSetAutoRepeatRate") {
        cpu.regs[0] = proxy->XkbSetAutoRepeatRate(
            cpu.regs[0], cpu.regs[1], static_cast<unsigned>(cpu.regs[2]),
            static_cast<unsigned>(cpu.regs[3]));
        return 0;
    }
    if (sym_name == "XkbGetAutoRepeatRate") {
        unsigned int delay = 0, interval = 0;
        cpu.regs[0] = proxy->XkbGetAutoRepeatRate(
            cpu.regs[0], cpu.regs[1],
            cpu.regs[2] ? &delay : nullptr,
            cpu.regs[3] ? &interval : nullptr);
        if (cpu.regs[2] && impl_->mem) impl_->mem->write(cpu.regs[2], &delay, sizeof(delay));
        if (cpu.regs[3] && impl_->mem) impl_->mem->write(cpu.regs[3], &interval, sizeof(interval));
        return 0;
    }
    if (sym_name == "XkbFreeKeyboard") {
        proxy->XkbFreeKeyboard(cpu.regs[0]);
        cpu.regs[0] = 0;
        return 0;
    }
    // ── Wayland host bridge: display lifecycle + proxy teardown ──
    // Forwards to the host compositor when wl_display_connect secured a
    // real connection; stub handles keep the old return-0 behavior, so
    // headless runs are unaffected.
    if (sym_name == "wl_display_get_fd") {
        cpu.regs[0] = static_cast<uint64_t>(
            static_cast<int64_t>(proxy->wl_display_get_fd(cpu.regs[0])));
        return 0;
    }
    if (sym_name == "wl_display_flush") {
        cpu.regs[0] = static_cast<uint64_t>(
            static_cast<int64_t>(proxy->wl_display_flush(cpu.regs[0])));
        return 0;
    }
    if (sym_name == "wl_display_dispatch") {
        cpu.regs[0] = static_cast<uint64_t>(
            static_cast<int64_t>(proxy->wl_display_dispatch(cpu.regs[0])));
        deliver_wl_events_(cpu);
        return 0;
    }
    if (sym_name == "wl_display_dispatch_pending") {
        cpu.regs[0] = static_cast<uint64_t>(
            static_cast<int64_t>(
                proxy->wl_display_dispatch_pending(cpu.regs[0])));
        deliver_wl_events_(cpu);
        return 0;
    }
    if (sym_name == "wl_display_roundtrip") {
        cpu.regs[0] = static_cast<uint64_t>(
            static_cast<int64_t>(proxy->wl_display_roundtrip(cpu.regs[0])));
        deliver_wl_events_(cpu);
        return 0;
    }
    if (sym_name == "wl_display_read_events") {
        cpu.regs[0] = static_cast<uint64_t>(
            static_cast<int64_t>(proxy->wl_display_read_events(cpu.regs[0])));
        return 0;
    }
    if (sym_name == "wl_display_prepare_read") {
        cpu.regs[0] = static_cast<uint64_t>(
            static_cast<int64_t>(
                proxy->wl_display_prepare_read(cpu.regs[0])));
        return 0;
    }
    if (sym_name == "wl_display_cancel_read") {
        cpu.regs[0] = static_cast<uint64_t>(
            static_cast<int64_t>(proxy->wl_display_cancel_read(cpu.regs[0])));
        return 0;
    }
    if (sym_name == "wl_proxy_destroy") {
        proxy->wl_proxy_destroy(cpu.regs[0]);
        cpu.regs[0] = 0;
        return 0;
    }
    if (sym_name == "wl_display_get_registry") {
        cpu.regs[0] = proxy->wl_display_get_registry(cpu.regs[0]);
        return 0;
    }
    if (sym_name == "wl_proxy_get_class") {
        cpu.regs[0] = proxy->wl_class_name(cpu.regs[0]);
        return 0;
    }
    // ── Wayland request marshalling (signature-driven) ─────────
    // Variadic guest layout: x0=proxy, w1=opcode, then x2=interface
    // (constructors) or first vararg, x3..x7, guest stack. The proxy
    // decodes per the opgen_wl table and forwards via the host array
    // forms; constructors map the new object back to a guest handle.
    if (sym_name == "wl_proxy_marshal") {
        // Plain marshal has no interface param: varargs start at x2.
        uint64_t regs[6] = {cpu.regs[2], cpu.regs[3], cpu.regs[4],
                            cpu.regs[5], cpu.regs[6], cpu.regs[7]};
        proxy->wl_marshal(cpu.regs[0],
                          static_cast<uint32_t>(cpu.regs[1]),
                          0 /*ifstruct*/, false, 0, regs, 6, cpu.sp);
        cpu.regs[0] = 0;
        return 0;
    }
    if (sym_name == "wl_proxy_marshal_constructor") {
        uint64_t regs[5] = {cpu.regs[3], cpu.regs[4], cpu.regs[5],
                            cpu.regs[6], cpu.regs[7]};
        cpu.regs[0] = proxy->wl_marshal(
            cpu.regs[0], static_cast<uint32_t>(cpu.regs[1]),
            cpu.regs[2], false, 0, regs, 5, cpu.sp);
        return 0;
    }
    if (sym_name == "wl_proxy_marshal_constructor_versioned") {
        uint64_t regs[4] = {cpu.regs[4], cpu.regs[5],
                            cpu.regs[6], cpu.regs[7]};
        cpu.regs[0] = proxy->wl_marshal(
            cpu.regs[0], static_cast<uint32_t>(cpu.regs[1]),
            cpu.regs[2], true, static_cast<uint32_t>(cpu.regs[3]),
            regs, 4, cpu.sp);
        return 0;
    }
    // ── Wayland listener registration ──────────────────────────
    // Stores the guest fn table (event_count entries) + data; installs
    // the host registry listener when applicable. Events queue during
    // dispatch and deliver afterwards (never re-entrant).
    if (sym_name == "wl_proxy_add_listener") {
        uint64_t proxy_h = cpu.regs[0];
        uint64_t listener = cpu.regs[1];
        uint64_t data = cpu.regs[2];
        std::string iface = proxy->wl_iface(proxy_h);
        size_t nev = iface.empty()
                         ? 0
                         : arm64emu::wl::event_count(iface.c_str());
        DisplayThunkImpl::WlListener lst;
        lst.data = data;
        // known iface: read exactly nev. unknown (xdg et al, nev==0):
        // read up to 16, stop at first fault. this keeps a future host
        // listener delivering instead of dropping for lack of a row.
        size_t want = (nev > 0 && nev <= 16) ? nev : 16;
        if (listener && impl_->mem) {
            for (size_t i = 0; i < want; i++) {
                uint64_t fn = 0;
                try {
                    impl_->mem->read(listener + i * 8, &fn, 8);
                } catch (...) { break; }
                lst.fns.push_back(fn);
            }
            if (nev == 0) {
                // trim trailing nulls from the blind read; keep at
                // least one entry so delivery has a target.
                while (!lst.fns.empty() && lst.fns.back() == 0)
                    lst.fns.pop_back();
            }
        }
        if (!lst.fns.empty()) impl_->wl_listeners_[proxy_h] = lst;
        if (iface == "wl_registry") {
            proxy->wl_registry_listen(proxy->wl_host(proxy_h));
        } else if (iface == "wl_pointer" || iface == "wl_keyboard" ||
                   iface == "wl_callback" || iface == "wl_touch" ||
                   iface == "wl_output" || iface == "wl_seat" ||
                   iface == "wl_data_offer" || iface == "xdg_wm_base" ||
                   iface == "xdg_surface" || iface == "xdg_toplevel") {
            proxy->wl_listen_for(proxy->wl_host(proxy_h), proxy_h, iface);
        }
        if (dbg().thunk_trace) {
            fprintf(stderr, "[thunk] wl_proxy_add_listener %s: %zu fns\n",
                    iface.c_str(), lst.fns.size());
        }
        cpu.regs[0] = 0;
        return 0;
    }
    // Unimplemented proxy symbol: one always-on note per symbol (then
    // silent). Without this, games needing the symbol misbehave with no
    // hint; `make proxy-check` lists the full backlog. Trace keeps the
    // per-call line for debugging.
    {
        static std::unordered_set<std::string> warned_;
        if (warned_.insert(sym_name).second) {
            fprintf(stderr, "[display-thunk] proxy: '%s' unimplemented, returning 0\n",
                    sym_name.c_str());
        }
    }
    if (trace) {
        fprintf(stderr, "[display-thunk] proxy_dispatch: unknown symbol '%s'\n",
                sym_name.c_str());
    }
    cpu.regs[0] = 0;
    return 0;
}
}  // namespace arm64emu
