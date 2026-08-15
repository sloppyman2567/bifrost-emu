/* test_sdl_gl_mapbuffer.c — exercises the glMapBuffer/glMapBufferRange/
 * glUnmapBuffer/glFlushMappedBufferRange bounce in GraphicThunk.
 *
 * The host glMapBuffer returns a HOST pointer the guest cannot deref, so
 * bifrost bounces through a guest-window allocation: map seeds the bounce
 * from the host buffer (GL_MAP_READ_BIT), the guest writes the bounce
 * directly, unmap copies it back (GL_MAP_WRITE_BIT). This test verifies
 * the round-trip on real GL state via glGetBufferSubData.
 *
 * Build:
 *   make cross SRC=ctest_real/test_sdl_gl_mapbuffer.c \
 *             OUT=ctest_real/test_sdl_gl_mapbuffer.elf
 *
 * Run (needs host SDL2 + GL, and a display):
 *   make USE_SDL2=1 USE_THUNK_GL=1
 *   ./bifrost-emu ctest_real/test_sdl_gl_mapbuffer.elf
 *
 * Headless / no GL: exits 77 (skip) instead of failing CI.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GL_ARRAY_BUFFER         0x8892
#define GL_STATIC_DRAW          0x88E4
#define GL_MAP_READ_BIT         0x0001
#define GL_MAP_WRITE_BIT        0x0002
#define GL_MAP_INVALIDATE_BUFFER_BIT 0x0008
#define GL_BUFFER_SIZE          0x8764
#define SDL_INIT_VIDEO          0x00000020u
#define SDL_WINDOW_OPENGL       0x00000002u

typedef int          (*SDL_Init_t)(uint32_t);
typedef void         (*SDL_Quit_t)(void);
typedef void*        (*SDL_CreateWindow_t)(const char*, int, int, int, int, uint32_t);
typedef void         (*SDL_DestroyWindow_t)(void*);
typedef void*        (*SDL_GL_CreateContext_t)(void*);
typedef int          (*SDL_GL_MakeCurrent_t)(void*, void*);
typedef int          (*SDL_GL_SetAttribute_t)(int, int);
typedef const char*  (*SDL_GetError_t)(void);

typedef void (*glGenBuffers_t)(int, uint32_t*);
typedef void (*glBindBuffer_t)(unsigned, uint32_t);
typedef void (*glBufferData_t)(unsigned, uintptr_t, const void*, unsigned);
typedef void (*glGetBufferSubData_t)(unsigned, uintptr_t, uintptr_t, void*);
typedef void* (*glMapBuffer_t)(unsigned, unsigned);
typedef void* (*glMapBufferRange_t)(unsigned, uintptr_t, uintptr_t, unsigned);
typedef int  (*glUnmapBuffer_t)(unsigned);
typedef void (*glFlushMappedBufferRange_t)(unsigned, uintptr_t, uintptr_t);
typedef void (*glGetBufferParameteriv_t)(unsigned, unsigned, int*);
typedef unsigned (*glGetError_t)(void);
typedef void (*glGetIntegerv_t)(unsigned, int*);

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

static int checks = 0;
static void chk(int ok, const char* what) {
    checks++;
    printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) exit(1);
}

int main(void) {
    printf("test_sdl_gl_mapbuffer: start\n");

    uint64_t hsdl = bifrost_dlopen("libSDL2-2.0.so.0", 1);
    if (!hsdl) hsdl = bifrost_dlopen("libSDL2.so.0", 1);
    if (!hsdl) hsdl = bifrost_dlopen("libSDL2.so", 1);
    if (!hsdl) { printf("test_sdl_gl_mapbuffer: SKIP (libSDL2)\n"); return 77; }
    uint64_t hgl = bifrost_dlopen("libGL.so.1", 1);
    if (!hgl) hgl = bifrost_dlopen("libGL.so", 1);
    if (!hgl) { printf("test_sdl_gl_mapbuffer: SKIP (libGL)\n"); return 77; }

    SDL_Init_t SDL_Init;
    SDL_Quit_t SDL_Quit;
    SDL_CreateWindow_t SDL_CreateWindow;
    SDL_DestroyWindow_t SDL_DestroyWindow;
    SDL_GL_CreateContext_t SDL_GL_CreateContext;
    SDL_GL_MakeCurrent_t SDL_GL_MakeCurrent;
    SDL_GL_SetAttribute_t SDL_GL_SetAttribute;
    SDL_GetError_t SDL_GetError;
    LOAD(hsdl, SDL_Init_t, SDL_Init);
    LOAD(hsdl, SDL_Quit_t, SDL_Quit);
    LOAD(hsdl, SDL_CreateWindow_t, SDL_CreateWindow);
    LOAD(hsdl, SDL_DestroyWindow_t, SDL_DestroyWindow);
    LOAD(hsdl, SDL_GL_CreateContext_t, SDL_GL_CreateContext);
    LOAD(hsdl, SDL_GL_MakeCurrent_t, SDL_GL_MakeCurrent);
    LOAD(hsdl, SDL_GL_SetAttribute_t, SDL_GL_SetAttribute);
    LOAD(hsdl, SDL_GetError_t, SDL_GetError);

    glGenBuffers_t glGenBuffers;
    glBindBuffer_t glBindBuffer;
    glBufferData_t glBufferData;
    glGetBufferSubData_t glGetBufferSubData;
    glMapBuffer_t glMapBuffer;
    glMapBufferRange_t glMapBufferRange;
    glUnmapBuffer_t glUnmapBuffer;
    glFlushMappedBufferRange_t glFlushMappedBufferRange;
    glGetBufferParameteriv_t glGetBufferParameteriv;
    glGetError_t glGetError;
    glGetIntegerv_t glGetIntegerv;
    LOAD(hgl, glGenBuffers_t, glGenBuffers);
    LOAD(hgl, glBindBuffer_t, glBindBuffer);
    LOAD(hgl, glBufferData_t, glBufferData);
    LOAD(hgl, glGetBufferSubData_t, glGetBufferSubData);
    LOAD(hgl, glMapBuffer_t, glMapBuffer);
    LOAD(hgl, glMapBufferRange_t, glMapBufferRange);
    LOAD(hgl, glUnmapBuffer_t, glUnmapBuffer);
    LOAD(hgl, glFlushMappedBufferRange_t, glFlushMappedBufferRange);
    LOAD(hgl, glGetBufferParameteriv_t, glGetBufferParameteriv);
    LOAD(hgl, glGetError_t, glGetError);
    LOAD(hgl, glGetIntegerv_t, glGetIntegerv);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("test_sdl_gl_mapbuffer: SKIP (SDL_Init: %s)\n",
               SDL_GetError() ? SDL_GetError() : "?");
        return 77;
    }
    SDL_GL_SetAttribute(17, 2);
    SDL_GL_SetAttribute(18, 1);
    void* win = SDL_CreateWindow("bifrost mapbuffer", 0x2FFF0000, 0x2FFF0000,
                                 320, 240, SDL_WINDOW_OPENGL);
    if (!win) { printf("SKIP (CreateWindow: %s)\n", SDL_GetError()); SDL_Quit(); return 77; }
    void* ctx = SDL_GL_CreateContext(win);
    if (!ctx) { printf("SKIP (CreateContext: %s)\n", SDL_GetError());
                SDL_DestroyWindow(win); SDL_Quit(); return 77; }
    SDL_GL_MakeCurrent(win, ctx);

    /* GL 2.1 context — check glMapBufferRange is even available. */
    int maj = 0;
    glGetIntegerv(0x821B /* GL_MAJOR_VERSION */, &maj);
    glGetError();

    uint32_t buf = 0;
    glGenBuffers(1, &buf);
    glBindBuffer(GL_ARRAY_BUFFER, buf);

    /* 64 bytes of seed data. */
    unsigned char init[64];
    for (int i = 0; i < 64; i++) init[i] = (unsigned char)(i * 3 + 1);
    glBufferData(GL_ARRAY_BUFFER, 64, init, GL_STATIC_DRAW);

    /* Whole-buffer map, read+write. The bounce should be seeded with the
     * host buffer contents (READ bit) and written back on unmap (WRITE). */
    unsigned char* p = (unsigned char*)glMapBuffer(GL_ARRAY_BUFFER,
                                                   GL_MAP_READ_BIT | GL_MAP_WRITE_BIT);
    chk(p != NULL && p != (unsigned char*)1, "glMapBuffer returns a real ptr");
    chk(memcmp(p, init, 64) == 0, "bounce seeded from host buffer (READ bit)");

    unsigned char w[64];
    for (int i = 0; i < 64; i++) w[i] = (unsigned char)(i * 7 + 5);
    memcpy(p, w, 64);

    int un = glUnmapBuffer(GL_ARRAY_BUFFER);
    chk(un == 1, "glUnmapBuffer returns GL_TRUE");

    unsigned char rb[64];
    glGetBufferSubData(GL_ARRAY_BUFFER, 0, 64, rb);
    chk(memcmp(rb, w, 64) == 0, "host buffer got bounce writeback");

    /* glMapBufferRange [16, 32): seeded from host, then flushed + written. */
    if (maj >= 3 || 1) {
        unsigned char* r = (unsigned char*)glMapBufferRange(
            GL_ARRAY_BUFFER, 16, 16, GL_MAP_READ_BIT | GL_MAP_WRITE_BIT);
        chk(r != NULL && r != (unsigned char*)1, "glMapBufferRange returns a real ptr");
        chk(memcmp(r, w + 16, 16) == 0, "range bounce seeded from host [16,32)");

        /* Explicit flush: a partial write lands before unmap. */
        for (int i = 0; i < 8; i++) r[i] = (unsigned char)(0xA0 + i);
        glFlushMappedBufferRange(GL_ARRAY_BUFFER, 16, 8);
        unsigned char fl[64];
        glGetBufferSubData(GL_ARRAY_BUFFER, 0, 64, fl);
        chk(memcmp(fl + 16, r, 8) == 0, "flush pushed first 8 bytes");
        chk(memcmp(fl + 24, w + 24, 8) == 0, "unflushed range untouched");

        /* Write the rest, then unmap → full range lands. */
        for (int i = 8; i < 16; i++) r[i] = (unsigned char)(0xC0 + i);
        glUnmapBuffer(GL_ARRAY_BUFFER);
        glGetBufferSubData(GL_ARRAY_BUFFER, 0, 64, fl);
        unsigned char want[16];
        for (int i = 0; i < 16; i++) want[i] = (unsigned char)(i < 8 ? 0xA0 + i : 0xC0 + i);
        chk(memcmp(fl + 16, want, 16) == 0, "unmap wrote back whole range [16,32)");
        chk(memcmp(fl, w, 16) == 0, "bytes before range untouched");
        chk(memcmp(fl + 32, w + 32, 32) == 0, "bytes after range untouched");
    }

    /* Buffer size query path used internally for whole-buffer maps. */
    int sz = 0;
    glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &sz);
    chk(sz == 64, "glGetBufferParameteriv GL_BUFFER_SIZE == 64");

    /* Unmapping an unmapped buffer must not crash and returns GL_TRUE. */
    int un2 = glUnmapBuffer(GL_ARRAY_BUFFER);
    chk(un2 == 1, "double unmap safe");

    chk(glGetError() == 0, "no GL error");

    printf("test_sdl_gl_mapbuffer: ALL PASS (%d checks)\n", checks);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}