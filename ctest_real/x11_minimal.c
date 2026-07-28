/* x11_minimal.c — minimal X11 display test for bifrost-emu.
 *
 * Uses internal syscalls 0x1002/0x1003 for dlopen/dlsym so a static
 * musl binary can resolve thunked libX11 symbols without musl's
 * "Dynamic loading not supported" stub.
 *
 * Build:
 *   make cross SRC=ctest_real/x11_minimal.c OUT=ctest_real/x11_minimal.elf
 *
 * Run:
 *   ./bifrost-emu ctest_real/x11_minimal.elf
 *
 * Success = visible host window + exit 0.
 * Exit 77 = skip when libX11 thunk unavailable.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef unsigned long XID;
typedef unsigned long Window;
typedef struct { void* display; unsigned long xdisplay; } Display;
typedef unsigned long GC;
typedef unsigned long Drawable;
typedef unsigned long Atom;

static uint64_t bifrost_dlopen(const char* path, uint64_t mode) {
    register uint64_t x0 __asm__("x0") = (uint64_t)(uintptr_t)path;
    register uint64_t x1 __asm__("x1") = mode;
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
    if (!name) { printf("x11_minimal: FAIL dlsym %s\n", #name); return 1; } \
} while(0)

typedef Display* (*XOpenDisplay_t)(const char*);
typedef int (*XCloseDisplay_t)(Display*);
typedef Window (*XCreateSimpleWindow_t)(Display*, Window, int, int,
    unsigned int, unsigned int, unsigned int, unsigned long, unsigned long);
typedef int (*XMapWindow_t)(Display*, Window);
typedef int (*XUnmapWindow_t)(Display*, Window);
typedef int (*XFlush_t)(Display*);
typedef int (*XFillRectangle_t)(Display*, Drawable, GC,
    int, int, unsigned int, unsigned int);

int main(void) {
    printf("x11_minimal: start\n");

    uint64_t hx11 = bifrost_dlopen("libX11.so.6", 1);
    if (!hx11) hx11 = bifrost_dlopen("libX11.so", 1);
    if (!hx11) {
        printf("x11_minimal: SKIP (libX11 thunk unavailable)\n");
        return 77;
    }

    XOpenDisplay_t XOpenDisplay;
    XCloseDisplay_t XCloseDisplay;
    XCreateSimpleWindow_t XCreateSimpleWindow;
    XMapWindow_t XMapWindow;
    XUnmapWindow_t XUnmapWindow;
    XFlush_t XFlush;
    XFillRectangle_t XFillRectangle;
    LOAD(hx11, XOpenDisplay_t, XOpenDisplay);
    LOAD(hx11, XCloseDisplay_t, XCloseDisplay);
    LOAD(hx11, XCreateSimpleWindow_t, XCreateSimpleWindow);
    LOAD(hx11, XMapWindow_t, XMapWindow);
    LOAD(hx11, XUnmapWindow_t, XUnmapWindow);
    LOAD(hx11, XFlush_t, XFlush);
    LOAD(hx11, XFillRectangle_t, XFillRectangle);

    Display* dpy = XOpenDisplay(NULL);
    if (!dpy) {
        printf("x11_minimal: FAIL (XOpenDisplay returned NULL)\n");
        return 1;
    }
    printf("x11_minimal: XOpenDisplay OK\n");

    Window root = 0x50000001ULL;
    Window win = XCreateSimpleWindow(dpy, root, 10, 10, 200, 150, 2, 0, 0);
    if (win == 0 || win == (Window)-1) {
        printf("x11_minimal: FAIL (XCreateSimpleWindow=0)\n");
        XCloseDisplay(dpy);
        return 1;
    }
    printf("x11_minimal: XCreateSimpleWindow OK win=%lx\n", (unsigned long)win);

    int r = XMapWindow(dpy, win);
    if (r == 0) {
        printf("x11_minimal: FAIL (XMapWindow failed)\n");
        XUnmapWindow(dpy, win);
        XCloseDisplay(dpy);
        return 1;
    }

    for (int i = 0; i < 3; i++) {
        XFillRectangle(dpy, win, 0, 10 + i * 10, 10 + i * 10, 60, 40);
        XFlush(dpy);
    }

    printf("x11_minimal: ALL PASS\n");
    XUnmapWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
