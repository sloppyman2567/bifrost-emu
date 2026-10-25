/* Idle guest threads: spawn argv[1] threads that sleep, hold them all alive for a few seconds (so the
 * driver can read the emulator's resident memory), then join. Each vCPU used to cost 1.7 MiB of
 * interpreter decode cache even if it never interpreted an instruction. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static void *idle(void *a) { struct timespec ts = {4, 0}; nanosleep(&ts, NULL); return a; }
int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 1;
    if (n < 0 || n > 1000) return 2;
    pthread_t t[1000];
    for (int i = 0; i < n; i++) if (pthread_create(&t[i], NULL, idle, NULL)) { puts("FAIL: pthread_create"); return 1; }
    struct timespec ts = {3, 0};
    nanosleep(&ts, NULL);
    for (int i = 0; i < n; i++) pthread_join(t[i], NULL);
    printf("test_thread_memory: %d threads joined\n", n);
    return 0;
}
