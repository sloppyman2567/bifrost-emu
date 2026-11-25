// Register passing between chained blocks (BIFROST_CARRY, Source/FrostJIT/Compiler/RegisterPassing.cpp). A predecessor leaves clean copies of
// guest registers in host registers and a successor may be entered past the loads of them, so each shape below hands registers from block to
// block in a different way and is checked against C: a chain of unconditional branches, a diamond with conditional forward branches,
// registers moved between each other across edges, a call in the middle of a chain (the callee changes registers a copy may hold), and a
// fault whose signal frame must show the registers of the iterations that finished. The same shapes then run on four threads.
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

// x0 = a, x1 = b, x2 = n.
extern uint64_t chain(uint64_t a, uint64_t b, uint64_t n);
// x0 = n, x1 = seed.
extern uint64_t diamond(uint64_t n, uint64_t seed);
// x0 = n, x1 = a, x2 = b.
extern uint64_t swaps(uint64_t n, uint64_t a, uint64_t b);
// x0 = n, x1 = a.
extern uint64_t calls(uint64_t n, uint64_t a);
// x0 = u64 array, x1 = initial sum, x2 = count: a fault inside shows the registers so far.
extern uint64_t walk_sum(const uint64_t *, uint64_t, uint64_t);
// x0 = a, x1 = b, x2 = n: registers read first by one block come from three different predecessors.
extern uint64_t merge(uint64_t a, uint64_t b, uint64_t n);
// x0 = n, x1 = a, x2 = b: a register set before a chain of blocks that do not touch it, read by the last one.
extern uint64_t passthru(uint64_t n, uint64_t a, uint64_t b);
// x0 = n, x1..x3 = initial x4..x6: a block reads three registers that one predecessor writes and another leaves alone (partial coverage).
extern uint64_t partial(uint64_t n, uint64_t a, uint64_t b, uint64_t c);

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); ++failures; } } while (0)

static uint64_t chain_c(uint64_t a, uint64_t b, uint64_t n) {
    uint64_t acc = 0, x5 = a, x6 = b;
    for (; n; --n) {
        uint64_t x7 = x5 + x6, x8 = x7 ^ (x5 << 3), x9 = x8 + x7, x10 = x9 - x6;
        x5 = x10 ^ (x9 >> 2);
        x6 += 7;
        acc += x5;
    }
    return acc;
}

static uint64_t diamond_c(uint64_t n, uint64_t seed) {
    uint64_t s = seed, acc = 0, k = 1;
    for (; n; --n) {
        if (s & 1) { acc += s; s >>= 1; k ^= s; }
        else { s += k; acc ^= s; k += 3; }
        acc += k;
    }
    return acc ^ s;
}

static uint64_t swaps_c(uint64_t n, uint64_t a, uint64_t b) {
    uint64_t x3 = a, x4 = b;
    while (n) {
        uint64_t t = x3; x3 = x4; x4 = t;
        x3 += 1;
        --n;
        if (x3 < x4) x4 += 2;
        else x4 ^= x3;
    }
    return x3 ^ x4;
}

static uint64_t helper_c(uint64_t x) { return x * 3 + 1; }
static uint64_t calls_c(uint64_t n, uint64_t a) {
    uint64_t acc = 0, k = 5;
    for (; n; --n) {
        acc += helper_c(a);
        a += k;
        k ^= acc;
    }
    return acc;
}

static uint64_t merge_c(uint64_t a, uint64_t b, uint64_t n) {
    uint64_t x3 = a, x4 = b, acc = 0;
    for (; n; --n) {
        if (n % 3 == 0) x3 += 5;
        else if (n % 3 == 1) x4 += 11;
        else { x3 ^= x4; x4 += 1; }
        acc += x3 * 3 + x4;
    }
    return acc;
}

static uint64_t passthru_c(uint64_t n, uint64_t a, uint64_t b) {
    uint64_t x12 = a + b, acc = 0;
    for (; n; --n) {
        uint64_t x5 = acc + b, x6 = x5 ^ (acc << 1), x7 = x6 + 3;
        acc += x7;
        acc += x12;
        x12 += 1;
    }
    return acc ^ x12;
}

static uint64_t partial_c(uint64_t n, uint64_t a, uint64_t b, uint64_t c) {
    uint64_t x4 = a, x5 = b, x6 = c;
    for (; n; --n) {
        if (n & 1) { x4 += 1; x5 += 7; x6 += 9; }
        else x4 += 2;
        const uint64_t x7 = x4 + x5 + x6;
        x4 ^= x7;
    }
    return x4 ^ x5 ^ x6;
}

static const uint64_t values[] = {0, 1, 2, 0x7fffffffffffffffull, 0x8000000000000000ull, 0xffffffffffffffffull, 0x123456789abcdefull, 77};

static void run_all(int rounds) {
    for (int round = 0; round < rounds; ++round) {
        for (unsigned i = 0; i < sizeof values / sizeof *values; ++i) {
            const uint64_t a = values[i], b = values[(i * 3 + round) % (sizeof values / sizeof *values)];
            for (uint64_t n = 0; n < 300; n += (n < 8 ? 1 : 37)) {
                CHECK(chain(a, b, n) == chain_c(a, b, n), "chain(%llx,%llx,%llu)", (unsigned long long)a, (unsigned long long)b, (unsigned long long)n);
                CHECK(diamond(n, a) == diamond_c(n, a), "diamond(%llu,%llx)", (unsigned long long)n, (unsigned long long)a);
                CHECK(swaps(n, a, b) == swaps_c(n, a, b), "swaps(%llu,%llx,%llx)", (unsigned long long)n, (unsigned long long)a, (unsigned long long)b);
                CHECK(calls(n, a) == calls_c(n, a), "calls(%llu,%llx)", (unsigned long long)n, (unsigned long long)a);
                CHECK(merge(a, b, n) == merge_c(a, b, n), "merge(%llx,%llx,%llu)", (unsigned long long)a, (unsigned long long)b, (unsigned long long)n);
                CHECK(passthru(n, a, b) == passthru_c(n, a, b), "passthru(%llu,%llx,%llx)", (unsigned long long)n, (unsigned long long)a, (unsigned long long)b);
                CHECK(partial(n, a, b, a ^ b) == partial_c(n, a, b, a ^ b), "partial(%llu,%llx,%llx)", (unsigned long long)n, (unsigned long long)a, (unsigned long long)b);
            }
        }
    }
}

// A fault inside walk_sum: the registers of the signal frame are those of the finished iterations.
static sigjmp_buf resume;
static uint64_t fault_addr, fault_x0, fault_x1, fault_x2, fault_x3;
static void on_segv(int sig, siginfo_t *info, void *ctx) {
    (void)sig;
    const ucontext_t *uc = ctx;
    fault_addr = (uint64_t)info->si_addr;
    fault_x0 = uc->uc_mcontext.regs[0];
    fault_x1 = uc->uc_mcontext.regs[1];
    fault_x2 = uc->uc_mcontext.regs[2];
    fault_x3 = uc->uc_mcontext.regs[3];
    siglongjmp(resume, 1);
}
static void fault_case(void) {
    const long page = sysconf(_SC_PAGESIZE);
    uint8_t *region = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED || mprotect(region + page, page, PROT_NONE)) { printf("FAIL: mmap\n"); ++failures; return; }
    const uint64_t per_page = (uint64_t)page / 8;
    uint64_t *base = (uint64_t *)region;
    for (uint64_t i = 0; i < per_page; ++i) base[i] = i * 5 + 3;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    for (int attempt = 0; attempt < 3; ++attempt) {  // translated on the first attempt, chained on the later ones
        fault_addr = 0;
        if (!sigsetjmp(resume, 1)) { walk_sum(base, 9, per_page + 40); CHECK(0, "walk_sum did not fault"); continue; }
        uint64_t sum = 9;
        for (uint64_t i = 0; i < per_page; ++i) { sum += base[i]; sum ^= base[i] << 1; }
        CHECK(fault_addr == (uint64_t)(base + per_page), "fault address %llx", (unsigned long long)fault_addr);
        CHECK(fault_x0 == (uint64_t)(base + per_page), "x0 at the fault: %llx", (unsigned long long)fault_x0);
        CHECK(fault_x1 == sum, "x1 at the fault: %llx want %llx", (unsigned long long)fault_x1, (unsigned long long)sum);
        CHECK(fault_x2 == 40, "x2 at the fault: %llu", (unsigned long long)fault_x2);
        CHECK(fault_x3 == base[per_page - 1], "x3 at the fault: %llx", (unsigned long long)fault_x3);
    }
    signal(SIGSEGV, SIG_DFL);
    munmap(region, 2 * page);
}

static void *worker(void *arg) { (void)arg; run_all(3); return NULL; }

int main(void) {
    run_all(10);
    fault_case();
    pthread_t threads[4];
    for (int i = 0; i < 4; ++i) pthread_create(&threads[i], NULL, worker, NULL);
    run_all(3);
    for (int i = 0; i < 4; ++i) pthread_join(threads[i], NULL);
    if (failures) { printf("jit_reg_pass: %d FAILURES\n", failures); return 1; }
    puts("jit_reg_pass: ALL PASS");
    return 0;
}

__asm__(".text\n.align 4\n.global chain\n.type chain,%function\nchain:\n"
        " mov x4, #0\n mov x5, x0\n mov x6, x1\n"
        " cbz x2, 9f\n"
        "1: add x7, x5, x6\n"
        " eor x8, x7, x5, lsl #3\n"
        " b 2f\n"
        "2: add x9, x8, x7\n"
        " sub x10, x9, x6\n"
        " b 3f\n"
        "3: eor x5, x10, x9, lsr #2\n"
        " add x6, x6, #7\n"
        " add x4, x4, x5\n"
        " b 4f\n"
        "4: subs x2, x2, #1\n"
        " b.ne 1b\n"
        "9: mov x0, x4\n ret\n"
        ".size chain,.-chain\n"
        ".global diamond\n.type diamond,%function\ndiamond:\n"
        " mov x9, x1\n mov x10, #0\n mov x11, #1\n"
        " cbz x0, 9f\n"
        "1: tst x9, #1\n"
        " b.eq 2f\n"
        " add x10, x10, x9\n"
        " lsr x9, x9, #1\n"
        " eor x11, x11, x9\n"
        " b 3f\n"
        "2: add x9, x9, x11\n"
        " eor x10, x10, x9\n"
        " add x11, x11, #3\n"
        "3: add x10, x10, x11\n"
        " subs x0, x0, #1\n"
        " b.ne 1b\n"
        "9: eor x0, x10, x9\n ret\n"
        ".size diamond,.-diamond\n"
        ".global swaps\n.type swaps,%function\nswaps:\n"
        " mov x3, x1\n mov x4, x2\n"
        "1: cbz x0, 9f\n"
        " mov x5, x3\n mov x3, x4\n mov x4, x5\n"
        " add x3, x3, #1\n"
        " sub x0, x0, #1\n"
        " cmp x3, x4\n"
        " b.lo 2f\n"
        " eor x4, x4, x3\n"
        " b 1b\n"
        "2: add x4, x4, #2\n"
        " b 1b\n"
        "9: eor x0, x3, x4\n ret\n"
        ".size swaps,.-swaps\n"
        ".global calls\n.type calls,%function\ncalls:\n"
        " stp x29, x30, [sp, #-48]!\n"
        " stp x19, x20, [sp, #16]\n"
        " stp x21, x22, [sp, #32]\n"
        " mov x19, x0\n mov x20, x1\n mov x21, #0\n mov x22, #5\n"
        " cbz x19, 9f\n"
        "1: mov x0, x20\n"
        " bl 8f\n"
        " add x21, x21, x0\n"
        " add x20, x20, x22\n"
        " eor x22, x22, x21\n"
        " subs x19, x19, #1\n"
        " b.ne 1b\n"
        "9: mov x0, x21\n"
        " ldp x21, x22, [sp, #32]\n"
        " ldp x19, x20, [sp, #16]\n"
        " ldp x29, x30, [sp], #48\n"
        " ret\n"
        "8: add x1, x0, x0, lsl #1\n"
        " add x0, x1, #1\n"
        " mov x2, #7\n"
        " ret\n"
        ".size calls,.-calls\n"
        ".global walk_sum\n.type walk_sum,%function\nwalk_sum:\n"
        "1: ldr x3, [x0]\n"
        " add x1, x1, x3\n"
        " b 2f\n"
        "2: eor x1, x1, x3, lsl #1\n"
        " add x0, x0, #8\n"
        " subs x2, x2, #1\n"
        " b.ne 1b\n"
        " mov x0, x1\n ret\n"
        ".size walk_sum,.-walk_sum\n"
        ".global merge\n.type merge,%function\nmerge:\n"
        " mov x3, x0\n mov x4, x1\n mov x6, #0\n mov x7, #3\n"
        " cbz x2, 9f\n"
        "1: udiv x8, x2, x7\n"
        " msub x8, x8, x7, x2\n"
        " cbz x8, 2f\n"
        " cmp x8, #1\n"
        " b.eq 3f\n"
        " eor x3, x3, x4\n"
        " add x4, x4, #1\n"
        " b 4f\n"
        "2: add x3, x3, #5\n"
        " b 4f\n"
        "3: add x4, x4, #11\n"
        "4: add x9, x3, x3, lsl #1\n"
        " add x9, x9, x4\n"
        " add x6, x6, x9\n"
        " subs x2, x2, #1\n"
        " b.ne 1b\n"
        "9: mov x0, x6\n ret\n"
        ".size merge,.-merge\n"
        ".global passthru\n.type passthru,%function\npassthru:\n"
        " add x12, x1, x2\n mov x4, #0\n"
        " cbz x0, 9f\n"
        "1: add x5, x4, x2\n"
        " b 2f\n"
        "2: eor x6, x5, x4, lsl #1\n"
        " b 3f\n"
        "3: add x7, x6, #3\n"
        " add x4, x4, x7\n"
        " b 4f\n"
        "4: add x4, x4, x12\n"
        " add x12, x12, #1\n"
        " subs x0, x0, #1\n"
        " b.ne 1b\n"
        "9: eor x0, x4, x12\n ret\n"
        ".size passthru,.-passthru\n"
        ".global partial\n.type partial,%function\npartial:\n"
        " mov x4, x1\n mov x5, x2\n mov x6, x3\n"
        " cbz x0, 9f\n"
        "1: tbz x0, #0, 3f\n"
        " add x4, x4, #1\n"
        " add x5, x5, #7\n"
        " add x6, x6, #9\n"
        " b 4f\n"
        "3: add x4, x4, #2\n"
        "4: add x7, x4, x5\n"
        " add x7, x7, x6\n"
        " eor x4, x4, x7\n"
        " subs x0, x0, #1\n"
        " b.ne 1b\n"
        "9: eor x0, x4, x5\n eor x0, x0, x6\n ret\n"
        ".size partial,.-partial\n");
