// SDIV at run time: volatile keeps the compiler from folding or lowering the division.
#include <stdio.h>
int main(void) {
    volatile int a = 1000000, b = 7;
    printf("a=%d b=%d q=%d\n", a, b, a / b);
    return 0;
}
