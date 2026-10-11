// guest_glfw.c — guest-side GLFW shim for minecraft_weekend.
//
// The game links against GLFW (no AArch64 libglfw.a exists), so every
// glfw_* symbol is resolved at runtime against the emulator's thunked
// host libglfw.so.3 using the internal dlopen/dlsym syscalls
// (0x1002/0x1003), the same mechanism as guest_sdl.c / test_sdl_gl_triangle.c.
// Real GLFW transitively pulls in stdio/stdlib/string/math.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <GLFW/glfw3.h>

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

typedef int            (*fn_glfwInit)(void);
typedef void           (*fn_glfwTerminate)(void);
typedef void           (*fn_glfwWindowHint)(int, int);
typedef GLFWwindow*    (*fn_glfwCreateWindow)(int, int, const char*, GLFWmonitor*, GLFWwindow*);
typedef void           (*fn_glfwMakeContextCurrent)(GLFWwindow*);
typedef void           (*fn_glfwSwapInterval)(int);
typedef void           (*fn_glfwSwapBuffers)(GLFWwindow*);
typedef void           (*fn_glfwPollEvents)(void);
typedef int            (*fn_glfwWindowShouldClose)(GLFWwindow*);
typedef int            (*fn_glfwGetKey)(GLFWwindow*, int);
typedef int            (*fn_glfwGetMouseButton)(GLFWwindow*, int);
typedef void           (*fn_glfwGetCursorPos)(GLFWwindow*, double*, double*);
typedef void           (*fn_glfwSetCursorPos)(GLFWwindow*, double, double);
typedef void           (*fn_glfwSetInputMode)(GLFWwindow*, int, int);
typedef int            (*fn_glfwGetInputMode)(GLFWwindow*, int);
typedef void           (*fn_glfwGetFramebufferSize)(GLFWwindow*, int*, int*);
typedef void*          (*fn_glfwGetProcAddress)(const char*);
typedef GLFWerrorfun   (*fn_glfwSetErrorCallback)(GLFWerrorfun);
typedef GLFWkeyfun     (*fn_glfwSetKeyCallback)(GLFWwindow*, GLFWkeyfun);
typedef GLFWmousebuttonfun (*fn_glfwSetMouseButtonCallback)(GLFWwindow*, GLFWmousebuttonfun);
typedef GLFWcursorposfun   (*fn_glfwSetCursorPosCallback)(GLFWwindow*, GLFWcursorposfun);
typedef GLFWframebuffersizefun (*fn_glfwSetFramebufferSizeCallback)(GLFWwindow*, GLFWframebuffersizefun);

static uint64_t glfw_handle = 0;
static fn_glfwInit glfw_glfwInit;
static fn_glfwTerminate glfw_glfwTerminate;
static fn_glfwWindowHint glfw_glfwWindowHint;
static fn_glfwCreateWindow glfw_glfwCreateWindow;
static fn_glfwMakeContextCurrent glfw_glfwMakeContextCurrent;
static fn_glfwSwapInterval glfw_glfwSwapInterval;
static fn_glfwSwapBuffers glfw_glfwSwapBuffers;
static fn_glfwPollEvents glfw_glfwPollEvents;
static fn_glfwWindowShouldClose glfw_glfwWindowShouldClose;
static fn_glfwGetKey glfw_glfwGetKey;
static fn_glfwGetMouseButton glfw_glfwGetMouseButton;
static fn_glfwGetCursorPos glfw_glfwGetCursorPos;
static fn_glfwSetCursorPos glfw_glfwSetCursorPos;
static fn_glfwSetInputMode glfw_glfwSetInputMode;
static fn_glfwGetInputMode glfw_glfwGetInputMode;
static fn_glfwGetFramebufferSize glfw_glfwGetFramebufferSize;
static fn_glfwGetProcAddress glfw_glfwGetProcAddress;
static fn_glfwSetErrorCallback glfw_glfwSetErrorCallback;
static fn_glfwSetKeyCallback glfw_glfwSetKeyCallback;
static fn_glfwSetMouseButtonCallback glfw_glfwSetMouseButtonCallback;
static fn_glfwSetCursorPosCallback glfw_glfwSetCursorPosCallback;
static fn_glfwSetFramebufferSizeCallback glfw_glfwSetFramebufferSizeCallback;

static int glfw_ready = 0;

#define LOAD(name) do { \
    glfw_##name = (fn_##name)(uintptr_t)bifrost_dlsym(glfw_handle, #name); \
    if (!glfw_##name) { \
        fprintf(stderr, "minecraft_weekend: dlsym(%s) failed\n", #name); \
        return -1; \
    } \
} while (0)

static int init_glfw(void) {
    if (glfw_ready) return 0;
    glfw_handle = bifrost_dlopen("libglfw.so.3", 1);
    if (!glfw_handle) glfw_handle = bifrost_dlopen("libglfw.so", 1);
    if (!glfw_handle) {
        fprintf(stderr, "minecraft_weekend: libglfw thunk unavailable\n");
        return -1;
    }
    LOAD(glfwInit);
    LOAD(glfwTerminate);
    LOAD(glfwWindowHint);
    LOAD(glfwCreateWindow);
    LOAD(glfwMakeContextCurrent);
    LOAD(glfwSwapInterval);
    LOAD(glfwSwapBuffers);
    LOAD(glfwPollEvents);
    LOAD(glfwWindowShouldClose);
    LOAD(glfwGetKey);
    LOAD(glfwGetMouseButton);
    LOAD(glfwGetCursorPos);
    LOAD(glfwSetCursorPos);
    LOAD(glfwSetInputMode);
    LOAD(glfwGetInputMode);
    LOAD(glfwGetFramebufferSize);
    LOAD(glfwGetProcAddress);
    LOAD(glfwSetErrorCallback);
    LOAD(glfwSetKeyCallback);
    LOAD(glfwSetMouseButtonCallback);
    LOAD(glfwSetCursorPosCallback);
    LOAD(glfwSetFramebufferSizeCallback);
    glfw_ready = 1;
    return 0;
}

int glfwInit(void)                { if (init_glfw()) return GLFW_FALSE; return glfw_glfwInit(); }
void glfwTerminate(void)          { if (glfw_ready) glfw_glfwTerminate(); }
void glfwWindowHint(int h, int v) { if (init_glfw()) return; glfw_glfwWindowHint(h, v); }
GLFWwindow* glfwCreateWindow(int w, int h, const char* t, GLFWmonitor* m, GLFWwindow* s)
    { if (init_glfw()) return NULL; return glfw_glfwCreateWindow(w, h, t, m, s); }
void glfwMakeContextCurrent(GLFWwindow* w)  { if (glfw_ready) glfw_glfwMakeContextCurrent(w); }
void glfwSwapInterval(int i)      { if (glfw_ready) glfw_glfwSwapInterval(i); }
void glfwSwapBuffers(GLFWwindow* w){ if (glfw_ready) glfw_glfwSwapBuffers(w); }
void glfwPollEvents(void)         { if (glfw_ready) glfw_glfwPollEvents(); }
int glfwWindowShouldClose(GLFWwindow* w) { if (!glfw_ready) return 0; return glfw_glfwWindowShouldClose(w); }
int glfwGetKey(GLFWwindow* w, int k)   { if (!glfw_ready) return GLFW_RELEASE; return glfw_glfwGetKey(w, k); }
int glfwGetMouseButton(GLFWwindow* w, int b) { if (!glfw_ready) return GLFW_RELEASE; return glfw_glfwGetMouseButton(w, b); }
void glfwGetCursorPos(GLFWwindow* w, double* x, double* y) { if (glfw_ready) glfw_glfwGetCursorPos(w, x, y); }
void glfwSetCursorPos(GLFWwindow* w, double x, double y) { if (glfw_ready) glfw_glfwSetCursorPos(w, x, y); }
void glfwSetInputMode(GLFWwindow* w, int m, int v) { if (glfw_ready) glfw_glfwSetInputMode(w, m, v); }
int glfwGetInputMode(GLFWwindow* w, int m) { if (!glfw_ready) return 0; return glfw_glfwGetInputMode(w, m); }
void glfwGetFramebufferSize(GLFWwindow* w, int* x, int* y) { if (glfw_ready) glfw_glfwGetFramebufferSize(w, x, y); }
void* glfwGetProcAddress(const char* n) { if (!glfw_ready) return NULL; return glfw_glfwGetProcAddress(n); }
GLFWerrorfun glfwSetErrorCallback(GLFWerrorfun c) { if (init_glfw()) return NULL; return glfw_glfwSetErrorCallback(c); }
GLFWkeyfun glfwSetKeyCallback(GLFWwindow* w, GLFWkeyfun c) { if (glfw_ready) return glfw_glfwSetKeyCallback(w, c); }
GLFWmousebuttonfun glfwSetMouseButtonCallback(GLFWwindow* w, GLFWmousebuttonfun c) { if (glfw_ready) return glfw_glfwSetMouseButtonCallback(w, c); }
GLFWcursorposfun glfwSetCursorPosCallback(GLFWwindow* w, GLFWcursorposfun c) { if (glfw_ready) return glfw_glfwSetCursorPosCallback(w, c); }
GLFWframebuffersizefun glfwSetFramebufferSizeCallback(GLFWwindow* w, GLFWframebuffersizefun c) { if (glfw_ready) return glfw_glfwSetFramebufferSizeCallback(w, c); }
