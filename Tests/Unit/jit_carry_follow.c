// Carry follows the hot side of a mid-block exit (BIFROST_CARRY, Source/FrostJIT/Carry/HotSide.cpp). A block that Carry formed an exit in
// profiles its first entries, and a forward branch taken more often than not is flipped on the next translation: the block goes on at the
// branch's target and leaves on the fall-through side. Each loop below has a forward cbnz, tbnz or b.ne whose direction comes from a
// pattern array, and runs over patterns that are taken always, never, only for bit 1, alternating and mostly, in phases, so a branch is
// flipped and flipped back, and every result is checked against C. The same loops then run on four threads, which trip the profile at once.
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// x0 = pattern, x1 = n, x2 = seed.
extern uint64_t bias_cbnz(const uint64_t *, uint64_t n, uint64_t seed);
extern uint64_t bias_tbnz(const uint64_t *, uint64_t n, uint64_t seed);
extern uint64_t bias_cond(const uint64_t *, uint64_t n, uint64_t seed);
// A flag set before the branch and read on both sides of it.
extern uint64_t bias_flags(const uint64_t *, uint64_t n, uint64_t seed);
// A call before the branch and one at its target: the flipped block reaches the two-call limit at the target's call.
extern uint64_t bias_calls(const uint64_t *, uint64_t n, uint64_t seed);

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); __atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED); } } while (0)

static uint64_t model(const uint64_t *p, uint64_t n, uint64_t seed, int kind) {
    uint64_t x4 = seed, x5 = 0;
    for (uint64_t i = 0; i < n; ++i) {
        const uint64_t v = p[i];
        const int taken = kind == 1 ? (v & 1) : v != 0;
        if (!taken) { x4 += 13; x5 ^= x4; }
        x4 += v;
        x5 += x4 << 1;
    }
    return x4 ^ x5;
}
static uint64_t model_flags(const uint64_t *p, uint64_t n, uint64_t seed) {
    uint64_t x4 = seed, x5 = 0;
    for (uint64_t i = 0; i < n; ++i) {
        const uint64_t v = p[i];
        x5 += (v < 3);                       // cset lo after cmp v, #3 (both sides read the flags)
        if (v == 0) x4 += 5; else x4 += v * 3;
        x5 += (v >= 3);
    }
    return x4 ^ x5;
}

static uint64_t model_calls(const uint64_t *p, uint64_t n, uint64_t seed) {
    uint64_t x4 = seed, x5 = 0;
    for (uint64_t i = 0; i < n; ++i) {
        const uint64_t v = p[i];
        x4 += 7;
        if (v == 0) { x4 += 13; x5 ^= x4; }
        x5 += x4;
        x4 += v;
        x5 += x4 << 1;
    }
    return x4 ^ x5;
}

enum { N = 3000 };
static void fill(uint64_t *p, int kind, unsigned salt) {
    for (int i = 0; i < N; ++i) {
        switch (kind) {
            case 0: p[i] = 1; break;                                   // always taken
            case 1: p[i] = 0; break;                                   // never
            case 2: p[i] = 2; break;                                   // cbnz taken, tbnz bit 0 not
            case 3: p[i] = (uint64_t)(i + salt) & 1; break;            // alternating
            default: p[i] = ((uint64_t)(i * 2654435761u + salt) >> 7) % 10 ? 3 : 0; break;  // mostly
        }
    }
}
static void run_all(unsigned rounds) {
    uint64_t (*pat)[N] = malloc(5 * sizeof *pat);  // one set per thread: the loops read it while the other threads fill theirs
    for (unsigned round = 0; round < rounds; ++round) {
        for (int kind = 0; kind < 5; ++kind) fill(pat[kind], kind, round);
        // phases: the same branch is taken for a while, then not, then taken again
        static const int order[] = {0, 0, 1, 1, 0, 2, 3, 4, 4, 0, 1};
        for (unsigned k = 0; k < sizeof order / sizeof *order; ++k) {
            const uint64_t *p = pat[order[k]];
            const uint64_t seed = 77 + round * 5 + k;
            CHECK(bias_cbnz(p, N, seed) == model(p, N, seed, 0), "bias_cbnz pattern %d", order[k]);
            CHECK(bias_tbnz(p, N, seed) == model(p, N, seed, 1), "bias_tbnz pattern %d", order[k]);
            CHECK(bias_cond(p, N, seed) == model(p, N, seed, 0), "bias_cond pattern %d", order[k]);
            CHECK(bias_flags(p, N, seed) == model_flags(p, N, seed), "bias_flags pattern %d", order[k]);
            CHECK(bias_calls(p, N, seed) == model_calls(p, N, seed), "bias_calls pattern %d", order[k]);
        }
    }
    free(pat);
}
static void *worker(void *arg) { (void)arg; run_all(2); return NULL; }

int main(void) {
    run_all(3);
    pthread_t threads[4];
    for (int i = 0; i < 4; ++i) pthread_create(&threads[i], NULL, worker, NULL);
    run_all(2);
    for (int i = 0; i < 4; ++i) pthread_join(threads[i], NULL);
    if (failures) { printf("jit_carry_follow: %d FAILURES\n", failures); return 1; }
    puts("jit_carry_follow: ALL PASS");
    return 0;
}

#define BODY_TAIL \
        " add x4, x4, x6\n" \
        " add x5, x5, x4, lsl #1\n" \
        " subs x1, x1, #1\n" \
        " b.ne 1b\n" \
        " eor x0, x4, x5\n ret\n"
__asm__(".text\n.align 4\n.global bias_cbnz\n.type bias_cbnz,%function\nbias_cbnz:\n"
        " mov x4, x2\n mov x5, #0\n cbz x1, 9f\n"
        "1: ldr x6, [x0], #8\n"
        " cbnz x6, 2f\n"
        " add x4, x4, #13\n"
        " eor x5, x5, x4\n"
        "2:" BODY_TAIL
        "9: eor x0, x4, x5\n ret\n"
        ".size bias_cbnz,.-bias_cbnz\n"
        ".global bias_tbnz\n.type bias_tbnz,%function\nbias_tbnz:\n"
        " mov x4, x2\n mov x5, #0\n cbz x1, 9f\n"
        "1: ldr x6, [x0], #8\n"
        " tbnz x6, #0, 2f\n"
        " add x4, x4, #13\n"
        " eor x5, x5, x4\n"
        "2:" BODY_TAIL
        "9: eor x0, x4, x5\n ret\n"
        ".size bias_tbnz,.-bias_tbnz\n"
        ".global bias_cond\n.type bias_cond,%function\nbias_cond:\n"
        " mov x4, x2\n mov x5, #0\n cbz x1, 9f\n"
        "1: ldr x6, [x0], #8\n"
        " cmp x6, #0\n"
        " b.ne 2f\n"
        " add x4, x4, #13\n"
        " eor x5, x5, x4\n"
        "2:" BODY_TAIL
        "9: eor x0, x4, x5\n ret\n"
        ".size bias_cond,.-bias_cond\n"
        ".global bias_flags\n.type bias_flags,%function\nbias_flags:\n"
        " mov x4, x2\n mov x5, #0\n cbz x1, 9f\n"
        "1: ldr x6, [x0], #8\n"
        " cmp x6, #3\n"
        " cset x7, lo\n"
        " add x5, x5, x7\n"
        " cbnz x6, 2f\n"
        " add x4, x4, #5\n"
        " b 3f\n"
        "2: add x4, x4, x6, lsl #1\n"
        " add x4, x4, x6\n"
        "3: cmp x6, #3\n"
        " cset x7, hs\n"
        " add x5, x5, x7\n"
        " subs x1, x1, #1\n"
        " b.ne 1b\n"
        "9: eor x0, x4, x5\n ret\n"
        ".size bias_flags,.-bias_flags\n"
        ".global bias_calls\n.type bias_calls,%function\nbias_calls:\n"
        " mov x20, x30\n adr x9, 8f\n mov x4, x2\n mov x5, #0\n cbz x1, 9f\n"
        "1: ldr x6, [x0], #8\n"
        " bl 7f\n"
        " cbnz x6, 2f\n"
        " add x4, x4, #13\n"
        " eor x5, x5, x4\n"
        "2: blr x9\n"
        " add x4, x4, x6\n"
        " add x5, x5, x4, lsl #1\n"
        " subs x1, x1, #1\n"
        " b.ne 1b\n"
        "9: eor x0, x4, x5\n mov x30, x20\n ret\n"
        "7: add x4, x4, #7\n ret\n"
        "8: add x5, x5, x4\n ret\n"
        ".size bias_calls,.-bias_calls\n");
