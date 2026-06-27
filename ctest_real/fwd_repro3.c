// fwd_repro3.c — debug loop iterations
#include <stdio.h>

int main(void) {
    double vals[] = {1.5, 2.5, 3.5, 4.5};
    int sum = 0;
    for (int k = 0; k < 4; k++) {
        int v = (int)vals[k];
        printf("k=%d vals[k]=%f v=%d sum=%d\n", k, vals[k], v, sum + v);
        sum += v;
    }
    printf("sum=%d (expected 10)\n", sum);
    return (sum == 10) ? 0 : 1;
}
