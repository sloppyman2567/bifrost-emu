// jit_llsc_native.c — native LDXR/LDAXR/STXR/STLXR/STLR sequences and the exclusive monitor behind them.
// 1. Single CPU, hashed and compared with QEMU: every width and acquire/release form, a store-exclusive
//    with no reservation, to another address, after CLREX, after a success, after a plain store of a
//    different value, and after another load-exclusive replaced the reservation.
// 2. Four CPUs, exact results: LL/SC counters of each width, a spinlock built from LDAXR/STXR and STLR, cells
//    that share a cache line, and an ABA-prone LIFO (pop reads next through the head, nodes are recycled):
//    a store-exclusive must fail when another CPU completed a store-exclusive to the same word in between,
//    which a bare value compare cannot see.
// 3. Faults: a store-exclusive to a read-only page, a load-exclusive from a PROT_NONE page and a store-release
//    to a read-only page each raise SIGSEGV once; the handler opens the page and the loop completes.
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int fails;
#define CHECK(c, what, a, b) do { if (!(c)) { ++fails; if (fails < 16) printf("NG %s %llx %llx\n", what, (unsigned long long)(a), (unsigned long long)(b)); } } while (0)

// ── single-register forms ────────────────────────────────────────────────
#define LL(name, insn, T, r) static inline T name(volatile void* p) { T v; \
    __asm__ volatile(insn " %" r "0, [%1]" : "=&r"(v) : "r"(p) : "memory"); return v; }
#define SC(name, insn, T, r) static inline uint32_t name(volatile void* p, T v) { uint32_t s; \
    __asm__ volatile(insn " %w0, %" r "1, [%2]" : "=&r"(s) : "r"(v), "r"(p) : "memory"); return s; }
LL(ldxrb, "ldxrb", uint32_t, "w")   LL(ldxrh, "ldxrh", uint32_t, "w")
LL(ldxrw, "ldxr", uint32_t, "w")    LL(ldxrx, "ldxr", uint64_t, "")
LL(ldaxrb, "ldaxrb", uint32_t, "w") LL(ldaxrh, "ldaxrh", uint32_t, "w")
LL(ldaxrw, "ldaxr", uint32_t, "w")  LL(ldaxrx, "ldaxr", uint64_t, "")
SC(stxrb, "stxrb", uint32_t, "w")   SC(stxrh, "stxrh", uint32_t, "w")
SC(stxrw, "stxr", uint32_t, "w")    SC(stxrx, "stxr", uint64_t, "")
SC(stlxrb, "stlxrb", uint32_t, "w") SC(stlxrh, "stlxrh", uint32_t, "w")
SC(stlxrw, "stlxr", uint32_t, "w")  SC(stlxrx, "stlxr", uint64_t, "")
static inline void clrex(void) { __asm__ volatile("clrex" ::: "memory"); }
static inline void stlrb(volatile void* p, uint32_t v) { __asm__ volatile("stlrb %w0, [%1]" :: "r"(v), "r"(p) : "memory"); }
static inline void stlrh(volatile void* p, uint32_t v) { __asm__ volatile("stlrh %w0, [%1]" :: "r"(v), "r"(p) : "memory"); }
static inline void stlrw(volatile void* p, uint32_t v) { __asm__ volatile("stlr %w0, [%1]" :: "r"(v), "r"(p) : "memory"); }
static inline void stlrx(volatile void* p, uint64_t v) { __asm__ volatile("stlr %1, [%0]" :: "r"(p), "r"(v) : "memory"); }
static inline uint32_t ldarb(volatile void* p) { uint32_t v; __asm__ volatile("ldarb %w0, [%1]" : "=r"(v) : "r"(p) : "memory"); return v; }
static inline uint32_t ldarh(volatile void* p) { uint32_t v; __asm__ volatile("ldarh %w0, [%1]" : "=r"(v) : "r"(p) : "memory"); return v; }
static inline uint32_t ldarw(volatile void* p) { uint32_t v; __asm__ volatile("ldar %w0, [%1]" : "=r"(v) : "r"(p) : "memory"); return v; }
static inline uint64_t ldarx(volatile void* p) { uint64_t v; __asm__ volatile("ldar %0, [%1]" : "=r"(v) : "r"(p) : "memory"); return v; }

typedef struct { int bytes; uint64_t (*ll)(volatile void*); uint32_t (*sc)(volatile void*, uint64_t); } Form;
#define WRAP_LL(n, f) static uint64_t n(volatile void* p) { return f(p); }
#define WRAP_SC(n, f, T) static uint32_t n(volatile void* p, uint64_t v) { return f(p, (T)v); }
WRAP_LL(w_ldxrb, ldxrb) WRAP_LL(w_ldxrh, ldxrh) WRAP_LL(w_ldxrw, ldxrw) WRAP_LL(w_ldxrx, ldxrx)
WRAP_LL(w_ldaxrb, ldaxrb) WRAP_LL(w_ldaxrh, ldaxrh) WRAP_LL(w_ldaxrw, ldaxrw) WRAP_LL(w_ldaxrx, ldaxrx)
WRAP_SC(w_stxrb, stxrb, uint32_t) WRAP_SC(w_stxrh, stxrh, uint32_t) WRAP_SC(w_stxrw, stxrw, uint32_t) WRAP_SC(w_stxrx, stxrx, uint64_t)
WRAP_SC(w_stlxrb, stlxrb, uint32_t) WRAP_SC(w_stlxrh, stlxrh, uint32_t) WRAP_SC(w_stlxrw, stlxrw, uint32_t) WRAP_SC(w_stlxrx, stlxrx, uint64_t)
static const Form forms[] = {
    {1, w_ldxrb, w_stxrb}, {2, w_ldxrh, w_stxrh}, {4, w_ldxrw, w_stxrw}, {8, w_ldxrx, w_stxrx},
    {1, w_ldaxrb, w_stlxrb}, {2, w_ldaxrh, w_stlxrh}, {4, w_ldaxrw, w_stlxrw}, {8, w_ldaxrx, w_stlxrx},
    {1, w_ldxrb, w_stlxrb}, {4, w_ldaxrw, w_stxrw}, {8, w_ldaxrx, w_stxrx}, {2, w_ldxrh, w_stlxrh},
};

static uint64_t h;
static void mix(uint64_t x) { h = (h ^ x) * 1099511628211ull; h ^= h >> 31; }
static uint64_t mem_at(volatile uint8_t* p, int bytes) { uint64_t v = 0; memcpy(&v, (const void*)p, bytes); return v; }

static void single_cpu(void) {
    static volatile uint8_t cells[256] __attribute__((aligned(64)));
    static const uint64_t seeds[] = {0, 1, 0x7f, 0x80, 0xff, 0x8000, 0x7fffffff, 0x80000000ull, 0xffffffffull,
                                     0x123456789abcdef0ull, ~0ull};
    for (unsigned f = 0; f < sizeof forms / sizeof *forms; ++f)
        for (unsigned s = 0; s < sizeof seeds / sizeof *seeds; ++s) {
            const Form* F = &forms[f];
            volatile uint8_t* a = cells + 64 * (f & 1);        // two granules
            volatile uint8_t* b = cells + 128;
            uint64_t v0 = seeds[s], v1 = seeds[(s + 3) % (sizeof seeds / sizeof *seeds)] * 3 + 1;
            memset((void*)cells, 0xA5, sizeof cells);
            memcpy((void*)a, &v0, F->bytes);
            // a. success, then the reservation is gone.
            uint64_t got = F->ll(a);
            uint32_t st = F->sc(a, v1);
            mix(got); mix(st); mix(mem_at(a, F->bytes));
            mix(F->sc(a, v0)); mix(mem_at(a, F->bytes));
            // b. no reservation (CLREX) / reservation for another address.
            clrex(); mix(F->sc(a, v0)); mix(mem_at(a, F->bytes));
            F->ll(b); mix(F->sc(a, v1)); mix(mem_at(a, F->bytes)); mix(mem_at(b, F->bytes));
            // c. the second load-exclusive replaces the first reservation.
            F->ll(a); F->ll(b); mix(F->sc(a, v1)); mix(mem_at(a, F->bytes));
            F->ll(a); F->ll(b); mix(F->sc(b, v1)); mix(mem_at(b, F->bytes));
            // d. a plain store of a different value between the pair.
            got = F->ll(a);
            uint64_t other = got ^ 0x5a;
            memcpy((void*)a, &other, F->bytes);
            mix(F->sc(a, v1)); mix(mem_at(a, F->bytes));
            // e. CLREX between.
            F->ll(a); clrex(); mix(F->sc(a, v1)); mix(mem_at(a, F->bytes));
        }
    // Release store and acquire load of every width.
    for (unsigned s = 0; s < sizeof seeds / sizeof *seeds; ++s) {
        volatile uint8_t* a = cells; memset((void*)cells, 0x5A, 32);
        stlrb(a, (uint32_t)seeds[s]); mix(ldarb(a)); mix(mem_at(a, 4));
        stlrh(a + 2, (uint32_t)seeds[s]); mix(ldarh(a + 2)); mix(mem_at(a, 8));
        stlrw(a + 4, (uint32_t)seeds[s]); mix(ldarw(a + 4)); mix(mem_at(a, 8));
        stlrx(a + 8, seeds[s]); mix(ldarx(a + 8)); mix(mem_at(a + 8, 8));
    }
    // A pair reservation (LDXP/STXP run in the interpreter) and its failures.
    static volatile uint64_t pair[2] __attribute__((aligned(16)));
    for (int i = 0; i < 4; ++i) {
        pair[0] = 11 + i; pair[1] = 22 + i;
        uint64_t x0, x1; uint32_t st1, st2, st3;
        __asm__ volatile("ldxp %0, %1, [%5]\n stxp %w2, %6, %7, [%5]\n stxp %w3, %6, %7, [%5]\n"
                         "clrex\n stxp %w4, %6, %7, [%5]\n"
                         : "=&r"(x0), "=&r"(x1), "=&r"(st1), "=&r"(st2), "=&r"(st3)
                         : "r"(pair), "r"((uint64_t)(100 + i)), "r"((uint64_t)(200 + i)) : "memory");
        mix(x0); mix(x1); mix(st1); mix(st2); mix(st3); mix(pair[0]); mix(pair[1]);
    }
}

// ── contention ───────────────────────────────────────────────────────────
#define THREADS 4
static volatile uint64_t cnt64, cnt32x, cnt16x, cnt8x;
static volatile uint32_t lock_word;
static volatile uint64_t guarded;
static volatile uint64_t lanes[THREADS * 2] __attribute__((aligned(64)));
static volatile uint32_t arrive;
static void barrier(void) {
    __atomic_add_fetch(&arrive, 1, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&arrive, __ATOMIC_SEQ_CST) < THREADS) __asm__ volatile("yield");
}
typedef struct Node { struct Node* next; } Node;
static Node nodes[8];
static volatile uint64_t head;
static volatile uint64_t aba_lost;

static uint64_t pop(void) {
    uint64_t n, next; uint32_t st;
    do {
        __asm__ volatile("ldaxr %0, [%3]\n"
                         "cbz %0, 1f\n"
                         "ldr %1, [%0]\n"
                         "stlxr %w2, %1, [%3]\n"
                         "b 2f\n"
                         "1: clrex\n mov %w2, #0\n 2:"
                         : "=&r"(n), "=&r"(next), "=&r"(st) : "r"(&head) : "memory");
    } while (st);
    return n;
}
static void push(uint64_t n) {
    uint64_t old; uint32_t st;
    do {
        __asm__ volatile("ldxr %0, [%3]\n str %0, [%2]\n stlxr %w1, %2, [%3]"
                         : "=&r"(old), "=&r"(st) : "r"(n), "r"(&head) : "memory");
    } while (st);
}

static void* worker(void* arg) {
    const int id = (int)(intptr_t)arg;
    barrier();
    for (int i = 0; i < 100000; ++i) {
        uint64_t v; uint32_t st;
        do { __asm__ volatile("ldaxr %0, [%2]\n add %0, %0, #1\n stlxr %w1, %0, [%2]" : "=&r"(v), "=&r"(st) : "r"(&cnt64) : "memory"); } while (st);
        uint32_t w;
        do { __asm__ volatile("ldxr %w0, [%2]\n add %w0, %w0, #1\n stxr %w1, %w0, [%2]" : "=&r"(w), "=&r"(st) : "r"(&cnt32x) : "memory"); } while (st);
        do { __asm__ volatile("ldxrh %w0, [%2]\n add %w0, %w0, #1\n stxrh %w1, %w0, [%2]" : "=&r"(w), "=&r"(st) : "r"(&cnt16x) : "memory"); } while (st);
        do { __asm__ volatile("ldaxrb %w0, [%2]\n add %w0, %w0, #1\n stlxrb %w1, %w0, [%2]" : "=&r"(w), "=&r"(st) : "r"(&cnt8x) : "memory"); } while (st);
        // each CPU increments its own cell; the cells share a cache line
        do { __asm__ volatile("ldxr %0, [%2]\n add %0, %0, #1\n stxr %w1, %0, [%2]" : "=&r"(v), "=&r"(st) : "r"(&lanes[id]) : "memory"); } while (st);
        // spinlock: LDAXR/STXR acquire, STLR release; guards a plain increment
        do {
            __asm__ volatile("1: ldaxr %w0, [%2]\n cbnz %w0, 1b\n mov %w0, #1\n stxr %w1, %w0, [%2]"
                             : "=&r"(w), "=&r"(st) : "r"(&lock_word) : "memory");
        } while (st);
        guarded = guarded + 1;
        stlrw(&lock_word, 0);
    }
    // ABA-prone stack: pop one, push it back; with eight nodes a node is popped and pushed again by other CPUs
    // between a pop's load-exclusive and its store-exclusive all the time.
    for (int i = 0; i < 150000; ++i) {
        uint64_t n = pop();
        if (n) push(n);
    }
    return 0;
}

static void contention(void) {
    pthread_t t[THREADS];
    for (int i = 0; i < 8; ++i) push((uint64_t)&nodes[i]);
    for (int i = 0; i < THREADS; ++i) pthread_create(&t[i], 0, worker, (void*)(intptr_t)i);
    for (int i = 0; i < THREADS; ++i) pthread_join(t[i], 0);
    CHECK(cnt64 == 400000, "cnt64", cnt64, 400000);
    CHECK(cnt32x == 400000, "cnt32", cnt32x, 400000);
    CHECK(cnt16x == (uint16_t)400000, "cnt16", cnt16x, (uint16_t)400000);
    CHECK(cnt8x == (uint8_t)400000, "cnt8", cnt8x, (uint8_t)400000);
    CHECK(guarded == 400000, "spinlock-guarded counter", guarded, 400000);
    for (int i = 0; i < THREADS; ++i) CHECK(lanes[i] == 100000, "lane", i, lanes[i]);
    int seen[8] = {0}, total = 0;
    for (uint64_t n; (n = pop()) && total < 16; ++total) {
        long idx = (Node*)n - nodes;
        if (idx >= 0 && idx < 8) ++seen[idx]; else ++aba_lost;
    }
    CHECK(total == 8 && aba_lost == 0, "stack node count", total, aba_lost);
    for (int i = 0; i < 8; ++i) CHECK(seen[i] == 1, "stack node seen once", i, seen[i]);
}

// ── faults ───────────────────────────────────────────────────────────────
static uint8_t* page;
static volatile int segv_count;
static volatile uint64_t segv_addr;
static void on_segv(int sig, siginfo_t* si, void* uc) {
    (void)sig; (void)uc;
    ++segv_count; segv_addr = (uint64_t)si->si_addr;
    mprotect(page, 4096, PROT_READ | PROT_WRITE);
}
static void faults(void) {
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = on_segv; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, 0);
    page = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    // F1: STXR to a read-only page
    *(uint64_t*)(page + 16) = 41;
    mprotect(page, 4096, PROT_READ);
    segv_count = 0;
    uint64_t v; uint32_t st; int tries = 0;
    do { __asm__ volatile("ldxr %0, [%2]\n add %0, %0, #1\n stxr %w1, %0, [%2]" : "=&r"(v), "=&r"(st) : "r"(page + 16) : "memory"); ++tries; } while (st && tries < 10);
    CHECK(*(uint64_t*)(page + 16) == 42 && segv_count == 1 && segv_addr == (uint64_t)(page + 16), "F1 stxr to read-only page", segv_count, *(uint64_t*)(page + 16));
    // F2: LDXR from a PROT_NONE page
    *(uint64_t*)(page + 32) = 7;
    mprotect(page, 4096, PROT_NONE);
    segv_count = 0; tries = 0;
    do { __asm__ volatile("ldaxr %0, [%2]\n add %0, %0, #1\n stlxr %w1, %0, [%2]" : "=&r"(v), "=&r"(st) : "r"(page + 32) : "memory"); ++tries; } while (st && tries < 10);
    CHECK(*(uint64_t*)(page + 32) == 8 && segv_count == 1 && segv_addr == (uint64_t)(page + 32), "F2 ldxr from PROT_NONE page", segv_count, *(uint64_t*)(page + 32));
    // F3: STLR to a read-only page; the instruction restarts and stores once
    mprotect(page, 4096, PROT_READ);
    segv_count = 0;
    stlrw(page + 64, 0x11223344);
    CHECK(*(uint32_t*)(page + 64) == 0x11223344 && segv_count == 1 && segv_addr == (uint64_t)(page + 64), "F3 stlr to read-only page", segv_count, *(uint32_t*)(page + 64));
    // F4: a fault between the load-exclusive and the store-exclusive does not leave a stale reservation
    // that lets a later store-exclusive to the first address succeed
    *(uint64_t*)(page + 80) = 5;
    uint64_t a = ldxrx(page + 80);
    mprotect(page, 4096, PROT_READ);
    segv_count = 0;
    stlrw(page + 96, 1);  // faults, handler runs (signal delivery clears the monitor)
    uint32_t s2 = stxrx(page + 80, a + 1);
    CHECK(segv_count == 1 && s2 == 1 && *(uint64_t*)(page + 80) == 5, "F4 stxr after a signal", s2, *(uint64_t*)(page + 80));
}

int main(void) {
    h = 1469598103934665603ull;
    single_cpu();
    static const uint64_t want = 0xbcc73c5d51090ef6ull;
    CHECK(h == want, "single-CPU hash", h, want);
    contention();
    faults();
    printf("jit_llsc_native: %s\n", fails ? "FAILED" : "ALL PASS");
    return fails != 0;
}
