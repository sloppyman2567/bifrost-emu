// test_thread_exit_unmapped_tls.c — a thread that unmaps its own stack and
// TLS block and then calls exit(2), as Bionic's detached-thread
// exit_with_stack_teardown does, must not write its exit-time tid words
// afterwards. The next thread reuses the freed hole, so a late store zeroes
// that thread's TCB (here a canary at TPIDR_EL0 - 0xa0, the word the musl
// layout exit path clears) or faults the emulator (SEGV_ACCERR) when the
// range is mid-remap. Threads are raw clones without CLONE_CHILD_CLEARTID so
// only the emulator's own exit path can write there. Emits AArch64 svc: check
// it under QEMU, not natively.
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
static atomic_long corrupt;

static char* alloc_stack(void) {
    char* p = mmap(NULL, STACK, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return NULL;
    if (mprotect(p + GUARD, STACK - GUARD, PROT_READ | PROT_WRITE)) return NULL;
    return p;
}

struct arg { char* base; char* tls; };

static int child(void* a) {
    struct arg* p = a;
    char* base = p->base;
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
        int flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD |
                    CLONE_SYSVSEM | CLONE_SETTLS;
        if (clone(child, base + STACK - 0x2000, flags, a, NULL, tls, NULL) < 0) {
            puts("FAIL: clone");
            return 3;
        }
    }
    while (atomic_load(&live) > 0) usleep(1000);
    atomic_store(&stop, 1);
    usleep(20000);
    long bad = atomic_load(&corrupt);
    printf("%ld of %d TCB canaries clobbered\n", bad, SPAWNS);
    puts(bad ? "FAIL" : "ALL PASS");
    return bad ? 1 : 0;
}
