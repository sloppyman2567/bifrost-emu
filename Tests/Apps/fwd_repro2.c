// fwd_repro2.c — simpler FP→int test
#include <stdio.h>

int main(void) {
    double a = 1.5;
    int ia = (int)a;
    printf("ia=%d (expected 1)\n", ia);

    double b = 2.5;
    int ib = (int)b;
    printf("ib=%d (expected 2)\n", ib);

    double c = 3.5;
    int ic = (int)c;
    printf("ic=%d (expected 3)\n", ic);

    double d = 4.5;
    int id_ = (int)d;
    printf("id=%d (expected 4)\n", id_);

    int sum = ia + ib + ic + id_;
    printf("sum=%d (expected 10)\n", sum);

    return (sum == 10) ? 0 : 1;
}
