/* test_gles_uint_index.c — an ES 3.00 array index of the form `(uint_expr) * 1 + 0`.
 *
 * GLSL ES 3.00 has no implicit int -> uint conversion, so Mesa rejects `uint * 1` ("could not
 * implicitly convert operands to arithmetic operator"); mobile drivers accept it. SPIRV-Cross
 * writes flattened uniform arrays this way, and Roblox's terrain, grass and mesh vertex shaders
 * (29 of the 925 in its GLSL ES 3 pack) failed to compile on Mesa because of it. glCompileShader
 * retries a rejected ES 3.x source once with the index operand wrapped in int(). The test checks
 * that the shader compiles and links, that an int index and a shader with a real error behave as
 * they would without the retry, and that a shader can be sourced and compiled again.
 *
 * Build:
 *   make cross SRC=Tests/Apps/test_gles_uint_index.c OUT=Tests/Apps/test_gles_uint_index.elf
 * Run (needs host EGL + GLES 3, GPU access; no window):
 *   ./bifrost-emu Tests/Apps/test_gles_uint_index.elf
 * Exits 77 (skip) without host EGL/GLES 3. On a driver that accepts the unmodified source it
 * passes without exercising the retry.
 */
#include <stdint.h>
#include <stdio.h>

#define EGL_NONE 0x3038
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 0x0001
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES3_BIT 0x0040
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82

static uint64_t bifrost_dlopen(const char *path, uint64_t mode) {
    register uint64_t x0 __asm__("x0") = (uint64_t)(uintptr_t)path;
    register uint64_t x1 __asm__("x1") = mode;
    register uint64_t x8 __asm__("x8") = 0x1002;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}
static uint64_t bifrost_dlsym(uint64_t handle, const char *name) {
    register uint64_t x0 __asm__("x0") = handle;
    register uint64_t x1 __asm__("x1") = (uint64_t)(uintptr_t)name;
    register uint64_t x8 __asm__("x8") = 0x1003;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}
#define FN(T, name) static __typeof__(T) name;
#define LOAD(h, name) name = (__typeof__(name))(uintptr_t)bifrost_dlsym(h, #name); \
    if (!name) { printf("SKIP: no %s\n", #name); return 77; }

FN(void *(*)(uintptr_t), eglGetDisplay)
FN(unsigned (*)(void *, int *, int *), eglInitialize)
FN(unsigned (*)(unsigned), eglBindAPI)
FN(unsigned (*)(void *, const int *, void **, int, int *), eglChooseConfig)
FN(void *(*)(void *, void *, const int *), eglCreatePbufferSurface)
FN(void *(*)(void *, void *, void *, const int *), eglCreateContext)
FN(unsigned (*)(void *, void *, void *, void *), eglMakeCurrent)
FN(unsigned (*)(unsigned), glCreateShader)
FN(void (*)(unsigned, int, const char *const *, const int *), glShaderSource)
FN(void (*)(unsigned), glCompileShader)
FN(void (*)(unsigned, unsigned, int *), glGetShaderiv)
FN(unsigned (*)(void), glCreateProgram)
FN(void (*)(unsigned, unsigned), glAttachShader)
FN(void (*)(unsigned), glLinkProgram)
FN(void (*)(unsigned, unsigned, int *), glGetProgramiv)

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* The shape SPIRV-Cross emits for a flattened uniform array: the uint index is scaled by an int
 * literal. Both index forms seen in Roblox's shaders are here. */
static const char uint_index_vertex[] =
    "#version 300 es\n"
    "uniform vec4 CB3[16];\n"
    "in vec4 position;\n"
    "void main() {\n"
    "    uint k = uint(position.w);\n"
    "    vec3 a = CB3[(k & 7u) * 1 + 0].xyz;\n"
    "    vec3 b = CB3[((uint(position.w) >> 1u) & 7u) * 1 + 8].xyz;\n"
    "    gl_Position = vec4(a + b, 1.0) + position;\n"
    "}\n";
/* The same array indexed by an int, which every driver accepts. */
static const char int_index_vertex[] =
    "#version 300 es\n"
    "uniform vec4 CB3[16];\n"
    "in vec4 position;\n"
    "void main() {\n"
    "    int k = int(position.w);\n"
    "    gl_Position = CB3[(k & 7) * 1 + 0] + position;\n"
    "}\n";
/* An index the rewrite must leave alone (the operand is not the whole index), plus a real error. */
static const char broken_vertex[] =
    "#version 300 es\n"
    "uniform vec4 CB3[16];\n"
    "in vec4 position;\n"
    "void main() {\n"
    "    uint k = uint(position.w);\n"
    "    gl_Position = CB3[(k & 7u) * 1 + 0] + undeclared_name;\n"
    "}\n";
static const char fragment_source[] =
    "#version 300 es\n"
    "precision mediump float;\n"
    "out vec4 color;\n"
    "void main() { color = vec4(1.0); }\n";

static int try_compile(unsigned shader, const char *source) {
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    int ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    return ok;
}

static int links_with_fragment(unsigned vertex) {
    unsigned fragment = glCreateShader(GL_FRAGMENT_SHADER);
    if (!try_compile(fragment, fragment_source)) return 0;
    unsigned program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    int ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    return ok;
}

int main(void) {
    uint64_t gles = bifrost_dlopen("libGLESv2.so.2", 1);
    if (!gles) gles = bifrost_dlopen("libGLESv2.so", 1);
    uint64_t egl = bifrost_dlopen("libEGL.so.1", 1);
    if (!egl) egl = bifrost_dlopen("libEGL.so", 1);
    if (!gles || !egl) { printf("SKIP: GL thunks unavailable\n"); return 77; }
    LOAD(egl, eglGetDisplay) LOAD(egl, eglInitialize) LOAD(egl, eglBindAPI)
    LOAD(egl, eglChooseConfig) LOAD(egl, eglCreatePbufferSurface) LOAD(egl, eglCreateContext)
    LOAD(egl, eglMakeCurrent)
    LOAD(gles, glCreateShader) LOAD(gles, glShaderSource) LOAD(gles, glCompileShader)
    LOAD(gles, glGetShaderiv) LOAD(gles, glCreateProgram) LOAD(gles, glAttachShader)
    LOAD(gles, glLinkProgram) LOAD(gles, glGetProgramiv)

    int major, minor, configs = 0;
    void *display = eglGetDisplay(0), *config = NULL;
    if (!display || !eglInitialize(display, &major, &minor)) { printf("SKIP: no EGL display\n"); return 77; }
    const int config_attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
    eglBindAPI(EGL_OPENGL_ES_API);
    if (!eglChooseConfig(display, config_attrs, &config, 1, &configs) || configs < 1) {
        printf("SKIP: no GLES3 pbuffer config\n"); return 77;
    }
    const int surface_attrs[] = {EGL_WIDTH, 4, EGL_HEIGHT, 4, EGL_NONE};
    const int context_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    void *surface = eglCreatePbufferSurface(display, config, surface_attrs);
    void *context = eglCreateContext(display, config, NULL, context_attrs);
    if (!surface || !context || !eglMakeCurrent(display, surface, surface, context)) {
        printf("SKIP: no GLES3 context\n"); return 77;
    }

    unsigned vertex = glCreateShader(GL_VERTEX_SHADER);
    CHECK(try_compile(vertex, uint_index_vertex), "a uint array index (the SPIRV-Cross form) does not compile");
    CHECK(links_with_fragment(vertex), "the shader that needed the retry does not link with a 300 es fragment shader");

    /* The same shader object sourced and compiled again, as an app does after a failed attempt. */
    CHECK(try_compile(vertex, uint_index_vertex), "compiling the same shader object again fails");

    unsigned control = glCreateShader(GL_VERTEX_SHADER);
    CHECK(try_compile(control, int_index_vertex), "an int array index does not compile");
    CHECK(links_with_fragment(control), "the int-index shader does not link");

    /* A genuine error still fails: the retry must not turn a broken shader into a working one. */
    unsigned broken = glCreateShader(GL_VERTEX_SHADER);
    CHECK(!try_compile(broken, broken_vertex), "a shader with an undeclared name compiled");

    printf(failures ? "gles_uint_index: %d FAILED\n" : "gles_uint_index: ALL PASS\n", failures);
    return failures != 0;
}
