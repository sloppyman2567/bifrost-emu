/* test_gles_string_arrays.c — GL calls that take an array of C strings.
 *
 * glGetUniformIndices(program, count, const GLchar *const *names, GLuint *indices),
 * glTransformFeedbackVaryings(program, count, const GLchar *const *varyings, mode) and
 * glCreateShaderProgramv(type, count, const GLchar *const *strings) pass an array of string
 * pointers. The thunk translated the array itself but left every entry a guest address, so the
 * host driver ran strlen on it and the process died with SIGSEGV (a WebView ad in Geometry Dash
 * Lite called glGetUniformIndices from Chromium's GPU thread). Each string now reaches the driver
 * as a host pointer. The test links a program, looks uniforms up by name (and a missing name, and
 * more names than the old fixed 64-entry limit), names a transform feedback varying, and creates
 * a separable program from source, checking every answer against the driver's own.
 *
 * Build:
 *   make cross SRC=Tests/Apps/test_gles_string_arrays.c OUT=Tests/Apps/test_gles_string_arrays.elf
 * Run (needs host EGL + GLES 3, GPU access; no window):
 *   ./bifrost-emu Tests/Apps/test_gles_string_arrays.elf
 * Exits 77 (skip) without host EGL/GLES 3.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
#define GL_ACTIVE_UNIFORMS 0x8B86
#define GL_SEPARATE_ATTRIBS 0x8C8D
#define GL_INVALID_INDEX 0xFFFFFFFFu

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
FN(unsigned (*)(void *), eglTerminate)
FN(unsigned (*)(unsigned), glCreateShader)
FN(void (*)(unsigned, int, const char *const *, const int *), glShaderSource)
FN(void (*)(unsigned), glCompileShader)
FN(void (*)(unsigned, unsigned, int *), glGetShaderiv)
FN(unsigned (*)(void), glCreateProgram)
FN(void (*)(unsigned, unsigned), glAttachShader)
FN(void (*)(unsigned), glLinkProgram)
FN(void (*)(unsigned, unsigned, int *), glGetProgramiv)
FN(void (*)(unsigned, int, const char *const *, unsigned *), glGetUniformIndices)
FN(void (*)(unsigned, unsigned, int, int *, int *, unsigned *, char *), glGetActiveUniform)
FN(void (*)(unsigned, int, const char *const *, unsigned), glTransformFeedbackVaryings)
FN(void (*)(unsigned, unsigned, int, int *, int *, unsigned *, char *), glGetTransformFeedbackVarying)
FN(unsigned (*)(void), glGetError)

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char vertex_source[] =
    "#version 300 es\n"
    "uniform vec4 alpha_uniform;\n"
    "uniform vec4 beta_uniform;\n"
    "in vec4 position;\n"
    "out vec4 v_out;\n"
    "void main() { v_out = alpha_uniform + beta_uniform; gl_Position = position + v_out; }\n";
static const char fragment_source[] =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec4 v_out;\n"
    "out vec4 color;\n"
    "void main() { color = v_out; }\n";

static unsigned compile(unsigned type, const char *source) {
    unsigned shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    int ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    return ok ? shader : 0;
}

static int link_program(unsigned program) {
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
    LOAD(egl, eglMakeCurrent) LOAD(egl, eglTerminate)
    LOAD(gles, glCreateShader) LOAD(gles, glShaderSource) LOAD(gles, glCompileShader)
    LOAD(gles, glGetShaderiv) LOAD(gles, glCreateProgram) LOAD(gles, glAttachShader)
    LOAD(gles, glLinkProgram) LOAD(gles, glGetProgramiv) LOAD(gles, glGetUniformIndices)
    LOAD(gles, glGetActiveUniform) LOAD(gles, glTransformFeedbackVaryings)
    LOAD(gles, glGetTransformFeedbackVarying) LOAD(gles, glGetError)

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

    unsigned vertex = compile(GL_VERTEX_SHADER, vertex_source);
    unsigned fragment = compile(GL_FRAGMENT_SHADER, fragment_source);
    CHECK(vertex && fragment, "the test shaders do not compile");
    unsigned program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);

    /* The transform feedback varying names are read at link time. */
    static const char *const varyings[] = {"v_out"};
    glTransformFeedbackVaryings(program, 1, varyings, GL_SEPARATE_ATTRIBS);
    CHECK(glGetError() == 0, "glTransformFeedbackVaryings raised an error");
    CHECK(link_program(program), "the program does not link");
    {
        char name[32] = {0};
        int length = 0, size = 0;
        unsigned type = 0;
        glGetTransformFeedbackVarying(program, 0, sizeof(name), &length, &size, &type, name);
        CHECK(strcmp(name, "v_out") == 0, "transform feedback varying is '%s', want v_out", name);
    }

    /* Names in a different order than the program declares them; one that does not exist. */
    static const char *const names[] = {"beta_uniform", "alpha_uniform", "no_such_uniform"};
    unsigned indices[3] = {7, 7, 7};
    glGetUniformIndices(program, 3, names, indices);
    CHECK(glGetError() == 0, "glGetUniformIndices raised an error");
    CHECK(indices[0] != GL_INVALID_INDEX && indices[1] != GL_INVALID_INDEX, "known uniforms not found");
    CHECK(indices[0] != indices[1], "two uniforms share index %u", indices[0]);
    CHECK(indices[2] == GL_INVALID_INDEX, "a missing uniform has index %u", indices[2]);
    for (int i = 0; i < 2; i++) {
        char name[32] = {0};
        int length = 0, size = 0;
        unsigned type = 0;
        glGetActiveUniform(program, indices[i], sizeof(name), &length, &size, &type, name);
        CHECK(strcmp(name, names[i]) == 0, "index %u is '%s', want '%s'", indices[i], name, names[i]);
    }
    int active = 0;
    glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &active);
    CHECK(active == 2, "program has %d active uniforms, want 2", active);

    /* More names than the old fixed 64-entry limit, the real one buried in the middle. */
    enum { MANY = 200 };
    static const char *many[MANY];
    static unsigned many_indices[MANY];
    static char missing[MANY][24];
    for (int i = 0; i < MANY; i++) {
        snprintf(missing[i], sizeof(missing[i]), "absent_%d", i);
        many[i] = missing[i];
        many_indices[i] = 7;
    }
    many[150] = "alpha_uniform";
    glGetUniformIndices(program, MANY, many, many_indices);
    CHECK(glGetError() == 0, "glGetUniformIndices with many names raised an error");
    int wrong = 0;
    for (int i = 0; i < MANY; i++)
        if (i == 150 ? many_indices[i] == GL_INVALID_INDEX : many_indices[i] != GL_INVALID_INDEX) wrong++;
    CHECK(wrong == 0, "%d of %d entries have the wrong index", wrong, MANY);
    CHECK(many_indices[150] == indices[1], "alpha_uniform is %u here, %u before", many_indices[150], indices[1]);

    /* A separable program from source (GLES 3.1 and later). */
    unsigned (*glCreateShaderProgramv)(unsigned, int, const char *const *) =
        (unsigned (*)(unsigned, int, const char *const *))(uintptr_t)bifrost_dlsym(gles, "glCreateShaderProgramv");
    if (glCreateShaderProgramv) {
        static const char fragment_only[] =
            "#version 310 es\nprecision mediump float;\nout vec4 color;\nvoid main() { color = vec4(1.0); }\n";
        const char *sources[] = {fragment_only};
        while (glGetError()) {}
        unsigned separable = glCreateShaderProgramv(GL_FRAGMENT_SHADER, 1, sources);
        unsigned error = glGetError();
        if (error == 0x0502 /* GL_INVALID_OPERATION: a 3.0 context */) {
            printf("note: glCreateShaderProgramv needs a 3.1 context, skipped\n");
        } else {
            int ok = 0;
            if (separable) glGetProgramiv(separable, GL_LINK_STATUS, &ok);
            CHECK(error == 0 && separable && ok, "glCreateShaderProgramv: error %#x program %u link %d",
                  error, separable, ok);
        }
    }

    eglTerminate(display);
    if (failures) return 1;
    printf("ALL PASS\n");
    return 0;
}
