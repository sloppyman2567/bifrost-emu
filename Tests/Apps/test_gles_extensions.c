/* test_gles_extensions.c — guest-visible OpenGL ES extensions.
 *
 * Every advertised extension's entry points must resolve: guests (Chromium,
 * Unity) call them without NULL checks. GraphicThunk generates rows for
 * registry aliases, promoted and plain commands, and hides ES extensions with
 * an unthunkable command (Include/Generated/GLExtensions.hpp). This test
 * checks the three extension queries agree, hidden extensions stay hidden,
 * common extensions resolve, and drives a few alias rows on the host GPU.
 *
 * Build:
 *   make cross SRC=Tests/Apps/test_gles_extensions.c OUT=Tests/Apps/test_gles_extensions.elf
 * Run (needs host EGL + GLES 3, GPU access; no window):
 *   ./bifrost-emu Tests/Apps/test_gles_extensions.elf
 * Exits 77 (skip) without host EGL/GLES 3.
 */
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
#define GL_EXTENSIONS 0x1F03
#define GL_NUM_EXTENSIONS 0x821D
#define GL_INVALID_VALUE 0x0501
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_EXT 0x1800
#define GL_DEBUG_SOURCE_APPLICATION 0x824A

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

/* Widely used ES extensions and all of their entry points (Khronos specs). */
static const char *const kCommon[][12] = {
    {"GL_OES_vertex_array_object", "glBindVertexArrayOES", "glDeleteVertexArraysOES",
     "glGenVertexArraysOES", "glIsVertexArrayOES"},
    {"GL_EXT_discard_framebuffer", "glDiscardFramebufferEXT"},
    {"GL_KHR_debug", "glDebugMessageControlKHR", "glDebugMessageInsertKHR",
     "glDebugMessageCallbackKHR", "glGetDebugMessageLogKHR", "glPushDebugGroupKHR",
     "glPopDebugGroupKHR", "glObjectLabelKHR", "glGetObjectLabelKHR",
     "glObjectPtrLabelKHR", "glGetObjectPtrLabelKHR", "glGetPointervKHR"},
    {"GL_EXT_disjoint_timer_query", "glGenQueriesEXT", "glDeleteQueriesEXT",
     "glBeginQueryEXT", "glEndQueryEXT", "glQueryCounterEXT", "glGetQueryObjectui64vEXT"},
    {"GL_OES_get_program_binary", "glGetProgramBinaryOES", "glProgramBinaryOES"},
    {"GL_OES_mapbuffer", "glMapBufferOES", "glUnmapBufferOES", "glGetBufferPointervOES"},
    {"GL_EXT_map_buffer_range", "glMapBufferRangeEXT", "glFlushMappedBufferRangeEXT"},
    {"GL_EXT_texture_storage", "glTexStorage2DEXT", "glTexStorage3DEXT"},
    {"GL_EXT_draw_buffers", "glDrawBuffersEXT"},
    {"GL_EXT_instanced_arrays", "glDrawArraysInstancedEXT", "glVertexAttribDivisorEXT"},
    {"GL_EXT_robustness", "glGetGraphicsResetStatusEXT", "glGetnUniformfvEXT"},
    {"GL_OES_EGL_image", "glEGLImageTargetTexture2DOES",
     "glEGLImageTargetRenderbufferStorageOES"},
};
static const char *const kHidden[] = {"GL_EXT_memory_object_fd", "GL_EXT_semaphore_fd"};

static int has(const char *list, const char *name) {
    size_t n = strlen(name);
    for (const char *p = list; (p = strstr(p, name)); p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0')) return 1;
    return 0;
}

int main(void) {
    uint64_t gles = bifrost_dlopen("libGLESv2.so.2", 1);
    if (!gles) gles = bifrost_dlopen("libGLESv2.so", 1);
    uint64_t egl = bifrost_dlopen("libEGL.so.1", 1);
    if (!egl) egl = bifrost_dlopen("libEGL.so", 1);
    if (!gles || !egl) { printf("SKIP: GL thunks unavailable\n"); return 77; }
    LOAD(egl, void *(*)(uintptr_t), eglGetDisplay)
    LOAD(egl, unsigned (*)(void *, int *, int *), eglInitialize)
    LOAD(egl, unsigned (*)(unsigned), eglBindAPI)
    LOAD(egl, unsigned (*)(void *, const int *, void **, int, int *), eglChooseConfig)
    LOAD(egl, void *(*)(void *, void *, const int *), eglCreatePbufferSurface)
    LOAD(egl, void *(*)(void *, void *, void *, const int *), eglCreateContext)
    LOAD(egl, unsigned (*)(void *, void *, void *, void *), eglMakeCurrent)
    LOAD(egl, Proc, eglGetProcAddress)
    LOAD(egl, unsigned (*)(void *), eglTerminate)
    LOAD(gles, const char *(*)(unsigned), glGetString)
    LOAD(gles, const char *(*)(unsigned, unsigned), glGetStringi)
    LOAD(gles, void (*)(unsigned, int *), glGetIntegerv)
    LOAD(gles, unsigned (*)(void), glGetError)
    LOAD(gles, void (*)(int, unsigned *), glGenFramebuffers)
    LOAD(gles, void (*)(unsigned, unsigned), glBindFramebuffer)

    void *display = eglGetDisplay(0);
    int major, minor, configs = 0;
    if (!display || !eglInitialize(display, &major, &minor)) { printf("SKIP: no EGL display\n"); return 77; }
    const int config_attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
    void *config;
    eglBindAPI(EGL_OPENGL_ES_API);
    if (!eglChooseConfig(display, config_attrs, &config, 1, &configs) || configs < 1) {
        printf("SKIP: no GLES3 pbuffer config\n"); return 77;
    }
    const int surface_attrs[] = {EGL_WIDTH, 4, EGL_HEIGHT, 4, EGL_NONE};
    const int context_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    void *surface = eglCreatePbufferSurface(display, config, surface_attrs);
    void *context = eglCreateContext(display, config, NULL, context_attrs);
    if (!surface || !context || !eglMakeCurrent(display, surface, surface, context)) {
        printf("SKIP: no GLES3 context\n"); return 77;
    }

    static char list[65536];
    const char *string = glGetString(GL_EXTENSIONS);
    if (!string || strlen(string) >= sizeof(list)) { printf("FAIL: GL_EXTENSIONS string\n"); return 1; }
    strcpy(list, string);
    int count = -1, tokens = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (const char *p = list; *p; ) {
        while (*p == ' ') p++;
        if (*p) tokens++;
        while (*p && *p != ' ') p++;
    }
    if (count != tokens) { printf("FAIL: NUM_EXTENSIONS %d, string has %d\n", count, tokens); return 1; }
    for (int i = 0; i < count; i++) {
        const char *name = glGetStringi(GL_EXTENSIONS, (unsigned)i);
        if (!name || !has(list, name)) { printf("FAIL: glGetStringi(%d) = %s\n", i, name ? name : "NULL"); return 1; }
    }
    while (glGetError()) {}
    if (glGetStringi(GL_EXTENSIONS, (unsigned)count) || glGetError() != GL_INVALID_VALUE) {
        printf("FAIL: index past the list is not GL_INVALID_VALUE\n"); return 1;
    }
    for (size_t i = 0; i < sizeof(kHidden) / sizeof(kHidden[0]); i++)
        if (has(list, kHidden[i])) { printf("FAIL: %s advertised\n", kHidden[i]); return 1; }
    int checked = 0;
    for (size_t e = 0; e < sizeof(kCommon) / sizeof(kCommon[0]); e++) {
        if (!has(list, kCommon[e][0])) continue;
        checked++;
        for (int f = 1; f < 12 && kCommon[e][f]; f++)
            if (!eglGetProcAddress(kCommon[e][f])) {
                printf("FAIL: %s advertised but %s is NULL\n", kCommon[e][0], kCommon[e][f]);
                return 1;
            }
    }

    /* Alias rows on the host GPU. */
    if (has(list, "GL_OES_vertex_array_object")) {
        void (*gen)(int, unsigned *) = (void (*)(int, unsigned *))eglGetProcAddress("glGenVertexArraysOES");
        void (*bind)(unsigned) = (void (*)(unsigned))eglGetProcAddress("glBindVertexArrayOES");
        unsigned char (*is)(unsigned) = (unsigned char (*)(unsigned))eglGetProcAddress("glIsVertexArrayOES");
        unsigned vao = 0;
        gen(1, &vao);
        bind(vao);
        if (!vao || !is(vao) || glGetError()) { printf("FAIL: OES vertex array object\n"); return 1; }
        bind(0);
    }
    if (has(list, "GL_EXT_discard_framebuffer")) {
        void (*discard)(unsigned, int, const unsigned *) =
            (void (*)(unsigned, int, const unsigned *))eglGetProcAddress("glDiscardFramebufferEXT");
        unsigned fbo, attachment = GL_COLOR_EXT;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        discard(GL_FRAMEBUFFER, 1, &attachment);
        if (glGetError()) { printf("FAIL: glDiscardFramebufferEXT\n"); return 1; }
    }
    if (has(list, "GL_KHR_debug")) {
        void (*push)(unsigned, unsigned, int, const char *) =
            (void (*)(unsigned, unsigned, int, const char *))eglGetProcAddress("glPushDebugGroupKHR");
        void (*pop)(void) = (void (*)(void))eglGetProcAddress("glPopDebugGroupKHR");
        push(GL_DEBUG_SOURCE_APPLICATION, 1, -1, "bifrost");
        pop();
        if (glGetError()) { printf("FAIL: KHR_debug group\n"); return 1; }
    }
    eglTerminate(display);
    printf("checked %d common extensions of %d advertised\nALL PASS\n", checked, count);
    return 0;
}
