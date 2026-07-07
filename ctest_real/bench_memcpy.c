// bench_memcpy.c — memcpy bandwidth benchmark.
// Tests large memory copies (mmap/memcpy performance).
// Run: ./bifrost-emu ctest_real/bench_memcpy.elf
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(void) {
    const size_t BUF_SIZE = 64 * 1024 * 1024;  // 64 MiB
    char *src = malloc(BUF_SIZE);
    char *dst = malloc(BUF_SIZE);
    if (!src || !dst) { printf("OOM\n"); return 1; }
    memset(src, 0xAB, BUF_SIZE);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 4; i++) {
        memcpy(dst, src, BUF_SIZE);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    double mib = (4.0 * BUF_SIZE) / (1024 * 1024);
    printf("memcpy: %.0f MiB in %.3fs = %.0f MiB/s\n", mib, secs, mib / secs);

    // Prevent optimizer from removing the copy
    if (dst[0] != (char)0xAB) printf("copy error\n");

    free(src); free(dst);
    return 0;
}
