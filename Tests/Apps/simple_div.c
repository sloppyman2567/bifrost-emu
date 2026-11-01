// SDIV and the MSUB of a remainder at run time: volatile keeps the compiler from folding them.
#include <stdio.h>
int main(void) {
    volatile int a = 100, b = 7;
    printf("%d / %d = %d\n", a, b, a / b);
    printf("%d %% %d = %d\n", a, b, a % b);
    return 0;
}
