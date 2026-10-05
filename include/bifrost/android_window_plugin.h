#ifndef BIFROST_ANDROID_WINDOW_PLUGIN_H
#define BIFROST_ANDROID_WINDOW_PLUGIN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Host ABI, not an Android guest ABI. Build plugins for the host architecture.
 * No C++ objects, guest pointers, Memory or CPU access cross this boundary.
 * All callbacks run on the display thread and must not throw or reenter the
 * surface manager. Status callbacks return 0 or a negative errno.
 */
#define BIFROST_ANDROID_WINDOW_ABI_V1 1u
#define BIFROST_ANDROID_WINDOW_PLUGIN_ENTRY "bifrost_android_window_plugin_get_api"

typedef struct bifrost_android_window_binding_v1 {
    uint64_t window; /* Host EGL native window token; zero means unavailable. */
    void *display;  /* Matching host EGL native display; NULL permits default. */
} bifrost_android_window_binding_v1;

typedef struct bifrost_android_window_plugin_v1 {
    uint32_t abi_version;
    uint32_t struct_size; /* At least sizeof this v1 struct; append-only. */
    const char *name;     /* Nonempty, NUL-terminated, <=128 bytes excluding NUL. */
    /* create returns a non-NULL owned instance on success. On failure a
     * non-NULL partial instance is passed to destroy for cleanup. */
    int32_t (*create)(void **instance);
    /* Stop/join any workers before returning: the module may then unload. */
    void (*destroy)(void *instance);
    /* The host owns the SDL_Window*. A failed attach preserves previous
     * state; successful replacement releases the old native wrapper. */
    int32_t (*attach)(void *instance, void *borrowed_sdl_window);
    /* Idempotent and infallible. Release all wrappers before SDL destruction. */
    void (*detach)(void *instance);
    /* Dimensions are positive. Failure leaves geometry unchanged.
     * resize_host is exactly 0 or 1. */
    int32_t (*resize)(void *instance, int32_t width, int32_t height,
                      uint32_t resize_host);
    /* Return the window/display pair from one connection, or a negative errno.
     * The pair stays valid until attach/detach/destroy. */
    int32_t (*binding)(void *instance, bifrost_android_window_binding_v1 *out);
} bifrost_android_window_plugin_v1;

/* Export this symbol with C linkage and default visibility. The returned
 * descriptor is immutable and lives until the module is unloaded. */
typedef const bifrost_android_window_plugin_v1 *
    (*bifrost_android_window_plugin_get_api_fn)(uint32_t host_abi);
const bifrost_android_window_plugin_v1 *
    bifrost_android_window_plugin_get_api(uint32_t host_abi);

#ifdef __cplusplus
}
#endif
#endif
