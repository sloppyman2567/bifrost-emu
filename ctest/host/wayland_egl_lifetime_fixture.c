/* An independently loaded helper library for the SDL backend lifetime oracle. */
#include <stdio.h>
#include <stdlib.h>

static void record(const char *event) {
    const char *path = getenv("BIFROST_TEST_WL_LOG");
    FILE *file = path ? fopen(path, "a") : NULL;
    if (file) { fprintf(file, "%s\n", event); fclose(file); }
}
__attribute__((constructor)) static void load(void) { record("load"); }
__attribute__((destructor)) static void unload(void) { record("unload"); }
void *wl_egl_window_create(void *surface, int width, int height) {
    if (!surface || width <= 0 || height <= 0 || getenv("BIFROST_TEST_WL_CREATE_FAIL"))
        return NULL;
    void *window = malloc(1);
    if (window) record("create");
    return window;
}
void wl_egl_window_destroy(void *window) { record("destroy"); free(window); }
void wl_egl_window_resize(void *window, int w, int h, int dx, int dy) {
    if (!window || w <= 0 || h <= 0 || dx || dy) abort();
    record("resize");
}
