/* test_sdl_gl_mapbuffer.c — exercises the glMapBuffer/glMapBufferRange/
 * glUnmapBuffer/glFlushMappedBufferRange bounce in GraphicThunk.
 *
 * A host mapping is a HOST pointer the guest cannot deref, so bifrost maps
 * the host buffer and hands the guest a guest-window bounce: map seeds it
 * from the host mapping (GL_MAP_READ_BIT); flush, unmap and, for persistent
 * mappings, buffer-consuming calls copy it back. Every check is valid GL
 * (GL 4.6 / ARB_buffer_storage), so the same source run natively against
 * the host driver is the reference result.
 *
 * Build:
 *   make cross SRC=Tests/Apps/test_sdl_gl_mapbuffer.c \
 *             OUT=Tests/Apps/test_sdl_gl_mapbuffer.elf
 *
 * Run (needs host SDL2 + GL, and a display):
 *   make USE_SDL2=1 USE_THUNK_GL=1
 *   ./bifrost-emu Tests/Apps/test_sdl_gl_mapbuffer.elf
 *   ./bifrost-emu Tests/Apps/test_sdl_gl_mapbuffer.elf gles
 * The GLES mode selects GLES policy entries in the same host GL context;
 * it does not create an OpenGL ES context.
 *
 * Native reference on the host driver:
 *   cc -O2 -o /tmp/mapbuffer Tests/Apps/test_sdl_gl_mapbuffer.c -ldl
 *
 * Headless / no GL: exits 77 (skip) instead of failing CI.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if !defined(__aarch64__)
#include <dlfcn.h>
#endif

#define GL_ARRAY_BUFFER         0x8892
#define GL_STATIC_DRAW          0x88E4
#define GL_INVALID_OPERATION    0x0502
#define GL_MAP_READ_BIT         0x0001
#define GL_MAP_WRITE_BIT        0x0002
#define GL_MAP_INVALIDATE_RANGE_BIT 0x0004
#define GL_MAP_FLUSH_EXPLICIT_BIT 0x0010
#define GL_MAP_PERSISTENT_BIT   0x0040
#define GL_MAP_COHERENT_BIT     0x0080
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
typedef void (*glDeleteBuffers_t)(int, const uint32_t*);
typedef void (*glBufferData_t)(unsigned, uintptr_t, const void*, unsigned);
typedef void (*glBufferStorage_t)(unsigned, uintptr_t, const void*, unsigned);
typedef void (*glGetBufferSubData_t)(unsigned, uintptr_t, uintptr_t, void*);
typedef void* (*glMapBuffer_t)(unsigned, unsigned);
typedef void* (*glMapBufferRange_t)(unsigned, uintptr_t, uintptr_t, unsigned);
typedef unsigned char (*glUnmapBuffer_t)(unsigned);
typedef void (*glFlushMappedBufferRange_t)(unsigned, uintptr_t, uintptr_t);
typedef void (*glGetBufferParameteriv_t)(unsigned, unsigned, int*);
typedef unsigned (*glGetError_t)(void);
typedef void (*glGetIntegerv_t)(unsigned, int*);
typedef const unsigned char* (*glGetStringi_t)(unsigned, unsigned);

#if defined(__aarch64__)
static uint64_t load_library(const char* path) {
    register uint64_t x0 __asm__("x0") = (uint64_t)(uintptr_t)path;
    register uint64_t x1 __asm__("x1") = 1;
    register uint64_t x8 __asm__("x8") = 0x1002;
    __asm__ volatile ("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}

static uint64_t load_symbol(uint64_t handle, const char* name) {
    register uint64_t x0 __asm__("x0") = handle;
    register uint64_t x1 __asm__("x1") = (uint64_t)(uintptr_t)name;
    register uint64_t x8 __asm__("x8") = 0x1003;
    __asm__ volatile ("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}
#else
static uint64_t load_library(const char* path) {
    return (uint64_t)(uintptr_t)dlopen(path, RTLD_NOW | RTLD_GLOBAL);
}

static uint64_t load_symbol(uint64_t handle, const char* name) {
    return (uint64_t)(uintptr_t)dlsym((void*)(uintptr_t)handle, name);
}
#endif

#define LOAD(h, T, name) do { \
    name = (T)(uintptr_t)load_symbol(h, #name); \
    if (!name) { printf("FAIL: dlsym %s\n", #name); return 1; } \
} while (0)

static int checks = 0;

/* glBufferStorage and persistent mapping: GL 4.4, or ARB_buffer_storage on an older context. Mesa raises GL_INVALID_VALUE
 * for the persistent bit without it, and a virtual GPU or a driver without the extension cannot run those checks. */
static int has_buffer_storage(glGetIntegerv_t glGetIntegerv, glGetStringi_t glGetStringi) {
    int major = 0, minor = 0, count = 0;
    glGetIntegerv(0x821B /* GL_MAJOR_VERSION */, &major);
    glGetIntegerv(0x821C /* GL_MINOR_VERSION */, &minor);
    if (major > 4 || (major == 4 && minor >= 4)) return 1;
    if (!glGetStringi) return 0;
    glGetIntegerv(0x821D /* GL_NUM_EXTENSIONS */, &count);
    for (int i = 0; i < count; i++) {
        const unsigned char* name = glGetStringi(0x1F03 /* GL_EXTENSIONS */, (unsigned)i);
        if (name && strcmp((const char*)name, "GL_ARB_buffer_storage") == 0) return 1;
    }
    return 0;
}
static void chk(int ok, const char* what) {
    checks++;
    printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) exit(1);
}

int main(int argc, char** argv) {
    printf("test_sdl_gl_mapbuffer: start\n");
    int gles = argc > 1 && strcmp(argv[1], "gles") == 0;
    /* Thunk dlsym uses the first global definition. Load GLES first in
     * this mode so overlapping names exercise its own policy entries. */
    if (gles && !load_library("libGLESv2.so")) {
        printf("test_sdl_gl_mapbuffer: SKIP (libGLESv2)\n"); return 77;
    }

    uint64_t hsdl = load_library("libSDL2-2.0.so.0");
    if (!hsdl) hsdl = load_library("libSDL2.so.0");
    if (!hsdl) hsdl = load_library("libSDL2.so");
    if (!hsdl) { printf("test_sdl_gl_mapbuffer: SKIP (libSDL2)\n"); return 77; }
    uint64_t hgl = load_library("libGL.so.1");
    if (!hgl) hgl = load_library("libGL.so");
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
    glDeleteBuffers_t glDeleteBuffers;
    glBufferData_t glBufferData;
    glBufferStorage_t glBufferStorage;
    glGetBufferSubData_t glGetBufferSubData;
    glMapBuffer_t glMapBuffer;
    glMapBufferRange_t glMapBufferRange;
    glUnmapBuffer_t glUnmapBuffer;
    glFlushMappedBufferRange_t glFlushMappedBufferRange;
    glGetBufferParameteriv_t glGetBufferParameteriv;
    glGetError_t glGetError;
    glGetIntegerv_t glGetIntegerv;
    glGetStringi_t glGetStringi;
    LOAD(hgl, glGenBuffers_t, glGenBuffers);
    LOAD(hgl, glBindBuffer_t, glBindBuffer);
    LOAD(hgl, glDeleteBuffers_t, glDeleteBuffers);
    LOAD(hgl, glBufferData_t, glBufferData);
    LOAD(hgl, glBufferStorage_t, glBufferStorage);
    LOAD(hgl, glGetBufferSubData_t, glGetBufferSubData);
    LOAD(hgl, glMapBuffer_t, glMapBuffer);
    LOAD(hgl, glMapBufferRange_t, glMapBufferRange);
    LOAD(hgl, glUnmapBuffer_t, glUnmapBuffer);
    LOAD(hgl, glFlushMappedBufferRange_t, glFlushMappedBufferRange);
    LOAD(hgl, glGetBufferParameteriv_t, glGetBufferParameteriv);
    LOAD(hgl, glGetError_t, glGetError);
    LOAD(hgl, glGetIntegerv_t, glGetIntegerv);
    glGetStringi = (glGetStringi_t)(uintptr_t)load_symbol(hgl, "glGetStringi");  /* absent before GL 3.0: no storage */

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("test_sdl_gl_mapbuffer: SKIP (SDL_Init: %s)\n",
               SDL_GetError() ? SDL_GetError() : "?");
        return 77;
    }
    void* win = SDL_CreateWindow("bifrost mapbuffer", 0x2FFF0000, 0x2FFF0000,
                                 320, 240, SDL_WINDOW_OPENGL);
    if (!win) { printf("SKIP (CreateWindow: %s)\n", SDL_GetError()); SDL_Quit(); return 77; }
    void* ctx = SDL_GL_CreateContext(win);
    if (!ctx) { printf("SKIP (CreateContext: %s)\n", SDL_GetError());
                SDL_DestroyWindow(win); SDL_Quit(); return 77; }
    SDL_GL_MakeCurrent(win, ctx);
    while (glGetError()) {}

    uint32_t buf = 0;
    glGenBuffers(1, &buf);
    glBindBuffer(GL_ARRAY_BUFFER, buf);

    /* 64 bytes of seed data. */
    unsigned char init[64];
    for (int i = 0; i < 64; i++) init[i] = (unsigned char)(i * 3 + 1);
    glBufferData(GL_ARRAY_BUFFER, 64, init, GL_STATIC_DRAW);

    /* Whole-buffer map, read+write: seeded from the buffer, written back on
     * unmap. */
    unsigned char* p = gles
        ? (unsigned char*)glMapBufferRange(GL_ARRAY_BUFFER, 0, 64,
                                          GL_MAP_READ_BIT | GL_MAP_WRITE_BIT)
        : (unsigned char*)glMapBuffer(GL_ARRAY_BUFFER, 0x88BA /* GL_READ_WRITE */);
    chk(p != NULL, "whole-buffer map returns a pointer");
    chk(memcmp(p, init, 64) == 0, "map seeded from buffer contents (READ)");
    unsigned char w[64];
    for (int i = 0; i < 64; i++) w[i] = (unsigned char)(i * 7 + 5);
    memcpy(p, w, 64);
    chk(glUnmapBuffer(GL_ARRAY_BUFFER) == 1, "glUnmapBuffer returns GL_TRUE");
    unsigned char rb[64];
    glGetBufferSubData(GL_ARRAY_BUFFER, 0, 64, rb);
    chk(memcmp(rb, w, 64) == 0, "unmap wrote the mapping back");

    /* Explicit flush of [16,32): the flush offset is relative to the
     * mapping. Unflushed modified bytes are undefined, so only the flushed
     * bytes and the bytes outside the mapping are checked. */
    unsigned char* r = (unsigned char*)glMapBufferRange(
        GL_ARRAY_BUFFER, 16, 16,
        GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT);
    chk(r != NULL, "range map returns a pointer");
    chk(memcmp(r, w + 16, 16) == 0, "range map seeded from [16,32)");
    for (int i = 0; i < 8; i++) r[i] = (unsigned char)(0xA0 + i);
    glFlushMappedBufferRange(GL_ARRAY_BUFFER, 0, 8);
    chk(glUnmapBuffer(GL_ARRAY_BUFFER) == 1, "range unmap returns GL_TRUE");
    unsigned char fl[64];
    glGetBufferSubData(GL_ARRAY_BUFFER, 0, 64, fl);
    unsigned char flushed[8];
    for (int i = 0; i < 8; i++) flushed[i] = (unsigned char)(0xA0 + i);
    chk(memcmp(fl + 16, flushed, 8) == 0, "flushed bytes landed at mapping offset 0");
    chk(memcmp(fl, w, 16) == 0, "bytes before the mapping untouched");
    chk(memcmp(fl + 32, w + 32, 32) == 0, "bytes after the mapping untouched");
    chk(glGetError() == 0, "no error from map, flush and unmap");

    /* Buffer size query path used internally for whole-buffer maps. */
    int sz = 0;
    glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &sz);
    chk(sz == 64, "glGetBufferParameteriv GL_BUFFER_SIZE == 64");

    /* Unmapping a buffer that is not mapped is an error. Its return value
     * is not checked: Mesa 26 returns GL_TRUE with the error. */
    glUnmapBuffer(GL_ARRAY_BUFFER);
    chk(glGetError() == GL_INVALID_OPERATION,
        "unmap of an unmapped buffer raises GL_INVALID_OPERATION");

    const int storage = has_buffer_storage(glGetIntegerv, glGetStringi);
    if (!storage) {
        glDeleteBuffers(1, &buf);
        printf("  SKIP persistent mapping and immutable storage: the driver lacks ARB_buffer_storage\n");
        printf("test_sdl_gl_mapbuffer: ALL PASS (%d checks, buffer-storage checks skipped)\n", checks);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 0;
    }

    /* Persistent mapping needs immutable storage created with it. */
    chk(glMapBufferRange(GL_ARRAY_BUFFER, 0, 64, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT) == NULL,
        "persistent map of glBufferData storage fails");
    chk(glGetError() == GL_INVALID_OPERATION, "...with GL_INVALID_OPERATION");
    glDeleteBuffers(1, &buf);

    /* Immutable storage without GL_DYNAMIC_STORAGE_BIT rejects
     * glBufferSubData; map/write/unmap must still land (Roblox's UI vertex
     * streams were lost this way). */
    glGenBuffers(1, &buf);
    glBindBuffer(GL_ARRAY_BUFFER, buf);
    glBufferStorage(GL_ARRAY_BUFFER, 64, init, GL_MAP_WRITE_BIT);
    unsigned char* s = (unsigned char*)glMapBufferRange(
        GL_ARRAY_BUFFER, 8, 32, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
    chk(s != NULL, "immutable storage map returns a pointer");
    for (int i = 0; i < 32; i++) s[i] = (unsigned char)(0x40 + i);
    chk(glUnmapBuffer(GL_ARRAY_BUFFER) == 1, "immutable storage unmap returns GL_TRUE");
    glGetBufferSubData(GL_ARRAY_BUFFER, 0, 64, rb);
    unsigned char want[64];
    memcpy(want, init, 64);
    for (int i = 0; i < 32; i++) want[8 + i] = (unsigned char)(0x40 + i);
    chk(memcmp(rb, want, 64) == 0, "immutable storage got the mapped writes");
    chk(glGetError() == 0, "no error on immutable storage");
    glDeleteBuffers(1, &buf);

    /* Persistent + coherent: the guest maps once and writes every frame
     * through the pointer; a consumer sees the writes while it stays
     * mapped. Unmap ends the mapping and the buffer can be mapped again. */
    {
        glGenBuffers(1, &buf);
        glBindBuffer(GL_ARRAY_BUFFER, buf);
        const unsigned flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
        glBufferStorage(GL_ARRAY_BUFFER, 64, init, flags);
        unsigned char* pm = (unsigned char*)glMapBufferRange(GL_ARRAY_BUFFER, 0, 64, flags);
        chk(pm != NULL, "persistent+coherent map returns a pointer");

        unsigned char f1[64];
        for (int i = 0; i < 64; i++) f1[i] = (unsigned char)(i + 1);
        memcpy(pm, f1, 64);
        unsigned char pc_rb[64];
        glGetBufferSubData(GL_ARRAY_BUFFER, 0, 64, pc_rb);
        chk(memcmp(pc_rb, f1, 64) == 0, "coherent write visible to a consumer while mapped");

        unsigned char f2[64];
        for (int i = 0; i < 64; i++) f2[i] = (unsigned char)(0x10 + i);
        memcpy(pm, f2, 64);
        glGetBufferSubData(GL_ARRAY_BUFFER, 0, 64, pc_rb);
        chk(memcmp(pc_rb, f2, 64) == 0, "second coherent write visible (multi-frame pattern)");

        chk(glUnmapBuffer(GL_ARRAY_BUFFER) == 1, "unmap of a persistent mapping returns GL_TRUE");
        unsigned char* again = (unsigned char*)glMapBufferRange(GL_ARRAY_BUFFER, 0, 64, flags);
        chk(again != NULL, "an unmapped persistent buffer maps again");
        chk(glUnmapBuffer(GL_ARRAY_BUFFER) == 1, "second persistent unmap returns GL_TRUE");
        glDeleteBuffers(1, &buf);
    }

    chk(glGetError() == 0, "no GL error");

    printf("test_sdl_gl_mapbuffer: ALL PASS (%d checks)\n", checks);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
