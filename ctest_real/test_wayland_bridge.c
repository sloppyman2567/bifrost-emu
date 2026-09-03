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
    LOAD(hwl, wl_display_connect_t, wl_display_connect);
    LOAD(hwl, wl_display_fd_t, wl_display_get_fd);
    LOAD(hwl, wl_display_op_t, wl_display_flush);
    LOAD(hwl, wl_display_op_t, wl_display_roundtrip);
    LOAD(hwl, wl_display_op_t, wl_display_dispatch_pending);
    LOAD(hwl, wl_display_disconnect_t, wl_display_disconnect);

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
    wl_display_disconnect(disp);
    printf("test_wayland_bridge: ALL PASS\n");
    return 0;
}
