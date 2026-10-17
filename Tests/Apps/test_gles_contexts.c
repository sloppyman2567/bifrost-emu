/* test_gles_contexts.c — GL binding state is per EGL context.
 *
 * An Android process renders with several EGL contexts on different threads
 * (the app, HWUI's RenderThread, WebView's GPU thread). GraphicThunk decides
 * from tracked bindings whether a pixel pointer is client memory or a pixel
 * unpack buffer offset. With one tracker shared by all contexts, another
 * context's glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0) made a PBO upload at
 * offset 0 look like a NULL client pointer, and the host driver copied from
 * NULL (Hungry Shark World's ad WebView crashed this way). Context A keeps a
 * PBO bound while thread B, in context B, unbinds it and uploads client
 * memory; then A uploads from the PBO at offset 0. Both read back.
 *
 * Build:
 *   make cross SRC=Tests/Apps/test_gles_contexts.c OUT=Tests/Apps/test_gles_contexts.elf CROSS_EXTRA=-pthread
 * Run (needs host EGL + GLES 3, GPU access; no window):
 *   ./bifrost-emu Tests/Apps/test_gles_contexts.elf
 * Exits 77 (skip) without host EGL/GLES 3.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define EGL_NONE 0x3038
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 0x0001
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES3_BIT 0x0040
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define GL_TEXTURE_2D 0x0DE1
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_NEAREST 0x2600
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#define GL_STATIC_DRAW 0x88E4

static uint64_t bifrost_dlopen(const char *path, uint64_t mode) {
    register uint64_t x0 __asm__("x0") = (uint64_t)(uintptr_t)path;
    register uint64_t x1 __asm__("x1") = mode;
    register uint64_t x8 __asm__("x8") = 0x1002;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}
static uint64_t bifrost_dlsym(uint64_t handle, const char *name) {
    register uint64_t x0 __asm__("x0") = handle;
    register uint64_t x1 __asm__("x1") = (uint64_t)(uintptr_t)name;
    register uint64_t x8 __asm__("x8") = 0x1003;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}
#define FN(T, name) static __typeof__(T) name;
#define LOAD(h, name) name = (__typeof__(name))(uintptr_t)bifrost_dlsym(h, #name); \
    if (!name) { printf("SKIP: no %s\n", #name); return 77; }

FN(void *(*)(uintptr_t), eglGetDisplay)
FN(unsigned (*)(void *, int *, int *), eglInitialize)
FN(unsigned (*)(unsigned), eglBindAPI)
FN(unsigned (*)(void *, const int *, void **, int, int *), eglChooseConfig)
FN(void *(*)(void *, void *, const int *), eglCreatePbufferSurface)
FN(void *(*)(void *, void *, void *, const int *), eglCreateContext)
FN(unsigned (*)(void *, void *, void *, void *), eglMakeCurrent)
FN(unsigned (*)(void *), eglTerminate)
FN(void (*)(int, unsigned *), glGenTextures)
FN(void (*)(unsigned, unsigned), glBindTexture)
FN(void (*)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *), glTexImage2D)
FN(void (*)(unsigned, unsigned, int), glTexParameteri)
FN(void (*)(int, unsigned *), glGenFramebuffers)
FN(void (*)(unsigned, unsigned), glBindFramebuffer)
FN(void (*)(unsigned, unsigned, unsigned, unsigned, int), glFramebufferTexture2D)
FN(unsigned (*)(unsigned), glCheckFramebufferStatus)
FN(void (*)(int, unsigned *), glGenBuffers)
FN(void (*)(unsigned, unsigned), glBindBuffer)
FN(void (*)(unsigned, intptr_t, const void *, unsigned), glBufferData)
FN(void (*)(int, int, int, int, unsigned, unsigned, void *), glReadPixels)

static void *display, *config;
static const uint8_t pbo_texels[16] = {255, 0, 0, 255,  0, 255, 0, 255,
                                       0, 0, 255, 255,  255, 255, 255, 255};
static const uint8_t client_texels[16] = {10, 20, 30, 255,  40, 50, 60, 255,
                                          70, 80, 90, 255,  100, 110, 120, 255};

static void *current_context(void) {
    const int surface_attrs[] = {EGL_WIDTH, 4, EGL_HEIGHT, 4, EGL_NONE};
    const int context_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    void *surface = eglCreatePbufferSurface(display, config, surface_attrs);
    void *context = eglCreateContext(display, config, NULL, context_attrs);
    return surface && context && eglMakeCurrent(display, surface, surface, context) ? context : NULL;
}

/* Upload a 2x2 RGBA texture from `pixels` (client memory or a PBO offset)
 * into a new texture and read it back through a framebuffer. */
static int upload_matches(const void *pixels, const uint8_t expected[16], const char *what) {
    unsigned texture, framebuffer;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        printf("FAIL: %s texture is not renderable\n", what);
        return 0;
    }
    uint8_t actual[16] = {0};
    glReadPixels(0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, actual);
    if (memcmp(actual, expected, sizeof(actual))) {
        printf("FAIL: %s texels differ (first %u,%u,%u,%u)\n", what,
               actual[0], actual[1], actual[2], actual[3]);
        return 0;
    }
    return 1;
}

static void *other_context(void *result) {
    *(int *)result = 0;
    if (!current_context()) { printf("FAIL: second context\n"); return NULL; }
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);  /* none was bound in this context */
    *(int *)result = upload_matches(client_texels, client_texels, "client-memory");
    eglMakeCurrent(display, NULL, NULL, NULL);
    return NULL;
}

int main(void) {
    uint64_t gles = bifrost_dlopen("libGLESv2.so.2", 1);
    if (!gles) gles = bifrost_dlopen("libGLESv2.so", 1);
    uint64_t egl = bifrost_dlopen("libEGL.so.1", 1);
    if (!egl) egl = bifrost_dlopen("libEGL.so", 1);
    uint64_t gl = bifrost_dlopen("libGL.so.1", 1);  /* glReadPixels has no GLES row */
    if (!gles || !egl || !gl) { printf("SKIP: GL thunks unavailable\n"); return 77; }
    LOAD(egl, eglGetDisplay) LOAD(egl, eglInitialize) LOAD(egl, eglBindAPI)
    LOAD(egl, eglChooseConfig) LOAD(egl, eglCreatePbufferSurface) LOAD(egl, eglCreateContext)
    LOAD(egl, eglMakeCurrent) LOAD(egl, eglTerminate)
    LOAD(gles, glGenTextures) LOAD(gles, glBindTexture) LOAD(gles, glTexImage2D)
    LOAD(gles, glTexParameteri) LOAD(gles, glGenFramebuffers) LOAD(gles, glBindFramebuffer)
    LOAD(gles, glFramebufferTexture2D) LOAD(gles, glCheckFramebufferStatus)
    LOAD(gles, glGenBuffers) LOAD(gles, glBindBuffer) LOAD(gles, glBufferData)
    LOAD(gl, glReadPixels)

    int major, minor, configs = 0;
    display = eglGetDisplay(0);
    if (!display || !eglInitialize(display, &major, &minor)) { printf("SKIP: no EGL display\n"); return 77; }
    const int config_attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
    eglBindAPI(EGL_OPENGL_ES_API);
    if (!eglChooseConfig(display, config_attrs, &config, 1, &configs) || configs < 1) {
        printf("SKIP: no GLES3 pbuffer config\n"); return 77;
    }
    if (!current_context()) { printf("SKIP: no GLES3 context\n"); return 77; }
    unsigned pbo;
    glGenBuffers(1, &pbo);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, sizeof(pbo_texels), pbo_texels, GL_STATIC_DRAW);

    int other = 0;
    pthread_t thread;
    if (pthread_create(&thread, NULL, other_context, &other) || pthread_join(thread, NULL) || !other)
        return 1;
    /* This context's PBO is still bound: pixels == 0 is its offset 0. */
    if (!upload_matches(NULL, pbo_texels, "pixel-unpack-buffer")) return 1;
    eglTerminate(display);
    printf("ALL PASS\n");
    return 0;
}
