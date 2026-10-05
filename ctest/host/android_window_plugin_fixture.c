/* Real host C plugin: failure injection and teardown ordering oracle. */
#include "bifrost/android_window_plugin.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void record(const char *event) {
    const char *path = getenv("BIFROST_TEST_PLUGIN_LOG");
    if (!path) return;
    FILE *f = fopen(path, "a");
    if (f) { fprintf(f, "%s\n", event); fclose(f); }
}
static int mode(const char *value) {
    const char *selected = getenv("BIFROST_TEST_PLUGIN_MODE");
    return selected && !strcmp(selected, value);
}
struct state { void *window; int32_t width, height; };
static int32_t create(void **out) {
    record("create");
    *out = NULL;
    if (mode("create-null")) return 0;
    *out = calloc(1, sizeof(struct state));
    if (!*out) return -ENOMEM;
    return mode("create-fail") ? -EIO : 0;
}
static void destroy(void *instance) { record("destroy"); free(instance); }
static int32_t attach(void *instance, void *window) {
    if (mode("attach-fail")) return -EIO;
    ((struct state *)instance)->window = window;
    record("attach");
    return 0;
}
static void detach(void *instance) {
    struct state *s = instance;
    if (s->window) record("detach");
    s->window = NULL;
}
static int32_t resize(void *instance, int32_t w, int32_t h, uint32_t host) {
    struct state *s = instance;
    if (mode("resize-fail")) return -ENOTSUP;
    if (mode("positive-status")) return EIO;
    if (w <= 0 || h <= 0 || host > 1) return -EINVAL;
    s->width = w; s->height = h;
    return 0;
}
static int32_t binding(void *instance, bifrost_android_window_binding_v1 *out) {
    struct state *s = instance;
    /* Write poison before failure: the host must discard the entire pair. */
    *out = (bifrost_android_window_binding_v1){0xbad, (void *)1};
    if (mode("binding-fail") || !s->window) return -ENODEV;
    *out = (bifrost_android_window_binding_v1){
        ((uint64_t)(uint32_t)s->width << 32) | (uint32_t)s->height, s->window};
    return 0;
}
#ifndef BIFROST_TEST_NO_ENTRY
const bifrost_android_window_plugin_v1 *bifrost_android_window_plugin_get_api(uint32_t host_abi) {
    static bifrost_android_window_plugin_v1 api;
    if (host_abi != BIFROST_ANDROID_WINDOW_ABI_V1 || mode("null-api")) return NULL;
    api = (bifrost_android_window_plugin_v1){1, sizeof(api), "c-fixture",
        create, destroy, attach, detach, resize, binding};
    if (mode("bad-version")) api.abi_version = 99;
    if (mode("bad-size")) api.struct_size = 8;
    if (mode("missing-callback")) api.detach = NULL;
    if (mode("empty-name")) api.name = "";
    return &api;
}
#endif
__attribute__((destructor)) static void unload(void) { record("unload"); }
