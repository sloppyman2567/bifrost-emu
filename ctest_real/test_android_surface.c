// test_android_surface.c — Android NativeActivity surface layer v1.
//
// Exercises the ANativeWindow shim + EGL window-surface interception:
//   dlopen("libandroid.so")            -> thunk registration
//   ANativeWindow_fromSurface          -> shim handle (host SDL window)
//   getWidth/getHeight/getFormat       -> shim geometry
//   setBuffersGeometry                 -> geometry update
//   eglGetDisplay/Initialize/ChooseConfig/CreateWindowSurface(win!)
//                                      -> intercepted, host X11 window
//   eglMakeCurrent + glClear + eglSwapBuffers -> real pixels
//   eglQuerySurface                    -> dims match the shim
//
// Exit 0 = pass. Uses the raw dlopen/dlsym SVC trampolines (0x1002/0x1003)
// like the other thunk tests — no libc dl dependency.
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>

// ── Android NDK types (minimal) ───────────────────────────────────────
typedef struct ANativeWindow ANativeWindow;

// ── EGL constants (android/EGL headers subset) ────────────────────────
typedef void* EGLDisplay;
typedef void* EGLSurface;
typedef void* EGLContext;
typedef uintptr_t EGLConfig;
typedef int32_t EGLint;
#define EGL_DEFAULT_DISPLAY ((EGLDisplay)0)
#define EGL_NO_CONTEXT      ((EGLContext)0)
#define EGL_NO_SURFACE      ((EGLSurface)0)
#define EGL_SUCCESS         0x3000
#define EGL_NOT_INITIALIZED 0x3001
#define EGL_WIDTH           0x3057
#define EGL_HEIGHT          0x3056
#define EGL_BLUE_SIZE       0x3022
#define EGL_GREEN_SIZE      0x3023
#define EGL_RED_SIZE        0x3024
#define EGL_ALPHA_SIZE      0x3021
#define EGL_DEPTH_SIZE      0x3025
#define EGL_STENCIL_SIZE    0x3026
#define EGL_SURFACE_TYPE    0x3033
#define EGL_WINDOW_BIT      0x0004
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_BIT      0x0008
#define EGL_NONE            0x3038
#define EGL_OPENGL_API      0x30A2

// ── GL constants ──────────────────────────────────────────────────────
#define GL_COLOR_BUFFER_BIT 0x00004000

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

// ── ANativeWindow fn types + dlsym targets (variables named after the
//    symbols so the LOAD macro stringizes correctly) ───────────────────
typedef ANativeWindow* (*ANativeWindow_fromSurface_t)(void* env, void* surface);
typedef void        (*ANativeWindow_acquire_t)(ANativeWindow*);
typedef void        (*ANativeWindow_release_t)(ANativeWindow*);
typedef int32_t     (*ANativeWindow_getWidth_t)(ANativeWindow*);
typedef int32_t     (*ANativeWindow_getHeight_t)(ANativeWindow*);
typedef int32_t     (*ANativeWindow_getFormat_t)(ANativeWindow*);
typedef int32_t     (*ANativeWindow_setBuffersGeometry_t)(ANativeWindow*, int32_t, int32_t, int32_t);
static ANativeWindow_fromSurface_t ANativeWindow_fromSurface;
static ANativeWindow_acquire_t ANativeWindow_acquire;
static ANativeWindow_release_t ANativeWindow_release;
static ANativeWindow_getWidth_t ANativeWindow_getWidth;
static ANativeWindow_getHeight_t ANativeWindow_getHeight;
static ANativeWindow_getFormat_t ANativeWindow_getFormat;
static ANativeWindow_setBuffersGeometry_t ANativeWindow_setBuffersGeometry;

// ── EGL fn types ──────────────────────────────────────────────────────
typedef EGLDisplay  (*eglGetDisplay_t)(void* native_display);
typedef unsigned    (*eglInitialize_t)(EGLDisplay, EGLint*, EGLint*);
typedef unsigned    (*eglChooseConfig_t)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
typedef unsigned    (*eglBindAPI_t)(unsigned);
typedef EGLSurface  (*eglCreateWindowSurface_t)(EGLDisplay, EGLConfig, void*, const EGLint*);
typedef EGLContext  (*eglCreateContext_t)(EGLDisplay, EGLConfig, EGLContext, const EGLint*);
typedef unsigned    (*eglMakeCurrent_t)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
typedef unsigned    (*eglSwapBuffers_t)(EGLDisplay, EGLSurface);
typedef unsigned    (*eglQuerySurface_t)(EGLDisplay, EGLSurface, EGLint, EGLint*);
typedef unsigned    (*eglDestroySurface_t)(EGLDisplay, EGLSurface);
typedef unsigned    (*eglDestroyContext_t)(EGLDisplay, EGLContext);
typedef unsigned    (*eglTerminate_t)(EGLDisplay);
typedef unsigned    (*eglGetError_t)(void);

// ── GL fn types ───────────────────────────────────────────────────────
typedef void (*glViewport_t)(int, int, int, int);
typedef void (*glClearColor_t)(float, float, float, float);
typedef void (*glClear_t)(unsigned);

int main(void) {
    printf("test_android_surface: start\n");

    // ── libandroid.so → ANativeWindow shims ─────────────────────────
    uint64_t hand = bifrost_dlopen("libandroid.so", 1);
    chk(hand != 0, "dlopen libandroid.so");
    LOAD(hand, ANativeWindow_fromSurface_t, ANativeWindow_fromSurface);
    LOAD(hand, ANativeWindow_acquire_t, ANativeWindow_acquire);
    LOAD(hand, ANativeWindow_release_t, ANativeWindow_release);
    LOAD(hand, ANativeWindow_getWidth_t, ANativeWindow_getWidth);
    LOAD(hand, ANativeWindow_getHeight_t, ANativeWindow_getHeight);
    LOAD(hand, ANativeWindow_getFormat_t, ANativeWindow_getFormat);
    LOAD(hand, ANativeWindow_setBuffersGeometry_t, ANativeWindow_setBuffersGeometry);

    // v1: JNIEnv/jobject are opaque — the shim ignores them.
    ANativeWindow* win = ANativeWindow_fromSurface((void*)0x1234, (void*)0x5678);
    chk(win != 0, "ANativeWindow_fromSurface returns a window");
    chk(ANativeWindow_getWidth(win) > 0, "getWidth positive");
    chk(ANativeWindow_getHeight(win) > 0, "getHeight positive");
    chk(ANativeWindow_getFormat(win) == 1 /* WINDOW_FORMAT_RGBA_8888 */,
        "getFormat RGBA8888");
    chk(ANativeWindow_setBuffersGeometry(win, 640, 480, 0) == 0,
        "setBuffersGeometry");
    chk(ANativeWindow_getWidth(win) == 640, "width updated to 640");
    chk(ANativeWindow_getHeight(win) == 480, "height updated to 480");
    ANativeWindow_acquire(win);
    ANativeWindow_release(win);

    // ── libEGL.so → EGL against the shim window ─────────────────────
    uint64_t hegl = bifrost_dlopen("libEGL.so.1", 1);
    if (!hegl) hegl = bifrost_dlopen("libEGL.so", 1);
    chk(hegl != 0, "dlopen libEGL");
    eglGetDisplay_t eglGetDisplay; eglInitialize_t eglInitialize;
    eglChooseConfig_t eglChooseConfig; eglBindAPI_t eglBindAPI;
    eglCreateWindowSurface_t eglCreateWindowSurface;
    eglCreateContext_t eglCreateContext; eglMakeCurrent_t eglMakeCurrent;
    eglSwapBuffers_t eglSwapBuffers; eglQuerySurface_t eglQuerySurface;
    eglDestroySurface_t eglDestroySurface; eglDestroyContext_t eglDestroyContext;
    eglTerminate_t eglTerminate; eglGetError_t eglGetError;
    LOAD(hegl, eglGetDisplay_t, eglGetDisplay);
    LOAD(hegl, eglInitialize_t, eglInitialize);
    LOAD(hegl, eglChooseConfig_t, eglChooseConfig);
    LOAD(hegl, eglBindAPI_t, eglBindAPI);
    LOAD(hegl, eglCreateWindowSurface_t, eglCreateWindowSurface);
    LOAD(hegl, eglCreateContext_t, eglCreateContext);
    LOAD(hegl, eglMakeCurrent_t, eglMakeCurrent);
    LOAD(hegl, eglSwapBuffers_t, eglSwapBuffers);
    LOAD(hegl, eglQuerySurface_t, eglQuerySurface);
    LOAD(hegl, eglDestroySurface_t, eglDestroySurface);
    LOAD(hegl, eglDestroyContext_t, eglDestroyContext);
    LOAD(hegl, eglTerminate_t, eglTerminate);
    LOAD(hegl, eglGetError_t, eglGetError);

    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    chk(dpy != 0, "eglGetDisplay(EGL_DEFAULT_DISPLAY)");
    EGLint maj = 0, mnr = 0;
    chk(eglInitialize(dpy, &maj, &mnr) && maj > 0, "eglInitialize");

    // Desktop-GL-on-EGL config first (host Mesa usually supports it);
    // fall back to a plain window config (GLES/default renderable).
    // NOTE: host Mesa on a foreign (SDL-owned) wl_display occasionally
    // returns garbage *num_config with EGL_SUCCESS (observed ~50% of runs,
    // value differs each time — internal enumeration race). Retry until
    // the answer is sane; the plain-config query has never failed twice
    // in a row.
    EGLConfig cfgs[4];
    EGLint ncfg = 0;
    EGLint cfg_attribs_gl[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_NONE
    };
    EGLint cfg_attribs_plain[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_NONE
    };
    int use_gl = 0;
    // EGL spec: *num_config receives the TOTAL number of matching configs
    // (may exceed config_size — only config_size entries are copied).
    // Host Mesa also refuses repeated eglChooseConfig on a foreign
    // wl_display with EGL_BAD_CONFIG, so the FIRST query must be accepted.
    ncfg = -1;
    unsigned e1 = 0;
    int ok_gl = eglChooseConfig(dpy, cfg_attribs_gl, cfgs, 4, &ncfg);
    e1 = eglGetError();
    printf("  INFO gl-bit: ok=%d ncfg=%d err=0x%x cfg0=%p\n", ok_gl, ncfg,
           e1, (void*)(uintptr_t)cfgs[0]);
    if (ok_gl && ncfg > 0) {
        use_gl = 1;
    } else {
        ncfg = -1;
        chk(eglChooseConfig(dpy, cfg_attribs_plain, cfgs, 4, &ncfg) &&
            ncfg > 0, "eglChooseConfig window config");
    }
    printf("  INFO eglChooseConfig: %d config(s), %s\n", ncfg,
           use_gl ? "desktop GL" : "default/GLES");
    printf("  INFO eglChooseConfig: %d config(s), %s\n", ncfg,
           use_gl ? "desktop GL" : "default/GLES");

    chk(eglBindAPI(use_gl ? EGL_OPENGL_API : 0x30A0 /*OPENGL_ES_API*/) != 0,
        "eglBindAPI");

    // THE interception point: win is an ANativeWindow shim handle — the
    // thunk substitutes the host native window behind our back.
    EGLSurface surf = eglCreateWindowSurface(dpy, cfgs[0], (void*)win, NULL);
    chk(surf != EGL_NO_SURFACE, "eglCreateWindowSurface(android shim)");
    chk(eglGetError() == EGL_SUCCESS, "no EGL error after create");

    EGLint ctx_attribs[] = { 0x3098 /*CONTEXT_CLIENT_VERSION*/, 2, EGL_NONE };
    EGLContext ctx = use_gl
        ? eglCreateContext(dpy, cfgs[0], EGL_NO_CONTEXT, NULL)
        : eglCreateContext(dpy, cfgs[0], EGL_NO_CONTEXT, ctx_attribs);
    chk(ctx != EGL_NO_CONTEXT, "eglCreateContext");
    chk(eglMakeCurrent(dpy, surf, surf, ctx) != 0, "eglMakeCurrent");

    uint64_t hgl = bifrost_dlopen(use_gl ? "libGL.so.1" : "libGLESv2.so", 1);
    if (!hgl) hgl = bifrost_dlopen("libGLESv2.so.2", 1);
    chk(hgl != 0, "dlopen GL library");
    glViewport_t glViewport; glClearColor_t glClearColor; glClear_t glClear;
    LOAD(hgl, glViewport_t, glViewport);
    LOAD(hgl, glClearColor_t, glClearColor);
    LOAD(hgl, glClear_t, glClear);
    glViewport(0, 0, 640, 480);
    glClearColor(0.15f, 0.35f, 0.75f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    chk(eglSwapBuffers(dpy, surf) != 0, "eglSwapBuffers (presents)");

    EGLint qw = 0, qh = 0;
    chk(eglQuerySurface(dpy, surf, EGL_WIDTH, &qw) && qw == 640,
        "eglQuerySurface width == 640");
    chk(eglQuerySurface(dpy, surf, EGL_HEIGHT, &qh) && qh == 480,
        "eglQuerySurface height == 480");

    // ── teardown ─────────────────────────────────────────────────────
    chk(eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) != 0,
        "eglMakeCurrent(NULL)");
    chk(eglDestroySurface(dpy, surf) != 0, "eglDestroySurface");
    chk(eglDestroyContext(dpy, ctx) != 0, "eglDestroyContext");
    chk(eglTerminate(dpy) != 0, "eglTerminate");

    printf("test_android_surface: ALL PASS (%d checks)\n", checks);
    return 0;
}
