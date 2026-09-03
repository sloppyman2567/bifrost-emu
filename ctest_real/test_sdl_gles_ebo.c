/* test_sdl_gles_ebo.c — GLES draw-row EBO-offset test for GraphicThunk.
 *
 * The GLES glDrawElements* rows spelled their indices pointer `p` with
 * policy `-`, so the generic loop bounced nonzero EBO offsets as guest
 * pointers (host GL then read a ~2^47 offset). They now use EL_PTR like
 * the GL rows. This test pins that: EBO-bound draw at byte offset 12
 * through the GLES glDrawElementsBaseVertex row must rasterize.
 *
 * Trick: dlsym checks the global table first, so libGLESv2 is dlopened
 * FIRST — shared GL/GLES names then resolve to the GLES trampolines and
 * this test really exercises the GLES rows. (A test that dlopens libGL
 * first silently tests the GL rows instead.)
 *
 * Build:
 *   make cross SRC=ctest_real/test_sdl_gles_ebo.c OUT=ctest_real/test_sdl_gles_ebo.elf
 *
 * Run (needs host SDL2 + GL ES 3.x, and a display):
 *   ./bifrost-emu ctest_real/test_sdl_gles_ebo.elf
 *
 * Headless / no ES3: exits 77 (skip) instead of failing CI.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_TRIANGLES        0x0004
#define GL_ARRAY_BUFFER     0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STATIC_DRAW      0x88E4
#define GL_FLOAT            0x1406
#define GL_UNSIGNED_INT     0x1405
#define GL_RGBA             0x1908
#define GL_UNSIGNED_BYTE    0x1401
#define GL_VERTEX_SHADER    0x8B31
#define GL_FRAGMENT_SHADER  0x8B30
#define GL_COMPILE_STATUS   0x8B81
#define GL_LINK_STATUS      0x8B82
#define GL_FALSE            0
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
typedef const char*  (*SDL_GetError_t)(void);

typedef void (*glClear_t)(unsigned);
typedef void (*glClearColor_t)(float, float, float, float);
typedef void (*glViewport_t)(int, int, int, int);
typedef unsigned (*glCreateShader_t)(unsigned);
typedef void (*glShaderSource_t)(unsigned, int, const char* const*, const int*);
typedef void (*glCompileShader_t)(unsigned);
typedef void (*glGetShaderiv_t)(unsigned, unsigned, int*);
typedef unsigned (*glCreateProgram_t)(void);
typedef void (*glAttachShader_t)(unsigned, unsigned);
typedef void (*glLinkProgram_t)(unsigned);
typedef void (*glGetProgramiv_t)(unsigned, unsigned, int*);
typedef void (*glDeleteShader_t)(unsigned);
typedef void (*glUseProgram_t)(unsigned);
typedef void (*glDeleteProgram_t)(unsigned);
typedef int  (*glGetUniformLocation_t)(unsigned, const char*);
typedef void (*glUniformMatrix4fv_t)(int, int, unsigned char, const float*);
typedef void (*glUniform4f_t)(int, float, float, float, float);
typedef void (*glGenVertexArrays_t)(int, unsigned*);
typedef void (*glBindVertexArray_t)(unsigned);
typedef void (*glDeleteVertexArrays_t)(int, const unsigned*);
typedef void (*glGenBuffers_t)(int, unsigned*);
typedef void (*glBindBuffer_t)(unsigned, unsigned);
typedef void (*glBufferData_t)(unsigned, long, const void*, unsigned);
typedef void (*glDeleteBuffers_t)(int, const unsigned*);
typedef void (*glEnableVertexAttribArray_t)(unsigned);
typedef void (*glVertexAttribPointer_t)(unsigned, int, unsigned, unsigned char, int, const void*);
typedef void (*glDrawElementsBaseVertex_t)(unsigned, int, unsigned, const void*, int);
typedef void (*glReadPixels_t)(int, int, int, int, unsigned, unsigned, void*);
typedef void (*glFlush_t)(void);
typedef unsigned (*glGetError_t)(void);

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

static const char* vs_src =
    "#version 300 es\n"
    "layout(location=0) in vec2 aPos;\n"
    "uniform mat4 uMVP;\n"
    "void main(){ gl_Position = uMVP * vec4(aPos, 0.0, 1.0); }";

static const char* fs_src =
    "#version 300 es\n"
    "precision mediump float;\n"
    "uniform vec4 uColor;\n"
    "out vec4 frag;\n"
    "void main(){ frag = uColor; }";

int main(void) {
    printf("test_sdl_gles_ebo: start\n");

    /* GLES FIRST: dlsym serves the global table first-wins, so this
     * order makes shared names resolve to the GLES trampolines. */
    uint64_t hgles = bifrost_dlopen("libGLESv2.so.2", 1);
    if (!hgles) hgles = bifrost_dlopen("libGLESv2.so", 1);
    if (!hgles) {
        printf("test_sdl_gles_ebo: SKIP (libGLESv2 thunk unavailable)\n");
        return 77;
    }
    uint64_t hsdl = bifrost_dlopen("libSDL2-2.0.so.0", 1);
    if (!hsdl) hsdl = bifrost_dlopen("libSDL2.so.0", 1);
    if (!hsdl) hsdl = bifrost_dlopen("libSDL2.so", 1);
    if (!hsdl) {
        printf("test_sdl_gles_ebo: SKIP (libSDL2 thunk unavailable)\n");
        return 77;
    }
    /* libGL handle: only needed for glReadPixels (no GLES row). */
    uint64_t hgl = bifrost_dlopen("libGL.so.1", 1);
    if (!hgl) hgl = bifrost_dlopen("libGL.so", 1);
    if (!hgl) {
        printf("test_sdl_gles_ebo: SKIP (libGL thunk unavailable)\n");
        return 77;
    }

    SDL_Init_t SDL_Init; SDL_Quit_t SDL_Quit;
    SDL_CreateWindow_t SDL_CreateWindow; SDL_DestroyWindow_t SDL_DestroyWindow;
    SDL_GL_CreateContext_t SDL_GL_CreateContext;
    SDL_GL_MakeCurrent_t SDL_GL_MakeCurrent;
    SDL_GL_SetAttribute_t SDL_GL_SetAttribute;
    SDL_PollEvent_t SDL_PollEvent; SDL_GetError_t SDL_GetError;
    LOAD(hsdl, SDL_Init_t, SDL_Init); LOAD(hsdl, SDL_Quit_t, SDL_Quit);
    LOAD(hsdl, SDL_CreateWindow_t, SDL_CreateWindow);
    LOAD(hsdl, SDL_DestroyWindow_t, SDL_DestroyWindow);
    LOAD(hsdl, SDL_GL_CreateContext_t, SDL_GL_CreateContext);
    LOAD(hsdl, SDL_GL_MakeCurrent_t, SDL_GL_MakeCurrent);
    LOAD(hsdl, SDL_GL_SetAttribute_t, SDL_GL_SetAttribute);
    LOAD(hsdl, SDL_PollEvent_t, SDL_PollEvent);
    LOAD(hsdl, SDL_GetError_t, SDL_GetError);

    glClear_t glClear; glClearColor_t glClearColor; glViewport_t glViewport;
    glCreateShader_t glCreateShader; glShaderSource_t glShaderSource;
    glCompileShader_t glCompileShader; glGetShaderiv_t glGetShaderiv;
    glCreateProgram_t glCreateProgram; glAttachShader_t glAttachShader;
    glLinkProgram_t glLinkProgram; glGetProgramiv_t glGetProgramiv;
    glDeleteShader_t glDeleteShader; glUseProgram_t glUseProgram;
    glDeleteProgram_t glDeleteProgram;
    glGetUniformLocation_t glGetUniformLocation;
    glUniformMatrix4fv_t glUniformMatrix4fv; glUniform4f_t glUniform4f;
    glGenVertexArrays_t glGenVertexArrays; glBindVertexArray_t glBindVertexArray;
    glDeleteVertexArrays_t glDeleteVertexArrays;
    glGenBuffers_t glGenBuffers; glBindBuffer_t glBindBuffer;
    glBufferData_t glBufferData; glDeleteBuffers_t glDeleteBuffers;
    glEnableVertexAttribArray_t glEnableVertexAttribArray;
    glVertexAttribPointer_t glVertexAttribPointer;
    glDrawElementsBaseVertex_t glDrawElementsBaseVertex;
    glFlush_t glFlush; glGetError_t glGetError;
    glReadPixels_t glReadPixels;
    LOAD(hgles, glClear_t, glClear); LOAD(hgles, glClearColor_t, glClearColor);
    LOAD(hgles, glViewport_t, glViewport);
    LOAD(hgles, glCreateShader_t, glCreateShader);
    LOAD(hgles, glShaderSource_t, glShaderSource);
    LOAD(hgles, glCompileShader_t, glCompileShader);
    LOAD(hgles, glGetShaderiv_t, glGetShaderiv);
    LOAD(hgles, glCreateProgram_t, glCreateProgram);
    LOAD(hgles, glAttachShader_t, glAttachShader);
    LOAD(hgles, glLinkProgram_t, glLinkProgram);
    LOAD(hgles, glGetProgramiv_t, glGetProgramiv);
    LOAD(hgles, glDeleteShader_t, glDeleteShader);
    LOAD(hgles, glUseProgram_t, glUseProgram);
    LOAD(hgles, glDeleteProgram_t, glDeleteProgram);
    LOAD(hgles, glGetUniformLocation_t, glGetUniformLocation);
    LOAD(hgles, glUniformMatrix4fv_t, glUniformMatrix4fv);
    LOAD(hgles, glUniform4f_t, glUniform4f);
    LOAD(hgles, glGenVertexArrays_t, glGenVertexArrays);
    LOAD(hgles, glBindVertexArray_t, glBindVertexArray);
    LOAD(hgles, glDeleteVertexArrays_t, glDeleteVertexArrays);
    LOAD(hgles, glGenBuffers_t, glGenBuffers);
    LOAD(hgles, glBindBuffer_t, glBindBuffer);
    LOAD(hgles, glBufferData_t, glBufferData);
    LOAD(hgles, glDeleteBuffers_t, glDeleteBuffers);
    LOAD(hgles, glEnableVertexAttribArray_t, glEnableVertexAttribArray);
    LOAD(hgles, glVertexAttribPointer_t, glVertexAttribPointer);
    LOAD(hgles, glDrawElementsBaseVertex_t, glDrawElementsBaseVertex);
    LOAD(hgles, glFlush_t, glFlush); LOAD(hgles, glGetError_t, glGetError);
    LOAD(hgl, glReadPixels_t, glReadPixels);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("test_sdl_gles_ebo: SKIP (SDL_Init failed)\n");
        return 77;
    }
    /* ES 3.0 context (attr numbers from SDL_video.h). */
    SDL_GL_SetAttribute(17 /* MAJOR */, 3);
    SDL_GL_SetAttribute(18 /* MINOR */, 0);
    SDL_GL_SetAttribute(21 /* PROFILE_MASK */, 4 /* ES */);

    void* win = SDL_CreateWindow("bifrost gles ebo",
                                 0x2FFF0000, 0x2FFF0000, 640, 480, SDL_WINDOW_OPENGL);
    if (!win) {
        printf("test_sdl_gles_ebo: SKIP (SDL_CreateWindow failed: %s)\n",
               SDL_GetError() ? SDL_GetError() : "?");
        SDL_Quit();
        return 77;
    }
    void* ctx = SDL_GL_CreateContext(win);
    if (!ctx) {
        printf("test_sdl_gles_ebo: SKIP (ES3 context unavailable: %s)\n",
               SDL_GetError() ? SDL_GetError() : "?");
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 77;
    }
    SDL_GL_MakeCurrent(win, ctx);
    glViewport(0, 0, 640, 480);

    unsigned vs = glCreateShader(GL_VERTEX_SHADER);
    {
        const char* src[1] = { vs_src };
        glShaderSource(vs, 1, src, NULL);
    }
    glCompileShader(vs);
    {
        int ok = 0;
        glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            printf("test_sdl_gles_ebo: SKIP (vs compile failed on ES3)\n");
            SDL_DestroyWindow(win);
            SDL_Quit();
            return 77;
        }
    }
    unsigned fs = glCreateShader(GL_FRAGMENT_SHADER);
    {
        const char* src[1] = { fs_src };
        glShaderSource(fs, 1, src, NULL);
    }
    glCompileShader(fs);
    {
        int ok = 0;
        glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            printf("test_sdl_gles_ebo: SKIP (fs compile failed on ES3)\n");
            SDL_DestroyWindow(win);
            SDL_Quit();
            return 77;
        }
    }
    unsigned prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    {
        int ok = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            printf("FAIL: program link\n");
            return 1;
        }
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    glUseProgram(prog);

    static const float verts[] = {
        -0.6f, -0.6f,
         0.6f, -0.6f,
         0.6f,  0.6f,
        -0.6f,  0.6f,
    };
    static const unsigned idx[] = { 0, 1, 2, 0, 2, 3 };

    unsigned vao = 0, vbo = 0, ebo = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (long)sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (const void*)0);
    glGenBuffers(1, &ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (long)sizeof(idx), idx, GL_STATIC_DRAW);

    int color_loc = glGetUniformLocation(prog, "uColor");
    if (color_loc < 0) {
        printf("FAIL: uniform location lookup\n");
        return 1;
    }
    int mvp_loc = glGetUniformLocation(prog, "uMVP");
    if (mvp_loc < 0) {
        printf("FAIL: mvp uniform location lookup\n");
        return 1;
    }
    {
        static const float ident[16] = {
            1, 0, 0, 0, 0, 1, 0, 0,
            0, 0, 1, 0, 0, 0, 0, 1,
        };
        glUniformMatrix4fv(mvp_loc, 1, GL_FALSE, ident);
    }

    /* Control: full quad at offset 0 in green. Pixel (400,140) is
     * covered only by triangle 1. If this fails, the ES pipeline
     * itself is broken, not the row under test. */
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUniform4f(color_loc, 0.0f, 1.0f, 0.0f, 1.0f);
    glDrawElementsBaseVertex(GL_TRIANGLES, 6, GL_UNSIGNED_INT,
                             (const void*)0, 0);
    glFlush();
    {
        unsigned char px[4] = {0, 0, 0, 0};
        glReadPixels(400, 140, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        printf("test_sdl_gles_ebo: control pixel r=%u g=%u b=%u\n",
               px[0], px[1], px[2]);
        if (!(px[1] > 128 && px[0] < 64 && px[2] < 64)) {
            printf("FAIL: control offset-0 draw missing (ES setup broken)\n");
            return 1;
        }
    }

    /* Second triangle (0,2,3) at index byte offset 12, drawn red.
     * Pixel (200,300) is covered only by that triangle. */
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUniform4f(color_loc, 1.0f, 0.0f, 0.0f, 1.0f);
    glDrawElementsBaseVertex(GL_TRIANGLES, 3, GL_UNSIGNED_INT,
                             (const void*)12, 0);
    glFlush();
    {
        unsigned char px[4] = {0, 0, 0, 0};
        glReadPixels(200, 300, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        printf("test_sdl_gles_ebo: ebo-offset pixel r=%u g=%u b=%u\n",
               px[0], px[1], px[2]);
        if (!(px[0] > 128 && px[1] < 64 && px[2] < 64)) {
            printf("FAIL: bound-EBO offset-12 triangle missing from readback\n");
            return 1;
        }
    }
    {
        unsigned err = glGetError();
        if (err != 0) {
            printf("FAIL: GL error %u\n", err);
            return 1;
        }
    }
    printf("test_sdl_gles_ebo: ALL PASS\n");
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(2, (const unsigned[]){ vbo, ebo });
    glDeleteProgram(prog);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
