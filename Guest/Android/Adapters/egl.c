// Android EGL entry points whose ABI cannot be forwarded to a desktop driver.
// The shader cache is optional: callbacks are registered but desktop drivers
// retain their own caches. Never hand an AArch64 callback to a host driver.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

typedef void *Display;
typedef void *Surface;
typedef void *Image;
typedef void (*Proc)(void);
extern const char *thunk_eglQueryString(Display, int);
extern Proc thunk_eglGetProcAddress(const char *);
extern int thunk_eglGetError(void);
extern unsigned thunk_eglTerminate(Display);
extern Image thunk_eglCreateImage(Display, void *, unsigned, void *, const intptr_t *);
extern Image thunk_eglCreateImageKHR(Display, void *, unsigned, void *, const int *);
extern Display eglGetDisplay(void *);
extern Surface eglCreateWindowSurface(Display, void *, void *, const int *);
extern unsigned eglQuerySurface(Display, Surface, int, int *);

enum { SUCCESS = 0x3000, BAD_ALLOC = 0x3003, BAD_ATTRIBUTE = 0x3004,
       BAD_MATCH = 0x3009, BAD_NATIVE_PIXMAP = 0x300A, BAD_PARAMETER = 0x300C,
       NONE = 0x3038, VERSION = 0x3054, EXTENSIONS = 0x3055 };
static _Thread_local int error;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
struct display_state {
    Display display;
    int cache_registered;
    Proc set, get;
    char *extensions;
    struct display_state *next;
};
static struct display_state *displays;

static void fail(int value) { (void)thunk_eglGetError(); error = value; }
int eglGetError(void) {
    int host = thunk_eglGetError(), result = error ? error : host;
    error = 0;
    return result;
}
static struct display_state *state(Display display) {
    for (struct display_state *s = displays; s; s = s->next)
        if (s->display == display) return s;
    struct display_state *s = calloc(1, sizeof(*s));
    if (s) { s->display = display; s->next = displays; displays = s; }
    return s;
}
// Exporting a symbol does not advertise an extension. Keep only contracts
// supported by the adapter, rather than exposing host DMA-BUF FDs, native
// pixmaps, native buffers or EGLStream handles to Android code.
static int supported(const char *word, size_t length) {
    static const char *const names[] = {
        "EGL_KHR_config_attribs", "EGL_KHR_context_flush_control",
        "EGL_KHR_create_context", "EGL_KHR_create_context_no_error",
        "EGL_KHR_fence_sync", "EGL_KHR_reusable_sync", "EGL_KHR_wait_sync",
        "EGL_KHR_surfaceless_context", "EGL_KHR_no_config_context",
        "EGL_MESA_configless_context", "EGL_KHR_get_all_proc_addresses",
        "EGL_KHR_gl_colorspace", "EGL_KHR_image_base", "EGL_KHR_gl_texture_2D_image",
        "EGL_KHR_gl_texture_cubemap_image", "EGL_KHR_gl_texture_3D_image",
        "EGL_KHR_gl_renderbuffer_image", "EGL_KHR_surfaceless_context",
        "EGL_EXT_buffer_age", "EGL_KHR_partial_update", "EGL_KHR_swap_buffers_with_damage",
        "EGL_EXT_present_opaque", "EGL_EXT_create_context_robustness",
        "EGL_EXT_pixel_format_float", "EGL_KHR_mutable_render_buffer"
    };
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (strlen(names[i]) == length && !memcmp(word, names[i], length)) return 1;
    return 0;
}
const char *eglQueryString(Display display, int name) {
    const char *source = thunk_eglQueryString(display, name);
    if (!source || name != EXTENSIONS) return source;
    pthread_mutex_lock(&mutex);
    struct display_state *s = state(display);
    if (s && !s->extensions) {
        const char *extra = display ? "EGL_ANDROID_blob_cache EGL_NV_system_time" : "";
        s->extensions = malloc(strlen(source) + strlen(extra) + 2);
        if (s->extensions) {
            char *out = s->extensions;
            for (const char *p = source; *p;) {
                while (*p == ' ') ++p;
                const char *begin = p;
                while (*p && *p != ' ') ++p;
                size_t n = (size_t)(p - begin);
                if (n && supported(begin, n)) {
                    memcpy(out, begin, n); out += n; *out++ = ' ';
                }
            }
            strcpy(out, extra);
            if (!*extra && out != s->extensions) out[-1] = 0;
        }
    }
    const char *result = s ? s->extensions : NULL;
    pthread_mutex_unlock(&mutex);
    if (!result) fail(BAD_ALLOC);
    return result;
}
const char *eglQueryStringImplementationANDROID(Display d, int n) { return eglQueryString(d, n); }
unsigned eglTerminate(Display display) {
    unsigned result = thunk_eglTerminate(display);
    if (!result) return result;
    pthread_mutex_lock(&mutex);
    struct display_state **link = &displays;
    while (*link && (*link)->display != display) link = &(*link)->next;
    if (*link) {
        struct display_state *s = *link; *link = s->next;
        free(s->extensions); free(s);
    }
    pthread_mutex_unlock(&mutex);
    return result;
}
void eglSetBlobCacheFuncsANDROID(Display display, Proc set, Proc get) {
    if (!thunk_eglQueryString(display, VERSION)) return;
    if (!set || !get) { fail(BAD_PARAMETER); return; }
    pthread_mutex_lock(&mutex);
    struct display_state *s = state(display);
    int failure = !s ? BAD_ALLOC : s->cache_registered ? BAD_PARAMETER : 0;
    if (!failure) { s->cache_registered = 1; s->set = set; s->get = get; }
    pthread_mutex_unlock(&mutex);
    if (failure) fail(failure);
}
static int attributes(const intptr_t *source, int out[257]) {
    if (!source) { out[0] = NONE; return 1; }
    for (int i = 0; i < 256; i += 2) {
        if (source[i] == NONE) { out[i] = NONE; return 1; }
        if (source[i] < INT32_MIN || source[i] > INT32_MAX ||
                source[i + 1] < INT32_MIN || source[i + 1] > INT32_MAX) return 0;
        out[i] = (int)source[i]; out[i + 1] = (int)source[i + 1];
    }
    return 0;
}
Display eglGetPlatformDisplay(unsigned platform, void *native, const intptr_t *attrs) {
    // Android has no native display handle. Its display zero maps to the
    // owned host connection through the existing eglGetDisplay adapter.
    if (platform != 0x3141 || native) { fail(BAD_PARAMETER); return NULL; }
    if (attrs && attrs[0] != NONE) { fail(BAD_ATTRIBUTE); return NULL; }
    return eglGetDisplay(NULL);
}
Surface eglCreatePlatformWindowSurface(Display d, void *config, void *window, const intptr_t *attrs) {
    int converted[257];
    if (!attributes(attrs, converted)) { fail(BAD_ATTRIBUTE); return NULL; }
    return eglCreateWindowSurface(d, config, window, converted);
}
Image eglCreateImage(Display d, void *c, unsigned target, void *buffer, const intptr_t *attrs) {
    if (target == 0x3140 || target == 0x3270 || target == 0x30B0) { fail(BAD_PARAMETER); return NULL; }
    return thunk_eglCreateImage(d, c, target, buffer, attrs);
}
Image eglCreateImageKHR(Display d, void *c, unsigned target, void *buffer, const int *attrs) {
    if (target == 0x3140 || target == 0x3270 || target == 0x30B0) { fail(BAD_PARAMETER); return NULL; }
    return thunk_eglCreateImageKHR(d, c, target, buffer, attrs);
}
uint64_t eglGetSystemTimeFrequencyNV(void) { return 1000000000; }
uint64_t eglGetSystemTimeNV(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000000000 + (uint64_t)t.tv_nsec;
}
// These optional native contracts require an Android buffer allocator,
// SurfaceFlinger, or an EGLStream driver. Fail explicitly and do not advertise
// their extensions. In particular never treat a guest FD as a host FD.
unsigned eglCopyBuffers(Display d, Surface s, void *p) {
    (void)d; (void)s; (void)p; fail(BAD_NATIVE_PIXMAP); return 0;
}
Surface eglCreatePixmapSurface(Display d, void *c, void *p, const int *a) {
    (void)d; (void)c; (void)p; (void)a; fail(BAD_NATIVE_PIXMAP); return NULL;
}
Surface eglCreatePlatformPixmapSurface(Display d, void *c, void *p, const intptr_t *a) {
    (void)d; (void)c; (void)p; (void)a; fail(BAD_NATIVE_PIXMAP); return NULL;
}
void *eglCreateNativeClientBufferANDROID(const int *a) { (void)a; fail(BAD_ALLOC); return NULL; }
void *eglGetNativeClientBufferANDROID(void *a) { (void)a; fail(BAD_PARAMETER); return NULL; }
int eglDupNativeFenceFDANDROID(Display d, void *s) { (void)d; (void)s; fail(BAD_PARAMETER); return -1; }
unsigned eglPresentationTimeANDROID(Display d, Surface s, int64_t t) {
    (void)t;
    int width;
    if (!eglQuerySurface(d, s, 0x3057, &width)) return 0;
    fail(BAD_MATCH); return 0;
}
void *eglCreateStreamKHR(Display d, const int *a) { (void)d; (void)a; fail(BAD_MATCH); return NULL; }
void *eglCreateStreamFromFileDescriptorKHR(Display d, int fd) { (void)d; (void)fd; fail(BAD_MATCH); return NULL; }
Surface eglCreateStreamProducerSurfaceKHR(Display d, void *c, void *s, const int *a) {
    (void)d; (void)c; (void)s; (void)a; fail(BAD_MATCH); return NULL;
}
int eglGetStreamFileDescriptorKHR(Display d, void *s) { (void)d; (void)s; fail(BAD_MATCH); return -1; }
unsigned eglStreamAttribKHR(Display d, void *s, int a, int v) {
    (void)d; (void)s; (void)a; (void)v; fail(BAD_MATCH); return 0;
}
#define STREAM_QUERY(name, type) \
    unsigned name(Display d, void *s, int a, type *v) { \
        (void)d; (void)s; (void)a; (void)v; fail(BAD_MATCH); return 0; }
STREAM_QUERY(eglQueryStreamKHR, int)
STREAM_QUERY(eglQueryStreamTimeKHR, uint64_t)
STREAM_QUERY(eglQueryStreamu64KHR, uint64_t)
#define STREAM_ACTION(name) \
    unsigned name(Display d, void *s) { (void)d; (void)s; fail(BAD_MATCH); return 0; }
STREAM_ACTION(eglDestroyStreamKHR)
STREAM_ACTION(eglStreamConsumerAcquireKHR)
STREAM_ACTION(eglStreamConsumerGLTextureExternalKHR)
STREAM_ACTION(eglStreamConsumerReleaseKHR)
unsigned eglLockSurfaceKHR(Display d, Surface s, const int *a) {
    (void)d; (void)s; (void)a; fail(BAD_MATCH); return 0;
}
unsigned eglUnlockSurfaceKHR(Display d, Surface s) { (void)d; (void)s; fail(BAD_MATCH); return 0; }

Proc eglGetProcAddress(const char *name) {
    if (!name) return NULL;
    // Keep direct linkage and dynamic lookup on the same guest wrappers.
    // This list is consumed by the adapter builder as well.
#define EGL_WRAPPER(fn) if (!strcmp(name, #fn)) return (Proc)fn;
    EGL_WRAPPER(eglGetError)
    EGL_WRAPPER(eglGetProcAddress)
    EGL_WRAPPER(eglQueryString)
    EGL_WRAPPER(eglQueryStringImplementationANDROID)
    EGL_WRAPPER(eglTerminate)
    EGL_WRAPPER(eglSetBlobCacheFuncsANDROID)
    EGL_WRAPPER(eglGetPlatformDisplay)
    EGL_WRAPPER(eglCreatePlatformWindowSurface)
    EGL_WRAPPER(eglCreatePlatformPixmapSurface)
    EGL_WRAPPER(eglCopyBuffers)
    EGL_WRAPPER(eglCreatePixmapSurface)
    EGL_WRAPPER(eglCreateImage)
    EGL_WRAPPER(eglCreateImageKHR)
    EGL_WRAPPER(eglCreateNativeClientBufferANDROID)
    EGL_WRAPPER(eglGetNativeClientBufferANDROID)
    EGL_WRAPPER(eglDupNativeFenceFDANDROID)
    EGL_WRAPPER(eglPresentationTimeANDROID)
    EGL_WRAPPER(eglGetSystemTimeFrequencyNV)
    EGL_WRAPPER(eglGetSystemTimeNV)
    EGL_WRAPPER(eglCreateStreamKHR)
    EGL_WRAPPER(eglCreateStreamFromFileDescriptorKHR)
    EGL_WRAPPER(eglCreateStreamProducerSurfaceKHR)
    EGL_WRAPPER(eglGetStreamFileDescriptorKHR)
    EGL_WRAPPER(eglStreamAttribKHR)
    EGL_WRAPPER(eglQueryStreamKHR)
    EGL_WRAPPER(eglQueryStreamTimeKHR)
    EGL_WRAPPER(eglQueryStreamu64KHR)
    EGL_WRAPPER(eglDestroyStreamKHR)
    EGL_WRAPPER(eglStreamConsumerAcquireKHR)
    EGL_WRAPPER(eglStreamConsumerGLTextureExternalKHR)
    EGL_WRAPPER(eglStreamConsumerReleaseKHR)
    EGL_WRAPPER(eglLockSurfaceKHR)
    EGL_WRAPPER(eglUnlockSurfaceKHR)
#undef EGL_WRAPPER
    return thunk_eglGetProcAddress(name);
}
