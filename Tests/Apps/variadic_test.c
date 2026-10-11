#include <stdio.h>
#include <stdarg.h>

int sum(int count, ...) {
    va_list args;
    va_start(args, count);
    int total = 0;
    for (int i = 0; i < count; i++)
        total += va_arg(args, int);
    va_end(args);
    return total;
}

double dsum(int count, ...) {
    va_list args;
    va_start(args, count);
    double total = 0;
    for (int i = 0; i < count; i++)
        total += va_arg(args, double);
    va_end(args);
    return total;
}

int main() {
    printf("Variadic function test\n");
    
    int s = sum(5, 1, 2, 3, 4, 5);
    printf("  sum(5, 1..5) = %d %s\n", s, s == 15 ? "OK" : "FAIL");
    if (s != 15) return 1;
    
    s = sum(3, 100, 200, 300);
    printf("  sum(3, 100,200,300) = %d %s\n", s, s == 600 ? "OK" : "FAIL");
    if (s != 600) return 1;
    
    s = sum(0);
    printf("  sum(0) = %d %s\n", s, s == 0 ? "OK" : "FAIL");
    if (s != 0) return 1;
    
    double d = dsum(3, 1.5, 2.5, 3.0);
    printf("  dsum(3, 1.5, 2.5, 3.0) = %f %s\n", d, d > 6.9 && d < 7.1 ? "OK" : "FAIL");
    if (d < 6.9 || d > 7.1) return 1;
    
    printf("PASS\n");
    return 0;
}
