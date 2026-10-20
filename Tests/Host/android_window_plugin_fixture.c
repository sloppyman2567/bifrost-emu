/* Real host C plugin: failure injection and teardown ordering oracle. */
#define _POSIX_C_SOURCE 200809L
#include "Bifrost/AndroidWindowPlugin.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
struct state {
    void *window; int32_t width, height;
    int text;                 /* text input started by the host */
    unsigned step;            /* next "text-input" script step */
    int64_t text_start_ms;
    const struct text_step *payload;  /* owed to the next poll_text */
};
/* "text-input" script, one step per 100 ms once the host starts text input:
 * a commit, a composition replaced by a commit, a key (Backspace) and a commit. */
struct text_step { int type; uint32_t kind; const char *utf8; int32_t key, action; };
static const struct text_step text_script[] = {
    {BIFROST_WINDOW_TEXT, BIFROST_WINDOW_TEXT_COMMIT, "h\xc3\xa9llo", 0, 0},
    {BIFROST_WINDOW_TEXT, BIFROST_WINDOW_TEXT_COMPOSING, "ni", 0, 0},
    {BIFROST_WINDOW_TEXT, BIFROST_WINDOW_TEXT_COMMIT, "\xe4\xbd\xa0", 0, 0},
    {BIFROST_WINDOW_KEY, 0, NULL, 67 /* AKEYCODE_DEL */, 0},
    {BIFROST_WINDOW_KEY, 0, NULL, 67, 1},
    {BIFROST_WINDOW_TEXT, BIFROST_WINDOW_TEXT_COMMIT, "!", 0, 0},
};
/* The scripted modes; the bad-* ones corrupt the payload poll_text hands over. */
static int text_mode(void) {
    return mode("text-input") || mode("text-bad-kind") || mode("text-bad-length");
}
static int64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int32_t create(void **out) {
    record("create");
    *out = NULL;
    if (mode("create-null")) return 0;
    *out = calloc(1, sizeof(struct state));
    if (!*out) return -ENOMEM;
    return mode("create-fail") ? -EIO : 0;
}
static void destroy(void *instance) { record("destroy"); free(instance); }
static int32_t open_window(void *instance, const char *title, int32_t w, int32_t h) {
    if (!title || w <= 0 || h <= 0) return -EINVAL;
    if (mode("open-fail")) return -EIO;
    ((struct state *)instance)->window = instance;
    record("open");
    return 0;
}
static void close_window(void *instance) {
    struct state *s = instance;
    if (s->window) record("close");
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
static int32_t binding(void *instance, bifrost_android_window_binding_v2 *out) {
    struct state *s = instance;
    /* Write poison before failure: the host must discard the entire pair. */
    *out = (bifrost_android_window_binding_v2){0xbad, (void *)1};
    if (mode("binding-fail") || !s->window) return -ENODEV;
    *out = (bifrost_android_window_binding_v2){
        ((uint64_t)(uint32_t)s->width << 32) | (uint32_t)s->height, s->window};
    return 0;
}
static int32_t set_text_input(void *instance, uint32_t active, int32_t x, int32_t y,
                              int32_t w, int32_t h) {
    struct state *s = instance;
    char line[96];
    snprintf(line, sizeof(line), "text-input %u %d,%d %dx%d", active, x, y, w, h);
    record(line);
    if (mode("text-input-fail")) return -ENOTSUP;
    s->text = active != 0;
    s->step = 0;
    s->text_start_ms = now_ms();
    s->payload = NULL;
    return 0;
}
static int32_t poll_text(void *instance, bifrost_android_window_text_v2 *out) {
    struct state *s = instance;
    *out = (bifrost_android_window_text_v2){0};
    if (!s->payload) return 0;
    out->kind = s->payload->kind;
    out->length = (uint32_t)strlen(s->payload->utf8);
    memcpy(out->utf8, s->payload->utf8, out->length);
    if (mode("text-bad-kind")) out->kind = 9;
    if (mode("text-bad-length")) out->length = BIFROST_WINDOW_TEXT_MAX + 1;
    s->payload = NULL;
    return 1;
}
static int32_t poll_window(void *instance, bifrost_android_window_event_v2 *out) {
    struct state *s = instance;
    *out = (bifrost_android_window_event_v2){0};
    /* TEXT before the host started text input is a protocol error. */
    if (mode("text-unsolicited")) { out->type = BIFROST_WINDOW_TEXT; return 1; }
    if (text_mode()) {
        const unsigned count = sizeof(text_script) / sizeof(text_script[0]);
        if (!s->text || s->step >= count || now_ms() - s->text_start_ms < 100 * (int64_t)(s->step + 1))
            return 0;
        const struct text_step *step = &text_script[s->step];
        out->type = step->type;
        out->time_ms = now_ms();
        if (step->type == BIFROST_WINDOW_KEY) { out->key_code = step->key; out->action = step->action; }
        else s->payload = step;
        s->step++;
        return 1;
    }
    if (mode("poll-fail")) { out->type = 99; return -EIO; }
    if (mode("poll-invalid")) { out->type = 99; return 1; }
    if (mode("poll-resize")) { out->type = BIFROST_WINDOW_RESIZE; out->width = 640; out->height = 480; return 1; }
    if (mode("poll-positive")) return 2;
    if (mode("poll-nan")) { out->type = BIFROST_WINDOW_POINTER; out->x = NAN; return 1; }
    if (mode("poll-key-invalid")) { out->type = BIFROST_WINDOW_KEY; out->action = 9; return 1; }
    if (mode("poll-pointer-down")) {
        out->type = BIFROST_WINDOW_POINTER; out->x = 10; out->y = 20; out->pressure = 1;
        out->time_ms = 1234;
        return 1;
    }
    if (mode("poll-two-fingers")) {
        /* key, action, x, y: two overlapping touches, a hover while touched,
         * a third touch reusing the freed id, then a move for no touch. */
        static const struct { int32_t key, action; float x, y; } script[] = {
            {7, 0, 10, 20}, {9, 0, 30, 40}, {7, 2, 12, 22}, {0, 7, 1, 1}, {7, 1, 12, 22},
            {11, 0, 50, 60}, {9, 1, 30, 40}, {11, 1, 50, 60}, {5, 2, 0, 0},
        };
        static unsigned step;
        if (step == sizeof(script) / sizeof(script[0])) { step = 0; return 0; }
        out->type = BIFROST_WINDOW_POINTER;
        out->pointer_id = script[step].key; out->action = script[step].action;
        out->x = script[step].x; out->y = script[step].y;
        out->pressure = script[step].action == 1 ? 0 : 1;
        out->time_ms = 1000 + step++;
        return 1;
    }
    return 0;
}
static int32_t show_popup(void *instance, uint32_t id, int32_t x, int32_t y, int32_t w, int32_t h,
                          const uint32_t *argb) {
    (void)instance;
    char line[96];
    snprintf(line, sizeof(line), "popup %u %d,%d %dx%d %08x", id, x, y, w, h, argb[0]);
    record(line);
    return 0;
}
static void remove_popup(void *instance, uint32_t id) {
    (void)instance;
    char line[32];
    snprintf(line, sizeof(line), "unpopup %u", id);
    record(line);
}
#ifndef BIFROST_TEST_NO_ENTRY
const bifrost_android_window_plugin_v2 *bifrost_android_window_plugin_get_api(uint32_t host_abi) {
    static bifrost_android_window_plugin_v2 api;
    if (host_abi != BIFROST_ANDROID_WINDOW_ABI_V2 || mode("null-api")) return NULL;
    api = (bifrost_android_window_plugin_v2){2, sizeof(api), "c-fixture",
        create, destroy, open_window, close_window, resize, binding, poll_window,
        show_popup, remove_popup, set_text_input, poll_text};
    // A descriptor from before the optional popup callbacks were appended.
    if (mode("old-size")) api.struct_size = BIFROST_ANDROID_WINDOW_PLUGIN_V2_MIN_SIZE;
    // Popups but no text input (before those callbacks were appended).
    if (mode("no-text")) api.struct_size = offsetof(bifrost_android_window_plugin_v2, set_text_input);
    if (mode("bad-version")) api.abi_version = 99;
    if (mode("legacy-version")) api.abi_version = 1;
    if (mode("bad-size")) api.struct_size = 8;
    if (mode("missing-callback")) api.close = NULL;
    if (mode("empty-name")) api.name = "";
    return &api;
}
#endif
__attribute__((destructor)) static void unload(void) { record("unload"); }
