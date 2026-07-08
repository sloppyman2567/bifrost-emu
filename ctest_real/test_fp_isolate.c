// test_fp_isolate.c — isolate which SIMD instruction breaks glibc float printf.
// glibc's __printf_fp_l uses SIMD for the digit conversion. The decimal
// point is being replaced by a space. We test individual FP operations
// to find which one is broken.
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

int main() {
    // Test 1: simple double print
    double d = 3.14;
    write(1, "[1 %f] ", 6);
    printf("%f\n", d);

    // Test 2: use snprintf to isolate
    char buf[64];
    snprintf(buf, sizeof(buf), "%f", d);
    write(1, "[2 snprintf] ", 13);
    write(1, buf, strlen(buf));
    write(1, "\n", 1);

    // Test 3: print the raw bytes of the result
    write(1, "[3 hex] ", 8);
    for (int i = 0; i < strlen(buf); i++) {
        printf("%02x ", (unsigned char)buf[i]);
    }
    write(1, "\n", 1);

    // Test 4: fcvt (the C library function for float-to-string)
    int decpt, sign;
    char *s = fcvt(d, 2, &decpt, &sign);
    write(1, "[4 fcvt] ", 9);
    write(1, s, strlen(s));
    printf(" decpt=%d sign=%d\n", decpt, sign);

    // Test 5: ecvt
    char *s2 = ecvt(d, 4, &decpt, &sign);
    write(1, "[5 ecvt] ", 9);
    write(1, s2, strlen(s2));
    printf(" decpt=%d sign=%d\n", decpt, sign);

    // Test 6: dtoa
    write(1, "[6 %.0f] ", 9);
    printf("%.0f\n", d);
    write(1, "[7 %.1f] ", 9);
    printf("%.1f\n", d);
    write(1, "[8 %.10f] ", 10);
    printf("%.10f\n", d);

    // Test 9: integer printf (should work)
    write(1, "[9 %d] ", 7);
    printf("%d\n", 42);

    // Test 10: print a string with a literal dot
    write(1, "[10 literal] ", 13);
    printf("3.14\n");

    write(1, "[done]\n", 7);
    return 0;
}
