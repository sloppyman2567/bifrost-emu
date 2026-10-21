/* test_dyn_thunk_stub.c — a guest ld.so loads a thunked library.
 *
 * With --guest-ld, dlopen("libGL.so.1") goes through the rootfs's own ld.so,
 * which finds no such file; the emulator answers with an in-memory stub whose
 * functions branch to the GL thunk trampolines. Headless: state setters are
 * thunked and queries answered from the GL state tracker (see test_gl_state).
 *
 * Build: aarch64-linux-gnu-gcc -O2 -o test_dyn_thunk_stub.elf test_dyn_thunk_stub.c -ldl
 *   (must be built with the glibc toolchain, dynamically linked)
 * Run:   ./bifrost-emu --guest-ld --rootfs rootfs test_dyn_thunk_stub.elf
 * Pass:  exit 0. Exit 77 = skip when the libGL thunk is unavailable.
 */
#include <dlfcn.h>
#include <stdio.h>

#define GL_BLEND 0x0BE2
#define GL_DEPTH_TEST 0x0B71

static int failures;
#define CHECK(cond, what) do { \
    if (!(cond)) { printf("FAIL: %s\n", what); failures++; } \
} while (0)

int main(void) {
    void *gl = dlopen("libGL.so.1", RTLD_NOW);
    if (!gl) {
        printf("test_dyn_thunk_stub: SKIP (%s)\n", dlerror());
        return 77;
    }
    void (*glEnable_)(unsigned) = (void (*)(unsigned))dlsym(gl, "glEnable");
    void (*glDisable_)(unsigned) = (void (*)(unsigned))dlsym(gl, "glDisable");
    unsigned char (*glIsEnabled_)(unsigned) = (unsigned char (*)(unsigned))dlsym(gl, "glIsEnabled");
    CHECK(glEnable_ && glDisable_ && glIsEnabled_, "dlsym of GL entry points");
    if (glEnable_ && glDisable_ && glIsEnabled_) {
        glEnable_(GL_BLEND);
        CHECK(glIsEnabled_(GL_BLEND) == 1, "glIsEnabled after glEnable");
        glDisable_(GL_BLEND);
        CHECK(glIsEnabled_(GL_BLEND) == 0, "glIsEnabled after glDisable");
        glEnable_(GL_DEPTH_TEST);
        CHECK(glIsEnabled_(GL_DEPTH_TEST) == 1, "second state bit");
    }
    CHECK(dlsym(gl, "glNotARealFunction") == NULL, "unknown symbol stays unresolved");
    /* Only thunked libraries are synthesized. */
    CHECK(dlopen("libnotthunked-xyz.so.9", RTLD_NOW) == NULL, "ordinary missing library still fails");
    printf(failures ? "test_dyn_thunk_stub: FAIL\n" : "test_dyn_thunk_stub: ALL PASS\n");
    return failures ? 1 : 0;
}
