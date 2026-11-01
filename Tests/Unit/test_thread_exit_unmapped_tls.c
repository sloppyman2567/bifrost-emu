// test_thread_exit_unmapped_tls.c — a thread that unmaps its own stack and
// TLS block and then calls exit(2), as Bionic's detached-thread
// exit_with_stack_teardown does, must not write its exit-time tid words
// afterwards. The next thread reuses the freed hole, so a late store zeroes
// that thread's TCB (here a canary at TPIDR_EL0 - 0xa0, the word the musl
// layout exit path clears) or faults the emulator (SEGV_ACCERR) when the
// range is mid-remap. Threads are raw clones without CLONE_CHILD_CLEARTID so
// only the emulator's own exit path can write there. A last phase checks an
// ordinary exit: it clears clear_child_tid (mm_release) and nothing else in the
// TLS block. That word is musl's detach_state, so writing it woke pthread_join
// early, and the joiner freed the stack and TLS the thread still had to write
// to. Each child also checks that CLONE_PARENT_SETTID was written before it
// ran, as the kernel does. Emits AArch64 svc: check it under QEMU, not natively.
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#define STACK 0x110000UL
#define GUARD 0x10000UL
#define CANARY 0x5a5a5a5au
#define SPAWNS 1500

static atomic_int live, stop;
static atomic_long corrupt, late_tid;

static char* alloc_stack(void) {
    char* p = mmap(NULL, STACK, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return NULL;
    if (mprotect(p + GUARD, STACK - GUARD, PROT_READ | PROT_WRITE)) return NULL;
    return p;
}

struct arg { char* base; char* tls; volatile uint32_t tid; };

static int child(void* a) {
    struct arg* p = a;
    char* base = p->base;
    if (!p->tid) atomic_fetch_add(&late_tid, 1);
    if (*(volatile uint32_t*)(p->tls - 0xa0) != CANARY) atomic_fetch_add(&corrupt, 1);
    atomic_fetch_sub(&live, 1);
    register long x0 __asm__("x0") = (long)base;
    register long x1 __asm__("x1") = (long)STACK;
    register long x8 __asm__("x8") = 215;  // munmap
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    register long y0 __asm__("x0") = 0;
    register long y8 __asm__("x8") = 93;   // exit
    __asm__ volatile("svc #0" : : "r"(y0), "r"(y8) : "memory");
    __builtin_unreachable();
}

// musl 1.2.5's clone() refuses CLONE_THREAD and CLONE_SETTLS without making the syscall. The child
// starts on `stack` with the other registers copied, so x9 and x10 carry its entry and argument.
static long raw_clone(int (*fn)(void*), void* stack, long flags, void* arg, void* parent_tid, void* tls, void* child_tid) {
    register long x0 __asm__("x0") = flags;
    register void* x1 __asm__("x1") = stack;
    register void* x2 __asm__("x2") = parent_tid;
    register void* x3 __asm__("x3") = tls;
    register void* x4 __asm__("x4") = child_tid;
    register long x8 __asm__("x8") = 220;  // clone
    register void* x9 __asm__("x9") = fn;
    register void* x10 __asm__("x10") = arg;
    __asm__ volatile("svc #0\n cbnz x0, 1f\n mov x0, x10\n blr x9\n 1:"
                     : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8), "r"(x9), "r"(x10) : "memory");
    return x0;
}

static int exit_now(void* a) {
    (void)a;
    register long x0 __asm__("x0") = 0;
    register long x8 __asm__("x8") = 93;  // exit
    __asm__ volatile("svc #0" : : "r"(x0), "r"(x8) : "memory");
    __builtin_unreachable();
}

// Recycles same-size ranges so the freed hole is claimed from another thread too.
static void* churn(void* a) {
    (void)a;
    while (!atomic_load(&stop)) {
        char* p = alloc_stack();
        if (!p) continue;
        *(volatile char*)(p + GUARD + 64) = 1;
        mprotect(p + GUARD, 0x1000, PROT_NONE);
        munmap(p, STACK);
    }
    return NULL;
}

int main(void) {
    pthread_t t;
    for (int i = 0; i < 2; ++i) {
        pthread_create(&t, NULL, churn, NULL);
        pthread_detach(t);
    }
    for (int i = 0; i < SPAWNS; ++i) {
        while (atomic_load(&live) >= 16) usleep(20);
        char* base = alloc_stack();
        if (!base) { puts("FAIL: stack allocation"); return 2; }
        char* tls = base + STACK - 0x400;
        *(volatile uint32_t*)(tls - 0xa0) = CANARY;
        struct arg* a = (struct arg*)(base + STACK - 0x1000);
        a->base = base;
        a->tls = tls;
        atomic_fetch_add(&live, 1);
        a->tid = 0;
        long flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD |
                     CLONE_SYSVSEM | CLONE_SETTLS | CLONE_PARENT_SETTID;
        if (raw_clone(child, base + STACK - 0x2000, flags, a, (void*)&a->tid, tls, NULL) < 0) {
            puts("FAIL: clone");
            return 3;
        }
    }
    while (atomic_load(&live) > 0) usleep(1000);
    atomic_store(&stop, 1);
    usleep(20000);
    long bad = atomic_load(&corrupt);
    printf("%ld of %d TCB canaries clobbered\n", bad, SPAWNS);
    static atomic_int exiting = 1;
    char* base = alloc_stack();
    if (!base) { puts("FAIL: stack allocation"); return 2; }
    char* tls = base + STACK - 0x400;
    *(volatile uint32_t*)(tls - 0xa0) = CANARY;
    if (raw_clone(exit_now, base + STACK - 0x2000,
                  CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM | CLONE_SETTLS |
                      CLONE_CHILD_CLEARTID,
                  NULL, NULL, tls, &exiting) < 0) {
        puts("FAIL: clone");
        return 3;
    }
    if (atomic_load(&late_tid)) {
        printf("%ld children ran before their parent tid was written\n", (long)atomic_load(&late_tid));
        ++bad;
    }
    while (atomic_load(&exiting)) usleep(100);
    if (*(volatile uint32_t*)(tls - 0xa0) != CANARY) {
        puts("exit wrote to the TLS block");
        ++bad;
    }
    puts(bad ? "FAIL" : "ALL PASS");
    return bad ? 1 : 0;
}
