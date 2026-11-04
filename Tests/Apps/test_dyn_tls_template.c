/* test_dyn_tls_template.c — a new thread's TLS starts from the module's PT_TLS image.
 *
 * glibc fills each block from the initial image (l_tls_initimage in elf/dl-tls.c). The built-in linker's
 * template for startup modules is the main thread's own TLS, so threads inherited whatever main had
 * stored there: vkQuake's SDL workers got main's mimalloc heap in _mi_heap_default and aborted.
 *
 * Build: aarch64-linux-gnu-gcc -O2 -o test_dyn_tls_template.elf test_dyn_tls_template.c -ldl -pthread
 * Run:   ./bifrost-emu test_dyn_tls_template.elf
 * Pass:  prints "test_dyn_tls_template: ALL PASS". The SDL half is reported as skipped without the thunk.
 */
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>

static int target;
static __thread long tdata_value = 7;
static __thread int *tdata_pointer = &target; /* relocated in a PIE: the image must hold the final value */
static __thread long tbss_value;

static int check(const char *who) {
    int bad = tdata_value != 7 || tdata_pointer != &target || tbss_value != 0;
    if (bad) printf("%s: tdata=%ld pointer=%p (want %p) tbss=%ld\n", who, tdata_value, (void *)tdata_pointer, (void *)&target, tbss_value);
    tdata_value = 11; /* a thread's stores stay its own */
    return bad;
}

static void *pthread_entry(void *arg) { return (void *)(long)check((const char *)arg); }
static int sdl_entry(void *arg) { return check((const char *)arg); }

int main(void) {
    int bad = 0, status = 0;
    void *result = 0;
    pthread_t thread;
    tdata_value = 99;
    tdata_pointer = 0;
    tbss_value = 5;
    if (pthread_create(&thread, 0, pthread_entry, "pthread") != 0 || pthread_join(thread, &result) != 0) bad = 1;
    bad |= result != 0;
    void *sdl = dlopen("libSDL2-2.0.so.0", RTLD_NOW | RTLD_GLOBAL);
    void *(*create)(int (*)(void *), const char *, void *) = sdl ? dlsym(sdl, "SDL_CreateThread") : 0;
    void (*wait)(void *, int *) = sdl ? dlsym(sdl, "SDL_WaitThread") : 0;
    if (create && wait) {
        wait(create(sdl_entry, "template", "SDL thread"), &status);
        bad |= status != 0;
    } else {
        printf("SDL thunk unavailable, SDL thread skipped\n");
    }
    if (tdata_value != 99 || tdata_pointer != 0 || tbss_value != 5) { printf("main's TLS changed\n"); bad = 1; }
    printf(bad ? "test_dyn_tls_template: FAIL\n" : "test_dyn_tls_template: ALL PASS\n");
    return bad;
}
