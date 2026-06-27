// test_tls_static.c — Minimal TLS test (no threads).
#include <stdio.h>
#include <string.h>

static __thread int counter = 100;
static __thread char buf[64] = "initial";
static __thread double fp_value = 3.14;

int main(void) {
    printf("Test 1: initial values\n");
    if (counter != 100) { printf("FAIL counter=%d\n", counter); return 1; }
    if (strcmp(buf, "initial") != 0) { printf("FAIL buf='%s'\n", buf); return 1; }
    if (fp_value != 3.14) { printf("FAIL fp=%f\n", fp_value); return 1; }
    printf("PASS\n");

    printf("Test 2: write/read\n");
    counter = 42;
    strcpy(buf, "modified");
    fp_value = 2.71;
    if (counter != 42) { printf("FAIL counter=%d\n", counter); return 1; }
    if (strcmp(buf, "modified") != 0) { printf("FAIL buf='%s'\n", buf); return 1; }
    if (fp_value != 2.71) { printf("FAIL fp=%f\n", fp_value); return 1; }
    printf("PASS\n");

    printf("test_tls_static: ALL PASS\n");
    return 0;
}
