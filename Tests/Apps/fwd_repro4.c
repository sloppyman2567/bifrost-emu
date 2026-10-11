// fwd_repro4.c — minimal snprintf reproduction
#include <stdio.h>

int main(void) {
    char buf[64];
    double d = 3.14;
    printf("d=%f\n", d);
    snprintf(buf, sizeof(buf), "%f", d);
    printf("buf='%s' (expected '3.140000')\n", buf);
    return 0;
}
