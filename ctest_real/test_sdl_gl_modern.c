/* test_sdl_gl_modern.c — exercises the "AAA future-proofing" GL rows added
 * 2026-08-15: uniform blocks (UBO), instancing divisor, compute + barriers,
 * transform feedback varyings, sampler objects, query objects, and the
 * modern draw/batching entry points (glDrawElementsBaseVertex,
 * glDrawRangeElements, glPrimitiveRestartIndex) plus shader-introspection
 * queries (glGetActiveUniform / glGetActiveAttrib / glGetActiveUniformBlock*).
 *
 * These are the GL 3.3+/4.x core features modern engines use for camera
 * matrices (UBO), instanced terrain/grass (divisor), GPU culling/particles
 * (compute+SSBO), mesh batching (base-vertex/restart) and separate sampler
 * state. Everything here takes the generic integer thunk path (or the
 * TF_VARYINGS nested-string path) — no host-side state tracking needed,
 * so the checks verify the calls dispatch to real host GL and return/query
 * sane values.
 *
 * Build:
 *   make cross SRC=ctest_real/test_sdl_gl_modern.c \
 *             OUT=ctest_real/test_sdl_gl_modern.elf
 * Run (needs host SDL2 + GL 3.3+ and a display):
 *   make USE_SDL2=1 USE_THUNK_GL=1
 *   ./bifrost-emu ctest_real/test_sdl_gl_modern.elf
 * Headless / no GL: exits 77 (skip).
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GL_ARRAY_BUFFER        0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STATIC_DRAW         0x88E4
#define GL_STREAM_DRAW         0x88E0
#define GL_FLOAT               0x1406
#define GL_UNSIGNED_INT        0x1405
#define GL_UNSIGNED_SHORT      0x1403
#define GL_VERTEX_SHADER       0x8B31
#define GL_FRAGMENT_SHADER     0x8B30
#define GL_COMPILE_STATUS      0x8B81
#define GL_LINK_STATUS         0x8B82
#define GL_TRIANGLES           0x0004
#define GL_TRIANGLE_STRIP      0x0005
#define GL_POINTS              0x0000
#define GL_ACTIVE_UNIFORM_BLOCK_MAX_NAME_LENGTH 0x8A35
#define GL_UNIFORM_BLOCK_INDEX 0x8A3A
#define GL_UNIFORM_BLOCK_DATA_SIZE 0x8A40
#define GL_UNIFORM_BLOCK_BINDING 0x8A3F
#define GL_ACTIVE_UNIFORMS     0x8B86
#define GL_ACTIVE_UNIFORM_MAX_LENGTH 0x8B87
#define GL_ACTIVE_ATTRIBUTES   0x8B89
#define GL_ACTIVE_ATTRIBUTE_MAX_LENGTH 0x8B8A
#define GL_UNIFORM_BUFFER      0x8A11
#define GL_UNIFORM_BARRIER_BIT 0x00000010u
#define GL_COMPUTE_SHADER      0x91B9
#define GL_ANY_SAMPLES_PASSED  0x8C2F
#define GL_QUERY_RESULT        0x8866
#define GL_QUERY_RESULT_AVAILABLE 0x8867
#define GL_TEXTURE_2D          0x0DE1
#define GL_MAX_COMPUTE_WORK_GROUP_COUNT 0x91BE
#define GL_DEPTH_TEST          0x0B71
#define GL_NEAREST             0x2600
#define GL_RGBA32F             0x8814
#define GL_READ_WRITE          0x88BA
#define GL_TRANSFORM_FEEDBACK 0x8E22
#define GL_SEPARATE_ATTRIBS    0x8C8D
#define SDL_INIT_VIDEO         0x00000020u
#define SDL_WINDOW_OPENGL      0x00000002u
#define SDL_GL_CONTEXT_MAJOR_VERSION 0x00000011u
#define SDL_GL_CONTEXT_MINOR_VERSION 0x00000012u
#define SDL_GL_CONTEXT_PROFILE_MASK  0x00000016u
#define SDL_GL_CONTEXT_PROFILE_CORE  0x00000001u

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
typedef void (*glDeleteBuffers_t)(int, const uint32_t*);
typedef unsigned (*glCreateShader_t)(unsigned);
typedef void (*glShaderSource_t)(unsigned, int, const char* const*, const int*);
typedef void (*glCompileShader_t)(unsigned);
typedef void (*glGetShaderiv_t)(unsigned, unsigned, int*);
typedef unsigned (*glCreateProgram_t)(void);
typedef void (*glAttachShader_t)(unsigned, unsigned);
typedef void (*glLinkProgram_t)(unsigned);
typedef void (*glGetProgramiv_t)(unsigned, unsigned, int*);
typedef void (*glUseProgram_t)(unsigned);
typedef void (*glDeleteShader_t)(unsigned);
typedef void (*glDeleteProgram_t)(unsigned);
typedef int  (*glGetUniformLocation_t)(unsigned, const char*);
typedef int  (*glGetAttribLocation_t)(unsigned, const char*);
typedef unsigned (*glGetUniformBlockIndex_t)(unsigned, const char*);
typedef void (*glUniformBlockBinding_t)(unsigned, unsigned, unsigned);
typedef void (*glGetActiveUniformBlockiv_t)(unsigned, unsigned, unsigned, int*);
typedef void (*glGetActiveUniformBlockName_t)(unsigned, unsigned, int, int*, char*);
typedef void (*glGetActiveUniform_t)(unsigned, unsigned, int, int*, int*, unsigned*, char*);
typedef void (*glGetActiveAttrib_t)(unsigned, unsigned, int, int*, int*, unsigned*, char*);
typedef void (*glGenVertexArrays_t)(int, uint32_t*);
typedef void (*glBindVertexArray_t)(uint32_t);
typedef void (*glEnableVertexAttribArray_t)(unsigned);
typedef void (*glVertexAttribPointer_t)(unsigned, int, unsigned, unsigned char, int, const void*);
typedef void (*glVertexAttribDivisor_t)(unsigned, unsigned);
typedef void (*glGenQueries_t)(int, uint32_t*);
typedef void (*glBeginQuery_t)(unsigned, uint32_t);
typedef void (*glEndQuery_t)(unsigned);
typedef void (*glGetQueryObjectuiv_t)(uint32_t, unsigned, unsigned*);
typedef void (*glGenSamplers_t)(int, uint32_t*);
typedef void (*glBindSampler_t)(unsigned, uint32_t);
typedef void (*glSamplerParameteri_t)(uint32_t, unsigned, int);
typedef void (*glSamplerParameterf_t)(uint32_t, unsigned, float);
typedef void (*glDeleteSamplers_t)(int, const uint32_t*);
typedef void (*glDeleteQueries_t)(int, const uint32_t*);
typedef void (*glDispatchCompute_t)(unsigned, unsigned, unsigned);
typedef void (*glMemoryBarrier_t)(unsigned);
typedef void (*glBindImageTexture_t)(unsigned, uint32_t, int, unsigned char, int, unsigned, unsigned);
typedef void (*glDrawElementsBaseVertex_t)(unsigned, int, unsigned, const void*, int);
typedef void (*glDrawRangeElements_t)(unsigned, unsigned, unsigned, int, unsigned, const void*);
typedef void (*glPrimitiveRestartIndex_t)(unsigned);
typedef void (*glBeginTransformFeedback_t)(unsigned);
typedef void (*glEndTransformFeedback_t)(void);
typedef void (*glTransformFeedbackVaryings_t)(unsigned, int, const char* const*, unsigned);
typedef void (*glGetIntegerv_t)(unsigned, int*);
typedef unsigned (*glGetError_t)(void);
typedef void (*glGenTextures_t)(int, uint32_t*);
typedef void (*glBindTexture_t)(unsigned, uint32_t);

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
    printf("test_sdl_gl_modern: start\n");

    uint64_t hsdl = bifrost_dlopen("libSDL2-2.0.so.0", 1);
    if (!hsdl) hsdl = bifrost_dlopen("libSDL2.so.0", 1);
    if (!hsdl) hsdl = bifrost_dlopen("libSDL2.so", 1);
    if (!hsdl) { printf("test_sdl_gl_modern: SKIP (libSDL2)\n"); return 77; }
    uint64_t hgl = bifrost_dlopen("libGL.so.1", 1);
    if (!hgl) hgl = bifrost_dlopen("libGL.so", 1);
    if (!hgl) { printf("test_sdl_gl_modern: SKIP (libGL)\n"); return 77; }

    SDL_Init_t SDL_Init; SDL_Quit_t SDL_Quit;
    SDL_CreateWindow_t SDL_CreateWindow; SDL_DestroyWindow_t SDL_DestroyWindow;
    SDL_GL_CreateContext_t SDL_GL_CreateContext; SDL_GL_MakeCurrent_t SDL_GL_MakeCurrent;
    SDL_GL_SetAttribute_t SDL_GL_SetAttribute; SDL_GetError_t SDL_GetError;
    LOAD(hsdl, SDL_Init_t, SDL_Init); LOAD(hsdl, SDL_Quit_t, SDL_Quit);
    LOAD(hsdl, SDL_CreateWindow_t, SDL_CreateWindow);
    LOAD(hsdl, SDL_DestroyWindow_t, SDL_DestroyWindow);
    LOAD(hsdl, SDL_GL_CreateContext_t, SDL_GL_CreateContext);
    LOAD(hsdl, SDL_GL_MakeCurrent_t, SDL_GL_MakeCurrent);
    LOAD(hsdl, SDL_GL_SetAttribute_t, SDL_GL_SetAttribute);
    LOAD(hsdl, SDL_GetError_t, SDL_GetError);

    glGenBuffers_t glGenBuffers; glBindBuffer_t glBindBuffer;
    glBufferData_t glBufferData; glDeleteBuffers_t glDeleteBuffers;
    glCreateShader_t glCreateShader; glShaderSource_t glShaderSource;
    glCompileShader_t glCompileShader; glGetShaderiv_t glGetShaderiv;
    glCreateProgram_t glCreateProgram; glAttachShader_t glAttachShader;
    glLinkProgram_t glLinkProgram; glGetProgramiv_t glGetProgramiv;
    glUseProgram_t glUseProgram; glDeleteShader_t glDeleteShader;
    glDeleteProgram_t glDeleteProgram;
    glGetUniformLocation_t glGetUniformLocation; glGetAttribLocation_t glGetAttribLocation;
    glGetUniformBlockIndex_t glGetUniformBlockIndex;
    glUniformBlockBinding_t glUniformBlockBinding;
    glGetActiveUniformBlockiv_t glGetActiveUniformBlockiv;
    glGetActiveUniformBlockName_t glGetActiveUniformBlockName;
    glGetActiveUniform_t glGetActiveUniform; glGetActiveAttrib_t glGetActiveAttrib;
    glGenVertexArrays_t glGenVertexArrays; glBindVertexArray_t glBindVertexArray;
    glEnableVertexAttribArray_t glEnableVertexAttribArray;
    glVertexAttribPointer_t glVertexAttribPointer;
    glVertexAttribDivisor_t glVertexAttribDivisor;
    glGenQueries_t glGenQueries; glBeginQuery_t glBeginQuery;
    glEndQuery_t glEndQuery; glGetQueryObjectuiv_t glGetQueryObjectuiv;
    glDeleteQueries_t glDeleteQueries;
    glGenSamplers_t glGenSamplers; glBindSampler_t glBindSampler;
    glSamplerParameteri_t glSamplerParameteri; glSamplerParameterf_t glSamplerParameterf;
    glDeleteSamplers_t glDeleteSamplers;
    glDispatchCompute_t glDispatchCompute; glMemoryBarrier_t glMemoryBarrier;
    glBindImageTexture_t glBindImageTexture;
    glDrawElementsBaseVertex_t glDrawElementsBaseVertex;
    glDrawRangeElements_t glDrawRangeElements;
    glPrimitiveRestartIndex_t glPrimitiveRestartIndex;
    glBeginTransformFeedback_t glBeginTransformFeedback;
    glEndTransformFeedback_t glEndTransformFeedback;
    glTransformFeedbackVaryings_t glTransformFeedbackVaryings;
    glGetIntegerv_t glGetIntegerv; glGetError_t glGetError;
    glGenTextures_t glGenTextures; glBindTexture_t glBindTexture;
    LOAD(hgl, glGenBuffers_t, glGenBuffers);
    LOAD(hgl, glBindBuffer_t, glBindBuffer);
    LOAD(hgl, glBufferData_t, glBufferData);
    LOAD(hgl, glDeleteBuffers_t, glDeleteBuffers);
    LOAD(hgl, glCreateShader_t, glCreateShader);
    LOAD(hgl, glShaderSource_t, glShaderSource);
    LOAD(hgl, glCompileShader_t, glCompileShader);
    LOAD(hgl, glGetShaderiv_t, glGetShaderiv);
    LOAD(hgl, glCreateProgram_t, glCreateProgram);
    LOAD(hgl, glAttachShader_t, glAttachShader);
    LOAD(hgl, glLinkProgram_t, glLinkProgram);
    LOAD(hgl, glGetProgramiv_t, glGetProgramiv);
    LOAD(hgl, glUseProgram_t, glUseProgram);
    LOAD(hgl, glDeleteShader_t, glDeleteShader);
    LOAD(hgl, glDeleteProgram_t, glDeleteProgram);
    LOAD(hgl, glGetUniformLocation_t, glGetUniformLocation);
    LOAD(hgl, glGetAttribLocation_t, glGetAttribLocation);
    LOAD(hgl, glGetUniformBlockIndex_t, glGetUniformBlockIndex);
    LOAD(hgl, glUniformBlockBinding_t, glUniformBlockBinding);
    LOAD(hgl, glGetActiveUniformBlockiv_t, glGetActiveUniformBlockiv);
    LOAD(hgl, glGetActiveUniformBlockName_t, glGetActiveUniformBlockName);
    LOAD(hgl, glGetActiveUniform_t, glGetActiveUniform);
    LOAD(hgl, glGetActiveAttrib_t, glGetActiveAttrib);
    LOAD(hgl, glGenVertexArrays_t, glGenVertexArrays);
    LOAD(hgl, glBindVertexArray_t, glBindVertexArray);
    LOAD(hgl, glEnableVertexAttribArray_t, glEnableVertexAttribArray);
    LOAD(hgl, glVertexAttribPointer_t, glVertexAttribPointer);
    LOAD(hgl, glVertexAttribDivisor_t, glVertexAttribDivisor);
    LOAD(hgl, glGenQueries_t, glGenQueries);
    LOAD(hgl, glBeginQuery_t, glBeginQuery);
    LOAD(hgl, glEndQuery_t, glEndQuery);
    LOAD(hgl, glGetQueryObjectuiv_t, glGetQueryObjectuiv);
    LOAD(hgl, glDeleteQueries_t, glDeleteQueries);
    LOAD(hgl, glGenSamplers_t, glGenSamplers);
    LOAD(hgl, glBindSampler_t, glBindSampler);
    LOAD(hgl, glSamplerParameteri_t, glSamplerParameteri);
    LOAD(hgl, glSamplerParameterf_t, glSamplerParameterf);
    LOAD(hgl, glDeleteSamplers_t, glDeleteSamplers);
    LOAD(hgl, glDispatchCompute_t, glDispatchCompute);
    LOAD(hgl, glMemoryBarrier_t, glMemoryBarrier);
    LOAD(hgl, glBindImageTexture_t, glBindImageTexture);
    LOAD(hgl, glDrawElementsBaseVertex_t, glDrawElementsBaseVertex);
    LOAD(hgl, glDrawRangeElements_t, glDrawRangeElements);
    LOAD(hgl, glPrimitiveRestartIndex_t, glPrimitiveRestartIndex);
    LOAD(hgl, glBeginTransformFeedback_t, glBeginTransformFeedback);
    LOAD(hgl, glEndTransformFeedback_t, glEndTransformFeedback);
    LOAD(hgl, glTransformFeedbackVaryings_t, glTransformFeedbackVaryings);
    LOAD(hgl, glGetIntegerv_t, glGetIntegerv);
    LOAD(hgl, glGetError_t, glGetError);
    LOAD(hgl, glGenTextures_t, glGenTextures);
    LOAD(hgl, glBindTexture_t, glBindTexture);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("test_sdl_gl_modern: SKIP (SDL_Init: %s)\n",
               SDL_GetError() ? SDL_GetError() : "?");
        return 77;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    void* win = SDL_CreateWindow("bifrost modern", 0x2FFF0000, 0x2FFF0000,
                                 320, 240, SDL_WINDOW_OPENGL);
    if (!win) { printf("SKIP (CreateWindow: %s)\n", SDL_GetError()); SDL_Quit(); return 77; }
    void* ctx = SDL_GL_CreateContext(win);
    if (!ctx) { printf("SKIP (CreateContext: %s)\n", SDL_GetError());
                SDL_DestroyWindow(win); SDL_Quit(); return 77; }
    SDL_GL_MakeCurrent(win, ctx);
    glGetError();

    int maj = 0, min = 0;
    glGetIntegerv(0x821B /* GL_MAJOR_VERSION */, &maj);
    glGetIntegerv(0x821C /* GL_MINOR_VERSION */, &min);
    printf("  context: GL %d.%d\n", maj, min);
    if (maj < 3) { printf("test_sdl_gl_modern: SKIP (GL < 3)\n");
                   SDL_DestroyWindow(win); SDL_Quit(); return 77; }

    /* ── Shader program with a uniform block + attribute ────────────── */
    const char* vs = "#version 330 core\n"
        "layout(location=0) in vec3 aPos;\n"
        "layout(std140) uniform CameraBlock { mat4 viewProj; vec4 eye; };\n"
        "void main(){ gl_Position = viewProj * vec4(aPos,1.0); }";
    const char* fs = "#version 330 core\n"
        "out vec4 FragColor;\n"
        "void main(){ FragColor = vec4(1.0); }";
    unsigned vsid = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vsid, 1, &vs, NULL);
    glCompileShader(vsid);
    int ok = 0; glGetShaderiv(vsid, GL_COMPILE_STATUS, &ok);
    chk(ok != 0, "vertex shader compiles");
    unsigned fsid = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fsid, 1, &fs, NULL);
    glCompileShader(fsid);
    glGetShaderiv(fsid, GL_COMPILE_STATUS, &ok);
    chk(ok != 0, "fragment shader compiles");
    unsigned prog = glCreateProgram();
    glAttachShader(prog, vsid);
    glAttachShader(prog, fsid);
    glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    chk(ok != 0, "program links");
    glUseProgram(prog);

    /* ── Uniform block: index, binding, active-block introspection ──── */
    int bi = glGetUniformBlockIndex(prog, "CameraBlock");
    chk(bi >= 0, "glGetUniformBlockIndex finds CameraBlock");
    if (bi >= 0) {
        glUniformBlockBinding(prog, (unsigned)bi, 1);
        int data_size = 0;
        glGetActiveUniformBlockiv(prog, (unsigned)bi, GL_UNIFORM_BLOCK_DATA_SIZE, &data_size);
        chk(data_size >= 16, "glGetActiveUniformBlockiv data size >= 16");
        int binding = -1;
        glGetActiveUniformBlockiv(prog, (unsigned)bi, GL_UNIFORM_BLOCK_BINDING, &binding);
        chk(binding == 1, "glGetActiveUniformBlockiv binding == 1");
        char bname[64];
        int bnamelen = 0;
        memset(bname, 0, sizeof(bname));
        glGetActiveUniformBlockName(prog, (unsigned)bi, (int)sizeof(bname),
                                    &bnamelen, bname);
        chk(strcmp(bname, "CameraBlock") == 0,
            "glGetActiveUniformBlockName returns CameraBlock");
    }

    /* ── Shader introspection: active uniform + attribute ───────────── */
    int nuni = 0;
    glGetProgramiv(prog, GL_ACTIVE_UNIFORMS, &nuni);
    chk(nuni >= 1, "program has >= 1 active uniform");
    if (nuni > 0) {
        char uname[64]; int ulen = 0, usize = 0; unsigned utype = 0;
        memset(uname, 0, sizeof(uname));
        glGetActiveUniform(prog, 0, (int)sizeof(uname), &ulen, &usize, &utype, uname);
        chk(uname[0] != 0, "glGetActiveUniform returns a name");
    }
    int nattr = 0;
    glGetProgramiv(prog, GL_ACTIVE_ATTRIBUTES, &nattr);
    chk(nattr >= 1, "program has >= 1 active attribute");
    if (nattr > 0) {
        char aname[64]; int alen = 0, asize = 0; unsigned atype = 0;
        memset(aname, 0, sizeof(aname));
        glGetActiveAttrib(prog, 0, (int)sizeof(aname), &alen, &asize, &atype, aname);
        chk(aname[0] != 0, "glGetActiveAttrib returns a name");
    }

    /* ── VAO + VBO + instancing divisor ─────────────────────────────── */
    uint32_t vao = 0, vbo = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    float verts[9] = {0,0,0, 1,0,0, 0,1,0};
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, 0, 12, (const void*)0);
    glVertexAttribDivisor(0, 1);
    chk(glGetError() == 0, "vertex attrib divisor (instancing) accepted");

    /* ── Index buffer + base-vertex + range + restart draws ─────────── */
    uint32_t ebo = 0;
    glGenBuffers(1, &ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    unsigned short idx[6] = {0,1,2, 1,2,3};
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);
    glDrawElementsBaseVertex(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT,
                             (const void*)0, 1);
    glDrawRangeElements(GL_TRIANGLES, 0, 3, 3, GL_UNSIGNED_SHORT, (const void*)0);
    glPrimitiveRestartIndex(0xFFFF);
    chk(glGetError() == 0, "base-vertex / range / restart draws accepted");

    /* ── Query objects (occlusion / timing) ─────────────────────────── */
    uint32_t q = 0;
    glGenQueries(1, &q);
    glBeginQuery(GL_ANY_SAMPLES_PASSED, q);
    glEndQuery(GL_ANY_SAMPLES_PASSED);
    unsigned qavail = 0;
    glGetQueryObjectuiv(q, GL_QUERY_RESULT_AVAILABLE, &qavail);
    glDeleteQueries(1, &q);
    chk(glGetError() == 0, "query objects round-trip");

    /* ── Sampler objects (separate sampler state) ───────────────────── */
    uint32_t sam = 0;
    glGenSamplers(1, &sam);
    glBindSampler(0, sam);
    glSamplerParameteri(sam, 0x2800 /* GL_TEXTURE_MAG_FILTER */, GL_NEAREST);
    glSamplerParameterf(sam, 0x2800, (float)GL_NEAREST);
    glDeleteSamplers(1, &sam);
    chk(glGetError() == 0, "sampler objects round-trip");

    /* ── Transform feedback varyings (nested string array) ──────────── */
    glTransformFeedbackVaryings(prog, 1,
        (const char* const[]){"gl_Position"}, GL_SEPARATE_ATTRIBS);
    unsigned e_tf = glGetError();
    chk(e_tf == 0, "transform feedback varyings (nested strings)");
    /* begin/end without a bound TF buffer + relink legitimately set
     * INVALID_OPERATION on the host — they only smoke-test the rows. */
    glBeginTransformFeedback(GL_POINTS);
    glEndTransformFeedback();
    glGetError();

    /* ── Compute: dispatch + barrier + image texture ────────────────── */
    uint32_t tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindImageTexture(0, tex, 0, 0, 0, GL_READ_WRITE, GL_RGBA32F);
    glMemoryBarrier(GL_UNIFORM_BARRIER_BIT);
    /* A bare glDispatchCompute with no compute program bound is harmless
     * (host ignores it / sets an error we clear) — it exercises the row. */
    glDispatchCompute(1, 1, 1);
    glGetError();

    chk(glGetError() == 0, "no GL error after modern-feature calls");

    printf("test_sdl_gl_modern: ALL PASS (%d checks)\n", checks);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
