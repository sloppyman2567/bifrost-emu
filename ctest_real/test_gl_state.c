/* test_gl_state.c — GL state tracker test for bifrost-emu.
 *
 * Verifies that the GLStateTracker correctly mirrors guest OpenGL state
 * by setting state, querying it back, and comparing with expected values.
 *
 * Uses bifrost internal syscalls 0x1002 (dlopen) / 0x1003 (dlsym) so a
 * static musl binary can resolve thunked libGL symbols.
 *
 * Build:
 *   make cross SRC=ctest_real/test_gl_state.c OUT=ctest_real/test_gl_state.elf
 *
 * Run:
 *   ./bifrost-emu ctest_real/test_gl_state.elf
 *
 * Success = exit 0. Exit 77 = skip when libGL thunk unavailable.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ── Bifrost internal dlopen/dlsym syscalls ─────────────────────────── */
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
} while(0)

/* ── GL enum constants (subset needed by the test) ──────────────────── */
#define GL_DEPTH_TEST              0x0B71
#define GL_CULL_FACE               0x0B44
#define GL_BLEND                   0x0BE2
#define GL_SCISSOR_TEST            0x0C11
#define GL_STENCIL_TEST            0x0B90
#define GL_TEXTURE_2D              0x0DE1
#define GL_LINE_SMOOTH             0x0B20
#define GL_POLYGON_OFFSET_FILL     0x8037
#define GL_SAMPLE_ALPHA_TO_COVERAGE 0x809E
#define GL_SAMPLE_COVERAGE         0x80A0
#define GL_MULTISAMPLE             0x809D

#define GL_FALSE                   0
#define GL_TRUE                    1

#define GL_NEVER                   0x0200
#define GL_LESS                    0x0201
#define GL_EQUAL                   0x0202
#define GL_LEQUAL                  0x0203
#define GL_GREATER                 0x0204
#define GL_NOTEQUAL                0x0205
#define GL_GEQUAL                  0x0206
#define GL_ALWAYS                  0x0207
#define GL_KEEP                    0x1E00
#define GL_REPLACE                 0x1E01
#define GL_INCR                    0x1E02
#define GL_DECR                    0x1E03
#define GL_INVERT                  0x150A

#define GL_FRONT                   0x0404
#define GL_BACK                    0x0405
#define GL_FRONT_AND_BACK          0x0408
#define GL_CW                      0x0900
#define GL_CCW                     0x0901

#define GL_POINT                   0x1B00
#define GL_LINE                    0x1B01
#define GL_FILL                    0x1B02

#define GL_SRC_ALPHA               0x0302
#define GL_ONE_MINUS_SRC_ALPHA     0x0303
#define GL_DST_ALPHA               0x0304
#define GL_ONE_MINUS_DST_ALPHA     0x0305
#define GL_SRC_COLOR               0x0300
#define GL_ONE_MINUS_SRC_COLOR     0x0301
#define GL_DST_COLOR               0x0306
#define GL_ONE_MINUS_DST_COLOR     0x0307
#define GL_SRC_ALPHA_SATURATE      0x0308
#define GL_ZERO                    0
#define GL_ONE                     1

#define GL_TEXTURE0                0x84C0
#define GL_TEXTURE1                0x84C1

#define GL_VIEWPORT                0x0BA2
#define GL_COLOR_CLEAR_VALUE       0x0C22
#define GL_COLOR_WRITEMASK         0x0BA3
#define GL_DEPTH_WRITEMASK         0x0B72
#define GL_DEPTH_FUNC              0x0B74
#define GL_DEPTH_RANGE             0x0B70
#define GL_BLEND_SRC               0x0BE1
#define GL_BLEND_DST               0x0BE0
#define GL_BLEND_SRC_ALPHA         0x0BC1
#define GL_BLEND_DST_ALPHA         0x0BC0
#define GL_BLEND_EQUATION_RGB      0x8009
#define GL_BLEND_EQUATION_ALPHA    0x883D
#define GL_CULL_FACE_MODE          0x0B45
#define GL_FRONT_FACE              0x0B46
#define GL_LINE_WIDTH              0x0B21
#define GL_POINT_SIZE              0x0B11
#define GL_POLYGON_MODE            0x0B40
#define GL_SCISSOR_BOX             0x0C10
#define GL_STENCIL_FUNC            0x0B92
#define GL_STENCIL_VALUE_MASK      0x0B93
#define GL_STENCIL_FAIL            0x0B94
#define GL_STENCIL_PASS_DEPTH_FAIL 0x0B95
#define GL_STENCIL_PASS_DEPTH_PASS 0x0B96
#define GL_STENCIL_WRITEMASK       0x0B98
#define GL_STENCIL_BACK_FUNC       0x8800
#define GL_STENCIL_BACK_VALUE_MASK 0x8CA4
#define GL_STENCIL_BACK_FAIL       0x8801
#define GL_STENCIL_BACK_PASS_DEPTH_FAIL 0x8802
#define GL_STENCIL_BACK_PASS_DEPTH_PASS 0x8803
#define GL_STENCIL_BACK_WRITEMASK  0x8CA5
#define GL_STENCIL_REF             0x0B97
#define GL_STENCIL_BACK_REF        0x8CA3
#define GL_ACTIVE_TEXTURE          0x84E0
#define GL_ARRAY_BUFFER_BINDING    0x8894
#define GL_ELEMENT_ARRAY_BUFFER_BINDING 0x8895
#define GL_TEXTURE_BINDING_2D      0x8069
#define GL_CURRENT_PROGRAM         0x8B8D
#define GL_MAX_TEXTURE_SIZE        0x0D33
#define GL_MAX_VERTEX_ATTRIBS      0x8869
#define GL_MAX_TEXTURE_IMAGE_UNITS 0x8872
#define GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS 0x8B4D
#define GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS 0x8B4C
#define GL_MAX_FRAGMENT_UNIFORM_COMPONENTS 0x8B49
#define GL_MAX_VERTEX_UNIFORM_COMPONENTS 0x8B4A
#define GL_NUM_EXTENSIONS          0x821D
#define GL_RED_BITS                0x00D5
#define GL_GREEN_BITS              0x00D6
#define GL_BLUE_BITS               0x00D7
#define GL_ALPHA_BITS              0x00D8
#define GL_DEPTH_BITS              0x00D2
#define GL_STENCIL_BITS            0x00D3
#define GL_MAJOR_VERSION           0x821B
#define GL_MINOR_VERSION           0x821C

#define GL_FLOAT                   0x1406
#define GL_INT                     0x1404
#define GL_UNSIGNED_BYTE           0x1401
#define GL_TRIANGLES               0x0004
#define GL_ARRAY_BUFFER            0x8892
#define GL_ELEMENT_ARRAY_BUFFER    0x8893
#define GL_STATIC_DRAW             0x88E4
#define GL_DYNAMIC_DRAW            0x88E8

/* ── Function pointer types ──────────────────────────────────────────── */
typedef void (*glEnable_t)(unsigned);
typedef void (*glDisable_t)(unsigned);
typedef unsigned (*glIsEnabled_t)(unsigned);
typedef void (*glClearColor_t)(float, float, float, float);
typedef void (*glViewport_t)(int, int, int, int);
typedef void (*glBlendFunc_t)(unsigned, unsigned);
typedef void (*glBlendFuncSeparate_t)(unsigned, unsigned, unsigned, unsigned);
typedef void (*glDepthFunc_t)(unsigned);
typedef void (*glDepthMask_t)(unsigned char);
typedef void (*glCullFace_t)(unsigned);
typedef void (*glFrontFace_t)(unsigned);
typedef void (*glScissor_t)(int, int, int, int);
typedef void (*glStencilFunc_t)(unsigned, int, unsigned);
typedef void (*glStencilOp_t)(unsigned, unsigned, unsigned);
typedef void (*glStencilMask_t)(unsigned);
typedef void (*glStencilFuncSeparate_t)(unsigned, unsigned, int, unsigned);
typedef void (*glStencilOpSeparate_t)(unsigned, unsigned, unsigned, unsigned);
typedef void (*glStencilMaskSeparate_t)(unsigned, unsigned);
typedef void (*glLineWidth_t)(float);
typedef void (*glPointSize_t)(float);
typedef void (*glActiveTexture_t)(unsigned);
typedef void (*glBindBuffer_t)(unsigned, unsigned);
typedef void (*glBindTexture_t)(unsigned, unsigned);
typedef void (*glUseProgram_t)(unsigned);
typedef void (*glPixelStorei_t)(unsigned, int);
typedef void (*glHint_t)(unsigned, unsigned);
typedef void (*glClear_t)(unsigned);
typedef void (*glFlush_t)(void);
typedef unsigned int (*glGetError_t)(void);
typedef const unsigned char* (*glGetString_t)(unsigned);
typedef void (*glGetIntegerv_t)(unsigned, int*);
typedef void (*glGetFloatv_t)(unsigned, float*);
typedef void (*glGetBooleanv_t)(unsigned, unsigned char*);

/* ── Helpers ──────────────────────────────────────────────────────────── */
static int check_eq(const char* name, int got, int want) {
    if (got != want) {
        printf("FAIL: %s: got %d, want %d\n", name, got, want);
        return 1;
    }
    return 0;
}

static int check_eq_f(const char* name, float got, float want) {
    if (fabsf(got - want) > 0.01f) {
        printf("FAIL: %s: got %f, want %f\n", name, got, want);
        return 1;
    }
    return 0;
}
static int check_eq_hex(const char* name, uint32_t got, uint32_t want) {
    if (got != want) {
        printf("FAIL: %s: got 0x%x, want 0x%x\n", name, got, want);
        return 1;
    }
    return 0;
}

/* ── Main test ────────────────────────────────────────────────────────── */
int main(void) {
    printf("test_gl_state: start\n");

    uint64_t hgl = bifrost_dlopen("libGL.so.1", 1);
    if (!hgl) hgl = bifrost_dlopen("libGL.so", 1);
    if (!hgl) {
        printf("test_gl_state: SKIP (libGL thunk unavailable)\n");
        return 77;
    }
    printf("test_gl_state: dlopen OK hgl=%llx\n", (unsigned long long)hgl);

    glEnable_t glEnable;
    glDisable_t glDisable;
    glIsEnabled_t glIsEnabled;
    glClearColor_t glClearColor;
    glViewport_t glViewport;
    glBlendFunc_t glBlendFunc;
    glBlendFuncSeparate_t glBlendFuncSeparate;
    glDepthFunc_t glDepthFunc;
    glDepthMask_t glDepthMask;
    glCullFace_t glCullFace;
    glFrontFace_t glFrontFace;
    glScissor_t glScissor;
    glStencilFunc_t glStencilFunc;
    glStencilOp_t glStencilOp;
    glStencilMask_t glStencilMask;
    glStencilFuncSeparate_t glStencilFuncSeparate;
    glStencilOpSeparate_t glStencilOpSeparate;
    glStencilMaskSeparate_t glStencilMaskSeparate;
    glLineWidth_t glLineWidth;
    glPointSize_t glPointSize;
    glActiveTexture_t glActiveTexture;
    glBindBuffer_t glBindBuffer;
    glBindTexture_t glBindTexture;
    glUseProgram_t glUseProgram;
    glPixelStorei_t glPixelStorei;
    glHint_t glHint;
    glClear_t glClear;
    glFlush_t glFlush;
    glGetIntegerv_t glGetIntegerv;
    glGetFloatv_t glGetFloatv;
    glGetBooleanv_t glGetBooleanv;
    glGetString_t glGetString;
    glGetError_t glGetError;
    LOAD(hgl, glEnable_t, glEnable);
    LOAD(hgl, glDisable_t, glDisable);
    LOAD(hgl, glIsEnabled_t, glIsEnabled);
    LOAD(hgl, glClearColor_t, glClearColor);
    LOAD(hgl, glViewport_t, glViewport);
    LOAD(hgl, glBlendFunc_t, glBlendFunc);
    LOAD(hgl, glBlendFuncSeparate_t, glBlendFuncSeparate);
    LOAD(hgl, glDepthFunc_t, glDepthFunc);
    LOAD(hgl, glDepthMask_t, glDepthMask);
    LOAD(hgl, glCullFace_t, glCullFace);
    LOAD(hgl, glFrontFace_t, glFrontFace);
    LOAD(hgl, glScissor_t, glScissor);
    LOAD(hgl, glStencilFunc_t, glStencilFunc);
    LOAD(hgl, glStencilOp_t, glStencilOp);
    LOAD(hgl, glStencilMask_t, glStencilMask);
    LOAD(hgl, glStencilFuncSeparate_t, glStencilFuncSeparate);
    LOAD(hgl, glStencilOpSeparate_t, glStencilOpSeparate);
    LOAD(hgl, glStencilMaskSeparate_t, glStencilMaskSeparate);
    LOAD(hgl, glLineWidth_t, glLineWidth);
    LOAD(hgl, glPointSize_t, glPointSize);
    LOAD(hgl, glActiveTexture_t, glActiveTexture);
    LOAD(hgl, glBindBuffer_t, glBindBuffer);
    LOAD(hgl, glBindTexture_t, glBindTexture);
    LOAD(hgl, glUseProgram_t, glUseProgram);
    LOAD(hgl, glPixelStorei_t, glPixelStorei);
    LOAD(hgl, glHint_t, glHint);
    LOAD(hgl, glClear_t, glClear);
    LOAD(hgl, glFlush_t, glFlush);
    LOAD(hgl, glGetIntegerv_t, glGetIntegerv);
    LOAD(hgl, glGetFloatv_t, glGetFloatv);
    LOAD(hgl, glGetBooleanv_t, glGetBooleanv);
    LOAD(hgl, glGetString_t, glGetString);
    LOAD(hgl, glGetError_t, glGetError);

    int failures = 0;

    /* ── Test 1: glEnable / glIsEnabled ──────────────────────────────── */
    printf("test_gl_state: [1] glEnable/glIsEnabled\n");
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glEnable(GL_SCISSOR_TEST);
    glEnable(GL_STENCIL_TEST);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_LINE_SMOOTH);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    glEnable(GL_SAMPLE_COVERAGE);
    glEnable(GL_MULTISAMPLE);

    {
        unsigned val = glIsEnabled(GL_DEPTH_TEST);
        failures += check_eq("GL_DEPTH_TEST", val, GL_TRUE);
    }
    {
        unsigned val = glIsEnabled(GL_CULL_FACE);
        failures += check_eq("GL_CULL_FACE", val, GL_TRUE);
    }
    {
        unsigned val = glIsEnabled(GL_BLEND);
        failures += check_eq("GL_BLEND", val, GL_TRUE);
    }
    {
        unsigned val = glIsEnabled(GL_SCISSOR_TEST);
        failures += check_eq("GL_SCISSOR_TEST", val, GL_TRUE);
    }
    {
        unsigned val = glIsEnabled(GL_STENCIL_TEST);
        failures += check_eq("GL_STENCIL_TEST", val, GL_TRUE);
    }
    {
        unsigned val = glIsEnabled(GL_TEXTURE_2D);
        failures += check_eq("GL_TEXTURE_2D", val, GL_TRUE);
    }
    {
        unsigned val = glIsEnabled(GL_LINE_SMOOTH);
        failures += check_eq("GL_LINE_SMOOTH", val, GL_TRUE);
    }
    {
        unsigned val = glIsEnabled(GL_POLYGON_OFFSET_FILL);
        failures += check_eq("GL_POLYGON_OFFSET_FILL", val, GL_TRUE);
    }
    {
        unsigned val = glIsEnabled(GL_SAMPLE_ALPHA_TO_COVERAGE);
        failures += check_eq("GL_SAMPLE_ALPHA_TO_COVERAGE", val, GL_TRUE);
    }
    {
        unsigned val = glIsEnabled(GL_SAMPLE_COVERAGE);
        failures += check_eq("GL_SAMPLE_COVERAGE", val, GL_TRUE);
    }
    {
        unsigned val = glIsEnabled(GL_MULTISAMPLE);
        failures += check_eq("GL_MULTISAMPLE", val, GL_TRUE);
    }

    /* Untracked cap should return FALSE. */
    {
        unsigned val = glIsEnabled(0xFFFF); // bogus enum
        failures += check_eq("bogus cap", val, GL_FALSE);
    }

    /* Now disable some and verify. */
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    {
        unsigned val = glIsEnabled(GL_BLEND);
        failures += check_eq("GL_BLEND after disable", val, GL_FALSE);
    }
    {
        unsigned val = glIsEnabled(GL_CULL_FACE);
        failures += check_eq("GL_CULL_FACE after disable", val, GL_FALSE);
    }
    {
        unsigned val = glIsEnabled(GL_DEPTH_TEST);
        failures += check_eq("GL_DEPTH_TEST still enabled", val, GL_TRUE);
    }

    /* ── Test 2: glClearColor + glGetFloatv(GL_COLOR_CLEAR_VALUE) ────── */
    printf("test_gl_state: [2] glClearColor / glGetFloatv\n");
    glClearColor(0.1f, 0.2f, 0.3f, 0.4f);
    {
        float cv[4] = {0};
        glGetFloatv(GL_COLOR_CLEAR_VALUE, cv);
        failures += check_eq_f("clear_color[0]", cv[0], 0.1f);
        failures += check_eq_f("clear_color[1]", cv[1], 0.2f);
        failures += check_eq_f("clear_color[2]", cv[2], 0.3f);
        failures += check_eq_f("clear_color[3]", cv[3], 0.4f);
    }

    /* ── Test 3: glViewport + glGetIntegerv(GL_VIEWPORT) ──────────────── */
    printf("test_gl_state: [3] glViewport / glGetIntegerv\n");
    glViewport(10, 20, 640, 480);
    {
        int vp[4] = {0};
        glGetIntegerv(GL_VIEWPORT, vp);
        failures += check_eq("viewport[0]", vp[0], 10);
        failures += check_eq("viewport[1]", vp[1], 20);
        failures += check_eq("viewport[2]", vp[2], 640);
        failures += check_eq("viewport[3]", vp[3], 480);
    }

    /* ── Test 4: glBlendFunc + glGetIntegerv ──────────────────────────── */
    printf("test_gl_state: [4] glBlendFunc / glGetIntegerv\n");
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    {
        int src = 0, dst = 0;
        glGetIntegerv(GL_BLEND_SRC, &src);
        glGetIntegerv(GL_BLEND_DST, &dst);
        failures += check_eq_hex("BLEND_SRC", src, GL_SRC_ALPHA);
        failures += check_eq_hex("BLEND_DST", dst, GL_ONE_MINUS_SRC_ALPHA);
    }
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                        GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    {
        int src_a = 0, dst_a = 0;
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &src_a);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &dst_a);
        failures += check_eq_hex("BLEND_SRC_ALPHA", src_a, GL_ONE);
        failures += check_eq_hex("BLEND_DST_ALPHA", dst_a, GL_ONE_MINUS_SRC_ALPHA);
    }

    /* ── Test 5: glDepthFunc + glDepthMask ────────────────────────────── */
    printf("test_gl_state: [5] glDepthFunc / glDepthMask\n");
    glDepthFunc(GL_GREATER);
    glDepthMask(0);
    {
        int func = 0;
        glGetIntegerv(GL_DEPTH_FUNC, &func);
        failures += check_eq_hex("DEPTH_FUNC", func, GL_GREATER);
    }
    {
        unsigned char mask = 0;
        glGetBooleanv(GL_DEPTH_WRITEMASK, &mask);
        failures += check_eq("DEPTH_WRITEMASK", mask, GL_FALSE);
    }

    /* ── Test 6: glLineWidth + glPointSize ────────────────────────────── */
    printf("test_gl_state: [6] glLineWidth / glPointSize\n");
    glLineWidth(2.5f);
    glPointSize(8.0f);
    {
        float lw = 0;
        glGetFloatv(GL_LINE_WIDTH, &lw);
        failures += check_eq_f("LINE_WIDTH", lw, 2.5f);
    }
    {
        float ps = 0;
        glGetFloatv(GL_POINT_SIZE, &ps);
        failures += check_eq_f("POINT_SIZE", ps, 8.0f);
    }

    /* ── Test 7: glActiveTexture + glBindTexture ──────────────────────── */
    printf("test_gl_state: [7] glActiveTexture / glBindTexture\n");
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(0x8069, 42);
    {
        int unit = 0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &unit);
        failures += check_eq_hex("ACTIVE_TEXTURE", unit, GL_TEXTURE1);
    }
    {
        int tex = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
        failures += check_eq("TEXTURE_BINDING_2D", tex, 42);
    }
    /* Switch back to TEXTURE0 and bind a different texture. */
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(0x8069, 99);
    {
        int tex = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
        failures += check_eq("TEXTURE_BINDING_2D unit0", tex, 99);
    }
    /* Verify TEXTURE1 still has 42. */
    glActiveTexture(GL_TEXTURE1);
    {
        int tex = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
        failures += check_eq("TEXTURE_BINDING_2D unit1", tex, 42);
    }

    /* ── Test 8: glBindBuffer ─────────────────────────────────────────── */
    printf("test_gl_state: [8] glBindBuffer\n");
    glBindBuffer(0x8892, 100); // GL_ARRAY_BUFFER
    glBindBuffer(0x8893, 200); // GL_ELEMENT_ARRAY_BUFFER
    {
        int buf = 0;
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buf);
        failures += check_eq("ARRAY_BUFFER_BINDING", buf, 100);
    }
    {
        int buf = 0;
        glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &buf);
        failures += check_eq("ELEMENT_ARRAY_BUFFER_BINDING", buf, 200);
    }

    /* ── Test 9: glUseProgram ─────────────────────────────────────────── */
    printf("test_gl_state: [9] glUseProgram\n");
    glUseProgram(123);
    {
        int prog = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
        failures += check_eq("CURRENT_PROGRAM", prog, 123);
    }

    /* ── Test 10: glScissor + glGetIntegerv(GL_SCISSOR_BOX) ───────────── */
    printf("test_gl_state: [10] glScissor / GL_SCISSOR_BOX\n");
    glScissor(5, 10, 320, 240);
    {
        int box[4] = {0};
        glGetIntegerv(GL_SCISSOR_BOX, box);
        failures += check_eq("scissor[0]", box[0], 5);
        failures += check_eq("scissor[1]", box[1], 10);
        failures += check_eq("scissor[2]", box[2], 320);
        failures += check_eq("scissor[3]", box[3], 240);
    }

    /* ── Test 11: glCullFace + glFrontFace ────────────────────────────── */
    printf("test_gl_state: [11] glCullFace / glFrontFace\n");
    glCullFace(GL_FRONT);
    glFrontFace(GL_CW);
    {
        int mode = 0;
        glGetIntegerv(GL_CULL_FACE_MODE, &mode);
        failures += check_eq_hex("CULL_FACE_MODE", mode, GL_FRONT);
    }
    {
        int mode = 0;
        glGetIntegerv(GL_FRONT_FACE, &mode);
        failures += check_eq_hex("FRONT_FACE", mode, GL_CW);
    }

    /* ── Test 12: glStencilFunc / glStencilOp / glStencilMask ─────────── */
    printf("test_gl_state: [12] glStencilFunc / glStencilOp / glStencilMask\n");
    glStencilFunc(GL_ALWAYS, 5, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glStencilMask(0xFF);
    {
        int func = 0, ref = 0, mask = 0;
        glGetIntegerv(GL_STENCIL_FUNC, &func);
        glGetIntegerv(GL_STENCIL_REF, &ref);
        glGetIntegerv(GL_STENCIL_VALUE_MASK, &mask);
        failures += check_eq_hex("STENCIL_FUNC", func, GL_ALWAYS);
        failures += check_eq("STENCIL_REF", ref, 5);
        failures += check_eq_hex("STENCIL_VALUE_MASK", mask, 0xFF);
    }
    {
        int fail = 0, zfail = 0, zpass = 0;
        glGetIntegerv(GL_STENCIL_FAIL, &fail);
        glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL, &zfail);
        glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &zpass);
        failures += check_eq_hex("STENCIL_FAIL", fail, GL_KEEP);
        failures += check_eq_hex("STENCIL_PASS_DEPTH_FAIL", zfail, GL_KEEP);
        failures += check_eq_hex("STENCIL_PASS_DEPTH_PASS", zpass, GL_REPLACE);
    }
    {
        int mask = 0;
        glGetIntegerv(GL_STENCIL_WRITEMASK, &mask);
        failures += check_eq_hex("STENCIL_WRITEMASK", mask, 0xFF);
    }

    /* ── Test 13: glStencilFuncSeparate / glStencilOpSeparate ─────────── */
    printf("test_gl_state: [13] glStencilFuncSeparate / glStencilOpSeparate\n");
    glStencilFuncSeparate(GL_BACK, GL_EQUAL, 3, 0xF0);
    glStencilOpSeparate(GL_BACK, GL_INCR, GL_DECR, GL_INVERT);
    glStencilMaskSeparate(GL_BACK, 0x0F);
    {
        int func = 0, ref = 0, mask = 0;
        glGetIntegerv(GL_STENCIL_BACK_FUNC, &func);
        glGetIntegerv(GL_STENCIL_BACK_REF, &ref);
        glGetIntegerv(GL_STENCIL_BACK_VALUE_MASK, &mask);
        failures += check_eq_hex("STENCIL_BACK_FUNC", func, GL_EQUAL);
        failures += check_eq("STENCIL_BACK_REF", ref, 3);
        failures += check_eq_hex("STENCIL_BACK_VALUE_MASK", mask, 0xF0);
    }
    {
        int fail = 0, zfail = 0, zpass = 0;
        glGetIntegerv(GL_STENCIL_BACK_FAIL, &fail);
        glGetIntegerv(GL_STENCIL_BACK_PASS_DEPTH_FAIL, &zfail);
        glGetIntegerv(GL_STENCIL_BACK_PASS_DEPTH_PASS, &zpass);
        failures += check_eq_hex("STENCIL_BACK_FAIL", fail, GL_INCR);
        failures += check_eq_hex("STENCIL_BACK_PASS_DEPTH_FAIL", zfail, GL_DECR);
        failures += check_eq_hex("STENCIL_BACK_PASS_DEPTH_PASS", zpass, GL_INVERT);
    }
    {
        int mask = 0;
        glGetIntegerv(GL_STENCIL_BACK_WRITEMASK, &mask);
        failures += check_eq_hex("STENCIL_BACK_WRITEMASK", mask, 0x0F);
    }

    /* ── Test 14: glPixelStorei / glHint ──────────────────────────────── */
    printf("test_gl_state: [14] glPixelStorei / glHint\n");
    glPixelStorei(0x0CF5, 8); // GL_UNPACK_ALIGNMENT
    glPixelStorei(0x0D31, 1); // GL_PACK_ALIGNMENT
    glHint(0x0C50, 0x1402);   // GL_FOG_HINT = GL_NICEST

    /* ── Test 15: untracked glGetIntegerv pnames fall through ─────────── */
    printf("test_gl_state: [15] untracked pnames fall through to host\n");
    {
        int major = 0, minor = 0;
        glGetIntegerv(GL_MAJOR_VERSION, &major);
        glGetIntegerv(GL_MINOR_VERSION, &minor);
        /* Host should return something sensible; just verify it doesn't crash. */
        printf("test_gl_state: GL_MAJOR_VERSION=%d GL_MINOR_VERSION=%d\n",
               major, minor);
    }
    {
        const unsigned char* vendor = glGetString(0x1F00); /* GL_VENDOR */
        printf("test_gl_state: GL_VENDOR=%s\n",
               vendor ? (const char*)vendor : "(null)");
    }

    /* ── Test 16: state transitions ──────────────────────────────────── */
    printf("test_gl_state: [16] state transitions\n");
    glDisable(GL_DEPTH_TEST);
    {
        unsigned val = glIsEnabled(GL_DEPTH_TEST);
        failures += check_eq("GL_DEPTH_TEST re-disable", val, GL_FALSE);
    }
    glEnable(GL_DEPTH_TEST);
    {
        unsigned val = glIsEnabled(GL_DEPTH_TEST);
        failures += check_eq("GL_DEPTH_TEST re-enable", val, GL_TRUE);
    }
    glViewport(0, 0, 800, 600);
    {
        int vp[4] = {0};
        glGetIntegerv(GL_VIEWPORT, vp);
        failures += check_eq("viewport[0] after re-set", vp[0], 0);
        failures += check_eq("viewport[1] after re-set", vp[1], 0);
        failures += check_eq("viewport[2] after re-set", vp[2], 800);
        failures += check_eq("viewport[3] after re-set", vp[3], 600);
    }

    /* ── Summary ──────────────────────────────────────────────────────── */
    if (failures > 0) {
        printf("test_gl_state: FAIL (%d checks failed)\n", failures);
        return 1;
    }
    printf("test_gl_state: ALL PASS\n");
    return 0;
}
