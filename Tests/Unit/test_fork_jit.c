// test_fork_jit.c — a fork child keeps the parent's JIT (a Zygote child starts with its parent's translations).
//
// 1. A child runs the parent's warmed loop at the parent's speed: within 4x of the parent's time, where an
//    interpreted child (the old behaviour) is tens of times slower. Under --no-jit both sides are interpreted.
// 2. After the parent went multi-threaded (MT JIT), a child spinning in a chained loop still takes a signal:
//    MT code returns to the dispatcher only when the preemption epoch moves, so the child must restart the ticker.
// 3. The child can start a thread and fork again, and results stay exact.
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, msg) do { if (cond) printf("PASS %s\n", msg); else { printf("FAIL %s\n", msg); failures++; } } while (0)

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static uint64_t __attribute__((noipa)) work(uint64_t rounds) {  // noipa: GCC moved a pure call out of the timing
    uint64_t x = 0x9e3779b97f4a7c15ull, sum = 0;
    for (uint64_t i = 0; i < rounds; i++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        sum += (x & 0xff) ? x % 1000003 : i;
    }
    return sum;
}

// Best of three: one slow run (a page fault, a preempted host thread) must not decide the comparison.
static double timed(uint64_t rounds, uint64_t* result) {
    double best = 1e9;
    for (int k = 0; k < 3; k++) {
        double t0 = now();
        *result = work(rounds);
        double dt = now() - t0;
        if (dt < best) best = dt;
    }
    return best;
}

static volatile sig_atomic_t alarmed;
static void on_alarm(int sig) { (void)sig; alarmed = 1; }
static void* worker(void* arg) { return (void*)(uintptr_t)work((uintptr_t)arg); }

int main(void) {
    const uint64_t rounds = 2000000;
    uint64_t parent_result;
    work(rounds);  // warm: translated (and chained) before the fork
    double parent_time = timed(rounds, &parent_result);

    // 1. Speed and result in a child of a single-threaded parent.
    int fds[2];
    if (pipe(fds) != 0) return 2;
    pid_t pid = fork();
    if (pid == 0) {
        uint64_t result;
        double child_time = timed(rounds, &result);
        double out[2] = {child_time, (double)(result == parent_result)};
        if (write(fds[1], out, sizeof out) != sizeof out) _exit(3);
        _exit(0);
    }
    double got[2] = {0, 0};
    if (read(fds[0], got, sizeof got) != sizeof got) got[1] = 0;
    int status = 0;
    waitpid(pid, &status, 0);
    printf("parent %.4f s, child %.4f s\n", parent_time, got[0]);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0 && got[1] == 1, "the child computes the parent's result");
    CHECK(got[0] <= parent_time * 4 + 0.002, "the child runs at JIT speed (within 4x of the parent)");

    // 2. and 3. After the parent went multi-threaded.
    pthread_t thread;
    void* value;
    pthread_create(&thread, NULL, worker, (void*)(uintptr_t)1000);
    pthread_join(thread, &value);
    pid = fork();
    if (pid == 0) {
        signal(SIGALRM, on_alarm);
        alarm(1);
        uint64_t spins = 0;
        while (!alarmed) spins++;  // only a signal ends it
        pthread_create(&thread, NULL, worker, (void*)(uintptr_t)rounds);
        pthread_join(thread, &value);
        if ((uint64_t)(uintptr_t)value != parent_result) _exit(4);
        pid_t grandchild = fork();
        if (grandchild == 0) _exit(work(rounds) == parent_result ? 0 : 5);
        int inner = 0;
        waitpid(grandchild, &inner, 0);
        _exit(WIFEXITED(inner) ? WEXITSTATUS(inner) : 6);
    }
    status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "an MT parent's child takes a signal in a tight loop, starts a thread and forks again");

    if (failures) { printf("%d FAILED\n", failures); return 1; }
    printf("ALL PASS\n");
    return 0;
}
