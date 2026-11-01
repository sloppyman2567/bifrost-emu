/* test_dyn_sdl_thread_malloc.c — an SDL_CreateThread thread: libc TLS, and malloc beside the main thread.
 *
 * The emulator creates SDL threads itself, so it must clear glibc's __libc_single_threaded as
 * pthread_create does. Under --guest-ld the libc is the one the guest ld.so mapped; when the flag
 * stayed set, malloc skipped its arena locks and two threads corrupted the heap (neverball died
 * with "malloc(): unsorted double linked list corrupted").
 *
 * The thread's TLS must also be a real glibc one: under --guest-ld it came from the built-in linker's
 * layout, which lacks the guest libc's TLS images, so locale-dependent calls (strtod, ctype, printf)
 * died with "strtod_l.c: assertion failed: decimal_len > 0".
 *
 * Build: aarch64-linux-gnu-gcc -O2 -o test_dyn_sdl_thread_malloc.elf test_dyn_sdl_thread_malloc.c -ldl -pthread
 * Run:   ./bifrost-emu [--guest-ld] test_dyn_sdl_thread_malloc.elf
 * Pass:  prints "test_dyn_sdl_thread_malloc: ALL PASS". Exit 77 = skip when the SDL thunk is unavailable.
 */
#include <ctype.h>
#include <dlfcn.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile int bad;
static const unsigned short *main_ctype;

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
    volatile int letter = 'q';
    char text[64];
    /* The thread runs glibc's __ctype_init: it follows setlocale even where a new TLS block defaults to "C". */
    if (*__ctype_b_loc() != main_ctype) {
        bad = 1;
        printf("the SDL thread's ctype table is not the process locale's\n");
        return 1;
    }
    snprintf(text, sizeof text, "%.3f|%5d", 3.14159, 42);
    if (!isalpha(letter) || toupper(letter) != 'Q' || strtod("2.5", 0) != 2.5 || strcmp(text, "3.142|   42") != 0) {
        bad = 1;
        printf("TLS-dependent libc calls gave wrong results in the SDL thread\n");
        return 1;
    }
    churn("sdl-thread");
    return 0;
}

int main(void) {
    void *sdl = dlopen("libSDL2-2.0.so.0", RTLD_NOW | RTLD_GLOBAL);
    void *(*create)(int (*)(void *), const char *, void *) = sdl ? dlsym(sdl, "SDL_CreateThread") : 0;
    void (*wait)(void *, int *) = sdl ? dlsym(sdl, "SDL_WaitThread") : 0;
    if (!create || !wait) { printf("test_dyn_sdl_thread_malloc: SDL thunk unavailable, skipping\n"); return 77; }
    setlocale(LC_CTYPE, "C.UTF-8"); /* may fail where the locale is not installed: the default still has to match */
    main_ctype = *__ctype_b_loc();
    void *thread = create(sdl_thread, "churn", 0);
    int status = 0;
    churn("main");
    wait(thread, &status);
    printf(bad ? "test_dyn_sdl_thread_malloc: FAIL\n" : "test_dyn_sdl_thread_malloc: ALL PASS\n");
    return bad;
}
