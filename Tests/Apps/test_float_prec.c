// test_float_prec.c — systematic test of glibc float printf precision.
// The bug: %.2f of 3.14 gives "3.1" (one fewer digit). Let's find the pattern.
#include <stdio.h>
#include <unistd.h>
#include <string.h>

static void test(const char *label, const char *fmt, double val) {
    char buf[128];
    int n = snprintf(buf, sizeof(buf), fmt, val);
    write(1, label, strlen(label));
    write(1, ": ", 2);
    write(1, buf, n);
    write(1, " [len=", 6);
    char lb[8]; lb[0]='0'+n; lb[1]=']'; lb[2]='\n';
    write(1, lb, 3);
}

int main() {
    // Basic precision tests
    test("%.0f", "%.0f", 3.14);
    test("%.1f", "%.1f", 3.14);
    test("%.2f", "%.2f", 3.14);
    test("%.3f", "%.3f", 3.14);
    test("%.4f", "%.4f", 3.14159);
    test("%.5f", "%.5f", 3.14159);
    test("%.6f", "%.6f", 3.141592);
    test("%.10f", "%.10f", 1.0/3.0);
    test("%.15f", "%.15f", 1.0/3.0);
    test("%.20f", "%.20f", 1.0/3.0);
    
    // Test with different values
    test("1.0 %.2f", "%.2f", 1.0);
    test("0.5 %.2f", "%.2f", 0.5);
    test("0.25 %.2f", "%.2f", 0.25);
    test("0.1 %.2f", "%.2f", 0.1);
    test("100.0 %.2f", "%.2f", 100.0);
    test("99.99 %.2f", "%.2f", 99.99);
    test("0.001 %.4f", "%.4f", 0.001);
    test("1234.5678 %.4f", "%.4f", 1234.5678);
    
    // %f default (6 digits)
    test("3.14 %f", "%f", 3.14);
    test("1.0 %f", "%f", 1.0);
    
    // %e tests
    test("3.14 %e", "%e", 3.14);
    test("3.14 %.2e", "%.2e", 3.14);
    
    // %g tests
    test("3.14 %g", "%g", 3.14);
    test("3.14 %.3g", "%.3g", 3.14);
    
    write(1, "[done]\n", 7);
    return 0;
}
