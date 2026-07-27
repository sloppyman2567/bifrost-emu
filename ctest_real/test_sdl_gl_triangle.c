/* test_sdl_gl_triangle.c — SDL2 + OpenGL triangle demo for GraphicThunk.
 *
 * Uses bifrost internal syscalls 0x1002 (dlopen) / 0x1003 (dlsym) so a
 * static musl binary can resolve thunked libSDL2 / libGL symbols without
 * musl's "Dynamic loading not supported" stub.
 *
 * Build:
 *   make cross SRC=ctest_real/test_sdl_gl_triangle.c OUT=ctest_real/test_sdl_gl_triangle.elf
 *
 * Run (needs host SDL2 + GL, and a display):
 *   make USE_SDL2=1 USE_THUNK_GL=1
 *   ./bifrost-emu ctest_real/test_sdl_gl_triangle.elf
 *
 * Headless / no GL: exits 77 (skip) instead of failing CI.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_TRIANGLES        0x0004
#define SDL_INIT_VIDEO      0x00000020u
#define SDL_WINDOW_OPENGL   0x00000002u
#define SDL_QUIT            0x100

typedef struct { uint32_t type; uint8_t pad[52]; } SDL_Event;

typedef int          (*SDL_Init_t)(uint32_t);
typedef void         (*SDL_Quit_t)(void);
typedef void*        (*SDL_CreateWindow_t)(const char*, int, int, int, int, uint32_t);
typedef void         (*SDL_DestroyWindow_t)(void*);
typedef void*        (*SDL_GL_CreateContext_t)(void*);
typedef int          (*SDL_GL_MakeCurrent_t)(void*, void*);
typedef void         (*SDL_GL_SwapWindow_t)(void*);
typedef int          (*SDL_GL_SetAttribute_t)(int, int);
typedef int          (*SDL_PollEvent_t)(SDL_Event*);
typedef void         (*SDL_Delay_t)(uint32_t);
typedef const char*  (*SDL_GetError_t)(void);

typedef void (*glClear_t)(unsigned);
typedef void (*glClearColor_t)(float, float, float, float);
typedef void (*glViewport_t)(int, int, int, int);
typedef void (*glBegin_t)(unsigned);
typedef void (*glEnd_t)(void);
typedef void (*glVertex3f_t)(float, float, float);
typedef void (*glColor3f_t)(float, float, float);
typedef void (*glFlush_t)(void);
typedef unsigned (*glGetError_t)(void);
typedef const unsigned char* (*glGetString_t)(unsigned);

static uint64_t bifrost_dlopen(const char* path, uint64_t mode) {
    register uint64_t x0 __asm__("x0") = (uint64_t)(uintptr_t)path;
    register uint64_t x1 __asm__("x1") = mode;
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

int main(void) {
    printf("test_sdl_gl_triangle: start\n");

    uint64_t hsdl = bifrost_dlopen("libSDL2-2.0.so.0", 1);
    if (!hsdl) hsdl = bifrost_dlopen("libSDL2.so.0", 1);
    if (!hsdl) hsdl = bifrost_dlopen("libSDL2.so", 1);
    if (!hsdl) {
        printf("test_sdl_gl_triangle: SKIP (libSDL2 thunk unavailable)\n");
        return 77;
    }
    uint64_t hgl = bifrost_dlopen("libGL.so.1", 1);
    if (!hgl) hgl = bifrost_dlopen("libGL.so", 1);
    if (!hgl) {
        printf("test_sdl_gl_triangle: SKIP (libGL thunk unavailable)\n");
        return 77;
    }
    printf("test_sdl_gl_triangle: dlopen OK sdl=%llx gl=%llx\n",
           (unsigned long long)hsdl, (unsigned long long)hgl);

    SDL_Init_t SDL_Init;
    SDL_Quit_t SDL_Quit;
    SDL_CreateWindow_t SDL_CreateWindow;
    SDL_DestroyWindow_t SDL_DestroyWindow;
    SDL_GL_CreateContext_t SDL_GL_CreateContext;
    SDL_GL_MakeCurrent_t SDL_GL_MakeCurrent;
    SDL_GL_SwapWindow_t SDL_GL_SwapWindow;
    SDL_GL_SetAttribute_t SDL_GL_SetAttribute;
    SDL_PollEvent_t SDL_PollEvent;
    SDL_Delay_t SDL_Delay;
    SDL_GetError_t SDL_GetError;
    LOAD(hsdl, SDL_Init_t, SDL_Init);
    LOAD(hsdl, SDL_Quit_t, SDL_Quit);
    LOAD(hsdl, SDL_CreateWindow_t, SDL_CreateWindow);
    LOAD(hsdl, SDL_DestroyWindow_t, SDL_DestroyWindow);
    LOAD(hsdl, SDL_GL_CreateContext_t, SDL_GL_CreateContext);
    LOAD(hsdl, SDL_GL_MakeCurrent_t, SDL_GL_MakeCurrent);
    LOAD(hsdl, SDL_GL_SwapWindow_t, SDL_GL_SwapWindow);
    LOAD(hsdl, SDL_GL_SetAttribute_t, SDL_GL_SetAttribute);
    LOAD(hsdl, SDL_PollEvent_t, SDL_PollEvent);
    LOAD(hsdl, SDL_Delay_t, SDL_Delay);
    LOAD(hsdl, SDL_GetError_t, SDL_GetError);

    glClear_t glClear;
    glClearColor_t glClearColor;
    glViewport_t glViewport;
    glBegin_t glBegin;
    glEnd_t glEnd;
    glVertex3f_t glVertex3f;
    glColor3f_t glColor3f;
    glFlush_t glFlush;
    glGetError_t glGetError;
    glGetString_t glGetString;
    LOAD(hgl, glClear_t, glClear);
    LOAD(hgl, glClearColor_t, glClearColor);
    LOAD(hgl, glViewport_t, glViewport);
    LOAD(hgl, glBegin_t, glBegin);
    LOAD(hgl, glEnd_t, glEnd);
    LOAD(hgl, glVertex3f_t, glVertex3f);
    LOAD(hgl, glColor3f_t, glColor3f);
    LOAD(hgl, glFlush_t, glFlush);
    LOAD(hgl, glGetError_t, glGetError);
    LOAD(hgl, glGetString_t, glGetString);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        const char* err = SDL_GetError();
        printf("test_sdl_gl_triangle: SKIP (SDL_Init failed: %s)\n",
               err ? err : "?");
        return 77;
    }

    /* SDL_GL_CONTEXT_MAJOR_VERSION = 17, MINOR = 18 — request compat. */
    SDL_GL_SetAttribute(17, 2);
    SDL_GL_SetAttribute(18, 1);

    void* win = SDL_CreateWindow("bifrost triangle",
                                  0x2FFF0000 /* SDL_WINDOWPOS_CENTERED */,
                                  0x2FFF0000,
                                  640, 480, SDL_WINDOW_OPENGL);
    if (!win) {
        printf("test_sdl_gl_triangle: SKIP (SDL_CreateWindow failed: %s)\n",
               SDL_GetError() ? SDL_GetError() : "?");
        SDL_Quit();
        return 77;
    }
    void* ctx = SDL_GL_CreateContext(win);
    if (!ctx) {
        printf("test_sdl_gl_triangle: SKIP (SDL_GL_CreateContext failed: %s)\n",
               SDL_GetError() ? SDL_GetError() : "?");
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 77;
    }
    SDL_GL_MakeCurrent(win, ctx);
    glViewport(0, 0, 640, 480);

    const unsigned char* vendor = glGetString(0x1F00); /* GL_VENDOR */
    printf("test_sdl_gl_triangle: GL_VENDOR=%s\n",
           vendor ? (const char*)vendor : "(null)");

    const int frames = 30;
    for (int f = 0; f < frames; f++) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                printf("test_sdl_gl_triangle: quit event\n");
                goto done;
            }
        }
        float t = (float)f / (float)frames;
        glClearColor(0.05f, 0.05f, 0.12f + 0.2f * t, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_TRIANGLES);
        glColor3f(1.0f, 0.2f, 0.2f);
        glVertex3f(0.0f, 0.7f, 0.0f);
        glColor3f(0.2f, 1.0f, 0.2f);
        glVertex3f(-0.7f, -0.7f, 0.0f);
        glColor3f(0.2f, 0.2f, 1.0f);
        glVertex3f(0.7f, -0.7f, 0.0f);
        glEnd();
        glFlush();
        SDL_GL_SwapWindow(win);
        SDL_Delay(16);
    }

done:
    {
        unsigned err = glGetError();
        printf("test_sdl_gl_triangle: glGetError=%u after %d frames\n",
               err, frames);
        if (err != 0) {
            printf("FAIL: GL error\n");
            SDL_DestroyWindow(win);
            SDL_Quit();
            return 1;
        }
    }
    SDL_DestroyWindow(win);
    SDL_Quit();
    printf("test_sdl_gl_triangle: ALL PASS\n");
    return 0;
}
