// Signed division by a constant, lowered to a multiply, with a value that changes each iteration.
#include <stdio.h>
int main(void) {
    int sum = 0;
    for (int a = 1000000; a < 1000010; ++a) {
        int q = a / 7;
        sum += q;
        printf("a=%d q=%d sum=%d\n", a, q, sum);
    }
    return 0;
}
