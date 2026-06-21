/* fib.c — benchmark: compute N-th fibonacci iteratively.
 * Used for MIPS measurement.
 */
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv) {
    long n = (argc > 1) ? atol(argv[1]) : 40;
    if (n < 0) n = 0;
    unsigned long long a = 0, b = 1;
    for (long i = 0; i < n; i++) {
        unsigned long long c = a + b;
        a = b;
        b = c;
    }
    printf("fib(%ld) = %llu\n", n, a);
    return 0;
}
