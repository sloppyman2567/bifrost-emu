#include <stdio.h>

long fib(int n) {
    if (n <= 1) return n;
    return fib(n-1) + fib(n-2);
}

int main() {
    printf("Recursive Fibonacci\n");
    long results[] = {0, 1, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233, 377, 610};
    for (int i = 0; i <= 15; i++) {
        long r = fib(i);
        printf("  fib(%d) = %ld %s\n", i, r, r == results[i] ? "OK" : "FAIL");
        if (r != results[i]) return 1;
    }
    // Stress: fib(35) = 9227465
    long r35 = fib(35);
    printf("  fib(35) = %ld %s\n", r35, r35 == 9227465 ? "OK" : "FAIL");
    if (r35 != 9227465) return 1;
    printf("PASS\n");
    return 0;
}
