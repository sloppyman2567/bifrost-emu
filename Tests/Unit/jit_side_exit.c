// Loops whose only inner branches are CBZ/CBNZ exits become one block with mid-block exits (SideExit.cpp). Each shape below is
// hand-written so the compiler cannot reshape it, and every result is checked against C: the registers a loop carries must be
// committed at whichever exit is taken, including the first test of the first iteration, and a fault inside the loop must show the
// state of the iterations that finished. The same loops then run on several threads (multithreaded chaining and exit cells).
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

typedef struct node { struct node *next; uint64_t val; } node;
// x0 = list, x1 = key; returns the sum of values up to and including the first node holding key, or -1 for a list without it.
extern int64_t walk(node *, uint64_t);
// x0 = bytes, x1 = out[4]: count, xor, weighted and prefix sums of the bytes before the first zero.
extern void scan(const uint8_t *, uint64_t *);
// x0 = n, x1 = u64 array: sum of the elements before the first zero or n elements, whichever is first; x2 gets the count taken.
extern uint64_t first_zero(uint64_t n, const uint64_t *, uint64_t *taken);
// x0 = u64 array, x2 = n: sum of n elements (a fault inside the loop shows the sum so far).
extern uint64_t sum_n(const uint64_t *, uint64_t, uint64_t);

__asm__(".text\n.align 4\n.global walk\n.type walk,%function\nwalk:\n"
        " mov x3, #0\n"
        "1: cbz x0, 3f\n"
        " ldr x4, [x0, #8]\n"
        " add x3, x3, x4\n"
        " ldr x0, [x0]\n"
        " cmp x4, x1\n"
        " b.ne 1b\n"
        " mov x0, x3\n ret\n"
        "3: mov x0, #-1\n ret\n"
        ".size walk,.-walk\n"
        ".global scan\n.type scan,%function\nscan:\n"
        " mov x2, #0\n mov x3, #0\n mov x4, #1\n mov x5, #0\n"
        "1: ldrb w6, [x0], #1\n"
        " cbz w6, 2f\n"
        " add x2, x2, #1\n"
        " eor x3, x3, x6\n"
        " add x4, x4, x6, lsl #1\n"
        " add x5, x5, x2\n"
        " b 1b\n"
        "2: stp x2, x3, [x1]\n stp x4, x5, [x1, #16]\n ret\n"
        ".size scan,.-scan\n"
        ".global first_zero\n.type first_zero,%function\nfirst_zero:\n"
        " mov x4, #0\n mov x5, x0\n"
        "1: cbz x0, 3f\n"
        " ldr x3, [x1], #8\n"
        " cbz x3, 2f\n"
        " add x4, x4, x3\n"
        " sub x0, x0, #1\n"
        " b 1b\n"
        "2: sub x5, x5, x0\n str x5, [x2]\n mov x0, x4\n ret\n"
        "3: sub x5, x5, x0\n str x5, [x2]\n mov x0, x4\n ret\n"
        ".size first_zero,.-first_zero\n"
        ".global sum_n\n.type sum_n,%function\nsum_n:\n"
        " mov x4, #0\n"
        "1: cbz x2, 3f\n"
        " ldr x3, [x0], #8\n"
        " add x4, x4, x3\n"
        " sub x2, x2, #1\n"
        " b 1b\n"
        "3: mov x0, x4\n ret\n"
        ".size sum_n,.-sum_n\n");

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); __atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED); } } while (0)

static void run_all(unsigned rounds) {
    enum { N = 40 };
    node nodes[N];
    for (int i = 0; i < N; ++i) { nodes[i].next = i + 1 < N ? &nodes[i + 1] : NULL; nodes[i].val = 100 + 7 * i; }
    uint8_t text[64];
    uint64_t numbers[N + 8];
    for (unsigned round = 0; round < rounds; ++round) {
        // walk: the key at every position, and a key that is not there
        for (int hit = -1; hit < N; ++hit) {
            uint64_t key = hit < 0 ? 5 : nodes[hit].val;
            int64_t want = -1, sum = 0;
            for (int i = 0; i < N; ++i) { sum += nodes[i].val; if (nodes[i].val == key) { want = sum; break; } }
            int64_t got = walk(nodes, key);
            CHECK(got == want, "walk key %d: got %lld want %lld", hit, (long long)got, (long long)want);
        }
        CHECK(walk(NULL, 1) == -1, "walk of an empty list");
        // scan: strings of every length, so the exit is taken at the first test and at the last
        for (int len = 0; len < 40; ++len) {
            for (int i = 0; i < len; ++i) text[i] = (uint8_t)(1 + (i * 37 + round) % 250);
            text[len] = 0;
            uint64_t want[4] = {0, 0, 1, 0}, got[4];
            for (int i = 0; i < len; ++i) { want[0]++; want[1] ^= text[i]; want[2] += (uint64_t)text[i] << 1; want[3] += want[0]; }
            scan(text, got);
            CHECK(!memcmp(got, want, sizeof got), "scan len %d: %llu %llu %llu %llu", len, (unsigned long long)got[0],
                  (unsigned long long)got[1], (unsigned long long)got[2], (unsigned long long)got[3]);
        }
        // first_zero: two exits, a zero at every position and none
        for (int zero = -1; zero < N; ++zero) {
            for (int i = 0; i < N; ++i) numbers[i] = 3 + (uint64_t)i * 5 + round;
            if (zero >= 0) numbers[zero] = 0;
            for (uint64_t n = 0; n <= N; n += 13) {
                uint64_t want = 0, taken = 0;
                while (taken < n && numbers[taken]) want += numbers[taken++];
                uint64_t got_taken = ~0ull, got = first_zero(n, numbers, &got_taken);
                CHECK(got == want && got_taken == taken, "first_zero zero=%d n=%llu: got %llu/%llu want %llu/%llu", zero,
                      (unsigned long long)n, (unsigned long long)got, (unsigned long long)got_taken, (unsigned long long)want,
                      (unsigned long long)taken);
            }
        }
    }
}

// A fault inside the loop: the registers in the signal frame must be those of the iterations that finished.
static sigjmp_buf resume;
static uint64_t fault_x0, fault_x2, fault_x4, fault_addr;
static void on_segv(int sig, siginfo_t *info, void *ctx) {
    (void)sig;
    const ucontext_t *uc = ctx;
    fault_addr = (uint64_t)info->si_addr;
    fault_x0 = uc->uc_mcontext.regs[0];
    fault_x2 = uc->uc_mcontext.regs[2];
    fault_x4 = uc->uc_mcontext.regs[4];
    siglongjmp(resume, 1);
}
static void fault_case(void) {
    const long page = sysconf(_SC_PAGESIZE);
    uint8_t *region = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED || mprotect(region + page, page, PROT_NONE)) { printf("FAIL: mmap\n"); failures++; return; }
    const uint64_t per_page = (uint64_t)page / 8;
    uint64_t *base = (uint64_t *)region;
    for (uint64_t i = 0; i < per_page; ++i) base[i] = i * 3 + 1;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    for (int attempt = 0; attempt < 3; ++attempt) {  // the loop is translated on the first, chained on the later ones
        fault_addr = 0;
        if (!sigsetjmp(resume, 1)) { sum_n(base, per_page + 50, per_page + 50); CHECK(0, "sum_n did not fault"); continue; }
        uint64_t want_sum = 0;
        for (uint64_t i = 0; i < per_page; ++i) want_sum += base[i];
        CHECK(fault_addr == (uint64_t)(base + per_page), "fault address %llx", (unsigned long long)fault_addr);
        CHECK(fault_x0 == (uint64_t)(base + per_page), "x0 at the fault: %llx", (unsigned long long)fault_x0);
        CHECK(fault_x2 == 50, "x2 at the fault: %llu", (unsigned long long)fault_x2);
        CHECK(fault_x4 == want_sum, "x4 at the fault: %llu want %llu", (unsigned long long)fault_x4, (unsigned long long)want_sum);
    }
    signal(SIGSEGV, SIG_DFL);
    munmap(region, 2 * page);
}

static void *worker(void *arg) { (void)arg; run_all(3); return NULL; }

int main(void) {
    run_all(20);
    fault_case();
    pthread_t threads[4];
    for (int i = 0; i < 4; ++i) pthread_create(&threads[i], NULL, worker, NULL);
    run_all(3);
    for (int i = 0; i < 4; ++i) pthread_join(threads[i], NULL);
    if (failures) { printf("jit_side_exit: %d FAILURES\n", failures); return 1; }
    puts("jit_side_exit: ALL PASS");
    return 0;
}
