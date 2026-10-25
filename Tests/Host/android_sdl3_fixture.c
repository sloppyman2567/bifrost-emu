/* SDL3 DSO oracle, built against real public headers; no compositor needed. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Pixel size doubles only for high-density windows, as on a 2x Wayland output. */
struct fake_window { int width, height, scale; unsigned id; char title[512]; };
const char *SDL_GetWindowTitle(SDL_Window *window) { return ((struct fake_window *)window)->title; }
bool SDL_SetWindowTitle(SDL_Window *window, const char *title) {
    snprintf(((struct fake_window *)window)->title, 512, "%s", title);
    return true;
}
bool SDL_GetWindowPosition(SDL_Window *window, int *x, int *y) {
    (void)window; *x = 0; *y = 0; return true;
}
SDL_WindowFlags SDL_GetWindowFlags(SDL_Window *window) { (void)window; return SDL_WINDOW_INPUT_FOCUS; }
SDL_DisplayID SDL_GetDisplayForWindow(SDL_Window *window) { return window ? 1 : 0; }
bool SDL_GetDisplayBounds(SDL_DisplayID display, SDL_Rect *bounds) {
    if (display != 1 || !bounds) return false;
    *bounds = (SDL_Rect){0, 0, 1920, 1080}; return true;
}
static struct fake_window *windows[16];
/* Required loader exports added by task visibility and host text input. */
bool SDL_HideWindow(SDL_Window *window) { return window != NULL; }
bool SDL_ShowWindow(SDL_Window *window) { return window != NULL; }
bool SDL_RaiseWindow(SDL_Window *window) { return window != NULL; }
bool SDL_RestoreWindow(SDL_Window *window) { return window != NULL; }
Uint64 SDL_GetTicksNS(void) { return 123000000; }
bool SDL_StartTextInput(SDL_Window *window) { return window != NULL; }
bool SDL_StopTextInput(SDL_Window *window) { return window != NULL; }
bool SDL_SetTextInputArea(SDL_Window *window, const SDL_Rect *rect, int cursor) {
    (void)cursor; return window != NULL && rect != NULL;
}
int SDL_PeepEvents(SDL_Event *events, int count, SDL_EventAction action, Uint32 min, Uint32 max) {
    (void)events; (void)count; (void)action; (void)min; (void)max; return 0;
}
SDL_Keymod SDL_GetModState(void) { return getenv("BIFROST_TEST_SDL_SHIFT") ? SDL_KMOD_LSHIFT : SDL_KMOD_NONE; }
bool SDL_PushEvent(SDL_Event *event) { return event != NULL; }
SDL_Scancode SDL_GetScancodeFromName(const char *name) { (void)name; return SDL_SCANCODE_UNKNOWN; }
SDL_Keycode SDL_GetKeyFromScancode(SDL_Scancode code, SDL_Keymod mods, bool event) {
    (void)code; (void)mods; (void)event; return SDLK_UNKNOWN;
}
static void record(const char *event) {
    const char *path = getenv("BIFROST_TEST_WL_LOG");
    FILE *file = path ? fopen(path, "a") : NULL;
    if (file) { fprintf(file, "%s\n", event); fclose(file); }
}
bool SDL_InitSubSystem(SDL_InitFlags flags) { (void)flags; record("video-init"); return true; }
void SDL_QuitSubSystem(SDL_InitFlags flags) { (void)flags; record("video-quit"); }
bool SDL_SetHint(const char *name, const char *value) { (void)name; (void)value; return true; }
const char *SDL_GetError(void) { return "injected SDL3 failure"; }
static int requested_width, requested_height;
static bool external_context, high_density;
SDL_PropertiesID SDL_CreateProperties(void) { external_context = high_density = false; return 100; }
void SDL_DestroyProperties(SDL_PropertiesID props) { (void)props; }
bool SDL_SetStringProperty(SDL_PropertiesID props, const char *name, const char *value) {
    (void)props; (void)name; (void)value; return true;
}
bool SDL_SetNumberProperty(SDL_PropertiesID props, const char *name, Sint64 value) {
    (void)props;
    if (!strcmp(name, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER)) requested_width = value;
    if (!strcmp(name, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER)) requested_height = value;
    return true;
}
bool SDL_SetBooleanProperty(SDL_PropertiesID props, const char *name, bool value) {
    (void)props;
    if (!strcmp(name, SDL_PROP_WINDOW_CREATE_EXTERNAL_GRAPHICS_CONTEXT_BOOLEAN)) external_context = value;
    if (!strcmp(name, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN)) high_density = value;
    return true;
}
SDL_Window *SDL_CreateWindowWithProperties(SDL_PropertiesID props) {
    (void)props;
    if (!external_context) return NULL;
    int width = requested_width, height = requested_height;
    if (getenv("BIFROST_TEST_SDL_CREATE_FAIL")) return NULL;
    for (unsigned i = 0; i < 16; ++i) if (!windows[i]) {
        struct fake_window *w = calloc(1, sizeof(*w));
        if (!w) return NULL;
        w->width = width; w->height = height; w->scale = high_density ? 2 : 1;
        w->id = i + 1; windows[i] = w;
        record("window-create"); return (SDL_Window *)w;
    }
    return NULL;
}
void SDL_DestroyWindow(SDL_Window *window) {
    struct fake_window *w = (struct fake_window *)window;
    windows[w->id - 1] = NULL; record("window-destroy"); free(w);
}
SDL_WindowID SDL_GetWindowID(SDL_Window *window) { return ((struct fake_window *)window)->id; }
SDL_PropertiesID SDL_GetWindowProperties(SDL_Window *window) { return SDL_GetWindowID(window); }
void *SDL_GetPointerProperty(SDL_PropertiesID props, const char *name, void *fallback) {
    if (getenv("BIFROST_TEST_SDL_BINDING_FAIL")) return fallback;
    if (!strcmp(name, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER)) return (void *)0x1234;
    if (!strcmp(name, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER)) return (void *)(uintptr_t)(0x5678 + props);
    return fallback;
}
Sint64 SDL_GetNumberProperty(SDL_PropertiesID props, const char *name, Sint64 fallback) {
    (void)props; (void)name; return fallback;
}
bool SDL_SetWindowSize(SDL_Window *window, int width, int height) {
    if (getenv("BIFROST_TEST_SDL_RESIZE_FAIL")) return false;
    struct fake_window *w = (struct fake_window *)window;
    w->width = width; w->height = height; record("host-resize"); return true;
}
bool SDL_SetWindowAspectRatio(SDL_Window *window, float minimum, float maximum) {
    (void)window; (void)minimum; (void)maximum;
    return getenv("BIFROST_TEST_SDL_ASPECT_FAIL") == NULL;
}
bool SDL_GetWindowSize(SDL_Window *window, int *width, int *height) {
    struct fake_window *w = (struct fake_window *)window;
    *width = w->width; *height = w->height; return true;
}
bool SDL_GetWindowSizeInPixels(SDL_Window *window, int *width, int *height) {
    bool result = SDL_GetWindowSize(window, width, height);
    int scale = ((struct fake_window *)window)->scale;
    *width *= scale; *height *= scale; return result;
}
/* One-shot events for the wheel gestures: a mode "wheel#2" is delivered once, then nothing until it changes.
   The cursor is at (400, 300) in a 800x600 window. */
static char delivered_mode[64];
bool SDL_PollEvent(SDL_Event *event) {
    const char *mode = getenv("BIFROST_TEST_SDL_EVENT");
    if (!mode) return false;
    memset(event, 0, sizeof(*event));
    event->common.timestamp = 123000000;
    if (!strncmp(mode, "gesture-", 8)) {
        if (!strcmp(mode, delivered_mode)) return false;
        snprintf(delivered_mode, sizeof(delivered_mode), "%s", mode);
        const char *what = mode + 8;
        if (!strncmp(what, "wheel", 5)) {
            event->type = SDL_EVENT_MOUSE_WHEEL; event->wheel.windowID = 1;
            event->wheel.mouse_x = 400; event->wheel.mouse_y = 300;
            event->wheel.y = (float)atof(strchr(what, ':') + 1);
            if (strstr(what, "flipped")) event->wheel.direction = SDL_MOUSEWHEEL_FLIPPED;
        } else if (!strncmp(what, "middle-down", 11) || !strncmp(what, "middle-up", 9)) {
            event->type = what[7] == 'd' ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
            event->button.windowID = 1; event->button.button = SDL_BUTTON_MIDDLE;
            event->button.x = 400; event->button.y = 300;
        } else if (!strncmp(what, "move", 4)) {
            event->type = SDL_EVENT_MOUSE_MOTION; event->motion.windowID = 1;
            event->motion.x = 410; event->motion.y = 310;
            event->motion.state = strstr(what, "held") ? SDL_BUTTON_MMASK : 0;
        } else if (!strncmp(what, "left-down", 9)) {
            event->type = SDL_EVENT_MOUSE_BUTTON_DOWN; event->button.windowID = 1;
            event->button.button = SDL_BUTTON_LEFT; event->button.x = 10; event->button.y = 20;
        } else if (!strncmp(what, "left-up", 7)) {
            event->type = SDL_EVENT_MOUSE_BUTTON_UP; event->button.windowID = 1;
            event->button.button = SDL_BUTTON_LEFT; event->button.x = 10; event->button.y = 20;
#if SDL_VERSION_ATLEAST(3, 4, 0)
        } else if (!strncmp(what, "pinch", 5)) {
            event->type = SDL_EVENT_PINCH_UPDATE; event->pinch.windowID = 1;
            event->pinch.scale = (float)atof(strchr(what, ':') + 1);
#endif
        } else { return false; }
        return true;
    }
    if (!strcmp(mode, "key")) {
        event->type = SDL_EVENT_KEY_DOWN; event->key.windowID = 1;
        event->key.scancode = SDL_SCANCODE_A; event->key.raw = 30;
        event->key.mod = SDL_KMOD_LSHIFT | SDL_KMOD_LCTRL;
    } else if (!strcmp(mode, "pointer")) {
        event->type = SDL_EVENT_MOUSE_BUTTON_DOWN; event->button.windowID = 1;
        event->button.button = SDL_BUTTON_LEFT; event->button.x = 10; event->button.y = 20;
    } else if (!strcmp(mode, "other-window")) {
        event->type = SDL_EVENT_KEY_DOWN; event->key.windowID = 99;
        event->key.scancode = SDL_SCANCODE_A;
    } else { return false; }
    return true;
}

SDL_Surface *SDL_CreateSurfaceFrom(int width, int height, SDL_PixelFormat format, void *pixels, int pitch) {
    if (width != 128 || height != 128 || format != SDL_PIXELFORMAT_RGBA32 || pitch != width * 4 || !pixels)
        return NULL;
    SDL_Surface *surface = calloc(1, sizeof(*surface));
    surface->w = width; surface->h = height; surface->pixels = pixels;
    return surface;
}
bool SDL_SetWindowIcon(SDL_Window *window, SDL_Surface *icon) {
    if (!window || !icon || !icon->pixels) return false;
    record("window-icon"); return true;
}
void SDL_DestroySurface(SDL_Surface *surface) { free(surface); }
