// bench_matrix.c — matrix multiplication benchmark.
// Tests nested loops and FP arithmetic (if compiled with -O2).
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define N 256

static double A[N][N], B[N][N], C[N][N];

int main(void) {
    // Initialize
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            A[i][j] = (double)(i + j);
            B[i][j] = (double)(i * j);
        }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    // Matrix multiply: C = A * B
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            double sum = 0;
            for (int k = 0; k < N; k++)
                sum += A[i][k] * B[k][j];
            C[i][j] = sum;
        }

    clock_gettime(CLOCK_MONOTONIC, &t1);

    double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    double flops = 2.0 * N * N * N;
    printf("matmul: %dx%d in %.3fs = %.1f MFLOPS\n", N, N, secs, flops / secs / 1e6);

    // Prevent optimizer from removing C
    printf("C[0][0]=%.0f C[N-1][N-1]=%.0f\n", C[0][0], C[N-1][N-1]);
    return 0;
}
