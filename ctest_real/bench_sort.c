// bench_sort.c — sorting benchmark (quicksort + mergesort).
// Tests recursion, memory allocation, and comparison-heavy code.
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int cmp(const void *a, const void *b) {
    return *(int*)a - *(int*)b;
}

int main(void) {
    const int N = 100000;
    int *arr = malloc(N * sizeof(int));
    if (!arr) { printf("OOM\n"); return 1; }

    // Fill with pseudo-random data
    unsigned int seed = 42;
    for (int i = 0; i < N; i++) {
        seed = seed * 1103515245 + 12345;
        arr[i] = (int)(seed >> 16);
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    qsort(arr, N, sizeof(int), cmp);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("qsort: %d ints in %.3fs\n", N, secs);

    // Verify sorted
    for (int i = 1; i < N; i++) {
        if (arr[i-1] > arr[i]) { printf("sort error at %d\n", i); break; }
    }

    free(arr);
    return 0;
}
