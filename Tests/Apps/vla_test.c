#include <stdio.h>
#include <stdlib.h>

int sum_vla(int n) {
    int arr[n];  // VLA
    for (int i = 0; i < n; i++) arr[i] = i * i;
    int sum = 0;
    for (int i = 0; i < n; i++) sum += arr[i];
    return sum;
}

int main() {
    printf("VLA (variable-length array) test\n");
    
    // sum of i^2 for i=0..9 = 0+1+4+9+16+25+36+49+64+81 = 285
    int r = sum_vla(10);
    printf("  sum_vla(10) = %d %s\n", r, r == 285 ? "OK" : "FAIL");
    if (r != 285) return 1;
    
    // Larger VLA
    r = sum_vla(1000);
    // sum of i^2 from 0 to 999 = 999*1000*1999/6 = 332833500
    printf("  sum_vla(1000) = %d %s\n", r, r == 332833500 ? "OK" : "FAIL");
    if (r != 332833500) return 1;
    
    // Nested VLA
    int m = 5;
    int matrix[m][m];
    for (int i = 0; i < m; i++)
        for (int j = 0; j < m; j++)
            matrix[i][j] = i * m + j;
    
    int trace = 0;
    for (int i = 0; i < m; i++) trace += matrix[i][i];
    printf("  trace(5x5) = %d %s\n", trace, trace == 0+6+12+18+24 ? "OK" : "FAIL");
    if (trace != 60) return 1;
    
    printf("PASS\n");
    return 0;
}
