#include <stdio.h>
#include <stdlib.h>

#define N 64

static double A[N][N], B[N][N], C[N][N];

int main() {
    printf("Matrix multiply %dx%d\n", N, N);
    
    // Initialize
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            A[i][j] = (i + j) * 0.1;
            B[i][j] = (i * j) * 0.01;
        }
    
    // Multiply
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            C[i][j] = 0;
            for (int k = 0; k < N; k++)
                C[i][j] += A[i][k] * B[k][j];
        }
    
    // Verify a few elements
    double expected_0_0 = 0;
    for (int k = 0; k < N; k++) expected_0_0 += A[0][k] * B[k][0];
    
    double expected_5_5 = 0;
    for (int k = 0; k < N; k++) expected_5_5 += A[5][k] * B[k][5];
    
    printf("  C[0][0] = %.4f (expected %.4f) %s\n", C[0][0], expected_0_0,
           (C[0][0] - expected_0_0) < 0.001 && (C[0][0] - expected_0_0) > -0.001 ? "OK" : "FAIL");
    printf("  C[5][5] = %.4f (expected %.4f) %s\n", C[5][5], expected_5_5,
           (C[5][5] - expected_5_5) < 0.001 && (C[5][5] - expected_5_5) > -0.001 ? "OK" : "FAIL");
    
    if ((C[0][0] - expected_0_0) > 0.001 || (C[5][5] - expected_5_5) > 0.001) return 1;
    printf("PASS\n");
    return 0;
}
