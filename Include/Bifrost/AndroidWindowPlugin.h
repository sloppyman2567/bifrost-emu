#ifndef BIFROST_ANDROID_WINDOW_PLUGIN_H
#define BIFROST_ANDROID_WINDOW_PLUGIN_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Trusted host ABI. No SDL/C++ objects, guest pointers, Memory or CPU cross
 * this boundary. Callbacks run on the display thread, must not throw/reenter.
 * Status returns 0 or negative errno; poll returns 1, 0 or negative errno. */
#define BIFROST_ANDROID_WINDOW_ABI_V2 2u
#define BIFROST_ANDROID_WINDOW_PLUGIN_ENTRY "bifrost_android_window_plugin_get_api"
typedef struct bifrost_android_window_binding_v2 {
    uint64_t window; /* Native EGL window token, zero means unavailable. */
    void *display;   /* Matching native EGL display from the same connection. */
} bifrost_android_window_binding_v2;
enum bifrost_android_window_event_type {
    BIFROST_WINDOW_CLOSE = 1, BIFROST_WINDOW_RESIZE = 2,
    BIFROST_WINDOW_KEY = 3, BIFROST_WINDOW_POINTER = 4,
    /* Only after set_text_input(active=1): one text payload is ready for
     * poll_text, in order with the surrounding key and pointer events. */
    BIFROST_WINDOW_TEXT = 5
};
enum bifrost_android_window_text_kind {
    BIFROST_WINDOW_TEXT_COMMIT = 1,   /* text the editor keeps (replaces any composition) */
    BIFROST_WINDOW_TEXT_COMPOSING = 2 /* the composition in progress; empty clears it */
};
#define BIFROST_WINDOW_TEXT_MAX 255u
/* Pointer shapes for set_cursor: the nearest of the host toolkit's own system
 * cursors; an app's own bitmap cursor is not carried. */
enum bifrost_android_window_cursor {
    BIFROST_WINDOW_CURSOR_DEFAULT = 0, /* the arrow */
    BIFROST_WINDOW_CURSOR_HIDDEN = 1,
    BIFROST_WINDOW_CURSOR_TEXT = 2,
    BIFROST_WINDOW_CURSOR_POINTER = 3, /* the hand over a link */
    BIFROST_WINDOW_CURSOR_WAIT = 4,
    BIFROST_WINDOW_CURSOR_CROSSHAIR = 5,
    BIFROST_WINDOW_CURSOR_MOVE = 6,
    BIFROST_WINDOW_CURSOR_NOT_ALLOWED = 7,
    BIFROST_WINDOW_CURSOR_EW_RESIZE = 8,
    BIFROST_WINDOW_CURSOR_NS_RESIZE = 9,
    BIFROST_WINDOW_CURSOR_NESW_RESIZE = 10,
    BIFROST_WINDOW_CURSOR_NWSE_RESIZE = 11,
    BIFROST_WINDOW_CURSOR_COUNT = 12
};
typedef struct bifrost_android_window_text_v2 {
    uint32_t kind;   /* bifrost_android_window_text_kind */
    uint32_t length; /* UTF-8 bytes in utf8, at most BIFROST_WINDOW_TEXT_MAX */
    char utf8[BIFROST_WINDOW_TEXT_MAX + 1u]; /* valid UTF-8, NUL-terminated */
} bifrost_android_window_text_v2;
/* Input uses Android keycodes/actions/meta bits; pointer coordinates are in
 * surface pixels. One pointer per event; concurrent touches use distinct
 * pointer_id values (opaque keys; the host assigns Android ids). No SDL enums.
 * time_ms is CLOCK_MONOTONIC milliseconds. Zero-initialize unused fields. */
typedef struct bifrost_android_window_event_v2 {
    uint32_t type;
    int32_t width, height;
    int32_t action, key_code, scan_code, meta_state, repeat_count, pointer_id;
    float x, y, pressure;
    int64_t time_ms;
} bifrost_android_window_event_v2;
typedef struct bifrost_android_window_plugin_v2 {
    uint32_t abi_version, struct_size; /* Append-only within v2. */
    const char *name; /* Nonempty NUL-terminated, at most 128 bytes. */
    /* Failed create may return a partial instance; host destroys it. */
    int32_t (*create)(void **instance);
    /* Stop/join workers before returning; module can then unload. */
    void (*destroy)(void *instance);
    /* Own or borrow a window internally. Failure leaves it closed. Called
     * once per open/close cycle; initial dimensions are positive pixels. */
    int32_t (*open)(void *instance, const char *title, int32_t width, int32_t height);
    /* Infallible, idempotent. Destroy native wrappers before their window. */
    void (*close)(void *instance);
    /* Failure preserves buffer geometry. resize_host is 0 or 1; false
     * accepts a host resize notification without requesting another resize. */
    int32_t (*resize)(void *instance, int32_t width, int32_t height, uint32_t resize_host);
    /* Pair remains valid until close/destroy, including across resize. */
    int32_t (*binding)(void *instance, bifrost_android_window_binding_v2 *out);
    /* Nonblocking; 1=event, 0=empty, negative errno=failure. */
    int32_t (*poll)(void *instance, bifrost_android_window_event_v2 *out);
    /* Optional: present when struct_size covers both and both are non-null.
     * Framework popup windows (dialogs, menus) drawn by the guest: width x
     * height unpremultiplied 0xAARRGGBB words at window-pixel x/y, above
     * every surface in creation order. Popups never take input; the primary
     * window keeps receiving it. The pixels are valid only during the call.
     * Return -EAGAIN to have the guest retry on its next frame. */
    int32_t (*show_popup)(void *instance, uint32_t id, int32_t x, int32_t y,
                          int32_t width, int32_t height, const uint32_t *argb);
    void (*remove_popup)(void *instance, uint32_t id);
    /* Optional: present when struct_size covers both and both are non-null.
     * Desktop text input for the focused editor. active=1 starts the host's
     * input method (layouts, dead keys, composition) and the window then
     * reports BIFROST_WINDOW_TEXT; active=0 stops it. x/y/width/height are
     * the composition anchor in window pixels (ignored when stopping).
     * Key events keep arriving either way. */
    int32_t (*set_text_input)(void *instance, uint32_t active, int32_t x, int32_t y,
                              int32_t width, int32_t height);
    /* Nonblocking; 1=payload, 0=none, negative errno=failure. Called once per
     * BIFROST_WINDOW_TEXT event. */
    int32_t (*poll_text)(void *instance, bifrost_android_window_text_v2 *out);
    /* Optional task visibility. 0=success, negative errno=failure. Hiding
     * preserves the window and its surfaces; showing may request focus. */
    int32_t (*set_task_visible)(void *instance, uint32_t visible);
    /* Optional. The pointer shape over the window's surfaces, a
     * bifrost_android_window_cursor value (anything else is -EINVAL). The host
     * sends it only when the shape changes. 0=success, negative errno=failure. */
    int32_t (*set_cursor)(void *instance, int32_t cursor);
} bifrost_android_window_plugin_v2;
/* Smallest valid v2 descriptor: every required callback, no optional ones. */
#define BIFROST_ANDROID_WINDOW_PLUGIN_V2_MIN_SIZE \
    offsetof(bifrost_android_window_plugin_v2, show_popup)
/* Immutable descriptor lives until module unload. V1 is incompatible and
 * deliberately rejected: an SDL2 pointer cannot become a native/v2 window. */
typedef const bifrost_android_window_plugin_v2 *
    (*bifrost_android_window_plugin_get_api_fn)(uint32_t host_abi);
const bifrost_android_window_plugin_v2 *bifrost_android_window_plugin_get_api(uint32_t host_abi);
#ifdef __cplusplus
}
#endif
#endif
