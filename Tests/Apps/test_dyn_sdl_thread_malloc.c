/* test_dyn_sdl_thread_malloc.c — malloc from the main thread and an SDL_CreateThread thread at once.
 *
 * The emulator creates SDL threads itself, so it must clear glibc's __libc_single_threaded as
 * pthread_create does. Under --guest-ld the libc is the one the guest ld.so mapped; when the flag
 * stayed set, malloc skipped its arena locks and two threads corrupted the heap (neverball died
 * with "malloc(): unsorted double linked list corrupted").
 *
 * Build: aarch64-linux-gnu-gcc -O2 -o test_dyn_sdl_thread_malloc.elf test_dyn_sdl_thread_malloc.c -ldl -pthread
 * Run:   ./bifrost-emu [--guest-ld] test_dyn_sdl_thread_malloc.elf
 * Pass:  prints "test_dyn_sdl_thread_malloc: ALL PASS". Exit 77 = skip when the SDL thunk is unavailable.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile int bad;

static void churn(const char *who) {
    unsigned char *p[256];
    size_t n[256];
    unsigned seed = (unsigned)(size_t)who;
    for (int round = 0; round < 3000 && !bad; round++) {
        for (int i = 0; i < 256; i++) {
            seed = seed * 1103515245 + 12345;
            n[i] = 8 + (seed >> 8) % 900;
            p[i] = malloc(n[i]);
            memset(p[i], i | 1, n[i]);
        }
        for (int i = 0; i < 256; i++) {
            for (size_t k = 0; k < n[i]; k += 7)
                if (p[i][k] != (unsigned char)(i | 1)) { bad = 1; printf("CORRUPT in %s, round %d\n", who, round); break; }
            free(p[i]);
        }
    }
}

static int sdl_thread(void *arg) {
    (void)arg;
    churn("sdl-thread");
    return 0;
}

int main(void) {
    void *sdl = dlopen("libSDL2-2.0.so.0", RTLD_NOW | RTLD_GLOBAL);
    void *(*create)(int (*)(void *), const char *, void *) = sdl ? dlsym(sdl, "SDL_CreateThread") : 0;
    void (*wait)(void *, int *) = sdl ? dlsym(sdl, "SDL_WaitThread") : 0;
    if (!create || !wait) { printf("test_dyn_sdl_thread_malloc: SDL thunk unavailable, skipping\n"); return 77; }
    void *thread = create(sdl_thread, "churn", 0);
    int status = 0;
    churn("main");
    wait(thread, &status);
    printf(bad ? "test_dyn_sdl_thread_malloc: FAIL\n" : "test_dyn_sdl_thread_malloc: ALL PASS\n");
    return bad;
}
