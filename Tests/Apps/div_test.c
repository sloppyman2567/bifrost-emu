// Folded quotients and remainders, then a sum of constant divisions.
#include <stdio.h>
int main(void) {
    int a = 100, b = 7, c = 0, d = 5, sum = 0;
    printf("100/7 = %d\n", a / b);
    printf("100%%7 = %d\n", a % b);
    printf("-100/7 = %d\n", -a / b);
    printf("0/5 = %d\n", c / d);
    for (int i = 1000000; i < 1001000; ++i) sum += i / 7;
    printf("sum = %d\n", sum);
    return 0;
}
