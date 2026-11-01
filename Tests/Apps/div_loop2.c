// A loop whose quotient the compiler folds: the running sum and the call to printf remain.
#include <stdio.h>
int main(void) {
    int a = 1000000, sum = 0;
    for (int i = 0; i < 3; ++i) {
        int q = a / 7;
        sum += q;
        printf("i=%d a=%d q=%d sum=%d\n", i, a, q, sum);
    }
    return 0;
}
