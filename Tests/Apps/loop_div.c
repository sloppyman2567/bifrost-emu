// A division loop the compiler folds entirely.
#include <stdio.h>
int main(void) {
    int sum = 0;
    for (int i = 0; i < 10; ++i) sum += 100 / 7;
    printf("sum = %d\n", sum);
    return 0;
}
