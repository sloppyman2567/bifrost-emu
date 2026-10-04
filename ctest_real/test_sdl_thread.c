/* test_sdl_thread.c — SDL_CreateThread / SDL_WaitThread / SDL_DetachThread
 * for the GraphicThunk SDL thread lifecycle.
 *
 * Resolves thunked libSDL2 via bifrost internal syscalls 0x1002/0x1003
 * (static musl binary; musl's dlopen is a stub).
 *
 * Covers the 1.5.5-alpha SDL lifecycle rework:
 *   - create/wait returns the thread function's int (status out-param)
 *   - many concurrent create/wait pairs (join + guest-resource free)
 *   - detach (no waiter): the record must be reaped, not leaked, and the
 *     guest stack must stay valid while the detached thread runs
 *
 * Build:
 *   make cross SRC=ctest_real/test_sdl_thread.c OUT=ctest_real/test_sdl_thread.elf
 * Run (needs host SDL2 + display):
 *   ./bifrost-emu ctest_real/test_sdl_thread.elf
 * Headless: exits 77 (skip).
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <setjmp.h>

#define SDL_INIT_VIDEO 0x00000020u

typedef void* (*SDL_CreateThread_t)(int (*)(void*), const char*, void*);
typedef void  (*SDL_WaitThread_t)(void*, int*);
typedef void  (*SDL_DetachThread_t)(void*);
typedef int   (*SDL_Init_t)(uint32_t);
typedef void  (*SDL_Quit_t)(void);
typedef void  (*SDL_Delay_t)(uint32_t);
typedef const char* (*SDL_GetError_t)(void);

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

/* ── thread functions ─────────────────────────────────────────────── */
static unsigned long (*current_thread_id)(void);
static int worker_sum(void* data) {
    int* out = (int*)data;
    int s = 0;
    for (int i = 0; i < 1000; i++) s += i;   /* 499500 */
    if (out) *out = s;
    return current_thread_id ? (int)current_thread_id() : 7;
}
static int worker_id(void* data) {
    return (int)(uintptr_t)data;             /* return the data value */
}
static int worker_detached(void* data) {
    int* out = (int*)data;
    /* Stay alive a while so the record is genuinely in-flight when the
       main thread moves on; the detached reaper must keep its stack valid. */
    for (int i = 0; i < 50; i++) {
        usleep(1000);
        if (out) (*out)++;
    }
    return 99;
}

__attribute__((noinline)) static void jump_from_nested(jmp_buf env) {
    longjmp(env, 37);
}
__attribute__((noinline)) static int worker_nonlocal(void* data) {
    jmp_buf env;
    int value = setjmp(env);
    if (!value) jump_from_nested(env);
    *(int*)data = value;
    return value + 5;
}

int main(void) {
    printf("test_sdl_thread: start\n");

    uint64_t hsdl = bifrost_dlopen("libSDL2-2.0.so.0", 1);
    if (!hsdl) hsdl = bifrost_dlopen("libSDL2.so.0", 1);
    if (!hsdl) hsdl = bifrost_dlopen("libSDL2.so", 1);
    if (!hsdl) { printf("test_sdl_thread: SKIP (libSDL2 thunk unavailable)\n"); return 77; }

    SDL_CreateThread_t SDL_CreateThread;
    SDL_WaitThread_t   SDL_WaitThread;
    SDL_DetachThread_t SDL_DetachThread;
    SDL_Init_t         SDL_Init;
    SDL_Quit_t         SDL_Quit;
    SDL_Delay_t        SDL_Delay;
    SDL_GetError_t     SDL_GetError;
    LOAD(hsdl, SDL_CreateThread_t, SDL_CreateThread);
    LOAD(hsdl, SDL_WaitThread_t,   SDL_WaitThread);
    LOAD(hsdl, SDL_DetachThread_t, SDL_DetachThread);
    LOAD(hsdl, SDL_Init_t,         SDL_Init);
    LOAD(hsdl, SDL_Quit_t,         SDL_Quit);
    LOAD(hsdl, SDL_Delay_t,        SDL_Delay);
    LOAD(hsdl, SDL_GetError_t,     SDL_GetError);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        const char* e = SDL_GetError();
        printf("test_sdl_thread: SKIP (SDL_Init failed: %s)\n", e ? e : "?");
        return 77;
    }

    current_thread_id=(void*)(uintptr_t)bifrost_dlsym(hsdl,"SDL_ThreadID");
    unsigned long (*get_thread_id)(void*)=(void*)(uintptr_t)bifrost_dlsym(hsdl,"SDL_GetThreadID");
    if(!current_thread_id || !get_thread_id || !current_thread_id() || get_thread_id(NULL)!=current_thread_id()) return 1;

    /* ── Test 1: create + wait, status and data round-trip ── */
    int sum = 0, status = -1;
    void* t1 = SDL_CreateThread(worker_sum, "sum", &sum);
    if (!t1) { printf("FAIL: CreateThread returned NULL (%s)\n", SDL_GetError()); SDL_Quit(); return 1; }
    unsigned long worker_tid=get_thread_id(t1);
    if(!worker_tid || worker_tid==current_thread_id()) return 1;
    SDL_WaitThread(t1, &status);
    if ((unsigned long)status != worker_tid) { printf("FAIL: worker thread id mismatch\n"); SDL_Quit(); return 1; }
    if (sum != 499500) { printf("FAIL: sum=%d want 499500\n", sum); SDL_Quit(); return 1; }
    printf("test 1 (create/wait): ok\n");

    /* ── Test 2: 8 concurrent create/wait pairs ── */
    {
        enum { N = 8 };
        void* th[N]; int st[N];
        for (int i = 0; i < N; i++) {
            st[i] = -1;
            th[i] = SDL_CreateThread(worker_id, "id", (void*)(uintptr_t)(i + 100));
            if (!th[i]) { printf("FAIL: CreateThread %d NULL\n", i); SDL_Quit(); return 1; }
        }
        for (int i = 0; i < N; i++) SDL_WaitThread(th[i], &st[i]);
        for (int i = 0; i < N; i++)
            if (st[i] != i + 100) { printf("FAIL: thread %d status=%d want %d\n", i, st[i], i + 100); SDL_Quit(); return 1; }
        printf("test 2 (8 concurrent create/wait): ok\n");
    }

    /* ── Test 3: detach — no waiter; reaper must free + keep stack valid ── */
    {
        enum { N = 4 };
        volatile int counters[N];
        for (int i = 0; i < N; i++) counters[i] = 0;
        for (int i = 0; i < N; i++) {
            void* t = SDL_CreateThread(worker_detached, "det", (void*)&counters[i]);
            if (!t) { printf("FAIL: detach CreateThread %d NULL\n", i); SDL_Quit(); return 1; }
            SDL_DetachThread(t);
        }
        /* Let them run past the point where the old code would have freed
           their stacks out from under them. */
        SDL_Delay(200);
        for (int i = 0; i < N; i++)
            if (counters[i] <= 0) { printf("FAIL: detached %d never ran (%d)\n", i, counters[i]); SDL_Quit(); return 1; }
        printf("test 3 (detach): ok\n");
    }

    /* The nonlocal continuation returns to the SDL sentinel while native
       call helpers still carry their original return PCs. */
    for (int i = 0; i < 16; ++i) {
        int value = 0, result = -1;
        void* t = SDL_CreateThread(worker_nonlocal, "longjmp", &value);
        if (!t) return 1;
        SDL_WaitThread(t, &result);
        if (value != 37 || result != 42) {
            printf("FAIL: nonlocal value=%d result=%d\n", value, result);
            return 1;
        }
    }
    printf("test 4 (nested longjmp/native thread return): ok\n");
    SDL_Quit();
    printf("test_sdl_thread: ALL PASS\n");
    return 0;
}
