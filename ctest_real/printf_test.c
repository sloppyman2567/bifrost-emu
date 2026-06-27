#include <stdio.h>
#include <string.h>

int main() {
    printf("printf edge case tests\n");
    int errors = 0;
    
    // %d
    char buf[256];
    snprintf(buf, sizeof(buf), "%d", 42);
    if (strcmp(buf, "42") != 0) { printf("FAIL %%d: '%s'\n", buf); errors++; }
    
    // %05d
    snprintf(buf, sizeof(buf), "%05d", 42);
    if (strcmp(buf, "00042") != 0) { printf("FAIL %%05d: '%s'\n", buf); errors++; }
    
    // %-5d|
    snprintf(buf, sizeof(buf), "[%-5d]", 42);
    if (strcmp(buf, "[42   ]") != 0) { printf("FAIL %%-5d: '%s'\n", buf); errors++; }
    
    // %x
    snprintf(buf, sizeof(buf), "%x", 255);
    if (strcmp(buf, "ff") != 0) { printf("FAIL %%x: '%s'\n", buf); errors++; }
    
    // %X
    snprintf(buf, sizeof(buf), "%X", 255);
    if (strcmp(buf, "FF") != 0) { printf("FAIL %%X: '%s'\n", buf); errors++; }
    
    // %o
    snprintf(buf, sizeof(buf), "%o", 8);
    if (strcmp(buf, "10") != 0) { printf("FAIL %%o: '%s'\n", buf); errors++; }
    
    // %f
    snprintf(buf, sizeof(buf), "%.2f", 3.14159);
    if (strcmp(buf, "3.14") != 0) { printf("FAIL %%f: '%s'\n", buf); errors++; }
    
    // %e
    snprintf(buf, sizeof(buf), "%.2e", 1234.5);
    if (strstr(buf, "1.23") == NULL) { printf("FAIL %%e: '%s'\n", buf); errors++; }
    
    // %g
    snprintf(buf, sizeof(buf), "%g", 0.0001);
    if (strcmp(buf, "0.0001") != 0) { printf("FAIL %%g: '%s'\n", buf); errors++; }
    
    // %s
    snprintf(buf, sizeof(buf), "%s", "hello");
    if (strcmp(buf, "hello") != 0) { printf("FAIL %%s: '%s'\n", buf); errors++; }
    
    // %c
    snprintf(buf, sizeof(buf), "%c%c%c", 'a', 'b', 'c');
    if (strcmp(buf, "abc") != 0) { printf("FAIL %%c: '%s'\n", buf); errors++; }
    
    // %p
    snprintf(buf, sizeof(buf), "%p", (void*)0xdeadbeef);
    // Just check it contains deadbeef
    if (strstr(buf, "deadbeef") == NULL) { printf("FAIL %%p: '%s'\n", buf); errors++; }
    
    // %% 
    snprintf(buf, sizeof(buf), "100%%");
    if (strcmp(buf, "100%") != 0) { printf("FAIL %%%s\n", buf); errors++; }
    
    // %ld
    snprintf(buf, sizeof(buf), "%ld", 1234567890L);
    if (strcmp(buf, "1234567890") != 0) { printf("FAIL %%ld: '%s'\n", buf); errors++; }
    
    // %llu
    snprintf(buf, sizeof(buf), "%llu", 18446744073709551615ULL);
    if (strcmp(buf, "18446744073709551615") != 0) { printf("FAIL %%llu: '%s'\n", buf); errors++; }
    
    // Multiple args
    snprintf(buf, sizeof(buf), "%d+%d=%d", 2, 3, 5);
    if (strcmp(buf, "2+3=5") != 0) { printf("FAIL multi: '%s'\n", buf); errors++; }
    
    if (errors == 0) printf("PASS\n");
    else printf("FAIL: %d errors\n", errors);
    return errors ? 1 : 0;
}
