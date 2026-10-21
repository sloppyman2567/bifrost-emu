/* test_dyn_sdl_exit.c — process exit with an SDL thread blocked in a host call.
 *
 * A guest thread created with SDL_CreateThread can sit inside a host SDL wait
 * that no other guest thread will ever end. The process must still exit:
 *   cond   - the thread is in SDL_CondWait; shutdown wakes SDL conditions.
 *   mutex  - the thread is in SDL_LockMutex on a mutex the main thread holds;
 *            nothing can wake it, so shutdown abandons it after a bounded wait.
 * Before the fix both hung forever in Emulator::stop_sdl_threads().
 *
 * Build: aarch64-linux-gnu-gcc -O2 -o test_dyn_sdl_exit.elf test_dyn_sdl_exit.c -ldl
 * Run:   ./bifrost-emu --rootfs rootfs test_dyn_sdl_exit.elf cond|mutex
 * Pass:  prints "ALL PASS" and exits 0 within the runner's timeout.
 * Exit 77 = skip when the SDL thunk is unavailable.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef void *(*create_fn)(void);
typedef int (*lock_fn)(void *);
typedef int (*wait_fn)(void *, void *);
typedef void *(*thread_fn)(int (*)(void *), const char *, void *);

static void *mutex, *cond;
static lock_fn lock_;
static wait_fn wait_;
static int use_cond;

static int worker(void *arg) {
    (void)arg;
    if (use_cond) {
        lock_(mutex);
        for (;;) wait_(cond, mutex);   /* never signalled */
    }
    lock_(mutex);                      /* main holds it */
    return 0;
}

int main(int argc, char **argv) {
    use_cond = argc > 1 && !strcmp(argv[1], "cond");
    void *sdl = dlopen("libSDL2-2.0.so.0", RTLD_NOW);
    if (!sdl) { printf("test_dyn_sdl_exit: SKIP (%s)\n", dlerror()); return 77; }
    create_fn create_mutex = (create_fn)dlsym(sdl, "SDL_CreateMutex");
    create_fn create_cond = (create_fn)dlsym(sdl, "SDL_CreateCond");
    lock_ = (lock_fn)dlsym(sdl, "SDL_LockMutex");
    wait_ = (wait_fn)dlsym(sdl, "SDL_CondWait");
    thread_fn create_thread = (thread_fn)dlsym(sdl, "SDL_CreateThread");
    if (!create_mutex || !create_cond || !lock_ || !wait_ || !create_thread) {
        printf("test_dyn_sdl_exit: FAIL (dlsym)\n");
        return 1;
    }
    mutex = create_mutex();
    cond = create_cond();
    if (!mutex || !cond) { printf("test_dyn_sdl_exit: SKIP (no host SDL)\n"); return 77; }
    if (!use_cond) lock_(mutex);       /* held until exit */
    if (!create_thread(worker, "blocked", 0)) {
        printf("test_dyn_sdl_exit: SKIP (no SDL thread support)\n");
        return 77;
    }
    usleep(300000);                    /* let the worker block */
    printf("test_dyn_sdl_exit: %s ALL PASS\n", use_cond ? "cond" : "mutex");
    fflush(stdout);
    return 0;                          /* exit with the worker still blocked */
}
