/* test_wayland_bridge.c — Wayland host-bridge lifecycle check.
 *
 * Exercises the real-compositor forwarding in DisplayProxy: connect to
 * the host compositor, get the connection fd, flush, roundtrip, and
 * disconnect. Pre-bridge these were stub return-0s; now a live
 * compositor yields a real fd and clean roundtrip.
 *
 * No compositor (headless CI): connect still returns a stub handle but
 * get_fd fails -> exit 77 (skip) instead of failing.
 *
 * Build:
 *   make cross SRC=ctest_real/test_wayland_bridge.c OUT=ctest_real/test_wayland_bridge.elf
 * Run: ./bifrost-emu ctest_real/test_wayland_bridge.elf
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

typedef uint64_t (*wl_display_connect_t)(const char*);
typedef int      (*wl_display_fd_t)(uint64_t);
typedef int      (*wl_display_op_t)(uint64_t);
typedef void     (*wl_display_disconnect_t)(uint64_t);
typedef uint64_t (*wl_display_get_registry_t)(uint64_t);
typedef uint64_t (*wl_marshal_ctor_t)(uint64_t, uint32_t, void*, ...);
typedef int      (*wl_add_listener_t)(uint64_t, void*, uint64_t);
typedef void     (*wl_proxy_destroy_t)(uint64_t);

struct wl_iface {
    const char* name;
    int version;
};

static int n_globals = 0;
static int n_frame_done = 0;
static uint32_t comp_name = 0;
static uint32_t comp_version = 0;
static char comp_iface[64];
static uint32_t seat_name = 0;
static uint32_t seat_version = 0;

static void on_global(uint64_t data, uint64_t reg, uint32_t name,
                      const char* iface, uint32_t version) {
    (void)data; (void)reg;
    n_globals++;
    if (iface) {
        const char* p = iface;
        int i = 0;
        while (i < 63 && p[i]) { comp_iface[i] = p[i]; i++; }
        comp_iface[i] = 0;
        /* strcmp for wl_compositor / wl_seat without libc help. */
        const char* want = "wl_compositor";
        int j = 0;
        while (want[j] && comp_iface[j] == want[j]) j++;
        if (!want[j] && !comp_iface[j]) {
            comp_name = name;
            comp_version = version;
        }
        want = "wl_seat";
        j = 0;
        while (want[j] && comp_iface[j] == want[j]) j++;
        if (!want[j] && !comp_iface[j]) {
            seat_name = name;
            seat_version = version;
        }
    }
}

static void on_frame(uint64_t data, uint64_t cb, uint32_t time) {
    (void)data; (void)cb; (void)time;
    n_frame_done++;
}

static void on_remove(uint64_t data, uint64_t reg, uint32_t name) {
    (void)data; (void)reg; (void)name;
}

static void* reg_listener[2] = {(void*)on_global, (void*)on_remove};
static struct wl_iface compositor_iface = {"wl_compositor", 0};
static struct wl_iface surface_iface = {"wl_surface", 0};

static uint64_t bifrost_dlopen(const char* name, uint64_t flags) {
    register uint64_t x0 __asm__("x0") = (uint64_t)(uintptr_t)name;
    register uint64_t x1 __asm__("x1") = flags;
    register uint64_t x8 __asm__("x8") = 0x1002;
    __asm__ volatile ("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}

static uint64_t bifrost_dlsym(uint64_t handle, const char* name) {
    register uint64_t x0 __asm__("x0") = handle;
    register uint64_t x1 __asm__("x1") = (uint64_t)(uintptr_t)name;
    register uint64_t x8 __asm__("x8") = 0x1003;
    __asm__ volatile ("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}

#define LOAD(h, T, name) do { \
    name = (T)(uintptr_t)bifrost_dlsym(h, #name); \
    if (!name) { printf("FAIL: dlsym %s\n", #name); return 1; } \
} while (0)

int main(void) {
    printf("test_wayland_bridge: start\n");

    uint64_t hwl = bifrost_dlopen("libwayland-client.so.0", 1);
    if (!hwl) hwl = bifrost_dlopen("libwayland-client.so", 1);
    if (!hwl) {
        printf("test_wayland_bridge: SKIP (libwayland thunk unavailable)\n");
        return 77;
    }

    wl_display_connect_t wl_display_connect;
    wl_display_fd_t wl_display_get_fd;
    wl_display_op_t wl_display_flush;
    wl_display_op_t wl_display_roundtrip;
    wl_display_op_t wl_display_dispatch_pending;
    wl_display_disconnect_t wl_display_disconnect;
    wl_display_get_registry_t wl_display_get_registry;
    wl_marshal_ctor_t wl_proxy_marshal_constructor;
    wl_add_listener_t wl_proxy_add_listener;
    wl_proxy_destroy_t wl_proxy_destroy;
    const char* (*wl_proxy_get_class_fn)(uint64_t);
    void (*wl_surface_commit_fn)(uint64_t);
    LOAD(hwl, wl_display_connect_t, wl_display_connect);
    LOAD(hwl, wl_display_fd_t, wl_display_get_fd);
    LOAD(hwl, wl_display_op_t, wl_display_flush);
    LOAD(hwl, wl_display_op_t, wl_display_roundtrip);
    LOAD(hwl, wl_display_op_t, wl_display_dispatch_pending);
    LOAD(hwl, wl_display_disconnect_t, wl_display_disconnect);
    LOAD(hwl, wl_display_get_registry_t, wl_display_get_registry);
    LOAD(hwl, wl_marshal_ctor_t, wl_proxy_marshal_constructor);
    LOAD(hwl, wl_add_listener_t, wl_proxy_add_listener);
    LOAD(hwl, wl_proxy_destroy_t, wl_proxy_destroy);
    wl_proxy_get_class_fn =
        (const char* (*)(uint64_t))(uintptr_t)bifrost_dlsym(
            hwl, "wl_proxy_get_class");
    if (!wl_proxy_get_class_fn) {
        printf("FAIL: dlsym wl_proxy_get_class\n");
        return 1;
    }
    wl_surface_commit_fn =
        (void (*)(uint64_t))(uintptr_t)bifrost_dlsym(hwl, "wl_surface_commit");
    if (!wl_surface_commit_fn) {
        printf("FAIL: dlsym wl_surface_commit\n");
        return 1;
    }

    uint64_t disp = wl_display_connect(0);
    if (!disp) {
        printf("test_wayland_bridge: SKIP (no display)\n");
        return 77;
    }
    int fd = wl_display_get_fd(disp);
    if (fd < 0) {
        /* Stub handle (headless): nothing real to talk to. */
        printf("test_wayland_bridge: SKIP (no compositor)\n");
        wl_display_disconnect(disp);
        return 77;
    }
    printf("test_wayland_bridge: connected fd=%d\n", fd);

    if (wl_display_flush(disp) < 0) {
        printf("FAIL: flush failed\n");
        return 1;
    }
    if (wl_display_roundtrip(disp) < 0) {
        printf("FAIL: roundtrip failed\n");
        return 1;
    }
    if (wl_display_dispatch_pending(disp) < 0) {
        printf("FAIL: dispatch_pending failed\n");
        return 1;
    }

    /* Registry dialog: get_registry, listen, roundtrip (globals arrive
     * as guest callbacks), bind wl_compositor, create a surface. */
    uint64_t reg = wl_display_get_registry(disp);
    if (!reg) {
        printf("FAIL: get_registry returned 0\n");
        return 1;
    }
    if (wl_proxy_add_listener(reg, reg_listener, 0) != 0) {
        printf("FAIL: add_listener failed\n");
        return 1;
    }
    if (wl_display_roundtrip(disp) < 0) {
        printf("FAIL: registry roundtrip failed\n");
        return 1;
    }
    printf("test_wayland_bridge: globals=%d\n", n_globals);
    if (n_globals <= 0) {
        printf("FAIL: no globals delivered\n");
        return 1;
    }
    if (!comp_name) {
        printf("FAIL: no wl_compositor advertised\n");
        return 1;
    }
    compositor_iface.version = (int)comp_version;
    uint64_t comp = wl_proxy_marshal_constructor(reg, 0, &compositor_iface,
                                                 comp_name, comp_version);
    if (!comp) {
        printf("FAIL: bind wl_compositor failed\n");
        return 1;
    }
    surface_iface.version = 1;
    uint64_t surf = wl_proxy_marshal_constructor(comp, 0, &surface_iface);
    if (!surf) {
        printf("FAIL: create_surface failed\n");
        return 1;
    }
    /* Class introspection on the new surface. */
    const char* cls = wl_proxy_get_class_fn(surf);
    if (!cls) {
        printf("FAIL: get_class returned null\n");
        return 1;
    }
    {
        char buf[32];
        int i = 0;
        while (i < 31 && cls[i]) { buf[i] = cls[i]; i++; }
        buf[i] = 0;
        printf("test_wayland_bridge: surface class=%s\n", buf);
        /* strcmp wl_surface. */
        const char* want = "wl_surface";
        int j = 0;
        while (want[j] && buf[j] == want[j]) j++;
        if (want[j] || buf[j]) {
            printf("FAIL: class is %s, want wl_surface\n", buf);
            return 1;
        }
    }
    /* Seat: bind, take pointer + keyboard, listen, roundtrip. */
    if (seat_name) {
        struct wl_iface seat_iface = {"wl_seat", 0};
        seat_iface.version = (int)seat_version;
        uint64_t seat = wl_proxy_marshal_constructor(
            reg, 0, &seat_iface, seat_name, seat_version);
        if (!seat) {
            printf("FAIL: bind wl_seat failed\n");
            return 1;
        }
        struct wl_iface ptr_iface = {"wl_pointer", 0};
        struct wl_iface kb_iface = {"wl_keyboard", 0};
        uint64_t ptr = wl_proxy_marshal_constructor(seat, 0, &ptr_iface);
        uint64_t kb = wl_proxy_marshal_constructor(seat, 1, &kb_iface);
        if (!ptr || !kb) {
            printf("FAIL: seat children failed\n");
            return 1;
        }
        static void* ptr_listener[9] = {0};
        static void* kb_listener[6] = {0};
        if (wl_proxy_add_listener(ptr, ptr_listener, 0) != 0 ||
            wl_proxy_add_listener(kb, kb_listener, 0) != 0) {
            printf("FAIL: seat add_listener failed\n");
            return 1;
        }
        if (wl_display_roundtrip(disp) < 0) {
            printf("FAIL: seat roundtrip failed\n");
            return 1;
        }
        printf("test_wayland_bridge: seat ok\n");
        wl_proxy_destroy(ptr);
        wl_proxy_destroy(kb);
        wl_proxy_destroy(seat);
    } else {
        printf("test_wayland_bridge: SKIP seat (not advertised)\n");
    }
    /* Frame callback: request, commit, roundtrip, expect done. */
    {
        struct wl_iface cb_iface = {"wl_callback", 0};
        /* frame is op 3 on wl_surface, returns wl_callback. */
        uint64_t cb = wl_proxy_marshal_constructor(surf, 3, &cb_iface);
        if (!cb) {
            printf("FAIL: surface.frame failed\n");
            return 1;
        }
        static void* cb_listener[1] = {(void*)on_frame};
        if (wl_proxy_add_listener(cb, cb_listener, 0) != 0) {
            printf("FAIL: callback add_listener failed\n");
            return 1;
        }
        wl_surface_commit_fn(surf);
        if (wl_display_roundtrip(disp) < 0) {
            printf("FAIL: frame roundtrip failed\n");
            return 1;
        }
        if (!n_frame_done) {
            printf("test_wayland_bridge: SKIP frame (no repaint)\n");
        } else {
            printf("test_wayland_bridge: frame done\n");
        }
        wl_proxy_destroy(cb);
    }
    wl_proxy_destroy(surf);
    wl_proxy_destroy(comp);
    wl_proxy_destroy(reg);
    wl_display_disconnect(disp);
    printf("test_wayland_bridge: ALL PASS\n");
    return 0;
}
