// Real guest libX11 speaks the X protocol through Bifrost sockets, rather
// than using DisplayProxy's synthetic X11 handles. Link with ARM64 glibc/X11.
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int main(void) {
    if (!getenv("DISPLAY")) return 77;
    Display* display = XOpenDisplay(NULL);
    if (!display) { puts("xwayland_wire: cannot open display"); return 1; }
    int screen = DefaultScreen(display);
    unsigned long color = WhitePixel(display, screen);
    Window window = XCreateSimpleWindow(display, RootWindow(display, screen),
                                         0, 0, 240, 160, 0, color,
                                         BlackPixel(display, screen));
    XStoreName(display, window, "Bifrost ARM64 XWayland wire test");
    XMapWindow(display, window);
    GC gc = XCreateGC(display, window, 0, NULL);
    XSetForeground(display, gc, color);
    // Repeated round-trips warm both branch targets and retroactive chaining;
    // the old flag elision could corrupt replies after successful startup.
    for (unsigned i = 0; i < 100; ++i) {
        XFillRectangle(display, window, gc, 0, 0, 240, 160);
        XSync(display, False);
    }
    XImage* image = XGetImage(display, window, 120, 80, 1, 1, AllPlanes, ZPixmap);
    int good = image && XGetPixel(image, 0, 0) == color;
    if (image) XDestroyImage(image);
    XFreeGC(display, gc);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    puts(good ? "xwayland_wire: checks passed" : "xwayland_wire: pixel mismatch");
    return good ? 0 : 1;
}
