// test_sdl_gl_elide.c — redundant-bind elision regression (2026-09-04).
//
// Exercises exactly the interactions the ELIDE_BIND path must preserve:
//   1. ARRAY_BUFFER binds elided across draws; VAO attrib capture must
//      still resolve to the right VBO (left tri red, right tri green).
//   2. ELEMENT_ARRAY_BUFFER binds elided + VAO switches restoring each
//      VAO's own index buffer (per-VAO capture model).
//   3. DeleteBuffers + name reuse: re-generated names must NOT observe
//      stale "already bound" state.
// Verified by glReadPixels (tolerant thresholds, no MSAA). Exit 0 =
// pass, 77 = skip without GL/SDL/display.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GL_ARRAY_BUFFER         0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STATIC_DRAW          0x88E4
#define GL_FLOAT                0x1406
#define GL_UNSIGNED_SHORT       0x1403
#define GL_UNSIGNED_BYTE        0x1401
#define GL_RGBA                 0x1908
#define GL_VERTEX_SHADER        0x8B31
#define GL_FRAGMENT_SHADER      0x8B30
#define GL_COMPILE_STATUS       0x8B81
#define GL_LINK_STATUS          0x8B82
#define GL_TRIANGLES            0x0004
#define GL_COLOR_BUFFER_BIT     0x4000

typedef int          (*SDL_Init_t)(uint32_t);
typedef void*        (*SDL_CreateWindow_t)(const char*, int, int, int, int, uint32_t);
typedef void*        (*SDL_GL_CreateContext_t)(void*);
typedef void*        (*SDL_GL_MakeCurrent_t)(void*, void*);
typedef int          (*SDL_GL_SetAttribute_t)(int, int);
typedef void         (*SDL_DestroyWindow_t)(void*);
typedef void         (*SDL_Quit_t)(void);
typedef const char*  (*SDL_GetError_t)(void);
typedef void         (*glClearColor_t)(float, float, float, float);
typedef void         (*glClear_t)(unsigned);
typedef void         (*glViewport_t)(int, int, int, int);
typedef unsigned     (*glCreateShader_t)(unsigned);
typedef void         (*glShaderSource_t)(unsigned, int, const char**, const int*);
typedef void         (*glCompileShader_t)(unsigned);
typedef void         (*glGetShaderiv_t)(unsigned, unsigned, int*);
typedef unsigned     (*glCreateProgram_t)(void);
typedef void         (*glAttachShader_t)(unsigned, unsigned);
typedef void         (*glLinkProgram_t)(unsigned);
typedef void         (*glGetProgramiv_t)(unsigned, unsigned, int*);
typedef void         (*glUseProgram_t)(unsigned);
typedef int          (*glGetUniformLocation_t)(unsigned, const char*);
typedef void         (*glUniform4f_t)(int, float, float, float, float);
typedef void         (*glGenBuffers_t)(int, uint32_t*);
typedef void         (*glBindBuffer_t)(unsigned, unsigned);
typedef void         (*glBufferData_t)(unsigned, long long, const void*, unsigned);
typedef void         (*glDeleteBuffers_t)(int, const uint32_t*);
typedef void         (*glGenVertexArrays_t)(int, uint32_t*);
typedef void         (*glBindVertexArray_t)(uint32_t);
typedef void         (*glDeleteVertexArrays_t)(int, const uint32_t*);
typedef void         (*glEnableVertexAttribArray_t)(unsigned);
typedef void         (*glVertexAttribPointer_t)(unsigned, int, unsigned, unsigned char, int, const void*);
typedef void         (*glDrawArrays_t)(unsigned, int, int);
typedef void         (*glDrawElements_t)(unsigned, int, unsigned, const void*);
typedef void         (*glReadPixels_t)(int, int, int, int, unsigned, unsigned, void*);
typedef unsigned     (*glGetError_t)(void);

static uint64_t bdl(const char* path, uint64_t mode) {
    register uint64_t x0 __asm__("x0") = (uint64_t)(uintptr_t)path;
    register uint64_t x1 __asm__("x1") = mode;
    register uint64_t x8 __asm__("x8") = 0x1002;
    __asm__ volatile ("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}
static uint64_t bsym(uint64_t h, const char* n) {
    register uint64_t x0 __asm__("x0") = h;
    register uint64_t x1 __asm__("x1") = (uint64_t)(uintptr_t)n;
    register uint64_t x8 __asm__("x8") = 0x1003;
    __asm__ volatile ("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}
#define LOAD(h, T, name) do { \
    name = (T)(uintptr_t)bsym(h, #name); \
    if (!name) { printf("FAIL: dlsym %s\n", #name); return 1; } \
} while (0)

static int g_fail = 0, g_n = 0;
#define CHK(c, msg) do { g_n++; if (!(c)) { printf(" FAIL  %s\n", msg); g_fail++; } \
    else printf("  OK  %s\n", msg); } while (0)

int main(void) {
    uint64_t hsdl = bdl("libSDL2-2.0.so.0", 1);
    if (!hsdl) hsdl = bdl("libSDL2.so", 1);
    if (!hsdl) { printf("SKIP no sdl\n"); return 77; }
    uint64_t hgl = bdl("libGL.so.1", 1);
    if (!hgl) { printf("SKIP no gl\n"); return 77; }
    SDL_Init_t SDL_Init; SDL_CreateWindow_t SDL_CreateWindow;
    SDL_GL_CreateContext_t SDL_GL_CreateContext; SDL_GL_MakeCurrent_t SDL_GL_MakeCurrent;
    SDL_GL_SetAttribute_t SDL_GL_SetAttribute; SDL_DestroyWindow_t SDL_DestroyWindow;
    SDL_Quit_t SDL_Quit; SDL_GetError_t SDL_GetError;
    LOAD(hsdl, SDL_Init_t, SDL_Init);
    LOAD(hsdl, SDL_CreateWindow_t, SDL_CreateWindow);
    LOAD(hsdl, SDL_GL_CreateContext_t, SDL_GL_CreateContext);
    LOAD(hsdl, SDL_GL_MakeCurrent_t, SDL_GL_MakeCurrent);
    LOAD(hsdl, SDL_GL_SetAttribute_t, SDL_GL_SetAttribute);
    LOAD(hsdl, SDL_DestroyWindow_t, SDL_DestroyWindow);
    LOAD(hsdl, SDL_Quit_t, SDL_Quit);
    LOAD(hsdl, SDL_GetError_t, SDL_GetError);
    glClearColor_t glClearColor; glClear_t glClear; glViewport_t glViewport;
    glCreateShader_t glCreateShader; glShaderSource_t glShaderSource;
    glCompileShader_t glCompileShader; glGetShaderiv_t glGetShaderiv;
    glCreateProgram_t glCreateProgram; glAttachShader_t glAttachShader;
    glLinkProgram_t glLinkProgram; glGetProgramiv_t glGetProgramiv;
    glUseProgram_t glUseProgram; glGetUniformLocation_t glGetUniformLocation;
    glUniform4f_t glUniform4f;
    glGenBuffers_t glGenBuffers; glBindBuffer_t glBindBuffer;
    glBufferData_t glBufferData; glDeleteBuffers_t glDeleteBuffers;
    glGenVertexArrays_t glGenVertexArrays; glBindVertexArray_t glBindVertexArray;
    glDeleteVertexArrays_t glDeleteVertexArrays;
    glEnableVertexAttribArray_t glEnableVertexAttribArray;
    glVertexAttribPointer_t glVertexAttribPointer;
    glDrawArrays_t glDrawArrays; glDrawElements_t glDrawElements;
    glReadPixels_t glReadPixels; glGetError_t glGetError;
    LOAD(hgl, glClearColor_t, glClearColor);
    LOAD(hgl, glClear_t, glClear);
    LOAD(hgl, glViewport_t, glViewport);
    LOAD(hgl, glCreateShader_t, glCreateShader);
    LOAD(hgl, glShaderSource_t, glShaderSource);
    LOAD(hgl, glCompileShader_t, glCompileShader);
    LOAD(hgl, glGetShaderiv_t, glGetShaderiv);
    LOAD(hgl, glCreateProgram_t, glCreateProgram);
    LOAD(hgl, glAttachShader_t, glAttachShader);
    LOAD(hgl, glLinkProgram_t, glLinkProgram);
    LOAD(hgl, glGetProgramiv_t, glGetProgramiv);
    LOAD(hgl, glUseProgram_t, glUseProgram);
    LOAD(hgl, glGetUniformLocation_t, glGetUniformLocation);
    LOAD(hgl, glUniform4f_t, glUniform4f);
    LOAD(hgl, glGenBuffers_t, glGenBuffers);
    LOAD(hgl, glBindBuffer_t, glBindBuffer);
    LOAD(hgl, glBufferData_t, glBufferData);
    LOAD(hgl, glDeleteBuffers_t, glDeleteBuffers);
    LOAD(hgl, glGenVertexArrays_t, glGenVertexArrays);
    LOAD(hgl, glBindVertexArray_t, glBindVertexArray);
    LOAD(hgl, glDeleteVertexArrays_t, glDeleteVertexArrays);
    LOAD(hgl, glEnableVertexAttribArray_t, glEnableVertexAttribArray);
    LOAD(hgl, glVertexAttribPointer_t, glVertexAttribPointer);
    LOAD(hgl, glDrawArrays_t, glDrawArrays);
    LOAD(hgl, glDrawElements_t, glDrawElements);
    LOAD(hgl, glReadPixels_t, glReadPixels);
    LOAD(hgl, glGetError_t, glGetError);

    if (SDL_Init(0x20) != 0) { printf("SKIP sdl init\n"); return 77; }
    SDL_GL_SetAttribute(17, 3);
    SDL_GL_SetAttribute(18, 0);
    void* win = SDL_CreateWindow("elide", 0x2FFF0000, 0x2FFF0000,
                                 128, 128, 0x00000002);
    if (!win) { printf("SKIP no win\n"); return 77; }
    void* ctx = SDL_GL_CreateContext(win);
    if (!ctx) { printf("SKIP no ctx\n"); return 77; }
    SDL_GL_MakeCurrent(win, ctx);
    glViewport(0, 0, 128, 128);

    const char* vsrc =
        "attribute vec4 pos; void main(){ gl_Position = pos; }";
    const char* fsrc =
        "uniform vec4 color; void main(){ gl_FragColor = color; }";
    unsigned vs = glCreateShader(0x8B31);
    glShaderSource(vs, 1, &vsrc, 0); glCompileShader(vs);
    unsigned fs = glCreateShader(0x8B30);
    glShaderSource(fs, 1, &fsrc, 0); glCompileShader(fs);
    unsigned pr = glCreateProgram();
    glAttachShader(pr, vs); glAttachShader(pr, fs);
    glLinkProgram(pr); glUseProgram(pr);
    int colorloc = glGetUniformLocation(pr, "color");
    CHK(colorloc >= 0, "program links, color uniform found");
    CHK(glGetError() == 0, "setup error-free");

    /* left-half verts in VBO1, right-half in VBO2 */
    float left[9]  = {-1,-1,0, 0,-1,0, -1,1,0};
    float right[9] = {0,-1,0, 1,-1,0, 0,1,0};
    uint32_t vao1 = 0, vao2 = 0, vbo1 = 0, vbo2 = 0;
    glGenVertexArrays(1, &vao1);
    glGenVertexArrays(1, &vao2);
    glGenBuffers(1, &vbo1);
    glGenBuffers(1, &vbo2);
    glBindVertexArray(vao1);
    glBindBuffer(GL_ARRAY_BUFFER, vbo1);
    glBufferData(GL_ARRAY_BUFFER, sizeof(left), left, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, 0, 12, (const void*)0);
    glBindVertexArray(vao2);
    glBindBuffer(GL_ARRAY_BUFFER, vbo2);
    glBufferData(GL_ARRAY_BUFFER, sizeof(right), right, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, 0, 12, (const void*)0);
    CHK(glGetError() == 0, "vao/vbo setup error-free");

    /* game pattern: redundant binds around every draw */
    glClearColor(0, 0, 0, 1);
    for (int f = 0; f < 4; f++) {
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(pr);
        glBindVertexArray(vao1);
        glBindBuffer(GL_ARRAY_BUFFER, vbo1);   /* redundant -> elided */
        glUniform4f(colorloc, 1, 0, 0, 1);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glUseProgram(pr);                       /* redundant -> elided */
        glBindVertexArray(vao2);
        glBindBuffer(GL_ARRAY_BUFFER, vbo2);   /* redundant -> elided */
        glUniform4f(colorloc, 0, 1, 0, 1);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    CHK(glGetError() == 0, "redundant-bind draws error-free");
    unsigned char px[128 * 128 * 4];
    memset(px, 0, sizeof(px));
    glReadPixels(0, 0, 128, 128, GL_RGBA, GL_UNSIGNED_BYTE, px);
    /* note: GL origin is bottom-left; left-half tri occupies x<64 */
    unsigned char* L = px + (43 * 128 + 21) * 4;
    unsigned char* R = px + (43 * 128 + 85) * 4;
    CHK(L[0] > 200 && L[1] < 50 && L[2] < 50, "left pixel red (array capture intact)");
    CHK(R[1] > 200 && R[0] < 50 && R[2] < 50, "right pixel green (array capture intact)");

    /* element path: separate index buffers per VAO, VAO switches */
    unsigned short idx1[3] = {0, 1, 2};
    unsigned short idx2[3] = {0, 1, 2};
    uint32_t ebo1 = 0, ebo2 = 0;
    glGenBuffers(1, &ebo1);
    glGenBuffers(1, &ebo2);
    glBindVertexArray(vao1);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo1);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx1), idx1, GL_STATIC_DRAW);
    glBindVertexArray(vao2);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo2);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx2), idx2, GL_STATIC_DRAW);
    glClear(GL_COLOR_BUFFER_BIT);
    for (int f = 0; f < 4; f++) {
        glBindVertexArray(vao1);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo1); /* redundant -> elided */
        glUniform4f(colorloc, 1, 0, 0, 1);
        glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, (const void*)0);
        glBindVertexArray(vao2);  /* restores ebo2 with no bind call */
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo2); /* redundant -> elided */
        glUniform4f(colorloc, 0, 1, 0, 1);
        glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, (const void*)0);
    }
    CHK(glGetError() == 0, "element draws error-free");
    memset(px, 0, sizeof(px));
    glReadPixels(0, 0, 128, 128, GL_RGBA, GL_UNSIGNED_BYTE, px);
    L = px + (43 * 128 + 21) * 4;
    R = px + (43 * 128 + 85) * 4;
    CHK(L[0] > 200 && L[1] < 50, "left pixel red (element capture intact)");
    CHK(R[1] > 200 && R[0] < 50, "right pixel green (element capture intact)");

    /* delete + name reuse must not observe stale bindings */
    glDeleteBuffers(1, &vbo1);
    glDeleteBuffers(1, &ebo1);
    uint32_t vbo3 = 0;
    glGenBuffers(1, &vbo3);
    glBindVertexArray(vao1);
    glBindBuffer(GL_ARRAY_BUFFER, vbo3);
    float full[9] = {-1,-1,0, 1,-1,0, 0,1,0};
    glBufferData(GL_ARRAY_BUFFER, sizeof(full), full, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, 0, 12, (const void*)0);
    glClear(GL_COLOR_BUFFER_BIT);
    glUniform4f(colorloc, 0, 0, 1, 1);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    CHK(glGetError() == 0, "post-delete rebind error-free");
    memset(px, 0, sizeof(px));
    glReadPixels(0, 0, 128, 128, GL_RGBA, GL_UNSIGNED_BYTE, px);
    unsigned char* C = px + (43 * 128 + 64) * 4;
    CHK(C[2] > 200, "center pixel blue after delete+rebind");

    glDeleteVertexArrays(1, &vao1);
    glDeleteVertexArrays(1, &vao2);
    SDL_DestroyWindow(win);
    SDL_Quit();
    if (!g_fail) printf("ELIDE TEST PASSED (%d checks)\n", g_n);
    else printf("ELIDE TEST FAILED (%d checks, %d failures)\n", g_n, g_fail);
    return g_fail ? 1 : 0;
}
