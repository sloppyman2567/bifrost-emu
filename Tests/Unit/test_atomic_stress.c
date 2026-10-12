// test_atomic_stress.c — high-contention atomic operations stress test.
//
// Validates that LSE CAS atomics and LL/SC (via mutex) are correct under
// high contention (8 threads). Games use these patterns heavily:
//   - CAS loops for lock-free refcounting, job queues, state machines
//   - Mutex-protected shared state for render/audio threads
//
// This test catches:
//   - Lost updates (CAS or LL/SC race conditions)
//   - Deadlocks (futex wake failures)
//   - Global exclusive monitor correctness (sharded monitor)
//   - DMB/DSB store-to-load ordering across guest threads
//
// Build: make cross SRC=Tests/Unit/test_atomic_stress.c OUT=Tests/Unit/test_atomic_stress.elf
// Run:   ./bifrost-emu Tests/Unit/test_atomic_stress.elf
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define NTHREADS 8

// ── Test 1: CAS atomic counter (no mutex) ──
#define CAS_ITERS 3000
static volatile int64_t cas_counter = 0;

static void* cas_worker(void* arg) {
    (void)arg;
    for (int i = 0; i < CAS_ITERS; i++) {
        int64_t old, new;
        do {
            old = cas_counter;
            new = old + 1;
        } while (!__sync_bool_compare_and_swap(&cas_counter, old, new));
    }
    return NULL;
}

// ── Test 2: Mutex-protected counter (LL/SC path) ──
#define MUTEX_ITERS 3000
static long mutex_counter = 0;
static pthread_mutex_t mutex_lock = PTHREAD_MUTEX_INITIALIZER;

static void* mutex_worker(void* arg) {
    (void)arg;
    for (int i = 0; i < MUTEX_ITERS; i++) {
        pthread_mutex_lock(&mutex_lock);
        mutex_counter++;
        pthread_mutex_unlock(&mutex_lock);
    }
    return NULL;
}

// ── Test 3: __atomic_add (LDADD instruction) ──
#define ADD_ITERS 3000
static volatile int64_t add_counter = 0;

static void* add_worker(void* arg) {
    (void)arg;
    for (int i = 0; i < ADD_ITERS; i++) {
        __atomic_add_fetch(&add_counter, 1, __ATOMIC_RELAXED);
    }
    return NULL;
}

// Store buffering: with a full barrier between each store and load, the
// two loads cannot both observe zero. x86 TSO alone still allows that result.
// The release/acquire round handshake keeps the main thread's resets outside
// the tested interval; it does not order the workers' accesses to each other.
#define BARRIER_ITERS 100000
static volatile unsigned barrier_x __attribute__((aligned(64)));
static volatile unsigned barrier_y __attribute__((aligned(64)));
static unsigned barrier_go __attribute__((aligned(64)));
static unsigned barrier_done[2] __attribute__((aligned(64)));
static unsigned barrier_result[2];

#define BARRIER_WORKER(name, instruction) \
static void* name(void* arg) { \
    unsigned id = (uintptr_t)arg; \
    volatile unsigned* mine = id ? &barrier_y : &barrier_x; \
    volatile unsigned* other = id ? &barrier_x : &barrier_y; \
    for (unsigned round = 1; round <= BARRIER_ITERS; round++) { \
        while (__atomic_load_n(&barrier_go, __ATOMIC_ACQUIRE) != round) {} \
        unsigned got, one = 1; \
        __asm__ volatile("str %w[one], [%[mine]]\n\t" instruction "\n\t" \
                         "ldr %w[got], [%[other]]" \
            : [got] "=&r"(got) \
            : [one] "r"(one), [mine] "r"(mine), [other] "r"(other) \
            : "memory"); \
        barrier_result[id] = got; \
        __atomic_store_n(&barrier_done[id], round, __ATOMIC_RELEASE); \
    } \
    return NULL; \
}
BARRIER_WORKER(dmb_ish_worker, "dmb ish")
BARRIER_WORKER(dmb_sy_worker, "dmb sy")
BARRIER_WORKER(dsb_ish_worker, "dsb ish")
BARRIER_WORKER(dsb_sy_worker, "dsb sy")
#undef BARRIER_WORKER

static int test_barrier(const char* name, void* (*worker)(void*)) {
    pthread_t threads[2];
    unsigned forbidden = 0;
    barrier_go = barrier_done[0] = barrier_done[1] = 0;
    for (uintptr_t id = 0; id < 2; id++) {
        if (pthread_create(&threads[id], NULL, worker, (void*)id) != 0) {
            fprintf(stderr, "%s: pthread_create failed\n", name);
            // An already-running worker cannot finish without the handshake.
            _Exit(1);
        }
    }
    for (unsigned round = 1; round <= BARRIER_ITERS; round++) {
        barrier_x = barrier_y = 0;
        __atomic_store_n(&barrier_go, round, __ATOMIC_RELEASE);
        while (__atomic_load_n(&barrier_done[0], __ATOMIC_ACQUIRE) != round ||
               __atomic_load_n(&barrier_done[1], __ATOMIC_ACQUIRE) != round) {}
        if (barrier_result[0] == 0 && barrier_result[1] == 0)
            forbidden++;
    }
    for (unsigned id = 0; id < 2; id++)
        pthread_join(threads[id], NULL);
    printf("%s: %u forbidden outcomes / %u rounds  %s\n", name, forbidden,
           BARRIER_ITERS, forbidden == 0 ? "OK" : "FAIL");
    return forbidden == 0;
}

static int test_barrier_state(void) {
    uint64_t flags, value = UINT64_C(0x123456789abcdef0);
    // Also cover the load-only/store-only options and outer/non-shareable
    // domains. None of these instructions may change GPRs or condition flags.
    __asm__ volatile(
        "cmp %x[value], #0\n\t"
        "dmb oshld\n\tdmb oshst\n\tdmb nsh\n\tdmb ish\n\tdmb sy\n\t"
        "dsb oshld\n\tdsb oshst\n\tdsb nsh\n\tdsb ish\n\tdsb sy\n\t"
        "mrs %x[flags], nzcv"
        : [value] "+r"(value), [flags] "=r"(flags) : : "cc", "memory");
    int ok = flags == UINT64_C(0x20000000) &&
             value == UINT64_C(0x123456789abcdef0);
    printf("Barrier GPR/NZCV preservation: %s\n", ok ? "OK" : "FAIL");
    return ok;
}

// Native LSE writes must invalidate an earlier LL/SC attempt. The handshake
// ensures the CAS occurs between LDAXR and STLXR without changing the value
// through any plain guest store on the failing path.
static unsigned value __attribute__((aligned(64)));
static unsigned loaded __attribute__((aligned(64)));
static unsigned changed __attribute__((aligned(64)));
static unsigned status, seen;
static void *mixed_worker(void *unused) {
 (void)unused;
 unsigned old, tmp, one=1;
 __asm__ volatile(
  "ldaxr %w[old], [%[value]]\n\t"
  "stlr %w[one], [%[loaded]]\n\t"
  "1: ldar %w[tmp], [%[changed]]\n\t"
  "cbz %w[tmp], 1b\n\t"
  "stlxr %w[tmp], %w[one], [%[value]]\n\t"
  : [old]"=&r"(old),[tmp]"=&r"(tmp)
  : [one]"r"(one),[value]"r"(&value),[loaded]"r"(&loaded),[changed]"r"(&changed)
  : "memory");
 seen=old; status=tmp;
 return 0;
}
static int test_mixed_exclusive(void) {
 pthread_t t;if (pthread_create(&t,0,mixed_worker,0)) return 0;
 while(!__atomic_load_n(&loaded,__ATOMIC_ACQUIRE)) {}
 unsigned expected=0, replacement=2;
 __asm__ volatile(".arch_extension lse\n\tcasal %w[expected], %w[replacement], [%[value]]"
  : [expected]"+r"(expected):[replacement]"r"(replacement),[value]"r"(&value):"memory");
 __atomic_store_n(&changed,1,__ATOMIC_RELEASE);
 pthread_join(t,0);
 printf("mixed LL/SC + CAS: initial=%u cas-old=%u stxr=%u final=%u\n",seen,expected,status,value);
 return seen==0 && expected==0 && status==1 && value==2;
}

// This block has no ordinary loads/stores to implicitly request the JIT's
// direct-window base. Its native acquire access must initialize that base.
__attribute__((noinline)) static uint64_t acquire_only(uint64_t *p) {
    uint64_t value;
    __asm__ volatile("ldar %x[v], [%[p]]" : [v]"=r"(value) : [p]"r"(p) : "memory");
    return value;
}
static int test_acquire_loads(void) {
    uint64_t source = UINT64_C(0xfedcba9876543210), other = 123;
    uint64_t byte, half, word, full, alias, flags, old, status;
    __asm__ volatile(
        "cmp %x[p], #0\n\t"
        "ldarb %w[b], [%[p]]\n\tldarh %w[h], [%[p]]\n\t"
        "ldar %w[w], [%[p]]\n\tldar %x[x], [%[p]]\n\t"
        "mov %x[a], %x[p]\n\tldar %x[a], [%[a]]\n\t"
        "ldar xzr, [%[p]]\n\tmrs %x[f], nzcv"
        : [b]"=&r"(byte), [h]"=&r"(half), [w]"=&r"(word),
          [x]"=&r"(full), [a]"=&r"(alias), [f]"=r"(flags)
        : [p]"r"(&source) : "cc", "memory");
    int ok = acquire_only(&source) == source &&
        byte == 0x10 && half == 0x3210 && word == 0x76543210 &&
        full == source && alias == source && flags == UINT64_C(0x20000000);
    // A normal acquire load neither creates nor replaces a reservation.
    __asm__ volatile(
        "clrex\n\tldar %x[v], [%[p]]\n\tstlxr %w[s], %x[v], [%[p]]"
        : [v]"=&r"(old), [s]"=&r"(status) : [p]"r"(&source) : "memory");
    ok &= status == 1 && old == source;
    unsigned attempts = 0;
    do {
        __asm__ volatile(
            "ldaxr %x[v], [%[p]]\n\tldar %x[t], [%[q]]\n\t"
            "stlxr %w[s], %x[v], [%[p]]"
            : [v]"=&r"(old), [t]"=&r"(full), [s]"=&r"(status)
            : [p]"r"(&source), [q]"r"(&other) : "memory");
    } while (status && ++attempts < 1000);
    ok &= !status && full == other && old == source;
    printf("Acquire widths, aliases, NZCV and reservations: %s\n", ok ? "OK" : "FAIL");
    return ok;
}

int main(void) {
    printf("=== atomic stress test: %d threads ===\n", NTHREADS);
    int all_ok = 1;

    // Test 1: CAS
    {
        cas_counter = 0;
        pthread_t t[NTHREADS];
        for (int i = 0; i < NTHREADS; i++)
            pthread_create(&t[i], NULL, cas_worker, NULL);
        for (int i = 0; i < NTHREADS; i++)
            pthread_join(t[i], NULL);
        int64_t exp = (int64_t)NTHREADS * CAS_ITERS;
        int ok = (cas_counter == exp);
        printf("CAS:         %lld (exp %lld)  %s\n",
               (long long)cas_counter, (long long)exp, ok ? "OK" : "FAIL");
        all_ok = all_ok && ok;
    }

    // Test 2: Mutex
    {
        mutex_counter = 0;
        pthread_t t[NTHREADS];
        for (int i = 0; i < NTHREADS; i++)
            pthread_create(&t[i], NULL, mutex_worker, NULL);
        for (int i = 0; i < NTHREADS; i++)
            pthread_join(t[i], NULL);
        long exp = (long)NTHREADS * MUTEX_ITERS;
        int ok = (mutex_counter == exp);
        printf("Mutex (LL/SC): %ld (exp %ld)  %s\n",
               mutex_counter, exp, ok ? "OK" : "FAIL");
        all_ok = all_ok && ok;
    }

    // Test 3: __atomic_add (LDADD)
    {
        add_counter = 0;
        pthread_t t[NTHREADS];
        for (int i = 0; i < NTHREADS; i++)
            pthread_create(&t[i], NULL, add_worker, NULL);
        for (int i = 0; i < NTHREADS; i++)
            pthread_join(t[i], NULL);
        int64_t exp = (int64_t)NTHREADS * ADD_ITERS;
        int ok = (add_counter == exp);
        printf("atomic_add:  %lld (exp %lld)  %s\n",
               (long long)add_counter, (long long)exp, ok ? "OK" : "FAIL");
        all_ok = all_ok && ok;
    }

    all_ok &= test_barrier("DMB ISH", dmb_ish_worker);
    all_ok &= test_barrier("DMB SY", dmb_sy_worker);
    all_ok &= test_barrier("DSB ISH", dsb_ish_worker);
    all_ok &= test_barrier("DSB SY", dsb_sy_worker);
    all_ok &= test_barrier_state();
    all_ok &= test_mixed_exclusive();
    all_ok &= test_acquire_loads();

    printf("test_atomic_stress: %s\n", all_ok ? "ALL PASS" : "FAIL");
    return all_ok ? 0 : 1;
}
