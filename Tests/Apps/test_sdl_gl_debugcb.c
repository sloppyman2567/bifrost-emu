/* test_sdl_gl_debugcb.c — glDebugMessageCallback interception check.
 *
 * The guest callback is AArch64 code the host GL driver cannot invoke,
 * so GraphicThunk must store it and pass NULL to the host (GL_DEBUG_CB
 * policy) instead of handing the guest address over (host SIGSEGV on
 * the first driver message).
 *
 * Also exercises EXT_debug_label through eglGetProcAddress, the lookup
 * path Unity uses before reading object labels. EXT object enums differ
 * from KHR_debug's, so suffix stripping is not a valid implementation.
 * Pass: register + unregister both return with glGetError() == 0 and
 * the process survives (the pre-fix path risked a host crash).
 *
 * Build:
 *   make cross SRC=Tests/Apps/test_sdl_gl_debugcb.c OUT=Tests/Apps/test_sdl_gl_debugcb.elf
 *
 * Run (needs host SDL2 + GL, and a display):
 *   ./bifrost-emu Tests/Apps/test_sdl_gl_debugcb.elf
 *
 * Headless / no GL: exits 77 (skip) instead of failing CI.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SDL_INIT_VIDEO      0x00000020u
#define SDL_WINDOW_OPENGL   0x00000002u

typedef int          (*SDL_Init_t)(uint32_t);
typedef void         (*SDL_Quit_t)(void);
typedef void*        (*SDL_CreateWindow_t)(const char*, int, int, int, int, uint32_t);
typedef void         (*SDL_DestroyWindow_t)(void*);
typedef void*        (*SDL_GL_CreateContext_t)(void*);
typedef int          (*SDL_GL_MakeCurrent_t)(void*, void*);
typedef const char*  (*SDL_GetError_t)(void);
typedef void         (*GLDebugCb_t)(void (*cb)(void), const void*);
typedef unsigned int (*glGetError_t)(void);
typedef const unsigned char* (*glGetString_t)(unsigned int);
typedef void* (*eglGetProcAddress_t)(const char*);
typedef void (*glGenBuffers_t)(int, unsigned int*);
typedef void (*glBindBuffer_t)(unsigned int, unsigned int);
typedef void (*glLabelObjectEXT_t)(unsigned int, unsigned int, int, const char*);
typedef void (*glGetObjectLabelEXT_t)(unsigned int, unsigned int, int, int*, char*);

static uint64_t bifrost_dlopen(const char* name, uint64_t flags) {
    register uint64_t x0 __asm__("x0") = (uint64_t)(uintptr_t)name;
    register uint64_t x1 __asm__("x1") = flags;
    register uint64_t x8 __asm__("x8") = 0x1002;
    __asm__ volatile ("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}

static uint64_t bifrost_dlsym(uint64_t handle, const char* name) {
    register uint64_t x0 __asm__("x0") = handle;
    register uint64_t x1 __asm__("x1") = (uint64_t)(uintptr_t)name;
    register uint64_t x8 __asm__("x8") = 0x1003;
    __asm__ volatile ("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}

#define LOAD(h, T, name) do { \
    name = (T)(uintptr_t)bifrost_dlsym(h, #name); \
    if (!name) { printf("FAIL: dlsym %s\n", #name); return 1; } \
} while (0)

/* Guest debug callback: must never be invoked as host code. If the
 * thunk handed this address to host GL, the process would die here. */
static void guest_debug_cb(void) {
    printf("FAIL: guest debug callback invoked as host code\n");
    _exit(2);
}

int main(void) {
    printf("test_sdl_gl_debugcb: start\n");

    uint64_t hsdl = bifrost_dlopen("libSDL2-2.0.so.0", 1);
    if (!hsdl) hsdl = bifrost_dlopen("libSDL2.so", 1);
    if (!hsdl) {
        printf("test_sdl_gl_debugcb: SKIP (libSDL2 thunk unavailable)\n");
        return 77;
    }
    uint64_t hgl = bifrost_dlopen("libGL.so.1", 1);
    if (!hgl) hgl = bifrost_dlopen("libGL.so", 1);
    if (!hgl) {
        printf("test_sdl_gl_debugcb: SKIP (libGL thunk unavailable)\n");
        return 77;
    }

    SDL_Init_t SDL_Init;
    SDL_Quit_t SDL_Quit;
    SDL_CreateWindow_t SDL_CreateWindow;
    SDL_DestroyWindow_t SDL_DestroyWindow;
    SDL_GL_CreateContext_t SDL_GL_CreateContext;
    SDL_GL_MakeCurrent_t SDL_GL_MakeCurrent;
    SDL_GetError_t SDL_GetError;
    LOAD(hsdl, SDL_Init_t, SDL_Init);
    LOAD(hsdl, SDL_Quit_t, SDL_Quit);
    LOAD(hsdl, SDL_CreateWindow_t, SDL_CreateWindow);
    LOAD(hsdl, SDL_DestroyWindow_t, SDL_DestroyWindow);
    LOAD(hsdl, SDL_GL_CreateContext_t, SDL_GL_CreateContext);
    LOAD(hsdl, SDL_GL_MakeCurrent_t, SDL_GL_MakeCurrent);
    LOAD(hsdl, SDL_GetError_t, SDL_GetError);

    GLDebugCb_t glDebugMessageCallback;
    glGetError_t glGetError;
    /* KHR_debug lives in the GLES family rows: resolve it from a GLES
     * handle (mirrors test_android_surface.c's use_gl switch). */
    uint64_t hgles = bifrost_dlopen("libGLESv2.so.2", 1);
    if (!hgles) hgles = bifrost_dlopen("libGLESv2.so", 1);
    if (!hgles) {
        printf("test_sdl_gl_debugcb: SKIP (libGLESv2 thunk unavailable)\n");
        return 77;
    }
    LOAD(hgles, GLDebugCb_t, glDebugMessageCallback);
    LOAD(hgl, glGetError_t, glGetError);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("test_sdl_gl_debugcb: SKIP (SDL_Init failed: %s)\n",
               SDL_GetError() ? SDL_GetError() : "?");
        return 77;
    }
    void* win = SDL_CreateWindow("bifrost debugcb",
                                  0x2FFF0000, 0x2FFF0000,
                                  640, 480, SDL_WINDOW_OPENGL);
    if (!win) {
        printf("test_sdl_gl_debugcb: SKIP (SDL_CreateWindow failed: %s)\n",
               SDL_GetError() ? SDL_GetError() : "?");
        SDL_Quit();
        return 77;
    }
    void* ctx = SDL_GL_CreateContext(win);
    if (!ctx) {
        printf("test_sdl_gl_debugcb: SKIP (SDL_GL_CreateContext failed: %s)\n",
               SDL_GetError() ? SDL_GetError() : "?");
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 77;
    }
    SDL_GL_MakeCurrent(win, ctx);

    /* Register: must not crash, error must stay 0. */
    glDebugMessageCallback(guest_debug_cb, (const void*)0x1234);
    unsigned int e1 = glGetError();
    /* Unregister: same. */
    glDebugMessageCallback(0, 0);
    unsigned int e2 = glGetError();

    /* Check host support before testing this optional extension. */
    glGetString_t glGetString;
    LOAD(hgl, glGetString_t, glGetString);
    const char* extensions = (const char*)glGetString(0x1f03); /* GL_EXTENSIONS */
    if (extensions && strstr(extensions, "GL_EXT_debug_label")) {
        uint64_t hegl = bifrost_dlopen("libEGL.so.1", 1);
        if (!hegl) hegl = bifrost_dlopen("libEGL.so", 1);
        eglGetProcAddress_t eglGetProcAddress;
        glGenBuffers_t glGenBuffers;
        glBindBuffer_t glBindBuffer;
        LOAD(hegl, eglGetProcAddress_t, eglGetProcAddress);
        LOAD(hgl, glGenBuffers_t, glGenBuffers);
        LOAD(hgl, glBindBuffer_t, glBindBuffer);
        glLabelObjectEXT_t label = (glLabelObjectEXT_t)eglGetProcAddress("glLabelObjectEXT");
        glGetObjectLabelEXT_t get_label =
                (glGetObjectLabelEXT_t)eglGetProcAddress("glGetObjectLabelEXT");
        if (!label || !get_label) {
            printf("FAIL: advertised EXT_debug_label has missing entry points\n");
            return 1;
        }
        unsigned int buffer = 0;
        glGenBuffers(1, &buffer);
        glBindBuffer(0x8892, buffer); /* GL_ARRAY_BUFFER: instantiate the name */
        label(0x9151, buffer, 0, "buffer-ext"); /* GL_BUFFER_OBJECT_EXT */
        char text[32] = {0};
        int length = -1;
        get_label(0x9151, buffer, sizeof(text), &length, text);
        if (length != 10 || strcmp(text, "buffer-ext")) {
            printf("FAIL: EXT buffer label: length=%d text=%s\n", length, text);
            return 1;
        }
        get_label(0x9151, buffer, 4, &length, text);
        if (length != 3 || strcmp(text, "buf")) {
            printf("FAIL: truncated EXT label: length=%d text=%s\n", length, text);
            return 1;
        }
        get_label(0x9151, buffer, sizeof(text), 0, text);
        if (strcmp(text, "buffer-ext") || glGetError()) {
            printf("FAIL: EXT label with null length\n");
            return 1;
        }
        printf("test_sdl_gl_debugcb: EXT labels passed\n");
    } else {
        printf("test_sdl_gl_debugcb: EXT labels skipped (host extension unavailable)\n");
    }

    SDL_DestroyWindow(win);
    SDL_Quit();

    if (e1 != 0 || e2 != 0) {
        printf("FAIL: glGetError %u/%u after register/unregister\n", e1, e2);
        return 1;
    }
    printf("test_sdl_gl_debugcb: ALL PASS\n");
    return 0;
}
