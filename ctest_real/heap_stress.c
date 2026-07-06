// heap_stress.c — reproduce mallocng heap corruption patterns.
//
// Trigger patterns observed in toybox sh case-statement crash:
//   1. Word splitting: unquoted variable expansion does malloc/free
//      of temporary buffers in tight loops.
//   2. Many small allocations interleaved with frees.
//   3. realloc patterns that trigger mremap.
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static void word_split_test(void) {
    // Simulate word splitting: many small allocs, then frees.
    char *parts[64];
    for (int iter = 0; iter < 100; iter++) {
        for (int i = 0; i < 64; i++) {
            parts[i] = malloc(16 + (i % 32));
            if (!parts[i]) { printf("OOM i=%d\n", i); exit(1); }
            memset(parts[i], 'A' + (i % 26), 16);
        }
        for (int i = 0; i < 64; i++) {
            free(parts[i]);
        }
    }
}

static void realloc_test(void) {
    // Simulate musl realloc of "big" allocations (> ~128KB)
    // which uses mremap.
    for (int iter = 0; iter < 20; iter++) {
        printf("  iter=%d malloc\n", iter); fflush(stdout);
        char *p = malloc(200000);  // big alloc → mmap
        if (!p) { printf("OOM realloc iter=%d\n", iter); exit(1); }
        memset(p, 'X', 200000);
        // Grow several times — exercises mremap_grow
        for (int k = 0; k < 5; k++) {
            printf("  iter=%d realloc k=%d\n", iter, k); fflush(stdout);
            p = realloc(p, 200000 + k * 65536);
            if (!p) { printf("OOM realloc grow k=%d\n", k); exit(1); }
            printf("  iter=%d memset k=%d p=%p\n", iter, k, p); fflush(stdout);
            if (k > 0) memset(p + 200000 + (k-1)*65536, 'Y', 65536);
        }
        printf("  iter=%d free\n", iter); fflush(stdout);
        free(p);

        // Mix in small allocs that go to mallocng's group slots
        void *small[32];
        for (int i = 0; i < 32; i++) {
            small[i] = malloc(64);
            memset(small[i], i, 64);
        }
        for (int i = 0; i < 32; i++) free(small[i]);
    }
}

static void mixed_test(void) {
    // Interleave big (mmap) and small (mallocng group) allocs
    // to create the bump-pointer pattern that triggers collisions.
    void *small[128];
    void *big[8];

    for (int iter = 0; iter < 10; iter++) {
        for (int i = 0; i < 128; i++) {
            small[i] = malloc(32 + (i % 16) * 8);
        }
        for (int i = 0; i < 8; i++) {
            big[i] = malloc(150000);
            memset(big[i], 'Z', 150000);
        }
        // Free smalls in REVERSE order to exercise the free list
        for (int i = 127; i >= 0; i--) free(small[i]);
        // Free bigs
        for (int i = 0; i < 8; i++) free(big[i]);
    }
}

int main(void) {
    printf("heap_stress start\n");
    fflush(stdout);
    word_split_test();
    printf("word_split_test done\n");
    fflush(stdout);
    realloc_test();
    printf("realloc_test done\n");
    fflush(stdout);
    mixed_test();
    printf("mixed_test done\n");
    fflush(stdout);
    printf("heap_stress OK\n");
    return 0;
}
