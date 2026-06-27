#include <stdio.h>
#include <math.h>
#include <float.h>
#include <stdint.h>

int main() {
    printf("Floating point edge case test\n");
    int errors = 0;
    
    // Inf arithmetic
    double inf = 1.0/0.0;
    if (!isinf(inf)) { printf("FAIL: 1.0/0.0 not inf\n"); errors++; }
    if (!isinf(-inf)) { printf("FAIL: -inf not inf\n"); errors++; }
    if (isinf(1.0)) { printf("FAIL: 1.0 is inf\n"); errors++; }
    printf("  Inf: OK\n");
    
    // NaN arithmetic
    double nan = 0.0/0.0;
    if (!isnan(nan)) { printf("FAIL: 0.0/0.0 not nan\n"); errors++; }
    if (nan == nan) { printf("FAIL: nan == nan\n"); errors++; }
    if (nan != nan) {} else { printf("FAIL: nan != nan is false\n"); errors++; }
    printf("  NaN: OK\n");
    
    // Inf + Inf = Inf, Inf - Inf = NaN
    if (!isinf(inf + inf)) { printf("FAIL: inf+inf\n"); errors++; }
    if (!isnan(inf - inf)) { printf("FAIL: inf-inf\n"); errors++; }
    printf("  Inf arithmetic: OK\n");
    
    // Overflow
    double big = DBL_MAX;
    if (!isinf(big * 2)) { printf("FAIL: DBL_MAX*2 not inf\n"); errors++; }
    printf("  Overflow: OK\n");
    
    // Underflow
    double tiny = DBL_MIN;
    if (tiny / 2 == 0.0) { printf("FAIL: DBL_MIN/2 == 0\n"); errors++; }
    printf("  Underflow: OK\n");
    
    // Rounding
    double pi = 3.14159265358979;
    if (floor(pi) != 3.0) { printf("FAIL: floor(pi)\n"); errors++; }
    if (ceil(pi) != 4.0) { printf("FAIL: ceil(pi)\n"); errors++; }
    if (round(pi) != 3.0) { printf("FAIL: round(pi)\n"); errors++; }
    if (trunc(pi) != 3.0) { printf("FAIL: trunc(pi)\n"); errors++; }
    printf("  Rounding: OK\n");
    
    // pow, sqrt, fmod
    if (sqrt(16.0) != 4.0) { printf("FAIL: sqrt(16)\n"); errors++; }
    if (pow(2.0, 10.0) != 1024.0) { printf("FAIL: pow(2,10)\n"); errors++; }
    if (fmod(10.0, 3.0) != 1.0) { printf("FAIL: fmod(10,3)\n"); errors++; }
    printf("  Math: OK\n");
    
    // Float comparison
    float a = 0.1f, b = 0.1f;
    if (a != b) { printf("FAIL: float comparison\n"); errors++; }
    printf("  Float comparison: OK\n");
    
    if (errors == 0) printf("PASS\n");
    else printf("FAIL: %d errors\n", errors);
    return errors ? 1 : 0;
}
