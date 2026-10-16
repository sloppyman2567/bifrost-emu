/* test_gles_egl_image.c — OES_EGL_image through GraphicThunk.
 *
 * Hosts advertise GL_OES_EGL_image, so guests (Chromium's GL bindings in
 * Android WebView) take glEGLImageTargetTexture2DOES from eglGetProcAddress
 * and call it without a NULL check. The thunk had no row for it and returned
 * NULL; the WebView GPU thread then branched to address 0. This test resolves
 * it the same way, shares a texture through an EGLImage
 * (EGL_KHR_gl_texture_2D_image) and reads the target texture back.
 *
 * Build:
 *   make cross SRC=Tests/Apps/test_gles_egl_image.c OUT=Tests/Apps/test_gles_egl_image.elf
 * Run (needs host EGL + GLES 2, GPU access; no window):
 *   ./bifrost-emu Tests/Apps/test_gles_egl_image.elf
 * Exits 77 (skip) without host EGL/GLES or the two extensions.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define EGL_NONE 0x3038
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 0x0001
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_EXTENSIONS 0x3055
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_GL_TEXTURE_2D_KHR 0x30B1
#define EGL_IMAGE_PRESERVED_KHR 0x30D2
#define GL_TEXTURE_2D 0x0DE1
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_NEAREST 0x2600
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_EXTENSIONS 0x1F03

typedef void *(*Proc)(const char *);
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
#define LOAD(h, T, name) __typeof__(T) name = (__typeof__(T))(uintptr_t)bifrost_dlsym(h, #name); \
    if (!name) { printf("SKIP: no %s\n", #name); return 77; }

int main(void) {
    /* GLES first so shared GL names resolve to the GLES rows. */
    uint64_t gles = bifrost_dlopen("libGLESv2.so.2", 1);
    if (!gles) gles = bifrost_dlopen("libGLESv2.so", 1);
    uint64_t egl = bifrost_dlopen("libEGL.so.1", 1);
    if (!egl) egl = bifrost_dlopen("libEGL.so", 1);
    uint64_t gl = bifrost_dlopen("libGL.so.1", 1);  /* glReadPixels has no GLES row */
    if (!gles || !egl || !gl) { printf("SKIP: GL thunks unavailable\n"); return 77; }
    LOAD(egl, void *(*)(uintptr_t), eglGetDisplay)
    LOAD(egl, unsigned (*)(void *, int *, int *), eglInitialize)
    LOAD(egl, unsigned (*)(unsigned), eglBindAPI)
    LOAD(egl, unsigned (*)(void *, const int *, void **, int, int *), eglChooseConfig)
    LOAD(egl, void *(*)(void *, void *, const int *), eglCreatePbufferSurface)
    LOAD(egl, void *(*)(void *, void *, void *, const int *), eglCreateContext)
    LOAD(egl, unsigned (*)(void *, void *, void *, void *), eglMakeCurrent)
    LOAD(egl, const char *(*)(void *, int), eglQueryString)
    LOAD(egl, Proc, eglGetProcAddress)
    LOAD(egl, unsigned (*)(void *), eglTerminate)
    LOAD(gles, void (*)(int, unsigned *), glGenTextures)
    LOAD(gles, void (*)(unsigned, unsigned), glBindTexture)
    LOAD(gles, void (*)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *), glTexImage2D)
    LOAD(gles, void (*)(unsigned, unsigned, int), glTexParameteri)
    LOAD(gles, void (*)(int, unsigned *), glGenFramebuffers)
    LOAD(gles, void (*)(unsigned, unsigned), glBindFramebuffer)
    LOAD(gles, void (*)(unsigned, unsigned, unsigned, unsigned, int), glFramebufferTexture2D)
    LOAD(gles, unsigned (*)(unsigned), glCheckFramebufferStatus)
    LOAD(gles, const char *(*)(unsigned), glGetString)
    LOAD(gl, void (*)(int, int, int, int, unsigned, unsigned, void *), glReadPixels)

    void *display = eglGetDisplay(0);
    int major, minor;
    if (!display || !eglInitialize(display, &major, &minor)) { printf("SKIP: no EGL display\n"); return 77; }
    const int config_attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
    void *config; int configs = 0;
    eglBindAPI(EGL_OPENGL_ES_API);
    if (!eglChooseConfig(display, config_attrs, &config, 1, &configs) || configs < 1) {
        printf("SKIP: no GLES2 pbuffer config\n"); return 77;
    }
    const int surface_attrs[] = {EGL_WIDTH, 4, EGL_HEIGHT, 4, EGL_NONE};
    const int context_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    void *surface = eglCreatePbufferSurface(display, config, surface_attrs);
    void *context = eglCreateContext(display, config, NULL, context_attrs);
    if (!surface || !context || !eglMakeCurrent(display, surface, surface, context)) {
        printf("SKIP: no GLES2 context\n"); return 77;
    }
    const char *egl_ext = eglQueryString(display, EGL_EXTENSIONS);
    const char *gl_ext = glGetString(GL_EXTENSIONS);
    if (!egl_ext || !strstr(egl_ext, "EGL_KHR_gl_texture_2D_image") ||
        !gl_ext || !strstr(gl_ext, "GL_OES_EGL_image")) {
        printf("SKIP: EGLImage extensions not advertised\n"); return 77;
    }
    void *(*create_image)(void *, void *, unsigned, void *, const int *) =
        (void *(*)(void *, void *, unsigned, void *, const int *))eglGetProcAddress("eglCreateImageKHR");
    void (*target_texture)(unsigned, void *) =
        (void (*)(unsigned, void *))eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!create_image || !target_texture) {
        printf("FAIL: advertised entry point resolved to NULL (create=%p target=%p)\n",
               (void *)create_image, (void *)target_texture);
        return 1;
    }

    static const uint8_t texels[4 * 4] = {255, 0, 0, 255,  0, 255, 0, 255,
                                          0, 0, 255, 255,  255, 255, 255, 255};
    unsigned textures[2], framebuffer;
    glGenTextures(2, textures);
    glBindTexture(GL_TEXTURE_2D, textures[0]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    const int image_attrs[] = {EGL_IMAGE_PRESERVED_KHR, 1, EGL_NONE};
    void *image = create_image(display, context, EGL_GL_TEXTURE_2D_KHR,
                               (void *)(uintptr_t)textures[0], image_attrs);
    if (!image) { printf("FAIL: eglCreateImageKHR\n"); return 1; }
    glBindTexture(GL_TEXTURE_2D, textures[1]);
    target_texture(GL_TEXTURE_2D, image);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[1], 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        printf("FAIL: EGLImage target texture is not renderable\n"); return 1;
    }
    uint8_t pixels[sizeof(texels)] = {0};
    glReadPixels(0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (memcmp(pixels, texels, sizeof(texels))) {
        printf("FAIL: EGLImage texels differ (first %u,%u,%u,%u)\n",
               pixels[0], pixels[1], pixels[2], pixels[3]);
        return 1;
    }
    eglTerminate(display);
    printf("ALL PASS\n");
    return 0;
}
