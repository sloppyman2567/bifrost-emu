// test_fork_threads.c — fork() while guest threads are alive.
//
// Regression: the fork child used to inherit joinable std::thread objects
// for host threads that do not exist in the child, so the child could
// deadlock, throw, or std::terminate at exit. The child must run a clean
// single-threaded image, exit with the expected code, and the parent must
// still be able to join its workers afterwards.
//
// Build: make setup-tests
// Run:   ./bifrost-emu Tests/Unit/test_fork_threads.elf
// Exit 0 + "ALL PASS" = pass.
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>

// Exercise the exact SDK vfork flags. A working implementation lets the
// child exit its own process group and leaves the parent alive to wait.
// Bifrost may explicitly reject this unsupported process mode with ENOSYS;
// it must never create a thread whose exit_group terminates the parent.
extern long vfork_exit_child(const uint64_t *args, long syscall_number);
__asm__(
    ".text\n"
    ".global vfork_exit_child\n"
    ".type vfork_exit_child,%function\n"
    "vfork_exit_child:\n"
    "mov x8, x1\n"
    "mov x1, #88\n" // clone3 struct size
    "cmp x8, #220\n"
    "b.ne 2f\n"
    "mov x0, #0x4111\n" // CLONE_VM | CLONE_VFORK | SIGCHLD
    "mov x1, #0\n"
    "mov x2, #0\n"
    "mov x3, #0\n"
    "mov x4, #0\n"
    "2: svc #0\n"
    "cbnz x0, 1f\n"
    "mov x0, #7\n"
    "mov x8, #94\n" // exit_group must affect only the child
    "svc #0\n"
    "brk #0\n"
    "1: ret\n"
    ".size vfork_exit_child, .-vfork_exit_child\n");

static int test_vfork_exit(void) {
    // clone3 stores the exit signal separately from flags.
    const uint64_t args[11] = {0x4100, 0, 0, 0, 17};
    const long numbers[] = {220, 435};
    for (unsigned i = 0; i < sizeof(numbers) / sizeof(numbers[0]); ++i) {
        long child = vfork_exit_child(args, numbers[i]);
        if (child == -ENOSYS) {
            printf("vfork syscall %ld: explicitly unsupported\n", numbers[i]);
            continue;
        }
        if (child < 0) return -1;
        int status = 0;
        if (waitpid((pid_t)child, &status, 0) != child) return -2;
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 7) return -3;
        printf("vfork syscall %ld: isolated child exit\n", numbers[i]);
    }
    return 0;
}

// The child shares its parent's stack, so it must not call C or push a
// frame. Read the inherited stack, record SP, then exit directly.
extern long zero_stack_clone(uint64_t *record, int *child_tid);
__asm__(
    ".text\n"
    ".global zero_stack_clone\n"
    ".type zero_stack_clone,%function\n"
    "zero_stack_clone:\n"
    "mov x6, x0\n"
    "mov x4, x1\n"
    "mov x7, sp\n"
    "str x7, [x6, #16]\n"
    "mov x0, #0x0f00\n" // CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND
    "movk x0, #0x0025, lsl #16\n" // CLONE_THREAD | CLONE_SYSVSEM | CLONE_CHILD_CLEARTID
    "mov x1, #0\n"
    "mov x2, #0\n"
    "mov x3, #0\n"
    "mov x8, #220\n"
    "svc #0\n"
    "cbnz x0, 1f\n"
    "mov x5, sp\n"
    "ldr x9, [sp]\n"
    "stp x5, x9, [x6]\n"
    "mov x0, #0\n"
    "mov x8, #93\n"
    "svc #0\n"
    "brk #0\n"
    "1: ret\n"
    ".size zero_stack_clone, .-zero_stack_clone\n");

static int test_zero_stack_clone(void) {
    static uint64_t record[3];
    static int child_tid;
    for (int round = 0; round < 10; ++round) {
        record[0] = 0;
        __atomic_store_n(&child_tid, 1, __ATOMIC_RELEASE);
        if (zero_stack_clone(record, &child_tid) < 0) return -1;
        int attempts = 0;
        while (__atomic_load_n(&child_tid, __ATOMIC_ACQUIRE)) {
            if (++attempts > 10000) return -2;
            usleep(1000);
        }
        if (!record[0] || record[0] != record[2]) return -3;
    }
    return 0;
}

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
    int r = test_vfork_exit();
    if (r != 0) { printf("FAIL vfork r=%d\n", r); return 1; }
    r = test_zero_stack_clone();
    printf("clone(stack=0): %s\n", r == 0 ? "ok" : "bad");
    if (r != 0) { printf("FAIL r=%d\n", r); return 1; }
    r = test_fork_with_threads();
    printf("fork(3 rounds, %d live threads): %s\n", NW, r == 0 ? "ok" : "bad");
    if (r != 0) { printf("FAIL r=%d\n", r); return 1; }
    printf("fork threads: ALL PASS\n");
    return 0;
}
