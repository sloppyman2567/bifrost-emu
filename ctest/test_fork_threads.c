// test_fork_threads.c — fork() while guest threads are alive.
//
// Regression: the fork child used to inherit joinable std::thread objects
// for host threads that do not exist in the child, so the child could
// deadlock, throw, or std::terminate at exit. The child must run a clean
// single-threaded image, exit with the expected code, and the parent must
// still be able to join its workers afterwards.
//
// Build: make setup-tests
// Run:   ./bifrost-emu ctest/test_fork_threads.elf
// Exit 0 + "ALL PASS" = pass.
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>

static volatile int g_stop = 0;
static void* spin_fn(void* arg) { (void)arg; while (!g_stop) { } return NULL; }

#define NW 3
static int test_fork_with_threads(void) {
    pthread_t t[NW];
    for (int i = 0; i < NW; i++)
        if (pthread_create(&t[i], NULL, spin_fn, NULL) != 0) return -1;
    usleep(50000);  // let the workers really be running

    for (int round = 0; round < 3; round++) {
        pid_t p = fork();
        if (p < 0) { g_stop = 1; return -2; }
        if (p == 0) _exit(40 + round);
        int st = 0;
        pid_t w = waitpid(p, &st, 0);
        if (w != p) { g_stop = 1; return -3; }
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 40 + round) {
            g_stop = 1;
            return -4;
        }
    }
    g_stop = 1;
    for (int i = 0; i < NW; i++) pthread_join(t[i], NULL);
    return 0;
}

int main(void) {
    printf("=== fork with threads test ===\n");
    int r = test_fork_with_threads();
    printf("fork(3 rounds, %d live threads): %s\n", NW, r == 0 ? "ok" : "bad");
    if (r != 0) { printf("FAIL r=%d\n", r); return 1; }
    printf("fork threads: ALL PASS\n");
    return 0;
}
