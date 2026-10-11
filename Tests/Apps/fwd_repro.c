// fwd_repro.c — minimal reproduction of the FWD cache bug.
// With BIFROST_ENABLE_FWD=1, this fails. Without it, it passes.
#include <stdio.h>
#include <string.h>

int main(void) {
    // Force a FCVTZS (FP→int) which emits FP_F2I, writing to an ARM
    // reg vreg directly. Then immediately read that GPR — the FWD
    // cache will substitute the stale pre-conversion value.
    double d = 3.14;
    int i = (int)d;  // FCVTZS → FP_F2I x0, fp_reg
    printf("i=%d (expected 3)\n", i);
    if (i != 3) {
        printf("FAIL\n");
        return 1;
    }

    // A more targeted test: convert, then use the result in a way
    // that would surface a stale cache.
    double vals[] = {1.5, 2.5, 3.5, 4.5};
    int sum = 0;
    for (int k = 0; k < 4; k++) {
        sum += (int)vals[k];  // FP_F2I each iteration
    }
    printf("sum=%d (expected 10)\n", sum);
    if (sum != 10) {
        printf("FAIL\n");
        return 1;
    }

    // fp_314_exact: snprintf with %f
    char buf[64];
    snprintf(buf, sizeof(buf), "%f", 3.14);
    printf("buf='%s' (expected '3.140000')\n", buf);
    if (strncmp(buf, "3.1400", 6) != 0) {
        printf("FAIL\n");
        return 1;
    }

    printf("PASS\n");
    return 0;
}
