// strtod_nan_test.c — verify strtod("-nan") preserves the sign bit.
// musl's __floatscan uses bit manipulation that may not survive the JIT.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

static void dump_double(const char *label, double d) {
    uint64_t bits;
    memcpy(&bits, &d, 8);
    printf("%-20s = %g  bits=0x%016lx  sign=%d exp=0x%03lx mant=0x%013lx\n",
           label, d, bits,
           (int)((bits >> 63) & 1),
           (bits >> 52) & 0x7ff,
           bits & 0xfffffffffffffULL);
}

int main(int argc, char **argv) {
    // Test various NaN-producing inputs
    const char *tests[] = {
        "nan", "-nan", "+nan", "NAN", "-NAN",
        "inf", "-inf", "+inf", "infinity", "-infinity",
        "1.5", "-1.5", "0.5", "-0.5",
        NULL
    };
    for (int i = 0; tests[i]; i++) {
        char *endp;
        double d = strtod(tests[i], &endp);
        dump_double(tests[i], d);
    }
    return 0;
}
