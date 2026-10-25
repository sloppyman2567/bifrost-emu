// test_fork_lock_contention.c — fork() and fork()+execve() while other guest threads hammer the emulator's locks.
//
// A host fork() copies the emulator's locks in whatever state other threads hold them. The child, which has only
// the forking thread, then waits forever for a lock nobody can release; a read/write lock also copies its queue
// of waiting writers and hands the lock to one that does not exist. Android SDKs that run a helper command
// (Runtime.exec, popen, posix_spawn) from a busy game hit this: the child never ran and kept a copy of the game.
//
// Six kinds of threads keep the guest memory index, the thread lists, the descriptor table and the futex
// queues busy while the main thread forks. Each child must exit (or exec a program that exits) with status 0
// within a few seconds. The first forks, while the JIT is still compiling, were the ones that hung.
//
// Build: make setup-tests
// Run:   ./bifrost-emu Tests/Unit/test_fork_lock_contention.elf
// Exit 0 + "ALL PASS" = pass.
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { ROUNDS = 100, CHILD_SECONDS = 8, WORKERS = 6 };
static atomic_int stop;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t turn_changed = PTHREAD_COND_INITIALIZER;
static atomic_long turn;

static void *memory_worker(void *arg) {  // guest memory index: mmap, mprotect, munmap
    (void)arg;
    while (!atomic_load(&stop)) {
        void *p = mmap(0, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) continue;
        ((volatile char *)p)[0] = 1;
        mprotect(p, 4096, PROT_READ);
        munmap(p, 65536);
    }
    return 0;
}
static void *short_lived(void *arg) { (void)arg; return 0; }
static void *thread_churn(void *arg) {  // thread lists: create and join
    (void)arg;
    while (!atomic_load(&stop)) {
        pthread_t t;
        if (!pthread_create(&t, 0, short_lived, 0)) pthread_join(t, 0);
    }
    return 0;
}
static void *descriptor_worker(void *arg) {  // descriptor table: open, dup, close
    (void)arg;
    while (!atomic_load(&stop)) {
        int fd = open("/dev/null", O_RDONLY);
        if (fd < 0) continue;
        int copy = dup(fd);
        if (copy >= 0) close(copy);
        close(fd);
    }
    return 0;
}
static void *futex_pingpong(void *arg) {  // futex queues: condition variable wait and broadcast
    long mine = (long)arg;
    while (!atomic_load(&stop)) {
        pthread_mutex_lock(&gate);
        while ((atomic_load(&turn) & 1) != mine && !atomic_load(&stop)) {
            struct timespec t;
            clock_gettime(CLOCK_REALTIME, &t);
            t.tv_nsec += 2000000;
            if (t.tv_nsec >= 1000000000) { t.tv_sec++; t.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&turn_changed, &gate, &t);
        }
        atomic_fetch_add(&turn, 1);
        pthread_cond_broadcast(&turn_changed);
        pthread_mutex_unlock(&gate);
    }
    return 0;
}
static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--child")) return 0;  // the exec target
    pthread_t workers[WORKERS];
    pthread_create(&workers[0], 0, memory_worker, 0);
    pthread_create(&workers[1], 0, memory_worker, 0);
    pthread_create(&workers[2], 0, thread_churn, 0);
    pthread_create(&workers[3], 0, descriptor_worker, 0);
    pthread_create(&workers[4], 0, futex_pingpong, (void *)0);
    pthread_create(&workers[5], 0, futex_pingpong, (void *)1);
    int bad = 0;
    for (int exec_mode = 0; exec_mode < 2; ++exec_mode) {
        const char *kind = exec_mode ? "fork+exec" : "fork";
        for (int i = 0; i < ROUNDS && bad < 3; ++i) {
            pid_t pid = fork();
            if (pid == 0) {
                if (exec_mode) {
                    char *args[] = {argv[0], "--child", 0};
                    execv(argv[0], args);
                    _exit(127);
                }
                _exit(0);
            }
            int status = 0, done = 0;
            for (double deadline = now() + CHILD_SECONDS; now() < deadline;) {
                if (waitpid(pid, &status, WNOHANG) == pid) { done = 1; break; }
                usleep(2000);
            }
            if (!done) {
                printf("FAIL: %s %d: child %d did not exit within %d s\n", kind, i, (int)pid, CHILD_SECONDS);
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                ++bad;
            } else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                printf("FAIL: %s %d: child status 0x%x\n", kind, i, status);
                ++bad;
            }
        }
    }
    atomic_store(&stop, 1);
    for (int i = 0; i < WORKERS; ++i) pthread_join(workers[i], 0);
    if (bad) return 1;
    puts("test_fork_lock_contention: ALL PASS");
    return 0;
}
