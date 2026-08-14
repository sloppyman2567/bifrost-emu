/* test_sdl_gl_vao.c — modern-OpenGL path test for GraphicThunk.
 *
 * Exercises the entry points real SDL2+GL games use: VAOs, VBOs/EBOs,
 * shaders, matrix uniforms and glDrawElements — the paths added in
 * 1.5.2.alpha (VAO registration, binding-aware glVertexAttribPointer /
 * glDrawElements, glUniformMatrix*fv pointer translation fix).
 *
 * Build:
 *   make cross SRC=ctest_real/test_sdl_gl_vao.c OUT=ctest_real/test_sdl_gl_vao.elf
 *
 * Run (needs host SDL2 + GL, and a display):
 *   ./bifrost-emu ctest_real/test_sdl_gl_vao.elf
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
#define GL_ARRAY_BUFFER     0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STATIC_DRAW      0x88E4
#define GL_FLOAT            0x1406
#define GL_UNSIGNED_INT     0x1405
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
typedef void         (*SDL_Delay_t)(uint32_t);
typedef const char*  (*SDL_GetError_t)(void);

typedef void (*glClear_t)(unsigned);
typedef void (*glClearColor_t)(float, float, float, float);
typedef void (*glViewport_t)(int, int, int, int);
typedef void (*glGetIntegerv_t)(unsigned, int*);
typedef void (*glGenVertexArrays_t)(int, unsigned*);
typedef void (*glBindVertexArray_t)(unsigned);
typedef void (*glDeleteVertexArrays_t)(int, const unsigned*);
typedef void (*glGenBuffers_t)(int, unsigned*);
typedef void (*glBindBuffer_t)(unsigned, unsigned);
typedef void (*glBufferData_t)(unsigned, long, const void*, unsigned);
typedef void (*glEnableVertexAttribArray_t)(unsigned);
typedef void (*glVertexAttribPointer_t)(unsigned, int, unsigned, unsigned char, int, const void*);
typedef void (*glDrawElements_t)(unsigned, int, unsigned, const void*);
typedef unsigned (*glCreateShader_t)(unsigned);
typedef void (*glShaderSource_t)(unsigned, int, const char* const*, const int*);
typedef void (*glCompileShader_t)(unsigned);
typedef void (*glGetShaderiv_t)(unsigned, unsigned, int*);
typedef void (*glGetShaderInfoLog_t)(unsigned, int, int*, char*);
typedef unsigned (*glCreateProgram_t)(void);
typedef void (*glAttachShader_t)(unsigned, unsigned);
typedef void (*glLinkProgram_t)(unsigned);
typedef void (*glGetProgramiv_t)(unsigned, unsigned, int*);
typedef void (*glGetProgramInfoLog_t)(unsigned, int, int*, char*);
typedef void (*glDeleteShader_t)(unsigned);
typedef void (*glUseProgram_t)(unsigned);
typedef void (*glBindAttribLocation_t)(unsigned, unsigned, const char*);
typedef int  (*glGetUniformLocation_t)(unsigned, const char*);
typedef void (*glUniformMatrix4fv_t)(int, int, unsigned char, const float*);
typedef void (*glUniform4f_t)(int, float, float, float, float);
typedef void (*glDeleteBuffers_t)(int, const unsigned*);
typedef void (*glDeleteVertexArrays_t)(int, const unsigned*);
typedef void (*glDeleteProgram_t)(unsigned);
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

static const char* vs_src =
    "#version 330 core\n"
    "layout(location=0) in vec2 aPos;\n"
    "uniform mat4 uMVP;\n"
    "void main(){ gl_Position = uMVP * vec4(aPos, 0.0, 1.0); }\n";

static const char* fs_src =
    "#version 330 core\n"
    "uniform vec4 uColor;\n"
    "out vec4 frag;\n"
    "void main(){ frag = uColor; }\n";

/* A simple orthographic-ish MVP: scale the quad to half-screen. */
static void build_mvp(float* m) {
    m[0]=1.0f; m[1]=0; m[2]=0; m[3]=0;
    m[4]=0; m[5]=1.0f; m[6]=0; m[7]=0;
    m[8]=0; m[9]=0; m[10]=1.0f; m[11]=0;
    m[12]=0; m[13]=0; m[14]=0; m[15]=1.0f;
}

int main(void) {
    printf("test_sdl_gl_vao: start\n");

    uint64_t hsdl = bifrost_dlopen("libSDL2-2.0.so.0", 1);
    if (!hsdl) hsdl = bifrost_dlopen("libSDL2.so.0", 1);
    if (!hsdl) hsdl = bifrost_dlopen("libSDL2.so", 1);
    if (!hsdl) {
        printf("test_sdl_gl_vao: SKIP (libSDL2 thunk unavailable)\n");
        return 77;
    }
    uint64_t hgl = bifrost_dlopen("libGL.so.1", 1);
    if (!hgl) hgl = bifrost_dlopen("libGL.so", 1);
    if (!hgl) {
        printf("test_sdl_gl_vao: SKIP (libGL thunk unavailable)\n");
        return 77;
    }
    printf("test_sdl_gl_vao: dlopen OK sdl=%llx gl=%llx\n",
           (unsigned long long)hsdl, (unsigned long long)hgl);

    SDL_Init_t SDL_Init; SDL_Quit_t SDL_Quit;
    SDL_CreateWindow_t SDL_CreateWindow; SDL_DestroyWindow_t SDL_DestroyWindow;
    SDL_GL_CreateContext_t SDL_GL_CreateContext;
    SDL_GL_MakeCurrent_t SDL_GL_MakeCurrent; SDL_GL_SwapWindow_t SDL_GL_SwapWindow;
    SDL_GL_SetAttribute_t SDL_GL_SetAttribute;
    SDL_PollEvent_t SDL_PollEvent; SDL_Delay_t SDL_Delay; SDL_GetError_t SDL_GetError;
    LOAD(hsdl, SDL_Init_t, SDL_Init); LOAD(hsdl, SDL_Quit_t, SDL_Quit);
    LOAD(hsdl, SDL_CreateWindow_t, SDL_CreateWindow);
    LOAD(hsdl, SDL_DestroyWindow_t, SDL_DestroyWindow);
    LOAD(hsdl, SDL_GL_CreateContext_t, SDL_GL_CreateContext);
    LOAD(hsdl, SDL_GL_MakeCurrent_t, SDL_GL_MakeCurrent);
    LOAD(hsdl, SDL_GL_SwapWindow_t, SDL_GL_SwapWindow);
    LOAD(hsdl, SDL_GL_SetAttribute_t, SDL_GL_SetAttribute);
    LOAD(hsdl, SDL_PollEvent_t, SDL_PollEvent); LOAD(hsdl, SDL_Delay_t, SDL_Delay);
    LOAD(hsdl, SDL_GetError_t, SDL_GetError);

    glClear_t glClear; glClearColor_t glClearColor; glViewport_t glViewport;
    glGetIntegerv_t glGetIntegerv;
    glGenVertexArrays_t glGenVertexArrays; glBindVertexArray_t glBindVertexArray;
    glDeleteVertexArrays_t glDeleteVertexArrays;
    glGenBuffers_t glGenBuffers; glBindBuffer_t glBindBuffer; glBufferData_t glBufferData;
    glDeleteBuffers_t glDeleteBuffers;
    glEnableVertexAttribArray_t glEnableVertexAttribArray;
    glVertexAttribPointer_t glVertexAttribPointer;
    glDrawElements_t glDrawElements;
    glCreateShader_t glCreateShader; glShaderSource_t glShaderSource;
    glCompileShader_t glCompileShader; glGetShaderiv_t glGetShaderiv;
    glGetShaderInfoLog_t glGetShaderInfoLog;
    glCreateProgram_t glCreateProgram; glAttachShader_t glAttachShader;
    glLinkProgram_t glLinkProgram; glGetProgramiv_t glGetProgramiv;
    glGetProgramInfoLog_t glGetProgramInfoLog; glDeleteShader_t glDeleteShader;
    glUseProgram_t glUseProgram; glBindAttribLocation_t glBindAttribLocation;
    glGetUniformLocation_t glGetUniformLocation;
    glUniformMatrix4fv_t glUniformMatrix4fv; glUniform4f_t glUniform4f;
    glDeleteProgram_t glDeleteProgram;
    glFlush_t glFlush; glGetError_t glGetError; glGetString_t glGetString;
    LOAD(hgl, glClear_t, glClear); LOAD(hgl, glClearColor_t, glClearColor);
    LOAD(hgl, glViewport_t, glViewport); LOAD(hgl, glGetIntegerv_t, glGetIntegerv);
    LOAD(hgl, glGenVertexArrays_t, glGenVertexArrays);
    LOAD(hgl, glBindVertexArray_t, glBindVertexArray);
    LOAD(hgl, glDeleteVertexArrays_t, glDeleteVertexArrays);
    LOAD(hgl, glGenBuffers_t, glGenBuffers); LOAD(hgl, glBindBuffer_t, glBindBuffer);
    LOAD(hgl, glBufferData_t, glBufferData); LOAD(hgl, glDeleteBuffers_t, glDeleteBuffers);
    LOAD(hgl, glEnableVertexAttribArray_t, glEnableVertexAttribArray);
    LOAD(hgl, glVertexAttribPointer_t, glVertexAttribPointer);
    LOAD(hgl, glDrawElements_t, glDrawElements);
    LOAD(hgl, glCreateShader_t, glCreateShader); LOAD(hgl, glShaderSource_t, glShaderSource);
    LOAD(hgl, glCompileShader_t, glCompileShader); LOAD(hgl, glGetShaderiv_t, glGetShaderiv);
    LOAD(hgl, glGetShaderInfoLog_t, glGetShaderInfoLog);
    LOAD(hgl, glCreateProgram_t, glCreateProgram); LOAD(hgl, glAttachShader_t, glAttachShader);
    LOAD(hgl, glLinkProgram_t, glLinkProgram); LOAD(hgl, glGetProgramiv_t, glGetProgramiv);
    LOAD(hgl, glGetProgramInfoLog_t, glGetProgramInfoLog); LOAD(hgl, glDeleteShader_t, glDeleteShader);
    LOAD(hgl, glUseProgram_t, glUseProgram); LOAD(hgl, glBindAttribLocation_t, glBindAttribLocation);
    LOAD(hgl, glGetUniformLocation_t, glGetUniformLocation);
    LOAD(hgl, glUniformMatrix4fv_t, glUniformMatrix4fv); LOAD(hgl, glUniform4f_t, glUniform4f);
    LOAD(hgl, glDeleteProgram_t, glDeleteProgram);
    LOAD(hgl, glFlush_t, glFlush); LOAD(hgl, glGetError_t, glGetError);
    LOAD(hgl, glGetString_t, glGetString);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("test_sdl_gl_vao: SKIP (SDL_Init failed: %s)\n",
               SDL_GetError() ? SDL_GetError() : "?");
        return 77;
    }
    /* Request a core 3.3 context so VAOs are mandatory. */
    SDL_GL_SetAttribute(17 /* MAJOR */, 3);
    SDL_GL_SetAttribute(18 /* MINOR */, 3);
    SDL_GL_SetAttribute(0x17 /* CONTEXT_PROFILE_MASK */, 1 /* CORE */);

    void* win = SDL_CreateWindow("bifrost vao",
                                 0x2FFF0000, 0x2FFF0000, 640, 480, SDL_WINDOW_OPENGL);
    if (!win) {
        printf("test_sdl_gl_vao: SKIP (SDL_CreateWindow failed: %s)\n",
               SDL_GetError() ? SDL_GetError() : "?");
        SDL_Quit();
        return 77;
    }
    void* ctx = SDL_GL_CreateContext(win);
    if (!ctx) {
        printf("test_sdl_gl_vao: SKIP (SDL_GL_CreateContext failed: %s)\n",
               SDL_GetError() ? SDL_GetError() : "?");
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 77;
    }
    SDL_GL_MakeCurrent(win, ctx);
    glViewport(0, 0, 640, 480);

    const unsigned char* vendor = glGetString(0x1F00);
    printf("test_sdl_gl_vao: GL_VENDOR=%s\n",
           vendor ? (const char*)vendor : "(null)");

    /* Shader compile. */
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
            char log[512] = {0};
            glGetShaderInfoLog(vs, (int)sizeof(log) - 1, NULL, log);
            printf("FAIL: vertex shader compile: %s\n", log);
            return 1;
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
            char log[512] = {0};
            glGetShaderInfoLog(fs, (int)sizeof(log) - 1, NULL, log);
            printf("FAIL: fragment shader compile: %s\n", log);
            return 1;
        }
    }
    unsigned prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "aPos");
    glLinkProgram(prog);
    {
        int ok = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[512] = {0};
            glGetProgramInfoLog(prog, (int)sizeof(log) - 1, NULL, log);
            printf("FAIL: program link: %s\n", log);
            return 1;
        }
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    glUseProgram(prog);

    /* Geometry: quad with an index buffer. */
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

    /* Matrix uniform must be bounced through the thunk at full size. */
    int mvp_loc = glGetUniformLocation(prog, "uMVP");
    int color_loc = glGetUniformLocation(prog, "uColor");
    printf("test_sdl_gl_vao: uniform locs mvp=%d color=%d\n", mvp_loc, color_loc);
    if (mvp_loc < 0 || color_loc < 0) {
        printf("FAIL: uniform location lookup\n");
        return 1;
    }
    float mvp[16];
    build_mvp(mvp);

    int glerr = 0;
    for (int f = 0; f < 30; f++) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) goto done;
        }
        glClearColor(0.05f, 0.05f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUniformMatrix4fv(mvp_loc, 1, GL_FALSE, mvp);
        glUniform4f(color_loc, 0.4f, 0.8f, 0.3f, 1.0f);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, (const void*)0);
        glFlush();
        glerr = glGetError();
        if (glerr != 0) break;
        SDL_GL_SwapWindow(win);
        SDL_Delay(16);
    }

done:
    {
        unsigned err = glGetError();
        if (glerr != 0) err = (unsigned)glerr;
        printf("test_sdl_gl_vao: glGetError=%u\n", err);
        if (err != 0) {
            printf("FAIL: GL error\n");
            glDeleteVertexArrays(1, &vao);
            glDeleteBuffers(2, (const unsigned[]){ vbo, ebo });
            glDeleteProgram(prog);
            SDL_DestroyWindow(win);
            SDL_Quit();
            return 1;
        }
    }
    printf("test_sdl_gl_vao: ALL PASS\n");
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(2, (const unsigned[]){ vbo, ebo });
    glDeleteProgram(prog);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
