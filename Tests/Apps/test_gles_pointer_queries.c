/* test_gles_pointer_queries.c — GL queries that return a pointer must return the guest's.
 *
 * glGetBufferPointerv(GL_BUFFER_MAP_POINTER), glGetVertexAttribPointerv(GL_VERTEX_ATTRIB_ARRAY_POINTER)
 * and glGetPointerv(GL_DEBUG_CALLBACK_FUNCTION / _USER_PARAM) write a pointer into a guest
 * `void **`. The host driver writes what it holds: the host's own mapping of a mapped buffer, the
 * host-translated address of a client vertex array, and nothing for a debug callback that the thunk
 * keeps to itself. The guest compares and dereferences those values, so each must be the value it
 * was given: the pointer glMapBufferRange returned, the pointer passed to glVertexAttribPointer (or
 * its byte offset when a buffer is bound), and the callback and user parameter passed to
 * glDebugMessageCallback.
 *
 * Build:
 *   make cross SRC=Tests/Apps/test_gles_pointer_queries.c OUT=Tests/Apps/test_gles_pointer_queries.elf
 * Run (needs host EGL + GLES 3, GPU access; no window):
 *   ./bifrost-emu Tests/Apps/test_gles_pointer_queries.elf
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
#define GL_ARRAY_BUFFER 0x8892
#define GL_STATIC_DRAW 0x88E4
#define GL_FLOAT 0x1406
#define GL_MAP_WRITE_BIT 0x0002
#define GL_BUFFER_MAP_POINTER 0x88BD
#define GL_VERTEX_ATTRIB_ARRAY_POINTER 0x8645
#define GL_DEBUG_CALLBACK_FUNCTION 0x8244
#define GL_DEBUG_CALLBACK_USER_PARAM 0x8245

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
FN(void (*)(int, unsigned *), glGenBuffers)
FN(void (*)(unsigned, unsigned), glBindBuffer)
FN(void (*)(unsigned, intptr_t, const void *, unsigned), glBufferData)
FN(void *(*)(unsigned, intptr_t, intptr_t, unsigned), glMapBufferRange)
FN(unsigned (*)(unsigned), glUnmapBuffer)
FN(void (*)(unsigned, unsigned, void **), glGetBufferPointerv)
FN(void (*)(unsigned, int, unsigned, unsigned char, int, const void *), glVertexAttribPointer)
FN(void (*)(unsigned, unsigned, void **), glGetVertexAttribPointerv)
FN(unsigned (*)(void), glGetError)

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void debug_callback(unsigned source, unsigned type, unsigned id, unsigned severity,
                           int length, const char *message, const void *user) {
    (void)source; (void)type; (void)id; (void)severity; (void)length; (void)message; (void)user;
}

int main(void) {
    uint64_t gles = bifrost_dlopen("libGLESv2.so.2", 1);
    if (!gles) gles = bifrost_dlopen("libGLESv2.so", 1);
    uint64_t egl = bifrost_dlopen("libEGL.so.1", 1);
    if (!egl) egl = bifrost_dlopen("libEGL.so", 1);
    if (!gles || !egl) { printf("SKIP: GL thunks unavailable\n"); return 77; }
    LOAD(egl, eglGetDisplay) LOAD(egl, eglInitialize) LOAD(egl, eglBindAPI)
    LOAD(egl, eglChooseConfig) LOAD(egl, eglCreatePbufferSurface) LOAD(egl, eglCreateContext)
    LOAD(egl, eglMakeCurrent) LOAD(egl, eglTerminate)
    LOAD(gles, glGenBuffers) LOAD(gles, glBindBuffer) LOAD(gles, glBufferData)
    LOAD(gles, glMapBufferRange) LOAD(gles, glUnmapBuffer) LOAD(gles, glGetBufferPointerv)
    LOAD(gles, glVertexAttribPointer) LOAD(gles, glGetVertexAttribPointerv) LOAD(gles, glGetError)

    int major, minor, configs = 0;
    void *display = eglGetDisplay(0), *config = NULL;
    if (!display || !eglInitialize(display, &major, &minor)) { printf("SKIP: no EGL display\n"); return 77; }
    const int config_attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
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

    /* A mapped buffer: the query returns the pointer the map returned. */
    unsigned buffer;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, 256, NULL, GL_STATIC_DRAW);
    void *query = (void *)(uintptr_t)1;
    glGetBufferPointerv(GL_ARRAY_BUFFER, GL_BUFFER_MAP_POINTER, &query);
    CHECK(query == NULL, "an unmapped buffer reports %p, want NULL", query);
    void *mapped = glMapBufferRange(GL_ARRAY_BUFFER, 16, 64, GL_MAP_WRITE_BIT);
    CHECK(mapped != NULL, "glMapBufferRange failed (error %#x)", glGetError());
    if (mapped) {
        memset(mapped, 0x5A, 64);
        query = NULL;
        glGetBufferPointerv(GL_ARRAY_BUFFER, GL_BUFFER_MAP_POINTER, &query);
        CHECK(query == mapped, "GL_BUFFER_MAP_POINTER is %p, glMapBufferRange returned %p", query, mapped);
        glUnmapBuffer(GL_ARRAY_BUFFER);
        query = (void *)(uintptr_t)1;
        glGetBufferPointerv(GL_ARRAY_BUFFER, GL_BUFFER_MAP_POINTER, &query);
        CHECK(query == NULL, "after the unmap the pointer is %p, want NULL", query);
    }

    /* Vertex attribute pointers: a client array (no buffer bound) and a byte offset (buffer bound). */
    static float vertices[12];
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(1, 3, GL_FLOAT, 0, 0, vertices);
    query = NULL;
    glGetVertexAttribPointerv(1, GL_VERTEX_ATTRIB_ARRAY_POINTER, &query);
    CHECK(query == (void *)vertices, "client array pointer is %p, passed %p", query, (void *)vertices);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glVertexAttribPointer(2, 4, GL_FLOAT, 0, 0, (const void *)(uintptr_t)16);
    query = NULL;
    glGetVertexAttribPointerv(2, GL_VERTEX_ATTRIB_ARRAY_POINTER, &query);
    CHECK(query == (void *)(uintptr_t)16, "buffer-offset pointer is %p, passed 0x10", query);

    /* The debug callback is the thunk's to keep (the host driver never sees it): ES 3.2 or KHR_debug. */
    void (*glDebugMessageCallback)(void (*)(unsigned, unsigned, unsigned, unsigned, int, const char *, const void *), const void *) =
        (void (*)(void (*)(unsigned, unsigned, unsigned, unsigned, int, const char *, const void *), const void *))
            (uintptr_t)bifrost_dlsym(gles, "glDebugMessageCallback");
    void (*glGetPointerv)(unsigned, void **) =
        (void (*)(unsigned, void **))(uintptr_t)bifrost_dlsym(gles, "glGetPointerv");
    if (glDebugMessageCallback && glGetPointerv) {
        glDebugMessageCallback(debug_callback, (const void *)(uintptr_t)0x1234);
        if (glGetError() == 0) {
            query = (void *)(uintptr_t)1;
            glGetPointerv(GL_DEBUG_CALLBACK_FUNCTION, &query);
            CHECK(query == (void *)debug_callback, "GL_DEBUG_CALLBACK_FUNCTION is %p, set %p", query, (void *)debug_callback);
            query = (void *)(uintptr_t)1;
            glGetPointerv(GL_DEBUG_CALLBACK_USER_PARAM, &query);
            CHECK(query == (void *)(uintptr_t)0x1234, "GL_DEBUG_CALLBACK_USER_PARAM is %p, set 0x1234", query);
        } else {
            printf("note: glDebugMessageCallback is not available in this context, skipped\n");
        }
    } else {
        printf("note: no glDebugMessageCallback/glGetPointerv, skipped\n");
    }

    eglTerminate(display);
    if (failures) return 1;
    printf("ALL PASS\n");
    return 0;
}
