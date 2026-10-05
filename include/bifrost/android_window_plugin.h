#ifndef BIFROST_ANDROID_WINDOW_PLUGIN_H
#define BIFROST_ANDROID_WINDOW_PLUGIN_H
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
    BIFROST_WINDOW_KEY = 3, BIFROST_WINDOW_POINTER = 4
};
/* Input uses Android keycodes/actions/meta bits; pointer coordinates are in
 * surface pixels. One pointer per event (v2 is single-touch). No SDL enums.
 * time_ms is monotonic milliseconds. Zero-initialize unused fields. */
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
} bifrost_android_window_plugin_v2;
/* Immutable descriptor lives until module unload. V1 is incompatible and
 * deliberately rejected: an SDL2 pointer cannot become a native/v2 window. */
typedef const bifrost_android_window_plugin_v2 *
    (*bifrost_android_window_plugin_get_api_fn)(uint32_t host_abi);
const bifrost_android_window_plugin_v2 *bifrost_android_window_plugin_get_api(uint32_t host_abi);
#ifdef __cplusplus
}
#endif
#endif
