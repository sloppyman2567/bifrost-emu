// test_dyn_float.c — test floating point printf under glibc dynamic.
#include <stdio.h>
#include <unistd.h>

int main() {
    write(1, "[1] ", 4);
    printf("%f\n", 3.14);
    write(1, "[2] ", 4);
    printf("%.2f\n", 3.14);
    write(1, "[3] ", 4);
    printf("%e\n", 3.14);
    write(1, "[4] ", 4);
    printf("%g\n", 3.14);
    write(1, "[5] ", 4);
    printf("%10.2f\n", 3.14);
    write(1, "[6] ", 4);
    printf("%lf\n", 2.71828);
    write(1, "[7] ", 4);
    double d = 1.5;
    printf("%f %f\n", d, d * 2);
    write(1, "[done]\n", 7);
    return 0;
}
