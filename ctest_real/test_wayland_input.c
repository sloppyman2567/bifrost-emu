/* test_wayland_input.c — Wayland input/output event payload check.
 *
 * Exercises the listener delivery path with REAL non-null callbacks and
 * asserts the decoded payloads, not just delivery:
 *   seat: capabilities event must arrive with nonzero caps.
 *   keyboard: keymap event must arrive with format 1 (xkb v1), a valid
 *     fd, and a size>0 whose mmap starts with "xkb_keymap". This is the
 *     host->guest fd publisher path (dup + HostNode + fds_.allocate).
 *     enter/leave/key/modifiers/repeat_info are recorded when the
 *     compositor sends them (needs focus/keys, so conditional).
 *   output: geometry (make/model strings) + mode (w/h > 0) are
 *     mandatory; scale/name/description are conditional on version.
 *   pointer: motion/button/axis listeners are installed with real fns
 *     and roundtripped (proves the 11-slot struct + dispatch shape);
 *     motion values need live input so they are recorded-only.
 *
 * Headless (no compositor): exit 77 like test_wayland_bridge.
 *
 * Build:
 *   make cross SRC=ctest_real/test_wayland_input.c OUT=ctest_real/test_wayland_input.elf
 * Run: ./bifrost-emu ctest_real/test_wayland_input.elf
 * Interactive (needs a live compositor + you): window maps, then
 *   ./bifrost-emu ctest_real/test_wayland_input.elf interactive
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

typedef uint64_t (*wl_display_connect_t)(const char*);
typedef int      (*wl_display_fd_t)(uint64_t);
typedef int      (*wl_display_op_t)(uint64_t);
typedef void     (*wl_display_disconnect_t)(uint64_t);
typedef uint64_t (*wl_display_get_registry_t)(uint64_t);
typedef uint64_t (*wl_marshal_ctor_t)(uint64_t, uint32_t, void*, ...);
typedef uint64_t (*wl_marshal_t)(uint64_t, uint32_t, ...);
typedef int      (*wl_add_listener_t)(uint64_t, void*, uint64_t);
typedef void     (*wl_proxy_destroy_t)(uint64_t);
typedef void     (*wl_surface_commit_t)(uint64_t);

struct wl_iface {
    const char* name;
    int version;
};

/* collected registry globals */
static uint32_t seat_name = 0, seat_version = 0;
static uint32_t out_name = 0, out_version = 0;
static uint32_t comp_name = 0, comp_version = 0;
static uint32_t shm_name = 0, shm_version = 0;
static uint32_t xdg_name = 0, xdg_version = 0;

/* seat */
static int got_caps = 0;
static uint32_t seat_caps = 0;
static int got_seat_name = 0;
static char seat_str[64];

/* keyboard: keymap (mandatory) */
static int got_keymap = 0;
static uint32_t km_format = 0;
static int km_fd = -1;
static uint32_t km_size = 0;
/* keyboard: conditional */
static int got_kb_enter = 0, got_kb_leave = 0, got_key = 0;
static int got_mods = 0, got_repeat = 0;
static uint32_t kb_serial = 0, key_code = 0, key_state = 0;
static uint32_t mod_depressed = 0, mod_latched = 0, mod_locked = 0, mod_group = 0;
static int32_t rep_rate = 0, rep_delay = 0;
static uint32_t enter_nkeys = 0;
static uint32_t enter_key0 = 0;

/* pointer: recorded-only (needs live input) */
static int n_ptr_enter = 0, n_ptr_motion = 0, n_ptr_button = 0, n_ptr_axis = 0;
static uint32_t ptr_serial = 0;
static int32_t ptr_x = 0, ptr_y = 0;
static uint32_t btn_button = 0, btn_state = 0;

/* output */
static int got_geometry = 0, got_mode = 0;
static int got_scale = 0, got_out_name = 0, got_out_desc = 0;
static int32_t out_w = 0, out_h = 0;
static int32_t out_scale = 0;
static char out_make[64], out_model[64], out_nm[64];

static void copy_str(char* dst, size_t cap, const char* src) {
    size_t i = 0;
    if (src) {
        while (i + 1 < cap && src[i]) { dst[i] = src[i]; i++; }
    }
    dst[i] = 0;
}

static void on_global(uint64_t data, uint64_t reg, uint32_t name,
                      const char* iface, uint32_t version) {
    (void)data; (void)reg;
    if (!iface) return;
    if (!strcmp(iface, "wl_seat")) { seat_name = name; seat_version = version; }
    if (!strcmp(iface, "wl_output") && !out_name) {
        out_name = name; out_version = version;
    }
    if (!strcmp(iface, "wl_compositor") && !comp_name) {
        comp_name = name; comp_version = version;
    }
    if (!strcmp(iface, "wl_shm") && !shm_name) {
        shm_name = name; shm_version = version;
    }
    if (!strcmp(iface, "xdg_wm_base") && !xdg_name) {
        xdg_name = name; xdg_version = version;
    }
}
static void on_remove(uint64_t data, uint64_t reg, uint32_t name) {
    (void)data; (void)reg; (void)name;
}

/* seat listener */
static void on_caps(uint64_t data, uint64_t seat, uint32_t caps) {
    (void)data; (void)seat;
    got_caps = 1; seat_caps = caps;
}
static void on_seat_name(uint64_t data, uint64_t seat, const char* name) {
    (void)data; (void)seat;
    got_seat_name = 1; copy_str(seat_str, sizeof(seat_str), name);
}

/* keyboard listener */
static void on_keymap(uint64_t data, uint64_t kb, uint32_t format,
                      int32_t fd, uint32_t size) {
    (void)data; (void)kb;
    got_keymap = 1; km_format = format; km_fd = fd; km_size = size;
}
static void on_kb_enter(uint64_t data, uint64_t kb, uint32_t serial,
                        uint64_t surf, const void* keys) {
    (void)data; (void)kb; (void)surf;
    got_kb_enter = 1; kb_serial = serial;
    if (keys) {
        /* struct wl_array { size_t size, alloc; void *data; } */
        uint64_t sz = 0, datap = 0;
        memcpy(&sz, keys, 8);
        memcpy(&datap, (const char*)keys + 16, 8);
        if (sz > 512) sz = 512;
        enter_nkeys = (uint32_t)(sz / 4);
        if (sz >= 4 && datap) memcpy(&enter_key0, (const void*)(uintptr_t)datap, 4);
    }
}
static void on_kb_leave(uint64_t data, uint64_t kb, uint32_t serial,
                        uint64_t surf) {
    (void)data; (void)kb; (void)surf;
    got_kb_leave = 1; kb_serial = serial;
}
static void on_key(uint64_t data, uint64_t kb, uint32_t serial, uint32_t time,
                   uint32_t key, uint32_t state) {
    (void)data; (void)kb; (void)serial; (void)time;
    got_key = 1; key_code = key; key_state = state;
}
static void on_mods(uint64_t data, uint64_t kb, uint32_t serial,
                    uint32_t dep, uint32_t lat, uint32_t lock, uint32_t grp) {
    (void)data; (void)kb; (void)serial;
    got_mods = 1;
    mod_depressed = dep; mod_latched = lat; mod_locked = lock; mod_group = grp;
}
static void on_repeat(uint64_t data, uint64_t kb, int32_t rate, int32_t delay) {
    (void)data; (void)kb;
    got_repeat = 1; rep_rate = rate; rep_delay = delay;
}

/* pointer listener (recorded-only) */
static void on_ptr_enter(uint64_t data, uint64_t ptr, uint32_t serial,
                         uint64_t surf, int32_t x, int32_t y) {
    (void)data; (void)ptr; (void)surf;
    n_ptr_enter++; ptr_serial = serial; ptr_x = x; ptr_y = y;
}
static void on_ptr_motion(uint64_t data, uint64_t ptr, uint32_t time,
                          int32_t x, int32_t y) {
    (void)data; (void)ptr; (void)time;
    n_ptr_motion++; ptr_x = x; ptr_y = y;
}
static void on_ptr_button(uint64_t data, uint64_t ptr, uint32_t serial,
                          uint32_t time, uint32_t button, uint32_t state) {
    (void)data; (void)ptr; (void)serial; (void)time;
    n_ptr_button++; btn_button = button; btn_state = state;
}
static void on_ptr_axis(uint64_t data, uint64_t ptr, uint32_t time,
                        uint32_t axis, int32_t value) {
    (void)data; (void)ptr; (void)time;
    n_ptr_axis++;
    (void)axis; (void)value;
}

/* output listener */
static void on_geometry(uint64_t data, uint64_t out, int32_t x, int32_t y,
                        int32_t pw, int32_t ph, int32_t sub,
                        const char* make, const char* model, int32_t tr) {
    (void)data; (void)out; (void)x; (void)y;
    (void)pw; (void)ph; (void)sub; (void)tr;
    got_geometry = 1;
    copy_str(out_make, sizeof(out_make), make);
    copy_str(out_model, sizeof(out_model), model);
}
static void on_mode(uint64_t data, uint64_t out, uint32_t flags,
                    int32_t w, int32_t h, int32_t refresh) {
    (void)data; (void)out; (void)flags; (void)refresh;
    got_mode = 1; out_w = w; out_h = h;
}
static void on_done(uint64_t data, uint64_t out) {
    (void)data; (void)out;
}
static void on_out_scale(uint64_t data, uint64_t out, int32_t factor) {
    (void)data; (void)out;
    got_scale = 1; out_scale = factor;
}
static void on_out_name(uint64_t data, uint64_t out, const char* name) {
    (void)data; (void)out;
    got_out_name = 1; copy_str(out_nm, sizeof(out_nm), name);
}
static void on_out_desc(uint64_t data, uint64_t out, const char* desc) {
    (void)data; (void)out; (void)desc;
    got_out_desc = 1;
}

/* xdg_shell listener state */
static int got_ping = 0;
static uint32_t ping_serial = 0;
static int got_xdg_configure = 0;
static uint32_t xdg_serial = 0;
static int got_top_configure = 0, got_top_close = 0;
static int32_t top_w = 0, top_h = 0;
static uint32_t top_nstates = 0;

static void on_ping(uint64_t data, uint64_t base, uint32_t serial) {
    (void)data; (void)base;
    got_ping = 1; ping_serial = serial;
}
static void on_xdg_configure(uint64_t data, uint64_t surf, uint32_t serial) {
    (void)data; (void)surf;
    got_xdg_configure = 1; xdg_serial = serial;
}
static void on_top_configure(uint64_t data, uint64_t top, int32_t w, int32_t h,
                             const void* states) {
    (void)data; (void)top;
    got_top_configure = 1; top_w = w; top_h = h;
    if (states) {
        uint64_t sz = 0;
        memcpy(&sz, states, 8);
        if (sz > 1024) sz = 1024;
        top_nstates = (uint32_t)sz;
    }
}
static void on_top_close(uint64_t data, uint64_t top) {
    (void)data; (void)top;
    got_top_close = 1;
}

static void* reg_listener[2] = {(void*)on_global, (void*)on_remove};
static void* seat_listener[2] = {(void*)on_caps, (void*)on_seat_name};
static void* kb_listener[6] = {(void*)on_keymap, (void*)on_kb_enter,
                                (void*)on_kb_leave, (void*)on_key,
                                (void*)on_mods, (void*)on_repeat};
static void* ptr_listener[11] = {(void*)on_ptr_enter, NULL, (void*)on_ptr_motion,
                                 (void*)on_ptr_button, (void*)on_ptr_axis,
                                 NULL, NULL, NULL, NULL, NULL, NULL};
static void* out_listener[6] = {(void*)on_geometry, (void*)on_mode,
                                (void*)on_done, (void*)on_out_scale,
                                (void*)on_out_name, (void*)on_out_desc};
static void* xdg_base_listener[1] = {(void*)on_ping};
static void* xdg_surf_listener[1] = {(void*)on_xdg_configure};
static void* xdg_top_listener[4] = {(void*)on_top_configure,
                                    (void*)on_top_close, NULL, NULL};

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

#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); return 1; } \
} while (0)

int main(int argc, char** argv) {
    int interactive = argc > 1 && !strcmp(argv[1], "interactive");
    printf("test_wayland_input: start%s\n", interactive ? " (interactive)" : "");

    uint64_t hwl = bifrost_dlopen("libwayland-client.so.0", 1);
    if (!hwl) hwl = bifrost_dlopen("libwayland-client.so", 1);
    if (!hwl) {
        printf("test_wayland_input: SKIP (libwayland thunk unavailable)\n");
        return 77;
    }

    wl_display_connect_t wl_display_connect;
    wl_display_fd_t wl_display_get_fd;
    wl_display_op_t wl_display_roundtrip;
    wl_display_op_t wl_display_dispatch_pending;
    wl_display_op_t wl_display_flush;
    wl_display_op_t wl_display_prepare_read;
    wl_display_op_t wl_display_read_events;
    wl_display_op_t wl_display_cancel_read;
    wl_display_disconnect_t wl_display_disconnect;
    wl_display_get_registry_t wl_display_get_registry;
    wl_marshal_ctor_t wl_proxy_marshal_constructor;
    wl_marshal_t wl_proxy_marshal;
    wl_add_listener_t wl_proxy_add_listener;
    wl_proxy_destroy_t wl_proxy_destroy;
    wl_surface_commit_t wl_surface_commit;
    LOAD(hwl, wl_display_connect_t, wl_display_connect);
    LOAD(hwl, wl_display_fd_t, wl_display_get_fd);
    LOAD(hwl, wl_display_op_t, wl_display_roundtrip);
    LOAD(hwl, wl_display_op_t, wl_display_dispatch_pending);
    LOAD(hwl, wl_display_op_t, wl_display_flush);
    LOAD(hwl, wl_display_op_t, wl_display_prepare_read);
    LOAD(hwl, wl_display_op_t, wl_display_read_events);
    LOAD(hwl, wl_display_op_t, wl_display_cancel_read);
    LOAD(hwl, wl_display_disconnect_t, wl_display_disconnect);
    LOAD(hwl, wl_display_get_registry_t, wl_display_get_registry);
    LOAD(hwl, wl_marshal_ctor_t, wl_proxy_marshal_constructor);
    LOAD(hwl, wl_marshal_t, wl_proxy_marshal);
    LOAD(hwl, wl_add_listener_t, wl_proxy_add_listener);
    LOAD(hwl, wl_proxy_destroy_t, wl_proxy_destroy);
    LOAD(hwl, wl_surface_commit_t, wl_surface_commit);

    uint64_t disp = wl_display_connect(0);
    if (!disp) {
        printf("test_wayland_input: SKIP (no display)\n");
        return 77;
    }
    if (wl_display_get_fd(disp) < 0) {
        printf("test_wayland_input: SKIP (no compositor)\n");
        wl_display_disconnect(disp);
        return 77;
    }

    uint64_t reg = wl_display_get_registry(disp);
    CHECK(reg, "get_registry returned 0");
    CHECK(wl_proxy_add_listener(reg, reg_listener, 0) == 0,
          "registry add_listener failed");
    CHECK(wl_display_roundtrip(disp) >= 0, "registry roundtrip failed");
    CHECK(seat_name, "no wl_seat advertised");
    printf("test_wayland_input: seat=%u v%u output=%u v%u\n",
           seat_name, seat_version, out_name, out_version);

    /* seat caps + name */
    struct wl_iface seat_iface = {"wl_seat", 0};
    seat_iface.version = (int)seat_version;
    uint64_t seat = wl_proxy_marshal_constructor(reg, 0, &seat_iface,
                                                 seat_name, seat_version);
    CHECK(seat, "bind wl_seat failed");
    CHECK(wl_proxy_add_listener(seat, seat_listener, 0) == 0,
          "seat add_listener failed");

    /* keyboard + pointer with real listeners */
    struct wl_iface kb_iface = {"wl_keyboard", 0};
    struct wl_iface ptr_iface = {"wl_pointer", 0};
    uint64_t kb = wl_proxy_marshal_constructor(seat, 1, &kb_iface);
    uint64_t ptr = wl_proxy_marshal_constructor(seat, 0, &ptr_iface);
    CHECK(kb && ptr, "seat children failed");
    CHECK(wl_proxy_add_listener(kb, kb_listener, 0) == 0,
          "keyboard add_listener failed");
    CHECK(wl_proxy_add_listener(ptr, ptr_listener, 0) == 0,
          "pointer add_listener failed");
    CHECK(wl_display_roundtrip(disp) >= 0, "seat roundtrip failed");

    /* seat caps: bit0 pointer, bit1 keyboard, bit2 touch */
    CHECK(got_caps, "no seat capabilities event");
    printf("test_wayland_input: caps=0x%x%s\n", seat_caps,
           got_seat_name ? seat_str : "");
    CHECK(seat_caps != 0, "seat caps are zero (caps=0x%x)", seat_caps);

    /* keymap: the fd publisher path */
    CHECK(got_keymap, "no keyboard keymap event");
    printf("test_wayland_input: keymap fmt=%u fd=%d size=%u\n",
           km_format, km_fd, km_size);
    CHECK(km_format == 1, "keymap format %u, want 1 (xkb v1)", km_format);
    CHECK(km_size > 0, "keymap size is zero");
    CHECK(km_fd >= 0, "keymap fd invalid (%d)", km_fd);
    {
        void* map = mmap(0, km_size, PROT_READ, MAP_PRIVATE, km_fd, 0);
        CHECK(map != MAP_FAILED, "mmap of keymap fd failed");
        CHECK(!memcmp(map, "xkb_keymap", 10),
              "keymap content mismatch (no xkb_keymap magic)");
        printf("test_wayland_input: keymap content ok (%u bytes)\n", km_size);
        munmap(map, km_size);
        close(km_fd);
    }
    printf("test_wayland_input: kb enter=%d leave=%d key=%d mods=%d repeat=%d\n",
           got_kb_enter, got_kb_leave, got_key, got_mods, got_repeat);
    if (got_repeat)
        printf("test_wayland_input: repeat rate=%d delay=%d\n",
               rep_rate, rep_delay);
    if (got_kb_enter)
        printf("test_wayland_input: enter serial=%u nkeys=%u key0=%u\n",
               kb_serial, enter_nkeys, enter_key0);
    if (got_mods)
        printf("test_wayland_input: mods dep=%u lat=%u lock=%u grp=%u\n",
               mod_depressed, mod_latched, mod_locked, mod_group);

    /* output geometry + mode */
    if (out_name) {
        struct wl_iface oiface = {"wl_output", 0};
        oiface.version = (int)out_version;
        uint64_t out = wl_proxy_marshal_constructor(reg, 0, &oiface,
                                                    out_name, out_version);
        CHECK(out, "bind wl_output failed");
        CHECK(wl_proxy_add_listener(out, out_listener, 0) == 0,
              "output add_listener failed");
        CHECK(wl_display_roundtrip(disp) >= 0, "output roundtrip failed");
        CHECK(got_geometry, "no output geometry event");
        CHECK(got_mode, "no output mode event");
        printf("test_wayland_input: output %dx%d make=%s model=%s\n",
               out_w, out_h, out_make, out_model);
        CHECK(out_w > 0 && out_h > 0,
              "output mode bogus (%dx%d)", out_w, out_h);
        CHECK(out_make[0] && out_model[0],
              "output geometry strings empty");
        if (got_scale) {
            printf("test_wayland_input: scale=%d name=%s desc=%d\n",
                   out_scale, out_nm, got_out_desc);
            CHECK(out_scale >= 1, "output scale %d < 1", out_scale);
        }
        wl_proxy_destroy(out);
    } else {
        printf("test_wayland_input: SKIP output (not advertised)\n");
    }

    printf("test_wayland_input: pointer enter=%d motion=%d button=%d axis=%d\n",
           n_ptr_enter, n_ptr_motion, n_ptr_button, n_ptr_axis);

    /* xdg_shell: map a real toplevel so input has somewhere to go.
     * needs wl_compositor + wl_shm + xdg_wm_base; skipped headless. */
    uint64_t xdg_top = 0, xdg_surf = 0, xdg_base = 0;
    uint64_t comp = 0, surf = 0, shm = 0, pool = 0, buf = 0;
    if (comp_name && shm_name && xdg_name) {
        struct wl_iface comp_iface = {"wl_compositor", 0};
        comp_iface.version = (int)comp_version;
        comp = wl_proxy_marshal_constructor(reg, 0, &comp_iface,
                                            comp_name, comp_version);
        CHECK(comp, "bind wl_compositor failed");
        struct wl_iface surf_iface = {"wl_surface", 0};
        surf_iface.version = 1;
        surf = wl_proxy_marshal_constructor(comp, 0, &surf_iface);
        CHECK(surf, "create_surface failed");

        /* shm buffer: 64x64 ARGB so the window is really mapped */
        struct wl_iface shm_iface = {"wl_shm", 0};
        shm_iface.version = (int)shm_version;
        shm = wl_proxy_marshal_constructor(reg, 0, &shm_iface,
                                           shm_name, shm_version);
        CHECK(shm, "bind wl_shm failed");
        int shm_fd = memfd_create("wlshm", 0);
        CHECK(shm_fd >= 0, "memfd_create failed");
        CHECK(ftruncate(shm_fd, 640 * 480 * 4) == 0, "ftruncate failed");
        {
            /* NOTE: guest mmap of a file fd is preload-only (writes
             * stay in emulator pages), so fill via pwrite instead —
             * otherwise the pool file stays zeros (transparent). */
            uint32_t* px = malloc(640 * 480 * 4);
            CHECK(px, "malloc pixels failed");
            for (int i = 0; i < 640 * 480; i++) px[i] = 0xFFFF0000u;
            ssize_t w = pwrite(shm_fd, px, 640 * 480 * 4, 0);
            free(px);
            CHECK(w == 640 * 480 * 4, "pwrite pixels failed");
        }
        struct wl_iface pool_iface = {"wl_shm_pool", 0};
        pool_iface.version = 1;
        pool = wl_proxy_marshal_constructor(shm, 0, &pool_iface,
                                            shm_fd, 640 * 480 * 4);
        CHECK(pool, "create_pool failed");
        struct wl_iface buf_iface = {"wl_buffer", 0};
        buf_iface.version = 1;
        buf = wl_proxy_marshal_constructor(pool, 0, &buf_iface,
                                           0, 640, 480, 640 * 4, 0);
        CHECK(buf, "create_buffer failed");
        close(shm_fd);

        /* xdg_wm_base at v1, then surface + toplevel */
        struct wl_iface xdg_iface = {"xdg_wm_base", 1};
        xdg_base = wl_proxy_marshal_constructor(reg, 0, &xdg_iface,
                                                xdg_name, 1);
        CHECK(xdg_base, "bind xdg_wm_base failed");
        CHECK(wl_proxy_add_listener(xdg_base, xdg_base_listener, 0) == 0,
              "xdg base add_listener failed");
        struct wl_iface xs_iface = {"xdg_surface", 0};
        xdg_surf = wl_proxy_marshal_constructor(xdg_base, 2, &xs_iface, surf);
        CHECK(xdg_surf, "get_xdg_surface failed");
        CHECK(wl_proxy_add_listener(xdg_surf, xdg_surf_listener, 0) == 0,
              "xdg surface add_listener failed");
        struct wl_iface xt_iface = {"xdg_toplevel", 0};
        xdg_top = wl_proxy_marshal_constructor(xdg_surf, 1, &xt_iface);
        CHECK(xdg_top, "get_toplevel failed");
        CHECK(wl_proxy_add_listener(xdg_top, xdg_top_listener, 0) == 0,
              "xdg toplevel add_listener failed");
        wl_proxy_marshal(xdg_top, 2, "wayland-input");
        wl_proxy_marshal(xdg_top, 9, 0); /* start maximized: big target */
        /* first commit carries no buffer: the compositor must configure
         * the surface before any buffer may attach (attaching earlier
         * is a protocol error). */
        wl_surface_commit(surf);
        /* the compositor can answer a tick after our commit, so poll a
         * few roundtrips instead of asserting on the first one. */
        for (int i = 0; i < 10 && !got_xdg_configure; i++)
            CHECK(wl_display_roundtrip(disp) >= 0, "xdg roundtrip failed");
        printf("test_wayland_input: ping=%d xdg_conf=%d top_conf=%d close=%d\n",
               got_ping, got_xdg_configure, got_top_configure, got_top_close);
        CHECK(got_xdg_configure, "no xdg_surface.configure event");
        CHECK(got_top_configure, "no xdg_toplevel.configure event");
        printf("test_wayland_input: toplevel %dx%d states=%u bytes\n",
               top_w, top_h, top_nstates);
        /* ack + first buffer: the window is now mapped and focusable */
        wl_proxy_marshal(xdg_surf, 4, xdg_serial);
        wl_proxy_marshal(surf, 1, buf, 0, 0);
        wl_proxy_marshal(surf, 2, 0, 0, 640, 480);
        wl_surface_commit(surf);
        CHECK(wl_display_roundtrip(disp) >= 0, "xdg ack roundtrip failed");

        if (interactive) {
            printf("test_wayland_input: mapped. click the red window,\n"
                    "then move the mouse and press keys. 20s, counting:\n");
            fflush(stdout);
            int wlfd = wl_display_get_fd(disp);
            // wall-clock loop: exactly 20 real seconds. the old
            // iteration-counted loop finished early under input
            // because poll returns instantly when events are waiting.
            struct timespec t0, tnow;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            int last_sec = -1;
            for (;;) {
                clock_gettime(CLOCK_MONOTONIC, &tnow);
                int elapsed = (int)(tnow.tv_sec - t0.tv_sec);
                if (elapsed >= 20) break;
                wl_display_dispatch_pending(disp);
                /* dispatch_pending neither reads nor flushes: queued
                 * replies (xdg pong) would sit unsent and socket data
                 * would sit unread until kwin gives up and marks us
                 * not-responding. flush every tick, and run the real
                 * client read cycle (prepare/poll/read) so live input
                 * actually arrives. */
                wl_display_flush(disp);
                if (wl_display_prepare_read(disp) == 0) {
                    struct pollfd pfd = { wlfd, POLLIN, 0 };
                    int pr = poll(&pfd, 1, 200);
                    if (pr > 0)
                        wl_display_read_events(disp);
                    else
                        wl_display_cancel_read(disp);
                }
                clock_gettime(CLOCK_MONOTONIC, &tnow);
                elapsed = (int)(tnow.tv_sec - t0.tv_sec);
                if (elapsed != last_sec) {
                    last_sec = elapsed;
                    printf("  ... %ds motion=%d button=%d key=%d\n",
                           elapsed, n_ptr_motion, n_ptr_button, got_key);
                }
            }
            /* drain anything still queued */
            wl_display_roundtrip(disp);
            printf("test_wayland_input: live enter=%d motion=%d "
                   "button=%d key=%d kb_enter=%d\n",
                   n_ptr_enter, n_ptr_motion, n_ptr_button,
                   got_key, got_kb_enter);
            CHECK(n_ptr_motion > 0,
                   "no pointer motion in 20s (click the window first)");
            printf("test_wayland_input: motion x=%d y=%d button=%u/%u "
                   "key=%u/%u\n",
                   ptr_x, ptr_y, btn_button, btn_state,
                   key_code, key_state);
        }
    } else {
        printf("test_wayland_input: SKIP xdg (need compositor+shm+xdg)\n");
    }

    if (xdg_top) wl_proxy_destroy(xdg_top);
    if (xdg_surf) wl_proxy_destroy(xdg_surf);
    if (buf) wl_proxy_destroy(buf);
    if (pool) wl_proxy_destroy(pool);
    if (surf) wl_proxy_destroy(surf);
    if (comp) wl_proxy_destroy(comp);
    if (shm) wl_proxy_destroy(shm);
    if (xdg_base) wl_proxy_destroy(xdg_base);
    wl_proxy_destroy(ptr);
    wl_proxy_destroy(kb);
    wl_proxy_destroy(seat);
    wl_proxy_destroy(reg);
    wl_display_disconnect(disp);
    printf("test_wayland_input: ALL PASS\n");
    return 0;
}
