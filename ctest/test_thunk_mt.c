/* test_thunk_mt.c — concurrent GraphicThunk dispatch smoke test.
 *
 * 4 guest threads hammer the previously-unlocked dispatch paths at once:
 * GetProcAddress (registry scan), glGetString (string-cache ring), and
 * SDL_GetError/SDL_ClearError (string cache + error slot). Before the
 * state_mu hardening these raced (same ring slot handed to two threads,
 * unordered_map R+W). Assertions: well-known proc addresses are stable
 * across threads, and no call crashes or returns garbage.
 *
 * Needs host SDL2/GL for the string paths to return non-null; without
 * them the proc-address stability checks still run (exit 0, noted).
 *
 * Build: make cross SRC=ctest/test_thunk_mt.c OUT=ctest/test_thunk_mt.elf
 * Run:   ./bifrost-emu ctest/test_thunk_mt.elf
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NTHREADS 4
#define ITERS    500

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

static const char* kNames[] = {
    "glClear", "glClearColor", "glViewport", "glGetString",
    "glDrawElements", "glBindTexture", "glUseProgram", "glGetError",
};

static uint64_t hgl, hsdl;
static uint64_t first_seen[8];
static int have_gl_strings = 0;

typedef const unsigned char* (*glGetString_t)(unsigned);
typedef const char* (*SDL_GetError_t)(void);
typedef void (*SDL_ClearError_t)(void);

static void* worker(void* arg) {
    (void)arg;
    glGetString_t glGetString = 0;
    SDL_GetError_t SDL_GetError = 0;
    SDL_ClearError_t SDL_ClearError = 0;
    if (hgl) glGetString = (glGetString_t)(uintptr_t)bifrost_dlsym(hgl, "glGetString");
    if (hsdl) {
        SDL_GetError = (SDL_GetError_t)(uintptr_t)bifrost_dlsym(hsdl, "SDL_GetError");
        SDL_ClearError = (SDL_ClearError_t)(uintptr_t)bifrost_dlsym(hsdl, "SDL_ClearError");
    }
    for (int i = 0; i < ITERS; i++) {
        /* Proc-address stability: same symbol, same trampoline, always. */
        for (int k = 0; k < 8; k++) {
            uint64_t a = hgl ? bifrost_dlsym(hgl, kNames[k]) : 0;
            if (a == 0) continue;
            uint64_t exp = __atomic_load_n(&first_seen[k], __ATOMIC_RELAXED);
            if (exp == 0) {
                __atomic_compare_exchange_n(&first_seen[k], &exp, a,
                                            0, __ATOMIC_RELAXED,
                                            __ATOMIC_RELAXED);
            } else if (exp != a) {
                printf("FAIL: %s unstable: 0x%llx vs 0x%llx\n", kNames[k],
                       (unsigned long long)exp, (unsigned long long)a);
                exit(1);
            }
        }
        /* String-cache paths (ring alloc + free-slot reuse). */
        if (glGetString) {
            const unsigned char* v = glGetString(0x1F00 /* VENDOR */);
            if (v) {
                __atomic_store_n(&have_gl_strings, 1, __ATOMIC_RELAXED);
                /* Must be NUL-terminated inside a sane bound. */
                size_t n = 0;
                while (n < 4096 && v[n]) n++;
                if (n >= 4096) {
                    printf("FAIL: vendor string unterminated\n");
                    exit(1);
                }
            }
        }
        if (SDL_GetError && SDL_ClearError) {
            SDL_ClearError();
            const char* e = SDL_GetError();
            if (e && e[0]) {
                size_t n = 0;
                while (n < 4096 && e[n]) n++;
                if (n >= 4096) {
                    printf("FAIL: sdl error unterminated\n");
                    exit(1);
                }
            }
        }
    }
    return NULL;
}

int main(void) {
    printf("test_thunk_mt: %d threads x %d iters\n", NTHREADS, ITERS);
    hgl = bifrost_dlopen("libGL.so.1", 1);
    if (!hgl) hgl = bifrost_dlopen("libGL.so", 1);
    hsdl = bifrost_dlopen("libSDL2-2.0.so.0", 1);
    if (!hsdl) hsdl = bifrost_dlopen("libSDL2.so.0", 1);
    if (!hsdl) hsdl = bifrost_dlopen("libSDL2.so", 1);
    if (!hgl && !hsdl) {
        printf("test_thunk_mt: SKIP (no thunk libs)\n");
        return 77;
    }
    pthread_t th[NTHREADS];
    for (int i = 0; i < NTHREADS; i++) {
        if (pthread_create(&th[i], NULL, worker, NULL) != 0) {
            printf("FAIL: pthread_create\n");
            return 1;
        }
    }
    for (int i = 0; i < NTHREADS; i++) pthread_join(th[i], NULL);
    printf("test_thunk_mt: stable=%d gl_strings=%d ALL PASS\n",
           1, __atomic_load_n(&have_gl_strings, __ATOMIC_RELAXED));
    return 0;
}
