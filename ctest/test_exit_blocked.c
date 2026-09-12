// test_exit_blocked.c — exit_group must not hang on siblings parked in a
// host syscall.
//
// Regression: exit_group used to join every guest thread. A thread blocked
// in a host read()/nanosleep() never observed cpu.running=false, so the
// join (and therefore process exit) hung forever. The fix kicks each vCPU
// with an internal host signal to interrupt the blocking call.
//
// Build: make setup-tests
// Run:   ./bifrost-emu ctest/test_exit_blocked.elf   (must exit 0 promptly)
#define _GNU_SOURCE
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int g_pipe[2];

static void* read_blocked(void* a) {
    (void)a;
    char c;
    (void)read(g_pipe[0], &c, 1);   // blocks (write end stays open in main)
    return NULL;
}

static void* sleep_blocked(void* a) {
    (void)a;
    struct timespec ts = {1000, 0}; // 1000 seconds
    nanosleep(&ts, NULL);           // blocks
    return NULL;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== exit_group with blocked siblings ===\n");
    if (pipe(g_pipe) != 0) { printf("FAIL pipe\n"); return 1; }

    pthread_t t1, t2;
    if (pthread_create(&t1, NULL, read_blocked, NULL) != 0) {
        printf("FAIL create read_blocked\n");
        return 1;
    }
    if (pthread_create(&t2, NULL, sleep_blocked, NULL) != 0) {
        printf("FAIL create sleep_blocked\n");
        return 1;
    }
    usleep(200000);  // let both block in their host syscalls
    printf("siblings blocked; calling _exit(0)\n");
    _exit(0);        // triggers exit_group
}
