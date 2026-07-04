/* test_gl_thunk.c — minimal GL test for the graphic thunk.
 *
 * Calls glClearColor + glClear + glFlush via dlsym("libGL.so.1").
 * The thunk intercepts the dlsym call and forwards it to the host's
 * libGL. This verifies the thunk plumbing works end-to-end.
 *
 * Build (static, so we don't need dynamic libc):
 *   make cross SRC=ctest_real/test_gl_thunk.c OUT=ctest_real/test_gl_thunk.elf
 *
 * Run:
 *   BIFROST_THUNK_GRAPHICS=1 BIFROST_THUNK_TRACE=1 \
 *     ./bifrost-emu ctest_real/test_gl_thunk.elf
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

typedef void (*glClearColor_t)(float, float, float, float);
typedef void (*glClear_t)(unsigned int);
typedef void (*glFlush_t)(void);
typedef unsigned int (*glGetError_t)(void);

#define GL_COLOR_BUFFER_BIT 0x00004000

int main(void) {
    printf("test_gl_thunk: opening libGL.so.1...\n");

    void *lib = dlopen("libGL.so.1", RTLD_LAZY);
    if (!lib) {
        printf("test_gl_thunk: dlopen failed: %s\n", dlerror());
        printf("test_gl_thunk: (this is expected if BIFROST_THUNK_GRAPHICS=1 "
               "is not set, or if the host has no libGL)\n");
        return 1;
    }
    printf("test_gl_thunk: dlopen OK: %p\n", lib);

    glClearColor_t pfn_clearColor = (glClearColor_t)dlsym(lib, "glClearColor");
    glClear_t      pfn_clear      = (glClear_t)dlsym(lib, "glClear");
    glFlush_t      pfn_flush      = (glFlush_t)dlsym(lib, "glFlush");
    glGetError_t   pfn_getError   = (glGetError_t)dlsym(lib, "glGetError");

    if (!pfn_clearColor || !pfn_clear || !pfn_flush || !pfn_getError) {
        printf("test_gl_thunk: dlsym failed for some symbol: %s\n", dlerror());
        dlclose(lib);
        return 1;
    }

    printf("test_gl_thunk: resolved symbols:\n");
    printf("  glClearColor -> %p\n", pfn_clearColor);
    printf("  glClear      -> %p\n", pfn_clear);
    printf("  glFlush      -> %p\n", pfn_flush);
    printf("  glGetError   -> %p\n", pfn_getError);

    printf("test_gl_thunk: calling glClearColor(0.2, 0.4, 0.6, 1.0)...\n");
    pfn_clearColor(0.2f, 0.4f, 0.6f, 1.0f);

    printf("test_gl_thunk: calling glClear(GL_COLOR_BUFFER_BIT)...\n");
    pfn_clear(GL_COLOR_BUFFER_BIT);

    printf("test_gl_thunk: calling glFlush()...\n");
    pfn_flush();

    unsigned int err = pfn_getError();
    printf("test_gl_thunk: glGetError() = %u\n", err);

    printf("test_gl_thunk: ALL PASS\n");
    dlclose(lib);
    return 0;
}
