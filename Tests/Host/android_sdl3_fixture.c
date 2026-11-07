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
bool SDL_InitSubSystem(SDL_InitFlags flags) {
    if (flags & SDL_INIT_GAMEPAD) {
        if (getenv("BIFROST_TEST_SDL_GAMEPAD_FAIL")) return false;
        record("gamepad-init");
        return true;
    }
    record("video-init");
    return true;
}
void SDL_QuitSubSystem(SDL_InitFlags flags) { record(flags & SDL_INIT_GAMEPAD ? "gamepad-quit" : "video-quit"); }
/* One fake pad; open and close are logged with its instance id. */
struct fake_gamepad { SDL_JoystickID id; };
SDL_Gamepad *SDL_OpenGamepad(SDL_JoystickID id) {
    char line[48];
    struct fake_gamepad *pad = malloc(sizeof(*pad));
    pad->id = id;
    snprintf(line, sizeof(line), "gamepad-open %u", (unsigned)id);
    record(line);
    return (SDL_Gamepad *)pad;
}
void SDL_CloseGamepad(SDL_Gamepad *gamepad) {
    char line[48];
    snprintf(line, sizeof(line), "gamepad-close %u", (unsigned)((struct fake_gamepad *)gamepad)->id);
    record(line);
    free(gamepad);
}
const char *SDL_GetGamepadName(SDL_Gamepad *gamepad) { (void)gamepad; return "Fake Pad"; }
Uint16 SDL_GetGamepadVendor(SDL_Gamepad *gamepad) { (void)gamepad; return 0x28de; }
Uint16 SDL_GetGamepadProduct(SDL_Gamepad *gamepad) { (void)gamepad; return 0x1304; }
bool SDL_SetHint(const char *name, const char *value) { (void)name; (void)value; return true; }
const char *SDL_GetError(void) { return "injected SDL3 failure"; }
/* System cursors: each call is logged with the SDL cursor id, so a test sees exactly what the backend asked for. */
struct fake_cursor { int id; };
SDL_Cursor *SDL_CreateSystemCursor(SDL_SystemCursor id) {
    char line[48];
    struct fake_cursor *cursor = malloc(sizeof(*cursor));
    cursor->id = (int)id;
    snprintf(line, sizeof(line), "cursor-create %d", cursor->id);
    record(line);
    return (SDL_Cursor *)cursor;
}
bool SDL_SetCursor(SDL_Cursor *cursor) {
    char line[48];
    snprintf(line, sizeof(line), "cursor-set %d", ((struct fake_cursor *)cursor)->id);
    record(line);
    return true;
}
void SDL_DestroyCursor(SDL_Cursor *cursor) {
    char line[48];
    snprintf(line, sizeof(line), "cursor-destroy %d", ((struct fake_cursor *)cursor)->id);
    record(line);
    free(cursor);
}
bool SDL_ShowCursor(void) { record("cursor-show"); return true; }
bool SDL_HideCursor(void) { record("cursor-hide"); return true; }
static int requested_width, requested_height;
/* A compositor keeps a window inside the desktop's work area (e.g. below a panel). */
static void clamp_to_work_area(int *width, int *height) {
    const char *area = getenv("BIFROST_TEST_SDL_WORK_AREA");
    int area_w = 0, area_h = 0;
    if (area && sscanf(area, "%dx%d", &area_w, &area_h) == 2) {
        if (*width > area_w) *width = area_w;
        if (*height > area_h) *height = area_h;
    }
}
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
        clamp_to_work_area(&width, &height);
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
bool SDL_SyncWindow(SDL_Window *window) { (void)window; return true; }
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
/* "pad:a,b,c" plays its comma-separated gamepad events once each, in order (pad 7 unless noted). */
static char pad_script[512];
static size_t pad_next;
static bool pad_event(const char *script, SDL_Event *event) {
    if (strcmp(script, pad_script)) { snprintf(pad_script, sizeof(pad_script), "%s", script); pad_next = 0; }
    const char *at = pad_script + 4;
    for (size_t i = 0; i < pad_next && at; ++i) { at = strchr(at, ','); if (at) ++at; }
    if (!at || !*at) return false;
    ++pad_next;
    char token[32];
    size_t length = strcspn(at, ",");
    snprintf(token, sizeof(token), "%.*s", (int)length, at);
    static const struct { const char *name; SDL_GamepadButton button; } buttons[] = {
        {"a", SDL_GAMEPAD_BUTTON_SOUTH}, {"guide", SDL_GAMEPAD_BUTTON_GUIDE},
        {"paddle", SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1}, {"left", SDL_GAMEPAD_BUTTON_DPAD_LEFT},
        {"right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT}, {"unknown", SDL_GAMEPAD_BUTTON_SOUTH},
    };
    static const struct { const char *name; SDL_GamepadAxis axis; } axes[] = {
        {"leftx", SDL_GAMEPAD_AXIS_LEFTX}, {"lefty", SDL_GAMEPAD_AXIS_LEFTY}, {"lt", SDL_GAMEPAD_AXIS_LEFT_TRIGGER},
    };
    if (!strcmp(token, "added") || !strcmp(token, "removed")) {
        event->type = token[0] == 'a' ? SDL_EVENT_GAMEPAD_ADDED : SDL_EVENT_GAMEPAD_REMOVED;
        event->gdevice.which = 7;
        return true;
    }
    if (!strcmp(token, "mouse-move")) {
        event->type = SDL_EVENT_MOUSE_MOTION; event->motion.windowID = 1;
        event->motion.x = 410; event->motion.y = 310;
        return true;
    }
    if (!strcmp(token, "focus-lost")) {
        event->type = SDL_EVENT_WINDOW_FOCUS_LOST; event->window.windowID = 1;
        return true;
    }
    for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i) {
        size_t name = strlen(buttons[i].name);
        if (strncmp(token, buttons[i].name, name) || token[name] != '-') continue;
        event->type = !strcmp(token + name, "-down") ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
        event->gbutton.which = !strcmp(buttons[i].name, "unknown") ? 99 : 7;
        event->gbutton.button = (Uint8)buttons[i].button;
        event->gbutton.down = event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        return true;
    }
    for (size_t i = 0; i < sizeof(axes) / sizeof(axes[0]); ++i) {
        size_t name = strlen(axes[i].name);
        if (strncmp(token, axes[i].name, name) || token[name] != ':') continue;
        event->type = SDL_EVENT_GAMEPAD_AXIS_MOTION; event->gaxis.which = 7;
        event->gaxis.axis = (Uint8)axes[i].axis; event->gaxis.value = (Sint16)atoi(token + name + 1);
        return true;
    }
    return false;
}
bool SDL_PollEvent(SDL_Event *event) {
    const char *mode = getenv("BIFROST_TEST_SDL_EVENT");
    if (!mode) return false;
    memset(event, 0, sizeof(*event));
    event->common.timestamp = 123000000;
    if (!strncmp(mode, "pad:", 4)) return pad_event(mode, event);
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
