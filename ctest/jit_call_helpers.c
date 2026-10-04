/* Nested call results and architectural NZCV after BL. Optional loop count
 * supplies a repeatable multithreaded short-call benchmark. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned loops = 30000;
static int failed;
__attribute__((noinline)) static uint64_t leaf(uint64_t x) {
    __asm__ volatile("" : "+r"(x));
    return x * 3 + 5;
}
__attribute__((noinline)) static uint64_t middle(uint64_t x) {
    return leaf(x) + leaf(x + 1);
}
__attribute__((noinline)) static uint64_t outer(uint64_t x) {
    return middle(x) + middle(x + 7);
}
__attribute__((noinline)) static int flags_after_call(uint64_t x) {
    unsigned eq, carry;
    __asm__ volatile(
        "mov x9, %2\n"
        "cmp x9, x9\n" /* caller Z=1 must not hide callee Z=0 */
        "bl 1f\n"
        "cset %w0, eq\n"
        "cset %w1, cs\n"
        "b 2f\n"
        "1: cmp x9, #7\n"
        "ret\n"
        "2:\n"
        : "=r"(eq), "=r"(carry) : "r"(x) : "x9", "x30", "cc", "memory");
    return eq == (x == 7) && carry == (x >= 7);
}
static void *run(void *arg) {
    uint64_t base = (uintptr_t)arg, sum = 0;
    for (unsigned r = 0; r < loops; ++r) sum += outer(base + r);
    uint64_t expected = (12 * base + 68) * loops +
                        6 * (uint64_t)loops * (loops - 1);
    if (sum != expected) __atomic_store_n(&failed, 1, __ATOMIC_RELAXED);
    for (uint64_t i = 0; i < 16; ++i)
        if (!flags_after_call(i)) __atomic_store_n(&failed, 1, __ATOMIC_RELAXED);
    return NULL;
}
int main(int argc, char **argv) {
    if (argc > 1) {
        unsigned long n = strtoul(argv[1], NULL, 10);
        if (!n || n > 10000000) return 2;
        loops = n;
    }
    if (argc > 2 && strcmp(argv[2], "single") == 0) {
        run(NULL);
        if (failed) return 1;
        puts("jit_call_helpers: ALL PASS");
        return 0;
    }
    pthread_t a, b;
    if (pthread_create(&a, NULL, run, (void *)1000) ||
        pthread_create(&b, NULL, run, (void *)2000)) return 3;
    run(NULL);
    pthread_join(a, NULL); pthread_join(b, NULL);
    if (failed) { puts("jit_call_helpers: FAIL"); return 1; }
    puts("jit_call_helpers: ALL PASS");
    return 0;
}
