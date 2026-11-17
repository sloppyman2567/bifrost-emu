// jit_native_libc.c — the host-native memcpy/memmove/memset path (Source/System/Core/NativeLibc.hpp).
//
// A call of 1 KiB or more leaves generated code for the host's routine when every byte of its ranges is accessible
// with the right permission; anything else must still run the guest's own routine and fault where it always did.
// This checks the results against byte-loop references (kept from being turned back into memset/memcpy calls), moves
// that overlap in both directions, a memcpy that overlaps, the edge of a mapping, protections that deny the access,
// callee-saved registers across the calls, and nested calls. test_jit_native_libc.sh checks that the native path ran
// (the stats line) and that BIFROST_NO_NATIVE_LIBC=1 gives the same results.
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// Called through volatile pointers so the compiler cannot expand or fold them: the call reaches the libc routine.
static void *(*volatile call_memset)(void *, int, size_t) = memset;
static void *(*volatile call_memcpy)(void *, const void *, size_t) = memcpy;
static void *(*volatile call_memmove)(void *, const void *, size_t) = memmove;

#pragma GCC optimize("no-tree-loop-distribute-patterns")
static void ref_set(uint8_t *d, int c, size_t n) { for (size_t i = 0; i < n; i++) d[i] = (uint8_t)c; }
static void ref_copy(uint8_t *d, const uint8_t *s, size_t n) { for (size_t i = 0; i < n; i++) d[i] = s[i]; }
static int same(const uint8_t *a, const uint8_t *b, size_t n) { for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return 0; return 1; }
static int all_equal(const uint8_t *a, uint8_t v, size_t n) { for (size_t i = 0; i < n; i++) if (a[i] != v) return 0; return 1; }
#pragma GCC reset_options

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

enum { PAD = 64, MAX = (1 << 20) + 4096 };
static uint8_t *src_buf, *dst_buf, *model;

static const size_t sizes[] = {1024, 1025, 1500, 2048, 4095, 4096, 4097, 8192, 12345, 65536, 100003, 1 << 20};
static const int offsets[] = {0, 1, 3, 7, 8, 15, 16, 33};

static void fill_random(uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) p[i] = (uint8_t)rnd(); }

static void test_set(void) {
    static const int values[] = {0, 0x5A, 0xFF, 0x1A5, -1};  // memset converts the value to an unsigned char
    for (unsigned s = 0; s < sizeof sizes / sizeof sizes[0]; s++)
        for (unsigned o = 0; o < sizeof offsets / sizeof offsets[0]; o++)
            for (unsigned v = 0; v < sizeof values / sizeof values[0]; v++) {
                const size_t n = sizes[s];
                uint8_t *d = dst_buf + PAD + offsets[o];
                ref_set(dst_buf, 0xAA, n + 2 * PAD + 64);
                void *r = call_memset(d, values[v], n);
                CHECK(r == d);
                CHECK(all_equal(d, (uint8_t)values[v], n));
                CHECK(all_equal(dst_buf, 0xAA, PAD + offsets[o]));        // nothing before
                CHECK(all_equal(d + n, 0xAA, PAD));                       // nothing after
            }
}

static void test_copy(void) {
    for (unsigned s = 0; s < sizeof sizes / sizeof sizes[0]; s++)
        for (unsigned o = 0; o < sizeof offsets / sizeof offsets[0]; o++) {
            const size_t n = sizes[s];
            const uint8_t *sp = src_buf + PAD + offsets[(o + 3) % 8];
            uint8_t *d = dst_buf + PAD + offsets[o];
            fill_random(src_buf, n + 2 * PAD + 64);
            ref_set(dst_buf, 0xAA, n + 2 * PAD + 64);
            void *r = call_memcpy(d, sp, n);
            CHECK(r == d);
            CHECK(same(d, sp, n));
            CHECK(all_equal(dst_buf, 0xAA, PAD + offsets[o]));
            CHECK(all_equal(d + n, 0xAA, PAD));
            // memmove between the same two disjoint buffers
            ref_set(dst_buf, 0xAA, n + 2 * PAD + 64);
            r = call_memmove(d, sp, n);
            CHECK(r == d);
            CHECK(same(d, sp, n));
            CHECK(all_equal(d + n, 0xAA, PAD));
        }
}

// memmove over its own source in both directions, compared with a copy through a separate buffer.
static void test_overlap(void) {
    static const size_t shifts[] = {1, 7, 16, 64, 1000, 4000};
    for (unsigned s = 0; s < sizeof sizes / sizeof sizes[0] - 1; s++)
        for (unsigned h = 0; h < sizeof shifts / sizeof shifts[0]; h++)
            for (int dir = 0; dir < 2; dir++) {
                const size_t n = sizes[s], shift = shifts[h];
                uint8_t *base = dst_buf + PAD;
                fill_random(base, n + shift + PAD);
                uint8_t *from = dir ? base + shift : base, *to = dir ? base : base + shift;
                ref_copy(model, from, n);            // what the move must produce
                void *r = call_memmove(to, from, n);
                CHECK(r == to);
                CHECK(same(to, model, n));
            }
}

// An overlapping memcpy is undefined; the call must still return its destination, touch only the two ranges' union
// and not crash (the guest routine runs it; the native path declines).
static void test_memcpy_overlap(void) {
    const size_t n = 4096;
    uint8_t *base = dst_buf + 4 * PAD;
    fill_random(dst_buf, n + 8 * PAD);
    ref_copy(model, dst_buf, 4 * PAD);
    uint8_t before[PAD], after[PAD];
    ref_copy(before, base - PAD, PAD);
    ref_copy(after, base + n + 8, PAD);
    void *r = call_memcpy(base + 8, base, n);
    CHECK(r == base + 8);
    CHECK(same(base - PAD, before, PAD));
    CHECK(same(base + n + 8, after, PAD));
}

static sigjmp_buf resume;
static volatile int caught;
static volatile uint64_t caught_addr;
static void on_segv(int sig, siginfo_t *info, void *ctx) {
    (void)sig; (void)ctx;
    caught = 1;
    caught_addr = (uint64_t)info->si_addr;
    siglongjmp(resume, 1);
}

// A call that must fault inside [lo, hi), as the guest routine does (the native path must decline and not write).
// kind: 0 memset(a, value b, n), 1 memcpy(a, b, n), 2 memmove(a, b, n). A store that straddles the boundary may report
// an address a few bytes before it, so callers pass lo 64 bytes early.
static void expect_fault(int kind, void *a, void *b, size_t n, uint64_t lo, uint64_t hi, const char *what) {
    caught = 0;
    if (sigsetjmp(resume, 1) == 0) {
        if (kind == 0) call_memset(a, (int)(uintptr_t)b, n);
        else if (kind == 1) call_memcpy(a, b, n);
        else call_memmove(a, b, n);
        printf("FAIL %s: no fault\n", what);
        failures++;
        return;
    }
    checks++;
    if (!caught || caught_addr < lo || caught_addr >= hi) {
        printf("FAIL %s: fault at %llx, expected [%llx, %llx)\n", what, (unsigned long long)caught_addr,
               (unsigned long long)lo, (unsigned long long)hi);
        failures++;
    }
}

static void test_edges(void) {
    const size_t page = 4096;
    uint8_t *m = mmap(NULL, 6 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(m != MAP_FAILED);
    // [rw][rw][rw][none][rw][ro]
    CHECK(mprotect(m + 3 * page, page, PROT_NONE) == 0);
    CHECK(mprotect(m + 5 * page, page, PROT_READ) == 0);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    uint8_t *edge = m + 3 * page;                 // first byte of the none page
    ref_set(m, 0x11, 3 * page);

    // Ends exactly at the last accessible byte: the whole range is accessible, so the native path may run it.
    void *r = call_memset(edge - 4096, 0x77, 4096);
    CHECK(r == edge - 4096 && all_equal(edge - 4096, 0x77, 4096));
    r = call_memset(edge - 2048, 0x66, 2048);
    CHECK(r == edge - 2048 && all_equal(edge - 2048, 0x66, 2048));
    r = call_memcpy(m, edge - 4096, 4096);
    CHECK(r == m && same(m, edge - 4096, 4096));

    // One byte further crosses into the none page: the guest routine runs and faults inside it.
    const uint64_t e = (uint64_t)edge;
    expect_fault(0, edge - 4096, (void *)(uintptr_t)0x33, 4097, e - 64, e + page, "memset crossing PROT_NONE");
    expect_fault(0, edge - 1024, (void *)(uintptr_t)0x33, 8192, e - 64, e + page, "memset long crossing");
    expect_fault(1, m, edge - 3000, 4096, e - 64, e + page, "memcpy source crossing");
    expect_fault(1, edge - 3000, m, 4096, e - 64, e + page, "memcpy destination crossing");
    expect_fault(2, edge - 3000, m, 4096, e - 64, e + page, "memmove destination crossing");
    // Read-only destination and unreadable source.
    uint8_t *ro = m + 5 * page;
    const uint64_t rw = (uint64_t)ro;
    expect_fault(0, ro, (void *)(uintptr_t)0x44, 2048, rw - 64, rw + page, "memset read-only");
    expect_fault(1, ro, m, 2048, rw - 64, rw + page, "memcpy read-only destination");
    expect_fault(1, m, edge, 2048, e - 64, e + page, "memcpy PROT_NONE source");
    // The region after the none page is writable and untouched by the failed calls above.
    CHECK(all_equal(m + 4 * page, 0, page));
    // Reading from a read-only source is fine.
    r = call_memcpy(m, ro, 4096);
    CHECK(r == m);
    munmap(m, 6 * page);
}

static int recurse(int depth, uint8_t *buf) {
    if (depth == 0) return 0;
    call_memset(buf, depth, 2048);
    int below = recurse(depth - 1, buf + 2048);
    // The frame above is untouched by the calls below it, and so is this level's own fill.
    return below + (all_equal(buf, (uint8_t)depth, 2048) ? 1 : 0);
}

// Callee-saved registers and values across a native call: the compiler keeps these in x19-x28 or on the stack.
__attribute__((noinline)) static uint64_t live_across(uint8_t *buf) {
    uint64_t a = rnd(), b = rnd() * 3, c = rnd() ^ 0x1234, d = rnd() + 7, e = rnd() * 11, f = rnd() ^ 0x55, g = rnd() + 99, h = rnd() * 5;
    const uint64_t sum = a + b + c + d + e + f + g + h;
    call_memset(buf, 0x42, 8192);
    call_memcpy(buf + 8192, buf, 4096);
    call_memmove(buf + 100, buf + 8192, 4000);
    return (a + b + c + d + e + f + g + h == sum && all_equal(buf + 8192, 0x42, 4096)) ? sum : 0;
}

int main(void) {
    src_buf = malloc(MAX + 4 * PAD);
    dst_buf = malloc(MAX + 4 * PAD);
    model = malloc(MAX + 4 * PAD);
    CHECK(src_buf && dst_buf && model);

    test_set();
    test_copy();
    test_overlap();
    test_memcpy_overlap();
    test_edges();
    CHECK(recurse(40, dst_buf) == 40);
    CHECK(live_across(dst_buf) != 0);
    // A call's return value feeds the next one.
    uint8_t *r = call_memset(call_memcpy(dst_buf + 100, src_buf, 3000), 0x21, 2000);
    CHECK(r == dst_buf + 100 && all_equal(r, 0x21, 2000));

    if (failures) {
        printf("%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("jit_native_libc: %d checks: ALL PASS\n", checks);
    return 0;
}
