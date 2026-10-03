/* Repeated guest/host thunk lifecycle check. Run with
 * scripts/run_thunk_soak.sh [rounds] on an SDL2-capable host. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

typedef struct { int32_t x, y, w, h; } Rect;
typedef struct { int32_t x, y; } Point;

static uint64_t thunk_dlopen(const char *path) {
    register uint64_t x0 __asm__("x0") = (uint64_t)(uintptr_t)path;
    register uint64_t x1 __asm__("x1") = 1;
    register uint64_t x8 __asm__("x8") = 0x1002;
    __asm__ volatile ("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}

static uint64_t thunk_dlsym(uint64_t handle, const char *name) {
    register uint64_t x0 __asm__("x0") = handle;
    register uint64_t x1 __asm__("x1") = (uint64_t)(uintptr_t)name;
    register uint64_t x8 __asm__("x8") = 0x1003;
    __asm__ volatile ("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}

static void *worker(void *arg) {
    __atomic_add_fetch((int *)arg, 1, __ATOMIC_SEQ_CST);
    return NULL;
}

int main(int argc, char **argv) {
    int rounds = argc > 1 ? atoi(argv[1]) : 64;
    if (rounds < 1 || rounds > 10000) return 2;
    uint64_t sdl = thunk_dlopen("libSDL2.so");
    if (!sdl) { fputs("SDL thunk unavailable\n", stderr); return 2; }
    int (*sdl_init)(uint32_t) = (void *)(uintptr_t)thunk_dlsym(sdl, "SDL_Init");
    void (*sdl_quit)(void) = (void *)(uintptr_t)thunk_dlsym(sdl, "SDL_Quit");
    void *(*create_window)(const char *, int, int, int, int, uint32_t) =
        (void *)(uintptr_t)thunk_dlsym(sdl, "SDL_CreateWindow");
    void (*destroy_window)(void *) =
        (void *)(uintptr_t)thunk_dlsym(sdl, "SDL_DestroyWindow");
    int (*render_copy_ex)(void *, void *, const void *, const void *,
                          double, const void *, int) =
        (void *)(uintptr_t)thunk_dlsym(sdl, "SDL_RenderCopyEx");
    float (*sdl_sqrtf)(float) =
        (void *)(uintptr_t)thunk_dlsym(sdl, "SDL_sqrtf");
    double (*sdl_pow)(double, double) =
        (void *)(uintptr_t)thunk_dlsym(sdl, "SDL_pow");
    void (*gamma_ramp)(float, uint16_t *) =
        (void *)(uintptr_t)thunk_dlsym(sdl, "SDL_CalculateGammaRamp");
    void *(*create_renderer)(void *, int, uint32_t) =
        (void *)(uintptr_t)thunk_dlsym(sdl, "SDL_CreateRenderer");
    void (*destroy_renderer)(void *) =
        (void *)(uintptr_t)thunk_dlsym(sdl, "SDL_DestroyRenderer");
    int (*render_set_scale)(void *, float, float) =
        (void *)(uintptr_t)thunk_dlsym(sdl, "SDL_RenderSetScale");
    if (!sdl_init || !sdl_quit || !create_window || !destroy_window ||
        !render_copy_ex || !sdl_sqrtf || !sdl_pow || !gamma_ramp ||
        !create_renderer || !destroy_renderer || !render_set_scale) {
        fputs("required SDL thunk symbol missing\n", stderr);
        return 2;
    }
    if (sdl_sqrtf(9.0f) != 3.0f || sdl_pow(2.0, 5.0) != 32.0)
        return 10;
    uint16_t ramp[256] = {0};
    gamma_ramp(1.0f, ramp);
    if (ramp[255] < 64000 || ramp[128] < 30000) return 11;
    for (int i = 0; i < rounds; ++i) {
        int *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page == MAP_FAILED) return 3;
        if (sdl_init(0x20) != 0) return 4;  /* SDL_INIT_VIDEO */
        void *window = create_window("thunk soak", 0, 0, 16, 16, 0x8);
        if (!window) return 5;              /* SDL_WINDOW_HIDDEN */
        void *renderer = create_renderer(window, -1, 0);
        if (!renderer) return 12;
        if (render_set_scale(renderer, 1.5f, 2.5f) != 0) return 13;
        pthread_t thread;
        if (pthread_create(&thread, NULL, worker, page) != 0) return 6;
        if (pthread_join(thread, NULL) != 0 || page[0] != 1) return 7;
        Rect src = {0, 0, 4, 4}, dst = {2, 2, 8, 8};
        Point center = {2, 2};
        if (render_copy_ex(NULL, NULL, &src, &dst, 37.5, &center, 0) != -1)
            return 8;  /* Must reach host SDL and preserve its error. */
        destroy_renderer(renderer);
        destroy_window(window);
        sdl_quit();
        if (munmap(page, 4096) != 0) return 9;
    }
    printf("thunk soak: %d cycles passed\n", rounds);
    return 0;
}
