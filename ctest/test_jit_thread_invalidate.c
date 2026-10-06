// test_jit_thread_invalidate.c — threads keep running JIT code while the
// main thread replaces an executable page they have called (MAP_FIXED,
// write, mprotect RX), which invalidates their translated blocks from
// another thread.
//
// With BIFROST_NO_SHARED_JIT=1 every thread had its own W^X JIT; the
// cross-thread invalidation flipped a running thread's whole code buffer
// to RW and the emulator died with a host SIGSEGV (SEGV_ACCERR) on its next
// instruction fetch. scripts/run_tests.sh runs this in both JIT modes.
//
// Two pages alternate: the writer only rewrites the page no caller can be
// in (every caller has acknowledged the newer epoch), so the test itself
// never runs a page that is being replaced. Emits AArch64 code: check it
// under QEMU, not natively.
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define ROUNDS 400
#define CALLERS 2

typedef int (*fn_t)(void);

static uint8_t* pages[2];
static _Atomic(fn_t) current;
static atomic_int epoch;             // value returned by `current`
static atomic_int seen[CALLERS];     // epoch each caller last started with
static atomic_int done;
static atomic_long calls, bad;

static void emit(uint8_t* page, int value) {
    // mov w0, #value ; ret
    uint32_t code[2] = {0x52800000u | ((uint32_t)value << 5), 0xd65f03c0u};
    void* p = mmap(page, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p != page) { perror("mmap fixed"); _exit(2); }
    memcpy(page, code, sizeof code);
    __builtin___clear_cache((char*)page, (char*)page + sizeof code);
    if (mprotect(page, 4096, PROT_READ | PROT_EXEC) != 0) { perror("mprotect"); _exit(2); }
}

static void* caller(void* arg) {
    int id = (int)(long)arg;
    unsigned long sum = 0;
    while (!atomic_load(&done)) {
        int e = atomic_load(&epoch);
        atomic_store(&seen[id], e);
        fn_t f = atomic_load(&current);
        int v = f();
        // The page we called returns its epoch (or the next one, if the
        // writer published between our two loads).
        if (v != e && v != e + 1) atomic_fetch_add(&bad, 1);
        for (int i = 0; i < 64; ++i) sum += (sum >> 3) ^ (unsigned long)i;
        atomic_fetch_add(&calls, 1);
    }
    return (void*)sum;
}

int main(void) {
    for (int i = 0; i < 2; ++i) {
        pages[i] = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (pages[i] == MAP_FAILED) { perror("mmap"); return 2; }
    }
    emit(pages[1], 1);
    atomic_store(&epoch, 1);
    atomic_store(&current, (fn_t)pages[1]);
    pthread_t t[CALLERS];
    for (long i = 0; i < CALLERS; ++i) {
        atomic_store(&seen[i], 1);
        pthread_create(&t[i], NULL, caller, (void*)i);
    }
    for (int r = 2; r <= ROUNDS; ++r) {
        // pages[r & 1] last served epoch r-2: wait until every caller has
        // started a call at epoch r-1, so none can still be inside it.
        for (int i = 0; i < CALLERS; ++i)
            while (atomic_load(&seen[i]) < r - 1) usleep(50);
        emit(pages[r & 1], r);
        atomic_store(&current, (fn_t)pages[r & 1]);
        atomic_store(&epoch, r);
    }
    atomic_store(&done, 1);
    for (int i = 0; i < CALLERS; ++i) pthread_join(t[i], NULL);
    printf("calls=%ld wrong=%ld\n", atomic_load(&calls), atomic_load(&bad));
    if (atomic_load(&bad) || atomic_load(&calls) < ROUNDS) {
        printf("FAILED\n");
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
